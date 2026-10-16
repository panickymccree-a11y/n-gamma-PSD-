#include "ui/spectrumview.h"
#include <QAreaSeries>
#include <QChart>
#include <QChartView>
#include <QLineSeries>
#include <QLogValueAxis>
#include <QMenu>
#include <QMouseEvent>
#include <QValueAxis>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace mca {

// ═══════════════════════════════════════════════════════════════════════════
// SpectrumOverviewView
// ═══════════════════════════════════════════════════════════════════════════

SpectrumOverviewView::SpectrumOverviewView(QChart *chart, QWidget *parent)
    : QChartView(chart, parent)
{
    setRenderHint(QPainter::Antialiasing, false);
    setDragMode(QGraphicsView::NoDrag);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_viewRect = new QGraphicsRectItem;
    m_viewRect->setPen(QPen(QColor("#0066cc"), 1));
    m_viewRect->setBrush(QColor(0, 102, 204, 40));
    m_viewRect->setZValue(100);
    chart->scene()->addItem(m_viewRect);
}

void SpectrumOverviewView::setViewRect(double xMin, double xMax) {
    const QRectF pa = chart()->plotArea();
    if (pa.width() <= 0) return;
    const double axisMin = 0;
    const double axisMax = chart()->axes(Qt::Horizontal).value(0)
        ? static_cast<QValueAxis*>(chart()->axes(Qt::Horizontal).value(0))->max() : 4095;
    const double range = axisMax - axisMin;
    if (range <= 0) return;
    const double rx = pa.left() + (xMin - axisMin) / range * pa.width();
    const double rw = (xMax - xMin) / range * pa.width();
    m_viewRect->setRect(rx, pa.top(), rw, pa.height());
    m_viewRect->show();
}

// Convert scene X coordinate to channel value
static double sceneXToChannelOv(const QChart *chart, double sceneX) {
    const QRectF pa = chart->plotArea();
    if (pa.width() <= 0) return 0;
    const auto *ax = static_cast<QValueAxis*>(chart->axes(Qt::Horizontal).value(0));
    if (!ax) return 0;
    const double frac = (sceneX - pa.left()) / pa.width();
    return ax->min() + frac * (ax->max() - ax->min());
}

void SpectrumOverviewView::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) { QChartView::mousePressEvent(event); return; }
    const QPointF sp = mapToScene(event->position().toPoint());
    const QRectF vr = m_viewRect->rect();
    const double edgeMargin = 6.0;
    const bool nearLeft  = qAbs(sp.x() - vr.left()) < edgeMargin;
    const bool nearRight = qAbs(sp.x() - (vr.left() + vr.width())) < edgeMargin;
    const bool inside = vr.contains(sp);

    // Convert scene rect to channel values for drag tracking
    const double chMin = sceneXToChannelOv(chart(), vr.left());
    const double chMax = sceneXToChannelOv(chart(), vr.left() + vr.width());
    const double axisMax = chart()->axes(Qt::Horizontal).value(0)
        ? static_cast<QValueAxis*>(chart()->axes(Qt::Horizontal).value(0))->max() : 4095;

    if (nearLeft || nearRight) {
        m_dragEdge = true;
        m_dragLeft = nearLeft;
        m_dragging = true;
        m_dragStart = sp;
        m_dragXMin = chMin;
        m_dragXMax = chMax;
    } else if (inside) {
        m_dragEdge = false;
        m_dragging = true;
        m_dragStart = sp;
        m_dragXMin = chMin;
        m_dragXMax = chMax;
    } else {
        // Click outside view rect → center on click
        const QRectF pa = chart()->plotArea();
        const double halfWidth = (chMax - chMin) / 2.0;
        double center = (sp.x() - pa.left()) / pa.width() * axisMax;
        double xMin = qMax(0.0, center - halfWidth);
        double xMax = qMin(axisMax, center + halfWidth);
        emit viewRectChanged(xMin, xMax);
    }
}

void SpectrumOverviewView::mouseMoveEvent(QMouseEvent *event) {
    if (!m_dragging) {
        const QPointF sp = mapToScene(event->position().toPoint());
        const QRectF vr = m_viewRect->rect();
        const double edgeMargin = 6.0;
        if (qAbs(sp.x() - vr.left()) < edgeMargin || qAbs(sp.x() - (vr.left() + vr.width())) < edgeMargin)
            setCursor(Qt::SizeHorCursor);
        else if (vr.contains(sp))
            setCursor(Qt::OpenHandCursor);
        else
            setCursor(Qt::ArrowCursor);
        QChartView::mouseMoveEvent(event);
        return;
    }
    setCursor(m_dragEdge ? Qt::SizeHorCursor : Qt::ClosedHandCursor);
    const QPointF sp = mapToScene(event->position().toPoint());
    const double dSceneX = sp.x() - m_dragStart.x();
    const QRectF pa = chart()->plotArea();
    const double axisMax = chart()->axes(Qt::Horizontal).value(0)
        ? static_cast<QValueAxis*>(chart()->axes(Qt::Horizontal).value(0))->max() : 4095;
    // Convert scene delta to channel delta
    const double dCh = pa.width() > 0 ? dSceneX * axisMax / pa.width() : 0;

    double xMin, xMax;
    if (m_dragEdge) {
        if (m_dragLeft) {
            xMin = qBound(0.0, m_dragXMin + dCh, m_dragXMax - 1.0);
            xMax = m_dragXMax;
        } else {
            xMin = m_dragXMin;
            xMax = qBound(m_dragXMin + 1.0, m_dragXMax + dCh, axisMax);
        }
    } else {
        const double w = m_dragXMax - m_dragXMin;
        xMin = qBound(0.0, m_dragXMin + dCh, axisMax - w);
        xMax = xMin + w;
    }
    emit viewRectChanged(xMin, xMax);
}

void SpectrumOverviewView::mouseReleaseEvent(QMouseEvent *event) {
    if (m_dragging && event->button() == Qt::LeftButton) {
        m_dragging = false;
        m_dragEdge = false;
        setCursor(Qt::ArrowCursor);
    }
    QChartView::mouseReleaseEvent(event);
}

void SpectrumOverviewView::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton)
        emit resetRequested();
    QChartView::mouseDoubleClickEvent(event);
}

// ═══════════════════════════════════════════════════════════════════════════
// Coordinate helpers — direct scene↔channel conversion.
// Avoids QChart::mapToValue/mapToPosition which can fail with step-histogram
// series or when points lie outside the series data range.
// ═══════════════════════════════════════════════════════════════════════════

