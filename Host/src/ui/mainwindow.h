#pragma once
#include "app/acquisition.h"
#include "analysis/analysis.h"
#include <QMainWindow>
#include <QPointer>
#include <array>

class QComboBox; class QSpinBox; class QTableWidget; class QLabel; class QProgressBar;
class QCheckBox; class QStackedWidget; class QActionGroup; class QAction; class QMenu;
class QDoubleSpinBox; class QLineEdit;
class QPushButton;
class QDockWidget;

namespace mca {

class SpectrumView;
class EventMapView;
class AcquisitionPresetWidget;
class WaveformPage;
class PsdPage;

class MainWindow : public QMainWindow {
    friend class ThroughputBenchmark;
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    // Acquisition
    void startAcquisition();
    void startUdp();
    void togglePause();
    void stopAcquisition();
    void clearSpectrum();

    // Waveform mode
    void startWaveform(WaveformTriggerMode mode, ChannelConfig config);
    void applyWaveformConfig(ChannelConfig config);
    void exportWaveformCsv();
    void updateWaveformStatus(const WaveformStatus &status);

    // Spectrum display
    void updateSpectrum(std::shared_ptr<const SpectrumSnapshot>);
    void updateStatus(const AcquisitionSnapshot &);
    void changeView();
    void applyDisplaySettings();

    // Channel
    void applyChannelConfig(int row, int column);
    void sendChannelThreshold(int row);
    void applyGlobalThreshold();
    void applyBaselineTracking();
    void updateBaselineStatus(const BaselineSnapshot &snapshot);
    void updateCapabilities(const DeviceCapabilities &capabilities);
    void updateDeviceStatus(const DeviceStatus &status);
    void applyModuleConfig(int row);
    void applyPairConfig(int row);

    // ROI
    void addRoi();
    void addRoiFromGraph(int left, int right);
    void deleteSelectedRoi();
    void clearAllRois();
    void modifyRoi(int index, int left, int right);
    void selectRoi(int index);
    void fitCurrentRoi();
    void syncRoisToView();

    // Fitting
    void fitRoi();
    void clearFit();

    // Calibration
    void calibrate();

    // File
    void saveSpectrum();
    void openSpectrum();
    void openList();
    void chooseEventTextFile();

    // Window
    void restoreDefaultLayout();

    // Helpers
    void updateActionsForState();
    void refreshRoiTable();
    void refreshRoiInfo();

private:
    void buildUi();
    void buildMenus();
    void buildToolBars();
    void buildStatusBar();
    void buildDockWidgets();
    void createActions();
    void connectSignals();

    // Data helpers
    QVector<quint64> currentData() const;
    int maximumSpectrumChannel(SpectrumKind kind, int channel) const;
    QVector<int> selectedChannels() const;
    void activateChannelRow(int row);
    ChannelConfig channelConfigFromRow(int row) const;
    void saveChannelConfig(int row);
    void commitIntegrationWindowEditor();
    void updateIntegrationWindowSummary();
    void ensureChannelRows(int count);
    void ensureModuleRows(int count);
    void ensurePairRows(int count);
    QSpinBox *channelThresholdWidget(int row) const;
    QComboBox *channelPolarityWidget(int row) const;
    QLabel *channelBaselineValueWidget(int row) const;
    QLabel *channelBaselineStateWidget(int row) const;
    ModuleConfig moduleConfigFromRow(int row) const;
    PairConfig pairConfigFromRow(int row, bool updateUi = true);
    bool validateAcquisitionConfiguration(QString *error) const;

    // ROI contexts
    struct RoiContext {
        QVector<Roi> rois;
        int selected = -1;
    };
    int activeRoiChannel() const;
    int roiContextIndex(int channel, SpectrumKind kind) const;
    void storeActiveRoiContext();
    void loadRoiContext(int channel, SpectrumKind kind);
    void updateRoiAvailability();

