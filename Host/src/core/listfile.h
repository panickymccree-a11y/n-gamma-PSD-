#pragma once
#include "core/protocol.h"
#include <QFile>

namespace mca {

class ListFileWriter {
public:
    bool open(const QString &path, const DeviceCapabilities &capabilities,
              AcquisitionMode mode, const QVector<ChannelConfig> &channels,
              const QVector<ModuleConfig> &modules,
              const QVector<PairConfig> &pairs, quint32 configVersion,
              QString *error = nullptr);
    bool appendPacket(const QByteArray &mca2Packet, QString *error = nullptr);
    bool close(bool complete, const PacketStats &stats,
               QString *error = nullptr);
    bool isOpen() const { return m_file.isOpen(); }

private:
    QFile m_file;
    quint64 m_events = 0;
};

class ListFileReader {
public:
    static bool readPackets(const QString &path, QVector<DecodedPacket> &packets,
                            QString *error = nullptr,
                            quint64 eventLimit = MaxEventsPerRun);
    static bool readAll(const QString &path, QVector<EventRecord> &events,
                        QString *error = nullptr,
                        quint64 limit = MaxEventsPerRun);
};

} // namespace mca
