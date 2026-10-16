#include "ui/waveformview.h"
#include "core/protocol.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QtGlobal>
#include <algorithm>
#include <cmath>

namespace mca {
namespace {

constexpr int HorizontalDivisions = 10;
constexpr int VerticalDivisions = 8;
constexpr double TriggerAnchorFraction = 0.30;   // T sits at 30 % by default

const QColor BackgroundColor("#101418");
const QColor GridColor("#2a3440");
const QColor CenterGridColor("#3d4c5c");
const QColor TraceColor("#6fd0ff");
const QColor EnvelopeColor("#3f93c9");
const QColor OverrangeColor("#ff5a52");
const QColor BaselineColor("#ffb000");
const QColor ThresholdColor("#7ee787");
const QColor TriggerColor("#d16bff");
const QColor TextColor("#c9d4dd");

} // namespace

const QVector<double> &WaveformView::timebaseSteps() {
    static const QVector<double> steps{
        8.0, 16.0, 40.0, 80.0, 160.0, 400.0,
        800.0, 1'600.0, 4'000.0, 8'000.0, 10'000.0
    };
    return steps;
}

const QVector<int> &WaveformView::adcSteps() {
    static const QVector<int> steps{
        1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048
    };
    return steps;
}

WaveformView::WaveformView(QWidget *parent) : QWidget(parent) {
    setMinimumSize(420, 260);
    setMouseTracking(true);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);
}

void WaveformView::setCapture(std::shared_ptr<const WaveformCapture> capture) {
    m_capture = std::move(capture);
    if (m_autoScale)
        autoScaleVertical();
    clampWindow();
    update();
    emitCaptureInfo();
}

void WaveformView::showFullRecord() {
    // The default full view is the documented -30 us .. +70 us, i.e.
    // 10 us/div with the trigger at 30 % of the screen.
    m_timebaseNsPerDiv = 10'000.0;
    m_horizontalPosition = 0.0;
    m_adcPerDiv = 2048;
    m_verticalPosition = 0;
    m_autoScale = false;
    clampWindow();
    update();
    emitCaptureInfo();
}

