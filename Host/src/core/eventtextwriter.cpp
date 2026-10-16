#include "core/eventtextwriter.h"
#include <QFile>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QStringConverter>
#include <QTextStream>
#include <QThread>

namespace mca {

class EventTextIoWorker final : public QObject {
    Q_OBJECT
public:
    bool openFile(const QString &path, const EventTextMetadata &metadata,
                  QString *error) {
        closeFile(false, QStringLiteral("new recording started"));
        m_file.setFileName(path);
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error)
                *error = QStringLiteral("无法创建事件 TXT：%1").arg(m_file.errorString());
            return false;
        }

        m_total = 0;
        QByteArray header;
        QTextStream stream(&header, QIODevice::WriteOnly);
        stream.setEncoding(QStringConverter::Utf8);
        stream << "# format=MCA_EVENT_TXT_V2\n";
        stream << "# delimiter=TAB\n";
        stream << "# started_at="
               << metadata.startedAt.toString(Qt::ISODateWithMs) << '\n';
        stream << "# device_ip=" << metadata.deviceAddress.toString() << '\n';
        stream << "# sample_rate_hz=" << metadata.sampleRateHz << '\n';
        stream << "# peak_fraction_bits="
               << int(metadata.peakFractionBits) << '\n';
        stream << "# polarity=0:positive,1:negative\n";
        stream << "eventcount\ttimestamp\tchannel\tpeak\tintegral"
                  "\tpileup\tpolarity\n";
        stream.flush();
        if (m_file.write(header) != header.size()) {
            if (error)
                *error = QStringLiteral("写入事件 TXT 文件头失败：%1")
                             .arg(m_file.errorString());
            m_file.close();
            return false;
        }
        m_sampleRateHz = metadata.sampleRateHz ? metadata.sampleRateHz
                                               : 80'000'000;
        m_peakFractionBits = qMin<quint8>(metadata.peakFractionBits, 15);
        return true;
    }

    void writeBatch(QVector<ChannelEvent> events) {
        if (!m_file.isOpen()) {
            emit failed(QStringLiteral("事件 TXT 未打开"));
            emit batchFinished(events.size(), m_total);
            return;
        }

        QByteArray bytes;
        bytes.reserve(events.size() * 80);
        QTextStream stream(&bytes, QIODevice::WriteOnly);
        stream.setEncoding(QStringConverter::Utf8);
        for (const auto &event : events) {
            stream << m_total++ << '\t'
                   << event.timestamp << '\t'
                   << (static_cast<int>(event.channel) + 1) << '\t'
                   << (event.peak >> m_peakFractionBits) << '\t'
                   << event.integral << '\t'
                   << (event.pileup ? 1 : 0) << '\t'
                   << (event.polarity ? 1 : 0) << '\n';
        }
        stream.flush();

        if (m_file.write(bytes) != bytes.size()) {
            const QString message =
                QStringLiteral("事件 TXT 写入失败：%1").arg(m_file.errorString());
            emit failed(message);
        }
        emit batchFinished(events.size(), m_total);
    }

    void closeFile(bool complete, const QString &reason) {
        if (!m_file.isOpen())
            return;
        QByteArray footer;
        QTextStream stream(&footer, QIODevice::WriteOnly);
        stream.setEncoding(QStringConverter::Utf8);
        stream << "# completed=" << (complete ? 1 : 0) << '\n';
        stream << "# events=" << m_total << '\n';
        stream << "# ended_at="
               << QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
               << '\n';
        if (!reason.isEmpty())
            stream << "# reason=" << reason.simplified() << '\n';
        stream.flush();
        const bool footerWritten = m_file.write(footer) == footer.size();
        const bool flushed = m_file.flush();
        if (!footerWritten || !flushed)
            emit failed(QStringLiteral("事件 TXT 结束标记写入失败：%1")
                            .arg(m_file.errorString()));
        m_file.close();
    }

    quint64 total() const { return m_total; }

