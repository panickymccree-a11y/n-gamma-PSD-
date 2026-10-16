#include "app/acquisition.h"
#include "core/eventtextwriter.h"
#include "core/listfile.h"
#include "core/spectrum.h"
#include "core/serialprotocol.h"
#include "core/waveform.h"
#include <QElapsedTimer>
#include <QDebug>
#include <QQueue>
#include <QRandomGenerator>
#include "core/sequencewindow.h"
#include <QMutexLocker>
#include <QTimer>
#include <QUdpSocket>
#include <QSerialPort>
#include <QtEndian>
#include <cmath>
#include <bit>
#include <memory>
#include <optional>
#include <utility>

namespace mca {
namespace {
constexpr int ControlRetryMilliseconds = 200;
constexpr int ControlMaximumAttempts = 10;
constexpr int SerialHelloRetryMilliseconds = 200;
constexpr int SerialHelloMaximumAttempts = 10;
constexpr int SerialFinalFlushTimeoutMilliseconds = 3000;
constexpr int FrontendCommitBusyRetryMilliseconds = 50;
constexpr int FrontendCommitBusyMaximumAttempts = 10;
// The waveform status is polled through the ordinary control queue.  If the
// queue is busy the poll is skipped instead of piling up read requests.
constexpr int WaveformPollMilliseconds = 100;
constexpr int WaveformUiMilliseconds = 100;
constexpr int MaximumDatagramsPerTurn = 256;
constexpr int DatagramTurnBudgetMilliseconds = 2;


QColor channelColor(int channel) {
    static const std::array<const char *, 8> colors{
        "#2563a6", "#c66a1b", "#2e7d5b", "#b44c5a",
        "#7656a8", "#2b7c83", "#a67c28", "#65758b"
    };
    return QColor(colors[size_t(channel) % colors.size()]);
}

QVector<ChannelEvent> moduleChannels(const ModuleEvent &module) {
    QVector<ChannelEvent> channels;
    channels.reserve(4);
    for (int i = 0; i < 4; ++i) {
        ChannelEvent event;
        event.timestamp = quint64(qint64(module.timestamp)
                          + module.relativeTime[size_t(i)]);
        event.peak = module.peak[size_t(i)];
        event.integral = module.integral[size_t(i)];
        const quint8 flags = module.channelFlags[size_t(i)];
        event.channel = flags >> 3;
        event.pileup = (flags & 0x04) != 0;
        event.overflow = (flags & 0x02) != 0;
        event.polarity = (flags & 0x01) != 0;
        channels.push_back(event);
    }
    return channels;
}
} // namespace

class AcquisitionWorker : public QObject {
    Q_OBJECT
    enum class SerialHelloAction : quint8 { StartRun, ApplyBaseline };
public:
    void setTypedEventNotificationsEnabled(bool enabled) {
        if (!enabled) flushTyped();
        m_typedNotifications=enabled;
    }
    explicit AcquisitionWorker(SerialPortFactory serialPortFactory = {})
        : m_serialPortFactory(std::move(serialPortFactory)) {
        // The built-in source retains a broad synthetic capability set so the
        // packet/list compatibility tests can exercise legacy Module/Pair
        // decoding.  The live UI never exposes those modes or editors.
        m_capabilities.adcCount = 4;
        m_capabilities.channelCount = 32;
        m_capabilities.moduleCount = 8;
        m_capabilities.pairCount = 1;
        resizeForCapabilities();
        m_generate = new QTimer(this);
        m_publish = new QTimer(this);
        m_controlRetry = new QTimer(this);
        m_startRepeatTimer = new QTimer(this);
        m_startRepeatTimer->setSingleShot(true);
        m_startRepeatTimer->setTimerType(Qt::PreciseTimer);
        m_startRepeatTimer->setInterval(100);
        connect(m_startRepeatTimer, &QTimer::timeout, this, [this] {
            if (m_startPhase == 1) {
                enqueue(ControlOpcode::Stop, ControlTarget::Global, 0, 0, 0);
                sendNextControl();
                return;
            }
            if (m_startPhase != 2)
                return;
            m_startPhase = 3;
            // Clear device histograms before the official run as UART sends
            // histogram deltas, including data accumulated during pre-start.
            enqueue(ControlOpcode::ClearCounters, ControlTarget::Global, 0, 0, 0);
            for (const auto &command : m_startConfiguration)
                enqueue(command.opcode, command.target, command.targetId,
                        command.registerAddress, command.value);
            enqueue(ControlOpcode::CommitAll, ControlTarget::Global, 0, 0, 0);
            sendNextControl();
        });
        m_serialHelloRetry = new QTimer(this);
        m_serialFinalFlush = new QTimer(this);
        m_waveformPoll = new QTimer(this);
        m_generate->setTimerType(Qt::PreciseTimer);
        m_generate->setInterval(4);
        m_publish->setTimerType(Qt::PreciseTimer);
        m_publish->setInterval(50);
        m_controlRetry->setInterval(ControlRetryMilliseconds);
        m_serialHelloRetry->setInterval(SerialHelloRetryMilliseconds);
        m_serialFinalFlush->setSingleShot(true);
        m_serialFinalFlush->setInterval(SerialFinalFlushTimeoutMilliseconds);
        m_waveformPoll->setInterval(WaveformPollMilliseconds);
        connect(m_generate, &QTimer::timeout, this, &AcquisitionWorker::generate);
        connect(m_publish, &QTimer::timeout, this, &AcquisitionWorker::publish);
        connect(m_controlRetry, &QTimer::timeout,
                this, &AcquisitionWorker::retryControl);
        connect(m_serialHelloRetry, &QTimer::timeout,
                this, &AcquisitionWorker::retrySerialHello);
        connect(m_serialFinalFlush, &QTimer::timeout,
                this, &AcquisitionWorker::serialFinalFlushTimeout);
        connect(m_waveformPoll, &QTimer::timeout,
                this, &AcquisitionWorker::pollWaveformStatus);
        connect(&m_textWriter, &EventTextWriter::progress,
                this, [this](quint64 total) {
                    m_state.eventTextEvents = total;
                });
        connect(&m_textWriter, &EventTextWriter::writeFailed,
                this, [this](const QString &message) {
                    m_state.eventTextError = message;
                    if (m_state.state != AcquisitionState::Error)
                        fail(message);
                });
    }

    bool ensureSerialPort() {
        if (m_serialPort)
            return true;
        m_serialPort = m_serialPortFactory
            ? m_serialPortFactory(this)
            : new QSerialPort(this);
        if (!m_serialPort)
            return false;
        if (m_serialPort->parent() != this)
            m_serialPort->setParent(this);
        connect(m_serialPort, &QSerialPort::readyRead,
                this, &AcquisitionWorker::readSerial);
        connect(m_serialPort, &QSerialPort::errorOccurred, this,
                [this](QSerialPort::SerialPortError error) {
            if (error != QSerialPort::NoError &&
                error != QSerialPort::TimeoutError &&
                m_state.state != AcquisitionState::Error)
                fail(QStringLiteral("串口错误：%1")
                         .arg(m_serialPort->errorString()));
        });
        return true;
    }

public slots:
    void shutdown() {
        m_generate->stop();
        m_publish->stop();
        m_waveformPoll->stop();
        m_serialHelloRetry->stop();
        m_serialFinalFlush->stop();
        abortControlOperation();
        m_textWriter.finish(false, QStringLiteral("application shutdown"));
        if (m_dataSocket)
            m_dataSocket->close();
        if (m_serialPort)
            m_serialPort->close();
    }

    void startSimulation(AcquisitionPreset preset, const QString &path) {
        Q_UNUSED(path);
        begin(preset, {});
        m_simulation = true;
        m_discardPrestartData = false;
        m_configVersion = 1;
        m_state.state = AcquisitionState::Running;
        m_state.status = QStringLiteral("采集中");
        m_generate->start();
    }

    void startUdp(quint16 port, QHostAddress device, quint16 controlPort,
                  AcquisitionPreset preset, const QString &path,
                  bool baselineTrackingEnabled,
                  quint16 recoveryTolerance,
                  bool bandwidthEnabled,
                  quint8 inputRange,
                  bool peakInterpolationEnabled) {
        if (controlBusy()) {
            emit errorOccurred(QStringLiteral("已有控制操作正在进行，请稍后再开始采集"));
            return;
        }
        begin(preset, path);
        m_simulation = false;
        m_serialMode = false;
        m_serialHelloAccepted = false;
        m_serialRunConfirmed = false;
        if (m_serialPort) m_serialPort->close();
        if (!ensureHardwareEndpoint(port, device, controlPort)) {
            fail(m_dataSocket->errorString());
            return;
        }
        m_baselineTracking = baselineTrackingEnabled;
        m_recoveryTolerance = qBound<quint16>(1, recoveryTolerance, 16383);
        m_bandwidthEnabled = bandwidthEnabled;
        m_peakInterpolationEnabled = peakInterpolationEnabled;
        m_state.peakInterpolationEnabled = peakInterpolationEnabled;
        m_frontendConfigRequested = inputRange != 0;
        m_inputRange = qBound<quint8>(1, inputRange, 2);
        if (m_mode == AcquisitionMode::ExternalTrigger)
            m_waveformPoll->start();
        configureHardware(ControlIntent::StartRun);
    }

    void startSerial(QString portName, AcquisitionPreset preset,
                     bool baselineTrackingEnabled,
                     quint16 recoveryTolerance,
                     bool bandwidthEnabled, quint8 inputRange,
                     bool peakInterpolationEnabled) {
        if (controlBusy()) {
            emit errorOccurred(QStringLiteral("已有控制操作正在进行，请稍后再开始采集"));
            return;
        }
        if (m_mode==AcquisitionMode::Psd) { emit errorOccurred(QStringLiteral("PSD 仅支持 UDP")); return; }
        begin(preset, {});
        m_simulation = false;
        m_serialMode = true;
        m_serialRunConfirmed = false;
        m_serialHelloAccepted = false;
        m_serialBaselineApplyPending = false;
        m_afterSerialHello = SerialHelloAction::StartRun;
        m_state.state = AcquisitionState::Starting;
        m_state.status = QStringLiteral("正在连接串口");
        m_state.serialTransport = true;
        m_serialCodec.clear();
        m_serialAssembler.clear();
        m_serialAwaitFinalFlush = false;
        if (m_dataSocket) m_dataSocket->close();
        if (!ensureSerialPort()) {
            fail(QStringLiteral("串口设备创建失败"));
            return;
        }
        if (m_serialPort->isOpen()) m_serialPort->close();
        m_serialPort->setPortName(portName);
        // Keep the open path tied to the protocol header's negotiated rate;
        // this comment also forces dependent host builds to recompile when the
        // fixed UART rate changes.
        m_serialPort->setBaudRate(SerialBaudRate);
        m_serialPort->setDataBits(QSerialPort::Data8);
        m_serialPort->setParity(QSerialPort::NoParity);
        m_serialPort->setStopBits(QSerialPort::OneStop);
        m_serialPort->setFlowControl(QSerialPort::NoFlowControl);
        if (!m_serialPort->open(QIODevice::ReadWrite)) {
            fail(QStringLiteral("无法打开串口%1：%2")
                     .arg(portName, m_serialPort->errorString()));
            return;
        }
        if (m_serialPort->baudRate() != SerialBaudRate) {
            fail(QStringLiteral("串口%1实际波特率为%2，要求%3")
                     .arg(portName)
                     .arg(m_serialPort->baudRate())
                     .arg(SerialBaudRate));
            return;
        }
        m_serialPort->clear(QSerialPort::AllDirections);
        qInfo() << "serial opened" << portName
                << "baud" << m_serialPort->baudRate()
                << "8N1 no-flow-control";
        m_baselineTracking = baselineTrackingEnabled;
        m_recoveryTolerance = qBound<quint16>(1, recoveryTolerance, 16383);
        m_bandwidthEnabled = bandwidthEnabled;
        m_peakInterpolationEnabled = peakInterpolationEnabled;
        m_state.peakInterpolationEnabled = peakInterpolationEnabled;
        m_frontendConfigRequested = inputRange != 0;
        m_inputRange = qBound<quint8>(1, inputRange, 2);
        m_serialHelloPending = true;
        // The first HELLO below counts as attempt 1.  This keeps the
        // ten-attempt budget and the two-second deadline inclusive of the
        // initial transmission (retries at 200..1800 ms, timeout at 2000 ms).
        m_serialHelloAttempts = 1;
        const QByteArray hello = SerialFrameCodec::encode(
            SerialMessageType::HelloRequest, ++m_serialTxSequence);
        if (m_serialPort->write(hello) != hello.size())
            fail(QStringLiteral("串口HELLO发送失败"));
        else
            m_serialHelloRetry->start();
    }

    void replay(QString path) {
        QVector<DecodedPacket> packets;
        QString error;
        if (!ListFileReader::readPackets(path, packets, &error)) {
            emit errorOccurred(error);
            return;
        }
        finishStop();
        m_accumulator->clear();
        m_state = {};
        m_state.includePileupInSpectrum = m_includePileupInSpectrum;
        m_state.state = AcquisitionState::Idle;
        for (const auto &packet : packets)
            processDecoded(packet, QByteArray(), false);
        m_state.state = AcquisitionState::Idle;
        m_state.status = QStringLiteral("已回放 %1 个类型化事件")
                             .arg(m_state.totalEvents);
        publish();
    }

    void pause() {
        if (m_state.state != AcquisitionState::Running)
            return;
        if (m_simulation) {
            m_generate->stop();
            m_state.state = AcquisitionState::Paused;
            m_state.status = QStringLiteral("已暂停");
            publish();
            return;
        }
        if (!beginControlOperation(ControlIntent::PauseRun)) {
            emit errorOccurred(QStringLiteral("已有控制操作正在进行，暂时不能暂停"));
            return;
        }
        m_state.state = AcquisitionState::Stopping;
        enqueue(ControlOpcode::Stop, ControlTarget::Global, 0, 0, 0);
        sendNextControl();
        publish();
    }

    void resume() {
        if (m_state.state != AcquisitionState::Paused)
            return;
        m_state.state = AcquisitionState::Running;
        m_state.status = QStringLiteral("采集中");
        if (m_simulation)
            m_generate->start();
        else if (!configureHardware(ControlIntent::ResumeRun)) {
            m_state.state = AcquisitionState::Paused;
            m_state.status = QStringLiteral("恢复失败：已有控制操作正在进行");
            emit errorOccurred(m_state.status);
        }
        publish();
    }

    void stop() {
        if (m_state.state == AcquisitionState::Idle)
            return;
        m_generate->stop();
        if (m_simulation) {
            finishStop();
            return;
        }
        if (m_serialMode && m_serialHelloPending) {
            m_serialHelloPending = false;
            m_serialHelloRetry->stop();
            clearControlQueue();
            m_serialRunConfirmed = false;
            if (m_serialPort) m_serialPort->close();
            finishStop();
            return;
        }
        abortControlOperation();
        if (!beginControlOperation(ControlIntent::StopRun))
            return;
        m_state.state = AcquisitionState::Stopping;
        // Stopping immediately forbids new triggers; a capture that is already
        // running still finishes and uploads, and the waveform status only
        // reports "stopped" once the buffer has been released.
        m_state.status = m_mode == AcquisitionMode::Waveform
            ? QStringLiteral("正在停止波形采集")
            : QStringLiteral("正在排空 FPGA 流水线");
        enqueue(ControlOpcode::Stop, ControlTarget::Global, 0, 0, 0);
        sendNextControl();
        publish();
    }

