#include "core/listfile.h"
#include <QDataStream>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace mca {
namespace {
constexpr quint32 FileMagic = 0x4d43414c;   // MCAL
constexpr quint16 FileVersion = 3;
constexpr quint32 PacketBlockMagic = 0x504b5433; // PKT3
constexpr quint32 FooterMagic = 0x454e4421; // END!

QJsonObject capabilitiesJson(const DeviceCapabilities &c) {
    return {
        {"protocolVersion", c.protocolVersion},
        {"adcCount", c.adcCount},
        {"maximumAdcCount", c.maximumAdcCount},
        {"channelCount", c.channelCount},
        {"maximumChannelCount", c.maximumChannelCount},
        {"moduleCount", c.moduleCount},
        {"maximumModuleCount", c.maximumModuleCount},
        {"pairCount", c.pairCount},
        {"sampleRateHz", qint64(c.sampleRateHz)},
        {"firmwareVersion", qint64(c.firmwareVersion)},
        {"formatFlags", qint64(c.formatFlags)},
        {"adcBits", c.adcBits},
        {"peakFractionBits", c.peakFractionBits},
        {"integralBits", c.integralBits},
        {"integrationWindowMinNs", qint64(c.integrationWindowMinNs)},
        {"integrationWindowMaxNs", qint64(c.integrationWindowMaxNs)},
        {"timebasePs", qint64(c.timebasePs)}
    };
}
} // namespace

bool ListFileWriter::open(const QString &path,
                          const DeviceCapabilities &capabilities,
                          AcquisitionMode mode,
                          const QVector<ChannelConfig> &channels,
                          const QVector<ModuleConfig> &modules,
                          const QVector<PairConfig> &pairs,
                          quint32 configVersion, QString *error) {
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly)) {
        if (error) *error = m_file.errorString();
        return false;
    }

    QJsonArray channelJson;
    for (const auto &c : channels) {
        channelJson.append(QJsonObject{
            {"enabled", c.enabled},
            {"thresholdLsb", c.thresholdLsb}, {"polarity", int(c.polarity)},
            {"baselineGuardLsb", c.baselineGuardLsb},
            {"baselinePulseThresholdLsb", c.baselinePulseThresholdLsb},
            {"baselineShift", c.baselineShift},
            {"baselineHoldSamples", c.baselineHoldSamples},
            {"integrationPreSamples", c.integrationPreSamples},
            {"integrationPostSamples", c.integrationPostSamples},
            {"gainQ4_12", c.gainQ4_12}, {"color", c.color.name()}
        });
    }
    QJsonArray moduleJson;
    for (const auto &m : modules) {
        moduleJson.append(QJsonObject{
            {"enabled", m.enabled}, {"windowSamples", m.windowSamples},
            {"energyGateEnabled", m.energyGateEnabled},
            {"energyMin", qint64(m.energyMin)},
            {"energyMax", qint64(m.energyMax)},
            {"qualityRejectMask", m.qualityRejectMask}
        });
    }
    QJsonArray pairJson;
    for (const auto &p : pairs) {
        pairJson.append(QJsonObject{
            {"enabled", p.enabled},
            {"scatterModuleMask", int(p.scatterModuleMask)},
            {"absorberModuleMask", int(p.absorberModuleMask)},
            {"minimumTimeSamples", p.minimumTimeSamples},
            {"maximumTimeSamples", p.maximumTimeSamples},
            {"scatterEnergyGateEnabled", p.scatterEnergyGateEnabled},
            {"scatterEnergyMin", qint64(p.scatterEnergyMin)},
            {"scatterEnergyMax", qint64(p.scatterEnergyMax)},
            {"absorberEnergyGateEnabled", p.absorberEnergyGateEnabled},
            {"absorberEnergyMin", qint64(p.absorberEnergyMin)},
            {"absorberEnergyMax", qint64(p.absorberEnergyMax)},
            {"qualityRejectMask", p.qualityRejectMask}
        });
    }

    const QJsonObject metadata{
        {"format", QStringLiteral("MCAL v3")},
        {"createdUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"mode", int(mode)}, {"configVersion", qint64(configVersion)},
        {"capabilities", capabilitiesJson(capabilities)},
        {"channelConfig", channelJson}, {"moduleConfig", moduleJson},
        {"pairConfig", pairJson}
    };
    const QByteArray json = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    QDataStream stream(&m_file);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << FileMagic << FileVersion << quint16(0) << quint32(json.size());
    stream.writeRawData(json.constData(), json.size());
    m_events = 0;
    if (stream.status() != QDataStream::Ok) {
        if (error) *error = m_file.errorString();
        m_file.close();
        return false;
    }
    return true;
}

