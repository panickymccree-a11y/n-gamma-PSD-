#include "core/protocol.h"
#include <QtEndian>

namespace mca {
namespace {

quint64 read48(const uchar *p) {
    return (quint64(p[0]) << 40) | (quint64(p[1]) << 32)
         | (quint64(p[2]) << 24) | (quint64(p[3]) << 16)
         | (quint64(p[4]) << 8) | quint64(p[5]);
}

void write48(uchar *p, quint64 value) {
    for (int i = 5; i >= 0; --i) {
        p[i] = uchar(value & 0xff);
        value >>= 8;
    }
}

quint32 read24(const uchar *p) {
    return (quint32(p[0]) << 16) | (quint32(p[1]) << 8) | quint32(p[2]);
}

void write24(uchar *p, quint32 value) {
    p[0] = uchar(value >> 16);
    p[1] = uchar(value >> 8);
    p[2] = uchar(value);
}

ChannelEvent decodeChannel(const uchar *p) {
    ChannelEvent event;
    event.timestamp = read48(p);
    event.peak = qFromBigEndian<quint16>(p + 6);
    event.integral = read24(p + 8);
    const quint8 quality = p[11];
    event.channel = quality >> 3;
    event.pileup = (quality & 0x04) != 0;
    event.overflow = (quality & 0x02) != 0;
    event.polarity = (quality & 0x01) != 0;
    return event;
}

void encodeChannel(uchar *p, const ChannelEvent &event) {
    write48(p, event.timestamp);
    qToBigEndian<quint16>(event.peak, p + 6);
    write24(p + 8, event.integral);
    p[11] = quint8((event.channel & 0x1f) << 3)
          | (event.pileup ? 0x04 : 0)
          | (event.overflow ? 0x02 : 0)
          | (event.polarity ? 0x01 : 0);
}

ModuleEvent decodeModule(const uchar *p) {
    ModuleEvent event;
    event.eventId = qFromBigEndian<quint32>(p);
    event.timestamp = read48(p + 4);
    event.module = p[10];
    event.flags = p[11];
    for (int i = 0; i < 4; ++i)
        event.relativeTime[i] = qFromBigEndian<qint16>(p + 12 + i * 2);
    for (int i = 0; i < 4; ++i)
        event.integral[i] = read24(p + 20 + i * 3);
    for (int i = 0; i < 4; ++i)
        event.peak[i] = qFromBigEndian<quint16>(p + 32 + i * 2);
    for (int i = 0; i < 4; ++i)
        event.channelFlags[i] = p[40 + i];
    event.x = qFromBigEndian<quint16>(p + 44);
    event.y = qFromBigEndian<quint16>(p + 46);
    event.energy = qFromBigEndian<quint32>(p + 48);
    return event;
}

void encodeModule(uchar *p, const ModuleEvent &event) {
    qToBigEndian<quint32>(event.eventId, p);
    write48(p + 4, event.timestamp);
    p[10] = event.module;
    p[11] = event.flags;
    for (int i = 0; i < 4; ++i)
        qToBigEndian<qint16>(event.relativeTime[i], p + 12 + i * 2);
    for (int i = 0; i < 4; ++i)
        write24(p + 20 + i * 3, event.integral[i]);
    for (int i = 0; i < 4; ++i)
        qToBigEndian<quint16>(event.peak[i], p + 32 + i * 2);
    for (int i = 0; i < 4; ++i)
        p[40 + i] = event.channelFlags[i];
    qToBigEndian<quint16>(event.x, p + 44);
    qToBigEndian<quint16>(event.y, p + 46);
    qToBigEndian<quint32>(event.energy, p + 48);
}

QByteArray makePacket(PacketType type, quint32 sequence, quint32 configVersion,
                      quint8 deviceId, quint8 flags, quint16 count,
                      int recordBytes) {
    const int payloadBytes = int(count) * recordBytes;
    QByteArray bytes(PacketHeaderBytes + payloadBytes, Qt::Uninitialized);
    auto *p = reinterpret_cast<uchar *>(bytes.data());
    qToBigEndian<quint32>(DataMagic, p);
    p[4] = 2;
    p[5] = quint8(type);
    p[6] = deviceId;
    p[7] = flags;
    qToBigEndian<quint32>(sequence, p + 8);
    qToBigEndian<quint32>(configVersion, p + 12);
    qToBigEndian<quint16>(count, p + 16);
    qToBigEndian<quint16>(quint16(payloadBytes), p + 18);
    return bytes;
}

quint32 read32(const uchar *p) {
    return qFromBigEndian<quint32>(p);
}

bool decodeStatusTlv(const uchar *p, int length, DeviceStatus &status,
                     QString *error) {
    int offset = 0;
    while (offset < length) {
        const quint8 type = p[offset];
        // New firmware uses a one-byte zero marker; older status images may
        // pad the remainder of their fixed payload with zeroes.
        if (type == 0) {
            for (int tail = offset + 1; tail < length; ++tail) {
                if (p[tail] != 0) {
                    if (error)
                        *error = QStringLiteral("MCA2 状态结束标记后含非零数据");
                    return false;
                }
            }
            return true;
        }
        if (offset + 2 > length) {
            if (error) *error = QStringLiteral("MCA2 状态 TLV 头截断");
            return false;
        }
        const int size = p[offset + 1];
        offset += 2;
        if (offset + size > length) {
            if (error) *error = QStringLiteral("MCA2 状态 TLV 长度越界");
            return false;
        }
        const uchar *value = p + offset;
        if (type == 1 && size < 8) {
            if (error) *error = QStringLiteral("MCA2 能力 TLV 长度不足");
            return false;
        } else if (type == 1) {
            status.capabilitiesValid = true;
            status.capabilities.protocolVersion = value[0];
            status.capabilities.adcCount = value[1];
            status.capabilities.channelCount = value[2];
            status.capabilities.moduleCount = value[3];
            status.capabilities.pairCount = value[4];
            status.mode = AcquisitionMode(value[5]);
            status.running = value[6] != 0;
            status.adcAlignmentMask = value[7];
            const quint8 expectedMask =
                status.capabilities.adcCount >= 8
                ? 0xff : quint8((1u << status.capabilities.adcCount) - 1u);
            status.adcAligned =
                (status.adcAlignmentMask & expectedMask) == expectedMask;
        } else if (type == 2 && size < 20) {
            if (error) *error = QStringLiteral("MCA2 epoch TLV 长度不足");
            return false;
        } else if (type == 2) {
            status.configVersion = read32(value);
            status.timestamp = read48(value + 4);
            status.baselineReadyMask = read32(value + 10);
            status.baselineTrackingMask = read32(value + 14);
            status.eventFifoCount = qFromBigEndian<quint16>(value + 18);
        } else if (type == 3 && size < 28) {
            if (error) *error = QStringLiteral("MCA2 计数 TLV 长度不足");
            return false;
        } else if (type == 3) {
            status.outputEvents = read32(value);
            status.moduleUnmatched = read32(value + 4);
            status.moduleRejected = read32(value + 8);
            status.pairExpired = read32(value + 12);
            status.ambiguous = read32(value + 16);
            status.configurationErrors = read32(value + 20);
            status.protocolErrors = read32(value + 24);
        } else if (type == 4 && size < 20) {
            if (error) *error = QStringLiteral("MCA2 流量 TLV 长度不足");
            return false;
        } else if (type == 4) {
            status.pairGateRejected = read32(value);
            status.pairCacheOverflow = read32(value + 4);
            status.fifoOverflow = read32(value + 8);
            status.outputBackpressure = read32(value + 12);
            status.commandFifoOverflow = read32(value + 16);
        } else if (type >= 5 && type <= 8) {
            if (size != 12) {
                if (error)
                    *error = QStringLiteral("MCA2 基线 TLV 长度必须为 12 字节");
                return false;
            }
            const int channelBase = (int(type) - 5) * 8;
            for (int pair = 0; pair < 4; ++pair) {
                const int byte = pair * 3;
                status.baselineValues[channelBase + pair * 2] =
                    quint16((quint16(value[byte]) << 4) |
                            (value[byte + 1] >> 4));
                status.baselineValues[channelBase + pair * 2 + 1] =
                    quint16((quint16(value[byte + 1] & 0x0f) << 8) |
                            value[byte + 2]);
            }
            status.baselineValueValidMask |= 0xffu << channelBase;
            status.baselineValueCount = quint8(
                qPopulationCount(status.baselineValueValidMask));
        } else if (type == 9 && size < 6) {
            if (error) *error = QStringLiteral("MCA2 基线状态 TLV 长度不足");
            return false;
        } else if (type == 9) {
            for (int channel = 0; channel < 24; ++channel) {
                const int bit = channel * 2;
                status.baselineStates[size_t(channel)] = quint8(
                    (value[bit / 8] >> (bit % 8)) & 0x03);
            }
        } else if (type == 10 && size < 3) {
            if (error) *error = QStringLiteral("MCA2 手动基线 TLV 长度不足");
            return false;
        } else if (type == 10) {
            status.baselineAutomatic = (value[0] & 0x01) != 0;
            status.manualBaseline = quint16(
                (quint16(value[1] & 0x3f) << 8) | value[2]);
            status.manualBaselineValid = true;
        } else if (type == 11 && size < 9) {
            if (error) *error = QStringLiteral("MCA2 额外计数 TLV 长度不足");
            return false;
        } else if (type == 11) {
            status.deadTimeRejected = read32(value);
            status.droppedEvents = read32(value + 4);
            status.eventActive = (value[8] & 0x01) != 0;
        } else if (type == 12 && size < 3) {
            if (error) *error = QStringLiteral("MCA2 基线16 TLV 长度不足");
            return false;
        } else if (type == 12) {
            const quint8 channel = value[0];
            if (channel < MaxChannels) {
                status.baselineValues[channel] = quint16(
                    (quint16(value[1] & 0x3f) << 8) | value[2]);
                status.baselineValueValidMask |= (1u << channel);
                status.baselineValueCount = quint8(
                    qPopulationCount(status.baselineValueValidMask));
            }
        } else if (type == 13) {
            if (size != 4) {
                if (error)
                    *error = QStringLiteral("MCA2 前端状态 TLV 长度必须为 4 字节");
                return false;
            }
            status.actualRange = value[0] & 0x03u;
            status.rangeBusy = (value[1] & 0x01u) != 0;
            status.filterEnabled = (value[1] & 0x02u) != 0;
            status.frontendInitDone = (value[1] & 0x04u) != 0;
            status.peakInterpolationEnabled = (value[1] & 0x08u) != 0;
            status.recoveryToleranceLsb = quint16(value[2] << 8) | value[3];
            status.frontendStateValid = true;
        }
        offset += size;
    }
    return offset == length;
}

} // namespace

bool PacketDecoder::decode(const QByteArray &bytes, DecodedPacket &out,
                           QString *error) {
    const auto fail = [&](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (bytes.size() < PacketHeaderBytes)
        return fail(QStringLiteral("数据包短于 MCA2 公共头"));
    if (bytes.size() > MaximumUdpPayloadBytes)
        return fail(QStringLiteral("MCA2 数据包超过 1472 字节"));

    const auto *p = reinterpret_cast<const uchar *>(bytes.constData());
    if (qFromBigEndian<quint32>(p) != DataMagic)
        return fail(QStringLiteral("无效的 MCA2 魔数"));
    if (p[4] != 2)
        return fail(QStringLiteral("不支持的 MCA2 版本"));

    const quint8 rawType = p[5];
    if (rawType < 1 || rawType > 7)
        return fail(QStringLiteral("无效的 MCA2 包类型"));
    const auto type = PacketType(rawType);
    const quint16 count = qFromBigEndian<quint16>(p + 16);
    const quint16 payload = qFromBigEndian<quint16>(p + 18);
    if (bytes.size() != PacketHeaderBytes + payload)
        return fail(QStringLiteral("MCA2 负载长度与 UDP 数据不一致"));

    int recordBytes = 0;
    switch (type) {
    case PacketType::PsdEvents: recordBytes = 24; break;
    case PacketType::ChannelEvents: recordBytes = ChannelEventBytes; break;
    case PacketType::ModuleEvents: recordBytes = ModuleEventBytes; break;
    case PacketType::CoincidenceEvents: recordBytes = CoincidenceEventBytes; break;
    case PacketType::ControlResponse: recordBytes = ControlResponseBytes; break;
    case PacketType::Waveform: break;   // per-fragment length, checked below
    case PacketType::Status: break;
    }
    if (recordBytes && payload != count * recordBytes)
        return fail(QStringLiteral("MCA2 记录数与负载长度不一致"));

    out = {};
    out.version = 2;
    out.type = type;
    out.deviceId = p[6];
    out.flags = p[7];
    out.sequence = qFromBigEndian<quint32>(p + 8);
    out.configVersion = qFromBigEndian<quint32>(p + 12);
    p += PacketHeaderBytes;

    if (type == PacketType::PsdEvents) {
        out.psdEvents.reserve(count);
        for (int i=0;i<count;++i) {
            const uchar *r=p+i*24;
            if (r[23]!=0 || (r[22]&0xfc)!=0 || qFromBigEndian<quint16>(r+20)>16383)
                return fail(QStringLiteral("PSD 事件保留位或基线非法"));
            PsdEvent event; event.legacy=decodeChannel(r);
            event.qtotal=qFromBigEndian<qint32>(r+12);
            event.qtail=qFromBigEndian<qint32>(r+16);
            event.baseline=qFromBigEndian<quint16>(r+20); event.quality=r[22];
            event.configVersion=out.configVersion; out.psdEvents.push_back(event);
        }
    } else if (type == PacketType::ChannelEvents) {
        out.channelEvents.reserve(count);
        for (int i = 0; i < count; ++i, p += ChannelEventBytes)
            out.channelEvents.push_back(decodeChannel(p));
    } else if (type == PacketType::ModuleEvents) {
        out.moduleEvents.reserve(count);
        for (int i = 0; i < count; ++i, p += ModuleEventBytes)
            out.moduleEvents.push_back(decodeModule(p));
    } else if (type == PacketType::CoincidenceEvents) {
        out.coincidenceEvents.reserve(count);
        for (int i = 0; i < count; ++i, p += CoincidenceEventBytes) {
            CoincidenceEvent event;
            event.matchId = qFromBigEndian<quint32>(p);
            event.pair = p[4];
            event.flags = p[5];
            event.layerTime = qFromBigEndian<qint16>(p + 6);
            event.scatter = decodeModule(p + 8);
            event.absorber = decodeModule(p + 60);
            out.coincidenceEvents.push_back(event);
        }
    } else if (type == PacketType::ControlResponse) {
        out.responses.reserve(count);
        for (int i = 0; i < count; ++i, p += ControlResponseBytes) {
            ControlResponse response;
            response.transactionId = qFromBigEndian<quint32>(p);
            response.opcode = p[4];
            response.target = p[5];
            response.targetId = p[6];
            response.status = p[7];
            response.registerAddress = qFromBigEndian<quint16>(p + 8);
            response.value = qFromBigEndian<quint32>(p + 12);
            out.responses.push_back(response);
        }
    } else if (type == PacketType::Waveform) {
        // record_count is always one: a waveform packet carries exactly one
        // fragment.  A wrong count is treated as a malformed packet rather
        // than silently accepted, because reassembly keys on the fragment
        // metadata.
        if (count != 1)
            return fail(QStringLiteral("MCA2 类型6 包的 record_count 必须为 1"));
        if (payload < WaveformHeaderBytes)
            return fail(QStringLiteral("MCA2 类型6 包的负载短于片段头"));
        const quint16 sampleBytes = quint16(payload - WaveformHeaderBytes);
        if ((sampleBytes & 1) != 0)
            return fail(QStringLiteral("MCA2 类型6 包的样本字节数必须为偶数"));
        const quint16 sampleCount = quint16(sampleBytes / 2);
        if (sampleCount == 0 || sampleCount > WaveformFragmentMax)
            return fail(QStringLiteral("MCA2 类型6 包的样本点数越界"));

        WaveformFragment fragment;
        fragment.sessionId = qFromBigEndian<quint32>(p);
        const uchar *h = p + WaveformSessionBytes;
        fragment.captureId = qFromBigEndian<quint32>(h + 0);
        fragment.triggerTimestamp = read48(h + 4);
        fragment.baseline = qFromBigEndian<quint16>(h + 10);
        fragment.threshold = qFromBigEndian<quint16>(h + 12);
        fragment.returnThreshold = qFromBigEndian<quint16>(h + 14);
        fragment.preSamples = qFromBigEndian<quint16>(h + 16);
        fragment.totalSamples = qFromBigEndian<quint16>(h + 18);
        fragment.sampleOffset = qFromBigEndian<quint16>(h + 20);
        fragment.sampleCount = qFromBigEndian<quint16>(h + 22);
        fragment.fragmentIndex = qFromBigEndian<quint16>(h + 24);
        fragment.fragmentCount = qFromBigEndian<quint16>(h + 26);
        fragment.flags = qFromBigEndian<quint16>(h + 28);
        fragment.samplePeriodNs = qFromBigEndian<quint16>(h + 30);

        if (fragment.sampleCount != sampleCount)
            return fail(QStringLiteral("MCA2 类型6 包的片段头点数与负载不一致"));
        if (fragment.preSamples != WaveformPreSamples ||
            fragment.totalSamples != WaveformTotalSamples ||
            fragment.fragmentCount != WaveformFragmentCount ||
            fragment.sampleOffset != fragment.fragmentIndex * WaveformFragmentMax ||
            fragment.samplePeriodNs != WaveformSamplePeriodNs)
            return fail(QStringLiteral("MCA2 类型6 包不符合固定波形规格"));

        const uchar *s = h + WaveformFragmentHeaderBytes;
        fragment.samples.resize(sampleCount);
        for (int i = 0; i < sampleCount; ++i)
            fragment.samples[i] = qFromBigEndian<quint16>(s + i * 2);
        out.waveformFragments.push_back(fragment);
    } else {
        if (payload == 0)
            return fail(QStringLiteral("MCA2 状态包负载为空"));
        if (!decodeStatusTlv(p, payload, out.status, error))
            return false;
        out.hasStatus = true;
    }
    return true;
}

QByteArray PacketDecoder::encodeWaveform(
    quint32 sequence, quint32 configVersion, quint8 deviceId,
    const WaveformFragment &fragment, quint8 flags) {
    const int sampleCount = fragment.samples.size();
    const int payloadBytes = WaveformHeaderBytes + sampleCount * 2;
    QByteArray bytes(PacketHeaderBytes + payloadBytes, Qt::Uninitialized);
    auto *p = reinterpret_cast<uchar *>(bytes.data());
    qToBigEndian<quint32>(DataMagic, p);
    p[4] = 2;
    p[5] = quint8(PacketType::Waveform);
    p[6] = deviceId;
    p[7] = flags;
    qToBigEndian<quint32>(sequence, p + 8);
    qToBigEndian<quint32>(configVersion, p + 12);
    qToBigEndian<quint16>(1, p + 16);
    qToBigEndian<quint16>(quint16(payloadBytes), p + 18);
    auto *body = p + PacketHeaderBytes;
    qToBigEndian<quint32>(fragment.sessionId, body);
    auto *h = body + WaveformSessionBytes;
    qToBigEndian<quint32>(fragment.captureId, h + 0);
    write48(h + 4, fragment.triggerTimestamp);
    qToBigEndian<quint16>(fragment.baseline, h + 10);
    qToBigEndian<quint16>(fragment.threshold, h + 12);
    qToBigEndian<quint16>(fragment.returnThreshold, h + 14);
    qToBigEndian<quint16>(fragment.preSamples, h + 16);
    qToBigEndian<quint16>(fragment.totalSamples, h + 18);
    qToBigEndian<quint16>(fragment.sampleOffset, h + 20);
    qToBigEndian<quint16>(quint16(sampleCount), h + 22);
    qToBigEndian<quint16>(fragment.fragmentIndex, h + 24);
    qToBigEndian<quint16>(fragment.fragmentCount, h + 26);
    qToBigEndian<quint16>(fragment.flags, h + 28);
    qToBigEndian<quint16>(fragment.samplePeriodNs, h + 30);
    auto *s = h + WaveformFragmentHeaderBytes;
    for (int i = 0; i < sampleCount; ++i)
        qToBigEndian<quint16>(fragment.samples[i], s + i * 2);
    return bytes;
}

QByteArray PacketDecoder::encodeChannels(
    quint32 sequence, quint32 configVersion, quint8 deviceId,
    const QVector<ChannelEvent> &events, quint8 flags) {
    const int maximum = (MaximumUdpPayloadBytes - PacketHeaderBytes)
                      / ChannelEventBytes;
    const quint16 count = quint16(qMin(events.size(), maximum));
    auto bytes = makePacket(PacketType::ChannelEvents, sequence, configVersion,
                            deviceId, flags, count, ChannelEventBytes);
    auto *p = reinterpret_cast<uchar *>(bytes.data()) + PacketHeaderBytes;
    for (int i = 0; i < count; ++i, p += ChannelEventBytes)
        encodeChannel(p, events[i]);
    return bytes;
}

QByteArray PacketDecoder::encodeModules(
    quint32 sequence, quint32 configVersion, quint8 deviceId,
    const QVector<ModuleEvent> &events, quint8 flags) {
    const int maximum = (MaximumUdpPayloadBytes - PacketHeaderBytes)
                      / ModuleEventBytes;
    const quint16 count = quint16(qMin(events.size(), maximum));
    auto bytes = makePacket(PacketType::ModuleEvents, sequence, configVersion,
                            deviceId, flags, count, ModuleEventBytes);
    auto *p = reinterpret_cast<uchar *>(bytes.data()) + PacketHeaderBytes;
    for (int i = 0; i < count; ++i, p += ModuleEventBytes)
        encodeModule(p, events[i]);
    return bytes;
}

QByteArray PacketDecoder::encodeCoincidences(
    quint32 sequence, quint32 configVersion, quint8 deviceId,
    const QVector<CoincidenceEvent> &events, quint8 flags) {
    const int maximum = (MaximumUdpPayloadBytes - PacketHeaderBytes)
                      / CoincidenceEventBytes;
    const quint16 count = quint16(qMin(events.size(), maximum));
    auto bytes = makePacket(PacketType::CoincidenceEvents, sequence,
                            configVersion, deviceId, flags, count,
                            CoincidenceEventBytes);
    auto *p = reinterpret_cast<uchar *>(bytes.data()) + PacketHeaderBytes;
    for (int i = 0; i < count; ++i, p += CoincidenceEventBytes) {
        const auto &event = events[i];
        qToBigEndian<quint32>(event.matchId, p);
        p[4] = event.pair;
        p[5] = event.flags;
        qToBigEndian<qint16>(event.layerTime, p + 6);
        encodeModule(p + 8, event.scatter);
        encodeModule(p + 60, event.absorber);
    }
    return bytes;
}

QByteArray makeControlRequest(ControlOpcode opcode, ControlTarget target,
                              quint8 targetId, quint16 registerAddress,
                              quint16 flags, quint32 transactionId,
                              quint32 value) {
    QByteArray frame(ControlFrameBytes, Qt::Uninitialized);
    auto *p = reinterpret_cast<uchar *>(frame.data());
    qToBigEndian<quint32>(ControlMagic, p);
    p[4] = 2;
    p[5] = quint8(opcode);
    p[6] = quint8(target);
    p[7] = targetId;
    qToBigEndian<quint16>(registerAddress, p + 8);
    qToBigEndian<quint16>(flags, p + 10);
    qToBigEndian<quint32>(transactionId, p + 12);
    qToBigEndian<quint32>(value, p + 16);
    return frame;
}

QByteArray makeShadowWrite(ControlTarget target, quint8 targetId,
                           quint16 registerAddress, quint32 value,
                           quint32 transactionId) {
    return makeControlRequest(ControlOpcode::WriteShadow, target, targetId,
                              registerAddress, 0, transactionId, value);
}

QByteArray makeRead(ControlTarget target, quint8 targetId,
                    quint16 registerAddress, quint32 transactionId) {
    return makeControlRequest(ControlOpcode::Read, target, targetId,
                              registerAddress, 0, transactionId, 0);
}

QByteArray makeCommitAll(quint32 transactionId) {
    return makeControlRequest(ControlOpcode::CommitAll, ControlTarget::Global,
                              0, 0, 0, transactionId, 0);
}

QByteArray makeStart(quint32 configVersion, quint32 transactionId) {
    return makeControlRequest(ControlOpcode::Start, ControlTarget::Global,
                              0, 0, 0, transactionId, configVersion);
}

QByteArray makeStop(quint32 transactionId) {
    return makeControlRequest(ControlOpcode::Stop, ControlTarget::Global,
                              0, 0, 0, transactionId, 0);
}

QByteArray makeAbort(quint32 transactionId) {
    return makeControlRequest(ControlOpcode::Abort, ControlTarget::Global,
                              0, 0, 0, transactionId, 0);
}

QByteArray makeClearCounters(quint32 transactionId) {
    return makeControlRequest(ControlOpcode::ClearCounters,
                              ControlTarget::Global, 0, 0, 0,
                              transactionId, 0);
}

QByteArray makeSetRange(quint8 range, quint32 transactionId) {
    return makeControlRequest(ControlOpcode::WriteShadow,
                              ControlTarget::Global, 0,
                              RegisterGlobalRange, 0,
                              transactionId, range & 0x03u);
}

quint32 makeChannelControlWord(const ChannelConfig &config, bool enabled) {
    return (enabled ? 1u : 0u)
         | (quint32(config.polarity) << 1);
}

} // namespace mca
