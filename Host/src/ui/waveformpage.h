#pragma once
#include "core/types.h"
#include "ui/waveformview.h"
#include <QWidget>
#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;

namespace mca {

// The complete waveform page: shared threshold and polarity controls, the
// continuous/single selector, start/stop, the display, the status line and the
// Time/div, ADC/div, position, full view, Y auto scale and CSV export tools.
//
// The page owns no acquisition logic: it emits intent signals and renders the
// immutable records it is given, so the worker thread stays the only place
// that talks to the device.
class WaveformPage : public QWidget {
    Q_OBJECT
public:
    explicit WaveformPage(QWidget *parent = nullptr);

    void setCapture(std::shared_ptr<const WaveformCapture> capture);
    std::shared_ptr<const WaveformCapture> capture() const {
        return m_view->capture();
    }
    void setStatus(const WaveformStatus &status);
    void setRunning(bool running);
    void setBusy(bool busy);
    void setCapability(bool supported, const QString &reason = {});

    ChannelConfig channelConfig() const;
    void setChannelConfig(const ChannelConfig &config);
    WaveformTriggerMode triggerMode() const;

signals:
    void startRequested(WaveformTriggerMode mode, ChannelConfig config);
    void stopRequested();
    void configChanged(ChannelConfig config);
    void triggerModeChanged(WaveformTriggerMode mode);
    void exportRequested();

private slots:
    void onStartClicked();
    void onStopClicked();
    void applyPendingConfig();

private:
    void buildUi();
    void refreshStateText();
    void updateControlState();

    WaveformView *m_view = nullptr;
    QSpinBox *m_threshold = nullptr;
    QComboBox *m_polarity = nullptr;
    QComboBox *m_triggerMode = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_stop = nullptr;
    QPushButton *m_export = nullptr;
    QComboBox *m_timebase = nullptr;
    QComboBox *m_vertical = nullptr;
    QSlider *m_horizontal = nullptr;
    QSlider *m_verticalOffset = nullptr;
    QCheckBox *m_autoScale = nullptr;
    QPushButton *m_fullView = nullptr;
    QLabel *m_state = nullptr;
    QLabel *m_counts = nullptr;
    QLabel *m_captureInfo = nullptr;
    QLabel *m_capability = nullptr;

    WaveformStatus m_status;
    bool m_running = false;
    bool m_busy = false;
    bool m_supported = true;
    bool m_updating = false;
};

} // namespace mca