void WaveformView::setTimebaseNsPerDiv(double nsPerDiv) {
    const double requested = qBound(8.0, nsPerDiv, 10'000.0);
    if (qFuzzyCompare(requested, m_timebaseNsPerDiv))
        return;
    // Keep the trigger point where it is on screen (30 % by default) and only
    // then clamp the window into the record.
    const Window before = m_windowCacheValid ? m_windowCache : computeWindow();
    const double anchorX =
        (m_capture && m_capture->valid())
            ? (double(m_capture->triggerIndex()) - before.firstIndex)
              / qMax(1.0, before.lastIndex - before.firstIndex)
            : TriggerAnchorFraction;
    m_timebaseNsPerDiv = requested;
    if (m_capture && m_capture->valid()) {
        const double span = m_timebaseNsPerDiv * HorizontalDivisions
                          / double(m_capture->samplePeriodNs);
        m_horizontalPosition =
            double(m_capture->triggerIndex()) - anchorX * span;
    }
    clampWindow();
    update();
    emitCaptureInfo();
}

void WaveformView::setAdcPerDiv(int adcPerDiv) {
    int best = adcSteps().first();
    for (int step : adcSteps()) {
        if (step <= adcPerDiv)
            best = step;
    }
    m_adcPerDiv = best;
    m_autoScale = false;
    clampWindow();
    update();
    emitCaptureInfo();
}

void WaveformView::setHorizontalPosition(double firstIndex) {
    m_horizontalPosition = firstIndex;
    clampWindow();
    update();
    emitCaptureInfo();
}

void WaveformView::setVerticalPosition(int adcOffset) {
    m_verticalPosition = adcOffset;
    clampWindow();
    update();
    emitCaptureInfo();
}

void WaveformView::setAutoScale(bool enabled) {
    m_autoScale = enabled;
    if (enabled)
        autoScaleVertical();
    update();
    emitCaptureInfo();
}

void WaveformView::autoScaleVertical() {
    if (!m_capture || !m_capture->valid())
        return;
    int minimum = 0x3fff;
    int maximum = 0;
    for (int i = 0; i < m_capture->size(); ++i) {
        const int code = m_capture->adcCode(i);
        minimum = qMin(minimum, code);
        maximum = qMax(maximum, code);
    }
    // Include the latched baseline and threshold levels so they stay visible.
    minimum = qMin(minimum, int(m_capture->baseline));
    maximum = qMax(maximum, int(m_capture->baseline)
                                + int(m_capture->threshold));
    const int span = qMax(1, maximum - minimum);
    const int perDiv = int(std::ceil(double(span) * 1.1 / VerticalDivisions));
    int best = adcSteps().last();
    for (int step : adcSteps()) {
        if (step >= perDiv) {
            best = step;
            break;
        }
    }
    m_adcPerDiv = best;
    // Centre the visible data inside the eight divisions.
    const int centre = (minimum + maximum) / 2;
    m_verticalPosition = centre - (m_adcPerDiv * VerticalDivisions) / 2;
}

WaveformView::Window WaveformView::computeWindow() const {
    Window window;
    if (!m_capture || !m_capture->valid()) {
        window.firstIndex = -WaveformPreSamples;
        window.lastIndex = window.firstIndex + 12'500.0;
        window.adcTop = m_adcPerDiv * VerticalDivisions;
        window.adcBottom = 0;
        return window;
    }
    const double spanDivisions = m_timebaseNsPerDiv * HorizontalDivisions;
    const double span = spanDivisions / double(m_capture->samplePeriodNs);
    double first = 0.0;
    if (qFuzzyIsNull(m_horizontalPosition)) {
        // Default: trigger at 30 % of the screen, covering -30 us .. +70 us at
        // the default timebase.
        first = double(m_capture->triggerIndex()) - TriggerAnchorFraction * span;
    } else {
        first = m_horizontalPosition;
    }
    // The window is clamped to the record: there are no samples outside it.
    const double recordLast = double(m_capture->size() - 1);
    first = qBound(0.0, first, qMax(0.0, recordLast - span + 1.0));
    window.firstIndex = first;
    window.lastIndex = qMin(recordLast, first + span);
    window.adcBottom = m_verticalPosition;
    window.adcTop = m_verticalPosition + m_adcPerDiv * VerticalDivisions;
    return window;
}

void WaveformView::clampWindow() {
    if (!m_capture || !m_capture->valid())
        return;
    const double span = m_timebaseNsPerDiv * HorizontalDivisions
                      / double(m_capture->samplePeriodNs);
    if (!qFuzzyIsNull(m_horizontalPosition)) {
        const double recordLast = double(m_capture->size() - 1);
        m_horizontalPosition = qBound(0.0, m_horizontalPosition,
                                      qMax(0.0, recordLast - span + 1.0));
    }
    const int visible = m_adcPerDiv * VerticalDivisions;
    m_verticalPosition = qBound(0, m_verticalPosition,
                                qMax(0, WaveformAdcFullScale - visible));
    m_windowCache = computeWindow();
    m_windowCacheValid = true;
}

int WaveformView::adcToY(const Window &window, double adc, int plotTop,
                         int plotHeight) const {
    const double span = qMax(1.0, double(window.adcTop - window.adcBottom));
    const double fraction = (adc - double(window.adcBottom)) / span;
    return plotTop + plotHeight
         - int(std::lround(fraction * double(plotHeight)));
}

double WaveformView::indexToX(const Window &window, double index,
                              int plotLeft, int plotWidth) const {
    const double span = qMax(1.0, window.lastIndex - window.firstIndex);
    return double(plotLeft)
         + (index - window.firstIndex) / span * double(plotWidth);
}

void WaveformView::drawGrid(QPainter &painter, const QRect &plot,
                            const Window &window) {
    painter.fillRect(rect(), BackgroundColor);
    painter.save();
    painter.setClipRect(plot);
    for (int division = 0; division <= HorizontalDivisions; ++division) {
        const int x = plot.left()
                    + (plot.width() * division) / HorizontalDivisions;
        const bool centre = division == HorizontalDivisions / 2;
        painter.setPen(QPen(centre ? CenterGridColor : GridColor,
                            centre ? 1.4 : 1.0));
        painter.drawLine(x, plot.top(), x, plot.bottom());
    }
    for (int division = 0; division <= VerticalDivisions; ++division) {
        const int y = plot.top()
                    + (plot.height() * division) / VerticalDivisions;
        const bool centre = division == VerticalDivisions / 2;
        painter.setPen(QPen(centre ? CenterGridColor : GridColor,
                            centre ? 1.4 : 1.0));
        painter.drawLine(plot.left(), y, plot.right(), y);
    }
    painter.setPen(QPen(QColor("#4c5a68"), 1.0, Qt::DashLine));
    painter.drawRect(plot.adjusted(0, 0, -1, -1));
    painter.restore();
    Q_UNUSED(window);
}

void WaveformView::drawTrace(QPainter &painter, const QRect &plot,
                             const Window &window) {
    if (!m_capture || !m_capture->valid() || plot.width() <= 1)
        return;
    const auto &capture = *m_capture;
    const double span = qMax(1.0, window.lastIndex - window.firstIndex);
    const double samplesPerPixel = span / double(plot.width());
    const int firstVisible = qMax(0, int(std::floor(window.firstIndex)) - 1);
    const int lastVisible = qMin(capture.size() - 1,
                                 int(std::ceil(window.lastIndex)) + 1);

    painter.save();
    painter.setClipRect(plot);

    if (samplesPerPixel <= 1.5) {
        // Sparse view: draw the real samples and straight connecting lines.
        QPainterPath path;
        bool started = false;
        for (int i = firstVisible; i <= lastVisible; ++i) {
            const double x = indexToX(window, i, plot.left(), plot.width());
            const double y = adcToY(window, capture.adcCode(i), plot.top(),
                                    plot.height());
            if (!started) {
                path.moveTo(x, y);
                started = true;
            } else {
                path.lineTo(x, y);
            }
        }
        painter.setPen(QPen(TraceColor, 1.2));
        painter.drawPath(path);
        painter.setPen(QPen(TraceColor, 1.0));
        for (int i = firstVisible; i <= lastVisible; ++i) {
            const double x = indexToX(window, i, plot.left(), plot.width());
            const double y = adcToY(window, capture.adcCode(i), plot.top(),
                                    plot.height());
            painter.drawEllipse(QPointF(x, y), 1.6, 1.6);
        }
    } else {
        // Dense view: one min/max envelope per pixel column so that a narrow
        // peak can never be skipped.
        QPainterPath path;
        bool started = false;
        for (int x = 0; x <= plot.width(); ++x) {
            const double firstIndex = window.firstIndex + x * samplesPerPixel;
            const double nextIndex = firstIndex + samplesPerPixel;
            int begin = int(std::floor(firstIndex));
            int end = int(std::ceil(nextIndex));
            begin = qBound(0, begin, capture.size() - 1);
            end = qBound(begin, end, capture.size() - 1);
            int minimum = 0x3fff;
            int maximum = 0;
            for (int i = begin; i <= end; ++i) {
                const int code = capture.adcCode(i);
                minimum = qMin(minimum, code);
                maximum = qMax(maximum, code);
            }
            const double xPixel = plot.left() + x;
            const double yTop = adcToY(window, maximum, plot.top(),
                                       plot.height());
            const double yBottom = adcToY(window, minimum, plot.top(),
                                          plot.height());
            if (!started) {
                path.moveTo(xPixel, yTop);
                started = true;
            } else {
                path.lineTo(xPixel, yTop);
            }
            path.lineTo(xPixel, yBottom);
        }
        painter.setPen(QPen(EnvelopeColor, 1.2));
        painter.drawPath(path);
    }

    // Overrange samples stay visible at any zoom level.
    painter.setPen(QPen(OverrangeColor, 1.0));
    for (int i = firstVisible; i <= lastVisible; ++i) {
        if (!capture.overrange(i))
            continue;
        const double x = indexToX(window, i, plot.left(), plot.width());
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }
    painter.restore();
}

void WaveformView::drawMarkers(QPainter &painter, const QRect &plot,
                               const Window &window) {
    if (!m_capture || !m_capture->valid())
        return;
    const auto &capture = *m_capture;
    painter.save();
    painter.setClipRect(plot);

    auto drawLevel = [&](double adc, const QColor &color,
                         const QString &label) {
        if (adc < window.adcBottom || adc > window.adcTop)
            return;
        const double y = adcToY(window, adc, plot.top(), plot.height());
        painter.setPen(QPen(color, 1.0, Qt::DashLine));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(QPen(color, 1.0));
        painter.drawText(QPointF(plot.left() + 4.0, y - 3.0), label);
    };

    // Latched baseline and the real threshold levels B and B +/- H.
    const double baseline = capture.baseline;
    drawLevel(baseline, BaselineColor,
              QStringLiteral("B=%1").arg(capture.baseline));
    const double threshold = capture.negativePolarity
        ? baseline - double(capture.threshold)
        : baseline + double(capture.threshold);
    drawLevel(threshold, ThresholdColor,
              QStringLiteral("B%1H=%2")
                  .arg(capture.negativePolarity ? QStringLiteral("-")
                                                : QStringLiteral("+"))
                  .arg(int(std::lround(threshold))));

    // Trigger instant T = 0.
    const double triggerX = indexToX(window, capture.triggerIndex(),
                                     plot.left(), plot.width());
    if (triggerX >= plot.left() - 1 && triggerX <= plot.right() + 1) {
        painter.setPen(QPen(TriggerColor, 1.4));
        painter.drawLine(QPointF(triggerX, plot.top()),
                         QPointF(triggerX, plot.bottom()));
        painter.setPen(QPen(TriggerColor, 1.0));
        painter.drawText(QPointF(triggerX + 3.0, plot.top() + 12.0),
                         QStringLiteral("T=0"));
    }
    painter.restore();
}

void WaveformView::drawBadges(QPainter &painter, const QRect &plot) {
    Q_UNUSED(plot);
    painter.setPen(QPen(TextColor, 1.0));
    const auto &steps = timebaseSteps();
    QString timebase;
    if (m_timebaseNsPerDiv >= 1000.0)
        timebase = QStringLiteral("%1 us/div").arg(m_timebaseNsPerDiv / 1000.0,
                                                   0, 'g', 4);
    else
        timebase = QStringLiteral("%1 ns/div").arg(m_timebaseNsPerDiv, 0, 'g', 4);
    const QString vertical = QStringLiteral("%1 ADC/div").arg(m_adcPerDiv);
    painter.drawText(QPointF(rect().left() + 8.0, rect().bottom() - 8.0),
                     QStringLiteral("%1   %2").arg(timebase, vertical));
    if (m_autoScale)
        painter.drawText(QPointF(rect().right() - 90.0, rect().bottom() - 8.0),
                         QStringLiteral("Y 自适应"));
    if (!m_capture || !m_capture->valid()) {
        painter.setPen(QPen(QColor("#7f8c98"), 1.0));
        painter.drawText(rect(), Qt::AlignCenter,
                         QStringLiteral("尚无完整波形"));
    }
}

void WaveformView::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int margin = 6;
    const int labelHeight = 22;
    const QRect plot = rect().adjusted(margin, margin, -margin, -labelHeight);
    if (plot.width() <= 2 || plot.height() <= 2)
        return;

    Window window = m_windowCacheValid ? m_windowCache : computeWindow();
    drawGrid(painter, plot, window);
    drawTrace(painter, plot, window);
    drawMarkers(painter, plot, window);
    drawBadges(painter, plot);
}

