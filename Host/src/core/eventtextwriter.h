#pragma once

#include "core/types.h"
#include <QObject>
#include <QHostAddress>
#include <atomic>

class QThread;

namespace mca {

struct EventTextMetadata {
    QHostAddress deviceAddress;
    quint32 sampleRateHz = 80'000'000;
    quint8 peakFractionBits = 4;
    QDateTime startedAt;
};

class EventTextIoWorker;

class EventTextWriter final : public QObject {
    Q_OBJECT
public:
    static constexpr qsizetype MaxPendingEvents = 1'000'000;

    explicit EventTextWriter(QObject *parent = nullptr);
    ~EventTextWriter() override;

    bool start(const QString &path, const EventTextMetadata &metadata,
               QString *error = nullptr);
    bool enqueue(const QVector<ChannelEvent> &events, QString *error = nullptr);
    void finish(bool complete, const QString &reason = {});

    bool isOpen() const { return m_open.load(std::memory_order_acquire); }
    quint64 writtenEvents() const {
        return m_writtenEvents.load(std::memory_order_acquire);
    }
    QString path() const;

signals:
    void progress(quint64 eventsWritten);
    void writeFailed(const QString &message);

private:
    void onBatchFinished(qsizetype count, quint64 total);
    void onWriteFailed(const QString &message);

    QThread *m_thread = nullptr;
    EventTextIoWorker *m_worker = nullptr;
    std::atomic_bool m_open{false};
    std::atomic<qsizetype> m_pendingEvents{0};
    std::atomic<quint64> m_writtenEvents{0};
    QString m_path;
};

} // namespace mca