signals:
    void batchFinished(qsizetype count, quint64 total);
    void failed(const QString &message);

private:
    QFile m_file;
    quint32 m_sampleRateHz = 80'000'000;
    quint8 m_peakFractionBits = 4;
    quint64 m_total = 0;
};

EventTextWriter::EventTextWriter(QObject *parent) : QObject(parent) {
    m_thread = new QThread(this);
    m_worker = new EventTextIoWorker;
    m_worker->moveToThread(m_thread);
    connect(m_worker, &EventTextIoWorker::batchFinished,
            this, &EventTextWriter::onBatchFinished, Qt::DirectConnection);
    connect(m_worker, &EventTextIoWorker::failed,
            this, &EventTextWriter::onWriteFailed, Qt::DirectConnection);
    m_thread->start();
}

EventTextWriter::~EventTextWriter() {
    finish(false, QStringLiteral("writer destroyed"));
    m_thread->quit();
    m_thread->wait();
    delete m_worker;
}

bool EventTextWriter::start(const QString &path,
                            const EventTextMetadata &metadata,
                            QString *error) {
    if (path.isEmpty()) {
        if (error)
            *error = QStringLiteral("事件 TXT 路径为空");
        return false;
    }
    finish(false, QStringLiteral("new recording started"));

    bool opened = false;
    QString openError;
    QMetaObject::invokeMethod(
        m_worker,
        [this, &opened, &openError, path, metadata] {
            opened = m_worker->openFile(path, metadata, &openError);
        },
        Qt::BlockingQueuedConnection);
    if (!opened) {
        if (error)
            *error = openError;
        return false;
    }

    m_path = path;
    m_pendingEvents.store(0, std::memory_order_release);
    m_writtenEvents.store(0, std::memory_order_release);
    m_open.store(true, std::memory_order_release);
    return true;
}

bool EventTextWriter::enqueue(const QVector<ChannelEvent> &events,
                              QString *error) {
    if (events.isEmpty() || !isOpen())
        return true;

    const qsizetype pending =
        m_pendingEvents.fetch_add(events.size(), std::memory_order_acq_rel)
        + events.size();
    if (pending > MaxPendingEvents) {
        m_pendingEvents.fetch_sub(events.size(), std::memory_order_acq_rel);
        m_open.store(false, std::memory_order_release);
        const QString message = QStringLiteral(
            "事件 TXT 待写队列超过 %1 个事件，磁盘写入速度不足")
            .arg(MaxPendingEvents);
        if (error)
            *error = message;
        emit writeFailed(message);
        return false;
    }

    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker, batch = events]() mutable {
            worker->writeBatch(std::move(batch));
        },
        Qt::QueuedConnection);
    return true;
}

void EventTextWriter::finish(bool complete, const QString &reason) {
    if (!m_worker || !m_thread || !m_thread->isRunning())
        return;
    const bool wasOpen = m_open.exchange(false, std::memory_order_acq_rel);
    if (!wasOpen && m_path.isEmpty())
        return;
    quint64 finalTotal = 0;
    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker, complete, reason, &finalTotal] {
            worker->closeFile(complete, reason);
            finalTotal = worker->total();
        },
        Qt::BlockingQueuedConnection);
    m_writtenEvents.store(finalTotal, std::memory_order_release);
    m_pendingEvents.store(0, std::memory_order_release);
    m_path.clear();
}

QString EventTextWriter::path() const {
    return m_path;
}

void EventTextWriter::onBatchFinished(qsizetype count, quint64 total) {
    m_pendingEvents.fetch_sub(count, std::memory_order_acq_rel);
    m_writtenEvents.store(total, std::memory_order_release);
    emit progress(total);
}

void EventTextWriter::onWriteFailed(const QString &message) {
    m_open.store(false, std::memory_order_release);
    emit writeFailed(message);
}

} // namespace mca

#include "eventtextwriter.moc"