    // UI helpers
    void applyDefaultDockLayout();
    void updateModeDependentPanels();
    void ensureWindowOnScreen();
    void updateEventTextAvailability();
    void addRecentFile(const QString &path);
    void rebuildRecentFilesMenu();

    // Settings
    void saveSettings();
    void restoreSettings();

    // ── Core ──
    AcquisitionController m_acq;
    std::shared_ptr<const SpectrumSnapshot> m_snapshot;
    Calibration m_peakCal, m_integralCal;
    AcquisitionState m_acquisitionState = AcquisitionState::Idle;
    bool m_controlBusy = false;
    bool m_hardwareResponsive = false;
    bool m_confirmedBaselineTracking = false;
    bool m_baselineTrackingDirty = false;
    // A status packet may have been queued before the latest baseline commit.
    // Keep the confirmed choice until a post-commit packet agrees with it.
    bool m_baselineTelemetrySyncPending = false;
    bool m_baselineTelemetryTarget = false;
    // Keep a manually entered baseline from being overwritten by a queued
    // status packet that still carries the previous FPGA value.
    bool m_manualBaselineDirty = false;
    bool m_manualBaselineTelemetrySyncPending = false;
    quint16 m_manualBaselineTelemetryTarget = 8192;
    quint16 m_confirmedManualBaseline = 8192;
    quint16 m_confirmedRecoveryTolerance = 32;
    quint8 m_confirmedInputRange = 1;
    bool m_confirmedBandwidthEnabled = false;
    bool m_confirmedPeakInterpolationEnabled = false;
    bool m_frontendTelemetrySyncPending = false;
    quint16 m_frontendTelemetryRecoveryTarget = 32;
    quint8 m_frontendTelemetryRangeTarget = 1;
    bool m_frontendTelemetryBandwidthTarget = false;
    bool m_frontendTelemetryPeakInterpolationTarget = false;
    DeviceCapabilities m_deviceCapabilities;
    DeviceStatus m_lastDeviceStatus;
    bool m_deviceStatusValid = false;
    // Default values are not a device response.  Until the first capability
    // scan completes, allow waveform start to perform that scan.
    bool m_deviceCapabilitiesKnown = false;
    bool m_closeAfterStop = false;
    int m_displayedChannelRow = 0;
    QVector<int> m_displayedChannelRows{0};

    // ── Central area ──
    QStackedWidget *m_center = nullptr;
    SpectrumView *m_spectrum = nullptr;
    EventMapView *m_eventMap = nullptr;
    PsdPage *m_psdPage = nullptr;
    int m_psdPageIndex = -1;
    WaveformPage *m_waveformPage = nullptr;
    int m_waveformPageIndex = -1;
    int m_spectrumPageIndex = 0;
    int m_gridPageIndex = 1;
    int m_eventMapPageIndex = 2;
    QVector<SpectrumView *> m_gridViews;

    // ── Actions (shared between menus and toolbars) ──
    // File
    QAction *m_actOpen = nullptr, *m_actSave = nullptr;
    QAction *m_actOpenList = nullptr, *m_actExit = nullptr, *m_actSetEventText = nullptr;
    // Acquisition
    QAction *m_actStart = nullptr, *m_actPause = nullptr, *m_actStop = nullptr;
    QAction *m_actClear = nullptr, *m_actConnect = nullptr;
    // Display
    QAction *m_actLinearY = nullptr, *m_actLogY = nullptr;
    QAction *m_actFill = nullptr;
    QAction *m_actLegend = nullptr, *m_actOverview = nullptr, *m_actGrid = nullptr;
    QAction *m_actRoiLabels = nullptr;
    QAction *m_actFullRange = nullptr, *m_actZoomIn = nullptr, *m_actZoomOut = nullptr;
    QAction *m_actSingleCh = nullptr, *m_actMultiCh = nullptr;
    QAction *m_actGrid8 = nullptr;
    // Interaction mode
    QAction *m_actCursorMode = nullptr, *m_actZoomMode = nullptr, *m_actPanMode = nullptr;
    QAction *m_actCreateRoiMode = nullptr, *m_actEditRoiMode = nullptr;
    // ROI
    QAction *m_actNewRoi = nullptr, *m_actDeleteRoi = nullptr, *m_actClearRois = nullptr;
    QAction *m_actPrevRoi = nullptr, *m_actNextRoi = nullptr;
    // Analysis
    QAction *m_actSingleFit = nullptr, *m_actClearFit = nullptr;
    // Calibration
    QAction *m_actEnergyCal = nullptr;
    QAction *m_actDefaultLayout = nullptr;
    // Help
    QAction *m_actAbout = nullptr;