static double channelToSceneX(const QChart *chart, double channel) {
    const QRectF pa = chart->plotArea();
    if (pa.width() <= 0) return 0;
    const auto *ax = static_cast<const QValueAxis *>(chart->axes(Qt::Horizontal).value(0));
    if (!ax) return 0;
    const double range = ax->max() - ax->min();
    if (range <= 0) return pa.left();
    return pa.left() + (channel - ax->min()) / range * pa.width();
}

// ═══════════════════════════════════════════════════════════════════════════
// SpectrumView constructor
// ═══════════════════════════════════════════════════════════════════════════

SpectrumView::SpectrumView(QWidget *parent) : QWidget(parent) {
    // ── Main chart ──
    m_chart = new QChart;
    m_chart->setTheme(QChart::ChartThemeLight);
    m_chart->setBackgroundBrush(QBrush(QColor("#ffffff")));
    m_chart->setPlotAreaBackgroundBrush(QBrush(QColor("#ffffff")));
    m_chart->setPlotAreaBackgroundVisible(true);
    m_chart->legend()->setVisible(true);
    m_chart->legend()->setAlignment(Qt::AlignBottom);
    m_chart->setMargins(QMargins(4, 8, 4, 2));
    m_chart->setAnimationOptions(QChart::NoAnimation);

    // X axis
    m_x = new QValueAxis;
    m_x->setTitleText(QStringLiteral("道址"));
    m_x->setRange(0, DefaultPeakBins - 1);
    m_x->setLabelFormat("%.0f");
    m_x->setGridLineVisible(true);
    m_x->setGridLineColor(QColor("#d4d8dd"));
    m_x->setMinorGridLineVisible(true);
    m_x->setMinorGridLineColor(QColor("#eaecf0"));
    m_x->setLabelsColor(QColor("#4a5568"));
    m_x->setTitleBrush(QBrush(QColor("#2d3748")));
    m_x->setLinePenColor(QColor("#c0c5cc"));
    m_chart->addAxis(m_x, Qt::AlignBottom);

    // Linear Y
    m_y = new QValueAxis;
    m_y->setTitleText(QStringLiteral("计数"));
    m_y->setRange(0, 100);
    m_y->setGridLineVisible(true);
    m_y->setGridLineColor(QColor("#d4d8dd"));
    m_y->setMinorGridLineVisible(true);
    m_y->setMinorGridLineColor(QColor("#eaecf0"));
    m_y->setLabelsColor(QColor("#4a5568"));
    m_y->setTitleBrush(QBrush(QColor("#2d3748")));
    m_y->setLinePenColor(QColor("#c0c5cc"));
    m_chart->addAxis(m_y, Qt::AlignLeft);

    // Log Y
    m_logY = new QLogValueAxis;
    m_logY->setBase(10);
    m_logY->setRange(1, 100);
    m_logY->setGridLineVisible(true);
    m_logY->setGridLineColor(QColor("#d4d8dd"));
    m_logY->setMinorGridLineVisible(true);
    m_logY->setMinorGridLineColor(QColor("#eaecf0"));
    m_logY->setLabelsColor(QColor("#4a5568"));
    m_logY->setTitleBrush(QBrush(QColor("#2d3748")));
    m_logY->setLinePenColor(QColor("#c0c5cc"));

    // Main chart view
    m_mainView = new QChartView(m_chart);
    m_mainView->setObjectName(QStringLiteral("spectrumMainChartView"));
    m_mainView->setRenderHint(QPainter::Antialiasing, false);
    m_mainView->setRubberBand(QChartView::NoRubberBand);
    m_mainView->setDragMode(QGraphicsView::NoDrag);
    m_mainView->setMouseTracking(true);
    m_mainView->viewport()->setMouseTracking(true);
    m_mainView->viewport()->installEventFilter(this);
    m_mainView->viewport()->setCursor(Qt::CrossCursor);

    // ── Overview chart ──
    m_overviewChart = new QChart;
    m_overviewChart->setTheme(QChart::ChartThemeLight);
    m_overviewChart->setBackgroundBrush(QBrush(QColor("#f5f6f8")));
    m_overviewChart->setPlotAreaBackgroundBrush(QBrush(QColor("#ffffff")));
    m_overviewChart->setPlotAreaBackgroundVisible(true);
    m_overviewChart->legend()->setVisible(false);
    m_overviewChart->setMargins(QMargins(4, 2, 4, 2));
    m_overviewChart->setAnimationOptions(QChart::NoAnimation);

    auto *ovX = new QValueAxis;
    ovX->setRange(0, DefaultPeakBins - 1);
    ovX->setLabelsVisible(false);
    ovX->setGridLineVisible(false);
    ovX->setLinePenColor(QColor("#c0c5cc"));
    m_overviewChart->addAxis(ovX, Qt::AlignBottom);

    auto *ovY = new QValueAxis;
    ovY->setRange(0, 100);
    ovY->setLabelsVisible(false);
    ovY->setGridLineVisible(false);
    ovY->setLineVisible(false);
    m_overviewChart->addAxis(ovY, Qt::AlignLeft);

    m_overviewView = new SpectrumOverviewView(m_overviewChart);
    m_overviewView->setMinimumHeight(80);
    m_overviewView->setMaximumHeight(140);

    connect(m_overviewView, &SpectrumOverviewView::viewRectChanged,
            this, [this](double xMin, double xMax) {
        if (m_updatingOverview) return;
        m_updatingMain = true;
        m_x->setRange(xMin, xMax);
        m_updatingMain = false;
        refresh();
        updateRoiGraphics();
    });
    connect(m_overviewView, &SpectrumOverviewView::resetRequested,
            this, &SpectrumView::resetZoom);

    // ── Splitter ──
    m_splitter = new QSplitter(Qt::Vertical);
    m_splitter->addWidget(m_mainView);
    m_splitter->addWidget(m_overviewView);
    m_splitter->setStretchFactor(0, 85);
    m_splitter->setStretchFactor(1, 15);
    m_splitter->setHandleWidth(3);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_splitter);

    // ── Cursor line ──
    m_cursorLine = new QGraphicsLineItem;
    m_cursorLine->setPen(QPen(QColor("#e53e3e"), 1, Qt::DashLine));
    m_cursorLine->setZValue(50);
    m_cursorLine->hide();
    m_chart->scene()->addItem(m_cursorLine);

    // ── Cursor info overlay ──
    m_cursorLabel = new QLabel(m_mainView);
    m_cursorLabel->setObjectName(QStringLiteral("spectrumCursorOverlay"));
    m_cursorLabel->move(8, 8);
    m_cursorLabel->hide();

    // ── Zoom rectangle (reusable) ──
    m_zoomRect = new QGraphicsRectItem;
    m_zoomRect->setPen(QPen(QColor("#0066cc"), 1, Qt::DashLine));
    m_zoomRect->setBrush(QColor(0, 102, 204, 30));
    m_zoomRect->setZValue(45);
    m_zoomRect->hide();
    m_chart->scene()->addItem(m_zoomRect);

    // ── Main axis range → update overview ──
    connect(m_x, &QValueAxis::rangeChanged, this, [this] {
        if (m_updatingMain) return;
        m_updatingOverview = true;
        m_overviewView->setViewRect(m_x->min(), m_x->max());
        m_updatingOverview = false;
        refresh();
        updateRoiGraphics();
    });
}