bool ListFileWriter::appendPacket(const QByteArray &mca2Packet, QString *error) {
    if (!m_file.isOpen()) {
        if (error) *error = QStringLiteral("MCAL v3 文件未打开");
        return false;
    }
    DecodedPacket packet;
    QString decodeError;
    if (!PacketDecoder::decode(mca2Packet, packet, &decodeError)) {
        if (error) *error = QStringLiteral("不能写入无效 MCA2 包：%1").arg(decodeError);
        return false;
    }
    QDataStream stream(&m_file);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << PacketBlockMagic << quint32(mca2Packet.size());
    stream.writeRawData(mca2Packet.constData(), mca2Packet.size());
    m_events += packet.eventCount();
    if (stream.status() != QDataStream::Ok) {
        if (error) *error = m_file.errorString();
        return false;
    }
    return true;
}

bool ListFileWriter::close(bool complete, const PacketStats &stats,
                           QString *error) {
    if (!m_file.isOpen())
        return true;
    QDataStream stream(&m_file);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << FooterMagic << m_events << stats.received << stats.lost
           << stats.duplicate << stats.reordered
           << quint8(complete ? 1 : 0);
    m_file.flush();
    const bool ok = stream.status() == QDataStream::Ok
                 && m_file.error() == QFile::NoError;
    if (!ok && error) *error = m_file.errorString();
    m_file.close();
    return ok;
}

bool ListFileReader::readPackets(const QString &path,
                                 QVector<DecodedPacket> &packets,
                                 QString *error, quint64 eventLimit) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::BigEndian);
    quint32 magic = 0, jsonBytes = 0;
    quint16 version = 0, reserved = 0;
    stream >> magic >> version >> reserved >> jsonBytes;
    if (magic != FileMagic) {
        if (error) *error = QStringLiteral("不是 MCAL 文件");
        return false;
    }
    if (version != FileVersion) {
        if (error) {
            *error = QStringLiteral("MCAL v%1 与当前软件不兼容；仅支持 MCAL v3")
                         .arg(version);
        }
        return false;
    }
    if (jsonBytes > 1024 * 1024 || file.bytesAvailable() < jsonBytes) {
        if (error) *error = QStringLiteral("MCAL v3 元数据长度无效");
        return false;
    }
    if (!file.seek(file.pos() + jsonBytes)) {
        if (error) *error = QStringLiteral("无法跳过 MCAL v3 元数据");
        return false;
    }

    packets.clear();
    quint64 events = 0;
    while (!stream.atEnd() && events < eventLimit) {
        quint32 marker = 0;
        stream >> marker;
        if (marker == FooterMagic)
            break;
        if (marker != PacketBlockMagic) {
            if (error) *error = QStringLiteral("MCAL v3 数据块标记损坏");
            return false;
        }
        quint32 packetBytes = 0;
        stream >> packetBytes;
        if (packetBytes < PacketHeaderBytes ||
            packetBytes > MaximumUdpPayloadBytes ||
            file.bytesAvailable() < packetBytes) {
            if (error) *error = QStringLiteral("MCAL v3 数据包长度无效");
            return false;
        }
        QByteArray raw(int(packetBytes), Qt::Uninitialized);
        if (stream.readRawData(raw.data(), raw.size()) != raw.size()) {
            if (error) *error = QStringLiteral("读取 MCAL v3 数据包失败");
            return false;
        }
        DecodedPacket packet;
        QString decodeError;
        if (!PacketDecoder::decode(raw, packet, &decodeError)) {
            if (error) *error = QStringLiteral("MCAL v3 内含损坏包：%1").arg(decodeError);
            return false;
        }
        events += packet.eventCount();
        packets.push_back(std::move(packet));
    }
    if (stream.status() != QDataStream::Ok && !stream.atEnd()) {
        if (error) *error = QStringLiteral("读取 MCAL v3 失败");
        return false;
    }
    return true;
}

bool ListFileReader::readAll(const QString &path, QVector<EventRecord> &events,
                             QString *error, quint64 limit) {
    QVector<DecodedPacket> packets;
    if (!readPackets(path, packets, error, limit))
        return false;
    events.clear();
    for (const auto &packet : packets) {
        for (const auto &event : packet.channelEvents) {
            if (quint64(events.size()) >= limit)
                return true;
            events.push_back(event);
        }
        const auto appendModuleChannels = [&](const ModuleEvent &module) {
            for (int i = 0; i < 4 && quint64(events.size()) < limit; ++i) {
                ChannelEvent event;
                event.timestamp = quint64(qint64(module.timestamp)
                                  + module.relativeTime[i]);
                event.peak = module.peak[i];
                event.integral = module.integral[i];
                const quint8 quality = module.channelFlags[i];
                event.channel = quality >> 3;
                event.pileup = (quality & 0x04) != 0;
                event.overflow = (quality & 0x02) != 0;
                event.polarity = (quality & 0x01) != 0;
                events.push_back(event);
            }
        };
        for (const auto &event : packet.moduleEvents)
            appendModuleChannels(event);
        for (const auto &event : packet.coincidenceEvents) {
            appendModuleChannels(event.scatter);
            appendModuleChannels(event.absorber);
        }
    }
    return true;
}

} // namespace mca
