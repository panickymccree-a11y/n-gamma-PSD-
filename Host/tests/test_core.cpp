#include "analysis/analysis.h"
#include "app/acquisition.h"
#include "core/listfile.h"
#include "core/eventtextwriter.h"
#include "core/protocol.h"
#include "core/serialprotocol.h"
#include "core/spectrum.h"
#include "core/sequencewindow.h"
#include "core/spectrumfile.h"
#include "core/waveform.h"
#include <QDataStream>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QNetworkDatagram>
#include <QUdpSocket>
#include <QtEndian>
#include <QtTest>
#include <algorithm>
#include <cmath>

using namespace mca;

namespace {
ModuleEvent sampleModule(quint8 module, quint32 id, quint64 timestamp) {
    ModuleEvent event;
    event.eventId = id;
    event.timestamp = timestamp;
    event.module = module;
    event.flags = 0x12;
    event.relativeTime = {-2, -1, 1, 2};
    event.integral = {0x010203, 0x040506, 0x070809, 0x0a0b0c};
    event.peak = {0x1111, 0x2222, 0x3333, 0x4444};
    event.channelFlags = {
        quint8((module * 4 + 0) << 3),
        quint8((module * 4 + 1) << 3),
        quint8((module * 4 + 2) << 3),
        quint8((module * 4 + 3) << 3)
    };
    event.x = 321;
    event.y = 1789;
    event.energy = 0x10203040;
    return event;
}

QByteArray controlResponsePacket(const ControlResponse &response,
                                 quint32 sequence = 7) {
    QByteArray bytes(PacketHeaderBytes + ControlResponseBytes, 0);
    auto *p = reinterpret_cast<uchar *>(bytes.data());
    qToBigEndian<quint32>(DataMagic, p);
    p[4] = 2;
    p[5] = quint8(PacketType::ControlResponse);
    qToBigEndian<quint32>(sequence, p + 8);
    qToBigEndian<quint32>(9, p + 12);
    qToBigEndian<quint16>(1, p + 16);
    qToBigEndian<quint16>(ControlResponseBytes, p + 18);
    p += PacketHeaderBytes;
    qToBigEndian<quint32>(response.transactionId, p);
    p[4] = response.opcode;
    p[5] = response.target;
    p[6] = response.targetId;
    p[7] = response.status;
    qToBigEndian<quint16>(response.registerAddress, p + 8);
    qToBigEndian<quint32>(response.value, p + 12);
    return bytes;
}

QByteArray baselineStatusPacket(const std::array<quint16, 32> &baselines) {
    QByteArray bytes(PacketHeaderBytes + 140, 0);
    auto *header = reinterpret_cast<uchar *>(bytes.data());
    qToBigEndian<quint32>(DataMagic, header);
    header[4] = 2;
    header[5] = quint8(PacketType::Status);
    qToBigEndian<quint16>(8, header + 16);
    qToBigEndian<quint16>(140, header + 18);

    auto *payload = header + PacketHeaderBytes;
    payload[0] = 1; payload[1] = 8;
    payload[2] = 2; payload[3] = 4;
    payload[4] = 32; payload[5] = 8;
    payload[6] = 1; payload[7] = 0;
    payload[8] = 1; payload[9] = 1;
    payload[10] = 2; payload[11] = 20;
    payload[32] = 3; payload[33] = 28;
    payload[62] = 4; payload[63] = 20;
    for (int group = 0; group < 4; ++group) {
        const int offset = 84 + group * 14;
        payload[offset] = uchar(5 + group);
        payload[offset + 1] = 12;
        for (int pair = 0; pair < 4; ++pair) {
            const quint16 first = baselines[group * 8 + pair * 2] & 0x0fff;
            const quint16 second = baselines[group * 8 + pair * 2 + 1] & 0x0fff;
            payload[offset + 2 + pair * 3] = uchar(first >> 4);
            payload[offset + 3 + pair * 3] =
                uchar(((first & 0x000f) << 4) | (second >> 8));
            payload[offset + 4 + pair * 3] = uchar(second);
        }
    }
    return bytes;
}

struct CapturedControl {
    ControlOpcode opcode = ControlOpcode::Read;
    ControlTarget target = ControlTarget::Global;
    quint8 targetId = 0;
    quint16 registerAddress = 0;
    quint32 transaction = 0;
    quint32 value = 0;
};

CapturedControl captureControl(const QByteArray &bytes) {
    CapturedControl result;
    if (bytes.size() != ControlFrameBytes)
        return result;
    const auto *p = reinterpret_cast<const uchar *>(bytes.constData());
    result.opcode = ControlOpcode(p[5]);
    result.target = ControlTarget(p[6]);
    result.targetId = p[7];
    result.registerAddress = qFromBigEndian<quint16>(p + 8);
    result.transaction = qFromBigEndian<quint32>(p + 12);
    result.value = qFromBigEndian<quint32>(p + 16);
    return result;
}

// One complete, well-formed waveform fragment with deterministic contents.
WaveformFragment waveformFragment(int index, quint32 captureId = 7,
                                  quint32 sessionId = 0x2222'3333,
                                  quint16 baseline = 8200,
                                  quint16 threshold = 120,
                                  quint16 flags = 0) {
    WaveformFragment fragment;
    fragment.sessionId = sessionId;
    fragment.captureId = captureId;
    fragment.triggerTimestamp = 0x0000'1234'5678ULL;
    fragment.baseline = baseline;
    fragment.threshold = threshold;
    fragment.returnThreshold = 32;
    fragment.preSamples = WaveformPreSamples;
    fragment.totalSamples = WaveformTotalSamples;
    fragment.sampleOffset = quint16(index * WaveformFragmentMax);
    fragment.fragmentIndex = quint16(index);
    fragment.fragmentCount = WaveformFragmentCount;
    fragment.flags = flags;
    fragment.samplePeriodNs = WaveformSamplePeriodNs;
    const int count = index == WaveformFragmentCount - 1
                          ? WaveformLastSamples : WaveformFragmentMax;
    fragment.sampleCount = quint16(count);
    fragment.samples.resize(count);
    for (int i = 0; i < count; ++i) {
        // Mark the trigger sample so tests can find it by content.
        const int absolute = index * WaveformFragmentMax + i;
        quint16 value = quint16((absolute * 7) & 0x3fff);
        if (absolute == WaveformPreSamples)
            value = 0x1234;
        if ((absolute % 997) == 0)
            value |= 0x4000;             // overrange marker
        fragment.samples[i] = value;
    }
    return fragment;
}
} // namespace