// ═══════════════════════════════════════════════════════════════════════════
// Event filter — installed on viewport
// ═══════════════════════════════════════════════════════════════════════════

bool SpectrumView::eventFilter(QObject *obj, QEvent *event) {
    if (obj == m_mainView->viewport()) {
        switch (event->type()) {
        case QEvent::MouseMove: {
            auto *me = static_cast<QMouseEvent *>(event);
            const QPointF scenePos = m_mainView->mapToScene(me->position().toPoint());
            handleMove(me);
            if (m_interactionMode == SpectrumInteractionMode::Cursor)
                updateCursor(scenePos);
            break;
        }
        case QEvent::MouseButtonPress:
            handlePress(static_cast<QMouseEvent *>(event));
            break;
        case QEvent::MouseButtonRelease:
            handleRelease(static_cast<QMouseEvent *>(event));
            break;
        case QEvent::MouseButtonDblClick:
            handleDoubleClick(static_cast<QMouseEvent *>(event));
            break;
        case QEvent::Leave:
            onCursorLeft();
            break;
        case QEvent::Wheel: {
            auto *we = static_cast<QWheelEvent *>(event);
            const double factor = we->angleDelta().y() > 0 ? 0.8 : 1.25;
            const QPointF scenePos =
                m_mainView->mapToScene(we->position().toPoint());
            const QRectF plotArea = m_chart->plotArea();
            const bool overYAxis =
                scenePos.x() < plotArea.left() &&
                scenePos.y() >= plotArea.top() &&
                scenePos.y() <= plotArea.bottom();
            if ((we->modifiers() & Qt::ControlModifier) || overYAxis) {
                zoomYBy(factor);
                we->accept();
                return true;
            }
            const double cx = m_x->min() + (m_x->max() - m_x->min()) / 2.0;
            const double half = (m_x->max() - m_x->min()) * factor / 2.0;
            const int maxCh = maximumChannel();
            double newMin = qMax(0.0, cx - half);
            double newMax = qMin(double(maxCh), cx + half);
            if (newMax - newMin < 2) { newMax = qMin(double(maxCh), newMin + 2); }
            m_updatingMain = true;
            m_x->setRange(newMin, newMax);
            m_updatingMain = false;
            refresh();
            updateRoiGraphics();
            we->accept();
            return true;
        }
        default: break;
        }
    }
    return QWidget::eventFilter(obj, event);
}

// ═══════════════════════════════════════════════════════════════════════════
// Interaction handlers
// ═══════════════════════════════════════════════════════════════════════════

void SpectrumView::handlePress(QMouseEvent *event) {
    if (event->button() == Qt::RightButton) {
        const QPointF sp = m_mainView->mapToScene(event->position().toPoint());
        showContextMenu(event->globalPosition().toPoint(), sp);
        return;
    }
    if (event->button() != Qt::LeftButton) return;

    const QPointF sp = m_mainView->mapToScene(event->position().toPoint());
    const double ch = sceneXToChannel(sp.x());
    m_pressScenePos = sp;
    m_dragStartScene = sp;

    switch (m_interactionMode) {
    case SpectrumInteractionMode::Cursor:
        break;
    case SpectrumInteractionMode::ZoomRectangle:
        m_dragging = true;
        m_zoomRect->setRect(sp.x(), m_chart->plotArea().top(), 0, m_chart->plotArea().height());
        m_zoomRect->show();
        break;
    case SpectrumInteractionMode::Pan:
        m_dragging = true;
        m_mainView->viewport()->setCursor(Qt::ClosedHandCursor);
        break;
    case SpectrumInteractionMode::CreateRoi:
        m_dragging = true;
        m_zoomRect->setRect(sp.x(), m_chart->plotArea().top(), 0, m_chart->plotArea().height());
        m_zoomRect->show();
        break;
    case SpectrumInteractionMode::EditRoi: {
        const int chInt = qRound(ch);
        const int roiIdx = findRoiAtChannel(chInt);
        if (roiIdx >= 0) {
            const auto &roi = m_rois[roiIdx];
            const double roiLeftX = channelToSceneX(m_chart, double(roi.left));
            const double roiRightX = channelToSceneX(m_chart, double(roi.right));
            const double margin = 5.0;
            if (qAbs(sp.x() - roiLeftX) < margin) {
                m_dragTarget = DragTarget::RoiLeft;
                m_dragRoiIndex = roiIdx;
                m_dragging = true;
            } else if (qAbs(sp.x() - roiRightX) < margin) {
                m_dragTarget = DragTarget::RoiRight;
                m_dragRoiIndex = roiIdx;
                m_dragging = true;
            } else if (sp.x() > roiLeftX && sp.x() < roiRightX) {
                m_dragTarget = DragTarget::RoiMove;
                m_dragRoiIndex = roiIdx;
                m_dragging = true;
            }
        }
        if (m_dragging) {
            m_mainView->viewport()->setCursor(
                m_dragTarget == DragTarget::RoiMove ? Qt::SizeAllCursor : Qt::SizeHorCursor);
        }
        break;
    }
    }
}