    void clear() {
        if (m_state.state == AcquisitionState::Starting ||
            m_state.state == AcquisitionState::Running)
            return;
        m_accumulator->clear();
        m_state.totalEvents = 0;
        m_state.pileupEvents = 0;
        m_state.pileupFraction = 0;
        if (!m_simulation &&
            ((m_serialMode && m_serialPort && m_serialPort->isOpen()) ||
             (!m_serialMode && m_controlPort))) {
            if (!beginControlOperation(ControlIntent::ClearCounters)) {
                emit errorOccurred(QStringLiteral("已有控制操作正在进行，暂时不能清除计数"));
                return;
            }
            enqueue(ControlOpcode::ClearCounters, ControlTarget::Global,
                    0, 0, 0);
            sendNextControl();
        }
        publish();
    }

    void setConfig(int channel, ChannelConfig config) {
        if (channel < 0 || channel >= m_channels.size())
            return;
        m_channels[channel] = normalizeChannelConfig(config);
    }

    void sendChannelControl(int channel, ChannelConfig config) {
        setConfig(channel, config);
        reconfigureIfConnected();
    }

    void sendThreshold(int channel, quint16 thresholdLsb) {
        if (channel < 0 || channel >= m_channels.size())
            return;
        m_channels[channel].thresholdLsb =
            qBound<quint16>(3, thresholdLsb, 4095);
        reconfigureIfConnected();
    }

    void applyAllThresholds(quint16 thresholdLsb) {
        const quint16 value =
            qBound<quint16>(3, thresholdLsb, 4095);
        for (auto &channel : m_channels)
            channel.thresholdLsb = value;
        reconfigureIfConnected();
    }

    void applyBaselineTracking(quint16 port, QHostAddress device,
                               quint16 controlPort, bool enabled,
                               quint16 manualBaseline,
                               quint16 recoveryTolerance,
                               bool bandwidthEnabled,
                               quint8 inputRange,
                               bool peakInterpolationEnabled) {
        if (m_state.state != AcquisitionState::Idle &&
            m_state.state != AcquisitionState::Error) {
            emit baselineTrackingApplyFinished(
                enabled, false, QStringLiteral("请先停止采集，再应用基线设置"));
            return;
        }
        if (controlBusy()) {
            emit baselineTrackingApplyFinished(
                enabled, false, QStringLiteral("已有控制操作正在进行，请稍后重试"));
            return;
        }
        if (!ensureHardwareEndpoint(port, device, controlPort)) {
            const QString message = QStringLiteral("无法绑定数据端口：%1")
                                        .arg(m_dataSocket->errorString());
            emit hardwareCommunicationChanged(false);
            emit baselineTrackingApplyFinished(enabled, false, message);
            return;
        }
        m_simulation = false;
        m_requestedBaselineTracking = enabled;
        m_requestedManualBaseline = qMin<quint16>(manualBaseline, 16383);
        m_requestedRecoveryTolerance =
            qBound<quint16>(1, recoveryTolerance, 16383);
        m_requestedBandwidthEnabled = bandwidthEnabled;
        m_requestedPeakInterpolation = peakInterpolationEnabled;
        m_frontendConfigRequested = inputRange != 0;
        m_requestedRange = qBound<quint8>(1, inputRange, 2);
        emit hardwareCommunicationChanged(false);
        if (!beginControlOperation(ControlIntent::ApplyBaseline)) {
            emit baselineTrackingApplyFinished(
                enabled, false, QStringLiteral("已有控制操作正在进行，请稍后重试"));
            return;
        }
        m_baselineValidationRecoveryAttempted = false;
        m_frontendCommitBusyRetries = 0;
        enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0, 1,
                enabled ? 1u : 0u);
        // The baseline-only command can be issued before the regular
        // capability scan has completed (for example when the user applies
        // the manual value from an idle, newly connected UI).  Do not let an
        // unknown capability cache silently drop a user-supplied value and
        // leave the FPGA at its reset default of 8192.  The current
        // single-channel protocol defines register 0x0007; if the value is
        // unchanged there is no need to add an extra command to the legacy
        // two-command idle sequence.
        if (m_capabilities.supportsManualBaseline() ||
            m_requestedManualBaseline != m_manualBaseline)
            enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0,
                    RegisterGlobalManualBaseline, m_requestedManualBaseline);
        if (m_capabilities.supportsFrontendState())
            enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0,
                    RegisterGlobalBandwidthEnable,
                    m_requestedBandwidthEnabled ? 1u : 0u);
        if (m_capabilities.supportsPeakInterpolation())
            enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0,
                    RegisterGlobalPeakInterpolation,
                    m_requestedPeakInterpolation ? 1u : 0u);
        if (m_capabilities.supportsRecoveryTolerance()) {
            for (int channel = 0; channel < m_channels.size(); ++channel)
                enqueue(ControlOpcode::WriteShadow, ControlTarget::Channel,
                        quint8(channel), RegisterChannelRecoveryTolerance,
                        m_requestedRecoveryTolerance);
        }
        if (m_frontendConfigRequested)
            enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0,
                    RegisterGlobalRange, m_requestedRange);
        enqueue(ControlOpcode::CommitAll, ControlTarget::Global, 0, 0, 0);
        sendNextControl();
    }

    void applyBaselineTrackingSerial(QString portName, bool enabled,
                                     quint16 manualBaseline,
                                     quint16 recoveryTolerance,
                                     bool bandwidthEnabled, quint8 inputRange,
                                     bool peakInterpolationEnabled) {
        if (m_state.state != AcquisitionState::Idle &&
            m_state.state != AcquisitionState::Error) {
            emit baselineTrackingApplyFinished(
                enabled, false, QStringLiteral("请先停止采集，再应用基线设置"));
            return;
        }
        if (controlBusy()) {
            emit baselineTrackingApplyFinished(
                enabled, false, QStringLiteral("已有控制操作正在进行，请稍后重试"));
            return;
        }
        m_simulation = false;
        m_serialMode = true;
        m_serialBaselineApplyPending = true;
        m_requestedBaselineTracking = enabled;
        m_requestedManualBaseline = qMin<quint16>(manualBaseline, 16383);
        m_requestedRecoveryTolerance = qBound<quint16>(1, recoveryTolerance, 16383);
        m_requestedBandwidthEnabled = bandwidthEnabled;
        m_requestedPeakInterpolation = peakInterpolationEnabled;
        m_frontendConfigRequested = true;
        m_requestedRange = qBound<quint8>(1, inputRange, 2);
        emit hardwareCommunicationChanged(false);
        if (!ensureSerialPort()) {
            fail(QStringLiteral("串口设备创建失败"));
            return;
        }
        const bool reopen = !m_serialPort->isOpen() ||
                            m_serialPort->portName() != portName;
        if (reopen) {
            if (m_serialPort->isOpen())
                m_serialPort->close();
            m_serialPort->setPortName(portName);
            m_serialPort->setBaudRate(SerialBaudRate);
            m_serialPort->setDataBits(QSerialPort::Data8);
            m_serialPort->setParity(QSerialPort::NoParity);
            m_serialPort->setStopBits(QSerialPort::OneStop);
            m_serialPort->setFlowControl(QSerialPort::NoFlowControl);
            if (!m_serialPort->open(QIODevice::ReadWrite)) {
                fail(QStringLiteral("无法打开串口%1：%2")
                         .arg(portName, m_serialPort->errorString()));
                return;
            }
            if (m_serialPort->baudRate() != SerialBaudRate) {
                fail(QStringLiteral("串口%1实际波特率为%2，要求%3")
                         .arg(portName)
                         .arg(m_serialPort->baudRate())
                         .arg(SerialBaudRate));
                return;
            }
            m_serialPort->clear(QSerialPort::AllDirections);
            qInfo() << "serial opened" << portName
                    << "baud" << m_serialPort->baudRate()
                    << "8N1 no-flow-control";
            m_serialHelloPending = true;
            m_serialHelloAccepted = false;
            m_afterSerialHello = SerialHelloAction::ApplyBaseline;
            // Count the immediate HELLO as the first attempt; the retry
            // timer therefore cannot extend the negotiated two-second
            // deadline by one extra interval.
            m_serialHelloAttempts = 1;
            const QByteArray hello = SerialFrameCodec::encode(
                SerialMessageType::HelloRequest, ++m_serialTxSequence);
            if (m_serialPort->write(hello) != hello.size()) {
                fail(QStringLiteral("串口HELLO发送失败"));
                return;
            }
            m_serialHelloRetry->start();
        } else if (!m_serialHelloPending && m_serialHelloAccepted) {
            beginBaselineControlOperation();
        } else {
            m_afterSerialHello = SerialHelloAction::ApplyBaseline;
        }
    }

    void setIncludePileup(bool include) {
        m_includePileupInSpectrum = include;
        m_state.includePileupInSpectrum = include;
        publish();
    }

    void setMode(AcquisitionMode mode) {
        if (m_state.state == AcquisitionState::Running) {
            emit errorOccurred(QStringLiteral("运行中不能切换模式，请先停止采集"));
            return;
        }
        m_mode = mode;
        m_state.mode = mode;
        publish();
    }

    void setExternalTriggerDurationUs(quint32 durationUs) {
        m_externalDurationUs = qBound<quint32>(1, durationUs, 1'000'000);
        publish();
    }

    void setModuleConfig(int module, ModuleConfig config) {
        if (module < 0 || module >= m_modules.size())
            return;
        if (config.windowSamples == 0)
            config.windowSamples = 8;
        m_modules[module] = config;
    }

    void applyModuleConfig(int module, ModuleConfig config) {
        setModuleConfig(module, config);
        reconfigureIfConnected();
    }

    void setPairConfig(int pair, PairConfig config) {
        if (pair < 0 || pair >= m_pairs.size())
            return;
        m_pairs[pair] = config;
    }

    void applyPairConfig(int pair, PairConfig config) {
        setPairConfig(pair, config);
        reconfigureIfConnected();
    }

    // -----------------------------------------------------------------
    // Waveform mode
    // -----------------------------------------------------------------
    void startWaveform(quint16 port, QHostAddress device, quint16 controlPort,
                       WaveformTriggerMode triggerMode,
                       ChannelConfig config) {
        if (controlBusy()) {
            emit errorOccurred(
                QStringLiteral("已有控制操作正在进行，请稍后再开始波形采集"));
            return;
        }
        beginWaveform(triggerMode, config);
        if (!ensureHardwareEndpoint(port, device, controlPort)) {
            fail(m_dataSocket->errorString());
            return;
        }
        m_simulation = false;
        if (!beginControlOperation(ControlIntent::StartWaveform))
            return;
        // A new start always uses a fresh session id, so fragments of the
        // previous session can never complete a new record.
        m_waveformReassembler.reset(m_waveformSession);
        m_waveformStatus = WaveformStatus{};
        m_waveformStatus.sessionId = m_waveformSession;
        m_waveformStatus.triggerMode = quint32(m_waveformTriggerMode);
        // Stop the old run, stage the shared waveform configuration, commit it
        // and only then start with the confirmed configuration version.  The
        // capability scan runs first so an old firmware that does not
        // advertise the waveform protocol is detected before anything is sent.
        enqueue(ControlOpcode::Stop, ControlTarget::Global, 0, 0, 0);
        if (m_capabilitiesScanned) {
            if (!m_capabilities.supportsWaveform()) {
                failControlOperation(QStringLiteral(
                    "该固件不支持波形模式（缺少能力位 bit5），"
                    "请升级 FPGA 固件"));
                return;
            }
            queueWaveformConfigurationWrites();
        } else {
            queueWaveformCapabilityReads();
        }
        m_waveformPoll->start();
        publish();
        sendNextControl();
    }

    void updateWaveformConfig(ChannelConfig config) {
        if (m_mode != AcquisitionMode::Waveform)
            return;
        m_waveformConfig = normalizeChannelConfig(config);
        if (m_simulation || !m_controlPort)
            return;
        if (controlBusy()) {
            emit errorOccurred(
                QStringLiteral("已有控制操作正在进行，本次波形配置未发送"));
            return;
        }
        if (!beginControlOperation(ControlIntent::ApplyWaveformConfig))
            return;
        // Running changes to the threshold or polarity follow the same
        // stop / commit / restart path as the spectrum mode.
        enqueue(ControlOpcode::Stop, ControlTarget::Global, 0, 0, 0);
        queueWaveformConfigurationWrites();
        sendNextControl();
        publish();
    }

    void setWaveformTriggerMode(WaveformTriggerMode mode) {
        if (m_mode == AcquisitionMode::Waveform && !m_simulation
            && m_state.state == AcquisitionState::Running) {
            emit errorOccurred(
                QStringLiteral("运行中不能切换触发方式，请先停止波形采集"));
            return;
        }
        m_waveformTriggerMode = mode;
        m_waveformStatus.triggerMode = quint32(mode);
        emit waveformStatusReady(m_waveformStatus);
    }

signals:
    void spectrumReady(std::shared_ptr<const SpectrumSnapshot>);
    void statusReady(const AcquisitionSnapshot &);
    void baselineReady(const BaselineSnapshot &);
    void capabilitiesReady(const DeviceCapabilities &);
    void deviceStatusReady(const DeviceStatus &);
    void typedEventsReady(const QVector<TypedEvent> &);
    void psdEventsReady(const QVector<PsdEvent> &);
    void controlBusyChanged(bool);
    void baselineTrackingApplyFinished(bool enabled, bool success,
                                       const QString &message);
    void hardwareCommunicationChanged(bool responsive);
    void errorOccurred(const QString &);
    void waveformReady(std::shared_ptr<const WaveformCapture>);
    void waveformStatusReady(const WaveformStatus &);