class CoreTests : public QObject {
    Q_OBJECT
private slots:
    void socketBufferMustBeSetAfterBind() {
        QUdpSocket socket;
        socket.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption,8*1024*1024);
        QVERIFY(socket.bind(QHostAddress::LocalHost,0));
        const auto before=socket.socketOption(QAbstractSocket::ReceiveBufferSizeSocketOption).toUInt();
        socket.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption,8*1024*1024);
        const auto after=socket.socketOption(QAbstractSocket::ReceiveBufferSizeSocketOption).toUInt();
        qInfo()<<"receive buffer: before-bind request actual"<<before<<"after-bind actual"<<after;
        QVERIFY(after>=8*1024*1024);
        socket.close();QVERIFY(socket.bind(QHostAddress::LocalHost,0));
        socket.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption,8*1024*1024);
        QVERIFY(socket.socketOption(QAbstractSocket::ReceiveBufferSizeSocketOption).toUInt()>=8*1024*1024);
    }
    void sequenceRepairWindow() {
        SequenceWindow w;PacketStats stats;
        QVERIFY(w.accept(100,8,stats));QVERIFY(w.accept(103,8,stats));
        QCOMPARE(stats.pendingMissing,2uLL);QCOMPARE(stats.lost,0uLL);
        QVERIFY(w.accept(101,8,stats));QCOMPARE(stats.reordered,1uLL);
        QVERIFY(!w.accept(101,8,stats));QCOMPARE(stats.duplicateEvents,8uLL);
        for(quint32 i=104;i<=1126;++i)QVERIFY(w.accept(i,8,stats));
        QCOMPARE(stats.lost,1uLL);QCOMPARE(stats.pendingMissing,0uLL);
        QVERIFY(!w.accept(102,8,stats));QCOMPARE(stats.latePackets,1uLL);
        w.clear();stats={};
        QVERIFY(w.accept(0xfffffffeu,1,stats));QVERIFY(w.accept(0,1,stats));
        QVERIFY(w.accept(0xffffffffu,1,stats));QVERIFY(w.accept(1,1,stats));
        QCOMPARE(stats.lost,0uLL);QCOMPARE(stats.pendingMissing,0uLL);
        QVERIFY(!w.accept(0,1,stats));
        w.clear();stats={};QVERIFY(w.accept(0,1,stats));
        QVERIFY(w.accept(5000,1,stats));QCOMPARE(stats.lost,3976uLL);
        QCOMPARE(stats.pendingMissing,1023uLL);
        QVERIFY(w.accept(5001,1,stats));QCOMPARE(stats.lost,3977uLL);
        w.clear();stats={};QVERIFY(w.accept(0,1,stats));
        QVERIFY(w.accept(1024,1,stats));QCOMPARE(stats.lost,0uLL);
        QCOMPARE(stats.pendingMissing,1023uLL);
    }
    void typedNotificationsAreOptInAndDrain() {
        AcquisitionController controller;
        QSignalSpy notifications(&controller,&AcquisitionController::typedEventsReady);
        AcquisitionPreset p;controller.startSimulation(p);
        QTest::qWait(150);controller.stop();QTest::qWait(100);
        QCOMPARE(notifications.count(),0);
        controller.setTypedEventNotificationsEnabled(true);
        controller.startSimulation(p);QTest::qWait(180);controller.stop();QTest::qWait(100);
        QVERIFY(!notifications.isEmpty());
        qsizetype count=0;for(const auto &call:notifications)count+=qvariant_cast<QVector<TypedEvent>>(call[0]).size();
        QVERIFY(count>0);
    }

    void panelNamesMatchFrontPanel() {
        QCOMPARE(panelChannelName(0), QStringLiteral("ADC1-A"));
        QCOMPARE(panelChannelName(7), QStringLiteral("ADC1-H"));
        QCOMPARE(panelChannelName(8), QStringLiteral("ADC2-A"));
        QCOMPARE(panelChannelName(31), QStringLiteral("ADC4-H"));
        QCOMPARE(panelModuleName(0), QStringLiteral("M0 (ADC1 A-D)"));
        QCOMPARE(panelModuleName(7), QStringLiteral("M7 (ADC4 E-H)"));
    }

    void channelPacketRoundTrip() {
        QVector<ChannelEvent> input{
            {0x010203040506, 0x1234, 0xabcdef, 7, true, true, false},
            {42, 0xffff, 0xffffff, 31, false, false, true}
        };
        const auto bytes = PacketDecoder::encodeChannels(123, 9, 2, input, 3);
        QCOMPARE(bytes.size(), PacketHeaderBytes + 2 * ChannelEventBytes);
        QCOMPARE(bytes.left(20).toHex(),
                 QByteArray("4d434132020102030000007b0000000900020018"));
        DecodedPacket packet;
        QString error;
        QVERIFY2(PacketDecoder::decode(bytes, packet, &error),
                 qPrintable(error));
        QCOMPARE(packet.type, PacketType::ChannelEvents);
        QCOMPARE(packet.sequence, quint32(123));
        QCOMPARE(packet.configVersion, quint32(9));
        QCOMPARE(packet.channelEvents.size(), 2);
        QCOMPARE(packet.channelEvents[0].timestamp, input[0].timestamp);
        QCOMPARE(packet.channelEvents[0].integral, input[0].integral);
        QVERIFY(packet.channelEvents[0].pileup);
        QVERIFY(packet.channelEvents[0].overflow);
        QVERIFY(packet.channelEvents[1].polarity);
    }

    void modulePacketRoundTrip() {
        const QVector<ModuleEvent> input{sampleModule(1, 77, 0xfffffffffffe)};
        const auto bytes = PacketDecoder::encodeModules(5, 3, 0, input);
        QCOMPARE(bytes.size(), PacketHeaderBytes + ModuleEventBytes);
        DecodedPacket packet;
        QString error;
        QVERIFY2(PacketDecoder::decode(bytes, packet, &error),
                 qPrintable(error));
        QCOMPARE(packet.moduleEvents.size(), 1);
        const auto &event = packet.moduleEvents[0];
        QCOMPARE(event.eventId, quint32(77));
        QCOMPARE(event.timestamp, quint64(0xfffffffffffe));
        QCOMPARE(event.module, quint8(1));
        QCOMPARE(event.relativeTime[0], qint16(-2));
        QCOMPARE(event.integral[3], quint32(0x0a0b0c));
        QCOMPARE(event.peak[2], quint16(0x3333));
        QCOMPARE(event.x, quint16(321));
        QCOMPARE(event.y, quint16(1789));
        QCOMPARE(event.energy, quint32(0x10203040));
    }

    void coincidencePacketRoundTrip() {
        CoincidenceEvent event;
        event.matchId = 0x11223344;
        event.pair = 0;
        event.flags = 1;
        event.layerTime = -8;
        event.scatter = sampleModule(0, 10, 100);
        event.absorber = sampleModule(1, 11, 92);
        const auto bytes = PacketDecoder::encodeCoincidences(
            1, 4, 0, QVector<CoincidenceEvent>{event});
        QCOMPARE(bytes.size(), PacketHeaderBytes + CoincidenceEventBytes);
        DecodedPacket packet;
        QString error;
        QVERIFY2(PacketDecoder::decode(bytes, packet, &error),
                 qPrintable(error));
        QCOMPARE(packet.coincidenceEvents.size(), 1);
        const auto &decoded = packet.coincidenceEvents[0];
        QCOMPARE(decoded.matchId, event.matchId);
        QCOMPARE(decoded.pair, quint8(0));
        QCOMPARE(decoded.layerTime, qint16(-8));
        QVERIFY(decoded.ambiguous());
        QCOMPARE(decoded.scatter.module, quint8(0));
        QCOMPARE(decoded.absorber.module, quint8(1));
    }

    void rejectsMalformedPackets() {
        DecodedPacket packet;
        QString error;
        QVERIFY(!PacketDecoder::decode(QByteArray(4, 'x'), packet, &error));
        QVERIFY(!error.isEmpty());
        auto bytes = PacketDecoder::encodeChannels(
            1, 0, 0, QVector<ChannelEvent>{{1, 2, 3, 4}});
        bytes.append('x');
        QVERIFY(!PacketDecoder::decode(bytes, packet, &error));
        bytes = PacketDecoder::encodeChannels(
            1, 0, 0, QVector<ChannelEvent>{{1, 2, 3, 4}});
        bytes[18] = 0x7f;
        QVERIFY(!PacketDecoder::decode(bytes, packet, &error));
    }

    void controlFramesAreByteExact() {
        QCOMPARE(makeShadowWrite(ControlTarget::Channel, 3, 1,
                                 0x12345678, 0x11223344).toHex(),
                 QByteArray("4d43463202010203000100001122334412345678"));
        QCOMPARE(makeRead(ControlTarget::Global, 0, 0x0102,
                          1).toHex(),
                 QByteArray("4d43463202020000010200000000000100000000"));
        QCOMPARE(makeCommitAll(2).toHex(),
                 QByteArray("4d43463202030000000000000000000200000000"));
        QCOMPARE(makeStart(9, 3).toHex(),
                 QByteArray("4d43463202040000000000000000000300000009"));
        QCOMPARE(makeStop(4).toHex(),
                 QByteArray("4d43463202050000000000000000000400000000"));
        QCOMPARE(makeAbort(5).toHex(),
                 QByteArray("4d43463202060000000000000000000500000000"));
    }

    void controlResponseDecode() {
        ControlResponse input;
        input.transactionId = 0x12345678;
        input.opcode = quint8(ControlOpcode::CommitAll);
        input.target = quint8(ControlTarget::Global);
        input.status = quint8(ControlStatus::Ok);
        input.registerAddress = 0x0109;
        input.value = 17;
        DecodedPacket packet;
        QString error;
        QVERIFY2(PacketDecoder::decode(controlResponsePacket(input),
                                       packet, &error), qPrintable(error));
        QCOMPARE(packet.responses.size(), 1);
        QCOMPARE(packet.responses[0].transactionId, input.transactionId);
        QCOMPARE(packet.responses[0].registerAddress, quint16(0x0109));
        QCOMPARE(packet.responses[0].value, quint32(17));
        QVERIFY(packet.responses[0].accepted());
    }

    void statusTlvDecode() {
        QByteArray bytes(PacketHeaderBytes + 96, 0);
        auto *p = reinterpret_cast<uchar *>(bytes.data());
        qToBigEndian<quint32>(DataMagic, p);
        p[4] = 2;
        p[5] = quint8(PacketType::Status);
        qToBigEndian<quint16>(4, p + 16);
        qToBigEndian<quint16>(96, p + 18);
        p += PacketHeaderBytes;
        p[0] = 1; p[1] = 8; p[2] = 2; p[3] = 4;
        p[4] = 32; p[5] = 8; p[6] = 4; p[7] = 2;
        p[8] = 1; p[9] = 0x0f;
        p[10] = 2; p[11] = 20;
        qToBigEndian<quint32>(7, p + 12);
        p[21] = 42;
        qToBigEndian<quint32>(0xffff'ffffu, p + 22);
        qToBigEndian<quint32>(0x0000'ffffu, p + 26);
        qToBigEndian<quint16>(9, p + 30);
        p[32] = 3; p[33] = 28;
        qToBigEndian<quint32>(100, p + 34);
        qToBigEndian<quint32>(5, p + 38);
        qToBigEndian<quint32>(2, p + 50);
        p[62] = 4; p[63] = 20;
        qToBigEndian<quint32>(11, p + 64);
        qToBigEndian<quint32>(12, p + 68);
        qToBigEndian<quint32>(13, p + 72);
        qToBigEndian<quint32>(14, p + 76);
        qToBigEndian<quint32>(15, p + 80);
        DecodedPacket packet;
        QString error;
        QVERIFY2(PacketDecoder::decode(bytes, packet, &error),
                 qPrintable(error));
        QVERIFY(packet.hasStatus);
        QVERIFY(packet.status.capabilitiesValid);
        QCOMPARE(packet.status.capabilities.channelCount, quint8(32));
        QCOMPARE(packet.status.mode, AcquisitionMode::Coincidence);
        QVERIFY(packet.status.running);
        QVERIFY(packet.status.adcAligned);
        QCOMPARE(packet.status.adcAlignmentMask, quint8(0x0f));
        QCOMPARE(packet.status.configVersion, quint32(7));
        QCOMPARE(packet.status.timestamp, quint64(42));
        QCOMPARE(packet.status.baselineReadyMask, quint32(0xffff'ffffu));
        QCOMPARE(packet.status.baselineTrackingMask, quint32(0x0000'ffffu));
        QCOMPARE(packet.status.eventFifoCount, quint16(9));
        QCOMPARE(packet.status.outputEvents, quint32(100));
        QCOMPARE(packet.status.moduleUnmatched, quint32(5));
        QCOMPARE(packet.status.ambiguous, quint32(2));
        QCOMPARE(packet.status.pairGateRejected, quint32(11));
        QCOMPARE(packet.status.pairCacheOverflow, quint32(12));
        QCOMPARE(packet.status.fifoOverflow, quint32(13));
        QCOMPARE(packet.status.outputBackpressure, quint32(14));
        QCOMPARE(packet.status.commandFifoOverflow, quint32(15));
        QCOMPARE(packet.status.baselineValueCount, quint8(0));
    }

    void statusBaselineTlvDecode() {
        std::array<quint16, 32> expected{};
        for (int channel = 0; channel < 32; ++channel)
            expected[channel] = quint16((channel * 127) & 0x0fff);
        DecodedPacket packet;
        QString error;
        const QByteArray bytes = baselineStatusPacket(expected);
        QVERIFY2(PacketDecoder::decode(bytes, packet, &error),
                 qPrintable(error));
        QVERIFY(packet.hasStatus);
        QVERIFY(packet.status.capabilitiesValid);
        QCOMPARE(packet.status.capabilities.adcCount, quint8(4));
        QCOMPARE(packet.status.capabilities.channelCount, quint8(32));
        QCOMPARE(packet.status.baselineValueCount, quint8(32));
        QCOMPARE(packet.status.baselineValueValidMask, quint32(0xffff'ffffu));
        for (int channel = 0; channel < 32; ++channel)
            QCOMPARE(packet.status.baselineValues[channel],
                     expected[channel]);
    }

    void rejectsMalformedBaselineTlv() {
        const std::array<quint16, 32> values{};
        DecodedPacket packet;
        QString error;

        QByteArray shortTlv = baselineStatusPacket(values);
        shortTlv[PacketHeaderBytes + 85] = char(11);
        QVERIFY(!PacketDecoder::decode(shortTlv, packet, &error));
        QVERIFY(!error.isEmpty());

        QByteArray overflowTlv = baselineStatusPacket(values);
        overflowTlv[PacketHeaderBytes + 85] = char(13);
        error.clear();
        QVERIFY(!PacketDecoder::decode(overflowTlv, packet, &error));
        QVERIFY(!error.isEmpty());

        QByteArray missingGroup = baselineStatusPacket(values);
        // TLV12 header sits at 112 inside the payload; group 3 (channels 24..31)
        // is rewritten as a duplicate full-baseline TLV for channel 0.
        missingGroup[PacketHeaderBytes + 112] = char(12);
        error.clear();
        QVERIFY2(PacketDecoder::decode(missingGroup, packet, &error),
                 qPrintable(error));
        QCOMPARE(packet.status.baselineValueCount, quint8(24));
        QCOMPARE(packet.status.baselineValueValidMask, quint32(0xff00'ffffu));
    }

    void decodesSingleChannelExtendedStatus() {
        // Payload grammar: [type][length][value...] TLVs terminated by a single
        // zero end marker, exactly as rtl/mca_status_snapshot.sv builds it.
        QByteArray bytes(PacketHeaderBytes + 31, 0);
        auto *header = reinterpret_cast<uchar *>(bytes.data());
        qToBigEndian<quint32>(DataMagic, header);
        header[4] = 2;
        header[5] = quint8(PacketType::Status);
        qToBigEndian<quint16>(4, header + 16);
        qToBigEndian<quint16>(31, header + 18);
        auto *p = header + PacketHeaderBytes;
        p[0] = 9; p[1] = 6;
        p[2] = 0x06;                     // ch0=2, ch1=1
        p[8] = 10; p[9] = 3; p[10] = 0;
        p[11] = 0x23; p[12] = 0x45;      // manual 14-bit baseline
        p[13] = 11; p[14] = 9;
        qToBigEndian<quint32>(17, p + 15);
        qToBigEndian<quint32>(19, p + 19);
        p[23] = 1;                       // event active
        p[24] = 12; p[25] = 3; p[26] = 0;
        p[27] = 0x3a; p[28] = 0xbc;      // full 14-bit baseline of channel 0
        p[29] = 0;                       // end marker
        p[30] = 0;                       // padding stays zero after the marker

        DecodedPacket packet;
        QString error;
        QVERIFY2(PacketDecoder::decode(bytes, packet, &error), qPrintable(error));
        QCOMPARE(packet.status.baselineStates[0], quint8(2));
        QCOMPARE(packet.status.baselineStates[1], quint8(1));
        QVERIFY(!packet.status.baselineAutomatic);
        QCOMPARE(packet.status.manualBaseline, quint16(0x2345));
        QCOMPARE(packet.status.deadTimeRejected, quint32(17));
        QCOMPARE(packet.status.droppedEvents, quint32(19));
        QVERIFY(packet.status.eventActive);
        QCOMPARE(packet.status.baselineValues[0], quint16(0x3abc));

        // Drop the end marker: the payload then ends inside a TLV and must be
        // rejected instead of being treated as a complete status image.
        bytes[PacketHeaderBytes + 29] = char(5);
        bytes[PacketHeaderBytes + 30] = char(1);
        QVERIFY(!PacketDecoder::decode(bytes, packet, &error));
    }

    void normalizesSingleChannelRanges() {
        ChannelConfig config;
        config.thresholdLsb = 0xffff;
        config.integrationPostSamples = 3000;
        config.integrationPreSamples = 2999;
        config = normalizeChannelConfig(config);
        QCOMPARE(config.thresholdLsb, quint16(16383));
        QCOMPARE(config.integrationPostSamples, quint16(2500));
        QCOMPARE(config.integrationPreSamples, quint16(2499));

        config.integrationPostSamples = 1;
        config.integrationPreSamples = 6;
        config = normalizeChannelConfig(config);
        QCOMPARE(config.integrationPostSamples, quint16(6));
        QCOMPARE(config.integrationPreSamples, quint16(5));
    }

    void q14Point2PeakUsesFourteenBitBins() {
        SpectrumAccumulator accumulator(1, 16384, 2, 125'000'000);
        ChannelEvent event;
        event.channel = 0;
        event.peak = 0xfffc;
        QCOMPARE(accumulator.add({event}), quint64(1));
        const auto snapshot = accumulator.snapshot();
        QCOMPARE(snapshot->peak[0].size(), 16384);
        QCOMPARE(snapshot->peak[0][16383], quint64(1));
        QCOMPARE(snapshot->sampleRateHz, quint32(125'000'000));
        QCOMPARE(snapshot->adcBits, quint8(14));
        QCOMPARE(snapshot->peakFractionBits, quint8(2));
    }

    void spectrumAndModuleObservables() {
        SpectrumAccumulator accumulator(8);
        QVector<ChannelEvent> channels{
            {1, quint16(100 << 4), quint32(200 << 10), 0},
            {2, quint16(100 << 4), quint32(200 << 10), 0},
            {3, quint16(4095 << 4), 0xffffff, 7}
        };
        QCOMPARE(accumulator.add(channels), quint64(3));
        const QVector<ModuleEvent> modules{sampleModule(1, 1, 20)};
        accumulator.addModuleObservables(modules);
        CoincidenceEvent coincidence;
        coincidence.scatter = sampleModule(0, 2, 30);
        coincidence.absorber = sampleModule(1, 3, 35);
        coincidence.layerTime = 5;
        accumulator.addCoincidenceObservables({coincidence});
        const auto snapshot = accumulator.snapshot();
        QCOMPARE(snapshot->totalEvents, quint64(3));
        QCOMPARE(snapshot->peak[0][100], quint64(2));
        QCOMPARE(snapshot->modulePositions.size(), 1);
        QCOMPARE(snapshot->coincidenceEnergy.size(), 1);
        QCOMPARE(snapshot->coincidenceTime[0], qint16(5));
    }

    void listV3RoundTrip() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        DeviceCapabilities capabilities;
        QVector<ChannelConfig> channels(8);
        QVector<ModuleConfig> modules(2);
        QVector<PairConfig> pairs(1);
        pairs[0].enabled = true;
        const QString path = directory.filePath("typed.mcalist");
        ListFileWriter writer;
        QString error;
        QVERIFY2(writer.open(path, capabilities, AcquisitionMode::Coincidence,
                             channels, modules, pairs, 12, &error),
                 qPrintable(error));
        const auto channelPacket = PacketDecoder::encodeChannels(
            1, 12, 0, QVector<ChannelEvent>{{1, 2, 3, 4}});
        CoincidenceEvent coincidence;
        coincidence.scatter = sampleModule(0, 1, 10);
        coincidence.absorber = sampleModule(1, 2, 12);
        const auto coincidencePacket = PacketDecoder::encodeCoincidences(
            2, 12, 0, QVector<CoincidenceEvent>{coincidence});
        QVERIFY(writer.appendPacket(channelPacket, &error));
        QVERIFY(writer.appendPacket(coincidencePacket, &error));
        PacketStats stats;
        QVERIFY(writer.close(true, stats, &error));

        QVector<DecodedPacket> packets;
        QVERIFY2(ListFileReader::readPackets(path, packets, &error),
                 qPrintable(error));
        QCOMPARE(packets.size(), 2);
        QCOMPARE(packets[0].channelEvents.size(), 1);
        QCOMPARE(packets[1].coincidenceEvents.size(), 1);
        QVector<EventRecord> replay;
        QVERIFY2(ListFileReader::readAll(path, replay, &error),
                 qPrintable(error));
        QCOMPARE(replay.size(), 9);
    }

    void listRejectsLegacyVersions() {
        QTemporaryDir directory;
        const QString path = directory.filePath("legacy.mcalist");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QDataStream stream(&file);
        stream.setByteOrder(QDataStream::BigEndian);
        stream << quint32(0x4d43414c) << quint16(2) << quint16(0)
               << quint32(2);
        stream.writeRawData("{}", 2);
        file.close();
        QVector<DecodedPacket> packets;
        QString error;
        QVERIFY(!ListFileReader::readPackets(path, packets, &error));
        QVERIFY(error.contains(QStringLiteral("仅支持 MCAL v3")));
    }

    void roiAndCalibration() {
        QVector<quint64> spectrum(400);
        for (int x = 0; x < spectrum.size(); ++x)
            spectrum[x] = quint64(10 + 1000 * std::exp(
                -0.5 * std::pow((x - 200) / 8.0, 2)));
        const auto roi = AnalysisService::roiStats(spectrum, 160, 240);
        QVERIFY(std::abs(roi.centroid - 200) < 0.5);
        const auto fit = AnalysisService::fitGaussian(
            spectrum, 160, 240, 1, true);
        QVERIFY2(fit.valid, qPrintable(fit.message));
        bool ok = false;
        const auto calibration = AnalysisService::calibrate(
            {{0, 10}, {100, 210}}, Calibration::Order::Linear, &ok);
        QVERIFY(ok);
        QVERIFY(std::abs(calibration.apply(50) - 110) < 1e-8);
    }

    void narrowGaussianFitUsesIntegratedBinsAndDenseCurve() {
        constexpr int left = 28;
        constexpr int right = 34;
        constexpr double centroid = 31.2;
        constexpr double sigma = 0.38;
        constexpr double area = 5000.0;
        QVector<quint64> spectrum(64, 0);
        const auto binFraction = [](double x, double mean,
                                    double standardDeviation) {
            const double scale =
                standardDeviation * std::sqrt(2.0);
            return 0.5 * (
                std::erf((x + 0.5 - mean) / scale) -
                std::erf((x - 0.5 - mean) / scale));
        };
        for (int x = left; x <= right; ++x) {
            const double background = 18.0 + 0.6 * (x - left);
            spectrum[x] = quint64(std::llround(
                background +
                area * binFraction(x, centroid, sigma)));
        }

        const auto fit = AnalysisService::fitGaussian(
            spectrum, left, right, 1, true);
        QVERIFY2(fit.valid, qPrintable(fit.message));
        QCOMPARE(fit.peaks.size(), 1);
        QVERIFY(std::abs(fit.peaks[0].centroid - centroid) < 0.15);
        QVERIFY(std::abs(fit.peaks[0].sigma - sigma) < 0.15);
        QVERIFY(std::abs(fit.peaks[0].area - area) / area < 0.15);
        QVERIFY(fit.curve.size() >= (right - left) * 16 + 1);
        for (int point = 1; point < fit.curve.size(); ++point) {
            QVERIFY(fit.curve[point].x() >
                    fit.curve[point - 1].x());
            QVERIFY(std::isfinite(fit.curve[point].y()));
        }
    }

    void fivePointConstantBackgroundGaussianFit() {
        constexpr int left = 20;
        constexpr int right = 24;
        constexpr double centroid = 22.1;
        constexpr double sigma = 0.45;
        constexpr double area = 2400.0;
        QVector<quint64> spectrum(48, 0);
        for (int x = left; x <= right; ++x) {
            const double scale = sigma * std::sqrt(2.0);
            const double fraction = 0.5 * (
                std::erf((x + 0.5 - centroid) / scale) -
                std::erf((x - 0.5 - centroid) / scale));
            spectrum[x] =
                quint64(std::llround(7.0 + area * fraction));
        }
        const auto fit = AnalysisService::fitGaussian(
            spectrum, left, right, 1, false);
        QVERIFY2(fit.valid, qPrintable(fit.message));
        QVERIFY(std::abs(fit.peaks[0].centroid - centroid) < 0.2);
        QVERIFY(std::abs(fit.peaks[0].sigma - sigma) < 0.2);
    }

    void gaussianFitRejectsUnidentifiableOrFlatRoi() {
        QVector<quint64> spectrum(32, 12);
        const auto tooNarrow = AnalysisService::fitGaussian(
            spectrum, 10, 13, 1, false);
        QVERIFY(!tooNarrow.valid);
        QVERIFY(tooNarrow.message.contains(QStringLiteral("不足")));

        const auto tooManyPeaks = AnalysisService::fitGaussian(
            spectrum, 10, 17, 2, true);
        QVERIFY(!tooManyPeaks.valid);
        QVERIFY(tooManyPeaks.message.contains(QStringLiteral("不足")));

        const auto flat = AnalysisService::fitGaussian(
            spectrum, 8, 20, 1, true);
        QVERIFY(!flat.valid);
        QVERIFY(flat.message.contains(QStringLiteral("有效峰")));
    }

    void simulationSupportsAllModes() {
        for (const auto mode : {
                 AcquisitionMode::SingleChannel,
                 AcquisitionMode::FourChannel,
                 AcquisitionMode::Coincidence}) {
            AcquisitionController controller;
            controller.setTypedEventNotificationsEnabled(true);
            QSignalSpy events(&controller,
                              &AcquisitionController::typedEventsReady);
            AcquisitionPreset preset;
            preset.useEventCount = true;
            preset.eventCount = 10;
            controller.setAcquisitionMode(mode);
            controller.startSimulation(preset);
            QTRY_VERIFY_WITH_TIMEOUT(events.count() > 0, 2000);
            const auto batch =
                qvariant_cast<QVector<TypedEvent>>(events.first().at(0));
            QVERIFY(!batch.isEmpty());
            if (mode == AcquisitionMode::SingleChannel)
                QVERIFY(std::holds_alternative<ChannelEvent>(batch.first()));
            else if (mode == AcquisitionMode::FourChannel)
                QVERIFY(std::holds_alternative<ModuleEvent>(batch.first()));
            else
                QVERIFY(std::holds_alternative<CoincidenceEvent>(batch.first()));
            controller.stop();
        }
    }

    void simulationSupportsPairMaskTopologies() {
        struct Scenario {
            quint8 scatter;
            quint8 absorber;
        };
        const std::array<Scenario, 4> scenarios{{
            {0x01, 0x02}, // 1:1
            {0x01, 0x1e}, // 1:4
            {0x0f, 0x10}, // 4:1
            {0x0f, 0xf0}  // 4:4
        }};

        for (const auto &scenario : scenarios) {
            AcquisitionController controller;
            controller.setTypedEventNotificationsEnabled(true);
            QSignalSpy events(&controller,
                              &AcquisitionController::typedEventsReady);
            PairConfig pair;
            pair.enabled = true;
            pair.scatterModuleMask = scenario.scatter;
            pair.absorberModuleMask = scenario.absorber;
            controller.setPairConfig(0, pair);
            controller.setAcquisitionMode(AcquisitionMode::Coincidence);
            // Configuration is applied by the acquisition worker thread.
            // Wait for the queued stopped-state update before starting the
            // high-rate simulator, just as the UI does between Apply/Start.
            QTest::qWait(20);

            AcquisitionPreset preset;
            preset.useEventCount = true;
            preset.eventCount = 6;
            controller.startSimulation(preset);
            QTRY_VERIFY_WITH_TIMEOUT(events.count() > 0, 2000);

            bool sawAmbiguous = false;
            const auto batch =
                qvariant_cast<QVector<TypedEvent>>(events.first().at(0));
            QCOMPARE(batch.size(), 6);
            for (const auto &typed : batch) {
                QVERIFY(std::holds_alternative<CoincidenceEvent>(typed));
                const auto coincidence =
                    std::get<CoincidenceEvent>(typed);
                QCOMPARE(coincidence.pair, quint8(0));
                QVERIFY2(scenario.scatter &
                         quint8(1u << coincidence.scatter.module),
                         qPrintable(QStringLiteral(
                             "散射掩码 0x%1 不包含模块 %2")
                             .arg(scenario.scatter, 2, 16, QLatin1Char('0'))
                             .arg(coincidence.scatter.module)));
                QVERIFY2(scenario.absorber &
                         quint8(1u << coincidence.absorber.module),
                         qPrintable(QStringLiteral(
                             "吸收掩码 0x%1 不包含模块 %2")
                             .arg(scenario.absorber, 2, 16, QLatin1Char('0'))
                             .arg(coincidence.absorber.module)));
                sawAmbiguous |= coincidence.ambiguous();
            }
            QVERIFY(sawAmbiguous);
            controller.stop();
        }
    }

    void udpControlUsesTheBoundDataPort() {
        QUdpSocket fpgaControl;
        QVERIFY(fpgaControl.bind(QHostAddress::LocalHost, 0));
        const quint16 controlPort = fpgaControl.localPort();

        QUdpSocket portProbe;
        QVERIFY(portProbe.bind(QHostAddress::LocalHost, 0));
        const quint16 dataPort = portProbe.localPort();
        portProbe.close();

        AcquisitionController controller;
        AcquisitionPreset preset;
        controller.startUdp(dataPort, QHostAddress(QHostAddress::LocalHost),
                            controlPort, preset, QString(), false);
        QTRY_VERIFY_WITH_TIMEOUT(fpgaControl.hasPendingDatagrams(), 2000);
        const QNetworkDatagram request = fpgaControl.receiveDatagram();
        QCOMPARE(request.senderPort(), dataPort);
        QCOMPARE(request.data().size(), ControlFrameBytes);
        QCOMPARE(qFromBigEndian<quint32>(
                     reinterpret_cast<const uchar *>(
                         request.data().constData())),
                 ControlMagic);
    }

    void baselineTrackingUsesDedicatedIdleSequence() {
        QUdpSocket fpgaControl;
        QVERIFY(fpgaControl.bind(QHostAddress::LocalHost, 0));
        const quint16 controlPort = fpgaControl.localPort();

        QUdpSocket portProbe;
        QVERIFY(portProbe.bind(QHostAddress::LocalHost, 0));
        const quint16 dataPort = portProbe.localPort();
        portProbe.close();

        AcquisitionController controller;
        QSignalSpy finished(
            &controller,
            &AcquisitionController::baselineTrackingApplyFinished);
        QSignalSpy states(&controller, &AcquisitionController::statusReady);
        quint32 responseSequence = 100;
        quint32 configVersion = 20;

        for (const bool enabled : {true, false}) {
            QVector<ControlOpcode> opcodes;
            controller.applyBaselineTracking(
                dataPort, QHostAddress(QHostAddress::LocalHost),
                controlPort, enabled);

            for (int commandIndex = 0; commandIndex < 2; ++commandIndex) {
                QTRY_VERIFY_WITH_TIMEOUT(
                    fpgaControl.hasPendingDatagrams(), 2000);
                const auto datagram = fpgaControl.receiveDatagram();
                QCOMPARE(datagram.senderPort(), dataPort);
                QCOMPARE(datagram.data().size(), ControlFrameBytes);
                const auto request = captureControl(datagram.data());
                opcodes.push_back(request.opcode);

                ControlResponse response;
                response.transactionId = request.transaction;
                response.opcode = quint8(request.opcode);
                response.target = quint8(request.target);
                response.targetId = request.targetId;
                response.status = quint8(ControlStatus::Ok);
                response.registerAddress = request.registerAddress;
                response.value =
                    request.opcode == ControlOpcode::CommitAll
                        ? ++configVersion : request.value;
                QCOMPARE(
                    fpgaControl.writeDatagram(
                        controlResponsePacket(response, responseSequence++),
                        QHostAddress::LocalHost, dataPort) > 0,
                    true);
            }

            QTRY_COMPARE_WITH_TIMEOUT(finished.count(),
                                      enabled ? 1 : 2, 2000);
            const auto result = finished.at(enabled ? 0 : 1);
            QCOMPARE(result.at(0).toBool(), enabled);
            QVERIFY(result.at(1).toBool());
            QCOMPARE(opcodes,
                     QVector<ControlOpcode>({
                         ControlOpcode::WriteShadow,
                         ControlOpcode::CommitAll
                     }));
            QVERIFY(!opcodes.contains(ControlOpcode::Start));
        }

        for (const auto &emission : states) {
            const auto snapshot =
                qvariant_cast<AcquisitionSnapshot>(emission.at(0));
            QCOMPARE(snapshot.state, AcquisitionState::Idle);
            QVERIFY(!snapshot.eventTextEnabled);
        }
    }

    void manualBaselineValueIsWrittenBeforeCommitWithoutCapabilityCache() {
        QUdpSocket fpgaControl;
        QVERIFY(fpgaControl.bind(QHostAddress::LocalHost, 0));
        const quint16 controlPort = fpgaControl.localPort();

        QUdpSocket portProbe;
        QVERIFY(portProbe.bind(QHostAddress::LocalHost, 0));
        const quint16 dataPort = portProbe.localPort();
        portProbe.close();

        AcquisitionController controller;
        QSignalSpy finished(
            &controller,
            &AcquisitionController::baselineTrackingApplyFinished);
        constexpr quint16 expectedBaseline = 1234;
        controller.applyBaselineTracking(
            dataPort, QHostAddress(QHostAddress::LocalHost),
            controlPort, false, expectedBaseline);

        QVector<CapturedControl> requests;
        quint32 responseSequence = 400;
        quint32 configVersion = 40;
        for (int commandIndex = 0; commandIndex < 3; ++commandIndex) {
            QTRY_VERIFY_WITH_TIMEOUT(
                fpgaControl.hasPendingDatagrams(), 2000);
            const auto request =
                captureControl(fpgaControl.receiveDatagram().data());
            requests.push_back(request);

            ControlResponse response;
            response.transactionId = request.transaction;
            response.opcode = quint8(request.opcode);
            response.target = quint8(request.target);
            response.targetId = request.targetId;
            response.status = quint8(ControlStatus::Ok);
            response.registerAddress = request.registerAddress;
            response.value = request.opcode == ControlOpcode::CommitAll
                ? ++configVersion : request.value;
            QVERIFY(fpgaControl.writeDatagram(
                        controlResponsePacket(response, responseSequence++),
                        QHostAddress::LocalHost, dataPort) > 0);
        }

        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 2000);
        QVERIFY(finished.at(0).at(1).toBool());
        QCOMPARE(requests.at(0).opcode, ControlOpcode::WriteShadow);
        QCOMPARE(requests.at(0).registerAddress, quint16(1));
        QCOMPARE(requests.at(1).opcode, ControlOpcode::WriteShadow);
        QCOMPARE(requests.at(1).registerAddress,
                 RegisterGlobalManualBaseline);
        QCOMPARE(requests.at(1).value, quint32(expectedBaseline));
        QCOMPARE(requests.at(2).opcode, ControlOpcode::CommitAll);
    }

    void startAfterBaselineApplyReachesStartCommand() {
        QUdpSocket fpgaControl;
        QVERIFY(fpgaControl.bind(QHostAddress::LocalHost, 0));
        const quint16 controlPort = fpgaControl.localPort();

        QUdpSocket portProbe;
        QVERIFY(portProbe.bind(QHostAddress::LocalHost, 0));
        const quint16 dataPort = portProbe.localPort();
        portProbe.close();

        AcquisitionController controller;
        QSignalSpy baselineFinished(
            &controller,
            &AcquisitionController::baselineTrackingApplyFinished);
        QSignalSpy controlBusy(
            &controller, &AcquisitionController::controlBusyChanged);
        QSignalSpy states(&controller, &AcquisitionController::statusReady);
        quint32 responseSequence = 500;
        quint32 configVersion = 10;

        controller.applyBaselineTracking(
            dataPort, QHostAddress(QHostAddress::LocalHost),
            controlPort, true);
        for (int commandIndex = 0; commandIndex < 2; ++commandIndex) {
            QTRY_VERIFY_WITH_TIMEOUT(
                fpgaControl.hasPendingDatagrams(), 2000);
            const auto request =
                captureControl(fpgaControl.receiveDatagram().data());
            ControlResponse response;
            response.transactionId = request.transaction;
            response.opcode = quint8(request.opcode);
            response.target = quint8(request.target);
            response.targetId = request.targetId;
            response.status = quint8(ControlStatus::Ok);
            response.registerAddress = request.registerAddress;
            response.value =
                request.opcode == ControlOpcode::CommitAll
                    ? ++configVersion : request.value;
            QVERIFY(fpgaControl.writeDatagram(
                        controlResponsePacket(
                            response, responseSequence++),
                        QHostAddress::LocalHost, dataPort) > 0);
        }
        QTRY_COMPARE_WITH_TIMEOUT(baselineFinished.count(), 1, 2000);
        QVERIFY(baselineFinished.at(0).at(1).toBool());

        // The UI stages its current module/pair table immediately before
        // starting. Staging must not launch a competing configuration
        // transaction merely because the baseline operation established an
        // FPGA endpoint.
        controller.setModuleConfig(0, ModuleConfig{});
        controller.setPairConfig(0, PairConfig{});
        ChannelConfig lowThreshold;
        lowThreshold.thresholdLsb = 15;
        controller.setChannelConfig(0, lowThreshold);
        QTest::qWait(50);
        QVERIFY(!fpgaControl.hasPendingDatagrams());

        AcquisitionPreset preset;
        controller.startUdp(
            dataPort, QHostAddress(QHostAddress::LocalHost),
            controlPort, preset, QString(), true);

        QVector<ControlOpcode> opcodes;
        quint32 startVersion = 0;
        bool sawLowTriggerThreshold = false;
        bool sawMatchingReturnThreshold = false;
        bool sawUnsupportedGainWrite = false;
        QVector<QByteArray> firstWrites, secondWrites;
        int commitCount = 0;
        int startCount = 0;
        QElapsedTimer repeatDelay;
        for (int commandIndex = 0; commandIndex < 320; ++commandIndex) {
            QTRY_VERIFY_WITH_TIMEOUT(
                fpgaControl.hasPendingDatagrams(), 2000);
            const auto request =
                captureControl(fpgaControl.receiveDatagram().data());
            opcodes.push_back(request.opcode);
            if (request.opcode == ControlOpcode::WriteShadow) {
                if (commitCount == 1 && secondWrites.isEmpty())
                    QVERIFY(repeatDelay.elapsed() >= 100);
                const QByteArray content = QByteArray::number(int(request.target))
                    + ':' + QByteArray::number(request.targetId)
                    + ':' + QByteArray::number(request.registerAddress)
                    + ':' + QByteArray::number(request.value);
                (commitCount == 0 ? firstWrites : secondWrites).push_back(content);
            }
            if (request.opcode == ControlOpcode::WriteShadow &&
                request.target == ControlTarget::Channel &&
                request.targetId == 0) {
                if (request.registerAddress == 1) {
                    // Automatic baseline mode reserves a 16-LSB pulse
                    // threshold, so the strictly larger trigger threshold is
                    // normalized to at least 17 LSB.
                    QCOMPARE(request.value, quint32(17));
                    sawLowTriggerThreshold = true;
                } else if (request.registerAddress == 2) {
                    QCOMPARE(request.value, quint32(16));
                    sawMatchingReturnThreshold = true;
                } else if (request.registerAddress == 12) {
                    sawUnsupportedGainWrite = true;
                }
            }

            quint32 responseValue = request.value;
            if (request.opcode == ControlOpcode::Read) {
                switch (request.registerAddress) {
                case 0x0100: responseValue = 1; break;
                case 0x0101: responseValue = 4; break;
                case 0x0102: responseValue = 8; break;
                case 0x0103: responseValue = 32; break;
                case 0x0104: responseValue = 2; break;
                case 0x0105: responseValue = 8; break;
                case 0x0106: responseValue = 1; break;
                case 0x0107: responseValue = 80'000'000; break;
                case 0x0108: responseValue = 0x0002'0100; break;
                case 0x0109: responseValue = configVersion; break;
                default: responseValue = 0; break;
                }
            } else if (request.opcode == ControlOpcode::CommitAll) {
                ++commitCount;
                if (commitCount == 1)
                    repeatDelay.start();
                responseValue = ++configVersion;
            } else if (request.opcode == ControlOpcode::Start) {
                ++startCount;
                startVersion = request.value;
            } else if (request.opcode == ControlOpcode::Stop && startCount == 1) {
                QVERIFY(repeatDelay.elapsed() >= 100);
                repeatDelay.restart();
            }

            ControlResponse response;
            response.transactionId = request.transaction;
            response.opcode = quint8(request.opcode);
            response.target = quint8(request.target);
            response.targetId = request.targetId;
            response.status = quint8(ControlStatus::Ok);
            response.registerAddress = request.registerAddress;
            response.value = responseValue;
            QVERIFY(fpgaControl.writeDatagram(
                        controlResponsePacket(
                            response, responseSequence++),
                        QHostAddress::LocalHost, dataPort) > 0);
            if (request.opcode == ControlOpcode::Start && startCount == 1) {
                repeatDelay.restart();
                ChannelEvent prestartEvent;
                prestartEvent.peak = 400;
                prestartEvent.integral = 800;
                QVERIFY(fpgaControl.writeDatagram(PacketDecoder::encodeChannels(
                    1000, configVersion, 0, {prestartEvent}),
                    QHostAddress::LocalHost, dataPort) > 0);
            }
            if (request.opcode == ControlOpcode::Start && startCount == 2)
                break;
        }

        QCOMPARE(opcodes.count(ControlOpcode::Start), 2);
        QCOMPARE(opcodes.count(ControlOpcode::Stop), 2);
        QVERIFY(opcodes.contains(ControlOpcode::ClearCounters));
        QCOMPARE(opcodes.first(), ControlOpcode::Stop);
        QCOMPARE(opcodes.at(1), ControlOpcode::Read);
        QCOMPARE(opcodes.count(ControlOpcode::CommitAll), 2);
        QVERIFY(!firstWrites.isEmpty());
        QCOMPARE(secondWrites, firstWrites);
        QCOMPARE(opcodes.last(), ControlOpcode::Start);
        QCOMPARE(startVersion, configVersion);
        QVERIFY(sawLowTriggerThreshold);
        QVERIFY(sawMatchingReturnThreshold);
        QVERIFY(!sawUnsupportedGainWrite);
        QTRY_VERIFY_WITH_TIMEOUT(
            controlBusy.count() >= 4 &&
                !controlBusy.last().at(0).toBool(),
            2000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !states.isEmpty() &&
                qvariant_cast<AcquisitionSnapshot>(
                    states.last().at(0)).status.contains(
                        QStringLiteral("采集中，配置版本")),
            2000);
        QCOMPARE(qvariant_cast<AcquisitionSnapshot>(states.last().at(0)).totalEvents,
                 quint64(0));
        ChannelEvent officialEvent;
        officialEvent.peak = 400;
        officialEvent.integral = 800;
        // A delayed pre-start packet must still be discarded after START.
        QVERIFY(fpgaControl.writeDatagram(PacketDecoder::encodeChannels(
            1001, configVersion - 1, 0, {officialEvent}),
            QHostAddress::LocalHost, dataPort) > 0);
        QVERIFY(fpgaControl.writeDatagram(PacketDecoder::encodeChannels(
            1002, configVersion, 0, {officialEvent}),
            QHostAddress::LocalHost, dataPort) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(
            qvariant_cast<AcquisitionSnapshot>(states.last().at(0)).totalEvents,
            quint64(1), 2000);
    }

    void thresholdNormalizationUsesFourteenBitRange() {
        ChannelConfig config;
        config.thresholdLsb = 0;
        QCOMPARE(normalizeChannelConfig(config).thresholdLsb,
                 quint16(3));
        config.thresholdLsb = 15;
        QCOMPARE(normalizeChannelConfig(config).thresholdLsb,
                 quint16(15));
        config.thresholdLsb = 4096;
        QCOMPARE(normalizeChannelConfig(config).thresholdLsb,
                 quint16(4096));
        config.thresholdLsb = 0xffff;
        QCOMPARE(normalizeChannelConfig(config).thresholdLsb,
                 quint16(16383));
    }

    void normalizesBaselineValidationTriplet() {
        ChannelConfig defaults;
        defaults = normalizeChannelConfig(defaults);
        QCOMPARE(defaults.baselineGuardLsb, quint16(16));
        QCOMPARE(defaults.baselinePulseThresholdLsb, quint16(32));
        QVERIFY(defaults.baselineGuardLsb <
                defaults.baselinePulseThresholdLsb);
        QVERIFY(defaults.baselinePulseThresholdLsb < defaults.thresholdLsb);

        ChannelConfig invalid;
        invalid.thresholdLsb = 64;
        invalid.baselineGuardLsb = 64;
        invalid.baselinePulseThresholdLsb = 64;
        invalid = normalizeChannelConfig(invalid);
        QVERIFY(invalid.baselineGuardLsb <
                invalid.baselinePulseThresholdLsb);
        QVERIFY(invalid.baselinePulseThresholdLsb < invalid.thresholdLsb);

        invalid.thresholdLsb = 1;
        invalid.baselineGuardLsb = 0;
        invalid.baselinePulseThresholdLsb = 0;
        invalid = normalizeChannelConfig(invalid);
        QCOMPARE(invalid.thresholdLsb, quint16(3));
        QVERIFY(invalid.baselineGuardLsb <
                invalid.baselinePulseThresholdLsb);
        QVERIFY(invalid.baselinePulseThresholdLsb < invalid.thresholdLsb);
    }

    void automaticBaselineEnforcesModalThresholdFloor() {
        ChannelConfig config;
        config.thresholdLsb = 3;
        config.baselineGuardLsb = 1;
        config.baselinePulseThresholdLsb = 2;

        const auto manual = normalizeChannelConfig(config, false);
        QCOMPARE(manual.thresholdLsb, quint16(3));
        QCOMPARE(manual.baselinePulseThresholdLsb, quint16(2));

        const auto automatic = normalizeChannelConfig(config, true);
        QCOMPARE(automatic.thresholdLsb, quint16(17));
        QCOMPARE(automatic.baselinePulseThresholdLsb, quint16(16));
        QVERIFY(automatic.baselineGuardLsb <
                automatic.baselinePulseThresholdLsb);
        QVERIFY(automatic.baselinePulseThresholdLsb <
                automatic.thresholdLsb);
    }

    void baselineTrackingBusyIsNonFatal() {
        QUdpSocket fpgaControl;
        QVERIFY(fpgaControl.bind(QHostAddress::LocalHost, 0));
        const quint16 controlPort = fpgaControl.localPort();

        QUdpSocket portProbe;
        QVERIFY(portProbe.bind(QHostAddress::LocalHost, 0));
        const quint16 dataPort = portProbe.localPort();
        portProbe.close();

        AcquisitionController controller;
        QSignalSpy finished(
            &controller,
            &AcquisitionController::baselineTrackingApplyFinished);
        QSignalSpy states(&controller, &AcquisitionController::statusReady);
        controller.applyBaselineTracking(
            dataPort, QHostAddress(QHostAddress::LocalHost),
            controlPort, true);

        QTRY_VERIFY_WITH_TIMEOUT(fpgaControl.hasPendingDatagrams(), 2000);
        const auto request =
            captureControl(fpgaControl.receiveDatagram().data());
        QCOMPARE(request.opcode, ControlOpcode::WriteShadow);
        ControlResponse response;
        response.transactionId = request.transaction;
        response.opcode = quint8(request.opcode);
        response.target = quint8(request.target);
        response.targetId = request.targetId;
        response.status = quint8(ControlStatus::Busy);
        response.registerAddress = request.registerAddress;
        QVERIFY(fpgaControl.writeDatagram(
                    controlResponsePacket(response, 200),
                    QHostAddress::LocalHost, dataPort) > 0);

        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 2000);
        QVERIFY(!finished.at(0).at(1).toBool());
        QVERIFY(finished.at(0).at(2).toString().contains(
            QStringLiteral("正在采集")));
        QTest::qWait(50);
        QVERIFY(!fpgaControl.hasPendingDatagrams());
        for (const auto &emission : states) {
            const auto snapshot =
                qvariant_cast<AcquisitionSnapshot>(emission.at(0));
            QVERIFY(snapshot.state != AcquisitionState::Running);
            QVERIFY(snapshot.state != AcquisitionState::Error);
        }
    }

    void baselineCommitRetriesWhileRangeIsSwitching() {
        QUdpSocket fpgaControl;
        QVERIFY(fpgaControl.bind(QHostAddress::LocalHost, 0));
        const quint16 controlPort = fpgaControl.localPort();

        QUdpSocket portProbe;
        QVERIFY(portProbe.bind(QHostAddress::LocalHost, 0));
        const quint16 dataPort = portProbe.localPort();
        portProbe.close();

        AcquisitionController controller;
        QSignalSpy finished(
            &controller,
            &AcquisitionController::baselineTrackingApplyFinished);
        controller.applyBaselineTracking(
            dataPort, QHostAddress(QHostAddress::LocalHost),
            controlPort, true);

        QTRY_VERIFY_WITH_TIMEOUT(fpgaControl.hasPendingDatagrams(), 2000);
        const auto writeRequest =
            captureControl(fpgaControl.receiveDatagram().data());
        QCOMPARE(writeRequest.opcode, ControlOpcode::WriteShadow);
        ControlResponse response;
        response.transactionId = writeRequest.transaction;
        response.opcode = quint8(writeRequest.opcode);
        response.target = quint8(writeRequest.target);
        response.targetId = writeRequest.targetId;
        response.status = quint8(ControlStatus::Ok);
        response.registerAddress = writeRequest.registerAddress;
        response.value = writeRequest.value;
        QVERIFY(fpgaControl.writeDatagram(
                    controlResponsePacket(response, 210),
                    QHostAddress::LocalHost, dataPort) > 0);

        QTRY_VERIFY_WITH_TIMEOUT(fpgaControl.hasPendingDatagrams(), 2000);
        const auto firstCommit =
            captureControl(fpgaControl.receiveDatagram().data());
        QCOMPARE(firstCommit.opcode, ControlOpcode::CommitAll);
        response.transactionId = firstCommit.transaction;
        response.opcode = quint8(firstCommit.opcode);
        response.status = quint8(ControlStatus::Busy);
        response.registerAddress = firstCommit.registerAddress;
        response.value = 0;
        QVERIFY(fpgaControl.writeDatagram(
                    controlResponsePacket(response, 211),
                    QHostAddress::LocalHost, dataPort) > 0);

        QTRY_VERIFY_WITH_TIMEOUT(fpgaControl.hasPendingDatagrams(), 2000);
        const auto retriedCommit =
            captureControl(fpgaControl.receiveDatagram().data());
        QCOMPARE(retriedCommit.opcode, ControlOpcode::CommitAll);
        QVERIFY(retriedCommit.transaction != firstCommit.transaction);
        QCOMPARE(finished.count(), 0);

        response.transactionId = retriedCommit.transaction;
        response.opcode = quint8(retriedCommit.opcode);
        response.status = quint8(ControlStatus::Ok);
        response.registerAddress = retriedCommit.registerAddress;
        response.value = 21;
        QVERIFY(fpgaControl.writeDatagram(
                    controlResponsePacket(response, 212),
                    QHostAddress::LocalHost, dataPort) > 0);

        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 2000);
        QVERIFY(finished.at(0).at(1).toBool());
    }

    void baselineTrackingRejectsRapidRepeat() {
        QUdpSocket fpgaControl;
        QVERIFY(fpgaControl.bind(QHostAddress::LocalHost, 0));
        const quint16 controlPort = fpgaControl.localPort();

        QUdpSocket portProbe;
        QVERIFY(portProbe.bind(QHostAddress::LocalHost, 0));
        const quint16 dataPort = portProbe.localPort();
        portProbe.close();

        AcquisitionController controller;
        QSignalSpy finished(
            &controller,
            &AcquisitionController::baselineTrackingApplyFinished);
        controller.applyBaselineTracking(
            dataPort, QHostAddress(QHostAddress::LocalHost),
            controlPort, true);
        controller.applyBaselineTracking(
            dataPort, QHostAddress(QHostAddress::LocalHost),
            controlPort, false);

        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 2000);
        QCOMPARE(finished.at(0).at(0).toBool(), false);
        QCOMPARE(finished.at(0).at(1).toBool(), false);

        QVector<ControlOpcode> opcodes;
        quint32 responseSequence = 300;
        for (int commandIndex = 0; commandIndex < 2; ++commandIndex) {
            QTRY_VERIFY_WITH_TIMEOUT(
                fpgaControl.hasPendingDatagrams(), 2000);
            const auto request =
                captureControl(fpgaControl.receiveDatagram().data());
            opcodes.push_back(request.opcode);

            ControlResponse response;
            response.transactionId = request.transaction;
            response.opcode = quint8(request.opcode);
            response.target = quint8(request.target);
            response.targetId = request.targetId;
            response.status = quint8(ControlStatus::Ok);
            response.registerAddress = request.registerAddress;
            response.value =
                request.opcode == ControlOpcode::CommitAll ? 31u
                                                           : request.value;
            QVERIFY(fpgaControl.writeDatagram(
                        controlResponsePacket(
                            response, responseSequence++),
                        QHostAddress::LocalHost, dataPort) > 0);
        }

        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 2000);
        QCOMPARE(finished.at(1).at(0).toBool(), true);
        QCOMPARE(finished.at(1).at(1).toBool(), true);
        QCOMPARE(opcodes,
                 QVector<ControlOpcode>({
                     ControlOpcode::WriteShadow,
                     ControlOpcode::CommitAll
                 }));
        QVERIFY(!opcodes.contains(ControlOpcode::Start));
        QTest::qWait(50);
        QVERIFY(!fpgaControl.hasPendingDatagrams());
    }

    void baselineTrackingRejectsRunningWorkerState() {
        AcquisitionController controller;
        QSignalSpy finished(
            &controller,
            &AcquisitionController::baselineTrackingApplyFinished);
        AcquisitionPreset preset;
        controller.startSimulation(preset);
        QTest::qWait(20);
        controller.applyBaselineTracking(
            49152, QHostAddress(QHostAddress::LocalHost), 49153, true);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1000);
        QVERIFY(!finished.at(0).at(1).toBool());
        QVERIFY(finished.at(0).at(2).toString().contains(
            QStringLiteral("先停止采集")));
        controller.stop();
    }

    void eventTextWriterProducesDocumentedColumns() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("events.txt"));

        EventTextWriter writer;
        EventTextMetadata metadata;
        metadata.deviceAddress = QHostAddress(QStringLiteral("192.168.1.10"));
        metadata.sampleRateHz = 80'000'000;
        metadata.startedAt = QDateTime::fromString(
            QStringLiteral("2026-07-30T12:00:00+08:00"), Qt::ISODate);
        QString error;
        QVERIFY2(writer.start(path, metadata, &error), qPrintable(error));

        ChannelEvent first;
        first.timestamp = 8;
        first.channel = 3;
        first.peak = 0x1230;
        first.integral = 0x010203;
        first.pileup = true;
        first.overflow = false;
        first.polarity = true;
        ChannelEvent second = first;
        second.timestamp = 16;
        second.channel = 7;
        second.pileup = false;
        QVERIFY(writer.enqueue(QVector<ChannelEvent>{first, second}, &error));
        writer.finish(true);

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        QVERIFY(text.contains(QStringLiteral("# format=MCA_EVENT_TXT_V2")));
        QVERIFY(text.contains(QStringLiteral("# sample_rate_hz=80000000")));
        QVERIFY(text.contains(QStringLiteral(
            "# polarity=0:positive,1:negative")));
        QVERIFY(text.contains(QStringLiteral(
            "eventcount\ttimestamp\tchannel\tpeak\tintegral"
            "\tpileup\tpolarity")));
        QVERIFY(text.contains(QStringLiteral(
            "0\t8\t4\t291\t66051\t1\t1")));
        QVERIFY(text.contains(QStringLiteral(
            "1\t16\t8\t291\t66051\t0\t1")));
        QVERIFY(!text.contains(QStringLiteral("time_ns")));
        QVERIFY(!text.contains(QStringLiteral("overflow")));
        QVERIFY(!text.contains(QStringLiteral("peak_raw")));
        QVERIFY(text.contains(QStringLiteral("# completed=1")));
        QVERIFY(text.contains(QStringLiteral("# events=2")));
    }

    void eventTextReportsInvalidPathAndIncompleteFooter() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        EventTextWriter writer;
        EventTextMetadata metadata;
        metadata.deviceAddress = QHostAddress(QStringLiteral("192.168.1.10"));
        metadata.sampleRateHz = 80'000'000;
        metadata.startedAt = QDateTime::currentDateTime();
        QString error;
        QVERIFY(!writer.start(
            directory.filePath(QStringLiteral("missing/events.txt")),
            metadata, &error));
        QVERIFY(!error.isEmpty());

        const QString path =
            directory.filePath(QStringLiteral("incomplete.txt"));
        QVERIFY2(writer.start(path, metadata, &error), qPrintable(error));
        ChannelEvent pileup;
        pileup.timestamp = 80;
        pileup.channel = 0;
        pileup.peak = 160;
        pileup.integral = 320;
        pileup.pileup = true;

        SpectrumAccumulator accumulator(1);
        QCOMPARE(accumulator.add(QVector<ChannelEvent>{pileup}, false),
                 quint64(0));
        QCOMPARE(accumulator.snapshot()->totalEvents, quint64(0));
        QVERIFY(writer.enqueue(QVector<ChannelEvent>{pileup}, &error));
        writer.finish(false, QStringLiteral("test interruption"));

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        QVERIFY(text.contains(QStringLiteral(
            "0\t80\t1\t10\t320\t1\t0")));
        QVERIFY(text.contains(QStringLiteral("# completed=0")));
        QVERIFY(text.contains(QStringLiteral("# reason=test interruption")));
    }

    void spectrumFileV1RoundTripAndRejectsTruncation() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        SpectrumSnapshot source;
        source.channelCount = 2;
        source.totalEvents = 3;
        source.peak.resize(2);
        source.integral.resize(2);
        for (int channel = 0; channel < 2; ++channel) {
            source.peak[channel].fill(0, 16384);
            source.integral[channel].fill(0, DefaultIntegralBins);
        }
        source.sampleRateHz = 125'000'000;
        source.adcBits = 14;
        source.peakFractionBits = 2;
        source.peak[0][123] = 2;
        source.peak[1][456] = 1;
        source.integral[0][789] = 2;
        source.integral[1][1024] = 1;

        const QString path = directory.filePath(QStringLiteral("test.mcaspec"));
        QString error;
        QVERIFY2(SpectrumFile::writeV1(path, source, &error),
                 qPrintable(error));
        const auto loaded = SpectrumFile::readV1(path, &error);
        QVERIFY2(loaded, qPrintable(error));
        QCOMPARE(loaded->channelCount, 2);
        QCOMPARE(loaded->totalEvents, quint64(3));
        QCOMPARE(loaded->peak[0][123], quint64(2));
        QCOMPARE(loaded->integral[1][1024], quint64(1));
        QCOMPARE(loaded->peak[0].size(), 16384);
        QCOMPARE(loaded->sampleRateHz, quint32(125'000'000));
        QCOMPARE(loaded->adcBits, quint8(14));
        QCOMPARE(loaded->peakFractionBits, quint8(2));

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.resize(20));
        file.close();
        QVERIFY(!SpectrumFile::readV1(path, &error));
        QVERIFY(!error.isEmpty());
    }

    void advancedProcessingDefaultsDisabled() {
        ModuleConfig module;
        PairConfig pair;
        QVERIFY(!module.enabled);
        QVERIFY(!pair.enabled);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Waveform mode
    // ═══════════════════════════════════════════════════════════════════

    void waveformChannelControlWordCarriesPolarity() {
        ChannelConfig config;
        config.polarity = Polarity::Positive;
        QCOMPARE(makeChannelControlWord(config, true), quint32(0x1));
        config.polarity = Polarity::Negative;
        QCOMPARE(makeChannelControlWord(config, true), quint32(0x3));
        QCOMPARE(makeChannelControlWord(config, false), quint32(0x2));
    }

    void waveformFragmentRoundTrip() {
        const auto fragment = waveformFragment(3);
        const auto bytes = PacketDecoder::encodeWaveform(11, 0x0002'0003, 0,
                                                         fragment);
        // 20-byte common header + 4-byte session + 32-byte fragment header
        // + 512 sample words.
        QCOMPARE(bytes.size(), PacketHeaderBytes + WaveformHeaderBytes
                                   + WaveformFragmentMax * 2);
        QCOMPARE(bytes.left(20).toHex(),
                 QByteArray("4d434132020600000000000b00020003"
                            "0001" "0424"));

        DecodedPacket packet;
        QString error;
        QVERIFY2(PacketDecoder::decode(bytes, packet, &error),
                 qPrintable(error));
        QCOMPARE(packet.type, PacketType::Waveform);
        QCOMPARE(packet.sequence, quint32(11));
        QCOMPARE(packet.configVersion, quint32(0x0002'0003));
        QCOMPARE(packet.waveformFragments.size(), 1);
        const auto &decoded = packet.waveformFragments[0];
        QCOMPARE(decoded.sessionId, fragment.sessionId);
        QCOMPARE(decoded.captureId, fragment.captureId);
        QCOMPARE(decoded.triggerTimestamp, fragment.triggerTimestamp);
        QCOMPARE(decoded.baseline, fragment.baseline);
        QCOMPARE(decoded.threshold, fragment.threshold);
        QCOMPARE(decoded.returnThreshold, fragment.returnThreshold);
        QCOMPARE(decoded.preSamples, quint16(WaveformPreSamples));
        QCOMPARE(decoded.totalSamples, quint16(WaveformTotalSamples));
        QCOMPARE(decoded.sampleOffset,
                 quint16(3 * WaveformFragmentMax));
        QCOMPARE(decoded.sampleCount, quint16(WaveformFragmentMax));
        QCOMPARE(decoded.fragmentIndex, quint16(3));
        QCOMPARE(decoded.fragmentCount, quint16(WaveformFragmentCount));
        QCOMPARE(decoded.samplePeriodNs, quint16(WaveformSamplePeriodNs));
        QCOMPARE(decoded.samples, fragment.samples);
        QVERIFY(!decoded.negativePolarity());
        QCOMPARE(decoded.adcCode(0), quint16(fragment.samples[0] & 0x3fff));
    }

    void waveformLastFragmentHas212Samples() {
        const auto fragment = waveformFragment(WaveformFragmentCount - 1);
        QCOMPARE(fragment.sampleCount, quint16(WaveformLastSamples));
        QCOMPARE(WaveformLastSamples, 212);
        const auto bytes = PacketDecoder::encodeWaveform(1, 4, 0, fragment);
        QCOMPARE(bytes.size(), PacketHeaderBytes + WaveformHeaderBytes
                                   + WaveformLastSamples * 2);
        DecodedPacket packet;
        QString error;
        QVERIFY2(PacketDecoder::decode(bytes, packet, &error),
                 qPrintable(error));
        QCOMPARE(packet.waveformFragments[0].sampleCount,
                 quint16(WaveformLastSamples));
    }

    void waveformRejectsMalformedFragments() {
        QString error;
        DecodedPacket packet;
        auto fragment = waveformFragment(0);

        // A common header that disagrees with the fragment header length.
        auto bytes = PacketDecoder::encodeWaveform(1, 4, 0, fragment);
        bytes[18] = 0x00; bytes[19] = 0x10;
        QVERIFY(!PacketDecoder::decode(bytes, packet, &error));

        // record_count must be one.
        bytes = PacketDecoder::encodeWaveform(1, 4, 0, fragment);
        bytes[16] = 0x00; bytes[17] = 0x02;
        QVERIFY(!PacketDecoder::decode(bytes, packet, &error));

        // Wrong pre_samples.
        fragment.preSamples = 100;
        bytes = PacketDecoder::encodeWaveform(1, 4, 0, fragment);
        QVERIFY(!PacketDecoder::decode(bytes, packet, &error));
        fragment.preSamples = WaveformPreSamples;

        // Wrong sample_period_ns.
        fragment.samplePeriodNs = 4;
        bytes = PacketDecoder::encodeWaveform(1, 4, 0, fragment);
        QVERIFY(!PacketDecoder::decode(bytes, packet, &error));
        fragment.samplePeriodNs = WaveformSamplePeriodNs;

        // An over-long fragment.
        fragment.samples.resize(WaveformFragmentMax + 1);
        fragment.sampleCount = quint16(fragment.samples.size());
        bytes = PacketDecoder::encodeWaveform(1, 4, 0, fragment);
        QVERIFY(!PacketDecoder::decode(bytes, packet, &error));

        // A type above the extended PSD type seven is still rejected.
        bytes = PacketDecoder::encodeChannels(1, 4, 0, {});
        bytes[5] = 8;
        QVERIFY(!PacketDecoder::decode(bytes, packet, &error));

        // The reassembler rejects a wrong geometry even if the packet is well
        // formed.
        auto assembled = waveformFragment(0);
        assembled.fragmentIndex = 1;
        assembled.sampleOffset = 0;
        QVERIFY(!WaveformReassembler::validate(assembled, &error));
        QVERIFY(!error.isEmpty());
        assembled = waveformFragment(0);
        assembled.flags = 0x8000;
        QVERIFY(!WaveformReassembler::validate(assembled, &error));
    }

    void waveformReassemblyAcceptsAnyOrder() {
        WaveformReassembler reassembler;
        std::shared_ptr<const WaveformCapture> capture;
        int expectedDuplicates = 0;
        // Deliver every fragment in reverse order, with duplicates in the
        // middle, and expect exactly one complete record at the end.
        for (int index = WaveformFragmentCount - 1; index >= 0; --index) {
            const auto fragment = waveformFragment(index);
            auto result = reassembler.add(fragment, 0, 0x0002'0003);
            if (index > 0 && index % 5 == 0) {
                // A byte-identical duplicate must be ignored.
                auto duplicate = reassembler.add(fragment, 0, 0x0002'0003);
                QVERIFY(!duplicate.has_value());
                ++expectedDuplicates;
            }
            if (result)
                capture = *result;
        }
        QVERIFY(capture != nullptr);
        QVERIFY(capture->valid());
        QCOMPARE(capture->size(), WaveformSampleCount);
        QCOMPARE(capture->triggerIndex(), WaveformPreSamples);
        QCOMPARE(capture->sessionId, quint32(0x2222'3333));
        QCOMPARE(capture->captureId, quint32(7));
        QCOMPARE(capture->baseline, quint16(8200));
        QCOMPARE(capture->threshold, quint16(120));
        // Every sample must be in place without duplication or omission.
        for (int index = 0; index < WaveformFragmentCount; ++index) {
            const auto fragment = waveformFragment(index);
            for (int i = 0; i < fragment.samples.size(); ++i) {
                QCOMPARE(capture->rawSample(index * WaveformFragmentMax + i),
                         fragment.samples[i]);
            }
        }
        QCOMPARE(capture->rawSample(WaveformPreSamples), quint16(0x1234));
        QCOMPARE(capture->timeNs(0), qint64(-30'000));
        QCOMPARE(capture->timeNs(WaveformPreSamples), qint64(0));
        QCOMPARE(capture->timeNs(WaveformSampleCount - 1), qint64(69'992));
        QCOMPARE(reassembler.statistics().completedCaptures, quint64(1));
        // Fragments 5, 10, 15 and 20 were each delivered twice.
        QCOMPARE(reassembler.statistics().duplicateFragments,
                 quint64(expectedDuplicates));
        QCOMPARE(expectedDuplicates, 4);
        QCOMPARE(reassembler.statistics().fragments,
                 quint64(WaveformFragmentCount + expectedDuplicates));
        QCOMPARE(reassembler.pendingCount(), 0);
    }

    void waveformReassemblyRejectsConflictsAndLostFragments() {
        // A conflicting duplicate voids the record.
        {
            WaveformReassembler reassembler;
            reassembler.add(waveformFragment(0), 0, 1);
            auto conflicting = waveformFragment(0);
            conflicting.samples[10] ^= 0x0001;
            QString error;
            auto result = reassembler.add(conflicting, 0, 1, &error);
            QVERIFY(!result.has_value());
            QVERIFY(!error.isEmpty());
            QCOMPARE(reassembler.pendingCount(), 0);
            QCOMPARE(reassembler.statistics().conflictingFragments, quint64(1));
        }
        // Metadata that disagrees with the first fragment voids the record.
        {
            WaveformReassembler reassembler;
            reassembler.add(waveformFragment(0), 0, 1);
            auto mismatched = waveformFragment(1);
            mismatched.baseline = 9000;
            QString error;
            auto result = reassembler.add(mismatched, 0, 1, &error);
            QVERIFY(!result.has_value());
            QVERIFY(!error.isEmpty());
            QCOMPARE(reassembler.pendingCount(), 0);
        }
        // A missing fragment never completes, and the timeout discards it.
        {
            WaveformReassembler reassembler;
            for (int index = 0; index < WaveformFragmentCount; ++index) {
                if (index == 13)
                    continue;
                auto result = reassembler.add(waveformFragment(index), 0, 1);
                QVERIFY(!result.has_value());
            }
            QCOMPARE(reassembler.pendingCount(), 1);
            QCOMPARE(reassembler.expire(QDateTime::currentDateTime()
                                        .addMSecs(1500)), 1);
            QCOMPARE(reassembler.pendingCount(), 0);
            QCOMPARE(reassembler.statistics().timedOutCaptures, quint64(1));
            QCOMPARE(reassembler.statistics().completedCaptures, quint64(0));
        }
        // A fragment from a superseded session is dropped.
        {
            WaveformReassembler reassembler;
            reassembler.add(waveformFragment(0), 0, 1);
            auto other = waveformFragment(1, 7, 0x9999'0000);
            QString error;
            auto result = reassembler.add(other, 0, 1, &error);
            QVERIFY(!result.has_value());
            QCOMPARE(reassembler.statistics().staleFragments, quint64(1));
        }
    }

    void waveformReassemblyEvictsOldestWhenFourArePending() {
        WaveformReassembler reassembler;
        // Four captures may be in flight; a fifth evicts the oldest.
        for (quint32 capture = 1; capture <= WaveformReassembler::MaximumPending;
             ++capture) {
            reassembler.add(waveformFragment(0, capture), 0, 1);
        }
        QCOMPARE(reassembler.pendingCount(), WaveformReassembler::MaximumPending);
        reassembler.add(waveformFragment(0, 99), 0, 1);
        QCOMPARE(reassembler.pendingCount(), WaveformReassembler::MaximumPending);
        QCOMPARE(reassembler.statistics().evictedCaptures, quint64(1));
        QCOMPARE(reassembler.statistics().discardedCaptures, quint64(1));
    }

    void waveformCsvHasFixedShape() {
        WaveformReassembler reassembler;
        std::shared_ptr<const WaveformCapture> capture;
        for (int index = 0; index < WaveformFragmentCount; ++index) {
            auto result = reassembler.add(waveformFragment(index), 3, 0x0002'0003);
            if (result)
                capture = *result;
        }
        QVERIFY(capture != nullptr);
        QVERIFY(capture->overrange(0));
        QVERIFY(!capture->overrange(1));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("wave.csv"));
        QString error;
        QVERIFY2(writeWaveformCsv(path, *capture, &error), qPrintable(error));

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        const QStringList lines = text.split(QLatin1Char('\n'));
        QCOMPARE(lines.first(), QStringLiteral("# MCA_WAVEFORM_CSV_V1"));
        QVERIFY(text.contains(QStringLiteral("# capture_id=7")));
        QVERIFY(text.contains(QStringLiteral("# baseline=8200")));
        QVERIFY(text.contains(QStringLiteral("# threshold=120")));
        QVERIFY(text.contains(QStringLiteral("# sample_period_ns=8")));
        QVERIFY(text.contains(QStringLiteral("# total_samples=12500")));
        QVERIFY(text.contains(QStringLiteral(
            "sample_index,time_ns,adc_code,overrange")));
        // Exactly 12500 data rows plus the comment block and header, and the
        // final row must be the last sample.
        int dataRows = 0;
        for (const auto &line : lines) {
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))
                || line.startsWith(QStringLiteral("sample_index")))
                continue;
            ++dataRows;
        }
        QCOMPARE(dataRows, WaveformSampleCount);
        const QString last = lines[lines.size() - 2];
        QVERIFY(last.startsWith(QStringLiteral("12499,69992,")));
        QVERIFY(text.contains(
            QStringLiteral("3750,0,%1,0").arg(capture->adcCode(3750))));
        // Overrange is reported per sample and the raw code is not changed.
        QVERIFY(text.contains(
            QStringLiteral("0,-30000,%1,1").arg(capture->adcCode(0))));
        QVERIFY(text.contains(
            QStringLiteral("1,-29992,%1,0").arg(capture->adcCode(1))));

        // An incomplete capture is never exported.
        WaveformCapture partial = *capture;
        partial.samples.resize(100);
        partial.totalSamples = 100;
        QVERIFY(!writeWaveformCsv(
            directory.filePath(QStringLiteral("partial.csv")), partial,
            &error));
        QVERIFY(!error.isEmpty());
    }

    void waveformRegistersAndControlFramesAreExact() {
        // The waveform registers are part of the wire protocol.
        QCOMPARE(RegisterGlobalWaveTriggerMode, quint16(0x0008));
        QCOMPARE(RegisterGlobalWaveSession, quint16(0x0009));
        QCOMPARE(RegisterWaveStatus, quint16(0x0120));
        QCOMPARE(RegisterWaveLastCapture, quint16(0x0121));
        QCOMPARE(RegisterWaveCaptureCount, quint16(0x0122));
        QCOMPARE(RegisterGlobalExternalDurationUs, quint16(0x000B));
        QCOMPARE(RegisterExternalStatus, quint16(0x0124));
        QCOMPARE(RegisterExternalTriggerCount, quint16(0x0125));
        QCOMPARE(ModeWaveform, quint32(3));
        QCOMPARE(ModeSpectrum, quint32(0));
        QCOMPARE(ModeExternalTrigger, quint32(4));
        QCOMPARE(FormatFlagWaveform, quint32(0x20));
        QCOMPARE(FormatFlagExternalTrigger, quint32(0x100));
        QCOMPARE(WaveformMaximumPayloadBytes, 1060);

        QCOMPARE(makeShadowWrite(ControlTarget::Global, 0,
                                 RegisterGlobalWaveSession,
                                 0x1122'3344, 9).toHex(),
                 QByteArray("4d43463202010000000900000000000911223344"));
        QCOMPARE(makeShadowWrite(ControlTarget::Global, 0,
                                 RegisterGlobalMode, ModeWaveform,
                                 10).toHex(),
                 QByteArray("4d43463202010000000000000000000a00000003"));
        QCOMPARE(makeRead(ControlTarget::Global, 0, RegisterWaveStatus,
                          11).toHex(),
                 QByteArray("4d43463202020000012000000000000b00000000"));

        DeviceCapabilities capabilities;
        QVERIFY(!capabilities.supportsWaveform());
        capabilities.formatFlags = FormatFlagWaveform;
        QVERIFY(capabilities.supportsWaveform());
        // The other capability bits alone must not enable the waveform entry.
        capabilities.formatFlags = FormatFlagExtendedQuery
                                 | FormatFlagManualBaseline
                                 | FormatFlagExtendedStatus;
        QVERIFY(!capabilities.supportsWaveform());
        QVERIFY(!capabilities.supportsExternalTrigger());
        capabilities.formatFlags |= FormatFlagExternalTrigger;
        QVERIFY(capabilities.supportsExternalTrigger());
    }

    void waveformCsvTimeAxisMatchesTheFixedGeometry() {
        WaveformReassembler reassembler;
        std::shared_ptr<const WaveformCapture> capture;
        for (int index = 0; index < WaveformFragmentCount; ++index) {
            auto result = reassembler.add(waveformFragment(index), 0, 1);
            if (result)
                capture = *result;
        }
        QVERIFY(capture != nullptr);
        // The full record covers -30 us .. +69.992 us over 12500 samples, and
        // the trigger sample sits at array index 3750 with time 0.
        QCOMPARE(capture->timeNs(0), qint64(-30'000));
        QCOMPARE(capture->timeNs(capture->triggerIndex()), qint64(0));
        QCOMPARE(capture->timeNs(capture->size() - 1), qint64(69'992));
        QCOMPARE(capture->size() - 1, WaveformSampleCount - 1);
        QCOMPARE(capture->triggerIndex(), 3750);
        QCOMPARE(WaveformStartNs, qint64(-30'000));
        QCOMPARE(WaveformEndNs, qint64(69'992));
        // Sample 1 of an 80 ns window is one period away from the trigger.
        QCOMPARE(capture->timeNs(WaveformPreSamples + 1), qint64(8));
        QCOMPARE(capture->timeNs(WaveformPreSamples - 1), qint64(-8));
    }

    void serialFrameHandlesFragmentationNoiseAndCrc() {
        const QByteArray payload = QByteArray::fromHex("001122334455");
        const QByteArray frame = SerialFrameCodec::encode(
            SerialMessageType::ControlRequest, 0xffff'ffffu, payload);
        QCOMPARE(frame.size(), SerialHeaderBytes + payload.size() + SerialCrcBytes);

        SerialFrameCodec codec;
        QStringList errors;
        QCOMPARE(codec.feed(QByteArray("noise") + frame.left(7), &errors).size(), 0);
        const auto decoded = codec.feed(frame.mid(7), &errors);
        QCOMPARE(decoded.size(), 1);
        QCOMPARE(decoded[0].type, SerialMessageType::ControlRequest);
        QCOMPARE(decoded[0].sequence, quint32(0xffff'ffffu));
        QCOMPARE(decoded[0].payload, payload);
        QVERIFY(!errors.isEmpty());

        QByteArray bad = frame;
        bad[SerialHeaderBytes] = char(uchar(bad[SerialHeaderBytes]) ^ 0x80u);
        errors.clear();
        QCOMPARE(codec.feed(bad + frame, &errors).size(), 1);
        QVERIFY(std::any_of(errors.cbegin(), errors.cend(), [](const QString &e) {
            return e.contains(QStringLiteral("CRC"));
        }));

        QByteArray damagedVersion = frame;
        damagedVersion[4] = char(1);
        errors.clear();
        QVERIFY(codec.feed(damagedVersion, &errors).isEmpty());
        QVERIFY(std::any_of(errors.cbegin(), errors.cend(), [](const QString &e) {
            return e.contains(QStringLiteral("CRC"));
        }));
        QVERIFY(std::none_of(errors.cbegin(), errors.cend(), [](const QString &e) {
            return e.contains(QStringLiteral("协议版本"));
        }));

        QByteArray oldVersion = frame;
        oldVersion[4] = char(1);
        const quint32 oldVersionCrc = SerialFrameCodec::crc32(
            oldVersion.left(oldVersion.size() - SerialCrcBytes));
        qToBigEndian<quint32>(oldVersionCrc,
            reinterpret_cast<uchar *>(oldVersion.data()
                                      + oldVersion.size() - SerialCrcBytes));
        errors.clear();
        QVERIFY(codec.feed(oldVersion, &errors).isEmpty());
        QVERIFY(std::any_of(errors.cbegin(), errors.cend(), [](const QString &e) {
            return e.contains(QStringLiteral("协议版本"));
        }));
    }

    void serialFrameAcceptsHelloSplit32And8() {
        const QByteArray captured = QByteArray::fromHex(
            "4d4353320281001000000ae300140000"
            "0007a1201000020c004007735940003f"
            "00020500dea3fbb0");
        QCOMPARE(captured.size(), 40);
        QCOMPARE(SerialFrameCodec::crc32(captured.left(36)), quint32(0xdea3fbb0u));

        SerialFrameCodec codec;
        QStringList errors;
        QVERIFY(codec.feed(captured.left(32), &errors).isEmpty());
        QVERIFY(errors.isEmpty());
        const auto frames = codec.feed(captured.mid(32), &errors);
        QVERIFY(errors.isEmpty());
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames[0].type, SerialMessageType::HelloResponse);
        QCOMPARE(frames[0].sequence, quint32(0x00000ae3u));

        SerialHello hello;
        QString error;
        QVERIFY2(!decodeSerialHello(frames[0].payload, hello, &error),
                 qPrintable(error));
        QCOMPARE(hello.baudRate, quint32(500'000));
        QCOMPARE(hello.binCount, quint16(4096));
        QCOMPARE(hello.chunkBins, quint16(64));
        QCOMPARE(hello.firmwareVersion,quint32(0x00020500));
        QVERIFY(error.contains(QStringLiteral("固件版本")));
    }

    void serialStatusV2DecodesAllFieldsAndRejectsInvalidPayloads() {
        QByteArray payload(52, char(0));
        auto *p = reinterpret_cast<uchar *>(payload.data());
        p[0] = 2;
        p[1] = 0xff; // all status flags set
        p[2] = 1; p[3] = 4;
        qToBigEndian<quint32>(0x01020304u, p + 4);
        qToBigEndian<quint32>(0x11121314u, p + 8);
        qToBigEndian<quint32>(0x21222324u, p + 12);
        qToBigEndian<quint32>(0x31323334u, p + 16);
        qToBigEndian<quint32>(0x41424344u, p + 20);
        qToBigEndian<quint32>(0x51525354u, p + 24);
        qToBigEndian<quint32>(0x61626364u, p + 28);
        qToBigEndian<quint16>(12345, p + 32);
        qToBigEndian<quint16>(8192, p + 34);
        qToBigEndian<quint16>(32, p + 36);
        p[38] = 2;
        qToBigEndian<quint32>(0x71727374u, p + 40);
        p[44] = 0x01; p[45] = 0x02; p[46] = 0x03;
        p[47] = 0x04; p[48] = 0x05; p[49] = 0x06;
        SerialStatus status;
        QString error;
        QVERIFY(decodeSerialStatus(payload, status, &error));
        QCOMPARE(status.configVersion, quint32(0x01020304u));
        QCOMPARE(status.outputEvents, quint32(0x11121314u));
        QCOMPARE(status.rejectedEvents, quint32(0x21222324u));
        QCOMPARE(status.droppedEvents, quint32(0x31323334u));
        QCOMPARE(status.crcErrors, quint32(0x41424344u));
        QCOMPARE(status.frameErrors, quint32(0x51525354u));
        QCOMPARE(status.retries, quint32(0x61626364u));
        QCOMPARE(status.baseline, quint16(12345));
        QCOMPARE(status.manualBaseline, quint16(8192));
        QCOMPARE(status.recoveryTolerance, quint16(32));
        QCOMPARE(status.actualRange, quint8(2));
        QCOMPARE(status.externalTriggerCount, quint32(0x71727374u));
        QCOMPARE(status.timestamp, quint64(0x010203040506ull));
        QVERIFY(status.running && status.baselineReady && status.baselineAutomatic);
        QVERIFY(status.frontendInitDone && status.rangeBusy && status.filterEnabled);
        QVERIFY(status.externalArmed && status.externalWindowActive);

        QByteArray wrongLength = payload.left(51);
        QVERIFY(!decodeSerialStatus(wrongLength, status, &error));
        payload[0] = 1;
        QVERIFY(!decodeSerialStatus(payload, status, &error));
        payload[0] = 2;
        payload[38] = 3;
        QVERIFY(!decodeSerialStatus(payload, status, &error));
    }

    void serialHelloRequiresV2FeaturesAndFirmware() {
        QByteArray payload(20, char(0));
        auto *p = reinterpret_cast<uchar *>(payload.data());
        qToBigEndian<quint32>(SerialBaudRate, p);
        qToBigEndian<quint16>(4096, p + 4);
        p[6] = 2; p[7] = 12;
        qToBigEndian<quint16>(256, p + 8);
        qToBigEndian<quint32>(125'000'000u, p + 10);
        qToBigEndian<quint16>(0x003f, p + 14);
        qToBigEndian<quint32>(SerialFirmwareVersion, p + 16);
        SerialHello hello;
        QString error;
        QVERIFY2(decodeSerialHello(payload, hello, &error), qPrintable(error));
        p[15] = 0x1f;
        QVERIFY(!decodeSerialHello(payload, hello, &error));
        p[15] = 0x3f;
        qToBigEndian<quint32>(0x0002'0400u, p + 16);
        QVERIFY(!decodeSerialHello(payload, hello, &error));
    }

    void serialSpectrumPublishesOnlyCompleteEpochAndDeduplicates() {
        SerialSpectrumAssembler assembler;
        SerialSpectrumChunk chunk;
        chunk.session = 7; chunk.epoch = 9; chunk.totalBins = 4;
        chunk.binCount = 2; chunk.chunkTotal = 2;
        chunk.intervalTicks = 125'000'000; chunk.intervalEvents = 11;
        chunk.counts = {1, 2};
        QVERIFY(!assembler.add(chunk));
        QVERIFY(!assembler.add(chunk)); // duplicate must not double-apply
        chunk.chunkIndex = 1; chunk.startBin = 2; chunk.counts = {3, 4};
        QVERIFY(!assembler.add(chunk));
        chunk.kind = 1; chunk.chunkIndex = 0; chunk.startBin = 0;
        chunk.binShift = 12; chunk.counts = {5, 6};
        QVERIFY(!assembler.add(chunk));
        chunk.chunkIndex = 1; chunk.startBin = 2; chunk.counts = {7, 8};
        const auto epoch = assembler.add(chunk);
        QVERIFY(epoch.has_value());
        QCOMPARE(epoch->peak, QVector<quint32>({1, 2, 3, 4}));
        QCOMPARE(epoch->integral, QVector<quint32>({5, 6, 7, 8}));
        QCOMPARE(epoch->intervalEvents, quint32(11));
    }
};

QTEST_MAIN(CoreTests)
#include "test_core.moc"
