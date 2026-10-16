#include "core/psd.h"
#include "core/protocol.h"
#include "ui/psdpage.h"
#include <QtTest>
#include <QFontDatabase>
#include <QtEndian>
#include <QTemporaryDir>
#include <QFile>
#include <QCheckBox>
#include <numeric>
#include <cmath>
using namespace mca;
class PsdTests : public QObject {
    Q_OBJECT
    static PsdEvent event(int total,int tail,int flags=0) {
        PsdEvent e;e.qtotal=total;e.qtail=tail;e.configVersion=4;e.baseline=8192;e.quality=quint8(flags);return e;
    }
private slots:
    void initTestCase() {
        QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
        QApplication::setFont(QFont("Microsoft YaHei",9));
    }
    void signedProtocolAndValidation() {
        QByteArray bytes(44,0);auto p=reinterpret_cast<uchar*>(bytes.data());
        qToBigEndian<quint32>(DataMagic,p);p[4]=2;p[5]=7;
        qToBigEndian<quint32>(77,p+12);qToBigEndian<quint16>(1,p+16);qToBigEndian<quint16>(24,p+18);
        qToBigEndian<qint32>(-12345,p+32);qToBigEndian<qint32>(-456,p+36);qToBigEndian<quint16>(8192,p+40);
        DecodedPacket decoded;QString error;
        QVERIFY2(PacketDecoder::decode(bytes,decoded,&error),qPrintable(error));
        QCOMPARE(decoded.psdEvents.size(),1);QCOMPARE(decoded.eventCount(),1);
        QCOMPARE(decoded.psdEvents[0].qtotal,-12345);QCOMPARE(decoded.psdEvents[0].qtail,-456);
        QCOMPARE(decoded.psdEvents[0].configVersion,77u);
        bytes[43]=1;QVERIFY(!PacketDecoder::decode(bytes,decoded,&error));
        bytes[43]=0;bytes[42]=4;QVERIFY(!PacketDecoder::decode(bytes,decoded,&error));
        bytes[42]=0;bytes.chop(1);QVERIFY(!PacketDecoder::decode(bytes,decoded,&error));
    }