private:
    enum class ControlIntent {
        None,
        StartRun,
        ResumeRun,
        ReconfigureAndResume,
        ApplyConfiguration,
        ApplyBaseline,
        StartWaveform,
        ApplyWaveformConfig,
        PollWaveform,
        PauseRun,
        StopRun,
        ClearCounters
    };
    struct QueuedControl {
        QByteArray frame;
        quint32 transaction = 0;
        ControlOpcode opcode = ControlOpcode::Read;
        ControlTarget target = ControlTarget::Global;
        quint8 targetId = 0;
        quint16 registerAddress = 0;
        quint32 value = 0;
        int attempts = 0;
    };

    void resizeForCapabilities() {
        const int channelCount = qBound(1, int(m_capabilities.channelCount),
                                        MaxChannels);
        const int oldChannelCount = m_channels.size();
        m_channels.resize(channelCount);
        for (int i = oldChannelCount; i < channelCount; ++i)
            m_channels[i].color = channelColor(i);
        m_modules.resize(int(m_capabilities.moduleCount));
        m_pairs.resize(int(m_capabilities.pairCount));
        if (m_pairs.size() > 0 && m_modules.size() > 1 &&
            oldChannelCount == 0) {
            m_pairs[0].enabled = false;
            m_pairs[0].scatterModuleMask = 0x01;
            m_pairs[0].absorberModuleMask = 0x02;
        }
        // UDP event mode presents a fixed 8192-bin peak spectrum.  Serial
        // spectrum frames retain the bin count negotiated in HELLO because
        // their payload is already histogrammed by the FPGA.
        const int peakBins = m_serialMode
            ? (1 << qMin<int>(m_capabilities.adcBits, 16))
            : DefaultPeakBins;
        m_accumulator = std::make_unique<SpectrumAccumulator>(
            channelCount, peakBins, m_capabilities.peakFractionBits,
            m_capabilities.sampleRateHz);
        m_state.capabilities = m_capabilities;
    }

    bool ensureHardwareEndpoint(quint16 port, const QHostAddress &device,
                                quint16 controlPort) {
        m_device = device;
        m_controlPort = controlPort;
        if (!m_dataSocket) {
            m_dataSocket = new QUdpSocket(this);
            connect(m_dataSocket, &QUdpSocket::readyRead,
                    this, &AcquisitionWorker::readDatagrams);
        }
        if (m_dataSocket->state() != QAbstractSocket::BoundState ||
            m_dataSocket->localPort() != port) {
            m_dataSocket->close();
            if (!m_dataSocket->bind(QHostAddress::AnyIPv4, port,
                                    QUdpSocket::DontShareAddress))
                return false;
        }
        // The native socket must exist before setting SO_RCVBUF. Closing and
        // rebinding creates a new descriptor, so apply and read back on every bind.
        m_dataSocket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption,
                                     8 * 1024 * 1024);
        m_state.packets.receiveBufferBytes=m_dataSocket->socketOption(
            QAbstractSocket::ReceiveBufferSizeSocketOption).toUInt();
        m_dataPort = port;
        return true;
    }

    bool controlBusy() const {
        return m_controlIntent != ControlIntent::None ||
               m_pendingControl.has_value() || !m_controlQueue.isEmpty();
    }

    static const char *intentName(ControlIntent intent) {
        switch (intent) {
        case ControlIntent::None: return "none";
        case ControlIntent::StartRun: return "start-run";
        case ControlIntent::ResumeRun: return "resume-run";
        case ControlIntent::ReconfigureAndResume: return "reconfigure-resume";
        case ControlIntent::ApplyConfiguration: return "apply-configuration";
        case ControlIntent::ApplyBaseline: return "apply-baseline";
        case ControlIntent::StartWaveform: return "start-waveform";
        case ControlIntent::ApplyWaveformConfig: return "apply-waveform-config";
        case ControlIntent::PollWaveform: return "poll-waveform";
        case ControlIntent::PauseRun: return "pause-run";
        case ControlIntent::StopRun: return "stop-run";
        case ControlIntent::ClearCounters: return "clear-counters";
        }
        return "unknown";
    }

    void clearControlQueue() {
        m_startRepeatTimer->stop();
        m_startConfiguration.clear();
        m_startConfigurationRepeated = false;
        m_startPhase = 0;
        m_controlRetry->stop();
        m_controlQueue.clear();
        m_pendingControl.reset();
    }

    bool beginControlOperation(ControlIntent intent) {
        if (controlBusy())
            return false;
        m_controlIntent = intent;
        qInfo().nospace() << "MCF2 begin intent=" << intentName(intent)
                          << " state=" << int(m_state.state);
        // Periodic waveform reads are background telemetry, not a user
        // control operation.  Publishing them as busy every 100 ms disabled
        // and re-enabled the start/configuration buttons on every poll.
        if (intent != ControlIntent::PollWaveform)
            emit controlBusyChanged(true);
        return true;
    }

    void completeControlOperation() {
        const auto completedIntent = m_controlIntent;
        clearControlQueue();
        m_controlIntent = ControlIntent::None;
        if (completedIntent == ControlIntent::PollWaveform)
            m_waveformPollPending = false;
        if (completedIntent != ControlIntent::None) {
            qInfo().nospace() << "MCF2 complete intent="
                              << intentName(completedIntent)
                              << " state=" << int(m_state.state);
            if (completedIntent != ControlIntent::PollWaveform)
                emit controlBusyChanged(false);
        }
    }

    void abortControlOperation() {
        const bool wasBusy = controlBusy();
        const auto abortedIntent = m_controlIntent;
        clearControlQueue();
        m_controlIntent = ControlIntent::None;
        if (wasBusy) {
            qInfo().nospace() << "MCF2 abort intent="
                              << intentName(abortedIntent);
            if (abortedIntent != ControlIntent::PollWaveform)
                emit controlBusyChanged(false);
        }
    }

    void failControlOperation(const QString &message) {
        const auto failedIntent = m_controlIntent;
        if (failedIntent == ControlIntent::ApplyBaseline) {
            const bool requested = m_requestedBaselineTracking;
            m_serialBaselineApplyPending = false;
            abortControlOperation();
            emit hardwareCommunicationChanged(false);
            emit baselineTrackingApplyFinished(requested, false, message);
            emit errorOccurred(message);
            return;
        }
        abortControlOperation();
        fail(message);
    }

    quint32 nextTransaction() {
        if (++m_nextTransaction == 0)
            ++m_nextTransaction;
        return m_nextTransaction;
    }

    void enqueue(ControlOpcode opcode, ControlTarget target, quint8 targetId,
                 quint16 registerAddress, quint32 value) {
        QueuedControl command;
        command.transaction = nextTransaction();
        command.opcode = opcode;
        command.target = target;
        command.targetId = targetId;
        command.registerAddress = registerAddress;
        command.value = value;
        command.frame = makeControlRequest(opcode, target, targetId,
                                           registerAddress, 0,
                                           command.transaction, value);
        m_controlQueue.enqueue(command);
        if (!m_startConfigurationRepeated &&
            (m_controlIntent == ControlIntent::StartRun ||
             m_controlIntent == ControlIntent::StartWaveform) &&
            opcode == ControlOpcode::WriteShadow)
            m_startConfiguration.push_back(command);
    }

    void sendNextControl() {
        if (m_simulation || m_pendingControl || m_controlQueue.isEmpty())
            return;
        if (m_serialMode) {
            if (!m_serialPort || !m_serialPort->isOpen() || m_serialHelloPending)
                return;
        } else if (!m_dataSocket || !m_controlPort) {
            return;
        }
        m_pendingControl = m_controlQueue.dequeue();
        transmitPendingControl();
    }

    void transmitPendingControl() {
        if (!m_pendingControl)
            return;
        auto &command = *m_pendingControl;
        ++command.attempts;
        qInfo().nospace() << "MCF2 tx intent=" << intentName(m_controlIntent)
                          << " opcode=" << int(command.opcode)
                          << " transaction=" << command.transaction
                          << " attempt=" << command.attempts;
        // The FPGA always sends MCA2 events and MCF2 responses to DATA_PORT.
        // Send requests through the same bound socket so their UDP source
        // port is also DATA_PORT. Besides matching the FPGA's fixed-port
        // design, this prevents host firewalls from treating responses as
        // unrelated inbound traffic to port 9000.
        QByteArray wire = command.frame;
        qint64 written = -1;
        QString transportError;
        if (m_serialMode) {
            wire = SerialFrameCodec::encode(SerialMessageType::ControlRequest,
                                             ++m_serialTxSequence,
                                             command.frame);
            written = m_serialPort->write(wire);
            transportError = m_serialPort->errorString();
        } else {
            written = m_dataSocket->writeDatagram(command.frame, m_device,
                                                   m_controlPort);
            transportError = m_dataSocket->errorString();
        }
        if (written != wire.size()) {
            if (m_serialMode) {
                failControlOperation(QStringLiteral(
                    "串口%1控制事务%2发送失败：register=0x%3：%4")
                    .arg(m_serialPort ? m_serialPort->portName() : QString())
                    .arg(command.transaction)
                    .arg(command.registerAddress, 4, 16, QLatin1Char('0'))
                    .arg(transportError));
            } else {
                failControlOperation(QStringLiteral(
                    "控制命令发送失败：%1；本地端口 %2，FPGA %3:%4")
                    .arg(transportError)
                    .arg(m_dataPort)
                    .arg(m_device.toString())
                    .arg(m_controlPort));
            }
            return;
        }
        m_controlRetry->start();
    }

    void retryControl() {
        if (!m_pendingControl) {
            m_controlRetry->stop();
            return;
        }
        if (m_pendingControl->attempts >= ControlMaximumAttempts) {
            const auto command = *m_pendingControl;
            if (m_serialMode) {
                failControlOperation(QStringLiteral(
                    "串口%1控制事务%2超时：opcode=%3，register=0x%4，"
                    "已重试%5次")
                    .arg(m_serialPort ? m_serialPort->portName() : QString())
                    .arg(command.transaction)
                    .arg(static_cast<int>(command.opcode))
                    .arg(command.registerAddress, 4, 16, QLatin1Char('0'))
                    .arg(ControlMaximumAttempts));
            } else {
                failControlOperation(QStringLiteral(
                    "控制事务 %1 超时：opcode=%2，target=%3，id=%4，"
                    "register=0x%5；本地端口 %6 → FPGA %7:%8，"
                    "已使用同一事务号重试 %9 次")
                    .arg(command.transaction)
                    .arg(static_cast<int>(command.opcode))
                    .arg(static_cast<int>(command.target))
                    .arg(static_cast<int>(command.targetId))
                    .arg(command.registerAddress, 4, 16, QLatin1Char('0'))
                    .arg(m_dataPort)
                    .arg(m_device.toString())
                    .arg(m_controlPort)
                    .arg(ControlMaximumAttempts));
            }
            return;
        }
        transmitPendingControl();
    }

    void queueCapabilityReads() {
        m_capabilities.formatFlagsValid = false;
        m_capabilities.extendedQuery = false;
        for (quint16 address = 0x0100; address <= 0x0110; ++address)
            enqueue(ControlOpcode::Read, ControlTarget::Global, 0, address, 0);
    }

    void queueConfigurationWrites() {
        enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0, 0,
                quint32(m_mode));
        if (m_mode==AcquisitionMode::Psd && !m_capabilities.supportsPsd()) {
            failControlOperation(QStringLiteral("当前固件不支持 PSD，请升级固件")); return;
        }
        if (m_mode==AcquisitionMode::Psd)
            enqueue(ControlOpcode::Read, ControlTarget::Global,0,0x0111,0);
        if (m_mode == AcquisitionMode::ExternalTrigger)
            enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0,
                    RegisterGlobalExternalDurationUs, m_externalDurationUs);
        enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0, 1,
                m_baselineTracking ? 1u : 0u);
        if (m_capabilities.supportsFrontendState())
            enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0,
                    RegisterGlobalBandwidthEnable,
                    m_bandwidthEnabled ? 1u : 0u);
        if (m_capabilities.supportsPeakInterpolation())
            enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0,
                    RegisterGlobalPeakInterpolation,
                    m_peakInterpolationEnabled ? 1u : 0u);
        if (m_capabilities.supportsManualBaseline())
            enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0,
                    0x0007, m_manualBaseline);
        if (m_frontendConfigRequested)
            enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0,
                    RegisterGlobalRange, m_inputRange);

        for (int i = 0; i < m_channels.size(); ++i) {
            const auto config = normalizeChannelConfig(m_channels[i],
                                                       m_baselineTracking);
            const auto write = [&](quint16 reg, quint32 value) {
                enqueue(ControlOpcode::WriteShadow, ControlTarget::Channel,
                        quint8(i), reg, value);
            };
            write(0, makeChannelControlWord(config, config.enabled));
            write(m_capabilities.extendedQuery ? 0x0010 : 0x0001,
                  config.thresholdLsb);
            // FPGA requires the return threshold (register 2) to remain
            // strictly below the trigger threshold. Preserve the historical
            // value 32 for normal thresholds and lower it only when needed.
            write(m_capabilities.extendedQuery ? 0x0011 : 0x0002,
                  qMin<quint16>(
                         32, quint16(config.thresholdLsb - 1)));
            write(m_capabilities.extendedQuery ? 0x0012 : 0x0003,
                  config.baselineGuardLsb);
            write(5, config.baselineShift);
            if (m_capabilities.extendedQuery) {
                write(0x0014, config.integrationPostSamples);
                write(0x0015, config.integrationPreSamples);
                if (m_mode==AcquisitionMode::Psd) {
                    write(0x001B,quint32(qint32(config.psdTailStart)));
                    write(0x001C,quint32(qint32(config.psdTailEnd)));
                }
                write(0x0013, config.baselinePulseThresholdLsb);
            } else {
                write(8, quint32(config.integrationPreSamples)
                       | (quint32(config.integrationPostSamples) << 16));
                write(10, config.baselinePulseThresholdLsb);
            }
            write(11, config.baselineHoldSamples);
            if (m_capabilities.supportsRecoveryTolerance())
                write(RegisterChannelRecoveryTolerance,
                     config.recoveryToleranceLsb);
            // The single-channel FPGA channel map has no gain register at
            // 0x000C.  Keep gain in the host model/list files for future
            // firmware, but do not send this unsupported write: the FPGA
            // correctly reports RESP_BAD_VALUE (status 4), which otherwise
            // makes a threshold or baseline operation appear to fail.
        }
        if (m_mode == AcquisitionMode::LegacyFourChannel ||
            m_mode == AcquisitionMode::LegacyCoincidence)
        for (int i = 0; i < m_modules.size(); ++i) {
            const auto &config = m_modules[i];
            const auto write = [&](quint16 reg, quint32 value) {
                enqueue(ControlOpcode::WriteShadow, ControlTarget::Module,
                        quint8(i), reg, value);
            };
            write(0, config.enabled ? 1u : 0u);
            write(1, config.windowSamples);
            write(2, config.energyGateEnabled ? 1u : 0u);
            write(3, config.energyMin);
            write(4, config.energyMax);
            write(5, config.qualityRejectMask);
        }
        if ((m_mode == AcquisitionMode::LegacyCoincidence) &&
            !m_pairs.isEmpty()) {
            const auto &config = m_pairs[0];
            const auto write = [&](quint16 reg, quint32 value) {
                enqueue(ControlOpcode::WriteShadow, ControlTarget::Pair,
                        0, reg, value);
            };
            write(0, (config.enabled ? 1u : 0u)
                   | (quint32(config.scatterModuleMask) << 8)
                   | (quint32(config.absorberModuleMask) << 16));
            write(1, quint16(config.minimumTimeSamples)
                   | (quint32(quint16(config.maximumTimeSamples)) << 16));
            write(2, config.scatterEnergyGateEnabled ? 1u : 0u);
            write(3, config.scatterEnergyMin);
            write(4, config.scatterEnergyMax);
            write(5, config.absorberEnergyGateEnabled ? 1u : 0u);
            write(6, config.absorberEnergyMin);
            write(7, config.absorberEnergyMax);
            write(8, config.qualityRejectMask);
        }
        enqueue(ControlOpcode::CommitAll, ControlTarget::Global, 0, 0, 0);
    }

    bool configureHardware(ControlIntent intent) {
        if (!controlTransportReady())
            return false;
        if (!beginControlOperation(intent))
            return false;
        enqueue(ControlOpcode::Stop, ControlTarget::Global, 0, 0, 0);
        queueCapabilityReads();
        sendNextControl();
        return true;
    }

    void reconfigureIfConnected() {
        if (m_simulation || !controlTransportReady())
            return;
        if (controlBusy()) {
            emit errorOccurred(QStringLiteral("已有控制操作正在进行，本次配置未发送"));
            return;
        }
        const bool restart = m_state.state == AcquisitionState::Running;
        if (restart)
            m_state.status = QStringLiteral("停止并原子提交新配置");
        configureHardware(restart ? ControlIntent::ReconfigureAndResume
                                  : ControlIntent::ApplyConfiguration);
    }

    bool controlTransportReady() const {
        if (m_serialMode)
            return m_serialPort && m_serialPort->isOpen() &&
                   !m_serialHelloPending && m_serialHelloAccepted;
        return m_dataSocket && m_controlPort;
    }

    // -----------------------------------------------------------------
    // Waveform helpers
    // -----------------------------------------------------------------
    void beginWaveform(WaveformTriggerMode triggerMode,
                       const ChannelConfig &config) {
        m_textWriter.finish(false, QStringLiteral("waveform acquisition started"));
        m_generate->stop();
        m_waveformPoll->stop();
        abortControlOperation();
        m_accumulator->clear();
        m_preset = {};
        m_pendingTextPath.clear();
        m_mode = AcquisitionMode::Waveform;
        m_waveformConfig = normalizeChannelConfig(config);
        m_waveformTriggerMode = triggerMode;
        ++m_waveformSession;
        if (m_waveformSession == 0)
            ++m_waveformSession;
        m_state = {};
        m_state.mode = AcquisitionMode::Waveform;
        m_state.capabilities = m_capabilities;
        m_state.state = AcquisitionState::Running;
        m_state.status = QStringLiteral("波形模式：正在提交配置");
        m_state.includePileupInSpectrum = m_includePileupInSpectrum;
        m_state.configVersion = m_configVersion;
        m_elapsed.restart();
        m_rateElapsed.restart();
        m_lastRateEvents = 0;
        m_lastRatePackets = 0;
        m_sequence = 0;
        resetSequenceWindow();
        m_serialCodec.clear();
        m_serialAssembler.clear();
        m_waveformStatus = WaveformStatus{};
        m_waveformStatus.sessionId = m_waveformSession;
        m_waveformStatus.triggerMode = quint32(triggerMode);
        m_waveformLastPoll.restart();
        m_waveformUiElapsed.restart();
        m_pendingWaveform.reset();
        m_publish->start();
        emit waveformStatusReady(m_waveformStatus);
    }

    void queueWaveformConfigurationWrites() {
        // Shared threshold, polarity and baseline configuration.
        const auto config = normalizeChannelConfig(m_waveformConfig,
                                                   m_baselineTracking);
        enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0,
                RegisterGlobalMode, ModeWaveform);
        enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0,
                RegisterGlobalBaselineTrack, m_baselineTracking ? 1u : 0u);
        if (m_capabilities.supportsFrontendState())
            enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0,
                    RegisterGlobalBandwidthEnable,
                    m_bandwidthEnabled ? 1u : 0u);
        if (m_capabilities.supportsPeakInterpolation())
            enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0,
                    RegisterGlobalPeakInterpolation,
                    m_peakInterpolationEnabled ? 1u : 0u);
        if (m_capabilities.supportsManualBaseline()) {
            enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0,
                    RegisterGlobalManualBaseline, m_manualBaseline);
        }
        // The session id is updated on every new start.
        enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0,
                RegisterGlobalWaveSession, m_waveformSession);
        enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0,
                RegisterGlobalWaveTriggerMode,
                m_waveformTriggerMode == WaveformTriggerMode::Single ? 1u : 0u);

        const auto write = [&](quint16 reg, quint32 value) {
            enqueue(ControlOpcode::WriteShadow, ControlTarget::Channel, 0,
                    reg, value);
        };
        // bit0 enable, bit1 negative polarity: both bits now really reach the
        // acquisition and waveform comparators.
        write(0x0000, makeChannelControlWord(config, config.enabled));
        write(m_capabilities.extendedQuery ? 0x0010 : 0x0001,
              config.thresholdLsb);
        write(m_capabilities.extendedQuery ? 0x0011 : 0x0002,
              qMin<quint16>(32, quint16(config.thresholdLsb - 1)));
        write(m_capabilities.extendedQuery ? 0x0012 : 0x0003,
              config.baselineGuardLsb);
        write(5, config.baselineShift);
        if (m_capabilities.extendedQuery) {
            write(0x0014, config.integrationPostSamples);
            write(0x0015, config.integrationPreSamples);
                if (m_mode==AcquisitionMode::Psd) {
                    write(0x001B,quint32(qint32(config.psdTailStart)));
                    write(0x001C,quint32(qint32(config.psdTailEnd)));
                }
            write(0x0013, config.baselinePulseThresholdLsb);
        } else {
            write(8, quint32(config.integrationPreSamples)
                   | (quint32(config.integrationPostSamples) << 16));
            write(10, config.baselinePulseThresholdLsb);
        }
        write(11, config.baselineHoldSamples);
        if (m_capabilities.supportsRecoveryTolerance())
            write(RegisterChannelRecoveryTolerance,
                 config.recoveryToleranceLsb);
        enqueue(ControlOpcode::CommitAll, ControlTarget::Global, 0, 0, 0);
    }

    void queueWaveformCapabilityReads() {
        // The capability scan only has to happen on the first connection;
        // afterwards the cached scan can be reused.
        if (m_capabilitiesScanned)
            return;
        m_capabilities.formatFlagsValid = false;
        m_capabilities.extendedQuery = false;
        for (quint16 address = 0x0100; address <= 0x0110; ++address)
            enqueue(ControlOpcode::Read, ControlTarget::Global, 0, address, 0);
        m_capabilitiesScanned = true;
    }

    void pollWaveformStatus() {
        if (m_simulation || !m_controlPort || !m_dataSocket)
            return;
        if (m_mode != AcquisitionMode::Waveform &&
            m_mode != AcquisitionMode::ExternalTrigger) {
            m_waveformPoll->stop();
            return;
        }
        // Incomplete records are expired even while the control queue is
        // busy, so a lost fragment can never keep the UI waiting.
        if (m_waveformReassembler.expire() > 0) {
            m_waveformStatus.receiveIncomplete = true;
            if (m_state.state == AcquisitionState::Running)
                emit errorOccurred(
                    QStringLiteral("单次波形接收不完整，请重新采集"));
            publishWaveformStatus();
        }
        // If a normal control operation is in progress the poll is skipped
        // instead of queueing another read request.
        if (controlBusy())
            return;
        if (!beginControlOperation(ControlIntent::PollWaveform))
            return;
        m_waveformPollPending = true;
        if (m_mode == AcquisitionMode::ExternalTrigger) {
            enqueue(ControlOpcode::Read, ControlTarget::Global, 0,
                    RegisterExternalStatus, 0);
            enqueue(ControlOpcode::Read, ControlTarget::Global, 0,
                    RegisterExternalTriggerCount, 0);
        } else {
            enqueue(ControlOpcode::Read, ControlTarget::Global, 0,
                    RegisterWaveStatus, 0);
            enqueue(ControlOpcode::Read, ControlTarget::Global, 0,
                    RegisterWaveLastCapture, 0);
            enqueue(ControlOpcode::Read, ControlTarget::Global, 0,
                    RegisterWaveCaptureCount, 0);
        }
        sendNextControl();
    }

    void publishWaveformStatus() {
        emit waveformStatusReady(m_waveformStatus);
    }

    void retrySerialHello() {
        if (!m_serialHelloPending || !m_serialPort ||
            !m_serialPort->isOpen()) {
            m_serialHelloRetry->stop();
            return;
        }
        if (m_serialHelloAttempts >= SerialHelloMaximumAttempts) {
            m_serialHelloRetry->stop();
            fail(QStringLiteral("串口HELLO超时：%1次、%2毫秒内未收到FPGA响应")
                     .arg(SerialHelloMaximumAttempts)
                     .arg(SerialHelloMaximumAttempts *
                          SerialHelloRetryMilliseconds));
            return;
        }
        const QByteArray hello = SerialFrameCodec::encode(
            SerialMessageType::HelloRequest, ++m_serialTxSequence);
        ++m_serialHelloAttempts;
        if (m_serialPort->write(hello) != hello.size())
            fail(QStringLiteral("串口HELLO重发失败：%1")
                     .arg(m_serialPort->errorString()));
    }

    void serialFinalFlushTimeout() {
        if (!m_serialAwaitFinalFlush)
            return;
        m_serialAwaitFinalFlush = false;
        m_serialAssembler.clear();
        m_state.complete = false;
        m_state.status = QStringLiteral("已停止（最终能谱超时，数据不完整）");
        emit errorOccurred(m_state.status);
        finishStop();
    }

    void beginBaselineControlOperation() {
        if (!beginControlOperation(ControlIntent::ApplyBaseline)) {
            m_serialBaselineApplyPending = false;
            emit baselineTrackingApplyFinished(
                m_requestedBaselineTracking, false,
                QStringLiteral("已有控制操作正在进行，请稍后重试"));
            return;
        }
        m_baselineValidationRecoveryAttempted = false;
        m_frontendCommitBusyRetries = 0;
        enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0, 1,
                m_requestedBaselineTracking ? 1u : 0u);
        enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0,
                RegisterGlobalManualBaseline, m_requestedManualBaseline);
        enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0,
                RegisterGlobalBandwidthEnable,
                m_requestedBandwidthEnabled ? 1u : 0u);
        if (m_capabilities.supportsPeakInterpolation())
            enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0,
                    RegisterGlobalPeakInterpolation,
                    m_requestedPeakInterpolation ? 1u : 0u);
        for (int channel = 0; channel < m_channels.size(); ++channel)
            enqueue(ControlOpcode::WriteShadow, ControlTarget::Channel,
                    quint8(channel), RegisterChannelRecoveryTolerance,
                    m_requestedRecoveryTolerance);
        enqueue(ControlOpcode::WriteShadow, ControlTarget::Global, 0,
                RegisterGlobalRange, m_requestedRange);
        enqueue(ControlOpcode::CommitAll, ControlTarget::Global, 0, 0, 0);
        sendNextControl();
    }

    void handleWaveformRegisterRead(quint16 address, quint32 value) {
        switch (address) {
        case RegisterWaveStatus:
            m_waveformStatus.state = waveformStateFromCode(value);
            break;
        case RegisterWaveLastCapture:
            m_waveformStatus.lastCaptureId = value;
            break;
        case RegisterWaveCaptureCount:
            m_waveformStatus.captureCount = value;
            break;
        case RegisterGlobalWaveSession:
            m_waveformStatus.sessionId = value;
            break;
        default:
            return;
        }
        const auto &stats = m_waveformReassembler.statistics();
        m_waveformStatus.completedCaptures = stats.completedCaptures;
        m_waveformStatus.discardedCaptures = stats.discardedCaptures;
        m_waveformStatus.droppedFragments = stats.timedOutCaptures
                                          + stats.evictedCaptures
                                          + stats.conflictingFragments;
        publishWaveformStatus();
        // Single-shot reception: once the device reports that it finished the
        // capture, the whole record must arrive.  If it has not been
        // reassembled one second later the reception is reported incomplete,
        // which also covers "every fragment was lost" and keeps the UI from
        // waiting forever.
        if (m_waveformTriggerMode != WaveformTriggerMode::Single)
            return;
        if (m_waveformStatus.state == WaveformState::SingleDone) {
            if (!m_waveformSingleArmed) {
                m_waveformSingleArmed = true;
                m_waveformSingleDeadline =
                    QDateTime::currentDateTime().addMSecs(
                        WaveformReassembler::TimeoutMilliseconds);
            }
        } else {
            m_waveformSingleArmed = false;
            m_waveformSingleDeadline = QDateTime();
            return;
        }
        if (m_waveformSingleDeadline.isValid()
            && m_waveformSingleDeadline <= QDateTime::currentDateTime()) {
            m_waveformSingleArmed = false;
            m_waveformSingleDeadline = QDateTime();
            m_waveformStatus.receiveIncomplete = true;
            m_waveformStatus.detail =
                QStringLiteral("单次波形接收不完整，请重新采集");
            emit errorOccurred(m_waveformStatus.detail);
            publishWaveformStatus();
        }
    }

    static WaveformState waveformStateFromCode(quint32 code) {
        switch (code) {
        case 0: return WaveformState::Stopped;
        case 1: return WaveformState::Filling;
        case 2: return WaveformState::Waiting;
        case 3: return WaveformState::Capturing;
        case 4: return WaveformState::Uploading;
        case 5: return WaveformState::SingleDone;
        default: return WaveformState::Stopped;
        }
    }

    void publishWaveform(const WaveformFragment &fragment, quint8 deviceId,
                         quint32 packetConfigVersion) {
        QString error;
        auto capture = m_waveformReassembler.add(fragment, deviceId,
                                                 packetConfigVersion, &error);
        if (!error.isEmpty()) {
            m_waveformStatus.receiveIncomplete = true;
            m_waveformStatus.detail = error;
            emit errorOccurred(error);
        }
        if (!capture)
            return;
        m_waveformSingleArmed = false;
        m_waveformSingleDeadline = QDateTime();
        m_waveformStatus.receiveIncomplete = false;
        m_waveformStatus.detail.clear();
        m_waveformStatus.completedCaptures =
            m_waveformReassembler.statistics().completedCaptures;
        // Receiving/reassembly stays lossless, but a fast continuous stream
        // must not enqueue every 12.5k-sample record on the UI thread.  Keep
        // only the newest complete record between display ticks.  Single-shot
        // mode remains immediate.
        if (m_waveformTriggerMode == WaveformTriggerMode::Single) {
            emit waveformReady(*capture);
        } else {
            m_pendingWaveform = *capture;
        }
    }

    void updateCapability(quint16 address, quint32 value) {
        switch (address) {
        case 0x0100: m_capabilities.adcCount = quint8(value); break;
        case 0x0101: m_capabilities.maximumAdcCount = quint8(value); break;
        case 0x0102: m_capabilities.channelCount = quint8(value); break;
        case 0x0103: m_capabilities.maximumChannelCount = quint8(value); break;
        case 0x0104: m_capabilities.moduleCount = quint8(value); break;
        case 0x0105: m_capabilities.maximumModuleCount = quint8(value); break;
        case 0x0106: m_capabilities.pairCount = quint8(value); break;
        case 0x0107: {
            const quint32 oldSampleRateHz = m_capabilities.sampleRateHz;
            if (m_mode!=AcquisitionMode::Psd && oldSampleRateHz != 0 && value != 0 &&
                oldSampleRateHz != value) {
                for (auto &channel : m_channels) {
                    const int postSamples = qRound(
                        double(channel.integrationPostSamples) *
                        double(value) / double(oldSampleRateHz));
                    channel.integrationPostSamples = quint16(
                        qBound(6, postSamples, 2500));

                    const int preSamples = qRound(
                        double(channel.integrationPreSamples) *
                        double(value) / double(oldSampleRateHz));
                    channel.integrationPreSamples = quint16(
                        qBound(0, preSamples,
                               int(channel.integrationPostSamples) - 1));
                }
            }
            m_capabilities.sampleRateHz = value;
            break;
        }
        case 0x0108: m_capabilities.firmwareVersion = value; break;
        case 0x0109: m_configVersion = value; break;
        case RegisterCapFormatFlags:
            m_capabilities.formatFlags = value;
            m_capabilities.formatFlagsValid = true;
            m_capabilities.extendedQuery = (value & 0x1u) != 0;
            if (m_mode == AcquisitionMode::Waveform &&
                !m_capabilities.supportsWaveform()) {
                // Reported once here so the failure is immediate rather than
                // after the whole capability scan.
                emit errorOccurred(QStringLiteral(
                    "该固件不支持波形模式（缺少能力位 bit5）"));
            }
            break;
        case 0x010B: m_capabilities.adcBits = quint8(value); break;
        case 0x010C:
            m_capabilities.peakFractionBits = quint8(value);
            // The live UDP event stream is Q14.2.  The accumulator is
            // constructed before the asynchronous capability scan finishes
            // and otherwise keeps its legacy Q4 default, making displayed
            // peak addresses exactly four times too small.  Apply the
            // authoritative value to the already-created decoder as soon as
            // this register is received.
            if (m_accumulator)
                m_accumulator->setPeakFractionBits(
                    m_capabilities.peakFractionBits);
            break;
        case 0x010D: m_capabilities.integralBits = quint8(value); break;
        case 0x010E: m_capabilities.integrationWindowMinNs = value; break;
        case 0x010F: m_capabilities.integrationWindowMaxNs = value; break;
        case 0x0110: m_capabilities.timebasePs = value; break;
        case 0x0111: m_capabilities.psdWindowMaxNs = value; break;
        default: return;
        }
        if (address == 0x0110) {
            const int oldCount = m_channels.size();
            if (m_capabilities.channelCount != oldCount)
                resizeForCapabilities();
            emit capabilitiesReady(m_capabilities);
        }
        m_state.capabilities = m_capabilities;
    }

    void handleControlResponse(const ControlResponse &response) {
        if (!m_pendingControl ||
            response.transactionId != m_pendingControl->transaction)
            return;
        const auto command = *m_pendingControl;
        m_controlRetry->stop();
        m_pendingControl.reset();
        qInfo().nospace() << "MCF2 rx intent=" << intentName(m_controlIntent)
                          << " opcode=" << int(command.opcode)
                          << " transaction=" << command.transaction
                          << " status=" << int(response.status);
        emit hardwareCommunicationChanged(true);

        if (!response.accepted()) {
            if (command.opcode == ControlOpcode::Read &&
                command.target == ControlTarget::Global &&
                command.registerAddress >= 0x010A &&
                command.registerAddress <= 0x0110 &&
                response.status == quint8(ControlStatus::BadValue)) {
                m_capabilities.extendedQuery = false;
                m_capabilities.formatFlags = 0;
                m_capabilities.formatFlagsValid = true;
                if (command.registerAddress == 0x0110) {
                    if (m_mode == AcquisitionMode::ExternalTrigger) {
                        failControlOperation(QStringLiteral(
                            "该固件不支持外置触发模式（缺少能力位 bit8），"
                            "请升级 FPGA 固件"));
                        return;
                    }
                    emit capabilitiesReady(m_capabilities);
                    queueConfigurationWrites();
                }
                sendNextControl();
                return;
            }
            if (command.opcode == ControlOpcode::Start &&
                response.status == quint8(ControlStatus::NotReady)) {
                QTimer::singleShot(250, this, [this] {
                    if (m_controlIntent != ControlIntent::StartRun &&
                        m_controlIntent != ControlIntent::ResumeRun &&
                        m_controlIntent !=
                            ControlIntent::ReconfigureAndResume)
                        return;
                    enqueue(ControlOpcode::Start, ControlTarget::Global,
                            0, 0, m_configVersion);
                    sendNextControl();
                });
                return;
            }
            // A range write starts a short front-end pulse immediately.  The
            // following atomic commit can therefore arrive while range_busy
            // is still asserted even though acquisition is stopped.  Retry
            // that commit with a new transaction id after the pulse instead
            // of reporting the temporary BUSY as a configuration failure.
            if (m_controlIntent == ControlIntent::ApplyBaseline &&
                command.opcode == ControlOpcode::CommitAll &&
                response.status == quint8(ControlStatus::Busy) &&
                m_frontendCommitBusyRetries <
                    FrontendCommitBusyMaximumAttempts) {
                ++m_frontendCommitBusyRetries;
                QTimer::singleShot(
                    FrontendCommitBusyRetryMilliseconds, this, [this] {
                        if (m_controlIntent != ControlIntent::ApplyBaseline ||
                            m_pendingControl || !m_controlQueue.isEmpty())
                            return;
                        enqueue(ControlOpcode::CommitAll,
                                ControlTarget::Global, 0, 0, 0);
                        sendNextControl();
                    });
                return;
            }
            // A previous host version could leave an invalid channel shadow
            // (for example threshold == baseline-pulse-threshold) after a
            // later unsupported write failed.  Repair the complete shadow
            // configuration once, then retry the requested baseline commit.
            // This keeps the recovery in the host and avoids requiring a
            // power-cycle just to clear stale FPGA shadow registers.
            if (m_controlIntent == ControlIntent::ApplyBaseline &&
                command.opcode == ControlOpcode::CommitAll &&
                response.status == quint8(ControlStatus::ValidationFailed) &&
                !m_baselineValidationRecoveryAttempted) {
                m_baselineValidationRecoveryAttempted = true;
                m_baselineTracking = m_requestedBaselineTracking;
                qInfo() << "MCF2 baseline validation failed; repairing shadow configuration";
                queueCapabilityReads();
                sendNextControl();
                return;
            }
            QString message;
            if (m_controlIntent == ControlIntent::ApplyBaseline &&
                response.status == quint8(ControlStatus::Busy)) {
                message = QStringLiteral(
                    "FPGA 正在采集，基线设置未应用；请先停止采集");
            } else if (m_serialMode) {
                message = QStringLiteral(
                    "串口%1控制事务%2被FPGA拒绝：register=0x%3，状态码%4")
                              .arg(m_serialPort ? m_serialPort->portName()
                                                 : QString())
                              .arg(response.transactionId)
                              .arg(command.registerAddress, 4, 16,
                                   QLatin1Char('0'))
                              .arg(response.status);
            } else {
                message = QStringLiteral(
                              "控制事务 %1 被 FPGA 拒绝，状态码 %2，"
                              "opcode=%3，target=%4:%5，register=0x%6，value=%7")
                              .arg(response.transactionId)
                              .arg(response.status)
                              .arg(static_cast<int>(command.opcode))
                              .arg(static_cast<int>(command.target))
                              .arg(static_cast<int>(command.targetId))
                              .arg(command.registerAddress, 4, 16,
                                   QLatin1Char('0'))
                              .arg(command.value);
            }
            failControlOperation(message);
            return;
        }

        if (command.opcode == ControlOpcode::Read &&
            command.target == ControlTarget::Global) {
            updateCapability(command.registerAddress, response.value);
            if (command.registerAddress == 0x0110) {
                if (m_mode == AcquisitionMode::Waveform) {
                    if (!m_capabilities.supportsWaveform()) {
                        // An old firmware cannot speak the waveform protocol;
                        // disable the entry point instead of sending new
                        // register writes that it would reject.
                        failControlOperation(QStringLiteral(
                            "该固件不支持波形模式（缺少能力位 bit5），"
                            "请升级 FPGA 固件"));
                        return;
                    }
                    queueWaveformConfigurationWrites();
                } else if (m_mode == AcquisitionMode::ExternalTrigger) {
                    if (!m_capabilities.supportsExternalTrigger()) {
                        failControlOperation(QStringLiteral(
                            "该固件不支持外置触发模式（缺少能力位 bit8），"
                            "请升级 FPGA 固件"));
                        return;
                    }
                    queueConfigurationWrites();
                } else {
                    queueConfigurationWrites();
                }
            } else if (command.registerAddress == RegisterWaveStatus ||
                       command.registerAddress == RegisterWaveLastCapture ||
                       command.registerAddress == RegisterWaveCaptureCount) {
                handleWaveformRegisterRead(command.registerAddress,
                                           response.value);
            } else if (command.registerAddress == RegisterExternalStatus) {
                m_state.externalArmed = (response.value & 0x1u) != 0;
                m_state.externalWindowActive = (response.value & 0x2u) != 0;
            } else if (command.registerAddress == RegisterExternalTriggerCount) {
                m_state.externalTriggerCount = response.value;
            }
        }

        if (command.opcode == ControlOpcode::CommitAll) {
            m_configVersion = response.value;
            m_state.configVersion = m_configVersion;
            const bool startsRun =
                m_controlIntent == ControlIntent::StartRun ||
                m_controlIntent == ControlIntent::ResumeRun ||
                m_controlIntent == ControlIntent::ReconfigureAndResume ||
                m_controlIntent == ControlIntent::StartWaveform ||
                m_controlIntent == ControlIntent::ApplyWaveformConfig;
            if (m_controlIntent == ControlIntent::StartRun && m_startPhase == 3 &&
                !openEventTextIfRequested()) {
                failControlOperation(m_state.eventTextError);
                return;
            }
            if (m_controlIntent == ControlIntent::StartWaveform) {
                m_state.status = QStringLiteral("波形模式：配置版本 %1")
                                     .arg(m_configVersion);
                enqueue(ControlOpcode::Start, ControlTarget::Global,
                        0, 0, m_configVersion);
            } else if (startsRun) {
                enqueue(ControlOpcode::Start, ControlTarget::Global,
                        0, 0, m_configVersion);
            } else if (m_controlIntent == ControlIntent::ApplyBaseline) {
                m_baselineTracking = m_requestedBaselineTracking;
                m_manualBaseline = m_requestedManualBaseline;
                m_recoveryTolerance = m_requestedRecoveryTolerance;
                m_bandwidthEnabled = m_requestedBandwidthEnabled;
                m_peakInterpolationEnabled = m_requestedPeakInterpolation;
                m_inputRange = m_requestedRange;
                m_serialBaselineApplyPending = false;
                const bool enabled = m_baselineTracking;
                completeControlOperation();
                emit baselineTrackingApplyFinished(
                    enabled, true,
                    enabled ? QStringLiteral("配置已提交，等待STATUS确认：基线跟踪")
                            : QStringLiteral("配置已提交，等待STATUS确认：手动基线 %1")
                                  .arg(m_manualBaseline));
            } else if (m_controlIntent == ControlIntent::ApplyConfiguration) {
                completeControlOperation();
            }
        } else if (command.opcode == ControlOpcode::Start) {
            const bool initialStart = m_controlIntent == ControlIntent::StartRun ||
                                      m_controlIntent == ControlIntent::StartWaveform;
            if (initialStart && m_startPhase == 0) {
                m_startConfigurationRepeated = true;
                m_startPhase = 1;
                m_discardPrestartData = true;
                m_state.state = AcquisitionState::Starting;
                m_state.status = QStringLiteral("预启动中，数据丢弃");
                m_startRepeatTimer->start();
                publish();
                return;
            }
            if (initialStart && m_startPhase == 3) {
                m_officialConfigVersion = m_configVersion;
                m_discardPrestartData = false;
                m_accumulator->clear();
                m_serialAssembler.clear();
                resetSequenceWindow();
                m_state.totalEvents = 0;
                m_state.pileupEvents = 0;
                const quint32 receiveBufferBytes=m_state.packets.receiveBufferBytes;
                m_state.packets = {};
                m_state.packets.receiveBufferBytes=receiveBufferBytes;
                m_elapsed.restart();
                m_rateElapsed.restart();
                m_lastRateEvents = 0;
                m_lastRatePackets = 0;
            }
            m_serialRunConfirmed = m_serialMode;
            m_state.state = AcquisitionState::Running;
            m_state.status = m_mode == AcquisitionMode::Waveform
                ? QStringLiteral("波形采集中，配置版本 %1").arg(m_configVersion)
                : (m_mode == AcquisitionMode::ExternalTrigger
                   ? QStringLiteral("外触发：等待EXTRI，配置版本 %1")
                         .arg(m_configVersion)
                   : QStringLiteral("采集中，配置版本 %1")
                         .arg(m_configVersion));
            completeControlOperation();
        } else if (command.opcode == ControlOpcode::Stop &&
                   m_controlQueue.isEmpty()) {
            if (m_startPhase == 1 &&
                (m_controlIntent == ControlIntent::StartRun ||
                 m_controlIntent == ControlIntent::StartWaveform)) {
                m_startPhase = 2;
                m_state.status = QStringLiteral("预启动已停止，准备正式采集");
                m_startRepeatTimer->start();
                publish();
                return;
            } else if (m_controlIntent == ControlIntent::PauseRun) {
                m_state.state = AcquisitionState::Paused;
                m_state.status = QStringLiteral("已暂停，FPGA 流水线已排空");
                completeControlOperation();
            } else if (m_controlIntent == ControlIntent::StopRun) {
                const bool waitForFinal = m_serialMode && m_serialRunConfirmed;
                if (waitForFinal) {
                    m_serialAwaitFinalFlush = true;
                    m_state.status = QStringLiteral("正在接收串口最终能谱");
                    m_serialFinalFlush->start();
                    completeControlOperation();
                } else {
                    finishStop();
                }
                return;
            } else if (m_controlIntent == ControlIntent::StartWaveform ||
                       m_controlIntent == ControlIntent::ApplyWaveformConfig) {
                // The stop half of the start/stop/commit/start sequence has
                // completed; the queued configuration writes follow.
            }
        } else if (command.opcode == ControlOpcode::ClearCounters &&
                   m_controlQueue.isEmpty() &&
                   m_controlIntent == ControlIntent::ClearCounters) {
            completeControlOperation();
        } else if (m_controlIntent == ControlIntent::PollWaveform &&
                   command.opcode == ControlOpcode::Read &&
                   ((m_mode == AcquisitionMode::Waveform &&
                     command.registerAddress == RegisterWaveCaptureCount) ||
                    (m_mode == AcquisitionMode::ExternalTrigger &&
                     command.registerAddress == RegisterExternalTriggerCount))) {
            m_waveformPollPending = false;
            completeControlOperation();
        }
        sendNextControl();
        publish();
    }

    void begin(const AcquisitionPreset &preset, const QString &path) {
        m_textWriter.finish(false, QStringLiteral("new acquisition started"));
        m_generate->stop();
        m_waveformPoll->stop();
        abortControlOperation();
        m_accumulator->clear();
        m_preset = preset;
        m_pendingTextPath = path;
        m_state = {};
        m_discardPrestartData = true;
        m_officialConfigVersion = 0;
        m_state.includePileupInSpectrum = m_includePileupInSpectrum;
        m_state.state = AcquisitionState::Starting;
        m_state.status = QStringLiteral("正在准备采集");
        m_state.mode = m_mode;
        m_state.capabilities = m_capabilities;
        m_state.eventTextEnabled = !path.isEmpty();
        m_state.eventTextPath = path;
        m_elapsed.restart();
        m_rateElapsed.restart();
        m_lastRateEvents = 0;
        m_lastRatePackets = 0;
        m_sequence = 0;
        m_simTimestamp = 0;
        resetSequenceWindow();
        m_state.packets.receiveBufferBytes =
            m_dataSocket
            ? m_dataSocket->socketOption(
                  QAbstractSocket::ReceiveBufferSizeSocketOption).toUInt()
            : 0;
        m_publish->start();
        publish();
    }

    bool openEventTextIfRequested() {
        if (m_pendingTextPath.isEmpty() || m_textWriter.isOpen())
            return true;
        if (m_mode != AcquisitionMode::AutomaticTrigger &&
            m_mode != AcquisitionMode::ExternalTrigger) {
            m_state.eventTextError =
                QStringLiteral("事件 TXT 仅支持单通道独立事件模式");
            return false;
        }
        QString error;
        EventTextMetadata metadata;
        metadata.deviceAddress = m_device;
        metadata.sampleRateHz = m_capabilities.sampleRateHz;
        metadata.peakFractionBits = m_capabilities.peakFractionBits;
        metadata.startedAt = QDateTime::currentDateTime();
        if (!m_textWriter.start(m_pendingTextPath, metadata, &error)) {
            m_state.eventTextError = error;
            return false;
        }
        m_state.eventTextEnabled = true;
        m_state.eventTextPath = m_pendingTextPath;
        return true;
    }

    ModuleEvent simulatedModule(int module, quint32 id, quint64 timestamp) {
        ModuleEvent event;
        event.eventId = id;
        event.module = quint8(module);
        event.timestamp = timestamp;
        quint64 energy = 0;
        for (int i = 0; i < 4; ++i) {
            const int channel = module * 4 + i;
            const double u1 = qMax(1e-9,
                QRandomGenerator::global()->generateDouble());
            const double u2 = QRandomGenerator::global()->generateDouble();
            const double normal = std::sqrt(-2.0 * std::log(u1))
                                * std::cos(6.283185307 * u2);
            const int adc = qBound(0, int(550 + channel * 180
                                          + normal * 20), 4095);
            event.relativeTime[size_t(i)] = qint16(i - 2);
            event.peak[size_t(i)] = quint16(adc << 4);
            event.integral[size_t(i)] = quint32(adc * 1024);
            event.channelFlags[size_t(i)] = quint8(channel << 3);
            energy += event.integral[size_t(i)];
        }
        event.energy = quint32(qMin<quint64>(energy, 0xffffffffu));
        const qint64 left = qint64(event.integral[0])
                          - qint64(event.integral[1]);
        const quint64 xden = quint64(event.integral[0])
                           + event.integral[1];
        const qint64 lower = qint64(event.integral[2])
                           - qint64(event.integral[3]);
        const quint64 yden = quint64(event.integral[2])
                           + event.integral[3];
        event.x = quint16(qBound<qint64>(qint64(0), qint64(1024)
                    + (xden ? (1024 * left) / qint64(xden) : 0),
                    qint64(2048)));
        event.y = quint16(qBound<qint64>(qint64(0), qint64(1024)
                    + (yden ? (1024 * lower) / qint64(yden) : 0),
                    qint64(2048)));
        return event;
    }

    void generate() {
        ++m_sequence;
        if (m_mode == AcquisitionMode::AutomaticTrigger ||
            m_mode == AcquisitionMode::ExternalTrigger) {
            QVector<ChannelEvent> events;
            events.reserve(64);
            for (int i = 0; i < 64; ++i) {
                const int channel = i % m_channels.size();
                if (!m_channels[channel].enabled)
                    continue;
                const double u1 = qMax(1e-9,
                    QRandomGenerator::global()->generateDouble());
                const double u2 = QRandomGenerator::global()->generateDouble();
                const double normal = std::sqrt(-2.0 * std::log(u1))
                                    * std::cos(6.283185307 * u2);
                const int adc = qBound(0, int(500 + channel * 220
                                              + normal * 18), 4095);
                ChannelEvent event;
                event.timestamp = (m_simTimestamp += 10);
                event.peak = quint16(adc << 4);
                event.integral = quint32(qMax(0, adc * 1024
                                                   + int(normal * 2000)));
                event.channel = quint8(channel);
                event.pileup = (i % 17) == 0;
                events.push_back(event);
            }
            processRaw(PacketDecoder::encodeChannels(
                m_sequence, m_configVersion, 0, events));
        } else if (m_mode == AcquisitionMode::LegacyFourChannel) {
            QVector<ModuleEvent> events;
            for (int i = 0; i < 12; ++i) {
                const int module = i % m_modules.size();
                m_simTimestamp += 20;
                events.push_back(simulatedModule(module, m_simEventId++,
                                                  m_simTimestamp));
            }
            processRaw(PacketDecoder::encodeModules(
                m_sequence, m_configVersion, 0, events));
        } else {
            QVector<CoincidenceEvent> events;
            for (int i = 0; i < 6; ++i) {
                m_simTimestamp += 25;
                CoincidenceEvent event;
                event.matchId = m_simEventId++;
                event.pair = 0;
                event.layerTime = qint16((i % 17) - 8);
                event.flags = (i % 5 == 0) ? 1 : 0;
                QVector<int> scatterModules;
                QVector<int> absorberModules;
                const auto pair = m_pairs.isEmpty() ? PairConfig{} : m_pairs[0];
                for (int module = 0; module < m_modules.size(); ++module) {
                    if (pair.scatterModuleMask & (1u << module))
                        scatterModules.push_back(module);
                    if (pair.absorberModuleMask & (1u << module))
                        absorberModules.push_back(module);
                }
                if (scatterModules.isEmpty()) scatterModules.push_back(0);
                if (absorberModules.isEmpty())
                    absorberModules.push_back(qMin(1, int(m_modules.size()) - 1));
                event.scatter = simulatedModule(
                    scatterModules[i % scatterModules.size()],
                    m_simEventId++, m_simTimestamp);
                event.absorber = simulatedModule(
                    absorberModules[i % absorberModules.size()],
                    m_simEventId++,
                    quint64(qint64(m_simTimestamp) + event.layerTime));
                events.push_back(event);
            }
            processRaw(PacketDecoder::encodeCoincidences(
                m_sequence, m_configVersion, 0, events));
        }
    }

    void readSerial() {
        if (!m_serialPort) return;
        const QByteArray serialBytes = m_serialPort->readAll();
        if (m_serialHelloPending) {
            qInfo().noquote()
                << QStringLiteral("串口HELLO阶段RX %1字节：%2")
                       .arg(serialBytes.size())
                       .arg(QString::fromLatin1(serialBytes.left(64).toHex(' ')));
        }
        QStringList errors;
        const auto frames = m_serialCodec.feed(serialBytes, &errors);
        for (const QString &error : errors) {
            ++m_state.packets.parseErrors;
            qWarning() << error;
            if (error.startsWith(QStringLiteral("串口协议版本"))) {
                fail(error);
                return;
            }
        }
        for (const auto &frame : frames) {
            ++m_state.packets.received;
            m_state.packets.datagramBytes += quint64(
                SerialHeaderBytes + frame.payload.size() + SerialCrcBytes);
            if (frame.type == SerialMessageType::HelloResponse) {
                SerialHello hello;
                QString error;
                if (!decodeSerialHello(frame.payload, hello, &error)) {
                    fail(QStringLiteral("串口能力协商失败：%1").arg(error));
                    return;
                }
                m_capabilities = {};
                m_capabilities.adcCount = 1;
                m_capabilities.maximumAdcCount = 1;
                m_capabilities.channelCount = 1;
                m_capabilities.maximumChannelCount = 1;
                m_capabilities.moduleCount = 0;
                m_capabilities.maximumModuleCount = 0;
                m_capabilities.pairCount = 0;
                m_capabilities.sampleRateHz = hello.sampleRateHz;
                m_capabilities.firmwareVersion = hello.firmwareVersion;
                m_capabilities.adcBits = quint8(std::countr_zero(
                    quint32(hello.binCount)));
                m_capabilities.peakFractionBits = 0;
                m_capabilities.peakBinShift = hello.peakShift;
                m_capabilities.integralBinShift = hello.integralShift;
                m_capabilities.integralBits = 24;
                m_capabilities.formatFlags = 0x0000'03dfu;
                m_capabilities.formatFlagsValid = true;
                m_capabilities.extendedQuery = true;
                resizeForCapabilities();
                m_serialHello = hello;
                m_serialHelloPending = false;
                m_serialHelloAccepted = true;
                m_serialHelloRetry->stop();
                qInfo().noquote()
                    << QStringLiteral("串口HELLO校验通过：MCS2 v2，%1 baud，%2 bins，固件0x%3")
                           .arg(hello.baudRate)
                           .arg(hello.binCount)
                           .arg(hello.firmwareVersion, 8, 16, QLatin1Char('0'));
                emit hardwareCommunicationChanged(true);
                emit capabilitiesReady(m_capabilities);
                if (m_afterSerialHello == SerialHelloAction::StartRun) {
                    if (!configureHardware(ControlIntent::StartRun))
                        fail(QStringLiteral("串口能力协商后无法启动控制队列"));
                } else if (m_afterSerialHello == SerialHelloAction::ApplyBaseline) {
                    beginBaselineControlOperation();
                }
                continue;
            }
            if (frame.type == SerialMessageType::ControlResponse) {
                if (frame.payload.size() != ControlResponseBytes) {
                    ++m_state.packets.parseErrors;
                    continue;
                }
                const auto *p = reinterpret_cast<const uchar *>(frame.payload.constData());
                ControlResponse response;
                response.transactionId = qFromBigEndian<quint32>(p);
                response.opcode = p[4]; response.target = p[5];
                response.targetId = p[6]; response.status = p[7];
                response.registerAddress = qFromBigEndian<quint16>(p + 8);
                response.value = qFromBigEndian<quint32>(p + 12);
                handleControlResponse(response);
                continue;
            }
            if (frame.type == SerialMessageType::Status) {
                if (m_discardPrestartData)
                    continue;
                SerialStatus status;
                QString error;
                if (!decodeSerialStatus(frame.payload, status, &error)) {
                    ++m_state.packets.parseErrors;
                    qWarning() << error;
                    continue;
                }
                emit hardwareCommunicationChanged(true);
                m_state.configVersion = status.configVersion;
                m_state.serialCrcErrors = status.crcErrors;
                m_state.serialFrameErrors = status.frameErrors;
                m_state.serialRetries = status.retries;
                // UART transports FPGA-built histogram deltas rather than
                // individual events, so no per-event pileup flag reaches the
                // host.  The STATUS rejected counter is the acquisition core's
                // pileup/dead-time crossing count and is the only authoritative
                // numerator available in serial mode.
                m_state.pileupEvents = status.rejectedEvents;
                m_state.externalArmed = status.externalArmed;
                m_state.externalWindowActive = status.externalWindowActive;
                m_state.externalTriggerCount = status.externalTriggerCount;
                m_state.baselineReady = status.baselineReady;
                m_state.baselineAutomatic = status.baselineAutomatic;
                m_state.baseline = status.baseline;
                m_state.manualBaseline = status.manualBaseline;
                m_state.recoveryTolerance = status.recoveryTolerance;
                m_state.actualRange = status.actualRange;
                m_state.rangeBusy = status.rangeBusy;
                m_state.filterEnabled = status.filterEnabled;
                m_state.peakInterpolationEnabled = status.peakInterpolationEnabled;
                m_state.frontendInitDone = status.frontendInitDone;
                DeviceStatus device;
                device.capabilities = m_capabilities;
                device.mode = AcquisitionMode(status.mode);
                device.running = status.running;
                device.configVersion = status.configVersion;
                device.timestamp = status.timestamp;
                device.baselineReadyMask = status.baselineReady ? 1u : 0u;
                device.baselineTrackingMask = status.baselineAutomatic ? 1u : 0u;
                device.baselineValues[0] = status.baseline;
                device.baselineValueCount = 1;
                device.baselineValueValidMask = status.baselineReady ? 1u : 0u;
                device.baselineStates[0] = status.baselineState;
                device.baselineAutomatic = status.baselineAutomatic;
                device.manualBaseline = status.manualBaseline;
                device.manualBaselineValid = true;
                device.externalArmed = status.externalArmed;
                device.externalWindowActive = status.externalWindowActive;
                device.externalTriggerCount = status.externalTriggerCount;
                device.outputEvents = status.outputEvents;
                device.deadTimeRejected = status.rejectedEvents;
                device.droppedEvents = status.droppedEvents;
                device.actualRange = status.actualRange;
                device.rangeBusy = status.rangeBusy;
                device.filterEnabled = status.filterEnabled;
                device.peakInterpolationEnabled = status.peakInterpolationEnabled;
                device.frontendInitDone = status.frontendInitDone;
                device.frontendStateValid = true;
                device.recoveryToleranceLsb = status.recoveryTolerance;
                emit deviceStatusReady(device);
                BaselineSnapshot baseline;
                baseline.sequence = status.configVersion;
                baseline.automatic = status.baselineAutomatic;
                baseline.manualBaseline = status.manualBaseline;
                baseline.manualBaselineValid = true;
                BaselineChannelStatus channel;
                channel.channel = 0;
                channel.state = status.baselineState;
                channel.baseline = status.baseline;
                channel.baselineValid = status.baselineReady;
                channel.trackingEnabled = status.baselineAutomatic;
                channel.ready = status.baselineReady;
                baseline.channels.push_back(channel);
                emit baselineReady(baseline);
                continue;
            }
            if (frame.type == SerialMessageType::Spectrum) {
                SerialSpectrumChunk chunk;
                QString error;
                if (!decodeSerialSpectrum(frame.payload, chunk, &error)) {
                    ++m_state.packets.parseErrors;
                    continue;
                }
                const QByteArray ackPayload = makeSerialAck(frame.sequence, chunk);
                m_serialPort->write(SerialFrameCodec::encode(
                    SerialMessageType::Ack, ++m_serialTxSequence, ackPayload));
                if (m_discardPrestartData ||
                    chunk.configVersion < m_officialConfigVersion)
                    continue;
                if (!m_serialRunConfirmed && !m_serialAwaitFinalFlush)
                    continue;
                auto epoch = m_serialAssembler.add(chunk, &error);
                if (!error.isEmpty()) {
                    ++m_state.packets.parseErrors;
                    continue;
                }
                if (epoch) {
                    m_accumulator->addSerialDelta(epoch->peak, epoch->integral,
                                                  epoch->intervalEvents,
                                                  epoch->peakShift,
                                                  epoch->integralShift);
                    m_state.totalEvents += epoch->intervalEvents;
                    m_state.eventsPerSecond = epoch->intervalTicks
                        ? double(epoch->intervalEvents) *
                          double(m_capabilities.sampleRateHz) /
                          double(epoch->intervalTicks) : 0.0;
                    if (epoch->flags & 0x03u) {
                        m_state.complete = false;
                        emit errorOccurred(QStringLiteral("FPGA串口能谱计数达到32位饱和"));
                    }
                    if (m_serialAwaitFinalFlush && (epoch->flags & 0x04u)) {
                        m_serialAwaitFinalFlush = false;
                        finishStop();
                        return;
                    }
                    if (shouldStop()) stop();
                }
            }
        }
    }

    void readDatagrams() {
        // Bound one callback so an always-full UDP socket cannot monopolise
        // the worker thread.  Control retries, stop/apply commands and the
        // 100 ms spectrum publication timer must all get a chance to run.
        m_datagramFollowupQueued = false;
        QElapsedTimer turn;
        turn.start();
        int processed = 0;
        while (m_dataSocket && m_dataSocket->hasPendingDatagrams()
               && processed < MaximumDatagramsPerTurn
               && turn.elapsed() < DatagramTurnBudgetMilliseconds) {
            const qint64 datagramSize = m_dataSocket->pendingDatagramSize();
            if (datagramSize <= 0 || datagramSize > MaximumUdpPayloadBytes) {
                QByteArray discarded(int(qMax<qint64>(0, datagramSize)),
                                     Qt::Uninitialized);
                m_dataSocket->readDatagram(discarded.data(), discarded.size());
                ++m_state.packets.malformed;
                ++m_state.packets.parseErrors;
                ++processed;
                continue;
            }
            QByteArray bytes(int(datagramSize), Qt::Uninitialized);
            const qint64 read = m_dataSocket->readDatagram(bytes.data(),
                                                            bytes.size());
            if (read != bytes.size()) {
                ++m_state.packets.malformed;
                ++m_state.packets.parseErrors;
            } else {
                processRaw(bytes);
            }
            ++processed;
        }
        const quint64 elapsed=quint64(turn.nsecsElapsed());
        ++m_state.packets.receiveTurns;
        m_state.packets.receiveNanoseconds+=elapsed;
        m_state.packets.maximumReceiveTurnNanoseconds=qMax(m_state.packets.maximumReceiveTurnNanoseconds,elapsed);
        if (m_dataSocket && m_dataSocket->hasPendingDatagrams()
            && !m_datagramFollowupQueued) {
            m_datagramFollowupQueued = true;
            QMetaObject::invokeMethod(this, &AcquisitionWorker::readDatagrams,
                                      Qt::QueuedConnection);
        }
    }

    void resetSequenceWindow() { m_sequenceWindow.clear(); }

    bool acceptSequence(quint32 sequence, qsizetype eventCount) {
        const bool accepted=m_sequenceWindow.accept(sequence,eventCount,m_state.packets);
        if (m_state.packets.confirmedMissing || m_state.packets.latePackets)
            m_state.complete=false;
        return accepted;
    }

    void processRaw(const QByteArray &bytes) {
        m_state.packets.datagramBytes += quint64(bytes.size());
        DecodedPacket packet;
        QString error;
        if (!PacketDecoder::decode(bytes, packet, &error)) {
            ++m_state.packets.malformed;
            ++m_state.packets.parseErrors;
            if (!m_parseErrorLog.isValid() || m_parseErrorLog.elapsed() >= 1000) {
                m_parseErrorLog.restart();
                emit errorOccurred(QStringLiteral("收到无效 UDP 数据（累计 %1 个）：%2")
                                   .arg(m_state.packets.parseErrors).arg(error));
            }
            return;
        }
        if (packet.type == PacketType::ChannelEvents ||
            packet.type == PacketType::ModuleEvents ||
            packet.type == PacketType::CoincidenceEvents ||
            packet.type == PacketType::PsdEvents) {
            if (!m_simulation && (m_discardPrestartData ||
                                  packet.configVersion < m_officialConfigVersion))
                return;
            ++m_state.packets.eventDatagrams;
        }
        if (packet.type == PacketType::Waveform) {
            if (m_discardPrestartData || packet.configVersion < m_officialConfigVersion)
                return;
            // Waveform fragments are validated and completed by the
            // reassembler, which accepts any arrival order.  The global
            // packet-sequence check must not discard a legal reordered
            // fragment, so the sequence counter is only used for statistics.
            auto &stats = m_state.packets;
            ++stats.received;
            stats.lastSequence = packet.sequence;
            stats.hasSequence = true;
            processDecoded(packet, bytes, true);
            return;
        }
        if (!acceptSequence(packet.sequence, packet.eventCount()))
            return;
        m_state.packets.validEventCount += quint64(packet.eventCount());
        processDecoded(packet, bytes, true);
    }

    void processDecoded(const DecodedPacket &packet, const QByteArray &raw,
                        bool enforceRunState) {
        if (packet.type == PacketType::ControlResponse) {
            for (const auto &response : packet.responses)
                handleControlResponse(response);
            return;
        }
        if (packet.type == PacketType::Waveform) {
            if (m_mode != AcquisitionMode::Waveform)
                return;
            for (const auto &fragment : packet.waveformFragments)
                publishWaveform(fragment, packet.deviceId,
                                packet.configVersion);
            return;
        }
        if (packet.type == PacketType::Status) {
            if (m_discardPrestartData && !m_simulation)
                return;
            if (packet.status.capabilitiesValid) {
                const auto &reported = packet.status.capabilities;
                const bool countsChanged =
                    m_capabilities.adcCount != reported.adcCount ||
                    m_capabilities.channelCount != reported.channelCount ||
                    m_capabilities.moduleCount != reported.moduleCount ||
                    m_capabilities.pairCount != reported.pairCount;
                m_capabilities.protocolVersion = reported.protocolVersion;
                m_capabilities.adcCount = reported.adcCount;
                m_capabilities.channelCount = reported.channelCount;
                m_capabilities.moduleCount = reported.moduleCount;
                m_capabilities.pairCount = reported.pairCount;
                if (countsChanged) {
                    resizeForCapabilities();
                    m_state.capabilities = m_capabilities;
                    emit capabilitiesReady(m_capabilities);
                }
            }
            // The single-channel firmware reports the configuration version
            // from the sampling domain in the status epoch.  It can be ahead
            // of the control-domain commit response while that CDC snapshot
            // is settling; adopt a newer device version so subsequent event
            // packets are not discarded as stale.
            if (!m_simulation &&
                packet.status.configVersion > m_configVersion) {
                qInfo().nospace()
                    << "MCF2 device config version advanced from "
                    << m_configVersion << " to "
                    << packet.status.configVersion;
                m_configVersion = packet.status.configVersion;
                m_state.configVersion = m_configVersion;
            }
            m_state.actualRange = packet.status.actualRange;
            m_state.rangeBusy = packet.status.rangeBusy;
            m_state.filterEnabled = packet.status.filterEnabled;
            m_state.peakInterpolationEnabled =
                packet.status.peakInterpolationEnabled;
            m_state.frontendInitDone = packet.status.frontendInitDone;
            emit deviceStatusReady(packet.status);
            BaselineSnapshot baseline;
            baseline.sequence = packet.sequence;
            baseline.automatic = packet.status.baselineAutomatic;
            baseline.manualBaseline = packet.status.manualBaseline;
            baseline.manualBaselineValid = packet.status.manualBaselineValid;
            const int reportedChannelCount = packet.status.capabilitiesValid
                ? int(packet.status.capabilities.channelCount)
                : int(m_capabilities.channelCount);
            baseline.channels.resize(qBound(
                0, reportedChannelCount, MaxChannels));
            for (int i = 0; i < baseline.channels.size(); ++i) {
                baseline.channels[i].channel = quint8(i);
                baseline.channels[i].ready =
                    (packet.status.baselineReadyMask & (1u << i)) != 0;
                baseline.channels[i].trackingEnabled =
                    (packet.status.baselineTrackingMask & (1u << i)) != 0;
                baseline.channels[i].state = packet.status.baselineStates[size_t(i)];
                if ((packet.status.baselineValueValidMask & (1u << i)) != 0) {
                    baseline.channels[i].baseline =
                        packet.status.baselineValues[i];
                    baseline.channels[i].baselineValid = true;
                }
            }
            emit baselineReady(baseline);
            return;
        }
        if (enforceRunState && !m_simulation && (m_discardPrestartData ||
            packet.configVersion < m_officialConfigVersion))
            return;
        if (packet.type==PacketType::PsdEvents && m_mode==AcquisitionMode::Psd &&
            (!enforceRunState || m_state.state==AcquisitionState::Running || m_state.state==AcquisitionState::Stopping)) {
            QVector<PsdEvent> valid;
            for (const auto &e:packet.psdEvents) if (e.legacy.channel<m_capabilities.channelCount) valid.push_back(e);
            m_state.totalEvents+=quint64(valid.size());
            if (!valid.isEmpty()) emit psdEventsReady(valid);
            if (shouldStop() && m_state.state==AcquisitionState::Running) stop();
            return;
        }
        if (enforceRunState && m_state.state != AcquisitionState::Running)
            return;
        if (packet.configVersion != m_configVersion && !m_simulation) {
            if (packet.configVersion > m_configVersion) {
                // A forward version is authoritative: the event stream is
                // already produced by the newly applied sampling config,
                // while the host may still have the older commit response.
                qInfo().nospace()
                    << "MCF2 event config version advanced from "
                    << m_configVersion << " to " << packet.configVersion;
                m_configVersion = packet.configVersion;
                m_state.configVersion = m_configVersion;
            } else {
                m_state.complete = false;
                emit errorOccurred(QStringLiteral(
                    "收到配置版本 %1 的事件，当前期望版本为 %2，已丢弃")
                    .arg(packet.configVersion).arg(m_configVersion));
                return;
            }
        }

        QVector<TypedEvent> typed;
        QVector<ChannelEvent> spectrumChannels;
        if (packet.type == PacketType::ChannelEvents) {
            spectrumChannels.reserve(packet.channelEvents.size());
            for (const auto &event : packet.channelEvents) {
                if (event.channel >= m_capabilities.channelCount) {
                    m_state.complete = false;
                    emit errorOccurred(QStringLiteral(
                        "收到超出设备能力范围的通道 %1 事件，已丢弃")
                        .arg(static_cast<int>(event.channel)));
                    continue;
                }
                spectrumChannels.push_back(event);
            }
            if (m_typedNotifications) typed.reserve(spectrumChannels.size());
            if (m_textWriter.isOpen()) {
                QString error;
                if (!m_textWriter.enqueue(spectrumChannels, &error)) {
                    m_state.eventTextError = error;
                    fail(error);
                    return;
                }
            }
            if (m_typedNotifications)
                for (const auto &event : spectrumChannels) typed.push_back(event);
        } else if (packet.type == PacketType::ModuleEvents) {
            typed.reserve(packet.moduleEvents.size());
            m_accumulator->addModuleObservables(packet.moduleEvents);
            for (const auto &event : packet.moduleEvents) {
                if (m_typedNotifications) typed.push_back(event);
                spectrumChannels += moduleChannels(event);
            }
        } else if (packet.type == PacketType::CoincidenceEvents) {
            typed.reserve(packet.coincidenceEvents.size());
            m_accumulator->addCoincidenceObservables(packet.coincidenceEvents);
            QVector<ModuleEvent> modules;
            modules.reserve(packet.coincidenceEvents.size() * 2);
            for (const auto &event : packet.coincidenceEvents) {
                if (m_typedNotifications) typed.push_back(event);
                modules.push_back(event.scatter);
                modules.push_back(event.absorber);
                spectrumChannels += moduleChannels(event.scatter);
                spectrumChannels += moduleChannels(event.absorber);
            }
            m_accumulator->addModuleObservables(modules);
        }

        for (const auto &event : spectrumChannels) {
            if (event.pileup)
                ++m_state.pileupEvents;
        }
        m_accumulator->add(spectrumChannels, m_includePileupInSpectrum);
        m_state.totalEvents += quint64(packet.type==PacketType::ChannelEvents ? spectrumChannels.size() : packet.eventCount());
        if (!typed.isEmpty()) {
            m_pendingTyped+=typed;
            if (m_pendingTyped.size()>=4096) flushTyped();
        }

        Q_UNUSED(raw);
        if (shouldStop())
            stop();
    }

    bool shouldStop() const {
        const double seconds = m_elapsed.elapsed() / 1000.0;
        // Continuous acquisition has no implicit event/count-rate ceiling.
        // Stop only for an explicitly selected count or time preset.
        return (m_preset.useEventCount &&
                m_state.totalEvents >= m_preset.eventCount)
            || (m_preset.useRealTime &&
                seconds >= m_preset.realTimeSeconds)
            || (m_preset.useLiveTime &&
                seconds >= m_preset.liveTimeSeconds);
    }

    void finishStop() {
        flushTyped();
        m_generate->stop();
        m_serialFinalFlush->stop();
        m_serialAwaitFinalFlush = false;
        m_serialRunConfirmed = false;
        if (m_pendingWaveform) {
            emit waveformReady(m_pendingWaveform);
            m_pendingWaveform.reset();
        }
        completeControlOperation();
        m_textWriter.finish(m_state.complete,
                            m_state.complete ? QString()
                                             : QStringLiteral("run incomplete"));
        m_state.eventTextEvents = m_textWriter.writtenEvents();
        m_state.state = AcquisitionState::Idle;
        if (m_state.status != QStringLiteral("已停止（最终能谱超时，数据不完整）"))
            m_state.status = QStringLiteral("已停止");
        publish();
        m_publish->stop();
    }

    void flushTyped() {
        if (m_pendingTyped.isEmpty()) return;
        emit typedEventsReady(m_pendingTyped);
        m_pendingTyped.clear();
    }
    void publish() {
        flushTyped();
        if (m_pendingWaveform &&
            (!m_waveformUiElapsed.isValid() ||
             m_waveformUiElapsed.elapsed() >= WaveformUiMilliseconds)) {
            emit waveformReady(m_pendingWaveform);
            m_pendingWaveform.reset();
            m_waveformUiElapsed.restart();
            publishWaveformStatus();
        }
        m_state.realSeconds = m_elapsed.isValid()
                            ? m_elapsed.elapsed() / 1000.0 : 0;
        m_state.liveSeconds = m_state.realSeconds;
        m_state.pileupFraction = m_state.totalEvents
            ? double(m_state.pileupEvents) / double(m_state.totalEvents) : 0;
        const double dt = m_rateElapsed.isValid()
                        ? m_rateElapsed.restart() / 1000.0 : 0;
        if (dt > 0 && !m_serialMode) {
            m_state.eventsPerSecond =
                (m_state.totalEvents - m_lastRateEvents) / dt;
            m_state.packetsPerSecond =
                (m_state.packets.received - m_lastRatePackets) / dt;
            m_state.udpMegabitsPerSecond =
                (m_state.packets.datagramBytes * 8.0 / 1.0e6) /
                qMax(1.0, m_state.realSeconds);
            m_lastRateEvents = m_state.totalEvents;
            m_lastRatePackets = m_state.packets.received;
        }
        m_state.mode = m_mode;
        m_state.capabilities = m_capabilities;
        m_state.configVersion = m_configVersion;
        emit spectrumReady(m_accumulator->snapshot());
        emit statusReady(m_state);
    }

    void fail(const QString &message) {
        const bool serialBaselineApplyFailed =
            m_serialBaselineApplyPending;
        m_generate->stop();
        m_waveformPoll->stop();
        m_serialHelloRetry->stop();
        m_serialFinalFlush->stop();
        m_serialHelloPending = false;
        m_serialAwaitFinalFlush = false;
        m_serialRunConfirmed = false;
        m_serialBaselineApplyPending = false;
        m_serialAssembler.clear();
        if (m_serialPort)
            m_serialPort->close();
        abortControlOperation();
        emit hardwareCommunicationChanged(false);
        // finish() also closes a writer whose accepting flag was cleared by
        // a queue overflow or disk error. This drains all already-queued
        // batches and writes the incomplete footer whenever the filesystem
        // still permits it.
        m_textWriter.finish(false, message);
        m_state.eventTextEvents = m_textWriter.writtenEvents();
        if (!m_simulation && !m_serialMode && m_dataSocket && m_controlPort) {
            const QByteArray stopFrame = makeControlRequest(
                ControlOpcode::Stop, ControlTarget::Global, 0, 0, 0,
                nextTransaction(), 0);
            m_dataSocket->writeDatagram(stopFrame, m_device, m_controlPort);
        }
        m_state.state = AcquisitionState::Error;
        m_state.status = message;
        m_state.complete = false;
        emit errorOccurred(message);
        if (serialBaselineApplyFailed)
            emit baselineTrackingApplyFinished(
                m_requestedBaselineTracking, false, message);
        publish();
    }

    std::unique_ptr<SpectrumAccumulator> m_accumulator;
    QVector<ChannelConfig> m_channels;
    QVector<ModuleConfig> m_modules;
    QVector<PairConfig> m_pairs;
    DeviceCapabilities m_capabilities;
    AcquisitionMode m_mode = AcquisitionMode::SingleChannel;
    AcquisitionPreset m_preset;
    AcquisitionSnapshot m_state;
    QTimer *m_generate = nullptr;
    QTimer *m_publish = nullptr;
    QTimer *m_controlRetry = nullptr;
    QTimer *m_serialHelloRetry = nullptr;
    QTimer *m_serialFinalFlush = nullptr;
    QElapsedTimer m_elapsed;
    QElapsedTimer m_rateElapsed;
    QUdpSocket *m_dataSocket = nullptr;
    QSerialPort *m_serialPort = nullptr;
    SerialPortFactory m_serialPortFactory;
    SerialFrameCodec m_serialCodec;
    SerialSpectrumAssembler m_serialAssembler;
    SerialHello m_serialHello;
    bool m_serialMode = false;
    bool m_serialHelloPending = false;
    bool m_serialHelloAccepted = false;
    bool m_serialRunConfirmed = false;
    bool m_serialAwaitFinalFlush = false;
    bool m_serialBaselineApplyPending = false;
    SerialHelloAction m_afterSerialHello = SerialHelloAction::StartRun;
    int m_serialHelloAttempts = 0;
    quint32 m_serialTxSequence = 0;
    QHostAddress m_device;
    quint16 m_controlPort = 0;
    quint16 m_dataPort = 0;
    bool m_simulation = true;
    bool m_baselineTracking = false;
    bool m_requestedBaselineTracking = false;
    bool m_baselineValidationRecoveryAttempted = false;
    int m_frontendCommitBusyRetries = 0;
    quint16 m_manualBaseline = 8192;
    quint16 m_requestedManualBaseline = 8192;
    quint16 m_recoveryTolerance = 32;
    quint16 m_requestedRecoveryTolerance = 32;
    bool m_bandwidthEnabled = false;
    bool m_requestedBandwidthEnabled = false;
    bool m_peakInterpolationEnabled = false;
    bool m_requestedPeakInterpolation = false;
    bool m_frontendConfigRequested = false;
    quint8 m_inputRange = 1;
    quint8 m_requestedRange = 1;
    quint32 m_externalDurationUs = 1000;
    bool m_includePileupInSpectrum = true;
    quint32 m_sequence = 0;
    quint32 m_configVersion = 0;
    quint32 m_nextTransaction = 0;
    quint32 m_simEventId = 0;
    quint64 m_simTimestamp = 0;
    quint64 m_lastRateEvents = 0;
    quint64 m_lastRatePackets = 0;
    bool m_datagramFollowupQueued = false;
    QElapsedTimer m_parseErrorLog;
    SequenceWindow m_sequenceWindow;
    bool m_typedNotifications=false;
    QVector<TypedEvent> m_pendingTyped;
    QString m_pendingTextPath;
    QQueue<QueuedControl> m_controlQueue;
    QVector<QueuedControl> m_startConfiguration;
    bool m_startConfigurationRepeated = false;
    int m_startPhase = 0;
    bool m_discardPrestartData = false;
    quint32 m_officialConfigVersion = 0;
    QTimer *m_startRepeatTimer = nullptr;
    std::optional<QueuedControl> m_pendingControl;
    ControlIntent m_controlIntent = ControlIntent::None;
    EventTextWriter m_textWriter;

    // ---- waveform mode ----
    WaveformReassembler m_waveformReassembler;
    ChannelConfig m_waveformConfig;
    WaveformTriggerMode m_waveformTriggerMode = WaveformTriggerMode::Continuous;
    WaveformStatus m_waveformStatus;
    quint32 m_waveformSession = 0;
    bool m_waveformPollPending = false;
    bool m_capabilitiesScanned = false;
    bool m_waveformSingleArmed = false;
    QDateTime m_waveformSingleDeadline;
    QTimer *m_waveformPoll = nullptr;
    QElapsedTimer m_waveformLastPoll;
    QElapsedTimer m_waveformUiElapsed;
    std::shared_ptr<const WaveformCapture> m_pendingWaveform;
};

