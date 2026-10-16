#include "core/psd.h"
#include "analysis/analysis.h"
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <QMutexLocker>
#include <QElapsedTimer>
#include <array>
#include <cmath>

namespace mca {
void PsdStore::clear() {
    m_events.clear();m_next=0;m_received=0;m_version=0;m_countsValid=false;
}
void PsdStore::adjust(const PsdEvent &e, bool remove) const {
    auto &out=m_counts;const auto &s=out.settings;
    auto change=[remove](quint64 &v){if(remove)--v;else ++v;};
    bool bad=false;
    if(e.legacy.pileup){change(out.pileup);bad=true;}
    if(e.quality&1){change(out.overrange);bad=true;}
    if(e.quality&2){change(out.arithmetic);bad=true;}
    if(e.qtotal<=0){change(out.nonpositive);bad=true;}
    if(bad || !(s.xMax>s.xMin && s.rMax>s.rMin))return;
    const double r=double(e.qtail)/e.qtotal;
    const bool inX=e.qtotal>=s.xMin && e.qtotal<s.xMax;
    const bool inR=r>=s.rMin && r<s.rMax;
    const int x=inX ? qMin(1023,int((e.qtotal-s.xMin)*1024/(s.xMax-s.xMin))) : -1;
    const int y=inR ? qMin(511,int((r-s.rMin)*512/(s.rMax-s.rMin))) : -1;
    if(!inX || !inR)change(out.outside);
    if(inX && inR)change(out.density[y*1024+x]);
    if(inX){const bool neutron=(r>=s.threshold)==s.highIsNeutron;
        change((neutron ? out.neutron : out.gamma)[x]);}
    if(e.qtotal>=s.roiMin && e.qtotal<s.roiMax && inR){
        change(out.projection[y]);change(out.roiEvents);}
}
void PsdStore::add(const QVector<PsdEvent> &events) {
    for(const auto &e:events) {
        if(!m_events.isEmpty() && e.configVersion!=m_version) {
            if(qint32(e.configVersion-m_version)<0)continue;
            clear();
        }
        m_version=e.configVersion;++m_received;
        if(m_events.size()<m_capacity)m_events.push_back(e);
        else {
            if(m_countsValid)adjust(m_events[m_next],true);
            m_events[m_next]=e;m_next=(m_next+1)%m_capacity;
        }
        if(m_countsValid)adjust(e,false);
    }
}
std::shared_ptr<const PsdSnapshot> PsdStore::snapshot(const PsdSettings &s,bool computeFit) const {
    if(!m_countsValid || !(m_counts.settings==s)) {
        m_counts={};m_counts.settings=s;
        m_counts.density.fill(0,1024*512);m_counts.projection.fill(0,512);
        m_counts.neutron.fill(0,1024);m_counts.gamma.fill(0,1024);
        for(const auto &e:m_events)adjust(e,false);
        m_countsValid=true;
    }
    auto out=std::make_shared<PsdSnapshot>(m_counts);
    out->retained=m_events.size();out->received=m_received;out->configVersion=m_version;
    if(!(s.xMax>s.xMin && s.rMax>s.rMin))return out;
    const double rScale=512/(s.rMax-s.rMin);
    if(computeFit && s.fitEnabled && out->roiEvents>=100) {
        const int left=qBound(0,int(std::floor((s.fitMin-s.rMin)*rScale)),511);
        const int right=qBound(0,int(std::ceil((s.fitMax-s.rMin)*rScale))-1,511);
        QVector<int> candidates,centers;
        quint64 maximum=0;for(int i=left;i<=right;++i)maximum=qMax(maximum,out->projection[i]);
        for(int i=left;i<=right;) {
            const auto count=out->projection[i];int end=i;
            while(end<right && out->projection[end+1]==count)++end;
            // Treat an equal-height plateau as one local maximum, not several seeds.
            if(count>0 && double(count)>=.05*maximum &&
               (i==left || count>out->projection[i-1]) && (end==right || count>out->projection[end+1]))
                candidates.push_back((i+end)/2);
            i=end+1;
        }
        std::sort(candidates.begin(),candidates.end(),[&](int a,int b){return out->projection[a]>out->projection[b];});
        for(int candidate:candidates) {
            if(centers.isEmpty() || std::abs(candidate-centers[0])>=3)centers.push_back(candidate);
            if(centers.size()==2)break;
        }
        if(centers.size()==2)out->fit=AnalysisService::fitGaussian(out->projection,left,right,2,true,centers);
        quint64 fitCount=0; for(int i=left;i<=right;++i)fitCount+=out->projection[i];
        if(fitCount>=100 && out->fit.valid && out->fit.peaks.size()==2) {
            const auto &a=out->fit.peaks[0],&b=out->fit.peaks[1];
            double denominator=a.fwhm+b.fwhm;
            double value=std::abs(a.centroid-b.centroid)/denominator;
            if(denominator>0 && std::isfinite(value) && a.area>0 && b.area>0)out->fom=value;
        }
    }
    if(computeFit && s.fitEnabled)out->fitDataVersion=m_received;
    return out;
}
QString psdHeader(const ChannelConfig &c,bool filtered) {
    return QStringLiteral("# format=MCA_PSD_TXT_V1\n# sample_rate_hz=125000000\n# sample_ns=8\n"
                          "# total_samples=%1\n# pre_samples=%2\n# tail_start=%3\n# tail_end=%4\n# filtered=%5\n"
                          "# threshold=%6\n# polarity=%7\n# charge_unit=ADC.sample\n"
                          "timestamp\tconfig_version\tpeak_raw\tintegral_raw\tbaseline\tqtotal\tqtail\tchannel\tpileup\toverflow\tpolarity\tquality\n")
        .arg(c.integrationPostSamples).arg(c.integrationPreSamples).arg(c.psdTailStart).arg(c.psdTailEnd)
        .arg(filtered?1:0).arg(c.thresholdLsb).arg(int(c.polarity));
}
QString psdEventLine(const PsdEvent &e) {
    return QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9\t%10\t%11\t%12\n")
        .arg(e.legacy.timestamp).arg(e.configVersion).arg(e.legacy.peak).arg(e.legacy.integral)
        .arg(e.baseline).arg(e.qtotal).arg(e.qtail).arg(e.legacy.channel)
        .arg(e.legacy.pileup?1:0).arg(e.legacy.overflow?1:0).arg(e.legacy.polarity?1:0).arg(e.quality);
}
bool readPsdEvents(const QString &path,QVector<PsdEvent> &events,QString *error,PsdStore *boundedStore) {
    QFile file(path);auto fail=[&](QString msg){if(error)*error=msg;return false;};
    if(!file.open(QIODevice::ReadOnly|QIODevice::Text))return fail(file.errorString());
    QTextStream in(&file);
    if(in.readLine()!=QStringLiteral("# format=MCA_PSD_TXT_V1"))return fail(QStringLiteral("不是 PSD V1 事件文件"));
    events.clear();QVector<PsdEvent> batch;
    if(boundedStore)boundedStore->clear();
    qsizetype line=1;
    while(!in.atEnd()) {
        const auto text=in.readLine();++line;
        if(text.startsWith('#')||text.startsWith("timestamp")||text.isEmpty())continue;
        const auto fields=text.split('\t');if(fields.size()!=12)return fail(QStringLiteral("PSD 文件第 %1 行列数错误").arg(line));
        qint64 v[12];
        for(int i=0;i<12;++i){bool ok;v[i]=fields[i].toLongLong(&ok);if(!ok)return fail(QStringLiteral("PSD 文件第 %1 行数值错误").arg(line));}
        if(v[0]<0||v[0]>0xffffffffffffLL||v[1]<0||v[1]>0xffffffffLL||v[2]<0||v[2]>65535||
           v[3]<0||v[3]>0xffffff||v[4]<0||v[4]>16383||v[5]<INT32_MIN||v[5]>INT32_MAX||
           v[6]<INT32_MIN||v[6]>INT32_MAX||v[7]<0||v[7]>31||v[8]<0||v[8]>1||v[9]<0||v[9]>1||
           v[10]<0||v[10]>1||v[11]<0||v[11]>3)return fail(QStringLiteral("PSD 文件第 %1 行越界").arg(line));
        PsdEvent e;e.legacy.timestamp=v[0];e.configVersion=quint32(v[1]);e.legacy.peak=quint16(v[2]);
        e.legacy.integral=quint32(v[3]);e.baseline=quint16(v[4]);e.qtotal=qint32(v[5]);e.qtail=qint32(v[6]);
        e.legacy.channel=quint8(v[7]);e.legacy.pileup=v[8]!=0;e.legacy.overflow=v[9]!=0;e.legacy.polarity=v[10]!=0;e.quality=quint8(v[11]);
        if(boundedStore) {
            batch.push_back(e);if(batch.size()==1024){boundedStore->add(batch);batch.clear();}
        } else events.push_back(e);
        if(!boundedStore && events.size()>MaxEventsPerRun)return fail(QStringLiteral("PSD 文件超过 2000 万条事件上限"));
    }
    if(boundedStore && !batch.isEmpty())boundedStore->add(batch);
    return true;
}
class PsdWorker : public QObject {
    Q_OBJECT
public:
    PsdStore store;
    PsdSettings settings;
    bool dirty=true;
    QElapsedTimer fitElapsed;
    FitResult lastFit;
    double lastFom=std::numeric_limits<double>::quiet_NaN();
    quint64 lastFitVersion=0;
    quint32 lastFitConfig=0;
    quint64 analysisHighWater=0,recordHighWater=0;
    void invalidateFit(){fitElapsed.invalidate();lastFit={};lastFom=std::numeric_limits<double>::quiet_NaN();lastFitVersion=0;}
    void publish() {
        if(!dirty)return;dirty=false;
        const bool fit=settings.fitEnabled && (!fitElapsed.isValid() || fitElapsed.elapsed()>=1000);
        auto out=std::make_shared<PsdSnapshot>(*store.snapshot(settings,fit));
        if(lastFitConfig!=out->configVersion){invalidateFit();lastFitConfig=out->configVersion;}
        if(fit){lastFit=out->fit;lastFom=out->fom;lastFitVersion=out->fitDataVersion;fitElapsed.restart();}
        else if(settings.fitEnabled){out->fit=lastFit;out->fom=lastFom;out->fitDataVersion=lastFitVersion;}
        out->analysisQueueHighWater=analysisHighWater;out->recordQueueHighWater=recordHighWater;
        out->densityImage=QImage(1024,512,QImage::Format_RGB32);
        quint64 maximum=1;for(auto count:out->density)maximum=qMax(maximum,count);
        const double scale=4095/std::log1p(double(maximum));
        std::array<QRgb,4096> colors;
        colors[0]=qRgb(255,255,255);
        for(int i=1;i<4096;++i){double c=double(i)/4095;colors[i]=QColor::fromRgbF(1-.88*c,1-.60*c,1-.30*c).rgb();}
        for(int y=0;y<512;++y){auto row=reinterpret_cast<QRgb*>(out->densityImage.scanLine(511-y));
            for(int x=0;x<1024;++x){auto count=out->density[y*1024+x];
                const int color=count ? qBound(1,int(std::log1p(double(count))*scale),4095):0;
                row[x]=colors[color];}}
        emit snapshotReady(out);
    }
signals:
    void snapshotReady(std::shared_ptr<const PsdSnapshot> snapshot);
};
class PsdRecordWorker : public QObject {
    Q_OBJECT
public:
    QFile file;
    void close(){if(file.isOpen()){if(!file.flush())emit errorOccurred(file.errorString());file.close();}}
    void begin(const QString &path,const QByteArray &header){
        close();file.setFileName(path);
        if(!file.open(QIODevice::WriteOnly|QIODevice::Truncate)){emit errorOccurred(file.errorString());return;}
        if(file.write(header)!=header.size()){emit errorOccurred(file.errorString());close();}
    }
    void write(const QVector<PsdEvent> &events){
        if(!file.isOpen())return;
        QByteArray bytes;bytes.reserve(qMin<qsizetype>(events.size(),4096)*100);
        for(const auto &e:events){bytes+=psdEventLine(e).toUtf8();
            if(bytes.size()>=256*1024){if(file.write(bytes)!=bytes.size()){emit errorOccurred(file.errorString());close();return;}bytes.clear();}}
        if(!bytes.isEmpty() && file.write(bytes)!=bytes.size()){emit errorOccurred(file.errorString());close();}
    }
signals:
    void errorOccurred(QString error);
};
PsdSession::PsdSession(QObject *parent):QObject(parent),m_recorder(new PsdRecordWorker),m_worker(new PsdWorker) {
    qRegisterMetaType<std::shared_ptr<const PsdSnapshot>>();
    m_worker->moveToThread(&m_thread);m_recorder->moveToThread(&m_recordThread);
    connect(&m_thread,&QThread::finished,m_worker,&QObject::deleteLater);
    connect(&m_recordThread,&QThread::finished,m_recorder,&QObject::deleteLater);
    connect(m_worker,&PsdWorker::snapshotReady,this,[this](std::shared_ptr<const PsdSnapshot> snapshot){
        QMutexLocker lock(&m_deliveryMutex);m_latestSnapshot=std::move(snapshot);
        if(m_snapshotQueued)return;m_snapshotQueued=true;
        QMetaObject::invokeMethod(this,[this]{std::shared_ptr<const PsdSnapshot> latest;
            {QMutexLocker lock(&m_deliveryMutex);latest=std::move(m_latestSnapshot);m_snapshotQueued=false;}
            emit snapshotReady(latest);
        },Qt::QueuedConnection);
    },Qt::DirectConnection);
    connect(m_recorder,&PsdRecordWorker::errorOccurred,this,[this](const QString &error){
        {QMutexLocker lock(&m_pendingMutex);m_recording=false;m_recordFailed=true;}
        emit errorOccurred(error);
    });
    m_thread.start();m_recordThread.start();
    QMetaObject::invokeMethod(m_worker,[w=m_worker]{auto timer=new QTimer(w);timer->setTimerType(Qt::PreciseTimer);timer->setInterval(100);
        QObject::connect(timer,&QTimer::timeout,w,[w]{w->publish();});timer->start();});
}
PsdSession::~PsdSession(){
    {QMutexLocker lock(&m_pendingMutex);m_recording=false;}
    QMetaObject::invokeMethod(m_worker,[this]{drainAnalysis();},Qt::BlockingQueuedConnection);
    QMetaObject::invokeMethod(m_recorder,[this]{drainRecording();m_recorder->close();},Qt::BlockingQueuedConnection);
    m_thread.quit();m_thread.wait();m_recordThread.quit();m_recordThread.wait();
}
void PsdSession::begin(const ChannelConfig &c,bool filtered,const QString &path){
    // Begin is issued before acquisition starts; barriers retire the previous session.
    {QMutexLocker lock(&m_pendingMutex);m_recording=false;}
    QMetaObject::invokeMethod(m_worker,[this]{drainAnalysis();m_worker->store.clear();m_worker->invalidateFit();m_worker->dirty=true;},Qt::BlockingQueuedConnection);
    QMetaObject::invokeMethod(m_recorder,[this,path,c,filtered]{drainRecording();m_recorder->close();
        if(!path.isEmpty())m_recorder->begin(path,psdHeader(c,filtered).toUtf8());},Qt::BlockingQueuedConnection);
    {QMutexLocker lock(&m_pendingMutex);m_analysisFailed=false;m_recordFailed=false;
        m_analysisHighWater=0;m_recordHighWater=0;m_recording=!path.isEmpty();}
}
void PsdSession::addEvents(const QVector<PsdEvent> &events){
    bool queueAnalysis=false,queueRecord=false;QString error;
    {
        QMutexLocker lock(&m_pendingMutex);
        if(m_analysisFailed)return;
        if(m_pending.size()+m_analysisActive+events.size()>1000000){m_analysisFailed=true;
            error=QStringLiteral("PSD 分析待处理队列达到 100 万条；停止采集，数据不完整");}
        else {
            m_pending+=events;m_analysisHighWater=qMax(m_analysisHighWater,quint64(m_pending.size()+m_analysisActive));
            if(!m_drainQueued){m_drainQueued=true;queueAnalysis=true;}
        }
        if(m_recording && !m_recordFailed){
            if(m_recordPending.size()+m_recordActive+events.size()>1000000){m_recordFailed=true;m_recording=false;
                error=QStringLiteral("PSD 记录待处理队列达到 100 万条；停止采集，记录文件不完整");}
            else {m_recordPending+=events;m_recordHighWater=qMax(m_recordHighWater,quint64(m_recordPending.size()+m_recordActive));
                if(!m_recordDrainQueued){m_recordDrainQueued=true;queueRecord=true;}}
        }
    }
    if(queueAnalysis)QMetaObject::invokeMethod(m_worker,[this]{drainAnalysis();},Qt::QueuedConnection);
    if(queueRecord)QMetaObject::invokeMethod(m_recorder,[this]{drainRecording();},Qt::QueuedConnection);
    if(!error.isEmpty())emit errorOccurred(error);
}
void PsdSession::drainAnalysis(){
    QVector<PsdEvent> events;
    {QMutexLocker lock(&m_pendingMutex);events.swap(m_pending);m_drainQueued=false;m_analysisActive=events.size();
        m_worker->analysisHighWater=m_analysisHighWater;m_worker->recordHighWater=m_recordHighWater;}
    if(!events.isEmpty()){m_worker->store.add(events);m_worker->dirty=true;}
    {QMutexLocker lock(&m_pendingMutex);m_analysisActive=0;}
}
void PsdSession::drainRecording(){
    QVector<PsdEvent> events;
    {QMutexLocker lock(&m_pendingMutex);events.swap(m_recordPending);m_recordDrainQueued=false;m_recordActive=events.size();}
    if(!events.isEmpty())m_recorder->write(events);
    {QMutexLocker lock(&m_pendingMutex);m_recordActive=0;}
}
void PsdSession::setSettings(PsdSettings s){
    QMetaObject::invokeMethod(m_worker,[w=m_worker,s]{if(s.revision<w->settings.revision)return;
        w->settings=s;w->invalidateFit();w->dirty=true;});
}
void PsdSession::end(){
    {QMutexLocker lock(&m_pendingMutex);m_recording=false;}
    QMetaObject::invokeMethod(m_worker,[this]{drainAnalysis();m_worker->publish();});
    QMetaObject::invokeMethod(m_recorder,[this]{drainRecording();m_recorder->close();});
}
void PsdSession::clear(){QMetaObject::invokeMethod(m_worker,[this]{drainAnalysis();m_worker->store.clear();m_worker->invalidateFit();m_worker->dirty=true;});}
void PsdSession::replay(const QString &path){
    end();
    QMetaObject::invokeMethod(m_worker,[w=m_worker,path,this]{
        QVector<PsdEvent> unused;QString error;PsdStore loaded;
        if(!readPsdEvents(path,unused,&error,&loaded)){emit errorOccurred(error);return;}
        w->store=std::move(loaded);w->invalidateFit();w->dirty=true;
    });
}
}
#include "psd.moc"