void WaveformView::emitCaptureInfo() {
    if (!m_capture || !m_capture->valid()) {
        emit captureInfoChanged(QString());
        return;
    }
    const auto &capture = *m_capture;
    emit captureInfoChanged(
        QStringLiteral("capture %1  会话 %2  配置版本 %3  %4")
            .arg(capture.captureId)
            .arg(capture.sessionId)
            .arg(capture.configVersion)
            .arg(capture.negativePolarity ? QStringLiteral("负极性")
                                          : QStringLiteral("正极性")));
}

void WaveformView::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        m_panning = true;
        m_panStartX = event->position().x();
        Window window = computeWindow();
        m_panStartPosition = window.firstIndex;
    }
    QWidget::mousePressEvent(event);
}

void WaveformView::mouseMoveEvent(QMouseEvent *event) {
    if (m_panning && m_capture && m_capture->valid()) {
        const int plotWidth = qMax(1, width() - 12);
        Window window = computeWindow();
        const double span = qMax(1.0, window.lastIndex - window.firstIndex);
        const double delta = (m_panStartX - event->position().x())
                           / double(plotWidth) * span;
        setHorizontalPosition(m_panStartPosition + delta);
        return;
    }
    if (m_capture && m_capture->valid()) {
        const int margin = 6;
        const int labelHeight = 22;
        const QRect plot = rect().adjusted(margin, margin, -margin,
                                           -labelHeight);
        if (plot.contains(event->position().toPoint())) {
            Window window = computeWindow();
            const double span = qMax(1.0, window.lastIndex - window.firstIndex);
            const double fraction =
                (event->position().x() - plot.left()) / double(plot.width());
            const int index = qBound(0, int(std::lround(window.firstIndex
                                                        + fraction * span)),
                                     m_capture->size() - 1);
            m_hasCursor = true;
            emit cursorMoved(index, m_capture->adcCode(index),
                             m_capture->timeNs(index),
                             m_capture->overrange(index));
        }
    }
    QWidget::mouseMoveEvent(event);
}

void WaveformView::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton)
        m_panning = false;
    QWidget::mouseReleaseEvent(event);
}

void WaveformView::leaveEvent(QEvent *event) {
    m_hasCursor = false;
    QWidget::leaveEvent(event);
}

} // namespace mca