AcquisitionController::AcquisitionController(QObject *parent)
    : AcquisitionController(SerialPortFactory{}, parent) {}

AcquisitionController::AcquisitionController(SerialPortFactory serialFactory,
                                             QObject *parent)
    : QObject(parent) {
    qRegisterMetaType<std::shared_ptr<const SpectrumSnapshot>>();
    qRegisterMetaType<AcquisitionSnapshot>();
    qRegisterMetaType<AcquisitionPreset>();
    qRegisterMetaType<ChannelConfig>();
    qRegisterMetaType<ModuleConfig>();
    qRegisterMetaType<PairConfig>();
    qRegisterMetaType<BaselineSnapshot>();
    qRegisterMetaType<DeviceCapabilities>();
    qRegisterMetaType<DeviceStatus>();
    qRegisterMetaType<QVector<TypedEvent>>();
    qRegisterMetaType<QVector<PsdEvent>>();
    qRegisterMetaType<WaveformTriggerMode>();
    qRegisterMetaType<WaveformStatus>();
    qRegisterMetaType<std::shared_ptr<const WaveformCapture>>();

    m_worker = new AcquisitionWorker(std::move(serialFactory));
    m_worker->moveToThread(&m_thread);
    connect(this, &AcquisitionController::requestSimulation,
            m_worker, &AcquisitionWorker::startSimulation);
    connect(this, &AcquisitionController::requestUdp,
            m_worker, &AcquisitionWorker::startUdp);
    connect(this, &AcquisitionController::requestSerial,
            m_worker, &AcquisitionWorker::startSerial);
    connect(this, &AcquisitionController::requestReplay,
            m_worker, &AcquisitionWorker::replay);
    connect(this, &AcquisitionController::requestPause,
            m_worker, &AcquisitionWorker::pause);
    connect(this, &AcquisitionController::requestResume,
            m_worker, &AcquisitionWorker::resume);
    connect(this, &AcquisitionController::requestStop,
            m_worker, &AcquisitionWorker::stop);
    connect(this, &AcquisitionController::requestClear,
            m_worker, &AcquisitionWorker::clear);
    connect(this, &AcquisitionController::requestConfig,
            m_worker, &AcquisitionWorker::setConfig);
    connect(this, &AcquisitionController::requestChannelControl,
            m_worker, &AcquisitionWorker::sendChannelControl);
    connect(this, &AcquisitionController::requestThreshold,
            m_worker, &AcquisitionWorker::sendThreshold);
    connect(this, &AcquisitionController::requestAllThresholds,
            m_worker, &AcquisitionWorker::applyAllThresholds);
    connect(this, &AcquisitionController::requestBaselineTracking,
            m_worker, &AcquisitionWorker::applyBaselineTracking);
    connect(this, &AcquisitionController::requestBaselineTrackingSerial,
            m_worker, &AcquisitionWorker::applyBaselineTrackingSerial);
    connect(this, &AcquisitionController::requestIncludePileup,
            m_worker, &AcquisitionWorker::setIncludePileup);
    connect(this, &AcquisitionController::requestMode,
            m_worker, &AcquisitionWorker::setMode);
    connect(this, &AcquisitionController::requestExternalTriggerDurationUs,
            m_worker, &AcquisitionWorker::setExternalTriggerDurationUs);
    connect(this, &AcquisitionController::requestModuleConfig,
            m_worker, &AcquisitionWorker::setModuleConfig);
    connect(this, &AcquisitionController::requestPairConfig,
            m_worker, &AcquisitionWorker::setPairConfig);
    connect(this, &AcquisitionController::requestApplyModuleConfig,
            m_worker, &AcquisitionWorker::applyModuleConfig);
    connect(this, &AcquisitionController::requestApplyPairConfig,
            m_worker, &AcquisitionWorker::applyPairConfig);
    connect(this, &AcquisitionController::requestStartWaveform,
            m_worker, &AcquisitionWorker::startWaveform);
    connect(this, &AcquisitionController::requestWaveformConfig,
            m_worker, &AcquisitionWorker::updateWaveformConfig);
    connect(this, &AcquisitionController::requestWaveformTriggerMode,
            m_worker, &AcquisitionWorker::setWaveformTriggerMode);

    connect(m_worker, &AcquisitionWorker::spectrumReady, this,
            [this](std::shared_ptr<const SpectrumSnapshot> snapshot) {
        QMutexLocker lock(&m_deliveryMutex);
        m_latestSpectrum=std::move(snapshot);
        if (m_spectrumQueued) return;
        m_spectrumQueued=true;
        QMetaObject::invokeMethod(this,[this] {
            std::shared_ptr<const SpectrumSnapshot> latest;
            { QMutexLocker lock(&m_deliveryMutex);
              latest=std::move(m_latestSpectrum); m_spectrumQueued=false; }
            emit spectrumReady(std::move(latest));
        },Qt::QueuedConnection);
    },Qt::DirectConnection);
    connect(m_worker, &AcquisitionWorker::statusReady, this,
            [this](const AcquisitionSnapshot &snapshot) {
        QMutexLocker lock(&m_deliveryMutex);
        m_latestStatus=snapshot;
        if (m_statusQueued) return;
        m_statusQueued=true;
        QMetaObject::invokeMethod(this,[this] {
            AcquisitionSnapshot latest;
            { QMutexLocker lock(&m_deliveryMutex);
              latest=m_latestStatus; m_statusQueued=false; }
            emit statusReady(latest);
        },Qt::QueuedConnection);
    },Qt::DirectConnection);
    connect(m_worker, &AcquisitionWorker::baselineReady,
            this, &AcquisitionController::baselineReady);
    connect(m_worker, &AcquisitionWorker::capabilitiesReady,
            this, &AcquisitionController::capabilitiesReady);
    connect(m_worker, &AcquisitionWorker::deviceStatusReady,
            this, &AcquisitionController::deviceStatusReady);
    connect(m_worker, &AcquisitionWorker::psdEventsReady, this, &AcquisitionController::psdEventsReady,Qt::DirectConnection);
    connect(m_worker, &AcquisitionWorker::typedEventsReady,
            this, &AcquisitionController::typedEventsReady);
    connect(m_worker, &AcquisitionWorker::controlBusyChanged,
            this, &AcquisitionController::controlBusyChanged);
    connect(m_worker, &AcquisitionWorker::baselineTrackingApplyFinished,
            this, &AcquisitionController::baselineTrackingApplyFinished);
    connect(m_worker, &AcquisitionWorker::hardwareCommunicationChanged,
            this, &AcquisitionController::hardwareCommunicationChanged);
    connect(m_worker, &AcquisitionWorker::errorOccurred,
            this, &AcquisitionController::errorOccurred);
    connect(m_worker, &AcquisitionWorker::waveformReady,
            this, &AcquisitionController::waveformReady);
    connect(m_worker, &AcquisitionWorker::waveformStatusReady,
            this, &AcquisitionController::waveformStatusReady);
    m_thread.start();
}

