#pragma once
#include "core/types.h"
#include <QImage>
#include <QObject>
#include <QThread>
#include <QMutex>
#include <limits>

namespace mca {
struct PsdSettings {
    double xMin=0, xMax=262144, rMin=0, rMax=1;
    double roiMin=0, roiMax=262144, fitMin=0, fitMax=1, threshold=.20;
    bool highIsNeutron=true, fitEnabled=false;
    quint64 revision=0;
    bool operator==(const PsdSettings &) const = default;
};
struct PsdSnapshot {
    PsdSettings settings;
    QVector<quint64> density, projection, neutron, gamma;
    quint64 retained=0, received=0, pileup=0, overrange=0, arithmetic=0, nonpositive=0, outside=0;
    quint64 roiEvents=0;
    quint32 configVersion=0;
    double fom=std::numeric_limits<double>::quiet_NaN();
    FitResult fit;
    QImage densityImage;
    quint64 fitDataVersion=0, analysisQueueHighWater=0, recordQueueHighWater=0;
};
class PsdStore {
public:
    explicit PsdStore(qsizetype capacity=1000000):m_capacity(qMax<qsizetype>(1,capacity)) {}
    void clear();
    void add(const QVector<PsdEvent> &events);
    std::shared_ptr<const PsdSnapshot> snapshot(const PsdSettings &settings, bool computeFit=true) const;
    qsizetype size() const { return m_events.size(); }
private:
    void adjust(const PsdEvent &event, bool remove) const;
    mutable PsdSnapshot m_counts;
    mutable bool m_countsValid=false;
    QVector<PsdEvent> m_events;
    qsizetype m_capacity, m_next=0;
    quint64 m_received=0;
    quint32 m_version=0;
};
QString psdHeader(const ChannelConfig &config, bool filtered);
QString psdEventLine(const PsdEvent &event);
bool readPsdEvents(const QString &path, QVector<PsdEvent> &events, QString *error=nullptr, PsdStore *boundedStore=nullptr);
class PsdWorker;
class PsdRecordWorker;
class PsdSession : public QObject {
    Q_OBJECT
public:
    explicit PsdSession(QObject *parent=nullptr);
    ~PsdSession() override;
    void begin(const ChannelConfig &config, bool filtered, const QString &path);
    void addEvents(const QVector<PsdEvent> &events);
    void setSettings(PsdSettings settings);
    void replay(const QString &path);
    void end();
    void clear();
signals:
    void snapshotReady(std::shared_ptr<const PsdSnapshot> snapshot);
    void errorOccurred(const QString &error);
private:
    void drainAnalysis();
    void drainRecording();
    QThread m_thread, m_recordThread;
    PsdRecordWorker *m_recorder;
    PsdWorker *m_worker;
    QMutex m_pendingMutex;
    QVector<PsdEvent> m_pending;
    bool m_drainQueued=false, m_recordDrainQueued=false, m_recording=false;
    bool m_analysisFailed=false, m_recordFailed=false;
    QVector<PsdEvent> m_recordPending;
    quint64 m_analysisHighWater=0, m_recordHighWater=0;
    qsizetype m_analysisActive=0,m_recordActive=0;
    QMutex m_deliveryMutex;
    std::shared_ptr<const PsdSnapshot> m_latestSnapshot;
    bool m_snapshotQueued=false;
};
}
Q_DECLARE_METATYPE(std::shared_ptr<const mca::PsdSnapshot>)
