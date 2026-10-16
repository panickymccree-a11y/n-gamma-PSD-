#pragma once
#include "core/protocol.h"
#include "core/types.h"
#include "core/waveform.h"
#include <QObject>
#include <QHostAddress>
#include <QThread>
#include <QMutex>
#include <functional>
#include <memory>

class QSerialPort;

namespace mca {
class AcquisitionWorker;

// Internal-only seam for host tests.  Production code leaves this empty and
// the worker creates a real QSerialPort; tests may supply a memory-backed
// QSerialPort subclass without exposing a replacement-device control in the
// UI.
using SerialPortFactory = std::function<::QSerialPort *(QObject *)>;

class AcquisitionController : public QObject {
    Q_OBJECT
public:
    explicit AcquisitionController(QObject *parent = nullptr);
    explicit AcquisitionController(SerialPortFactory serialFactory,
                                   QObject *parent = nullptr);
    ~AcquisitionController() override;
    void startSimulation(const AcquisitionPreset &preset, const QString &listPath = {});
    void startUdp(quint16 dataPort, const QHostAddress &device, quint16 controlPort,
                  const AcquisitionPreset &preset, const QString &eventTextPath,
                  bool baselineTrackingEnabled);
    void startUdp(quint16 dataPort, const QHostAddress &device, quint16 controlPort,
                  const AcquisitionPreset &preset, const QString &eventTextPath,
                  bool baselineTrackingEnabled, quint16 recoveryTolerance,
                  bool bandwidthEnabled, quint8 inputRange,
                  bool peakInterpolationEnabled = false);
    void startSerial(const QString &portName,
                     const AcquisitionPreset &preset,
                     bool baselineTrackingEnabled,
                     quint16 recoveryTolerance,
                     bool bandwidthEnabled, quint8 inputRange,
                     bool peakInterpolationEnabled = false);
    void replay(const QString &path);
    void pause(); void resume(); void stop(); void clear();
    void setTypedEventNotificationsEnabled(bool enabled);
    void setChannelConfig(int channel, const ChannelConfig &config);
    void sendChannelControl(int channel, const ChannelConfig &config);
    void sendChannelThreshold(int channel, quint16 thresholdLsb);
    void applyAllThresholds(quint16 thresholdLsb);
    void applyBaselineTracking(quint16 dataPort, const QHostAddress &device,
                               quint16 controlPort, bool enabled,
                               quint16 manualBaseline = 8192);
    void applyBaselineTracking(quint16 dataPort, const QHostAddress &device,
                               quint16 controlPort, bool enabled,
                               quint16 manualBaseline, quint16 recoveryTolerance,
                               bool bandwidthEnabled, quint8 inputRange,
                               bool peakInterpolationEnabled = false);
    void applyBaselineTrackingSerial(const QString &portName, bool enabled,
                                     quint16 manualBaseline,
                                     quint16 recoveryTolerance,
                                     bool bandwidthEnabled, quint8 inputRange,
                                     bool peakInterpolationEnabled = false);
    void setIncludePileupInSpectrum(bool include);
    void setAcquisitionMode(AcquisitionMode mode);
    void setExternalTriggerDurationUs(quint32 durationUs);
    void setModuleConfig(int module, const ModuleConfig &config);
    void setPairConfig(int pair, const PairConfig &config);
    void applyModuleConfig(int module, const ModuleConfig &config);
    void applyPairConfig(int pair, const PairConfig &config);

