#include "core/waveform.h"
#include <QSaveFile>
#include <QTextStream>

namespace mca {

WaveformReassembler::WaveformReassembler(int timeoutMs)
    : m_timeoutMs(timeoutMs > 0 ? timeoutMs : TimeoutMilliseconds) {}

bool WaveformReassembler::validate(const WaveformFragment &fragment,
                                   QString *error) {
    const auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (fragment.totalSamples != WaveformTotalSamples)
        return fail(QStringLiteral("波形 total_samples 不是 %1")
                        .arg(WaveformTotalSamples));
    if (fragment.preSamples != WaveformPreSamples)
        return fail(QStringLiteral("波形 pre_samples 不是 %1")
                        .arg(WaveformPreSamples));
    if (fragment.fragmentCount != WaveformFragmentCount)
        return fail(QStringLiteral("波形 fragment_count 不是 %1")
                        .arg(WaveformFragmentCount));
    if (fragment.samplePeriodNs != WaveformSamplePeriodNs)
        return fail(QStringLiteral("波形 sample_period_ns 不是 %1")
                        .arg(WaveformSamplePeriodNs));
    if (fragment.fragmentIndex >= WaveformFragmentCount)
        return fail(QStringLiteral("波形 fragment_index %1 越界")
                        .arg(fragment.fragmentIndex));
    if (fragment.sampleOffset
        != quint16(fragment.fragmentIndex * WaveformFragmentMax))
        return fail(QStringLiteral("波形 sample_offset 与 fragment_index 不一致"));
    const quint16 expected =
        quint16(fragment.fragmentIndex == WaveformFragmentCount - 1
                    ? WaveformLastSamples : WaveformFragmentMax);
    if (fragment.sampleCount != expected)
        return fail(QStringLiteral("波形片段 %1 的点数应为 %2，实际 %3")
                        .arg(fragment.fragmentIndex).arg(expected)
                        .arg(fragment.sampleCount));
    if (fragment.samples.size() != int(expected))
        return fail(QStringLiteral("波形片段 %1 的负载点数与片段头不一致")
                        .arg(fragment.fragmentIndex));
    if ((fragment.flags & 0xfffeu) != 0)
        return fail(QStringLiteral("波形 flags 保留了非零位"));
    return true;
}

int WaveformReassembler::findOrCreate(const WaveformCaptureKey &key,
                                      const WaveformFragment &fragment) {
    for (int i = 0; i < m_pending.size(); ++i) {
        if (sameKey(m_pending[i].key, key))
            return i;
    }
    if (m_pending.size() >= MaximumPending)
        evictOldest();
    WaveformPendingCapture pending;
    pending.key = key;
    pending.parts.resize(WaveformFragmentCount);
    pending.meta = fragment;
    pending.metaValid = true;
    pending.firstSeen = QDateTime::currentDateTime();
    pending.order = ++m_orderCounter;
    m_pending.push_back(pending);
    return m_pending.size() - 1;
}

void WaveformReassembler::evictOldest() {
    if (m_pending.isEmpty())
        return;
    int oldest = 0;
    for (int i = 1; i < m_pending.size(); ++i) {
        if (m_pending[i].order < m_pending[oldest].order)
            oldest = i;
    }
    m_pending.remove(oldest);
    ++m_statistics.evictedCaptures;
    ++m_statistics.discardedCaptures;
}

std::optional<std::shared_ptr<const WaveformCapture>>
WaveformReassembler::add(const WaveformFragment &fragment, quint8 deviceId,
                         quint32 packetConfigVersion, QString *error) {
    if (!validate(fragment, error))
        return std::nullopt;

    // A fragment from a superseded session can never belong to the record the
    // host is currently assembling.
    if (m_sessionKnown && fragment.sessionId != m_sessionId) {
        ++m_statistics.staleFragments;
        return std::nullopt;
    }
    if (!m_sessionKnown) {
        m_sessionId = fragment.sessionId;
        m_sessionKnown = true;
        // A new session invalidates everything that was still pending.
        if (!m_pending.isEmpty()) {
            m_statistics.discardedCaptures += quint64(m_pending.size());
            m_pending.clear();
        }
    }

    WaveformCaptureKey key;
    key.deviceId = deviceId;
    key.sessionId = fragment.sessionId;
    // The fragment header carries the config version latched at the trigger
    // and the MCA2 common header repeats it, so the packet version is the
    // authoritative value here.
    key.configVersion = packetConfigVersion;
    key.captureId = fragment.captureId;

    const int slotIndex = int(fragment.fragmentIndex);
    const int pendingIndex = findOrCreate(key, fragment);
    WaveformPendingCapture &pending = m_pending[pendingIndex];
    WaveformFragmentSlot &slot = pending.parts[slotIndex];
    ++m_statistics.fragments;

    if (slot.present) {
        // Identical duplicates are ignored; a genuine conflict voids the whole
        // record, because one of the two fragments must be corrupt.
        bool identical = slot.sampleCount == fragment.sampleCount;
        for (int i = 0; identical && i < fragment.sampleCount; ++i)
            identical = slot.samples[size_t(i)] == fragment.samples[i];
        if (identical) {
            ++m_statistics.duplicateFragments;
            return std::nullopt;
        }
        ++m_statistics.conflictingFragments;
        ++m_statistics.discardedCaptures;
        if (error)
            *error = QStringLiteral("波形片段 %1 内容冲突，整条记录作废")
                         .arg(slotIndex);
        m_pending.remove(pendingIndex);
        return std::nullopt;
    }

    // Fragments of one record must agree about the capture header.
    if (fragment.captureId != pending.meta.captureId
        || fragment.triggerTimestamp != pending.meta.triggerTimestamp
        || fragment.baseline != pending.meta.baseline
        || fragment.threshold != pending.meta.threshold
        || fragment.returnThreshold != pending.meta.returnThreshold
        || fragment.flags != pending.meta.flags
        || fragment.sessionId != pending.meta.sessionId) {
        ++m_statistics.conflictingFragments;
        ++m_statistics.discardedCaptures;
        if (error)
            *error = QStringLiteral("波形片段 %1 的元数据与首片不一致，整条记录作废")
                         .arg(slotIndex);
        m_pending.remove(pendingIndex);
        return std::nullopt;
    }

    slot.present = true;
    slot.sampleCount = fragment.sampleCount;
    for (int i = 0; i < fragment.sampleCount && i < WaveformFragmentMax; ++i)
        slot.samples[size_t(i)] = fragment.samples[i];
    ++pending.presentCount;

    if (pending.presentCount < WaveformFragmentCount)
        return std::nullopt;

    // All 25 fragments are present: build the immutable record, checking that
    // the covered sample range is exactly the documented one.
    auto capture = std::make_shared<WaveformCapture>();
    capture->sessionId = pending.meta.sessionId;
    capture->captureId = pending.meta.captureId;
    capture->deviceId = deviceId;
    capture->configVersion = packetConfigVersion;
    capture->triggerTimestamp = pending.meta.triggerTimestamp;
    capture->baseline = pending.meta.baseline;
    capture->threshold = pending.meta.threshold;
    capture->returnThreshold = pending.meta.returnThreshold;
    capture->preSamples = pending.meta.preSamples;
    capture->totalSamples = pending.meta.totalSamples;
    capture->samplePeriodNs = pending.meta.samplePeriodNs;
    capture->negativePolarity = pending.meta.negativePolarity();
    capture->samples.resize(WaveformTotalSamples);
    int covered = 0;
    for (int index = 0; index < WaveformFragmentCount; ++index) {
        const WaveformFragmentSlot &source = pending.parts[index];
        if (!source.present) {
            if (error)
                *error = QStringLiteral("波形记录缺少片段 %1").arg(index);
            ++m_statistics.discardedCaptures;
            m_pending.remove(pendingIndex);
            return std::nullopt;
        }
        const int offset = index * WaveformFragmentMax;
        for (int i = 0; i < int(source.sampleCount); ++i)
            capture->samples[offset + i] = source.samples[size_t(i)];
        covered += int(source.sampleCount);
    }
    m_pending.remove(pendingIndex);
    if (covered != WaveformTotalSamples || !capture->valid()) {
        if (error)
            *error = QStringLiteral("波形记录覆盖点数不是 %1")
                         .arg(WaveformTotalSamples);
        ++m_statistics.discardedCaptures;
        return std::nullopt;
    }
    ++m_statistics.completedCaptures;
    return std::shared_ptr<const WaveformCapture>(std::move(capture));
}

int WaveformReassembler::expire(const QDateTime &now) {
    int removed = 0;
    for (int i = m_pending.size() - 1; i >= 0; --i) {
        const qint64 age = m_pending[i].firstSeen.msecsTo(now);
        if (age >= m_timeoutMs) {
            m_pending.remove(i);
            ++removed;
            ++m_statistics.timedOutCaptures;
            ++m_statistics.discardedCaptures;
        }
    }
    return removed;
}

void WaveformReassembler::reset(quint32 newSessionId) {
    if (!m_pending.isEmpty())
        m_statistics.discardedCaptures += quint64(m_pending.size());
    m_pending.clear();
    m_sessionId = newSessionId;
    m_sessionKnown = newSessionId != 0;
}

void WaveformReassembler::clear() {
    m_pending.clear();
    m_statistics = Statistics{};
}

bool writeWaveformCsv(const QString &path, const WaveformCapture &capture,
                      QString *error) {
    const auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (!capture.valid())
        return fail(QStringLiteral("波形不完整，无法导出"));
    if (path.isEmpty())
        return fail(QStringLiteral("未指定导出文件"));

    QSaveFile file(path);
    // Opened without QIODevice::Text so the line endings are exactly the ones
    // written below: the documented CSV layout must not depend on the host
    // platform's newline translation.
    if (!file.open(QIODevice::WriteOnly))
        return fail(QStringLiteral("无法写入 %1：%2")
                        .arg(path, file.errorString()));

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << "# MCA_WAVEFORM_CSV_V1\n";
    stream << "# session_id=" << capture.sessionId << "\n";
    stream << "# capture_id=" << capture.captureId << "\n";
    stream << "# device_id=" << capture.deviceId << "\n";
    stream << "# config_version=" << capture.configVersion << "\n";
    stream << "# trigger_timestamp=" << capture.triggerTimestamp << "\n";
    stream << "# trigger_timestamp_unit=8ns\n";
    stream << "# baseline=" << capture.baseline << "\n";
    stream << "# threshold=" << capture.threshold << "\n";
    stream << "# return_threshold=" << capture.returnThreshold << "\n";
    stream << "# polarity=" << (capture.negativePolarity ? 1 : 0) << ":"
           << (capture.negativePolarity ? QStringLiteral("negative")
                                        : QStringLiteral("positive")) << "\n";
    stream << "# pre_samples=" << capture.preSamples << "\n";
    stream << "# total_samples=" << capture.totalSamples << "\n";
    stream << "# sample_period_ns=" << capture.samplePeriodNs << "\n";
    stream << "# adc_bits=" << WaveformAdcBits << "\n";
    stream << "# adc_full_scale=" << WaveformAdcFullScale << "\n";
    stream << "# time_origin=trigger\n";
    stream << "sample_index,time_ns,adc_code,overrange\n";

    for (int i = 0; i < capture.totalSamples; ++i) {
        stream << i << ',' << capture.timeNs(i) << ','
               << capture.adcCode(i) << ','
               << (capture.overrange(i) ? 1 : 0) << '\n';
    }
    stream.flush();
    if (stream.status() != QTextStream::Ok)
        return fail(QStringLiteral("写入 %1 失败：%2")
                        .arg(path, file.errorString()));
    if (!file.commit())
        return fail(QStringLiteral("提交 %1 失败：%2")
                        .arg(path, file.errorString()));
    return true;
}

} // namespace mca