AcquisitionController::~AcquisitionController() {
    QThread *ownerThread = QThread::currentThread();
    QMetaObject::invokeMethod(
        m_worker,
        [this, ownerThread] {
            m_worker->shutdown();
            m_worker->moveToThread(ownerThread);
        },
        Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
    delete m_worker;
}

void AcquisitionController::startSimulation(const AcquisitionPreset &preset,
                                             const QString &path) {
    emit requestSimulation(preset, path);
}
void AcquisitionController::startUdp(quint16 port, const QHostAddress &address,
                                     quint16 controlPort,
                                     const AcquisitionPreset &preset,
                                     const QString &path,
                                     bool baselineTracking) {
    startUdp(port, address, controlPort, preset, path, baselineTracking,
             32, false, 0);
}
void AcquisitionController::startSerial(const QString &portName,
                                        const AcquisitionPreset &preset,
                                        bool baselineTracking,
                                        quint16 recoveryTolerance,
                                        bool bandwidthEnabled,
                                        quint8 inputRange,
                                        bool peakInterpolationEnabled) {
    emit requestSerial(portName, preset, baselineTracking,
                       recoveryTolerance, bandwidthEnabled, inputRange,
                       peakInterpolationEnabled);
}
void AcquisitionController::startUdp(quint16 port, const QHostAddress &address,
                                     quint16 controlPort,
                                     const AcquisitionPreset &preset,
                                     const QString &path,
                                     bool baselineTracking,
                                     quint16 recoveryTolerance,
                                     bool bandwidthEnabled,
                                     quint8 inputRange,
                                     bool peakInterpolationEnabled) {
    emit requestUdp(port, address, controlPort, preset, path,
                    baselineTracking, recoveryTolerance,
                    bandwidthEnabled, inputRange, peakInterpolationEnabled);
}
void AcquisitionController::replay(const QString &path) {
    emit requestReplay(path);
}
void AcquisitionController::setTypedEventNotificationsEnabled(bool enabled) {
    QMetaObject::invokeMethod(m_worker,[w=m_worker,enabled] {
        w->setTypedEventNotificationsEnabled(enabled);
    },Qt::QueuedConnection);
}
void AcquisitionController::pause() { emit requestPause(); }
void AcquisitionController::resume() { emit requestResume(); }
void AcquisitionController::stop() { emit requestStop(); }
void AcquisitionController::clear() { emit requestClear(); }
void AcquisitionController::setChannelConfig(int channel,
                                              const ChannelConfig &config) {
    emit requestConfig(channel, config);
}
void AcquisitionController::sendChannelControl(int channel,
                                                const ChannelConfig &config) {
    emit requestChannelControl(channel, config);
}
void AcquisitionController::sendChannelThreshold(int channel,
                                                  quint16 threshold) {
    emit requestThreshold(channel, threshold);
}
void AcquisitionController::applyAllThresholds(quint16 threshold) {
    emit requestAllThresholds(threshold);
}
void AcquisitionController::applyBaselineTracking(
    quint16 port, const QHostAddress &device, quint16 controlPort,
    bool enabled, quint16 manualBaseline) {
    applyBaselineTracking(port, device, controlPort, enabled, manualBaseline,
                          32, false, 0, false);
}
void AcquisitionController::applyBaselineTracking(
    quint16 port, const QHostAddress &device, quint16 controlPort,
    bool enabled, quint16 manualBaseline, quint16 recoveryTolerance,
    bool bandwidthEnabled, quint8 inputRange,
    bool peakInterpolationEnabled) {
    emit requestBaselineTracking(port, device, controlPort, enabled,
                                 manualBaseline, recoveryTolerance,
                                 bandwidthEnabled, inputRange,
                                 peakInterpolationEnabled);
}
void AcquisitionController::applyBaselineTrackingSerial(
    const QString &portName, bool enabled, quint16 manualBaseline,
    quint16 recoveryTolerance, bool bandwidthEnabled, quint8 inputRange,
    bool peakInterpolationEnabled) {
    emit requestBaselineTrackingSerial(portName, enabled, manualBaseline,
                                       recoveryTolerance, bandwidthEnabled,
                                       inputRange, peakInterpolationEnabled);
}
void AcquisitionController::setIncludePileupInSpectrum(bool include) {
    emit requestIncludePileup(include);
}
void AcquisitionController::setAcquisitionMode(AcquisitionMode mode) {
    emit requestMode(mode);
}

void AcquisitionController::setExternalTriggerDurationUs(quint32 durationUs) {
    emit requestExternalTriggerDurationUs(qBound<quint32>(1, durationUs, 1'000'000));
}
void AcquisitionController::setModuleConfig(int module,
                                            const ModuleConfig &config) {
    emit requestModuleConfig(module, config);
}
void AcquisitionController::setPairConfig(int pair,
                                          const PairConfig &config) {
    emit requestPairConfig(pair, config);
}
void AcquisitionController::applyModuleConfig(
    int module, const ModuleConfig &config) {
    emit requestApplyModuleConfig(module, config);
}
void AcquisitionController::applyPairConfig(
    int pair, const PairConfig &config) {
    emit requestApplyPairConfig(pair, config);
}
void AcquisitionController::startWaveform(quint16 port,
                                          const QHostAddress &device,
                                          quint16 controlPort,
                                          WaveformTriggerMode mode,
                                          const ChannelConfig &config) {
    emit requestStartWaveform(port, device, controlPort, mode, config);
}
void AcquisitionController::updateWaveformConfig(const ChannelConfig &config) {
    emit requestWaveformConfig(config);
}
void AcquisitionController::setWaveformTriggerMode(WaveformTriggerMode mode) {
    emit requestWaveformTriggerMode(mode);
}

} // namespace mca
#include "acquisition.moc"