    void incrementalMatchesIndependentReference() {
        PsdStore store(257);QVector<PsdEvent> retained;quint64 received=0;
        PsdSettings settings;settings.xMin=100;settings.xMax=9000;
        settings.rMin=-.2;settings.rMax=.8;settings.roiMin=300;settings.roiMax=7000;
        for(int batch=0;batch<40;++batch) {
            QVector<PsdEvent> incoming;
            for(int j=0;j<37;++j) {
                const int n=batch*37+j;auto e=event((n*371)%12000-100,(n*719)%8000-1000,n%19==0 ? 3:0);
                e.legacy.pileup=n%23==0;incoming.push_back(e);retained.push_back(e);++received;
                if(retained.size()>257)retained.removeFirst();
            }
            store.add(incoming);
            if(batch==15){settings.threshold=.35;settings.roiMin=2000;++settings.revision;}
            if(batch==25){settings.xMax=12000;settings.rMax=1.2;settings.highIsNeutron=false;++settings.revision;}
            auto actual=store.snapshot(settings,false);
            QVector<quint64> density(1024*512),projection(512),neutron(1024),gamma(1024);
            quint64 pileup=0,over=0,arithmetic=0,nonpositive=0,outside=0,roi=0;
            for(const auto &e:retained) {
                pileup+=e.legacy.pileup;over+=(e.quality&1)!=0;arithmetic+=(e.quality&2)!=0;nonpositive+=e.qtotal<=0;
                if(e.legacy.pileup || e.quality || e.qtotal<=0)continue;
                const double r=double(e.qtail)/e.qtotal;
                const bool ix=e.qtotal>=settings.xMin && e.qtotal<settings.xMax;
                const bool ir=r>=settings.rMin && r<settings.rMax;
                if(!ix || !ir)++outside;
                int x=ix ? int((e.qtotal-settings.xMin)/(settings.xMax-settings.xMin)*1024):-1;
                int y=ir ? int((r-settings.rMin)/(settings.rMax-settings.rMin)*512):-1;
                if(ix && ir)++density[y*1024+x];
                if(ix)++(((r>=settings.threshold)==settings.highIsNeutron) ? neutron:gamma)[x];
                if(ir && e.qtotal>=settings.roiMin && e.qtotal<settings.roiMax){++projection[y];++roi;}
            }
            QCOMPARE(actual->density,density);QCOMPARE(actual->projection,projection);
            QCOMPARE(actual->neutron,neutron);QCOMPARE(actual->gamma,gamma);
            QCOMPARE(actual->pileup,pileup);QCOMPARE(actual->overrange,over);QCOMPARE(actual->arithmetic,arithmetic);
            QCOMPARE(actual->nonpositive,nonpositive);QCOMPARE(actual->outside,outside);QCOMPARE(actual->roiEvents,roi);
            QCOMPARE(actual->received,received);
        }
    }
    void recordingStopDrainsLargeTail() {
        PsdSession session;QTemporaryDir dir;const auto path=dir.filePath("tail.txt");
        session.begin({},false,path);QVector<PsdEvent> batch;
        for(int i=0;i<25000;++i){auto e=event(1000+i,200);e.legacy.timestamp=i;batch.push_back(e);}
        session.addEvents(batch);session.end();
        // Destruction is the completion barrier, not an arbitrary sleep.
        QTest::qWait(100);
        QVector<PsdEvent> read;QString error;
        QTRY_VERIFY_WITH_TIMEOUT(readPsdEvents(path,read,&error) && read.size()==25000,5000);
        QCOMPARE(read.last().legacy.timestamp,24999uLL);
    }
    void cacheAndThresholdRoiRecalculation() {
        PsdStore store(3);store.add({event(1000,100),event(2000,600),event(3000,900),event(4000,-400)});
        PsdSettings settings;auto s=store.snapshot(settings);
        QCOMPARE(s->retained,3uLL);QCOMPARE(s->received,4uLL);
        QCOMPARE(std::accumulate(s->neutron.begin(),s->neutron.end(),quint64(0)),2uLL);
        QCOMPARE(std::accumulate(s->gamma.begin(),s->gamma.end(),quint64(0)),1uLL);
        QCOMPARE(s->outside,1uLL); // Negative tail charge is preserved, not clipped.
        settings.threshold=.4;settings.roiMin=2500;settings.roiMax=3500;s=store.snapshot(settings);
        QCOMPARE(std::accumulate(s->neutron.begin(),s->neutron.end(),quint64(0)),0uLL);
        QCOMPARE(s->roiEvents,1uLL);
        auto newer=event(5000,500);newer.configVersion=5;store.add({newer,event(1,1)});
        QCOMPARE(store.size(),qsizetype(1));QCOMPARE(store.snapshot(settings)->configVersion,5u);
    }
    void millionEventWindow() {
        PsdStore store;QVector<PsdEvent> events;events.reserve(1000005);
        for(int i=0;i<1000005;++i)events.push_back(event(1000+(i%1000),200));
        store.add(events);QElapsedTimer timer;timer.start();auto snapshot=store.snapshot({});
        QCOMPARE(snapshot->retained,1000000uLL);QCOMPARE(snapshot->received,1000005uLL);
        QCOMPARE(snapshot->roiEvents,1000000uLL);
        qInfo()<<"million-event snapshot milliseconds"<<timer.elapsed();
    }
    void qualityDoesNotRejectLegacyIntegralSaturation() {
        PsdStore store;auto valid=event(1000,200);valid.legacy.overflow=true;
        auto pileup=event(1000,100);pileup.legacy.pileup=true;
        store.add({valid,pileup,event(1000,100,1),event(1000,100,2),event(0,5)});
        auto s=store.snapshot({});QCOMPARE(s->pileup,1uLL);QCOMPARE(s->overrange,1uLL);
        QCOMPARE(s->arithmetic,1uLL);QCOMPARE(s->nonpositive,1uLL);QCOMPARE(s->roiEvents,1uLL);
    }
    void doublePeakFom() {
        PsdStore store;QVector<PsdEvent> events;
        // Deterministic integer synthetic mixture: means .15/.40, sigma .02.
        for(int i=0;i<512;i++) {
            double r=(i+.5)/512;
            int count=qRound(100*std::exp(-.5*std::pow((r-.15)/.02,2))+100*std::exp(-.5*std::pow((r-.40)/.02,2)));
            for(int j=0;j<count;j++)events.push_back(event(100000,qRound(r*100000)));
        }
        store.add(events);PsdSettings s;s.fitEnabled=true;s.fitMin=.05;s.fitMax=.5;
        auto snapshot=store.snapshot(s);QVERIFY(snapshot->fit.valid);QVERIFY(std::isfinite(snapshot->fom));
        QVERIFY2(std::abs(snapshot->fom-(.25/(2*2.354820045*.02)))<.15,qPrintable(QString::number(snapshot->fom)));
        store.clear();store.add({event(1000,200)});QVERIFY(!std::isfinite(store.snapshot(s)->fom));
    }
    void fileRoundTrip() {
        QTemporaryDir dir;QFile file(dir.filePath("events.txt"));QVERIFY(file.open(QIODevice::WriteOnly));
        ChannelConfig c;c.integrationPostSamples=150;
        auto e=event(-1234,-567,1);e.legacy.timestamp=123456;e.legacy.polarity=true;
        file.write(psdHeader(c,false).toUtf8());file.write(psdEventLine(e).toUtf8());file.close();
        QVector<PsdEvent> read;QString error;QVERIFY2(readPsdEvents(file.fileName(),read,&error),qPrintable(error));
        QCOMPARE(read.size(),1);QCOMPARE(read[0].qtotal,-1234);QCOMPARE(read[0].qtail,-567);
        QCOMPARE(read[0].legacy.timestamp,123456uLL);QVERIFY(read[0].legacy.polarity);
        QVERIFY(file.open(QIODevice::Append));file.write("broken\n");file.close();QVERIFY(!readPsdEvents(file.fileName(),read,&error));
    }
    void workerRevisionAndRecording() {
        PsdSession session;QSignalSpy spy(&session,&PsdSession::snapshotReady);
        QTemporaryDir dir;auto path=dir.filePath("all.txt");ChannelConfig c;
        session.begin(c,false,path);PsdSettings s;s.revision=5;session.setSettings(s);
        session.addEvents({event(1000,100),event(2000,600)});s.revision=6;s.threshold=.4;session.setSettings(s);
        QTRY_VERIFY_WITH_TIMEOUT(!spy.isEmpty(),3000);
        auto snapshot=qvariant_cast<std::shared_ptr<const PsdSnapshot>>(spy.last()[0]);
        QTRY_VERIFY_WITH_TIMEOUT(qvariant_cast<std::shared_ptr<const PsdSnapshot>>(spy.last()[0])->settings.revision==6,3000);
        session.end();QTest::qWait(200);QVector<PsdEvent> read;QString error;
        QVERIFY2(readPsdEvents(path,read,&error),qPrintable(error));QCOMPARE(read.size(),2);
    }
    void pageRenderAndWindowDefaults() {
        PsdPage page;page.resize(1250,850);page.show();QString error;QVERIFY(page.validate(&error));
        auto c=page.channelConfig({});QCOMPARE(c.integrationPostSamples,quint16(150));QCOMPARE(c.psdTailStart,qint16(7));
        QVector<PsdEvent> events;
        for(int i=0;i<3000;i++)events.push_back(event(1000+i*75,qRound((1000+i*75)*((i%2?.35:.15)+.0003*(i%37-18)))));
        page.addEvents(events);QTest::qWait(300);
        QVERIFY(page.grab().save("psd_page_preview.png"));
        QSignalSpy snapshots(page.session(),&PsdSession::snapshotReady);
        const auto original=page.grab().toImage();
        for(int i=0;i<4;++i){
            auto box=page.findChild<QCheckBox*>(QStringLiteral("psdAutoRange%1").arg(i));
            QVERIFY(box);QVERIFY(!box->isChecked());box->setChecked(true);
            QVERIFY(page.grab().toImage()!=original);
            for(int j=0;j<4;++j)QCOMPARE(page.findChild<QCheckBox*>(QStringLiteral("psdAutoRange%1").arg(j))->isChecked(),j==i);
            box->setChecked(false);QCOMPARE(page.grab().toImage(),original);
        }
        for(int i=0;i<4;++i)page.findChild<QCheckBox*>(QStringLiteral("psdAutoRange%1").arg(i))->setChecked(true);
        QVERIFY(page.grab().save("psd_page_adaptive.png"));
        page.resize(1000,760);QTest::qWait(100);
        QVERIFY(page.grab().save("psd_page_compact.png"));
        QCOMPARE(snapshots.count(),0); // Display changes must not reconfigure analysis.
    }
};
QTEST_MAIN(PsdTests)
#include "test_psd.moc"