void SpectrumView::handleMove(QMouseEvent *event) {
    const QPointF sp = m_mainView->mapToScene(event->position().toPoint());
    if (!m_dragging) {
        // Update cursor for EditRoi mode hover
        if (m_interactionMode == SpectrumInteractionMode::EditRoi && !m_series.isEmpty()) {
            const int chInt = qRound(sceneXToChannel(sp.x()));
            const int roiIdx = findRoiAtChannel(chInt);
            if (roiIdx >= 0) {
                const auto &roi = m_rois[roiIdx];
                const double rx0 = channelToSceneX(m_chart, double(roi.left));
                const double rx1 = channelToSceneX(m_chart, double(roi.right));
                const double margin = 5.0;
                if (qAbs(sp.x() - rx0) < margin || qAbs(sp.x() - rx1) < margin)
                    m_mainView->viewport()->setCursor(Qt::SizeHorCursor);
                else if (sp.x() > rx0 && sp.x() < rx1)
                    m_mainView->viewport()->setCursor(Qt::SizeAllCursor);
                else
                    m_mainView->viewport()->setCursor(Qt::CrossCursor);
            } else {
                m_mainView->viewport()->setCursor(Qt::CrossCursor);
            }
        }
        return;
    }

    const double chCurrent = sceneXToChannel(sp.x());
    const double chStart = sceneXToChannel(m_dragStartScene.x());
    const double dx = chCurrent - chStart;
    const int maxCh = maximumChannel();

    switch (m_interactionMode) {
    case SpectrumInteractionMode::ZoomRectangle: {
        const QRectF pa = m_chart->plotArea();
        const double x0 = qMin(m_dragStartScene.x(), sp.x());
        const double w = qAbs(sp.x() - m_dragStartScene.x());
        m_zoomRect->setRect(x0, pa.top(), w, pa.height());
        break;
    }
    case SpectrumInteractionMode::Pan: {
        const double range = m_x->max() - m_x->min();
        double newMin = m_x->min() - dx;
        double newMax = m_x->max() - dx;
        if (newMin < 0) { newMax -= newMin; newMin = 0; }
        if (newMax > maxCh) { newMin -= (newMax - maxCh); newMax = maxCh; }
        if (newMin < 0) newMin = 0;
        if (newMax - newMin < 2) { newMax = qMin(double(maxCh), newMin + 2); }
        m_updatingMain = true;
        m_x->setRange(newMin, newMax);
        m_updatingMain = false;
        refresh();
        updateRoiGraphics();
        m_dragStartScene = sp;
        break;
    }
    case SpectrumInteractionMode::CreateRoi: {
        const QRectF pa = m_chart->plotArea();
        const double x0 = qMin(m_dragStartScene.x(), sp.x());
        const double w = qAbs(sp.x() - m_dragStartScene.x());
        m_zoomRect->setRect(x0, pa.top(), w, pa.height());
        break;
    }
    case SpectrumInteractionMode::EditRoi: {
        if (m_dragRoiIndex < 0 || m_dragRoiIndex >= m_rois.size()) break;
        auto &roi = m_rois[m_dragRoiIndex];
        const int dCh = qRound(dx);
        switch (m_dragTarget) {
        case DragTarget::RoiLeft:
            roi.left = qBound(0, roi.left + dCh, roi.right - 1);
            break;
        case DragTarget::RoiRight:
            roi.right = qBound(roi.left + 1, roi.right + dCh, maxCh);
            break;
        case DragTarget::RoiMove: {
            const int width = roi.right - roi.left;
            roi.left = qBound(0, roi.left + dCh, maxCh - width);
            roi.right = roi.left + width;
            break;
        }
        default: break;
        }
        m_dragStartScene = sp;
        updateRoiGraphics();
        emit roiModified(m_dragRoiIndex, roi.left, roi.right);
        break;
    }
    default: break;
    }
}

void SpectrumView::handleRelease(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;

    const QPointF sp = m_mainView->mapToScene(event->position().toPoint());

    if (m_dragging) {
        // Use direct scene→channel conversion (more reliable than mapToValue)
        const double chEnd = sceneXToChannel(sp.x());
        const double chStart = sceneXToChannel(m_dragStartScene.x());

        switch (m_interactionMode) {
        case SpectrumInteractionMode::ZoomRectangle: {
            const double w = qAbs(chEnd - chStart);
            if (w > 2) {
                const double xMin = qMin(chStart, chEnd);
                const double xMax = qMax(chStart, chEnd);
                zoomTo(xMin, xMax);
            }
            break;
        }
        case SpectrumInteractionMode::CreateRoi: {
            const int left = qRound(qMin(chStart, chEnd));
            const int right = qRound(qMax(chStart, chEnd));
            const double w = qAbs(chEnd - chStart);
            if (w > 2 && right > left) {
                createRoiFromDrag(left, right);
            }
            break;
        }
        case SpectrumInteractionMode::EditRoi:
            if (m_dragRoiIndex >= 0 && m_dragRoiIndex < m_rois.size()) {
                const auto &roi = m_rois[m_dragRoiIndex];
                emit roiModified(m_dragRoiIndex, roi.left, roi.right);
            }
            break;
        default: break;
        }
    }

    m_zoomRect->hide();
    m_dragging = false;
    m_dragTarget = DragTarget::None;
    m_dragRoiIndex = -1;
    m_mainView->viewport()->setCursor(
        m_interactionMode == SpectrumInteractionMode::Cursor ? Qt::CrossCursor :
        m_interactionMode == SpectrumInteractionMode::Pan ? Qt::OpenHandCursor : Qt::CrossCursor);
}