    // Waveform mode.  `startWaveform` stops any running acquisition, commits
    // the shared configuration plus the new session id, and then starts the
    // device with the confirmed configuration version.
    void startWaveform(quint16 dataPort, const QHostAddress &device,
                       quint16 controlPort, WaveformTriggerMode mode,
                       const ChannelConfig &config);
    void updateWaveformConfig(const ChannelConfig &config);
    void setWaveformTriggerMode(WaveformTriggerMode mode);
signals:
    void spectrumReady(std::shared_ptr<const SpectrumSnapshot> snapshot);
    void statusReady(const AcquisitionSnapshot &snapshot);
    void baselineReady(const BaselineSnapshot &snapshot);
    void capabilitiesReady(const DeviceCapabilities &capabilities);
    void deviceStatusReady(const DeviceStatus &status);
    void typedEventsReady(const QVector<TypedEvent> &events);
    void psdEventsReady(const QVector<PsdEvent> &events);
    void controlBusyChanged(bool busy);
    void baselineTrackingApplyFinished(bool enabled, bool success,
                                       const QString &message);
    void hardwareCommunicationChanged(bool responsive);
    void errorOccurred(const QString &message);
    // Waveform mode
    void waveformReady(std::shared_ptr<const WaveformCapture> capture);
    void waveformStatusReady(const WaveformStatus &status);
    void requestSimulation(AcquisitionPreset preset, QString listPath);
    void requestUdp(quint16 dataPort, QHostAddress device, quint16 controlPort,
                    AcquisitionPreset preset, QString eventTextPath,
                    bool baselineTrackingEnabled,
                    quint16 recoveryTolerance,
                    bool bandwidthEnabled,
                    quint8 inputRange,
                    bool peakInterpolationEnabled);
    void requestSerial(QString portName, AcquisitionPreset preset,
                       bool baselineTrackingEnabled,
                       quint16 recoveryTolerance,
                       bool bandwidthEnabled, quint8 inputRange,
                       bool peakInterpolationEnabled);
    void requestReplay(QString path);
    void requestPause(); void requestResume(); void requestStop(); void requestClear();
    void requestConfig(int channel, ChannelConfig config);
    void requestChannelControl(int channel, ChannelConfig config);
    void requestThreshold(int channel, quint16 thresholdLsb);
    void requestAllThresholds(quint16 thresholdLsb);
    void requestBaselineTracking(quint16 dataPort, QHostAddress device,
                                 quint16 controlPort, bool enabled,
                                 quint16 manualBaseline,
                                 quint16 recoveryTolerance,
                                 bool bandwidthEnabled,
                                 quint8 inputRange,
                                 bool peakInterpolationEnabled);
    void requestBaselineTrackingSerial(QString portName, bool enabled,
                                       quint16 manualBaseline,
                                       quint16 recoveryTolerance,
                                       bool bandwidthEnabled, quint8 inputRange,
                                       bool peakInterpolationEnabled);
    void requestIncludePileup(bool include);
    void requestMode(AcquisitionMode mode);
    void requestExternalTriggerDurationUs(quint32 durationUs);
    void requestModuleConfig(int module, ModuleConfig config);
    void requestPairConfig(int pair, PairConfig config);
    void requestApplyModuleConfig(int module, ModuleConfig config);
    void requestApplyPairConfig(int pair, PairConfig config);
    void requestStartWaveform(quint16 dataPort, QHostAddress device,
                              quint16 controlPort, WaveformTriggerMode mode,
                              ChannelConfig config);
    void requestWaveformConfig(ChannelConfig config);
    void requestWaveformTriggerMode(WaveformTriggerMode mode);
private:
    QMutex m_deliveryMutex;
    std::shared_ptr<const SpectrumSnapshot> m_latestSpectrum;
    AcquisitionSnapshot m_latestStatus;
    bool m_spectrumQueued=false, m_statusQueued=false;
    QThread m_thread;
    AcquisitionWorker *m_worker = nullptr;
};

} // namespace mca

Q_DECLARE_METATYPE(mca::AcquisitionPreset)
Q_DECLARE_METATYPE(mca::ChannelConfig)
Q_DECLARE_METATYPE(mca::ModuleConfig)
Q_DECLARE_METATYPE(mca::PairConfig)
Q_DECLARE_METATYPE(mca::WaveformTriggerMode)
Q_DECLARE_METATYPE(QVector<mca::TypedEvent>)
