#include "core/spectrumfile.h"
#include <QDataStream>
#include <QFile>
#include <numeric>

namespace mca {
namespace {
constexpr quint32 SpectrumMagic = 0x4d434153;
constexpr quint16 SpectrumVersion = 2;
}

bool SpectrumFile::writeV1(const QString &path,
                           const SpectrumSnapshot &snapshot,
                           QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << SpectrumMagic << SpectrumVersion
           << quint16(snapshot.channelCount) << snapshot.totalEvents
           << snapshot.sampleRateHz << snapshot.adcBits
           << snapshot.peakFractionBits << quint16(0);
    for (int channel = 0; channel < snapshot.channelCount; ++channel)
        stream << snapshot.peak.value(channel)
               << snapshot.integral.value(channel);
    if (stream.status() != QDataStream::Ok) {
        if (error)
            *error = QStringLiteral("写入能谱文件失败");
        return false;
    }
    return true;
}

std::shared_ptr<const SpectrumSnapshot>
SpectrumFile::readV1(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return {};
    }
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::BigEndian);
    quint32 magic = 0;
    quint16 version = 0;
    quint16 channelCount = 0;
    quint64 totalEvents = 0;
    stream >> magic >> version >> channelCount >> totalEvents;
    if (magic != SpectrumMagic || (version != 1 && version != SpectrumVersion)) {
        if (error)
            *error = QStringLiteral("不是受支持的 MCAS 能谱文件");
        return {};
    }
    if (channelCount == 0 || channelCount > MaxChannels) {
        if (error)
            *error = QStringLiteral("能谱文件通道数非法：%1").arg(channelCount);
        return {};
    }

    auto snapshot = std::make_shared<SpectrumSnapshot>();
    snapshot->channelCount = channelCount;
    snapshot->totalEvents = totalEvents;
    if (version >= 2) {
        quint16 reserved = 0;
        stream >> snapshot->sampleRateHz >> snapshot->adcBits
               >> snapshot->peakFractionBits >> reserved;
    }
    snapshot->peak.resize(channelCount);
    snapshot->integral.resize(channelCount);
    for (int channel = 0; channel < channelCount; ++channel) {
        stream >> snapshot->peak[channel] >> snapshot->integral[channel];
        const int peakSize = snapshot->peak[channel].size();
        if (stream.status() != QDataStream::Ok ||
            peakSize < 256 || peakSize > 65536 ||
            (channel > 0 && peakSize != snapshot->peak[0].size()) ||
            snapshot->integral[channel].size() != DefaultIntegralBins) {
            if (error)
                *error = QStringLiteral("能谱文件内容截断或道数不正确");
            return {};
        }
        snapshot->channelEvents[size_t(channel)] =
            std::accumulate(snapshot->peak[channel].cbegin(),
                            snapshot->peak[channel].cend(), quint64(0));
    }
    if (!file.atEnd()) {
        // Future-compatible trailing data is intentionally ignored.
    }
    return snapshot;
}

} // namespace mca