void SpectrumView::handleDoubleClick(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        const QPointF scenePos =
            m_mainView->mapToScene(event->position().toPoint());
        const QRectF plotArea = m_chart->plotArea();
        if (scenePos.x() < plotArea.left() &&
            scenePos.y() >= plotArea.top() &&
            scenePos.y() <= plotArea.bottom()) {
            resetYAutoScale();
        } else {
            resetZoom();
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Context menu
// ═══════════════════════════════════════════════════════════════════════════

void SpectrumView::showContextMenu(const QPoint &globalPos, const QPointF &scenePos) {
    const int ch = qRound(sceneXToChannel(scenePos.x()));
    const int roiIdx = findRoiAtChannel(ch);

    QMenu menu;
    if (roiIdx >= 0) {
        QAction *fitAction = menu.addAction(QStringLiteral("拟合当前 ROI"));
        connect(fitAction, &QAction::triggered, this, [this, roiIdx] { emit roiFitRequested(roiIdx); });
        menu.addSeparator();
        QAction *deleteAction = menu.addAction(QStringLiteral("删除 ROI"));
        connect(deleteAction, &QAction::triggered, this, [this, roiIdx] { emit roiDeleteRequested(roiIdx); });
        QAction *zoomAction = menu.addAction(QStringLiteral("放大至 ROI"));
        connect(zoomAction, &QAction::triggered, this, [this, roiIdx] {
            if (roiIdx >= 0 && roiIdx < m_rois.size()) {
                zoomTo(m_rois[roiIdx].left - 10, m_rois[roiIdx].right + 10);
            }
        });
    } else {
        QAction *addRoiAction = menu.addAction(QStringLiteral("在此处添加 ROI"));
        connect(addRoiAction, &QAction::triggered, this, [this, ch] {
            createRoiFromDrag(ch - 10, ch + 10);
        });
        menu.addSeparator();
        QAction *fullAction = menu.addAction(QStringLiteral("恢复全谱"));
        connect(fullAction, &QAction::triggered, this, &SpectrumView::resetZoom);
        QAction *logAction = menu.addAction(m_log ? QStringLiteral("切换线性 Y") : QStringLiteral("切换对数 Y"));
        connect(logAction, &QAction::triggered, this, [this] { setLogarithmic(!m_log); });
    }
    menu.addSeparator();
    QAction *autoYAction = menu.addAction(QStringLiteral("自动 Y 轴"));
    autoYAction->setEnabled(!m_yAutoScaleEnabled);
    connect(autoYAction, &QAction::triggered,
            this, &SpectrumView::resetYAutoScale);
    menu.exec(globalPos);
}

// ═══════════════════════════════════════════════════════════════════════════
// ROI helpers
// ═══════════════════════════════════════════════════════════════════════════

int SpectrumView::findRoiAtChannel(int channel) const {
    for (int i = 0; i < m_rois.size(); ++i) {
        if (channel >= m_rois[i].left && channel <= m_rois[i].right)
            return i;
    }
    return -1;
}

void SpectrumView::createRoiFromDrag(int left, int right) {
    if (left > right) std::swap(left, right);
    const int maxCh = maximumChannel();
    left = qBound(0, left, maxCh);
    right = qBound(left + 1, right, maxCh);

    Roi roi;
    roi.name = QStringLiteral("ROI %1").arg(m_rois.size() + 1);
    roi.left = left;
    roi.right = right;
    static const QColor roiColors[] = {
        "#ffb000", "#e53e3e", "#38a169", "#3182ce",
        "#805ad5", "#dd6b20", "#319795", "#d53f8c"
    };
    roi.color = roiColors[m_rois.size() % 8];
    m_rois.push_back(roi);
    m_selectedRoi = m_rois.size() - 1;
    updateRoiGraphics();
    emit roiCreated(left, right);
}

// ═══════════════════════════════════════════════════════════════════════════
// Scene → channel helper (member — needs m_chart & m_x)
// ═══════════════════════════════════════════════════════════════════════════

double SpectrumView::sceneXToChannel(double sceneX) const {
    const QRectF pa = m_chart->plotArea();
    if (pa.width() <= 0) return 0;
    const double frac = (sceneX - pa.left()) / pa.width();
    return m_x->min() + frac * (m_x->max() - m_x->min());
}

int SpectrumView::maximumChannel() const {
    if (m_snapshot) {
        const auto &spectra = m_kind == SpectrumKind::Peak
            ? m_snapshot->peak : m_snapshot->integral;
        for (int channel : m_channels) {
            if (channel >= 0 && channel < spectra.size() &&
                !spectra[channel].isEmpty())
                return spectra[channel].size() - 1;
        }
        if (!spectra.isEmpty() && !spectra.front().isEmpty())
            return spectra.front().size() - 1;
    }
    return (m_kind == SpectrumKind::Peak
        ? DefaultPeakBins : DefaultIntegralBins) - 1;
}

// ═══════════════════════════════════════════════════════════════════════════
// Cursor
// ═══════════════════════════════════════════════════════════════════════════

void SpectrumView::updateCursor(const QPointF &scenePos) {
    if (!m_cursorEnabled || !m_snapshot || m_series.isEmpty()) return;

    const int ch = qRound(sceneXToChannel(scenePos.x()));
    const int maxCh = maximumChannel();
    if (ch < 0 || ch > maxCh) { onCursorLeft(); return; }
    if (ch == m_lastCursorCh) return;
    m_lastCursorCh = ch;

    // Look up count
    quint64 count = 0;
    const int dataCh = m_channels.value(0, 0);
    const auto &src = (m_kind == SpectrumKind::Peak)
        ? m_snapshot->peak : m_snapshot->integral;
    if (dataCh >= 0 && dataCh < src.size() && ch < src[dataCh].size())
        count = src[dataCh][ch];

    // Position cursor line clipped to plot area
    const QRectF pa = m_chart->plotArea();
    const double sceneX = channelToSceneX(m_chart, double(ch));
    m_cursorLine->setLine(sceneX, pa.top(), sceneX, pa.bottom());
    m_cursorLine->show();

    // Build label text
    QString text = QStringLiteral("Ch: %1\nCnt: %2").arg(ch).arg(count);

    if (m_calibration.isValid()) {
        const double energy = m_calibration.apply(double(ch));
        text += QStringLiteral("\nE: %1 %2").arg(energy, 0, 'f', 2).arg(m_calibration.unit);
    }

    const int roiIdx = findRoiAtChannel(ch);
    if (roiIdx >= 0) {
        text += QStringLiteral("\nROI: %1").arg(m_rois[roiIdx].name);
    }

    m_cursorLabel->setText(text);
    m_cursorLabel->adjustSize();

    // Position label near cursor but inside view
    int lx = qRound(scenePos.x()) + 16;
    int ly = qRound(scenePos.y()) + 16;
    if (lx + m_cursorLabel->width() > m_mainView->width())
        lx = qRound(scenePos.x()) - m_cursorLabel->width() - 8;
    if (ly + m_cursorLabel->height() > m_mainView->height())
        ly = qRound(scenePos.y()) - m_cursorLabel->height() - 8;
    m_cursorLabel->move(qMax(4, lx), qMax(4, ly));
    m_cursorLabel->show();

    const double energy = m_calibration.isValid() ? m_calibration.apply(double(ch)) : 0.0;
    emit cursorMoved(double(ch), count, energy);
}

void SpectrumView::onCursorLeft() {
    m_cursorLine->hide();
    m_cursorLabel->hide();
    m_lastCursorCh = -1;
    emit cursorLeft();
}

// ═══════════════════════════════════════════════════════════════════════════
// Public slots
// ═══════════════════════════════════════════════════════════════════════════

void SpectrumView::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    refresh();refreshOverview();
}
void SpectrumView::setSnapshot(std::shared_ptr<const SpectrumSnapshot> s) {

    const int previousMaximum = maximumChannel();
    m_snapshot = std::move(s);
    if (m_snapshot) {
        const int shift = m_kind == SpectrumKind::Peak
            ? m_snapshot->peakBinShift : m_snapshot->integralBinShift;
        m_x->setTitleText(shift
            ? QStringLiteral("道址（每 bin = 2 LSB）")
            : QStringLiteral("道址"));
    }
    const int nextMaximum = maximumChannel();
    if (qFuzzyIsNull(m_x->min()) &&
        qAbs(m_x->max() - previousMaximum) < 0.5)
        m_x->setRange(0, nextMaximum);
    else if (m_x->max() > nextMaximum)
        m_x->setRange(qMin(m_x->min(), double(nextMaximum - 1)), nextMaximum);
    if (isVisible()) { refresh(); refreshOverview(); }
}

void SpectrumView::setKind(SpectrumKind k) {
    m_kind = k;
    if (m_snapshot) {
        const int shift = m_kind == SpectrumKind::Peak
            ? m_snapshot->peakBinShift : m_snapshot->integralBinShift;
        m_x->setTitleText(shift
            ? QStringLiteral("道址（每 bin = 2 LSB）")
            : QStringLiteral("道址"));
    }
    const int maxCh = maximumChannel();
    if (m_x->max() > maxCh || m_x->min() < 0)
        resetZoom();
    else
        m_x->setRange(m_x->min(), qMin(m_x->max(), double(maxCh)));
    refresh();
    refreshOverview();
}

void SpectrumView::setChannels(const QVector<int> &c) {
    m_channels = c;
    refresh();
    refreshOverview();
}

void SpectrumView::setLogarithmic(bool on) {
    if (m_log == on) return;
    if (on) {
        for (auto *series : m_series) {
            auto points = series->points();
            for (auto &point : points)
                point.setY(qMax(1.0, point.y()));
            series->replace(points);
        }
        for (auto *area : m_areaSeries) {
            if (auto *lower = area->lowerSeries()) {
                const auto points = lower->points();
                if (!points.isEmpty()) {
                    lower->replace({
                        {points.first().x(), 1.0},
                        {points.last().x(), 1.0}
                    });
                }
            }
        }
    }
    m_chart->removeAxis(m_log ? static_cast<QAbstractAxis *>(m_logY)
                              : static_cast<QAbstractAxis *>(m_y));
    m_log = on;
    m_chart->addAxis(m_log ? static_cast<QAbstractAxis *>(m_logY)
                           : static_cast<QAbstractAxis *>(m_y),
                     Qt::AlignLeft);
    auto *yAxis = m_log ? static_cast<QAbstractAxis *>(m_logY)
                        : static_cast<QAbstractAxis *>(m_y);
    if (m_areaSeries.isEmpty()) {
        for (auto *series : m_series)
            series->attachAxis(yAxis);
    } else {
        for (auto *area : m_areaSeries)
            area->attachAxis(yAxis);
    }
    if (m_fit) m_fit->attachAxis(yAxis);
    m_yAutoScaleEnabled = true;
    resetStableY();
    refresh();
}

void SpectrumView::setLegendVisible(bool visible) {
    m_legendVisible = visible;
    m_chart->legend()->setVisible(visible);
}

void SpectrumView::setGridVisible(bool visible) {
    m_gridVisible = visible;
    m_x->setGridLineVisible(visible);
    m_x->setMinorGridLineVisible(visible);
    m_y->setGridLineVisible(visible);
    m_y->setMinorGridLineVisible(visible);
    m_logY->setGridLineVisible(visible);
    m_logY->setMinorGridLineVisible(visible);
}

void SpectrumView::setRoiLabelsVisible(bool visible) {
    m_roiLabelsVisible = visible;
    for (auto *label : m_roiLabels)
        label->setVisible(visible);
}

void SpectrumView::resetZoom() {
    const int maxCh = maximumChannel();
    m_x->setRange(0, maxCh);
    m_yAutoScaleEnabled = true;
    resetStableY();
    refresh();
}

void SpectrumView::zoomTo(double left, double right) {
    if (left > right) std::swap(left, right);
    const int maxCh = maximumChannel();
    left = qBound(0.0, left, double(maxCh - 1));
    right = qBound(left + 1.0, right, double(maxCh));
    m_x->setRange(left, right);
}

void SpectrumView::zoomBy(double factor) {
    if (factor <= 0)
        return;
    const int maxCh = maximumChannel();
    const double center = (m_x->min() + m_x->max()) * 0.5;
    const double width = qBound(2.0, (m_x->max() - m_x->min()) * factor,
                                double(maxCh));
    zoomTo(center - width * 0.5, center + width * 0.5);
}

void SpectrumView::zoomYBy(double factor) {
    if (factor <= 0)
        return;
    m_yAutoScaleEnabled = false;
    if (m_log) {
        const double minimum = qMax(1.0, m_logY->min());
        const double maximum = qMax(minimum * 1.01, m_logY->max());
        const double nextMaximum = qBound(
            minimum * 1.01, maximum * factor, 1.0e18);
        m_logY->setRange(minimum, nextMaximum);
    } else {
        const double maximum = qMax(1.0, m_y->max());
        m_y->setRange(0, qBound(1.0, maximum * factor, 1.0e18));
    }
    updateRoiGraphics();
}

void SpectrumView::resetYAutoScale() {
    m_yAutoScaleEnabled = true;
    resetStableY();
    refresh();
}

QPair<int, int> SpectrumView::visibleRange() const {
    return {qRound(m_x->min()), qRound(m_x->max())};
}

QPair<double, double> SpectrumView::visibleYRange() const {
    return m_log
        ? QPair<double, double>{m_logY->min(), m_logY->max()}
        : QPair<double, double>{m_y->min(), m_y->max()};
}

void SpectrumView::setInteractionMode(SpectrumInteractionMode mode) {
    if (m_interactionMode == mode) return;
    m_interactionMode = mode;
    switch (mode) {
    case SpectrumInteractionMode::Cursor:
        m_mainView->viewport()->setCursor(Qt::CrossCursor);
        break;
    case SpectrumInteractionMode::ZoomRectangle:
        m_mainView->viewport()->setCursor(Qt::CrossCursor);
        break;
    case SpectrumInteractionMode::Pan:
        m_mainView->viewport()->setCursor(Qt::OpenHandCursor);
        break;
    case SpectrumInteractionMode::CreateRoi:
        m_mainView->viewport()->setCursor(Qt::CrossCursor);
        break;
    case SpectrumInteractionMode::EditRoi:
        m_mainView->viewport()->setCursor(Qt::CrossCursor);
        break;
    }
    emit interactionModeChanged(mode);
}

void SpectrumView::setStableYEnabled(bool on) {
    m_stableYEnabled = on;
    if (!on) resetStableY();
}

void SpectrumView::resetStableY() {
    m_stableYMax = 0;
}

void SpectrumView::setRoi(int l, int r) {
    m_roiLeft = l;
    m_roiRight = r;
}

void SpectrumView::setRois(const QVector<Roi> &rois) {
    m_rois = rois;
    updateRoiGraphics();
}

void SpectrumView::setSelectedRoi(int index) {
    m_selectedRoi = index;
    updateRoiGraphics();
}

void SpectrumView::setFitCurve(const QVector<QPointF> &curve) {
    if (!m_fit) {
        m_fit = new QLineSeries;
        m_fit->setName(QStringLiteral("拟合"));
        m_fit->setColor(QColor("#d62728"));
        m_fit->setPen(QPen(m_fit->color(), 2));
        m_chart->addSeries(m_fit);
        m_fit->attachAxis(m_x);
        m_fit->attachAxis(m_log ? static_cast<QAbstractAxis *>(m_logY)
                                : static_cast<QAbstractAxis *>(m_y));
    }
    m_fit->replace(curve);
}

void SpectrumView::clearFitCurve() {
    if (!m_fit) return;
    m_chart->removeSeries(m_fit);
    delete m_fit;
    m_fit = nullptr;
}

void SpectrumView::setFillMode(bool on) {
    if (m_fillMode == on) return;
    m_fillMode = on;
    rebuildSeries();
    refresh();
}

void SpectrumView::setCalibration(const Calibration &cal) {
    m_calibration = cal;
    if (cal.isValid())
        m_x->setTitleText(QStringLiteral("道址 / %1").arg(cal.unit));
    else
        m_x->setTitleText(QStringLiteral("道址"));
}

void SpectrumView::setOverviewVisible(bool visible) {
    m_overviewVisible = visible;
    m_overviewView->setVisible(visible);
}

// ═══════════════════════════════════════════════════════════════════════════
// ROI graphics
// ═══════════════════════════════════════════════════════════════════════════

void SpectrumView::updateRoiGraphics() {
    // Clean up old items
    for (auto *item : m_roiLeftLines) { m_chart->scene()->removeItem(item); delete item; }
    for (auto *item : m_roiRightLines) { m_chart->scene()->removeItem(item); delete item; }
    for (auto *item : m_roiShades) { m_chart->scene()->removeItem(item); delete item; }
    for (auto *item : m_roiLabels) { m_chart->scene()->removeItem(item); delete item; }
    m_roiLeftLines.clear(); m_roiRightLines.clear();
    m_roiShades.clear(); m_roiLabels.clear();

    if (m_rois.isEmpty() || m_series.isEmpty()) {
        // Also handle backward-compat single ROI
        if (m_roiLeft >= 0 && m_roiRight > m_roiLeft && !m_series.isEmpty()) {
            const QRectF pa = m_chart->plotArea();
            const double x0 = channelToSceneX(m_chart, double(m_roiLeft));
            const double x1 = channelToSceneX(m_chart, double(m_roiRight));
            const QColor c("#ffb000");
            QColor fill = c; fill.setAlpha(30);

            auto *shade = new QGraphicsRectItem(x0, pa.top(), x1 - x0, pa.height());
            shade->setBrush(QBrush(fill));
            shade->setPen(QPen(Qt::NoPen));
            shade->setZValue(10);
            m_chart->scene()->addItem(shade);
            m_roiShades.push_back(shade);

            auto *ll = new QGraphicsLineItem(x0, pa.top(), x0, pa.bottom());
            ll->setPen(QPen(c, 2, Qt::DashLine));
            ll->setZValue(40);
            m_chart->scene()->addItem(ll);
            m_roiLeftLines.push_back(ll);

            auto *rl = new QGraphicsLineItem(x1, pa.top(), x1, pa.bottom());
            rl->setPen(QPen(c, 2, Qt::DashLine));
            rl->setZValue(40);
            m_chart->scene()->addItem(rl);
            m_roiRightLines.push_back(rl);
        }
        return;
    }

    const QRectF pa = m_chart->plotArea();

    for (int i = 0; i < m_rois.size(); ++i) {
        const auto &roi = m_rois[i];
        const bool selected = (i == m_selectedRoi);
        const double x0 = channelToSceneX(m_chart, double(roi.left));
        const double x1 = channelToSceneX(m_chart, double(roi.right));
        const double w = x1 - x0;
        if (w <= 0) continue;

        const QColor c = roi.color.isValid() ? roi.color : QColor("#ffb000");
        QColor fillColor = c;
        fillColor.setAlpha(selected ? 50 : 30);

        // Shade (z=10 — below spectrum lines)
        auto *shade = new QGraphicsRectItem(x0, pa.top(), w, pa.height());
        shade->setBrush(QBrush(fillColor));
        shade->setPen(QPen(Qt::NoPen));
        shade->setZValue(10);
        m_chart->scene()->addItem(shade);
        m_roiShades.push_back(shade);

        // Left boundary (z=40 — above fit curve, below cursor)
        auto *ll = new QGraphicsLineItem(x0, pa.top(), x0, pa.bottom());
        ll->setPen(QPen(c, selected ? 3 : 2, Qt::DashLine));
        ll->setZValue(40);
        m_chart->scene()->addItem(ll);
        m_roiLeftLines.push_back(ll);

        // Right boundary
        auto *rl = new QGraphicsLineItem(x1, pa.top(), x1, pa.bottom());
        rl->setPen(QPen(c, selected ? 3 : 2, Qt::DashLine));
        rl->setZValue(40);
        m_chart->scene()->addItem(rl);
        m_roiRightLines.push_back(rl);

        // Label at top of ROI area
        auto *label = new QGraphicsSimpleTextItem(roi.name);
        QFont f; f.setPointSize(8);
        if (selected) f.setBold(true);
        label->setFont(f);
        label->setBrush(QBrush(c.darker(150)));
        label->setPos(x0 + 2, pa.top() + 2);
        label->setZValue(41);
        label->setVisible(m_roiLabelsVisible);
        m_chart->scene()->addItem(label);
        m_roiLabels.push_back(label);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Rebuild series (fill ↔ line switch)
// ═══════════════════════════════════════════════════════════════════════════

void SpectrumView::rebuildSeries() {
    if (!m_areaSeries.isEmpty()) {
        for (auto *a : m_areaSeries) { m_chart->removeSeries(a); delete a; }
        m_areaSeries.clear();
        m_series.clear();
    } else {
        for (auto *s : m_series) { m_chart->removeSeries(s); delete s; }
        m_series.clear();
    }
    if (m_fit) { m_chart->removeSeries(m_fit); delete m_fit; m_fit = nullptr; }
}

// ═══════════════════════════════════════════════════════════════════════════
// Main refresh with adaptive downsampling + step histogram
// ═══════════════════════════════════════════════════════════════════════════

void SpectrumView::refresh() {
    if (!m_snapshot) return;

    auto *yAxis = m_log ? static_cast<QAbstractAxis *>(m_logY)
                        : static_cast<QAbstractAxis *>(m_y);

    // Rebuild if channel count or mode changed
    if (int(m_series.size()) != m_channels.size()
        || (m_fillMode && int(m_areaSeries.size()) != m_channels.size())
        || (!m_fillMode && !m_areaSeries.isEmpty())) {
        rebuildSeries();
    }

    // Create series if empty
    if (m_series.isEmpty()) {
        for (int i = 0; i < m_channels.size(); ++i) {
            auto *line = new QLineSeries;
            line->setName(panelChannelName(m_channels[i]));
            line->setColor(channelColor(m_channels[i]));

            if (m_fillMode) {
                auto *lower = new QLineSeries;
                auto *area = new QAreaSeries(line, lower);
                area->setName(line->name());
                area->setColor(channelColor(m_channels[i]));
                area->setColor(channelColor(m_channels[i])); // will be alpha'd
                area->setBorderColor(channelColor(m_channels[i]));
                area->setPen(QPen(channelColor(m_channels[i]), 1));
                m_chart->addSeries(area);
                area->attachAxis(m_x);
                area->attachAxis(yAxis);
                m_areaSeries.push_back(area);
            } else {
                m_chart->addSeries(line);
                line->attachAxis(m_x);
                line->attachAxis(yAxis);
            }
            m_series.push_back(line);
        }
    }

    // ── Adaptive downsampling ──
    const int lo = qMax(0, int(m_x->min()));
    const int hi = int(m_x->max());
    const int totalChannels = maximumChannel() + 1;

    // Estimate plot area pixel width
    const int pixelWidth = qMax(1, int(m_chart->plotArea().width()));
    const int visibleChannels = hi - lo + 1;
    const double channelsPerPixel = double(visibleChannels) / pixelWidth;

    double maxY = 1;
    const double baseline = m_log ? 1.0 : 0.0;

    for (int i = 0; i < m_channels.size(); ++i) {
        const int ch = m_channels[i];
        if (ch < 0 || ch >= m_snapshot->channelCount) continue;

        const auto &src = (m_kind == SpectrumKind::Peak)
            ? m_snapshot->peak[ch] : m_snapshot->integral[ch];
        const int dataLen = src.size();

        QVector<QPointF> pts;
        const int right = qMin(hi, dataLen - 1);

        if (channelsPerPixel <= 1.0) {
            // Full resolution with step histogram
            const int nPoints = right - lo + 1;
            pts.reserve(nPoints * 2);
            for (int x = lo; x <= right; ++x) {
                const double y = double(qMax<quint64>(quint64(baseline), m_log ? qMax<quint64>(1, src[x]) : src[x]));
                pts.push_back({double(x), y});
                pts.push_back({double(x) + 1.0, y});
                maxY = qMax(maxY, y);
            }
        } else {
            // Downsampled — bin by pixel, take max
            const int binSize = qMax(1, int(channelsPerPixel));
            pts.reserve(((right - lo) / binSize + 1) * 2);
            for (int x = lo; x <= right; x += binSize) {
                quint64 binMax = 0;
                const int binEnd = qMin(x + binSize, right + 1);
                for (int k = x; k < binEnd; ++k)
                    binMax = qMax(binMax, src[k]);
                const double y = double(qMax<quint64>(quint64(baseline), m_log ? qMax<quint64>(1, binMax) : binMax));
                pts.push_back({double(x), y});
                pts.push_back({double(qMin(binEnd, right)), y});
                maxY = qMax(maxY, y);
            }
        }

        const QColor color = channelColor(ch);
        m_series[i]->setName(panelChannelName(ch));
        m_series[i]->setColor(color);
        m_series[i]->replace(pts);

        // Update area series
        if (i < m_areaSeries.size()) {
            auto *area = m_areaSeries[i];
            area->setName(panelChannelName(ch));
            QColor areaColor = color;
            areaColor.setAlpha(120);
            area->setColor(areaColor);
            area->setBorderColor(color);
            area->setPen(QPen(color, 1));

            auto *lower = area->lowerSeries();
            lower->clear();
            if (!pts.isEmpty()) {
                lower->append(pts.first().x(), baseline);
                lower->append(pts.last().x(), baseline);
            }
        }
    }

    // ── Y range ──
    if (m_stableYEnabled) {
        m_stableYMax = qMax(m_stableYMax, maxY);
        maxY = m_stableYMax;
    }
    if (m_yAutoScaleEnabled) {
        if (m_log) {
            m_logY->setRange(1, qMax(10.0, maxY * 1.15));
        } else {
            m_y->setRange(0, qMax(10.0, maxY * 1.1));
        }
    }

    updateRoiGraphics();
}

// ═══════════════════════════════════════════════════════════════════════════
// Overview refresh — aggressive max-value downsampling
// ═══════════════════════════════════════════════════════════════════════════

void SpectrumView::refreshOverview() {
    if (!m_snapshot || !m_overviewVisible) return;

    const int ch = m_channels.value(0, 0);
    if (ch < 0 || ch >= m_snapshot->channelCount) return;

    const auto &src = (m_kind == SpectrumKind::Peak)
        ? m_snapshot->peak[ch] : m_snapshot->integral[ch];
    const int dataLen = src.size();
    const int totalChannels = dataLen;

    auto *ovX = static_cast<QValueAxis *>(m_overviewChart->axes(Qt::Horizontal).value(0));
    auto *ovY = static_cast<QValueAxis *>(m_overviewChart->axes(Qt::Vertical).value(0));
    if (ovX) ovX->setRange(0, totalChannels - 1);

    // Reuse or create overview series
    QLineSeries *ovSeries = nullptr;
    const auto existingSeries = m_overviewChart->series();
    if (!existingSeries.isEmpty()) {
        ovSeries = static_cast<QLineSeries *>(existingSeries[0]);
    } else {
        ovSeries = new QLineSeries;
        ovSeries->setColor(QColor("#0066cc"));
        m_overviewChart->addSeries(ovSeries);
        ovSeries->attachAxis(ovX);
        ovSeries->attachAxis(ovY);
    }

    // Aggressive max-value downsampling: target ~500 points
    const int overviewWidth = qMax(100, m_overviewView->width());
    const int binSize = qMax(1, totalChannels / overviewWidth);

    QVector<QPointF> pts;
    pts.reserve(dataLen / binSize + 2);
    double maxY = 1;
    for (int x = 0; x < dataLen; x += binSize) {
        quint64 binMax = 0;
        const int binEnd = qMin(x + binSize, dataLen);
        for (int k = x; k < binEnd; ++k)
            binMax = qMax(binMax, src[k]);
        pts.push_back({double(x), double(binMax)});
        maxY = qMax(maxY, double(binMax));
    }
    // Ensure last channel is represented
    if (dataLen > 0 && (pts.isEmpty() || pts.last().x() < dataLen - 1)) {
        pts.push_back({double(dataLen - 1), double(src[dataLen - 1])});
        maxY = qMax(maxY, double(src[dataLen - 1]));
    }

    ovSeries->replace(pts);
    if (ovY) ovY->setRange(0, qMax(10.0, maxY * 1.1));

    m_overviewView->setViewRect(m_x->min(), m_x->max());
}

QColor SpectrumView::channelColor(int c) const {
    static const QColor a[] = {
        "#2563a6", "#c66a1b", "#2e7d5b", "#b44c5a",
        "#7656a8", "#2b7c83", "#a67c28", "#65758b"
    };
    return a[c % 8];
}

} // namespace mca
