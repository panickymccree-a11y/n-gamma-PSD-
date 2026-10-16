#pragma once

#include <QColor>
#include <QDateTime>
#include <QMetaType>
#include <QPointF>
#include <QString>
#include <QVector>
#include <array>
#include <cstdint>
#include <memory>
#include <variant>

namespace mca {

constexpr int MaxChannels = 32;
constexpr int DefaultPeakBins = 8192;
constexpr int DefaultIntegralBins = 16384;
constexpr quint64 MaxEventsPerRun = 20'000'000;

inline QString panelChannelName(int channel) {
    if (channel < 0 || channel >= MaxChannels)
        return QStringLiteral("ADC?-?");
    return QStringLiteral("ADC%1-%2")
        .arg(channel / 8 + 1)
        .arg(QChar(u'A' + channel % 8));
}

inline QString panelModuleName(int module) {
    if (module < 0 || module >= 8)
        return QStringLiteral("M?");
    const int adc = module / 2 + 1;
    const QChar first = (module & 1) ? QChar(u'E') : QChar(u'A');
    const QChar last  = (module & 1) ? QChar(u'H') : QChar(u'D');
    return QStringLiteral("M%1 (ADC%2 %3-%4)")
        .arg(module).arg(adc).arg(first).arg(last);
}

enum class Polarity : quint8 { Positive = 0, Negative = 1 };
enum class AcquisitionMode : quint8 {
    AutomaticTrigger = 0,
    LegacyFourChannel = 1,
    LegacyCoincidence = 2,
    Waveform = 3,
    ExternalTrigger = 4,
    Psd = 5,
    // Source compatibility for old list/replay code. These values are not
    // exposed by the live acquisition-mode selector.
    SingleChannel = AutomaticTrigger,
    FourChannel = LegacyFourChannel,
    Coincidence = LegacyCoincidence
};
enum class SpectrumKind : quint8 { Peak, Integral };
enum class AcquisitionState : quint8 { Idle, Starting, Running, Paused, Stopping, Error };
enum class SpectrumInteractionMode : quint8 { Cursor, ZoomRectangle, Pan, CreateRoi, EditRoi };
enum class SpectrumSourceType : quint8 { LiveDetector, Buffer, SpectrumFile, ListReplay };

struct ChannelEvent {
    quint64 timestamp = 0;
    quint16 peak = 0;
    quint32 integral = 0;
    quint8 channel = 0;
    bool pileup = false;
    bool overflow = false;
    bool polarity = false;
};
using EventRecord = ChannelEvent;
struct PsdEvent {
    ChannelEvent legacy;
    qint32 qtotal = 0, qtail = 0;
    quint16 baseline = 0;
    quint8 quality = 0;
    quint32 configVersion = 0;
};

struct ModuleEvent {
    quint32 eventId = 0;
    quint64 timestamp = 0;
    quint8 module = 0;
    quint8 flags = 0;
    std::array<qint16, 4> relativeTime{};
    std::array<quint32, 4> integral{};
    std::array<quint16, 4> peak{};
    std::array<quint8, 4> channelFlags{};
    quint16 x = 1024;
    quint16 y = 1024;
    quint32 energy = 0;
};

struct CoincidenceEvent {
    quint32 matchId = 0;
    quint8 pair = 0;
    quint8 flags = 0;
    qint16 layerTime = 0;
    ModuleEvent scatter;
    ModuleEvent absorber;
    bool ambiguous() const { return (flags & 0x01) != 0; }
};

using TypedEvent = std::variant<ChannelEvent, ModuleEvent, CoincidenceEvent>;

struct ChannelConfig {
  bool enabled = true;
  quint16 thresholdLsb = 64;
  Polarity polarity = Polarity::Positive;
  // The FPGA commit invariant is guard < pulse threshold < trigger
  // threshold.  These defaults mirror the single-channel firmware values.
  quint16 baselineGuardLsb = 16;
    quint16 baselinePulseThresholdLsb = 32;
    quint16 recoveryToleranceLsb = 32;
    // Match the FPGA's documented/default Q24 tracking rate.  A value of 20
    // makes a stable offset take milliseconds to settle and appears as a long
    // baseline excursion on the debug probe.
    quint8 baselineShift = 8;
    quint16 baselineHoldSamples = 64;
    quint16 integrationPreSamples = 0;
    qint16 psdTailStart = 7, psdTailEnd = 150;
    // MCA v2 uses this field as the total integration-window length.
    quint16 integrationPostSamples = 32;
    quint16 gainQ4_12 = 0x1000;
    QColor color;
};

struct ModuleConfig {
    bool enabled = false;
    quint16 windowSamples = 8;
    bool energyGateEnabled = false;
    quint32 energyMin = 0;
    quint32 energyMax = 0xffffffffu;
    quint8 qualityRejectMask = 0;
};

struct PairConfig {
    bool enabled = false;
    quint8 scatterModuleMask = 0x01;
    quint8 absorberModuleMask = 0x02;
    qint16 minimumTimeSamples = -8;
    qint16 maximumTimeSamples = 8;
    bool scatterEnergyGateEnabled = false;
    quint32 scatterEnergyMin = 0;
    quint32 scatterEnergyMax = 0xffffffffu;
    bool absorberEnergyGateEnabled = false;
    quint32 absorberEnergyMin = 0;
    quint32 absorberEnergyMax = 0xffffffffu;
    quint8 qualityRejectMask = 0;
};

struct DeviceCapabilities {
    quint8 protocolVersion = 2;
    quint8 adcCount = 4;
    quint8 maximumAdcCount = 4;
    quint8 channelCount = 32;
    quint8 maximumChannelCount = 32;
    quint8 moduleCount = 8;
    quint8 maximumModuleCount = 8;
    quint8 pairCount = 1;
    quint32 sampleRateHz = 80'000'000;
    quint32 firmwareVersion = 0x0002'0600;
    quint32 formatFlags = 0;
    quint8 adcBits = 12;
    quint8 peakFractionBits = 4;
    quint8 peakBinShift = 0;
    quint8 integralBinShift = 0;
    quint8 integralBits = 24;
    quint32 integrationWindowMinNs = 100;
    quint32 integrationWindowMaxNs = 10'000;
    quint32 timebasePs = 12'500;
    quint32 psdWindowMaxNs = 10000;
    // UDP status packets report channel counts before the extended
    // capability query returns format flags.  Keep that intermediate state
    // distinct from an authoritative formatFlags value of zero.
    bool formatFlagsValid = false;
    bool extendedQuery = false;
    bool supportsManualBaseline() const {
        return (formatFlags & 0x0000'0002u) != 0;
    }
    bool supportsExtendedStatus() const {
        return (formatFlags & 0x0000'0004u) != 0;
    }
    bool supportsPsd() const { return (formatFlags & 0x400u) != 0; }
    bool supportsWaveform() const {
        return (formatFlags & 0x0000'0020u) != 0;
    }
    bool supportsFrontendState() const {
        return (formatFlags & 0x0000'0040u) != 0;
    }
    bool supportsExternalTrigger() const {
        return (formatFlags & 0x0000'0100u) != 0;
    }
    bool supportsPeakInterpolation() const {
        return (formatFlags & 0x0000'0200u) != 0;
    }
    bool supportsRecoveryTolerance() const {
        return (formatFlags & 0x0000'0080u) != 0;
    }
};

// ---------------------------------------------------------------------------
// Waveform mode
// ---------------------------------------------------------------------------
constexpr int WaveformSampleCount   = 12500;
constexpr int WaveformPreSamples    = 3750;
constexpr int WaveformSamplePeriodNs = 8;
constexpr int WaveformAdcBits        = 14;
constexpr int WaveformAdcFullScale   = 16384;

enum class WaveformTriggerMode : quint32 { Continuous = 0, Single = 1 };
enum class WaveformState : quint32 {
    Stopped = 0,
    Filling = 1,
    Waiting = 2,
    Capturing = 3,
    Uploading = 4,
    SingleDone = 5
};

// One immutable, fully reassembled waveform record.  The UI only ever sees
// this through a shared_ptr so that a redraw can never observe a half-built
// record.
struct WaveformCapture {
    quint32 sessionId = 0;
    quint32 captureId = 0;
    quint32 deviceId = 0;
    quint32 configVersion = 0;
    quint64 triggerTimestamp = 0;
    quint16 baseline = 0;
    quint16 threshold = 0;
    quint16 returnThreshold = 0;
    quint16 preSamples = WaveformPreSamples;
    quint16 totalSamples = WaveformSampleCount;
    quint16 samplePeriodNs = WaveformSamplePeriodNs;
    bool negativePolarity = false;
    QVector<quint16> samples;          // raw 16-bit words, bit14 = overrange
    QDateTime receivedAt = QDateTime::currentDateTime();

    int size() const { return samples.size(); }
    int triggerIndex() const { return preSamples; }
    bool valid() const {
        return samples.size() == totalSamples
            && totalSamples > 0 && preSamples < totalSamples;
    }
    quint16 rawSample(int index) const { return samples.value(index); }
    quint16 adcCode(int index) const {
        return quint16(samples.value(index) & 0x3fffu);
    }
    bool overrange(int index) const {
        return (samples.value(index) & 0x4000u) != 0;
    }
    // Relative time of a sample index in nanoseconds, exactly
    // time_ns[i] = (i - preSamples) * samplePeriodNs.
    qint64 timeNs(int index) const {
        return qint64(index - preSamples) * qint64(samplePeriodNs);
    }
};

// Live waveform status as reported by the device, plus host-side reception
// accounting so the UI can distinguish "no data" from "data lost".
struct WaveformStatus {
    WaveformState state = WaveformState::Stopped;
    quint32 lastCaptureId = 0;
    quint32 captureCount = 0;
    quint32 sessionId = 0;
    quint32 triggerMode = 0;                 // 0 continuous, 1 single
    quint64 completedCaptures = 0;
    quint64 discardedCaptures = 0;
    quint64 droppedFragments = 0;
    bool receiveIncomplete = false;
    QString detail;
};

struct DeviceStatus {
    DeviceCapabilities capabilities;
    bool capabilitiesValid = false;
    AcquisitionMode mode = AcquisitionMode::SingleChannel;
    bool running = false;
    bool adcAligned = false;
    quint8 adcAlignmentMask = 0;
    quint32 configVersion = 0;
    quint64 timestamp = 0;
    quint32 baselineReadyMask = 0;
    quint32 baselineTrackingMask = 0;
    std::array<quint16, MaxChannels> baselineValues{};
    quint8 baselineValueCount = 0;
    quint32 baselineValueValidMask = 0;
    quint16 eventFifoCount = 0;
    quint32 outputEvents = 0;
    quint32 moduleUnmatched = 0;
    quint32 moduleRejected = 0;
    quint32 pairExpired = 0;
    quint32 ambiguous = 0;
    quint32 configurationErrors = 0;
    quint32 protocolErrors = 0;
    quint32 pairGateRejected = 0;
    quint32 pairCacheOverflow = 0;
    quint32 fifoOverflow = 0;
    quint32 outputBackpressure = 0;
    quint32 commandFifoOverflow = 0;
    std::array<quint8, MaxChannels> baselineStates{};
    bool baselineAutomatic = true;
    quint16 manualBaseline = 0;
    bool externalArmed = false;
    bool externalWindowActive = false;
    quint32 externalTriggerCount = 0;
    bool manualBaselineValid = false;
    quint32 deadTimeRejected = 0;
    quint32 droppedEvents = 0;
    bool eventActive = false;
    quint8 actualRange = 0;             // 0 unknown, 1=Q1/1 V, 2=Q2/5 V
    bool rangeBusy = false;
    bool filterEnabled = false;
    bool peakInterpolationEnabled = false;
    bool frontendInitDone = false;
    quint16 recoveryToleranceLsb = 0;
    bool frontendStateValid = false;
};

struct BaselineChannelStatus {
    quint8 channel = 0;
    quint8 state = 0;
    quint16 baseline = 0;
    bool baselineValid = false;
    bool trackingEnabled = false;
    bool ready = false;
};

struct BaselineSnapshot {
    quint32 sequence = 0;
    bool automatic = true;
    quint16 manualBaseline = 0;
    bool manualBaselineValid = false;
    QVector<BaselineChannelStatus> channels;
};

// Keep malformed or stale cross-thread configuration objects from
// programming values outside the single-channel firmware interface.
inline ChannelConfig normalizeChannelConfig(ChannelConfig config,
                                             bool automaticBaseline = false) {
    // The firmware requires three strictly ordered positive values.  A
    // trigger threshold below 3 cannot represent a valid guard/pulse pair,
    // so lift it to the smallest legal value before clamping the other two.
    const quint16 minimumThreshold = automaticBaseline ? 17 : 3;
    if (config.thresholdLsb < minimumThreshold)
        config.thresholdLsb = minimumThreshold;
    else if (config.thresholdLsb > 16383)
        config.thresholdLsb = 16383;

    const quint16 maxGuard = config.thresholdLsb - 2;
    if (config.baselineGuardLsb == 0)
        config.baselineGuardLsb = 16;
    if (config.baselineGuardLsb > maxGuard)
        config.baselineGuardLsb = maxGuard;

    const quint16 minimumPulse = automaticBaseline
        ? qMax<quint16>(16, config.baselineGuardLsb + 1)
        : quint16(config.baselineGuardLsb + 1);
    if (config.baselinePulseThresholdLsb < minimumPulse ||
        config.baselinePulseThresholdLsb <= config.baselineGuardLsb ||
        config.baselinePulseThresholdLsb >= config.thresholdLsb)
        config.baselinePulseThresholdLsb = minimumPulse;
    if (config.baselineShift == 0 || config.baselineShift > 27)
        config.baselineShift = 8;
    if (config.baselineHoldSamples == 0 || config.baselineHoldSamples > 4096)
        config.baselineHoldSamples = 64;
    if (config.recoveryToleranceLsb == 0 || config.recoveryToleranceLsb > 16383)
        config.recoveryToleranceLsb = 32;
    if (config.integrationPostSamples < 6)
        config.integrationPostSamples = 6;
    else if (config.integrationPostSamples > 2500)
        config.integrationPostSamples = 2500;
    if (config.integrationPreSamples >= config.integrationPostSamples)
        config.integrationPreSamples = config.integrationPostSamples - 1;
    return config;
}

struct AcquisitionPreset {
    bool useRealTime = false;
    double realTimeSeconds = 0;
    bool useLiveTime = false;
    double liveTimeSeconds = 0;
    bool useEventCount = false;
    quint64 eventCount = 0;
};

struct PacketStats {
    quint64 received = 0;
    quint64 lost = 0;
    quint64 duplicate = 0;
    quint64 reordered = 0;
    quint64 latePackets = 0;
    quint64 receiveTurns = 0;
    quint64 receiveNanoseconds = 0;
    quint64 maximumReceiveTurnNanoseconds = 0;
    quint64 malformed = 0;
    quint64 confirmedMissing = 0;
    quint64 pendingMissing = 0;
    quint64 duplicateEvents = 0;
    quint64 validEventCount = 0;
    quint64 datagramBytes = 0;
    quint64 eventDatagrams = 0;
    quint64 parseErrors = 0;
    quint32 receiveBufferBytes = 0;
    quint32 lastSequence = 0;
    bool hasSequence = false;
};

struct SpectrumSnapshot {
    quint64 version = 0;
    int channelCount = 8;
    QVector<QVector<quint64>> peak;
    QVector<QVector<quint64>> integral;
    QVector<QVector<quint64>> moduleEnergy;
    QVector<QPointF> modulePositions;
    QVector<QPointF> coincidenceEnergy;
    QVector<qint16> coincidenceTime;
    std::array<quint64, MaxChannels> channelEvents{};
    quint64 totalEvents = 0;
    quint32 sampleRateHz = 80'000'000;
    quint8 adcBits = 12;
    quint8 peakFractionBits = 4;
    quint8 peakBinShift = 0;
    quint8 integralBinShift = 0;
};

struct AcquisitionSnapshot {
    AcquisitionState state = AcquisitionState::Idle;
    quint64 totalEvents = 0;
    quint64 pileupEvents = 0;
    double pileupFraction = 0;
    bool includePileupInSpectrum = true;
    double realSeconds = 0;
    double liveSeconds = 0;
    bool liveTimeEstimated = true;
    bool complete = true;
    bool serialTransport = false;
    quint32 serialCrcErrors = 0;
    quint32 serialFrameErrors = 0;
    quint32 serialRetries = 0;
    PacketStats packets;
    double eventsPerSecond = 0;
    double packetsPerSecond = 0;
    double udpMegabitsPerSecond = 0;
    QString status;
    AcquisitionMode mode = AcquisitionMode::SingleChannel;
    DeviceCapabilities capabilities;
    quint32 configVersion = 0;
    bool eventTextEnabled = false;
    quint64 eventTextEvents = 0;
    QString eventTextPath;
    QString eventTextError;
    bool externalArmed = false;
    bool externalWindowActive = false;
    quint32 externalTriggerCount = 0;
    // v2 serial STATUS snapshot (kept in the acquisition snapshot so the UI
    // observes one coherent, validated hardware state alongside counters).
    bool baselineReady = false;
    bool baselineAutomatic = false;
    quint16 baseline = 0;
    quint16 manualBaseline = 0;
    quint16 recoveryTolerance = 0;
    quint8 actualRange = 0;
    bool rangeBusy = false;
    bool filterEnabled = false;
    bool peakInterpolationEnabled = false;
    bool frontendInitDone = false;
};

struct Roi {
    QString name;
    int left = 0;
    int right = 0;
    QColor color = QColor("#ffb000");
};

struct RoiStats {
    quint64 gross = 0;
    double background = 0;
    double net = 0;
    double centroid = 0;
    int maximumChannel = 0;
};

struct PeakResult {
    double centroid = 0;
    double sigma = 0;
    double fwhm = 0;
    double height = 0;
    double area = 0;
};

struct FitResult {
    bool valid = false;
    QString message;
    QVector<PeakResult> peaks;
    double background0 = 0;
    double background1 = 0;
    double reducedChiSquare = 0;
    QVector<QPointF> curve;
};

struct Calibration {
    enum class Order { Linear = 1, Quadratic = 2 };
    Order order = Order::Linear;
    QVector<double> coefficients{0.0, 1.0, 0.0};
    QString unit = QStringLiteral("keV");
    double apply(double channel) const {
        return coefficients.value(0) + coefficients.value(1) * channel
             + coefficients.value(2) * channel * channel;
    }
    bool isValid() const {
        if (order == Order::Quadratic) return true;
        return coefficients.value(1) > 0.0;
    }
};

struct SpectrumMetadata {
    QString title;
    QString sourceName;
    SpectrumSourceType sourceType = SpectrumSourceType::LiveDetector;
    int detectorId = -1;
    SpectrumKind kind = SpectrumKind::Peak;
    bool live = false;
    bool modified = false;
    double realTimeSeconds = 0.0;
    double liveTimeSeconds = 0.0;
    double deadTimePercent = 0.0;
    quint64 totalCounts = 0;
    double countRate = 0.0;
    QString filePath;
    QDateTime acquisitionStartTime;
};

} // namespace mca

Q_DECLARE_METATYPE(mca::ChannelEvent)
Q_DECLARE_METATYPE(mca::ModuleEvent)
Q_DECLARE_METATYPE(mca::CoincidenceEvent)
Q_DECLARE_METATYPE(mca::TypedEvent)
Q_DECLARE_METATYPE(mca::AcquisitionMode)
Q_DECLARE_METATYPE(mca::DeviceCapabilities)
Q_DECLARE_METATYPE(mca::DeviceStatus)
Q_DECLARE_METATYPE(mca::BaselineSnapshot)
Q_DECLARE_METATYPE(mca::AcquisitionSnapshot)
Q_DECLARE_METATYPE(mca::WaveformStatus)
Q_DECLARE_METATYPE(std::shared_ptr<const mca::SpectrumSnapshot>)
Q_DECLARE_METATYPE(std::shared_ptr<const mca::WaveformCapture>)

Q_DECLARE_METATYPE(QVector<mca::PsdEvent>)
