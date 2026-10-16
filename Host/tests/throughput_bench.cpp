#include "app/acquisition.h"
#include "ui/spectrumview.h"
#include "ui/mainwindow.h"
#include <QComboBox>
#include "ui/psdpage.h"
#include <QApplication>
#include <QFontDatabase>
#include <QStyleFactory>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QProcess>
#include <QTimer>
#include <QUdpSocket>
#include <QVBoxLayout>
#include <QtEndian>
#include <algorithm>
#include <numeric>
#include <cstdio>
#define NOMINMAX
#include <windows.h>
using namespace mca;
namespace mca {
class ThroughputBenchmark {
public:
    static AcquisitionController &controller(MainWindow &w){return w.m_acq;}
    static PsdPage &psd(MainWindow &w){return *w.m_psdPage;}
    static void mode(MainWindow &w,AcquisitionMode mode){w.m_acquisitionMode->setCurrentIndex(w.m_acquisitionMode->findData(int(mode)));}
};
}
static double cpuSeconds(){FILETIME c,e,k,u;GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u);
    auto val=[](FILETIME t){return (quint64(t.dwHighDateTime)<<32)|t.dwLowDateTime;};return (val(k)+val(u))*1e-7;}
int main(int argc,char **argv){
    QApplication app(argc,argv);const auto args=app.arguments();if(args.size()!=6)return 2;
    const QString mode=args[1],output=args[5];const bool psd=mode=="psd";
    const int rate=args[2].toInt(),batch=args[3].toInt(),seconds=args[4].toInt();
    QUdpSocket control,probe;if(!control.bind(QHostAddress::LocalHost,0) || !probe.bind(QHostAddress::LocalHost,0))return 2;
    const quint16 dataPort=probe.localPort();probe.close();quint32 sequence=0,version=10;
    QCoreApplication::setOrganizationName("MCA-Throughput-Test");
    QCoreApplication::setApplicationName("IsolatedBenchmark");
    app.setStyle(QStyleFactory::create("Fusion"));

    // Compact professional light palette. Semantic run/pause/error colors
    // are applied by the stylesheet instead of becoming global accents.
    QPalette p;
    p.setColor(QPalette::Window, "#edf0f3");
    p.setColor(QPalette::WindowText, "#20252b");
    p.setColor(QPalette::Base, "#ffffff");
    p.setColor(QPalette::AlternateBase, "#f7f9fb");
    p.setColor(QPalette::Text, "#20252b");
    p.setColor(QPalette::Button, "#f7f8fa");
    p.setColor(QPalette::ButtonText, "#20252b");
    p.setColor(QPalette::Highlight, "#2563a6");
    p.setColor(QPalette::HighlightedText, "#ffffff");
    p.setColor(QPalette::ToolTipBase, "#20252b");
    p.setColor(QPalette::ToolTipText, "#e9edf2");
    app.setPalette(p);

    // Load global stylesheet from embedded resources
    QFile qss(":/mca.qss");
    if (qss.open(QIODevice::ReadOnly | QIODevice::Text)) {
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));
        qss.close();
    }

    QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    QApplication::setFont(QFont("Microsoft YaHei",9));
    MainWindow window;auto &controller=ThroughputBenchmark::controller(window);
    auto &psdPage=ThroughputBenchmark::psd(window);
    ThroughputBenchmark::mode(window,psd?AcquisitionMode::Psd:AcquisitionMode::AutomaticTrigger);
    window.resize(1500,1000);window.show();
    QProcess sender;QElapsedTimer elapsed,stopClock;QVector<double> gaps;double last=-1,cpuStart=0,stopLatency=-1;
    bool launched=false,finished=false,stopIssued=false;QStringList errors;
    AcquisitionSnapshot finalState;std::shared_ptr<const SpectrumSnapshot> finalSpectrum;
    std::shared_ptr<const PsdSnapshot> finalPsd;QJsonObject senderResult;
    auto frame=[&]{if(!elapsed.isValid() || finished)return;const double now=elapsed.nsecsElapsed()/1e6;
        if(last>=0)gaps.push_back(now-last);last=now;};
    QObject::connect(psdPage.session(),&PsdSession::snapshotReady,&app,[&](auto s){finalPsd=s;if(psd && s->received)frame();});
    QObject::connect(&controller,&AcquisitionController::spectrumReady,&app,[&](auto s){finalSpectrum=s;
        if(!psd && s->totalEvents)frame();});
    QObject::connect(&controller,&AcquisitionController::errorOccurred,&app,[&](const QString &e){errors.push_back(e);});
    QObject::connect(psdPage.session(),&PsdSession::errorOccurred,&app,[&](const QString &e){errors.push_back(e);});
    QObject::connect(&control,&QUdpSocket::readyRead,&app,[&]{while(control.hasPendingDatagrams()){
        auto datagram=control.receiveDatagram();const auto bytes=datagram.data();if(bytes.size()!=20)continue;
        const auto p=reinterpret_cast<const uchar*>(bytes.constData());const quint8 op=p[5];const quint16 reg=qFromBigEndian<quint16>(p+8);
        quint32 value=qFromBigEndian<quint32>(p+16);
        if(op==quint8(ControlOpcode::Read)){switch(reg){
            case 0x100:value=1;break;case 0x101:case 0x102:case 0x103:value=1;break;
            case 0x104:case 0x105:case 0x106:value=0;break;case 0x107:value=125000000;break;
            case 0x108:value=0x00020700;break;case 0x109:value=version;break;
            case 0x10a:value=0x7ff;break;case 0x10b:value=14;break;case 0x10c:value=2;break;
            case 0x10d:value=24;break;case 0x10e:value=48;break;case 0x10f:value=20000;break;
            case 0x110:value=8000;break;case 0x111:value=10000;break;default:value=0;}}
        if(op==quint8(ControlOpcode::CommitAll))value=++version;
        QByteArray response(36,0);auto r=reinterpret_cast<uchar*>(response.data());
        qToBigEndian<quint32>(DataMagic,r);r[4]=2;r[5]=5;qToBigEndian<quint32>(sequence++,r+8);
        qToBigEndian<quint32>(version,r+12);qToBigEndian<quint16>(1,r+16);qToBigEndian<quint16>(16,r+18);
        qToBigEndian<quint32>(qFromBigEndian<quint32>(p+12),r+20);r[24]=op;r[25]=p[6];r[26]=p[7];
        qToBigEndian<quint16>(reg,r+28);qToBigEndian<quint32>(value,r+32);
        control.writeDatagram(response,datagram.senderAddress(),datagram.senderPort());
    }});
    QObject::connect(&controller,&AcquisitionController::statusReady,&app,[&](const AcquisitionSnapshot &s){finalState=s;
        if(s.state==AcquisitionState::Running && !launched){launched=true;cpuStart=cpuSeconds();elapsed.start();
            sender.start(QCoreApplication::applicationDirPath()+"/mca_udp_sender.exe",
                {QString::number(dataPort),QString::number(rate),QString::number(batch),QString::number(seconds),mode,QString::number(version),QString::number(sequence)});}
        if(stopIssued && s.state==AcquisitionState::Idle && stopLatency<0){stopLatency=stopClock.nsecsElapsed()/1e6;
            QTimer::singleShot(200,&app,&QCoreApplication::quit);}
    });
    QObject::connect(&sender,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),&app,[&](int code,QProcess::ExitStatus){
        finished=true;senderResult=QJsonDocument::fromJson(sender.readAllStandardOutput().trimmed()).object();
        if(code || senderResult.isEmpty())errors.push_back("sender failed");sequence=quint32(senderResult["last_sequence"].toDouble())+1;
        QTimer::singleShot(500,&app,[&]{stopIssued=true;stopClock.start();controller.stop();if(psd)psdPage.session()->end();});
    });
    controller.setAcquisitionMode(psd?AcquisitionMode::Psd:AcquisitionMode::AutomaticTrigger);
    if(psd)psdPage.begin({},false);
    controller.startUdp(dataPort,QHostAddress::LocalHost,control.localPort(),{},QString{},false,32,false,1,false);
    QTimer::singleShot((seconds+15)*1000,&app,[&]{errors.push_back("timeout");app.quit();});app.exec();
    std::sort(gaps.begin(),gaps.end());const double mean=gaps.isEmpty()?0:std::accumulate(gaps.begin(),gaps.end(),0.0)/gaps.size();
    const double p95=gaps.isEmpty()?0:gaps[qMin(gaps.size()-1,qsizetype(gaps.size()*.95))];
    const quint64 expected=quint64(rate)*seconds;bool counts=finalState.totalEvents==expected;
    if(psd)counts=counts && finalPsd && finalPsd->received==expected && finalPsd->roiEvents==qMin<quint64>(expected,1000000);
    else counts=counts && finalSpectrum && finalSpectrum->peak.size()==1 && finalSpectrum->peak[0][4000]==expected;
    const bool pass=counts && errors.isEmpty() && finalState.packets.lost==0 && finalState.packets.parseErrors==0 &&
        senderResult["send_errors"].toDouble()==0 && senderResult["actual_cps"].toDouble()>=rate*.99 &&
        mean>0 && 1000/mean>=(psd?9:18) && p95<=(psd?200:100) && stopLatency>=0 && stopLatency<=500;
    QJsonObject out{{"mode",mode},{"target_cps",rate},{"batch",batch},{"seconds",seconds},{"pass",pass},
        {"received_events",double(finalState.totalEvents)},{"lost_packets",double(finalState.packets.lost)},
        {"pending_missing",double(finalState.packets.pendingMissing)},{"receive_buffer_bytes",double(finalState.packets.receiveBufferBytes)},{"parse_errors",double(finalState.packets.parseErrors)},
        {"counts_match",counts},{"refresh_hz",mean>0?1000/mean:0},{"refresh_p95_ms",p95},{"stop_latency_ms",stopLatency},
        {"cpu_one_core_percent",elapsed.isValid()?(cpuSeconds()-cpuStart)/qMax(1.0,elapsed.elapsed()/1000.0)*100:0},
        {"analysis_queue_high_water",double(finalPsd?finalPsd->analysisQueueHighWater:0)},
        {"max_receive_turn_ms",finalState.packets.maximumReceiveTurnNanoseconds/1e6},{"sender",senderResult},
        {"errors",errors.join("; ")},{"display_platform",QGuiApplication::platformName()}};
    QFile file(output);if(file.open(QIODevice::WriteOnly))file.write(QJsonDocument(out).toJson());
    std::puts(QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
    window.grab().save(output+".png");
    return pass?0:1;
}
