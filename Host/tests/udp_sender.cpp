#include "core/protocol.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUdpSocket>
#include <QtEndian>
#include <cstdio>
#define NOMINMAX
#include <windows.h>
using namespace mca;
int main(int argc,char **argv){
    QCoreApplication app(argc,argv);const auto args=app.arguments();
    if(args.size()!=8)return 2;
    const quint16 port=args[1].toUShort();const int rate=args[2].toInt(),batch=args[3].toInt();
    const int seconds=args[4].toInt();const bool psd=args[5]=="psd";
    const quint32 version=args[6].toUInt();quint32 sequence=args[7].toUInt();
    if(rate<=0 || batch<1 || batch>64 || seconds<1)return 2;
    QUdpSocket socket;socket.setSocketOption(QAbstractSocket::SendBufferSizeSocketOption,4*1024*1024);
    const quint64 target=quint64(rate)*seconds;quint64 sent=0,packets=0,errors=0;
    QElapsedTimer clock;clock.start();
    while(sent<target){
        const qint64 deadline=qint64((static_cast<long double>(sent)*1.0e9L)/rate);
        while(clock.nsecsElapsed()<deadline){if(deadline-clock.nsecsElapsed()>2000000)Sleep(1);else YieldProcessor();}
        const int count=int(qMin<quint64>(batch,target-sent));const int record=psd?24:12;
        QByteArray bytes(20+count*record,0);auto p=reinterpret_cast<uchar*>(bytes.data());
        qToBigEndian<quint32>(DataMagic,p);p[4]=2;p[5]=psd?7:1;
        qToBigEndian<quint32>(sequence,p+8);qToBigEndian<quint32>(version,p+12);
        qToBigEndian<quint16>(count,p+16);qToBigEndian<quint16>(count*record,p+18);
        for(int i=0;i<count;++i){auto r=p+20+i*record;quint64 timestamp=sent+i;
            for(int j=0;j<6;++j)r[j]=uchar(timestamp>>(8*(5-j)));
            qToBigEndian<quint16>(16000,r+6);r[8]=1;r[9]=0x38;r[10]=0x80;
            if(psd){qToBigEndian<qint32>(100000,r+12);qToBigEndian<qint32>(20000,r+16);qToBigEndian<quint16>(8192,r+20);}}
        if(socket.writeDatagram(bytes,QHostAddress::LocalHost,port)!=bytes.size())++errors;
        sent+=count;++packets;++sequence;
    }
    const double duration=clock.nsecsElapsed()/1.0e9;
    QJsonObject out{{"sent_events",double(sent)},{"sent_packets",double(packets)},{"send_errors",double(errors)},
        {"duration_seconds",duration},{"actual_cps",sent/duration},{"last_sequence",double(sequence-1)}};
    const auto json=QJsonDocument(out).toJson(QJsonDocument::Compact);std::puts(json.constData());return errors?1:0;
}
