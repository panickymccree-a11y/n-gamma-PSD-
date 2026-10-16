#pragma once
#include "core/types.h"
#include <QWidget>
#include <memory>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;

namespace mca {

// ---------------------------------------------------------------------------
// Oscilloscope-style waveform page.
//
// The record is fixed at 12500 samples of 8 ns and the trigger sample always
// sits at index 3750, i.e. -30 us .. +69.992 us around the trigger.  The view
// keeps the raw samples untouched:
//
//   * the horizontal scale is always ten divisions, so the timebase list
//     8 ns/div .. 10 us/div spans 80 ns .. 100 us on screen;
//   * changing the timebase keeps the trigger at 30 % of the screen width by
//     default and then clamps the window to the record;
//   * dense views draw a per-pixel min/max envelope so a narrow peak can
//     never disappear, and sparse views draw the samples plus a polyline;
//   * the vertical scale snaps to the documented ADC/div steps and auto scale
//     picks the smallest step that fits the current record with 10 % margin.
//
// Drawing only ever uses floating point for pixel coordinates; sample indices
// and sample times are integer arithmetic.
// ---------------------------------------------------------------------------
class WaveformView : public QWidget {
    Q_OBJECT
public:
    explicit WaveformView(QWidget *parent = nullptr);

    void setCapture(std::shared_ptr<const WaveformCapture> capture);
    std::shared_ptr<const WaveformCapture> capture() const { return m_capture; }

    // Display controls (all of them persist across records).
    void setTimebaseNsPerDiv(double nsPerDiv);
    double timebaseNsPerDiv() const { return m_timebaseNsPerDiv; }
    void setAdcPerDiv(int adcPerDiv);
    int adcPerDiv() const { return m_adcPerDiv; }
    void setHorizontalPosition(double fractionOfRecord);
    double horizontalPosition() const { return m_horizontalPosition; }
    void setVerticalPosition(int adcOffset);
    int verticalPosition() const { return m_verticalPosition; }
    void setAutoScale(bool enabled);
    bool autoScale() const { return m_autoScale; }
    void showFullRecord();
    void autoScaleVertical();

    static const QVector<double> &timebaseSteps();
    static const QVector<int> &adcSteps();

signals:
    void captureInfoChanged(const QString &text);
    void cursorMoved(int sampleIndex, int adcCode, qint64 timeNs, bool overrange);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct Window {
        double firstIndex = 0.0;   // sample index at the left edge
        double lastIndex = 0.0;    // sample index at the right edge
        int adcTop = 0;
        int adcBottom = 0;
    };

    Window computeWindow() const;
    void clampWindow();
    int adcToY(const Window &window, double adc, int plotTop,
               int plotHeight) const;
    double indexToX(const Window &window, double index, int plotLeft,
                    int plotWidth) const;
    void drawGrid(QPainter &painter, const QRect &plot, const Window &window);
    void drawTrace(QPainter &painter, const QRect &plot, const Window &window);
    void drawMarkers(QPainter &painter, const QRect &plot, const Window &window);
    void drawBadges(QPainter &painter, const QRect &plot);
    void emitCaptureInfo();

    std::shared_ptr<const WaveformCapture> m_capture;
    double m_timebaseNsPerDiv = 10'000.0;   // default 10 us/div
    int m_adcPerDiv = 2048;                 // default 2048 ADC/div
    double m_horizontalPosition = 0.0;      // 0 = default anchored view
    int m_verticalPosition = 0;
    bool m_autoScale = false;
    bool m_panning = false;
    double m_panStartX = 0.0;
    double m_panStartPosition = 0.0;
    bool m_hasCursor = false;
    // Cached view window.  Recomputing it in every paint keeps the drawing
    // code free of hidden state changes.
    Window m_windowCache;
    bool m_windowCacheValid = false;
};

} // namespace mca
