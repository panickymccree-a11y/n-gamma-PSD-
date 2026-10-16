#pragma once
#include "core/types.h"
#include <QByteArray>
#include <QString>

namespace mca {

constexpr quint32 DataMagic = 0x4d434132; // MCA2
constexpr int PacketHeaderBytes = 20;
constexpr int ChannelEventBytes = 12;
constexpr int ModuleEventBytes = 52;
constexpr int CoincidenceEventBytes = 112;
constexpr int ControlResponseBytes = 16;
constexpr int MaximumUdpPayloadBytes = 1472;
enum class PacketType : quint8 {
    ChannelEvents = 1,
    ModuleEvents = 2,
    CoincidenceEvents = 3,
    Status = 4,
    ControlResponse = 5,
    Waveform = 6,
    PsdEvents = 7
};

// ---------------------------------------------------------------------------
// Waveform mode.  These values are part of the wire protocol and must stay in
// step with rtl/mca_pkg.sv and docs/protocol.md.
// ---------------------------------------------------------------------------
constexpr int WaveformTotalSamples  = 12500;
constexpr int WaveformFragmentMax   = 512;
constexpr int WaveformFragmentCount = 25;
constexpr int WaveformLastSamples   =
    WaveformTotalSamples - (WaveformFragmentCount - 1) * WaveformFragmentMax;
constexpr int WaveformTriggerIndex   = WaveformPreSamples;   // array index of T
constexpr quint16 WaveformSessionBytes   = 4;
constexpr quint16 WaveformFragmentHeaderBytes = 32;
constexpr quint16 WaveformHeaderBytes = WaveformSessionBytes
                                        + WaveformFragmentHeaderBytes;
constexpr int WaveformMaximumPayloadBytes =
    WaveformHeaderBytes + WaveformFragmentMax * 2;   // 1060
// Relative time of sample i is (i - 3750) * 8 ns, so the record covers
// -30000 ns .. +69992 ns.
constexpr qint64 WaveformStartNs = -qint64(WaveformPreSamples)
                                   * WaveformSamplePeriodNs;
constexpr qint64 WaveformEndNs = qint64(WaveformTotalSamples - 1
                                        - WaveformPreSamples)
                                 * WaveformSamplePeriodNs;
// The WaveformState and WaveformTriggerMode enumerations live in core/types.h
// so that the data model has a single definition of both.

// One waveform fragment exactly as it appears on the wire.
struct WaveformFragment {
    quint32 sessionId = 0;
    quint32 captureId = 0;
    quint64 triggerTimestamp = 0;
    quint16 baseline = 0;
    quint16 threshold = 0;
    quint16 returnThreshold = 0;
    quint16 preSamples = 0;
    quint16 totalSamples = 0;
    quint16 sampleOffset = 0;
    quint16 sampleCount = 0;
    quint16 fragmentIndex = 0;
    quint16 fragmentCount = 0;
    quint16 flags = 0;
    quint16 samplePeriodNs = 0;
    QVector<quint16> samples;   // 14-bit ADC code | overrange in bit 14

    bool negativePolarity() const { return (flags & 0x0001u) != 0; }
    bool overrange(int index) const {
        return index >= 0 && index < samples.size()
            && (samples[index] & 0x4000u) != 0;
    }
    quint16 adcCode(int index) const {
        return index >= 0 && index < samples.size()
            ? quint16(samples[index] & 0x3fffu) : quint16(0);
    }
};

struct ControlResponse {
    quint32 transactionId = 0;
    quint8 opcode = 0;
    quint8 target = 0;
    quint8 targetId = 0;
    quint8 status = 0;
    quint16 registerAddress = 0;
    quint32 value = 0;
    bool accepted() const { return status == 0; }
};

struct DecodedPacket {
    quint8 version = 0;
    PacketType type = PacketType::ChannelEvents;
    quint8 deviceId = 0;
    quint8 flags = 0;
    quint32 sequence = 0;
    quint32 configVersion = 0;
    QVector<PsdEvent> psdEvents;
    QVector<ChannelEvent> channelEvents;
    QVector<ModuleEvent> moduleEvents;
    QVector<CoincidenceEvent> coincidenceEvents;
    QVector<ControlResponse> responses;
    QVector<WaveformFragment> waveformFragments;
    DeviceStatus status;
    bool hasStatus = false;

