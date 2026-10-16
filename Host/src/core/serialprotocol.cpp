#include "core/serialprotocol.h"
#include <QtEndian>
#include <algorithm>
#include <cstring>

namespace mca {
namespace {
quint64 keyFor(quint32 session, quint32 epoch) {
    return (quint64(session) << 32) | epoch;
}
void setError(QString *error, const QString &text) {
    if (error) *error = text;
}
}

quint32 SerialFrameCodec::crc32(const QByteArray &bytes) {
    quint32 crc = 0xffff'ffffu;
    for (uchar byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xedb8'8320u) : (crc >> 1);
    }
    return ~crc;
}

QByteArray SerialFrameCodec::encode(SerialMessageType type, quint32 sequence,
                                    const QByteArray &payload, quint8 flags) {
    if (payload.size() > SerialMaximumPayload)
        return {};
    QByteArray frame(SerialHeaderBytes + payload.size(), Qt::Uninitialized);
    auto *p = reinterpret_cast<uchar *>(frame.data());
    qToBigEndian<quint32>(SerialMagic, p);
    p[4] = SerialVersion;
    p[5] = quint8(type);
    p[6] = flags;
    p[7] = SerialHeaderBytes;
    qToBigEndian<quint32>(sequence, p + 8);
    qToBigEndian<quint16>(quint16(payload.size()), p + 12);
    qToBigEndian<quint16>(0, p + 14);
    if (!payload.isEmpty())
        memcpy(frame.data() + SerialHeaderBytes, payload.constData(), payload.size());
    const quint32 crc = crc32(frame);
    const int oldSize = frame.size();
    frame.resize(oldSize + SerialCrcBytes);
    qToBigEndian<quint32>(crc,
        reinterpret_cast<uchar *>(frame.data() + oldSize));
    return frame;
}

QVector<SerialFrame> SerialFrameCodec::feed(const QByteArray &bytes,
                                            QStringList *errors) {
    m_buffer += bytes;
    QVector<SerialFrame> frames;
    const QByteArray magic("MCS2", 4);
    for (;;) {
        int start = m_buffer.indexOf(magic);
        if (start < 0) {
            if (m_buffer.size() > 3)
                m_buffer = m_buffer.right(3);
            break;
        }
        if (start > 0) {
            if (errors) errors->push_back(QStringLiteral("已跳过%1个串口噪声字节").arg(start));
            m_buffer.remove(0, start);
        }
        if (m_buffer.size() < SerialHeaderBytes)
            break;
        const auto *p = reinterpret_cast<const uchar *>(m_buffer.constData());
        const quint16 payloadBytes = qFromBigEndian<quint16>(p + 12);
        if (p[7] != SerialHeaderBytes || payloadBytes > SerialMaximumPayload) {
            if (errors) errors->push_back(QStringLiteral("串口帧头无效"));
            m_buffer.remove(0, 1);
            continue;
        }
        const int frameBytes = SerialHeaderBytes + payloadBytes + SerialCrcBytes;
        if (m_buffer.size() < frameBytes)
            break;
        const QByteArray checked = m_buffer.left(SerialHeaderBytes + payloadBytes);
        const quint32 expected = qFromBigEndian<quint32>(
            reinterpret_cast<const uchar *>(m_buffer.constData()
                                             + SerialHeaderBytes + payloadBytes));
        const quint32 calculated = crc32(checked);
        if (calculated != expected) {
            if (errors) {
                errors->push_back(
                    QStringLiteral("串口帧CRC错误：类型0x%1，负载%2字节，计算0x%3，收到0x%4，缓冲%5字节，帧首%6，帧尾及后续%7")
                        .arg(p[5], 2, 16, QLatin1Char('0'))
                        .arg(payloadBytes)
                        .arg(calculated, 8, 16, QLatin1Char('0'))
                        .arg(expected, 8, 16, QLatin1Char('0'))
                        .arg(m_buffer.size())
                        .arg(QString::fromLatin1(
                            m_buffer.left(qMin(frameBytes, 64)).toHex(' ')))
                        .arg(QString::fromLatin1(
                            m_buffer.mid(qMax(0, frameBytes - 32), 64).toHex(' '))));
            }
            m_buffer.remove(0, 1);
            continue;
        }
        // A damaged serial stream can contain the four magic bytes inside a
        // payload.  Do not treat the following arbitrary byte as a genuine
        // protocol version until the complete candidate frame has passed its
        // CRC.  Otherwise one recoverable line error tears down an otherwise
        // healthy v2 connection as an alleged old-version device.
        if (p[4] != SerialVersion) {
            if (errors) {
                errors->push_back(QStringLiteral("串口协议版本%1不支持，要求MCS2 v%2")
                                      .arg(p[4]).arg(SerialVersion));
            }
            m_buffer.remove(0, frameBytes);
            continue;
        }
        SerialFrame frame;
        frame.type = SerialMessageType(p[5]);
        frame.flags = p[6];
        frame.sequence = qFromBigEndian<quint32>(p + 8);
        frame.payload = m_buffer.mid(SerialHeaderBytes, payloadBytes);
        frames.push_back(std::move(frame));
        m_buffer.remove(0, frameBytes);
    }
    return frames;
}

void SerialFrameCodec::clear() { m_buffer.clear(); }

bool decodeSerialHello(const QByteArray &payload, SerialHello &hello,
                       QString *error) {
    if (payload.size() != 20) {
        setError(error, QStringLiteral("HELLO长度不是20字节"));
        return false;
    }
    const auto *p = reinterpret_cast<const uchar *>(payload.constData());
    hello.baudRate = qFromBigEndian<quint32>(p);
    hello.binCount = qFromBigEndian<quint16>(p + 4);
    hello.peakShift = p[6];
    hello.integralShift = p[7];
    hello.chunkBins = qFromBigEndian<quint16>(p + 8);
    hello.sampleRateHz = qFromBigEndian<quint32>(p + 10);
    hello.featureFlags = qFromBigEndian<quint16>(p + 14);
    hello.firmwareVersion = qFromBigEndian<quint32>(p + 16);
    if (hello.firmwareVersion != SerialFirmwareVersion && hello.firmwareVersion != 0x00020700u) {
        setError(error, QStringLiteral("HELLO固件版本0x%1不匹配，要求0x%2")
                     .arg(hello.firmwareVersion, 8, 16, QLatin1Char('0'))
                     .arg(SerialFirmwareVersion, 8, 16, QLatin1Char('0')));
        return false;
    }
    if (hello.baudRate != SerialBaudRate || hello.binCount < 1024 ||
        hello.binCount > 8192 || (hello.binCount & (hello.binCount - 1)) ||
        hello.chunkBins == 0 || hello.binCount % hello.chunkBins ||
        (hello.featureFlags & 0x003fu) != 0x003fu) {
        setError(error, QStringLiteral("HELLO能力字段无效或与500 kbaud协议不兼容"));
        return false;
    }
    return true;
}

bool decodeSerialStatus(const QByteArray &payload, SerialStatus &status,
                        QString *error) {
    if (payload.size() != 52) {
        setError(error, QStringLiteral("串口STATUS长度不是52字节"));
        return false;
    }
    const auto *p = reinterpret_cast<const uchar *>(payload.constData());
    status.formatVersion = p[0];
    if (status.formatVersion != 2) {
        setError(error, QStringLiteral("串口STATUS版本不是2"));
        return false;
    }
    const quint8 flags = p[1];
    status.running = (flags & 0x01u) != 0;
    status.baselineReady = (flags & 0x02u) != 0;
    status.baselineAutomatic = (flags & 0x04u) != 0;
    status.frontendInitDone = (flags & 0x08u) != 0;
    status.rangeBusy = (flags & 0x10u) != 0;
    status.filterEnabled = (flags & 0x20u) != 0;
    status.peakInterpolationEnabled = (p[39] & 0x01u) != 0;
    status.externalArmed = (flags & 0x40u) != 0;
    status.externalWindowActive = (flags & 0x80u) != 0;
    status.baselineState = p[2];
    status.mode = p[3];
    status.configVersion = qFromBigEndian<quint32>(p + 4);
    status.outputEvents = qFromBigEndian<quint32>(p + 8);
    status.rejectedEvents = qFromBigEndian<quint32>(p + 12);
    status.droppedEvents = qFromBigEndian<quint32>(p + 16);
    status.crcErrors = qFromBigEndian<quint32>(p + 20);
    status.frameErrors = qFromBigEndian<quint32>(p + 24);
    status.retries = qFromBigEndian<quint32>(p + 28);
    status.baseline = qFromBigEndian<quint16>(p + 32);
    status.manualBaseline = qFromBigEndian<quint16>(p + 34);
    status.recoveryTolerance = qFromBigEndian<quint16>(p + 36);
    status.actualRange = p[38];
    status.externalTriggerCount = qFromBigEndian<quint32>(p + 40);
    status.timestamp = (quint64(p[44]) << 40) |
                       (quint64(p[45]) << 32) |
                       (quint64(p[46]) << 24) |
                       (quint64(p[47]) << 16) |
                       (quint64(p[48]) << 8) |
                       quint64(p[49]);
    if (status.baseline > 16383 || status.manualBaseline > 16383 ||
        status.recoveryTolerance > 16383 || status.actualRange > 2 ||
        status.baselineState > 3 || status.mode > 4) {
        setError(error, QStringLiteral("串口STATUS字段超出范围"));
        return false;
    }
    return true;
}

bool decodeSerialSpectrum(const QByteArray &payload, SerialSpectrumChunk &chunk,
                          QString *error) {
    if (payload.size() < SerialSpectrumHeaderBytes) {
        setError(error, QStringLiteral("能谱块头被截断"));
        return false;
    }
    const auto *p = reinterpret_cast<const uchar *>(payload.constData());
    chunk.session = qFromBigEndian<quint32>(p);
    chunk.configVersion = qFromBigEndian<quint32>(p + 4);
    chunk.epoch = qFromBigEndian<quint32>(p + 8);
    chunk.kind = p[12]; chunk.flags = p[13]; chunk.binShift = p[14];
    chunk.totalBins = qFromBigEndian<quint16>(p + 16);
    chunk.startBin = qFromBigEndian<quint16>(p + 18);
    chunk.binCount = qFromBigEndian<quint16>(p + 20);
    chunk.chunkIndex = qFromBigEndian<quint16>(p + 22);
    chunk.chunkTotal = qFromBigEndian<quint16>(p + 24);
    chunk.intervalTicks = qFromBigEndian<quint16>(p + 26);
    chunk.intervalTicks = (chunk.intervalTicks << 32) |
                          qFromBigEndian<quint32>(p + 28);
    chunk.intervalEvents = qFromBigEndian<quint32>(p + 32);
    chunk.droppedEvents = qFromBigEndian<quint32>(p + 36);
    const int expected = SerialSpectrumHeaderBytes + int(chunk.binCount) * 4;
    if (chunk.kind > 1 || chunk.totalBins == 0 || chunk.binCount == 0 ||
        chunk.startBin + chunk.binCount > chunk.totalBins ||
        chunk.chunkIndex >= chunk.chunkTotal || payload.size() != expected) {
        setError(error, QStringLiteral("能谱块几何或长度无效"));
        return false;
    }
    chunk.counts.resize(chunk.binCount);
    for (int i = 0; i < chunk.binCount; ++i)
        chunk.counts[i] = qFromBigEndian<quint32>(p + SerialSpectrumHeaderBytes + i * 4);
    return true;
}

QByteArray makeSerialAck(quint32 frameSequence,
                         const SerialSpectrumChunk &chunk) {
    QByteArray payload(12, 0);
    auto *p = reinterpret_cast<uchar *>(payload.data());
    qToBigEndian<quint32>(frameSequence, p);
    qToBigEndian<quint32>(chunk.epoch, p + 4);
    p[8] = chunk.kind;
    qToBigEndian<quint16>(chunk.chunkIndex, p + 9);
    return payload;
}

std::optional<SerialSpectrumAssembler::Epoch>
SerialSpectrumAssembler::add(const SerialSpectrumChunk &chunk, QString *error) {
    const quint64 key = keyFor(chunk.session, chunk.epoch);
    auto it = m_partial.find(key);
    if (it == m_partial.end()) {
        Partial partial;
        partial.epoch.session = chunk.session;
        partial.epoch.epoch = chunk.epoch;
        partial.epoch.intervalTicks = chunk.intervalTicks;
        partial.epoch.intervalEvents = chunk.intervalEvents;
        partial.epoch.droppedEvents = chunk.droppedEvents;
        partial.epoch.peak.fill(0, chunk.totalBins);
        partial.epoch.integral.fill(0, chunk.totalBins);
        partial.peakChunks.fill(false, chunk.chunkTotal);
        partial.integralChunks.fill(false, chunk.chunkTotal);
        it = m_partial.insert(key, std::move(partial));
    }
    Partial &partial = it.value();
    if (partial.epoch.peak.size() != chunk.totalBins ||
        partial.peakChunks.size() != chunk.chunkTotal) {
        setError(error, QStringLiteral("同一epoch的能谱几何不一致"));
        m_partial.erase(it);
        return std::nullopt;
    }
    auto &seen = chunk.kind ? partial.integralChunks : partial.peakChunks;
    auto &bins = chunk.kind ? partial.epoch.integral : partial.epoch.peak;
    if (!seen[chunk.chunkIndex]) {
        for (int i = 0; i < chunk.counts.size(); ++i)
            bins[chunk.startBin + i] = chunk.counts[i];
        seen[chunk.chunkIndex] = true;
    }
    if (chunk.kind) partial.epoch.integralShift = chunk.binShift;
    else partial.epoch.peakShift = chunk.binShift;
    partial.epoch.flags |= chunk.flags;
    const bool complete = std::all_of(partial.peakChunks.cbegin(), partial.peakChunks.cend(),
                                      [](bool value) { return value; }) &&
                          std::all_of(partial.integralChunks.cbegin(), partial.integralChunks.cend(),
                                      [](bool value) { return value; });
    if (!complete) return std::nullopt;
    Epoch result = partial.epoch;
    m_partial.erase(it);
    // Bound damage from a disconnected sender changing sessions repeatedly.
    while (m_partial.size() > 4)
        m_partial.erase(m_partial.begin());
    return result;
}

void SerialSpectrumAssembler::clear() { m_partial.clear(); }

} // namespace mca