    // ── Widgets ──
    AcquisitionPresetWidget *m_presetWidget = nullptr;
    QComboBox *m_kind = nullptr, *m_layout = nullptr, *m_background = nullptr;
    QComboBox *m_acquisitionMode = nullptr;
    QComboBox *m_connectionType = nullptr, *m_serialPort = nullptr;
    QSpinBox *m_roiLeft = nullptr, *m_roiRight = nullptr, *m_peakCount = nullptr;
    QSpinBox *m_globalThreshold = nullptr;
    QDoubleSpinBox *m_integrationPostUs = nullptr, *m_integrationPreUs = nullptr;
    QSpinBox *m_externalDurationUs = nullptr;
    QCheckBox *m_baselineTracking = nullptr;
    QSpinBox *m_manualBaseline = nullptr;
    QSpinBox *m_recoveryTolerance = nullptr;
    QComboBox *m_inputRange = nullptr;
    QCheckBox *m_bandwidthLimit = nullptr;
    QCheckBox *m_peakInterpolation = nullptr;
    QLabel *m_frontendStatus = nullptr;
    QPushButton *m_applyBaseline = nullptr;
    QTableWidget *m_channels = nullptr, *m_modules = nullptr, *m_pairs = nullptr;
    QTableWidget *m_roiTable = nullptr;
    QLabel *m_roiInfo = nullptr, *m_fitInfo = nullptr, *m_integrationSummary = nullptr;
    QLabel *m_statusState = nullptr, *m_statusCh = nullptr, *m_statusLive = nullptr;
    QLabel *m_statusDead = nullptr, *m_statusRate = nullptr, *m_statusPkt = nullptr;
    QLabel *m_statusLoss = nullptr, *m_statusPileup = nullptr, *m_statusCursor = nullptr;
    QLabel *m_statusTextFile = nullptr;
    QLabel *m_statusDevice = nullptr;
    QProgressBar *m_queueBar = nullptr;
    QWidget *m_statusPanel = nullptr;
    QCheckBox *m_saveEventText = nullptr, *m_includePileup = nullptr;
    QLineEdit *m_eventTextPath = nullptr;
    QPushButton *m_chooseEventText = nullptr;

    // Docks
    QDockWidget *m_connectionDock = nullptr, *m_channelDock = nullptr;
    QDockWidget *m_acquisitionDock = nullptr, *m_moduleDock = nullptr;
    QDockWidget *m_pairDock = nullptr, *m_analysisDock = nullptr;

    // Menus
    QMenu *m_recentFilesMenu = nullptr;
    QMenu *m_panelsMenu = nullptr;

    // Settings
    QStringList m_recentFiles;
    QString m_lastDirectory;
    static constexpr int MaxRecentFiles = 8;

    // Fitting
    quint64 m_fitRequestId = 0;
    QPointer<QObject> m_fitContext;

    std::array<RoiContext, MaxChannels * 2> m_roiContexts;
    int m_activeRoiChannel = 0;
    SpectrumKind m_activeRoiKind = SpectrumKind::Peak;
    bool m_loadingRoiContext = false;
};
} // namespace mca
