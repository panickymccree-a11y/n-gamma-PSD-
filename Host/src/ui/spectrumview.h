#pragma once
#include "core/types.h"
#include <QWidget>
#include <QChartView>
#include <QGraphicsLineItem>
#include <QGraphicsRectItem>
#include <QGraphicsSimpleTextItem>
#include <QLabel>
#include <QSplitter>

QT_BEGIN_NAMESPACE
class QLineSeries; class QAreaSeries;
class QValueAxis; class QLogValueAxis; class QChart;
QT_END_NAMESPACE

namespace mca {

class SpectrumOverviewView final : public QChartView {
    Q_OBJECT
public:
    explicit SpectrumOverviewView(QChart *chart, QWidget *parent = nullptr);
    void setViewRect(double xMin, double xMax);
signals:
    void viewRectChanged(double xMin, double xMax);
    void resetRequested();
protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
private:
    QGraphicsRectItem *m_viewRect = nullptr;
    bool m_dragging = false;
    bool m_dragEdge = false;
    bool m_dragLeft = false;
    QPointF m_dragStart;
    double m_dragXMin = 0, m_dragXMax = 0;
};

class SpectrumView : public QWidget {
    Q_OBJECT
public:
    explicit SpectrumView(QWidget *parent = nullptr);

    // Data input
    void setSnapshot(std::shared_ptr<const SpectrumSnapshot> snapshot);
    void setKind(SpectrumKind kind);
    void setChannels(const QVector<int> &channels);
    void setCalibration(const Calibration &cal);

    // Display options
    void setLogarithmic(bool on);
    void setFillMode(bool on);
    bool fillMode() const { return m_fillMode; }
    void setCursorEnabled(bool on) { m_cursorEnabled = on; }
    void setOverviewVisible(bool visible);
    bool isOverviewVisible() const { return m_overviewVisible; }
    void setLegendVisible(bool visible);
    bool isLegendVisible() const { return m_legendVisible; }
    void setGridVisible(bool visible);
    bool isGridVisible() const { return m_gridVisible; }
    void setRoiLabelsVisible(bool visible);
    bool areRoiLabelsVisible() const { return m_roiLabelsVisible; }

    // Zoom
    void resetZoom();
    void zoomTo(double left, double right);
    void zoomBy(double factor);
    void zoomYBy(double factor);
    void resetYAutoScale();
    bool isYAutoScaleEnabled() const { return m_yAutoScaleEnabled; }
    QPair<int, int> visibleRange() const;
    QPair<double, double> visibleYRange() const;
    const QVector<int> &displayedChannels() const { return m_channels; }

    // Interaction
    void setInteractionMode(SpectrumInteractionMode mode);
    SpectrumInteractionMode interactionMode() const { return m_interactionMode; }

    // Stable Y
    void setStableYEnabled(bool on);
    void resetStableY();

    // ROI (backward compat + new)
    void setRoi(int left, int right);
    void setRois(const QVector<Roi> &rois);
    void setSelectedRoi(int index);
    const QVector<Roi> &rois() const { return m_rois; }
    int selectedRoi() const { return m_selectedRoi; }

    // Fit curve
    void setFitCurve(const QVector<QPointF> &curve);
    void clearFitCurve();
    bool hasFitCurve() const { return m_fit != nullptr; }

signals:
    void cursorMoved(double channel, quint64 count, double energy);
    void cursorLeft();
    void roiCreated(int left, int right);
    void roiModified(int index, int left, int right);
    void roiSelected(int index);
    void roiDeleteRequested(int index);
    void roiFitRequested(int index);
    void interactionModeChanged(SpectrumInteractionMode mode);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    // Rendering
    void refresh();
    void refreshOverview();
    void rebuildSeries();
    void updateRoiGraphics();
    QColor channelColor(int c) const;

    // Cursor
    void updateCursor(const QPointF &scenePos);
    void onCursorLeft();
    int findRoiAtChannel(int channel) const;

    // Coordinate helpers
    double sceneXToChannel(double sceneX) const;
    int maximumChannel() const;

    // ROI helpers
    void createRoiFromDrag(int left, int right);
    int hitTestRoiEdge(const QPointF &scenePos) const;

    // Interaction handlers
    void handlePress(QMouseEvent *event);
    void handleMove(QMouseEvent *event);
    void handleRelease(QMouseEvent *event);
    void handleDoubleClick(QMouseEvent *event);
    void showContextMenu(const QPoint &globalPos, const QPointF &scenePos);

    // ── Main chart ──
    QChart *m_chart = nullptr;
    QChartView *m_mainView = nullptr;
    QValueAxis *m_x = nullptr;
    QValueAxis *m_y = nullptr;
    QLogValueAxis *m_logY = nullptr;

    // ── Overview ──
    QChart *m_overviewChart = nullptr;
    SpectrumOverviewView *m_overviewView = nullptr;
    QSplitter *m_splitter = nullptr;

    // ── Series ──
    QVector<QLineSeries *> m_series;
    QVector<QAreaSeries *> m_areaSeries;
    QLineSeries *m_fit = nullptr;

    // ── Data ──
    std::shared_ptr<const SpectrumSnapshot> m_snapshot;
    QVector<int> m_channels{0};
    SpectrumKind m_kind = SpectrumKind::Peak;
    bool m_log = false;
    bool m_fillMode = true;
    bool m_cursorEnabled = true;
    bool m_overviewVisible = true;
    bool m_legendVisible = true;
    bool m_gridVisible = true;
    bool m_roiLabelsVisible = true;
    bool m_yAutoScaleEnabled = true;
    Calibration m_calibration;

    // ── Stable Y ──
    bool m_stableYEnabled = false;
    double m_stableYMax = 0;

    // ── Interaction state ──
    SpectrumInteractionMode m_interactionMode = SpectrumInteractionMode::Cursor;
    bool m_dragging = false;
    QPointF m_dragStartScene;
    QPointF m_pressScenePos;
    int m_dragRoiIndex = -1;
    enum class DragTarget { None, RoiMove, RoiLeft, RoiRight };
    DragTarget m_dragTarget = DragTarget::None;
    QGraphicsRectItem *m_zoomRect = nullptr;

    // ── ROI ──
    QVector<Roi> m_rois;
    int m_selectedRoi = -1;
    QVector<QGraphicsLineItem *> m_roiLeftLines, m_roiRightLines;
    QVector<QGraphicsRectItem *> m_roiShades;
    QVector<QGraphicsSimpleTextItem *> m_roiLabels;

    // ── Cursor ──
    QGraphicsLineItem *m_cursorLine = nullptr;
    QLabel *m_cursorLabel = nullptr;
    int m_lastCursorCh = -1;

    // ── Backward compat ──
    int m_roiLeft = -1, m_roiRight = -1;

    // ── Recursion guards ──
    bool m_updatingOverview = false;
    bool m_updatingMain = false;
};

} // namespace mca