    qsizetype eventCount() const {
        return psdEvents.size() + channelEvents.size() + moduleEvents.size() + coincidenceEvents.size();
    }
};

class PacketDecoder {
public:
    static bool decode(const QByteArray &bytes, DecodedPacket &out,
                       QString *error = nullptr);
    static QByteArray encodeChannels(quint32 sequence, quint32 configVersion,
                                     quint8 deviceId,
                                     const QVector<ChannelEvent> &events,
                                     quint8 flags = 0);
    static QByteArray encodeModules(quint32 sequence, quint32 configVersion,
                                    quint8 deviceId,
                                    const QVector<ModuleEvent> &events,
                                    quint8 flags = 0);
    static QByteArray encodeCoincidences(
        quint32 sequence, quint32 configVersion, quint8 deviceId,
        const QVector<CoincidenceEvent> &events, quint8 flags = 0);
    // Encodes exactly one waveform fragment (MCA2 type 6).  The session id,
    // fragment header fields and sample words are taken from `fragment`.
    static QByteArray encodeWaveform(quint32 sequence, quint32 configVersion,
                                     quint8 deviceId,
                                     const WaveformFragment &fragment,
                                     quint8 flags = 0);
    // Kept as a convenient channel-event alias for existing analysis code.
    static QByteArray encode(quint32 sequence, quint8 deviceId,
                             const QVector<EventRecord> &events,
                             quint8 flags = 0) {
        return encodeChannels(sequence, 0, deviceId, events, flags);
    }
};

constexpr int ControlFrameBytes = 20;
constexpr quint32 ControlMagic = 0x4d434632; // MCF2

enum class ControlOpcode : quint8 {
    WriteShadow = 1,
    Read = 2,
    CommitAll = 3,
    Start = 4,
    Stop = 5,
    Abort = 6,
    ClearCounters = 7,
    SetRange = 9
};
enum class ControlTarget : quint8 {
    Global = 0, Adc = 1, Channel = 2, Module = 3, Pair = 4
};

// Global register numbers used by the waveform mode.
constexpr quint16 RegisterGlobalMode            = 0x0000;
constexpr quint16 RegisterGlobalBaselineTrack   = 0x0001;
constexpr quint16 RegisterGlobalRange           = 0x0005;
constexpr quint16 RegisterGlobalManualBaseline  = 0x0007;
constexpr quint16 RegisterGlobalWaveTriggerMode = 0x0008;
constexpr quint16 RegisterGlobalWaveSession     = 0x0009;
constexpr quint16 RegisterWaveStatus            = 0x0120;
constexpr quint16 RegisterWaveLastCapture       = 0x0121;
constexpr quint16 RegisterWaveCaptureCount      = 0x0122;
constexpr quint16 RegisterCapFormatFlags        = 0x010A;
constexpr quint16 RegisterCapTimebasePs         = 0x0110;
constexpr quint16 RegisterGlobalBandwidthEnable = 0x000A;
constexpr quint16 RegisterGlobalExternalDurationUs = 0x000B;
constexpr quint16 RegisterGlobalPeakInterpolation = 0x000C;
constexpr quint16 RegisterFrontendStatus        = 0x0123;
constexpr quint16 RegisterExternalStatus        = 0x0124;
constexpr quint16 RegisterExternalTriggerCount  = 0x0125;
constexpr quint16 RegisterChannelRecoveryTolerance = 0x001A;

// Acquisition mode values on the wire.  Waveform keeps a distinct number so
// it can never collide with an older host's mode 1 or 2.
constexpr quint32 ModeSpectrum = 0;
constexpr quint32 ModeWaveform = 3;
constexpr quint32 ModeExternalTrigger = 4;

// Format capability bits.
constexpr quint32 FormatFlagExtendedQuery   = 0x0000'0001u;
constexpr quint32 FormatFlagManualBaseline  = 0x0000'0002u;
constexpr quint32 FormatFlagExtendedStatus  = 0x0000'0004u;
constexpr quint32 FormatFlagPeakAbsolute    = 0x0000'0008u;
constexpr quint32 FormatFlagBaselineTlv16   = 0x0000'0010u;
constexpr quint32 FormatFlagWaveform        = 0x0000'0020u;
constexpr quint32 FormatFlagFrontendState   = 0x0000'0040u;
constexpr quint32 FormatFlagRecoveryTolerance = 0x0000'0080u;
constexpr quint32 FormatFlagExternalTrigger = 0x0000'0100u;
constexpr quint32 FormatFlagPeakInterpolation = 0x0000'0200u;
enum class ControlStatus : quint8 {
    Ok = 0, BadFrame = 1, BadOpcode = 2, BadTarget = 3,
    BadValue = 4, Busy = 5, NotReady = 6, ValidationFailed = 7,
    ConfigVersionMismatch = 8
};

QByteArray makeControlRequest(ControlOpcode opcode, ControlTarget target,
                              quint8 targetId, quint16 registerAddress,
                              quint16 flags, quint32 transactionId,
                              quint32 value);
QByteArray makeShadowWrite(ControlTarget target, quint8 targetId,
                           quint16 registerAddress, quint32 value,
                           quint32 transactionId);
QByteArray makeRead(ControlTarget target, quint8 targetId,
                    quint16 registerAddress, quint32 transactionId);
QByteArray makeCommitAll(quint32 transactionId);
QByteArray makeStart(quint32 configVersion, quint32 transactionId);
QByteArray makeStop(quint32 transactionId);
QByteArray makeAbort(quint32 transactionId);
QByteArray makeClearCounters(quint32 transactionId);
QByteArray makeSetRange(quint8 range, quint32 transactionId);
quint32 makeChannelControlWord(const ChannelConfig &config, bool enabled);

} // namespace mca
