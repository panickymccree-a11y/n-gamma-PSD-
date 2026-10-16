#include "ui/mainwindow.h"
#include "ui/psdpage.h"
#include "ui/spectrumview.h"
#include "ui/waveformpage.h"
#include "core/spectrumfile.h"
#include "core/waveform.h"
#include <QAction>
#include <QActionGroup>
#include <QAbstractItemView>
#include <QAbstractSocket>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDataStream>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QSerialPortInfo>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace mca {

class ChannelTableWidget final : public QTableWidget {
public:
    using QTableWidget::QTableWidget;
    std::function<void(int)> activateCurrentRow;

protected:
    QSize minimumSizeHint() const override {
        QSize hint = QTableWidget::minimumSizeHint();
        hint.setWidth(0);
        return hint;
    }

    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Return ||
            event->key() == Qt::Key_Enter) {
            if (activateCurrentRow)
                activateCurrentRow(currentRow());
            event->accept();
            return;
        }
        QTableWidget::keyPressEvent(event);
    }
};

class EventMapView : public QWidget {
public:
    explicit EventMapView(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(640, 400);
    }
    void setSnapshot(std::shared_ptr<const SpectrumSnapshot> snapshot,
                     AcquisitionMode mode) {
        m_snapshot = std::move(snapshot);
        m_mode = mode;
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#ffffff"));
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QColor("#9aa6b2"));
        const QRect left = rect().adjusted(35, 35, -width() / 2 - 15, -35);
        const QRect right = rect().adjusted(width() / 2 + 15, 35, -35, -35);
        painter.drawRect(left);
        painter.drawRect(right);
        painter.drawText(left.adjusted(4, -28, 0, 0),
            m_mode == AcquisitionMode::FourChannel
                ? QStringLiteral("模块 X/Y 位置图")
                : QStringLiteral("散射/吸收位置叠加图"));
        painter.drawText(right.adjusted(4, -28, 0, 0),
            m_mode == AcquisitionMode::FourChannel
                ? QStringLiteral("模块总能谱")
                : QStringLiteral("散射能量–吸收能量相关图"));
        if (!m_snapshot)
            return;

        painter.setPen(QColor(37, 99, 166, 150));
        const auto &positions = m_snapshot->modulePositions;
        const int positionStart = qMax(0, int(positions.size()) - 20000);
        for (int i = positionStart; i < positions.size(); ++i) {
            const int x = left.left() + qRound(positions[i].x()
                                            * left.width() / 2048.0);
            const int y = left.bottom() - qRound(positions[i].y()
                                               * left.height() / 2048.0);
            painter.drawPoint(x, y);
        }

        if (m_mode == AcquisitionMode::FourChannel) {
            QVector<quint64> sum(DefaultIntegralBins, 0);
            quint64 maximum = 1;
            for (const auto &module : m_snapshot->moduleEnergy)
                for (int i = 0; i < module.size(); ++i) {
                    sum[i] += module[i];
                    maximum = qMax(maximum, sum[i]);
                }
            QPainterPath path;
            for (int pixel = 0; pixel < right.width(); ++pixel) {
                const int bin = pixel * sum.size() / qMax(1, right.width());
                const qreal y = right.bottom()
                    - qreal(sum[bin]) * right.height() / maximum;
                if (pixel == 0) path.moveTo(right.left(), y);
                else path.lineTo(right.left() + pixel, y);
            }
            painter.setPen(QPen(QColor("#a67c28"), 1.2));
            painter.drawPath(path);
        } else {
            const auto &correlation = m_snapshot->coincidenceEnergy;
            qreal maximum = 1;
            for (const auto &point : correlation)
                maximum = qMax(maximum, qMax(point.x(), point.y()));
            painter.setPen(QColor(166, 124, 40, 150));
            const int start = qMax(0, int(correlation.size()) - 20000);
            for (int i = start; i < correlation.size(); ++i) {
                const int x = right.left()
                    + qRound(correlation[i].x() * right.width() / maximum);
                const int y = right.bottom()
                    - qRound(correlation[i].y() * right.height() / maximum);
                painter.drawPoint(x, y);
            }
        }
    }
private:
    std::shared_ptr<const SpectrumSnapshot> m_snapshot;
    AcquisitionMode m_mode = AcquisitionMode::FourChannel;
};

// ═══════════════════════════════════════════════════════════════════════════
// Helper: format count rate with engineering units
// ═══════════════════════════════════════════════════════════════════════════
static QString formatRate(double cps) {
    if (cps >= 1e6) return QStringLiteral("%1 Mcps").arg(cps / 1e6, 0, 'f', 2);
    if (cps >= 1e3) return QStringLiteral("%1 kcps").arg(cps / 1e3, 0, 'f', 2);
    return QStringLiteral("%1 cps").arg(cps, 0, 'f', 1);
}

static QString formatSeconds(double s) {
    if (s < 60) return QStringLiteral("%1 s").arg(s, 0, 'f', 1);
    if (s < 3600) {
        const int m = int(s) / 60;
        const double sec = s - m * 60;
        return QStringLiteral("%1 m %2 s").arg(m).arg(sec, 0, 'f', 1);
    }
    const int h = int(s) / 3600;
    const int m = (int(s) % 3600) / 60;
    return QStringLiteral("%1 h %2 m").arg(h).arg(m);
}

// ═══════════════════════════════════════════════════════════════════════════
// AcquisitionPresetWidget — compact preset bar
// ═══════════════════════════════════════════════════════════════════════════
class AcquisitionPresetWidget : public QWidget {
    Q_OBJECT
public:
    explicit AcquisitionPresetWidget(QWidget *parent = nullptr) : QWidget(parent) {
        auto *lay = new QHBoxLayout(this);
        lay->setContentsMargins(4, 2, 4, 2);
        lay->setSpacing(6);

        m_type = new QComboBox;
        m_type->addItems({QStringLiteral("连续采集"), QStringLiteral("实时时间"), QStringLiteral("活时"), QStringLiteral("总计数")});
        m_type->setCurrentIndex(0);
        m_type->setToolTip(QStringLiteral("采集预置类型\n活时和总计数依赖底层硬件支持，当前可能无法使用"));
        // Disable LiveTime and TotalCounts until backend supports them
        if (auto *stdModel = qobject_cast<QStandardItemModel *>(m_type->model())) {
            if (auto *item = stdModel->item(2)) item->setEnabled(false);
            if (auto *item = stdModel->item(3)) item->setEnabled(false);
        }

        m_value = new QDoubleSpinBox;
        m_value->setRange(1, 86400);
        m_value->setValue(600);
        m_value->setDecimals(1);
        m_value->setSuffix(QStringLiteral(" s"));
        m_value->setEnabled(false);

        m_elapsed = new QLabel(QStringLiteral("—"));
        m_elapsed->setMinimumWidth(80);
        m_remaining = new QLabel(QStringLiteral("—"));
        m_remaining->setMinimumWidth(80);

        lay->addWidget(new QLabel(QStringLiteral("预置:")));
        lay->addWidget(m_type);
        lay->addWidget(m_value);
        lay->addWidget(new QLabel(QStringLiteral("已用:")));
        lay->addWidget(m_elapsed);
        lay->addWidget(new QLabel(QStringLiteral("剩余:")));
        lay->addWidget(m_remaining);
        lay->addStretch();

        connect(m_type, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
            m_value->setEnabled(idx > 0);
            if (idx == 0) { m_value->setSuffix(QStringLiteral("")); m_value->setDecimals(0); }
            else if (idx == 3) { m_value->setSuffix(QStringLiteral(" evt")); m_value->setDecimals(0); m_value->setRange(1, 2e7); }
            else { m_value->setSuffix(QStringLiteral(" s")); m_value->setDecimals(1); m_value->setRange(1, 86400); }
        });
    }

    AcquisitionPreset preset() const {
        AcquisitionPreset p;
        switch (m_type->currentIndex()) {
        case 1: p.useRealTime = true; p.realTimeSeconds = m_value->value(); break;
        case 2: p.useLiveTime = true; p.liveTimeSeconds = m_value->value(); break;
        case 3: p.useEventCount = true; p.eventCount = quint64(m_value->value()); break;
        default: break;
        }
        return p;
    }

    void setElapsed(double seconds) {
        m_elapsed->setText(formatSeconds(seconds));
    }

    void setRemaining(double seconds) {
        if (m_type->currentIndex() == 0)
            m_remaining->setText(QStringLiteral("∞"));
        else if (seconds <= 0)
            m_remaining->setText(QStringLiteral("完成"));
        else
            m_remaining->setText(formatSeconds(seconds));
    }

    void reset() { m_elapsed->setText(QStringLiteral("—")); m_remaining->setText(QStringLiteral("—")); }
    void setEditingEnabled(bool enabled) { m_type->setEnabled(enabled); m_value->setEnabled(enabled && m_type->currentIndex() > 0); }

private:
    QComboBox *m_type;
    QDoubleSpinBox *m_value;
    QLabel *m_elapsed, *m_remaining;
};

// ═══════════════════════════════════════════════════════════════════════════
// MainWindow constructor / destructor
// ═══════════════════════════════════════════════════════════════════════════

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("MCA Studio — 多通道分析器"));
    resize(1500, 900);

    createActions();
    // Status widgets live in the right analysis dock.  Build them first so
    // buildDockWidgets() can place the complete panel above the ROI controls.
    buildStatusBar();
    buildDockWidgets();
    buildMenus();
    buildToolBars();
    buildUi();
    connectSignals();
    restoreSettings();

    // Initial state
    updateActionsForState();
}

MainWindow::~MainWindow() {
    saveSettings();
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (m_acquisitionState == AcquisitionState::Starting ||
        m_acquisitionState == AcquisitionState::Running ||
        m_acquisitionState == AcquisitionState::Paused ||
        m_acquisitionState == AcquisitionState::Stopping) {
        if (!m_closeAfterStop) {
            const auto answer = QMessageBox::question(
                this, QStringLiteral("停止采集并退出"),
                QStringLiteral("采集尚未结束。是否停止采集、完成文件写入后退出？"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes) {
                event->ignore();
                return;
            }
            m_closeAfterStop = true;
            if (m_acquisitionState != AcquisitionState::Stopping)
                m_acq.stop();
            statusBar()->showMessage(QStringLiteral("正在安全停止采集…"));
        }
        event->ignore();
        return;
    }
    saveSettings();
    event->accept();
}

// ═══════════════════════════════════════════════════════════════════════════
// Action creation — all QActions shared between menus and toolbars
// ═══════════════════════════════════════════════════════════════════════════

void MainWindow::createActions() {
    m_actOpen = new QAction(QStringLiteral("打开能谱…"), this);
    m_actOpen->setShortcut(QKeySequence::Open);
    m_actOpen->setToolTip(QStringLiteral("打开 MCAS v1 能谱文件"));
    m_actSave = new QAction(QStringLiteral("保存当前谱"), this);
    m_actSave->setShortcut(QKeySequence::Save);
    m_actSave->setToolTip(QStringLiteral("保存当前累计能谱"));
    m_actOpenList = new QAction(QStringLiteral("打开 List…"), this);
    m_actOpenList->setToolTip(QStringLiteral("回放已有的 MCAL List 文件"));
    m_actSetEventText = new QAction(QStringLiteral("设置事件 TXT 文件…"), this);
    m_actSetEventText->setToolTip(QStringLiteral("选择下次单通道采集的事件 TXT 文件"));
    m_actExit = new QAction(QStringLiteral("退出"), this);
    m_actExit->setShortcut(QKeySequence::Quit);

    m_actStart = new QAction(QStringLiteral("开始采集"), this);
    m_actStart->setShortcut(Qt::Key_F5);
    m_actStart->setToolTip(QStringLiteral("连接 FPGA 并开始 UDP 采集"));
    m_actPause = new QAction(QStringLiteral("暂停"), this);
    m_actPause->setShortcut(Qt::Key_F6);
    m_actStop = new QAction(QStringLiteral("停止"), this);
    m_actStop->setShortcut(Qt::Key_F7);
    m_actClear = new QAction(QStringLiteral("清谱"), this);
    m_actClear->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    m_actConnect = new QAction(QStringLiteral("设备连接…"), this);

    m_actLinearY = new QAction(QStringLiteral("线性 Y"), this);
    m_actLinearY->setShortcut(Qt::Key_L);
    m_actLinearY->setCheckable(true);
    m_actLinearY->setChecked(true);
    m_actLogY = new QAction(QStringLiteral("对数 Y"), this);
    m_actLogY->setShortcut(Qt::Key_G);
    m_actLogY->setCheckable(true);
    auto *yGroup = new QActionGroup(this);
    yGroup->addAction(m_actLinearY); yGroup->addAction(m_actLogY);
    yGroup->setExclusive(true);

    m_actFill = new QAction(QStringLiteral("填充谱"), this);
    m_actFill->setCheckable(true);
    m_actFill->setChecked(true);

    m_actLegend = new QAction(QStringLiteral("图例"), this);
    m_actLegend->setCheckable(true);
    m_actLegend->setChecked(true);
    m_actOverview = new QAction(QStringLiteral("全谱导航"), this);
    m_actOverview->setCheckable(true);
    m_actOverview->setChecked(true);
    m_actGrid = new QAction(QStringLiteral("网格"), this);
    m_actGrid->setCheckable(true);
    m_actGrid->setChecked(true);
    m_actRoiLabels = new QAction(QStringLiteral("ROI 标签"), this);
    m_actRoiLabels->setCheckable(true);
    m_actRoiLabels->setChecked(true);
    m_actFullRange = new QAction(QStringLiteral("全谱"), this);
    m_actFullRange->setShortcut(Qt::Key_Home);
    m_actZoomIn = new QAction(QStringLiteral("放大"), this);
    m_actZoomIn->setShortcut(Qt::Key_Plus);
    m_actZoomOut = new QAction(QStringLiteral("缩小"), this);
    m_actZoomOut->setShortcut(Qt::Key_Minus);
    m_actSingleCh = new QAction(QStringLiteral("单通道"), this);
    m_actMultiCh = new QAction(QStringLiteral("多通道叠加"), this);
    m_actGrid8 = new QAction(QStringLiteral("8 通道布局"), this);

    m_actCursorMode = new QAction(QStringLiteral("光标"), this);
    m_actCursorMode->setCheckable(true);
    m_actCursorMode->setChecked(true);
    m_actZoomMode = new QAction(QStringLiteral("框选放大"), this);
    m_actZoomMode->setCheckable(true);
    m_actPanMode = new QAction(QStringLiteral("平移"), this);
    m_actPanMode->setCheckable(true);
    m_actCreateRoiMode = new QAction(QStringLiteral("创建 ROI"), this);
    m_actCreateRoiMode->setCheckable(true);
    m_actEditRoiMode = new QAction(QStringLiteral("编辑 ROI"), this);
    m_actEditRoiMode->setCheckable(true);
    auto *interactGroup = new QActionGroup(this);
    interactGroup->addAction(m_actCursorMode); interactGroup->addAction(m_actZoomMode);
    interactGroup->addAction(m_actPanMode); interactGroup->addAction(m_actCreateRoiMode);
    interactGroup->addAction(m_actEditRoiMode);
    interactGroup->setExclusive(true);

    m_actNewRoi = new QAction(QStringLiteral("新建 ROI"), this);
    m_actNewRoi->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    m_actDeleteRoi = new QAction(QStringLiteral("删除选中 ROI"), this);
    m_actDeleteRoi->setShortcut(Qt::Key_Delete);
    m_actClearRois = new QAction(QStringLiteral("清除全部 ROI"), this);
    m_actPrevRoi = new QAction(QStringLiteral("上一个 ROI"), this);
    m_actPrevRoi->setShortcut(Qt::Key_PageUp);
    m_actNextRoi = new QAction(QStringLiteral("下一个 ROI"), this);
    m_actNextRoi->setShortcut(Qt::Key_PageDown);

    m_actSingleFit = new QAction(QStringLiteral("单峰拟合"), this);
    m_actSingleFit->setShortcut(Qt::Key_F);
    m_actClearFit = new QAction(QStringLiteral("清除拟合"), this);

    m_actEnergyCal = new QAction(QStringLiteral("能量标定…"), this);
    m_actDefaultLayout = new QAction(QStringLiteral("恢复默认布局"), this);
    m_actAbout = new QAction(QStringLiteral("关于 MCA Studio"), this);

    m_actOpen->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    m_actSave->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    m_actStart->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_actPause->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
    m_actStop->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    m_actClear->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
    m_actFullRange->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));

    // QAction's default tooltip follows its text, but set it explicitly so
    // toolbar and menu actions always expose a useful description.
    for (auto *action : findChildren<QAction *>()) {
        if (action->toolTip().isEmpty()) {
            QString tip = action->text();
            tip.remove(QLatin1Char('&'));
            action->setToolTip(tip);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Menu bar
// ═══════════════════════════════════════════════════════════════════════════

void MainWindow::buildMenus() {
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    fileMenu->addAction(m_actOpen);
    fileMenu->addAction(m_actSave);
    fileMenu->addSeparator();
    fileMenu->addAction(m_actSetEventText);
    fileMenu->addAction(m_actOpenList);
    fileMenu->addSeparator();
    m_recentFilesMenu = fileMenu->addMenu(QStringLiteral("最近文件"));
    rebuildRecentFilesMenu();
    fileMenu->addSeparator();
    fileMenu->addAction(m_actExit);

    auto *acqMenu = menuBar()->addMenu(QStringLiteral("采集(&A)"));
    acqMenu->addAction(m_actStart);
    acqMenu->addAction(m_actPause);
    acqMenu->addAction(m_actStop);
    acqMenu->addAction(m_actClear);
    acqMenu->addAction(m_actConnect);

    auto *dispMenu = menuBar()->addMenu(QStringLiteral("显示(&D)"));
    dispMenu->addAction(m_actLinearY);
    dispMenu->addAction(m_actLogY);
    dispMenu->addSeparator();
    dispMenu->addAction(m_actFill);
    dispMenu->addAction(m_actLegend);
    dispMenu->addAction(m_actOverview);
    dispMenu->addAction(m_actGrid);
    dispMenu->addAction(m_actRoiLabels);
    dispMenu->addSeparator();
    dispMenu->addAction(m_actFullRange);
    dispMenu->addAction(m_actZoomIn);
    dispMenu->addAction(m_actZoomOut);
    dispMenu->addSeparator();
    dispMenu->addAction(m_actSingleCh);
    dispMenu->addAction(m_actMultiCh);
    dispMenu->addAction(m_actGrid8);

    auto *roiMenu = menuBar()->addMenu(QStringLiteral("ROI(&R)"));
    roiMenu->addAction(m_actNewRoi);
    roiMenu->addAction(m_actDeleteRoi);
    roiMenu->addAction(m_actClearRois);
    roiMenu->addSeparator();
    roiMenu->addAction(m_actCreateRoiMode);
    roiMenu->addAction(m_actEditRoiMode);
    roiMenu->addSeparator();
    roiMenu->addAction(m_actPrevRoi);
    roiMenu->addAction(m_actNextRoi);

    auto *analysisMenu = menuBar()->addMenu(QStringLiteral("分析(&N)"));
    analysisMenu->addAction(m_actSingleFit);
    analysisMenu->addAction(m_actClearFit);

    auto *calMenu = menuBar()->addMenu(QStringLiteral("标定(&C)"));
    calMenu->addAction(m_actEnergyCal);

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("视图(&V)"));
    m_panelsMenu = viewMenu->addMenu(QStringLiteral("面板"));
    for (auto *dock : {m_connectionDock, m_channelDock, m_acquisitionDock,
                       m_moduleDock, m_pairDock, m_analysisDock})
        if (dock)
            m_panelsMenu->addAction(dock->toggleViewAction());
    viewMenu->addSeparator();
    viewMenu->addAction(m_actDefaultLayout);

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    helpMenu->addAction(m_actAbout);
}

// ═══════════════════════════════════════════════════════════════════════════
// Toolbars
// ═══════════════════════════════════════════════════════════════════════════

void MainWindow::buildToolBars() {
    auto *acqTb = addToolBar(QStringLiteral("采集"));
    acqTb->setObjectName(QStringLiteral("acqToolBar"));
    acqTb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    acqTb->setIconSize(QSize(16, 16));
    acqTb->addAction(m_actStart);
    acqTb->addAction(m_actPause);
    acqTb->addAction(m_actStop);
    acqTb->addAction(m_actClear);

    auto *specTb = addToolBar(QStringLiteral("谱图与ROI"));
    specTb->setObjectName(QStringLiteral("specToolBar"));
    specTb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    specTb->setIconSize(QSize(16, 16));
    specTb->addAction(m_actCursorMode);
    specTb->addAction(m_actZoomMode);
    specTb->addAction(m_actPanMode);
    specTb->addSeparator();
    specTb->addAction(m_actFullRange);
    specTb->addAction(m_actLinearY);
    specTb->addAction(m_actLogY);
    specTb->addAction(m_actOverview);
    specTb->addSeparator();
    specTb->addAction(m_actCreateRoiMode);
    specTb->addAction(m_actEditRoiMode);
    specTb->addAction(m_actDeleteRoi);
    specTb->addAction(m_actSingleFit);
}

// ═══════════════════════════════════════════════════════════════════════════
// Status bar
// ═══════════════════════════════════════════════════════════════════════════

void MainWindow::buildStatusBar() {
    m_statusDevice = new QLabel(QStringLiteral("设备 未连接"));
    m_statusDevice->setObjectName(QStringLiteral("deviceStatusBadge"));
    m_statusDevice->setProperty("state", QStringLiteral("offline"));
    m_statusDevice->setAlignment(Qt::AlignCenter);
    m_statusDevice->setToolTip(QStringLiteral("尚未收到 FPGA 控制应答"));

    m_statusState = new QLabel(QStringLiteral("空闲"));
    m_statusState->setObjectName(QStringLiteral("acquisitionStatusBadge"));
    m_statusState->setProperty("state", QStringLiteral("idle"));
    m_statusState->setAlignment(Qt::AlignCenter);

    m_statusCh = new QLabel(panelChannelName(0));
    m_statusCh->setAlignment(Qt::AlignCenter);

    m_statusLive = new QLabel(QStringLiteral("LIVE —"));

    m_statusDead = new QLabel(QStringLiteral("DEAD —"));

    m_statusRate = new QLabel(QStringLiteral("— cps"));

    m_statusPkt = new QLabel(QStringLiteral("— pkt/s"));

    m_statusLoss = new QLabel(QStringLiteral("LOSS 0"));

    m_statusPileup = new QLabel(QStringLiteral("PILEUP 0.00%"));
    m_statusPileup->setToolTip(QStringLiteral("堆积事件数 / 接收到的有效事件总数"));

    m_statusTextFile = new QLabel(QStringLiteral("TXT 关闭"));
    m_statusTextFile->setToolTip(QStringLiteral("单通道事件 TXT 记录状态"));

    m_statusCursor = new QLabel;
    m_statusCursor->setWordWrap(true);

    m_queueBar = new QProgressBar;
    m_queueBar->setRange(0, 100);
    m_queueBar->setValue(0);
    m_queueBar->setMaximumWidth(80);
    m_queueBar->setMaximumHeight(16);
    m_queueBar->setToolTip(QStringLiteral("事件队列占用率（当前底层未提供真实数据）"));
    m_queueBar->hide(); // Hidden until backend provides real data

    auto *panel = new QGroupBox(QStringLiteral("采集状态"));
    panel->setObjectName(QStringLiteral("acquisitionStatusPanel"));
    auto *grid = new QGridLayout(panel);
    grid->setContentsMargins(6, 12, 6, 6);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(4);
    grid->addWidget(m_statusDevice, 0, 0);
    grid->addWidget(m_statusState, 0, 1);
    grid->addWidget(m_statusCh, 1, 0);
    grid->addWidget(m_statusLive, 1, 1);
    grid->addWidget(m_statusDead, 2, 0);
    grid->addWidget(m_statusRate, 2, 1);
    grid->addWidget(m_statusPkt, 3, 0);
    grid->addWidget(m_statusLoss, 3, 1);
    grid->addWidget(m_statusPileup, 4, 0);
    grid->addWidget(m_statusTextFile, 4, 1);
    grid->addWidget(m_statusCursor, 5, 0, 1, 2);
    grid->addWidget(m_queueBar, 6, 0, 1, 2);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    m_statusPanel = panel;

    // Keep the native status bar for short-lived operation/error messages,
    // but no persistent telemetry remains at the bottom of the window.
    statusBar()->setSizeGripEnabled(true);
}

// ═══════════════════════════════════════════════════════════════════════════
// Dock widgets
// ═══════════════════════════════════════════════════════════════════════════

#if 0
void MainWindow::buildDockWidgets() {
    // ── Left dock: device & channels ──
    auto *deviceDock = new QDockWidget(QStringLiteral("探测器与通道"), this);
    deviceDock->setObjectName(QStringLiteral("deviceDock"));
    deviceDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    auto *deviceWidget = new QWidget;
    auto *deviceLayout = new QVBoxLayout(deviceWidget);
    deviceLayout->setContentsMargins(4, 4, 4, 4);
    deviceLayout->setSpacing(6);

    // Connection group (compact)
    auto *connBox = new QGroupBox(QStringLiteral("FPGA 连接"));
    auto *connForm = new QFormLayout(connBox);
    connForm->setContentsMargins(4, 12, 4, 4);
    auto *ip = new QLineEdit("192.168.1.10");
    ip->setObjectName("deviceIp");
    auto *dataPort = new QSpinBox;
    dataPort->setRange(1, 65535);
    dataPort->setValue(9000);
    dataPort->setObjectName("dataPort");
    auto *controlPort = new QSpinBox;
    controlPort->setRange(1, 65535);
    controlPort->setValue(9001);
    controlPort->setObjectName("controlPort");
    m_acquisitionMode = new QComboBox;
    m_acquisitionMode->addItems({
        QStringLiteral("单通道独立事件"),
        QStringLiteral("Compton 四通道模块"),
        QStringLiteral("散射/吸收层符合")
    });
    m_includePileup = new QCheckBox(QStringLiteral("堆积事件计入能谱"));
    m_includePileup->setChecked(true);
    m_includePileup->setToolTip(QStringLiteral("取消后，带 pileup 标记的事件仍计入总事件和堆积占比，但不填充峰值与积分能谱"));
    connForm->addRow(QStringLiteral("地址"), ip);
    connForm->addRow(QStringLiteral("数据端口"), dataPort);
    connForm->addRow(QStringLiteral("控制端口"), controlPort);
    connForm->addRow(QStringLiteral("采集模式"), m_acquisitionMode);
    connForm->addRow(m_includePileup);
    deviceLayout->addWidget(connBox);

    auto *baselineBox = new QGroupBox(QStringLiteral("基线与前端（全通道）"));
    auto *baselineLayout = new QHBoxLayout(baselineBox);
    baselineLayout->setContentsMargins(4, 4, 4, 4);
    m_baselineTracking = new QCheckBox(QStringLiteral("启用基线跟踪"));
    m_baselineTracking->setChecked(false);
    m_baselineTracking->setToolTip(QStringLiteral("关闭时固定使用 2048；启用后从 2048 开始快速训练"));
    m_applyBaseline = new QPushButton(QStringLiteral("应用"));
    baselineLayout->addWidget(m_baselineTracking);
    baselineLayout->addWidget(m_applyBaseline);
    connect(m_applyBaseline, &QPushButton::clicked, this, &MainWindow::applyBaselineTracking);
    deviceLayout->addWidget(baselineBox);

    // Channel table
    m_channels = new QTableWidget(8, 8);
    m_channels->setHorizontalHeaderLabels({QStringLiteral("启用"), QStringLiteral("通道"), QStringLiteral("阈值(LSB)"), QStringLiteral("发送"), QStringLiteral("极性"), QStringLiteral("相对增益"), QStringLiteral("基线值"), QStringLiteral("基线状态")});
    m_channels->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_channels->setMaximumHeight(320);
    for (int row = 0; row < 8; ++row) {
        auto *enabled = new QTableWidgetItem;
        enabled->setCheckState(Qt::Checked);
        m_channels->setItem(row, 0, enabled);
        auto *channel = new QTableWidgetItem(panelChannelName(row));
        channel->setFlags(channel->flags() & ~Qt::ItemIsEditable);
        m_channels->setItem(row, 1, channel);
        auto *threshold = new QSpinBox;
        threshold->setRange(1, 4095);
        threshold->setValue(64);
        m_channels->setCellWidget(row, 2, threshold);
        auto *sendThreshold = new QPushButton(QStringLiteral("发送"));
        sendThreshold->setMaximumWidth(50);
        m_channels->setCellWidget(row, 3, sendThreshold);
        auto *polarity = new QComboBox;
        polarity->addItems({QStringLiteral("正"), QStringLiteral("负")});
        polarity->setMinimumWidth(54);
        m_channels->setCellWidget(row, 4, polarity);
        auto *gain = new QDoubleSpinBox;
        gain->setRange(0.0, 15.9998);
        gain->setDecimals(4);
        gain->setSingleStep(0.01);
        gain->setValue(1.0);
        gain->setToolTip(QStringLiteral("Q4.12 相对增益，1.0 对应 0x1000"));
        m_channels->setCellWidget(row, 5, gain);
        auto *baselineValue = new QTableWidgetItem(QStringLiteral("2048"));
        baselineValue->setFlags(baselineValue->flags() & ~Qt::ItemIsEditable);
        baselineValue->setTextAlignment(Qt::AlignCenter);
        m_channels->setItem(row, 6, baselineValue);
        auto *baselineState = new QTableWidgetItem(QStringLiteral("固定"));
        baselineState->setFlags(baselineState->flags() & ~Qt::ItemIsEditable);
        baselineState->setTextAlignment(Qt::AlignCenter);
        m_channels->setItem(row, 7, baselineState);
        connect(threshold, qOverload<int>(&QSpinBox::valueChanged), this, [this, row] { saveChannelConfig(row); });
        connect(sendThreshold, &QPushButton::clicked, this, [this, row] { sendChannelThreshold(row); });
        connect(polarity, &QComboBox::currentIndexChanged, this, [this, row] { saveChannelConfig(row); });
        connect(gain, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, [this, row] { saveChannelConfig(row); });
    }
    connect(m_channels, &QTableWidget::cellChanged, this, &MainWindow::applyChannelConfig);
    connect(m_channels, &QTableWidget::itemSelectionChanged, this, &MainWindow::changeView);
    deviceLayout->addWidget(m_channels, 1);

#if 0
    auto *moduleBox = new QGroupBox(QStringLiteral("四通道模块"));
    auto *moduleLayout = new QVBoxLayout(moduleBox);
    moduleLayout->setContentsMargins(4, 8, 4, 4);
    m_modules = new QTableWidget(0, 7);
    m_modules->setHorizontalHeaderLabels({
        QStringLiteral("启用"), QStringLiteral("模块"),
        QStringLiteral("窗口/采样"), QStringLiteral("能量门"),
        QStringLiteral("最小能量"), QStringLiteral("最大能量"),
        QStringLiteral("质量掩码")
    });
    m_modules->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_modules->setMaximumHeight(150);
    ensureModuleRows(2);
    connect(m_modules, &QTableWidget::cellChanged, this,
            [this](int row, int) { applyModuleConfig(row); });
    moduleLayout->addWidget(m_modules);
    deviceLayout->addWidget(moduleBox);

    auto *pairBox = new QGroupBox(QStringLiteral("散射/吸收配对（每侧最多 4 个模块）"));
    auto *pairLayout = new QVBoxLayout(pairBox);
    pairLayout->setContentsMargins(4, 8, 4, 4);
    m_pairs = new QTableWidget(0, 13);
    m_pairs->setHorizontalHeaderLabels({
        QStringLiteral("启用"), QStringLiteral("配对"),
        QStringLiteral("散射"), QStringLiteral("吸收"),
        QStringLiteral("ΔT最小"), QStringLiteral("ΔT最大"),
        QStringLiteral("散射门"), QStringLiteral("散射E最小"),
        QStringLiteral("散射E最大"), QStringLiteral("吸收门"),
        QStringLiteral("吸收E最小"), QStringLiteral("吸收E最大"),
        QStringLiteral("质量掩码")
    });
    m_pairs->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_pairs->setMaximumHeight(170);
    ensurePairRows(1);
    connect(m_pairs, &QTableWidget::cellChanged, this,
            [this](int row, int) { applyPairConfig(row); });
    pairLayout->addWidget(m_pairs);
    deviceLayout->addWidget(pairBox);
#endif

    // Global threshold
    auto *globalBox = new QGroupBox(QStringLiteral("统一阈值"));
    auto *globalLayout = new QHBoxLayout(globalBox);
    globalLayout->setContentsMargins(4, 4, 4, 4);
    m_globalThreshold = new QSpinBox;
    m_globalThreshold->setRange(1, 4095);
    m_globalThreshold->setValue(64);
    auto *applyAll = new QPushButton(QStringLiteral("应用全部"));
    globalLayout->addWidget(m_globalThreshold);
    globalLayout->addWidget(applyAll);
    connect(applyAll, &QPushButton::clicked, this, &MainWindow::applyGlobalThreshold);
    deviceLayout->addWidget(globalBox);

    // Global integration window (80 MHz, 12.5 ns/sample).  Values are
    // quantized to integer samples before entering ChannelConfig.
    auto *integrationBox = new QGroupBox(QStringLiteral("积分窗口（全通道）"));
    auto *integrationForm = new QFormLayout(integrationBox);
    integrationForm->setContentsMargins(4, 12, 4, 4);
    m_integrationPostUs = new QDoubleSpinBox;
    m_integrationPostUs->setDecimals(4);
    m_integrationPostUs->setRange(0.1000, 10.0000);
    m_integrationPostUs->setSingleStep(0.0125);
    m_integrationPostUs->setValue(3.2000);
    m_integrationPostUs->setSuffix(QStringLiteral(" μs"));
    m_integrationPreUs = new QDoubleSpinBox;
    m_integrationPreUs->setDecimals(4);
    m_integrationPreUs->setRange(0.0, 19.9920);
    m_integrationPreUs->setSingleStep(0.0125);
    m_integrationPreUs->setValue(0.1000);
    m_integrationPreUs->setSuffix(QStringLiteral(" μs"));
    m_integrationSummary = new QLabel;
    integrationForm->addRow(QStringLiteral("触发后窗口"), m_integrationPostUs);
    integrationForm->addRow(QStringLiteral("前置窗口"), m_integrationPreUs);
    integrationForm->addRow(m_integrationSummary);
    connect(m_integrationPostUs, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this] { updateIntegrationWindowSummary(); });
    connect(m_integrationPreUs, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this] { updateIntegrationWindowSummary(); });
    updateIntegrationWindowSummary();
    deviceLayout->addWidget(integrationBox);

    deviceDock->setWidget(deviceWidget);
    addDockWidget(Qt::LeftDockWidgetArea, deviceDock);

    // ── Right dock: analysis panel ──
    auto *analysisDock = new QDockWidget(QStringLiteral("ROI 与峰分析"), this);
    analysisDock->setObjectName(QStringLiteral("analysisDock"));
    auto *analysisWidget = new QWidget;
    auto *analysisLayout = new QVBoxLayout(analysisWidget);
    analysisLayout->setContentsMargins(4, 4, 4, 4);
    analysisLayout->setSpacing(6);

    // ROI entry
    auto *roiBox = new QGroupBox(QStringLiteral("选定区域"));
    auto *roiForm = new QFormLayout(roiBox);
    roiForm->setContentsMargins(4, 12, 4, 4);
    m_roiLeft = new QSpinBox;
    m_roiRight = new QSpinBox;
    m_roiLeft->setRange(0, 16383);
    m_roiRight->setRange(0, 16383);
    m_roiLeft->setValue(400);
    m_roiRight->setValue(650);
    auto *addRoiBtn = new QPushButton(QStringLiteral("添加 ROI"));
    roiForm->addRow(QStringLiteral("左边界"), m_roiLeft);
    roiForm->addRow(QStringLiteral("右边界"), m_roiRight);
    roiForm->addRow(addRoiBtn);
    analysisLayout->addWidget(roiBox);

    // ROI list
    m_roiTable = new QTableWidget(0, 3);
    m_roiTable->setHorizontalHeaderLabels({QStringLiteral("名称"), QStringLiteral("左"), QStringLiteral("右")});
    m_roiTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_roiTable->setMaximumHeight(200);
    analysisLayout->addWidget(m_roiTable);

    m_roiInfo = new QLabel(QStringLiteral("请选择 ROI"));
    m_roiInfo->setWordWrap(true);
    analysisLayout->addWidget(m_roiInfo);

    // Fit controls
    auto *fitBox = new QGroupBox(QStringLiteral("峰拟合"));
    auto *fitForm = new QFormLayout(fitBox);
    fitForm->setContentsMargins(4, 12, 4, 4);
    m_peakCount = new QSpinBox;
    m_peakCount->setRange(1, 4);
    m_background = new QComboBox;
    m_background->addItems({QStringLiteral("线性背景"), QStringLiteral("常数背景")});
    auto *fitBtn = new QPushButton(QStringLiteral("拟合"));
    auto *clearFitBtn = new QPushButton(QStringLiteral("清除拟合"));
    m_fitInfo = new QLabel(QStringLiteral("未拟合"));
    m_fitInfo->setWordWrap(true);
    fitForm->addRow(QStringLiteral("峰数量"), m_peakCount);
    fitForm->addRow(QStringLiteral("背景"), m_background);
    fitForm->addRow(fitBtn);
    fitForm->addRow(clearFitBtn);
    fitForm->addRow(m_fitInfo);
    analysisLayout->addWidget(fitBox);

    analysisDock->setWidget(analysisWidget);
    addDockWidget(Qt::RightDockWidgetArea, analysisDock);

    // Connect dock buttons
    connect(addRoiBtn, &QPushButton::clicked, this, &MainWindow::addRoi);
    connect(fitBtn, &QPushButton::clicked, this, &MainWindow::fitRoi);
    connect(clearFitBtn, &QPushButton::clicked, this, &MainWindow::clearFit);
    connect(m_roiTable, &QTableWidget::itemSelectionChanged, this, &MainWindow::refreshRoiInfo);
}

// ═══════════════════════════════════════════════════════════════════════════
// Central area
// ═══════════════════════════════════════════════════════════════════════════

#endif

void MainWindow::buildDockWidgets() {
    const auto prepareDock = [this](QDockWidget *dock, const QString &name) {
        dock->setObjectName(name);
        dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        dock->setMinimumWidth(220);
        dock->setFeatures(QDockWidget::DockWidgetClosable |
                          QDockWidget::DockWidgetMovable |
                          QDockWidget::DockWidgetFloatable);
        addDockWidget(Qt::LeftDockWidgetArea, dock);
    };

    // Connection
    m_connectionDock = new QDockWidget(QStringLiteral("设备连接"), this);
    prepareDock(m_connectionDock, QStringLiteral("connectionDockV2"));
    auto *connectionWidget = new QWidget;
    auto *connectionForm = new QFormLayout(connectionWidget);
    connectionForm->setContentsMargins(10, 10, 10, 10);
    connectionForm->setHorizontalSpacing(12);
    connectionForm->setVerticalSpacing(8);
    connectionForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *ip = new QLineEdit(QStringLiteral("192.168.1.10"));
    ip->setObjectName(QStringLiteral("deviceIp"));
    auto *dataPort = new QSpinBox;
    dataPort->setObjectName(QStringLiteral("dataPort"));
    dataPort->setRange(1, 65535);
    dataPort->setValue(9000);
    auto *controlPort = new QSpinBox;
    controlPort->setObjectName(QStringLiteral("controlPort"));
    controlPort->setRange(1, 65535);
    controlPort->setValue(9001);
    m_connectionType = new QComboBox;
    m_connectionType->setObjectName(QStringLiteral("connectionType"));
    m_connectionType->addItem(QStringLiteral("UDP"), 0);
    m_connectionType->addItem(QStringLiteral("串口"), 1);
    m_serialPort = new QComboBox;
    m_serialPort->setObjectName(QStringLiteral("serialPort"));
    auto *refreshSerial = new QPushButton(QStringLiteral("刷新"));
    auto *serialRow = new QWidget;
    auto *serialLayout = new QHBoxLayout(serialRow);
    serialLayout->setContentsMargins(0, 0, 0, 0);
    serialLayout->addWidget(m_serialPort, 1);
    serialLayout->addWidget(refreshSerial);
    auto refreshSerialPorts = [this] {
        const QString selected = m_serialPort->currentData().toString();
        m_serialPort->clear();
        for (const auto &port : QSerialPortInfo::availablePorts())
            m_serialPort->addItem(QStringLiteral("%1 — %2")
                                      .arg(port.portName(), port.description()),
                                  port.portName());
        const int restore = m_serialPort->findData(selected);
        if (restore >= 0) m_serialPort->setCurrentIndex(restore);
    };
    refreshSerialPorts();
    connect(refreshSerial, &QPushButton::clicked, this, refreshSerialPorts);
    m_acquisitionMode = new QComboBox;
    m_acquisitionMode->addItem(QStringLiteral("自动触发"),
                               int(AcquisitionMode::AutomaticTrigger));
    m_acquisitionMode->addItem(QStringLiteral("外置触发"),
                               int(AcquisitionMode::ExternalTrigger));
    m_acquisitionMode->addItem(QStringLiteral("波形模式"),
                               int(AcquisitionMode::Waveform));
    m_acquisitionMode->addItem(QStringLiteral("PSD 调试（尾部电荷比）"),int(AcquisitionMode::Psd));
    m_acquisitionMode->setCurrentIndex(0);
    connectionForm->addRow(QStringLiteral("连接方式"), m_connectionType);
    connectionForm->addRow(QStringLiteral("FPGA 地址"), ip);
    connectionForm->addRow(QStringLiteral("数据端口"), dataPort);
    connectionForm->addRow(QStringLiteral("控制端口"), controlPort);
    connectionForm->addRow(QStringLiteral("串口 (500 kbaud)"), serialRow);
    connectionForm->addRow(QStringLiteral("采集模式"), m_acquisitionMode);
    m_connectionDock->setWidget(connectionWidget);
    const auto updateTransport = [this, ip, dataPort, controlPort, serialRow] {
        const bool serial = m_connectionType->currentData().toInt() == 1;
        ip->setVisible(!serial);
        dataPort->setVisible(!serial);
        controlPort->setVisible(!serial);
        serialRow->setVisible(serial);
        if (m_acquisitionMode) {
            const int wave = m_acquisitionMode->findData(int(AcquisitionMode::Waveform));
            if (wave >= 0)
                qobject_cast<QStandardItemModel *>(m_acquisitionMode->model())
                    ->item(wave)->setEnabled(!serial);
            const int psd=m_acquisitionMode->findData(int(AcquisitionMode::Psd));
            if(auto model=qobject_cast<QStandardItemModel*>(m_acquisitionMode->model()))
                if(auto item=model->item(psd))item->setEnabled(!serial && (!m_deviceCapabilities.formatFlagsValid || m_deviceCapabilities.supportsPsd()));
            if (serial && (m_acquisitionMode->currentData().toInt()==int(AcquisitionMode::Waveform) ||
                           m_acquisitionMode->currentData().toInt()==int(AcquisitionMode::Psd)))
                m_acquisitionMode->setCurrentIndex(0);
        }
        if (m_saveEventText) m_saveEventText->setEnabled(!serial);
        if (m_includePileup) {
            m_includePileup->setChecked(true);
            m_includePileup->setEnabled(!serial);
            m_includePileup->setToolTip(serial
                ? QStringLiteral("串口本地能谱固定包含已完成的堆积事件") : QString());
        }
    };
    connect(m_connectionType, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [updateTransport](int) { updateTransport(); });
    updateTransport();

    // Channels
    m_channelDock = new QDockWidget(QStringLiteral("通道设置"), this);
    prepareDock(m_channelDock, QStringLiteral("channelDockV2"));
    auto *channelWidget = new QWidget;
    auto *channelLayout = new QVBoxLayout(channelWidget);
    channelLayout->setContentsMargins(8, 8, 8, 8);
    channelLayout->setSpacing(8);

    auto *baselineBox = new QGroupBox(QStringLiteral("基线与前端（全通道）"));
    auto *baselineLayout = new QGridLayout(baselineBox);
    baselineLayout->setContentsMargins(8, 12, 8, 8);
    baselineLayout->setHorizontalSpacing(6);
    baselineLayout->setVerticalSpacing(4);
    m_baselineTracking = new QCheckBox(QStringLiteral("启用基线跟踪"));
    m_baselineTracking->setObjectName(
        QStringLiteral("baselineTrackingCheckBox"));
    m_baselineTracking->setChecked(false);
    m_baselineTracking->setToolTip(
        QStringLiteral("修改后点击“应用”；关闭时使用手动基线"));
    m_manualBaseline = new QSpinBox;
    m_manualBaseline->setObjectName(QStringLiteral("manualBaselineSpinBox"));
    m_manualBaseline->setRange(0, 16383);
    m_manualBaseline->setValue(8192);
    m_manualBaseline->setPrefix(QStringLiteral("手动 "));
    m_manualBaseline->setEnabled(false);
    m_recoveryTolerance = new QSpinBox;
    m_recoveryTolerance->setObjectName(QStringLiteral("recoveryToleranceSpinBox"));
    m_recoveryTolerance->setRange(1, 16383);
    m_recoveryTolerance->setValue(32);
    m_recoveryTolerance->setPrefix(QStringLiteral("容限 "));
    m_recoveryTolerance->setToolTip(QStringLiteral("恢复容限（LSB）"));
    // The modal-baseline firmware no longer exposes an independent recovery
    // tolerance capability.  Keep the legacy widget only for settings/API
    // compatibility, but do not present it in the live UI.
    m_recoveryTolerance->setVisible(false);
    m_inputRange = new QComboBox;
    m_inputRange->setObjectName(QStringLiteral("inputRangeComboBox"));
    m_inputRange->addItem(QStringLiteral("Q1 / 1 V"), 1);
    m_inputRange->addItem(QStringLiteral("Q2 / 5 V"), 2);
    m_bandwidthLimit = new QCheckBox(QStringLiteral("15 MHz 通带 FIR"));
    m_bandwidthLimit->setObjectName(QStringLiteral("bandwidthLimitCheckBox"));
    m_peakInterpolation = new QCheckBox(QStringLiteral("三点峰值插值"));
    m_peakInterpolation->setObjectName(QStringLiteral("peakInterpolationCheckBox"));
    m_peakInterpolation->setToolTip(QStringLiteral(
        "对局部峰值使用三点抛物线插值，仅改善峰值幅度，时间戳保持不变"));
    m_frontendStatus = new QLabel(QStringLiteral(
        "输入状态　前端尚未确认\n"
        "处理状态　FIR / 峰值插值：等待 FPGA 回报\n"
        "配置状态　等待设备确认"));
    m_frontendStatus->setObjectName(QStringLiteral("frontendStatusLabel"));
    m_frontendStatus->setToolTip(QStringLiteral(
        "显示 FPGA 实际已应用的量程、滤波和初始化状态"));
    m_applyBaseline = new QPushButton(QStringLiteral("应用"));
    m_applyBaseline->setObjectName(QStringLiteral("baselineApplyButton"));
    m_frontendStatus->setSizePolicy(QSizePolicy::Expanding,
                                    QSizePolicy::Preferred);
    m_frontendStatus->setMinimumWidth(0);
    m_frontendStatus->setWordWrap(true);
    m_frontendStatus->setMinimumHeight(72);
    m_frontendStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    baselineLayout->addWidget(m_baselineTracking, 0, 0);
    baselineLayout->addWidget(m_applyBaseline, 0, 1);
    baselineLayout->addWidget(m_manualBaseline, 1, 0);
    baselineLayout->addWidget(m_inputRange, 1, 1);
    baselineLayout->addWidget(m_bandwidthLimit, 2, 0);
    baselineLayout->addWidget(m_peakInterpolation, 2, 1);
    baselineLayout->addWidget(m_frontendStatus, 3, 0, 1, 2);
    baselineLayout->addWidget(m_recoveryTolerance, 4, 0);
    baselineLayout->setColumnStretch(0, 1);
    baselineLayout->setColumnStretch(1, 1);
    channelLayout->addWidget(baselineBox);

    auto *globalBox = new QGroupBox(QStringLiteral("统一阈值"));
    globalBox->setObjectName(QStringLiteral("globalThresholdBox"));
    auto *globalLayout = new QGridLayout(globalBox);
    m_globalThreshold = new QSpinBox;
    m_globalThreshold->setObjectName(QStringLiteral("globalThresholdSpinBox"));
    m_globalThreshold->setRange(1, 4095);
    m_globalThreshold->setValue(64);
    m_globalThreshold->setSizePolicy(QSizePolicy::Expanding,
                                     QSizePolicy::Fixed);
    auto *applyAll = new QPushButton(QStringLiteral("应用全部"));
    applyAll->setObjectName(QStringLiteral("applyGlobalThresholdButton"));
    applyAll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    globalLayout->addWidget(new QLabel(QStringLiteral("阈值 (LSB)")), 0, 0);
    globalLayout->addWidget(m_globalThreshold, 0, 1);
    globalLayout->addWidget(applyAll, 1, 0, 1, 2);
    globalLayout->setColumnStretch(1, 1);
    channelLayout->addWidget(globalBox);

    auto *channelTable = new ChannelTableWidget(0, 8);
    m_channels = channelTable;
    m_channels->setObjectName(QStringLiteral("channelTable"));
    m_channels->setMinimumWidth(0);
    m_channels->setSizePolicy(QSizePolicy::Ignored,
                              QSizePolicy::Expanding);
    m_channels->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_channels->setHorizontalHeaderLabels({
        QStringLiteral("启用"), QStringLiteral("通道"),
        QStringLiteral("阈值 / 发送"), QString(),
        QStringLiteral("极性"), QString(),
        QStringLiteral("基线 / 状态"), QString()
    });
    auto *channelHeader = m_channels->horizontalHeader();
    channelHeader->setMinimumSectionSize(24);
    channelHeader->setSectionResizeMode(QHeaderView::Fixed);
    channelHeader->setStretchLastSection(false);
    // Keep the polarity selector wide enough for both Chinese labels and the
    // combo-box arrow while reclaiming a few pixels from the surrounding
    // columns so the left dock can remain compact.
    channelHeader->resizeSection(0, 36);
    channelHeader->resizeSection(1, 56);
    channelHeader->resizeSection(2, 94);
    channelHeader->resizeSection(4, 64);
    channelHeader->resizeSection(6, 90);
    m_channels->setColumnHidden(3, true);
    m_channels->setColumnHidden(5, true);
    m_channels->setColumnHidden(7, true);
    m_channels->verticalHeader()->setDefaultSectionSize(68);
    m_channels->verticalHeader()->setMinimumSectionSize(68);
    m_channels->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_channels->setAlternatingRowColors(true);
    m_channels->setShowGrid(false);
    ensureChannelRows(8);
    m_channels->selectRow(m_displayedChannelRow);
    channelLayout->addWidget(m_channels, 1);
    m_channelDock->setWidget(channelWidget);
    m_channelDock->setMinimumWidth(345);
    channelWidget->setMinimumWidth(0);
    channelWidget->setSizePolicy(QSizePolicy::Ignored,
                                 QSizePolicy::Expanding);
    connect(m_baselineTracking, &QCheckBox::toggled,
            this, [this](bool enabled) {
                m_baselineTrackingDirty =
                    enabled != m_confirmedBaselineTracking;
                m_baselineTelemetrySyncPending = m_baselineTrackingDirty;
                m_baselineTelemetryTarget = enabled;
                m_manualBaseline->setEnabled(
                    !enabled && m_deviceCapabilities.supportsManualBaseline());
            });
    connect(m_bandwidthLimit, &QCheckBox::toggled,
            this, [this](bool enabled) {
                m_frontendTelemetryBandwidthTarget = enabled;
                m_frontendTelemetrySyncPending = true;
            });
    connect(m_peakInterpolation, &QCheckBox::toggled,
            this, [this](bool enabled) {
                m_frontendTelemetryPeakInterpolationTarget = enabled;
                m_frontendTelemetrySyncPending = true;
            });
    connect(m_manualBaseline,
            qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int) {
                m_manualBaselineDirty = true;
                m_manualBaselineTelemetrySyncPending = false;
            });
    connect(m_applyBaseline, &QPushButton::clicked,
            this, &MainWindow::applyBaselineTracking);
    connect(applyAll, &QPushButton::clicked,
            this, &MainWindow::applyGlobalThreshold);
    connect(m_channels, &QTableWidget::cellChanged,
            this, &MainWindow::applyChannelConfig);
    connect(m_channels, &QTableWidget::cellClicked,
            this, [this](int row, int) { activateChannelRow(row); });
    channelTable->activateCurrentRow =
        [this](int row) { activateChannelRow(row); };

    // Acquisition parameters
    m_acquisitionDock = new QDockWidget(QStringLiteral("采集参数"), this);
    prepareDock(m_acquisitionDock, QStringLiteral("acquisitionDockV2"));
    auto *acquisitionWidget = new QWidget;
    auto *acquisitionLayout = new QVBoxLayout(acquisitionWidget);
    acquisitionLayout->setContentsMargins(8, 8, 8, 8);
    acquisitionLayout->setSpacing(8);
    m_presetWidget = new AcquisitionPresetWidget;
    acquisitionLayout->addWidget(m_presetWidget);

    m_externalDurationUs = new QSpinBox;
    m_externalDurationUs->setObjectName(QStringLiteral("externalDurationUsSpinBox"));
    m_externalDurationUs->setRange(1, 1'000'000);
    m_externalDurationUs->setSingleStep(1);
    m_externalDurationUs->setValue(1000);
    m_externalDurationUs->setSuffix(QStringLiteral(" µs"));
    m_externalDurationUs->setToolTip(QStringLiteral(
        "每次EXTRI上升沿开启的采集窗口，范围1 µs～1 s"));
    auto *externalDurationRow = new QFormLayout;
    externalDurationRow->addRow(QStringLiteral("外触发采集时间"),
                                m_externalDurationUs);
    acquisitionLayout->addLayout(externalDurationRow);

    m_includePileup = new QCheckBox(QStringLiteral("堆积事件计入能谱"));
    m_includePileup->setChecked(true);
    m_includePileup->setToolTip(QStringLiteral(
        "取消后，pileup 事件仍写入事件 TXT，但不填充峰值与积分能谱"));
    acquisitionLayout->addWidget(m_includePileup);

    auto *textBox = new QGroupBox(QStringLiteral("单通道事件 TXT"));
    auto *textLayout = new QGridLayout(textBox);
    m_saveEventText = new QCheckBox(QStringLiteral("保存事件 TXT"));
    m_eventTextPath = new QLineEdit;
    m_eventTextPath->setReadOnly(true);
    m_eventTextPath->setPlaceholderText(QStringLiteral("开始采集前选择文件"));
    m_chooseEventText = new QPushButton(QStringLiteral("选择…"));
    textLayout->addWidget(m_saveEventText, 0, 0, 1, 2);
    textLayout->addWidget(m_eventTextPath, 1, 0);
    textLayout->addWidget(m_chooseEventText, 1, 1);
    acquisitionLayout->addWidget(textBox);

    auto *integrationBox = new QGroupBox(QStringLiteral("积分窗口（全通道）"));
    auto *integrationForm = new QFormLayout(integrationBox);
    m_integrationPostUs = new QDoubleSpinBox;
    m_integrationPostUs->setDecimals(4);
    m_integrationPostUs->setRange(0.0480, 20.0000);
    m_integrationPostUs->setSingleStep(0.0080);
    m_integrationPostUs->setValue(0.2560);
    m_integrationPostUs->setSuffix(QStringLiteral(" μs"));
    m_integrationPreUs = new QDoubleSpinBox;
    m_integrationPreUs->setDecimals(4);
    m_integrationPreUs->setRange(0.0, 1.0000);
    m_integrationPreUs->setSingleStep(0.0080);
    m_integrationPreUs->setValue(0.0000);
    m_integrationPreUs->setSuffix(QStringLiteral(" μs"));
    m_integrationSummary = new QLabel;
    integrationForm->addRow(QStringLiteral("总窗口"), m_integrationPostUs);
    integrationForm->addRow(QStringLiteral("前置窗口"), m_integrationPreUs);
    integrationForm->addRow(m_integrationSummary);
    acquisitionLayout->addWidget(integrationBox);
    acquisitionLayout->addStretch();
    m_acquisitionDock->setWidget(acquisitionWidget);
    connect(m_chooseEventText, &QPushButton::clicked,
            this, &MainWindow::chooseEventTextFile);
    connect(m_integrationPostUs, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this] { updateIntegrationWindowSummary(); });
    connect(m_integrationPreUs, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this] { updateIntegrationWindowSummary(); });
    updateIntegrationWindowSummary();

    // Legacy SiPM/coincidence editors are intentionally not created for the
    // single-channel host. Their packet/list decoders remain available for
    // read-only replay.
#if 0
    // SiPM modules
    m_moduleDock = new QDockWidget(QStringLiteral("SiPM 四通道模块"), this);
    prepareDock(m_moduleDock, QStringLiteral("moduleDockV2"));
    m_modules = new QTableWidget(0, 7);
    m_modules->setHorizontalHeaderLabels({
        QStringLiteral("启用"), QStringLiteral("模块"),
        QStringLiteral("窗口/采样"), QStringLiteral("能量门"),
        QStringLiteral("最小能量"), QStringLiteral("最大能量"),
        QStringLiteral("质量掩码")
    });
    m_modules->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_modules->setAlternatingRowColors(true);
    m_modules->setShowGrid(false);
    ensureModuleRows(2);
    connect(m_modules, &QTableWidget::cellChanged,
            this, [this](int row, int) { applyModuleConfig(row); });
    m_moduleDock->setWidget(m_modules);

    // Coincidence pairs
    m_pairDock = new QDockWidget(QStringLiteral("散射/吸收配对"), this);
    prepareDock(m_pairDock, QStringLiteral("pairDockV2"));
    m_pairs = new QTableWidget(0, 13);
    m_pairs->setHorizontalHeaderLabels({
        QStringLiteral("启用"), QStringLiteral("配对"),
        QStringLiteral("散射"), QStringLiteral("吸收"),
        QStringLiteral("ΔT最小"), QStringLiteral("ΔT最大"),
        QStringLiteral("散射门"), QStringLiteral("散射E最小"),
        QStringLiteral("散射E最大"), QStringLiteral("吸收门"),
        QStringLiteral("吸收E最小"), QStringLiteral("吸收E最大"),
        QStringLiteral("质量掩码")
    });
    m_pairs->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_pairs->setAlternatingRowColors(true);
    m_pairs->setShowGrid(false);
    ensurePairRows(1);
    connect(m_pairs, &QTableWidget::cellChanged,
            this, [this](int row, int) { applyPairConfig(row); });
    m_pairDock->setWidget(m_pairs);
#endif

    // ROI and fit analysis
    m_analysisDock = new QDockWidget(QStringLiteral("ROI 与峰分析"), this);
    m_analysisDock->setObjectName(QStringLiteral("analysisDockV2"));
    m_analysisDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    auto *analysisWidget = new QWidget;
    auto *analysisLayout = new QVBoxLayout(analysisWidget);
    analysisLayout->setContentsMargins(8, 8, 8, 8);
    analysisLayout->setSpacing(8);
    if (m_statusPanel)
        analysisLayout->addWidget(m_statusPanel);
    auto *roiBox = new QGroupBox(QStringLiteral("选定区域"));
    roiBox->setObjectName(QStringLiteral("roiEntryGroup"));
    auto *roiForm = new QFormLayout(roiBox);
    m_roiLeft = new QSpinBox;
    m_roiRight = new QSpinBox;
    m_roiLeft->setRange(0, DefaultIntegralBins - 1);
    m_roiRight->setRange(0, DefaultIntegralBins - 1);
    m_roiLeft->setValue(400);
    m_roiRight->setValue(650);
    auto *addRoiBtn = new QPushButton(QStringLiteral("添加 ROI"));
    roiForm->addRow(QStringLiteral("左边界"), m_roiLeft);
    roiForm->addRow(QStringLiteral("右边界"), m_roiRight);
    roiForm->addRow(addRoiBtn);
    analysisLayout->addWidget(roiBox);
    m_roiTable = new QTableWidget(0, 3);
    m_roiTable->setHorizontalHeaderLabels({
        QStringLiteral("名称"), QStringLiteral("左"), QStringLiteral("右")
    });
    m_roiTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    analysisLayout->addWidget(m_roiTable);
    m_roiInfo = new QLabel(QStringLiteral("请选择 ROI"));
    m_roiInfo->setWordWrap(true);
    analysisLayout->addWidget(m_roiInfo);
    auto *fitBox = new QGroupBox(QStringLiteral("峰拟合"));
    auto *fitForm = new QFormLayout(fitBox);
    m_peakCount = new QSpinBox;
    m_peakCount->setRange(1, 4);
    m_background = new QComboBox;
    m_background->addItems({QStringLiteral("线性背景"),
                            QStringLiteral("常数背景")});
    auto *fitBtn = new QPushButton(QStringLiteral("拟合"));
    auto *clearFitBtn = new QPushButton(QStringLiteral("清除拟合"));
    m_fitInfo = new QLabel(QStringLiteral("未拟合"));
    m_fitInfo->setWordWrap(true);
    fitForm->addRow(QStringLiteral("峰数量"), m_peakCount);
    fitForm->addRow(QStringLiteral("背景"), m_background);
    fitForm->addRow(fitBtn);
    fitForm->addRow(clearFitBtn);
    fitForm->addRow(m_fitInfo);
    analysisLayout->addWidget(fitBox);
    m_analysisDock->setWidget(analysisWidget);
    addDockWidget(Qt::RightDockWidgetArea, m_analysisDock);
    connect(addRoiBtn, &QPushButton::clicked, this, &MainWindow::addRoi);
    connect(fitBtn, &QPushButton::clicked, this, &MainWindow::fitRoi);
    connect(clearFitBtn, &QPushButton::clicked, this, &MainWindow::clearFit);
    connect(m_roiTable, &QTableWidget::itemSelectionChanged,
            this, &MainWindow::refreshRoiInfo);
    connect(m_roiTable, &QTableWidget::cellChanged,
            this, &MainWindow::refreshRoiTable);

    tabifyDockWidget(m_connectionDock, m_channelDock);
    tabifyDockWidget(m_channelDock, m_acquisitionDock);
    m_channelDock->raise();
    if (m_moduleDock)
        m_moduleDock->hide();
    if (m_pairDock)
        m_pairDock->hide();
}

void MainWindow::buildUi() {
    m_center = new QStackedWidget;
    m_spectrum = new SpectrumView;
    m_spectrum->setStableYEnabled(true);
    m_center->addWidget(m_spectrum);

    auto *gridHost = new QWidget;
    auto *grid = new QGridLayout(gridHost);
    grid->setContentsMargins(1, 1, 1, 1);
    grid->setSpacing(2);
    for (int i = 0; i < 8; ++i) {
        auto *view = new SpectrumView;
        view->setChannels({i});
        view->setCursorEnabled(false);
        view->setOverviewVisible(false);
        view->setStableYEnabled(true);
        m_gridViews.push_back(view);
        grid->addWidget(view, i / 2, i % 2);
    }
    m_center->addWidget(gridHost);
    m_eventMap = new EventMapView;
    m_center->addWidget(m_eventMap);
    // Independent waveform page.  It has its own scope-style display and does
    // not share the ROI, logarithmic-axis or energy-calibration logic.
    m_waveformPage = new WaveformPage;
    m_waveformPageIndex = m_center->addWidget(m_waveformPage);
    m_psdPage=new PsdPage; m_psdPageIndex=m_center->addWidget(m_psdPage);

    setCentralWidget(m_center);

    // View mode combo (in toolbar area)
    m_kind = new QComboBox;
    m_kind->addItems({QStringLiteral("峰值谱 8192 道"), QStringLiteral("积分谱 16384 道")});
    m_layout = new QComboBox;
    m_layout->addItems({QStringLiteral("单通道"), QStringLiteral("选中通道叠加"), QStringLiteral("8 通道宫格")});
    auto *viewTb = addToolBar(QStringLiteral("视图"));
    viewTb->setObjectName(QStringLiteral("viewToolBar"));
    viewTb->addWidget(new QLabel(QStringLiteral(" 谱类型 ")));
    viewTb->addWidget(m_kind);
    viewTb->addWidget(new QLabel(QStringLiteral(" 布局 ")));
    viewTb->addWidget(m_layout);
}

// ═══════════════════════════════════════════════════════════════════════════
// Signal connections
// ═══════════════════════════════════════════════════════════════════════════

void MainWindow::connectSignals() {
    // ── Acquisition controller ──
    connect(&m_acq, &AcquisitionController::spectrumReady, this, &MainWindow::updateSpectrum);
    connect(&m_acq, &AcquisitionController::statusReady, this, &MainWindow::updateStatus);
    connect(&m_acq, &AcquisitionController::baselineReady, this, &MainWindow::updateBaselineStatus);
    connect(&m_acq, &AcquisitionController::capabilitiesReady,
            this, &MainWindow::updateCapabilities);
    connect(&m_acq, &AcquisitionController::deviceStatusReady,
            this, &MainWindow::updateDeviceStatus);
    connect(&m_acq, &AcquisitionController::controlBusyChanged,
            this, [this](bool busy) {
                m_controlBusy = busy;
                m_applyBaseline->setText(
                    busy ? QStringLiteral("应用中…") : QStringLiteral("应用"));
                if (m_waveformPage)
                    m_waveformPage->setBusy(busy);
                if (busy && !m_hardwareResponsive) {
                    m_statusDevice->setText(QStringLiteral("设备 通信中"));
                    m_statusDevice->setProperty(
                        "state", QStringLiteral("connecting"));
                    m_statusDevice->style()->unpolish(m_statusDevice);
                    m_statusDevice->style()->polish(m_statusDevice);
                }
                updateActionsForState();
            });
    connect(&m_acq,
            &AcquisitionController::baselineTrackingApplyFinished,
            this, [this](bool enabled, bool success, const QString &message) {
                if (success) {
                    m_confirmedBaselineTracking = enabled;
                    m_manualBaselineDirty = false;
                    m_manualBaselineTelemetrySyncPending = true;
                    m_baselineTelemetrySyncPending = true;
                    m_baselineTelemetryTarget = enabled;
                } else {
                    m_manualBaselineTelemetrySyncPending = false;
                    m_baselineTelemetrySyncPending = false;
                    m_frontendTelemetrySyncPending = false;
                    {
                        const QSignalBlocker baselineBlocker(m_baselineTracking);
                        const QSignalBlocker manualBlocker(m_manualBaseline);
                        const QSignalBlocker recoveryBlocker(m_recoveryTolerance);
                        const QSignalBlocker rangeBlocker(m_inputRange);
                        const QSignalBlocker bandwidthBlocker(m_bandwidthLimit);
                        const QSignalBlocker interpolationBlocker(m_peakInterpolation);
                        m_baselineTracking->setChecked(m_confirmedBaselineTracking);
                        m_manualBaseline->setValue(m_confirmedManualBaseline);
                        m_recoveryTolerance->setValue(m_confirmedRecoveryTolerance);
                        const int rangeIndex = m_inputRange->findData(
                            m_confirmedInputRange);
                        if (rangeIndex >= 0)
                            m_inputRange->setCurrentIndex(rangeIndex);
                        m_bandwidthLimit->setChecked(m_confirmedBandwidthEnabled);
                        m_peakInterpolation->setChecked(m_confirmedPeakInterpolationEnabled);
                    }
                }
                {
                    const QSignalBlocker blocker(m_baselineTracking);
                    m_baselineTracking->setChecked(
                        success ? enabled : m_confirmedBaselineTracking);
                }
                m_baselineTrackingDirty = false;
                updateActionsForState();
                statusBar()->showMessage(message, success ? 4000 : 8000);
            });
    connect(&m_acq, &AcquisitionController::hardwareCommunicationChanged,
            this, [this](bool responsive) {
                m_hardwareResponsive = responsive;
                m_statusDevice->setText(
                    responsive ? QStringLiteral("设备 已响应")
                               : (m_controlBusy
                                      ? QStringLiteral("设备 通信中")
                                      : QStringLiteral("设备 未连接")));
                m_statusDevice->setProperty(
                    "state", responsive ? QStringLiteral("online")
                                         : (m_controlBusy
                                                ? QStringLiteral("connecting")
                                                : QStringLiteral("offline")));
                m_statusDevice->setToolTip(
                    responsive ? QStringLiteral("FPGA 控制通道应答正常")
                               : QStringLiteral("尚未收到 FPGA 控制应答"));
                m_statusDevice->style()->unpolish(m_statusDevice);
                m_statusDevice->style()->polish(m_statusDevice);
            });
    connect(&m_acq,&AcquisitionController::psdEventsReady,m_psdPage->session(),&PsdSession::addEvents,Qt::DirectConnection);
    connect(m_psdPage,&PsdPage::errorOccurred,this,[this](const QString &error){statusBar()->showMessage(error,10000);m_acq.stop();});
    connect(&m_acq, &AcquisitionController::errorOccurred, this, [this](const QString &msg) {
        statusBar()->showMessage(msg, 8000);
    });
    connect(m_includePileup, &QCheckBox::toggled,
            &m_acq, &AcquisitionController::setIncludePileupInSpectrum);
    connect(m_acquisitionMode, &QComboBox::currentIndexChanged,
            this, [this](int index) {
                const auto mode = AcquisitionMode(
                    m_acquisitionMode->itemData(index).toInt());
                // The waveform mode is a separate page, not another spectrum
                // layout, so selecting it must also switch the centre view.
                if(mode==AcquisitionMode::Psd) {
                    m_peakInterpolation->setChecked(false);m_bandwidthLimit->setChecked(false);
                }
                if (mode == AcquisitionMode::Waveform) {
                    m_acq.setAcquisitionMode(mode);
                    if (m_waveformPageIndex >= 0)
                        m_center->setCurrentIndex(m_waveformPageIndex);
                } else {
                    m_acq.setAcquisitionMode(mode);
                }
                updateModeDependentPanels();
                updateEventTextAvailability();
                changeView();
            });
    connect(m_saveEventText, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked && m_eventTextPath->text().isEmpty())
            chooseEventTextFile();
    });

    // ── File actions ──
    connect(m_actOpen, &QAction::triggered, this, &MainWindow::openSpectrum);
    connect(m_actSave, &QAction::triggered, this, &MainWindow::saveSpectrum);
    connect(m_actOpenList, &QAction::triggered, this, &MainWindow::openList);
    connect(m_actSetEventText, &QAction::triggered,
            this, &MainWindow::chooseEventTextFile);
    connect(m_actExit, &QAction::triggered, this, &QWidget::close);

    // ── Acquisition actions ──
    connect(m_actStart, &QAction::triggered, this, &MainWindow::startAcquisition);
    connect(m_actPause, &QAction::triggered, this, &MainWindow::togglePause);
    connect(m_actStop, &QAction::triggered, this, &MainWindow::stopAcquisition);
    connect(m_actClear, &QAction::triggered, this, &MainWindow::clearSpectrum);
    connect(m_actConnect, &QAction::triggered, this, [this] {
        m_connectionDock->show();
        m_connectionDock->raise();
        auto *ip = findChild<QLineEdit *>("deviceIp");
        if (ip) ip->setFocus();
    });

    // ── Display actions ──
    connect(m_actLinearY, &QAction::triggered, this, [this] { applyDisplaySettings(); });
    connect(m_actLogY, &QAction::triggered, this, [this] { applyDisplaySettings(); });
    connect(m_actFill, &QAction::toggled, this, [this](bool on) {
        m_spectrum->setFillMode(on);
        for (auto *v : m_gridViews) v->setFillMode(on);
    });
    connect(m_actOverview, &QAction::toggled, this, [this](bool on) {
        m_spectrum->setOverviewVisible(on);
    });
    connect(m_actLegend, &QAction::toggled, this, [this](bool on) {
        m_spectrum->setLegendVisible(on);
        for (auto *v : m_gridViews) v->setLegendVisible(on);
    });
    connect(m_actGrid, &QAction::toggled, this, [this](bool on) {
        m_spectrum->setGridVisible(on);
        for (auto *v : m_gridViews) v->setGridVisible(on);
    });
    connect(m_actRoiLabels, &QAction::toggled,
            m_spectrum, &SpectrumView::setRoiLabelsVisible);
    connect(m_actFullRange, &QAction::triggered, m_spectrum, &SpectrumView::resetZoom);
    connect(m_actZoomIn, &QAction::triggered,
            this, [this] { m_spectrum->zoomBy(0.5); });
    connect(m_actZoomOut, &QAction::triggered,
            this, [this] { m_spectrum->zoomBy(2.0); });
    connect(m_actSingleCh, &QAction::triggered, this, [this] { m_layout->setCurrentIndex(0); });
    connect(m_actMultiCh, &QAction::triggered, this, [this] { m_layout->setCurrentIndex(1); });
    connect(m_actGrid8, &QAction::triggered, this, [this] { m_layout->setCurrentIndex(2); });

    // ── Interaction mode actions ──
    connect(m_actCursorMode, &QAction::triggered, this, [this] { m_spectrum->setInteractionMode(SpectrumInteractionMode::Cursor); });
    connect(m_actZoomMode, &QAction::triggered, this, [this] { m_spectrum->setInteractionMode(SpectrumInteractionMode::ZoomRectangle); });
    connect(m_actPanMode, &QAction::triggered, this, [this] { m_spectrum->setInteractionMode(SpectrumInteractionMode::Pan); });
    connect(m_actCreateRoiMode, &QAction::triggered, this, [this] { m_spectrum->setInteractionMode(SpectrumInteractionMode::CreateRoi); });
    connect(m_actEditRoiMode, &QAction::triggered, this, [this] { m_spectrum->setInteractionMode(SpectrumInteractionMode::EditRoi); });

    // ── ROI actions ──
    connect(m_actNewRoi, &QAction::triggered, this, [this] {
        m_spectrum->setInteractionMode(SpectrumInteractionMode::CreateRoi);
        m_actCreateRoiMode->setChecked(true);
    });
    connect(m_actDeleteRoi, &QAction::triggered, this, &MainWindow::deleteSelectedRoi);
    connect(m_actClearRois, &QAction::triggered, this, &MainWindow::clearAllRois);
    connect(m_actPrevRoi, &QAction::triggered, this, [this] {
        int idx = m_spectrum->selectedRoi();
        if (idx > 0) { m_spectrum->setSelectedRoi(idx - 1); refreshRoiInfo(); }
    });
    connect(m_actNextRoi, &QAction::triggered, this, [this] {
        int idx = m_spectrum->selectedRoi();
        if (idx < m_spectrum->rois().size() - 1) { m_spectrum->setSelectedRoi(idx + 1); refreshRoiInfo(); }
    });

    // ── Analysis actions ──
    connect(m_actSingleFit, &QAction::triggered, this, &MainWindow::fitCurrentRoi);
    connect(m_actClearFit, &QAction::triggered, this, &MainWindow::clearFit);

    // ── Calibration actions ──
    connect(m_actEnergyCal, &QAction::triggered, this, &MainWindow::calibrate);
    connect(m_actDefaultLayout, &QAction::triggered,
            this, &MainWindow::restoreDefaultLayout);
    connect(m_actAbout, &QAction::triggered, this, [this] {
        QMessageBox::about(this, QStringLiteral("关于 MCA Studio"),
            QStringLiteral("MCA Studio 0.2\n"
                           "ADC0 八通道多道分析器上位机\n"
                           "支持 UDP 采集、事件 TXT、ROI、拟合与标定。"));
    });

    // ── SpectrumView signals ──
    connect(m_spectrum, &SpectrumView::cursorMoved, this, [this](double ch, quint64 count, double energy) {
        QString text = QStringLiteral("Ch: %1  Cnt: %2").arg(qRound(ch)).arg(count);
        if (energy > 0) text += QStringLiteral("  E: %1 keV").arg(energy, 0, 'f', 2);
        m_statusCursor->setText(text);
    });
    connect(m_spectrum, &SpectrumView::cursorLeft, this, [this] { m_statusCursor->clear(); });
    connect(m_spectrum, &SpectrumView::roiCreated, this, &MainWindow::addRoiFromGraph);
    connect(m_spectrum, &SpectrumView::roiModified, this, &MainWindow::modifyRoi);
    connect(m_spectrum, &SpectrumView::roiSelected,
            this, &MainWindow::selectRoi);
    connect(m_spectrum, &SpectrumView::roiFitRequested, this, [this](int) { fitCurrentRoi(); });
    connect(m_spectrum, &SpectrumView::roiDeleteRequested, this, [this](int idx) {
        if (idx >= 0 && idx < m_spectrum->rois().size()) {
            m_spectrum->setSelectedRoi(idx);
            deleteSelectedRoi();
        }
    });

    // ── View controls ──
    connect(m_kind, &QComboBox::currentIndexChanged, this, &MainWindow::changeView);
    connect(m_layout, &QComboBox::currentIndexChanged, this, &MainWindow::changeView);

    // ── State change → update actions ──
    connect(m_spectrum, &SpectrumView::interactionModeChanged, this, [this](SpectrumInteractionMode) {
        updateActionsForState();
    });

    // ── Waveform page ──
    connect(m_waveformPage, &WaveformPage::startRequested,
            this, &MainWindow::startWaveform);
    connect(m_waveformPage, &WaveformPage::stopRequested,
            this, &MainWindow::stopAcquisition);
    connect(m_waveformPage, &WaveformPage::configChanged,
            this, &MainWindow::applyWaveformConfig);
    connect(m_waveformPage, &WaveformPage::triggerModeChanged,
            this, [this](WaveformTriggerMode mode) {
                m_acq.setWaveformTriggerMode(mode);
            });
    connect(m_waveformPage, &WaveformPage::exportRequested,
            this, &MainWindow::exportWaveformCsv);
    connect(&m_acq, &AcquisitionController::waveformReady,
            this, [this](std::shared_ptr<const WaveformCapture> capture) {
                if (!capture)
                    return;
                // Continuous acquisition keeps only the newest complete
                // record; the worker already throttles how often it publishes.
                m_waveformPage->setCapture(std::move(capture));
                m_waveformPage->setRunning(true);
                updateActionsForState();
            });
    connect(&m_acq, &AcquisitionController::waveformStatusReady,
            this, &MainWindow::updateWaveformStatus);

    updateEventTextAvailability();
    loadRoiContext(0, SpectrumKind::Peak);
}

// ═══════════════════════════════════════════════════════════════════════════
// Action state updates
// ═══════════════════════════════════════════════════════════════════════════

void MainWindow::updateActionsForState() {
    const bool starting = m_acquisitionState == AcquisitionState::Starting;
    const bool running = m_acquisitionState == AcquisitionState::Running;
    const bool paused = m_acquisitionState == AcquisitionState::Paused;
    const bool idle = m_acquisitionState == AcquisitionState::Idle ||
                      m_acquisitionState == AcquisitionState::Error;

    m_actStart->setEnabled(idle && !m_controlBusy);
    m_actPause->setEnabled((running || paused) && !m_controlBusy);
    m_actStop->setEnabled(starting || running || paused);
    m_actClear->setEnabled(idle && !m_controlBusy);
    m_actOpen->setEnabled(idle && !m_controlBusy);
    m_actOpenList->setEnabled(idle && !m_controlBusy);
    if (m_recentFilesMenu)
        m_recentFilesMenu->setEnabled(idle);
    m_actSave->setEnabled(bool(m_snapshot));

    // Keep dock parents enabled while a control transaction is in flight.
    // Toggling a whole dock steals focus from its spin-box line edits and
    // causes visible flicker during the stop/commit handshake.  The stable
    // acquisition state remains the editing boundary; commit buttons below
    // still reflect m_controlBusy.
    m_connectionDock->widget()->setEnabled(idle);
    m_channelDock->widget()->setEnabled(true);
    m_acquisitionDock->widget()->setEnabled(true);
    m_presetWidget->setEditingEnabled(idle);
    if (m_externalDurationUs)
        m_externalDurationUs->setEnabled(idle &&
                                         m_acquisitionMode->currentData().toInt() ==
                                             int(AcquisitionMode::ExternalTrigger));
    const bool serialTransport = m_connectionType &&
                                 m_connectionType->currentData().toInt() == 1;
    m_includePileup->setEnabled(idle && !serialTransport);
    m_integrationPostUs->setEnabled(idle && m_acquisitionMode->currentData().toInt()!=int(AcquisitionMode::Psd));
    m_integrationPreUs->setEnabled(idle && m_acquisitionMode->currentData().toInt()!=int(AcquisitionMode::Psd));
    m_manualBaseline->setEnabled(
        idle &&
        m_deviceCapabilities.supportsManualBaseline() &&
        !m_baselineTracking->isChecked());
    m_baselineTracking->setEnabled(idle);
    m_recoveryTolerance->setEnabled(idle && !m_controlBusy);
    m_inputRange->setEnabled(idle && !m_controlBusy);
    m_bandwidthLimit->setEnabled(idle && !m_controlBusy);
    m_peakInterpolation->setEnabled(
        idle && !m_controlBusy && m_deviceCapabilities.supportsPeakInterpolation() &&
        m_acquisitionMode->currentData().toInt()!=int(AcquisitionMode::Psd));
    m_applyBaseline->setEnabled(idle && !m_controlBusy);
    m_acquisitionMode->setEnabled(idle);
    m_channels->setEnabled(idle);
    m_globalThreshold->setEnabled(idle);
    if (m_modules)
        m_modules->setEnabled(idle);
    if (m_pairs)
        m_pairs->setEnabled(idle);
    m_saveEventText->setEnabled(idle && !serialTransport &&
        (m_acquisitionMode->currentData().toInt() ==
             int(AcquisitionMode::AutomaticTrigger) ||
         m_acquisitionMode->currentData().toInt() ==
             int(AcquisitionMode::ExternalTrigger)));
    m_chooseEventText->setEnabled(m_saveEventText->isEnabled());
    m_actSetEventText->setEnabled(m_chooseEventText->isEnabled());
    m_layout->setEnabled(m_acquisitionMode->currentData().toInt() ==
                         int(AcquisitionMode::AutomaticTrigger) ||
                         m_acquisitionMode->currentData().toInt() ==
                         int(AcquisitionMode::ExternalTrigger));

    // Update interaction mode check states
    const auto mode = m_spectrum->interactionMode();
    m_actCursorMode->setChecked(mode == SpectrumInteractionMode::Cursor);
    m_actZoomMode->setChecked(mode == SpectrumInteractionMode::ZoomRectangle);
    m_actPanMode->setChecked(mode == SpectrumInteractionMode::Pan);
    m_actCreateRoiMode->setChecked(mode == SpectrumInteractionMode::CreateRoi);
    m_actEditRoiMode->setChecked(mode == SpectrumInteractionMode::EditRoi);
}

// ═══════════════════════════════════════════════════════════════════════════
// Acquisition slots
// ═══════════════════════════════════════════════════════════════════════════

void MainWindow::startAcquisition() {
    if (m_acquisitionMode &&
        m_acquisitionMode->currentData().toInt() ==
            int(AcquisitionMode::Waveform)) {
        startWaveform(m_waveformPage->triggerMode(),
                      m_waveformPage->channelConfig());
        return;
    }
    startUdp();
}

void MainWindow::startUdp() {
    const bool psd=m_acquisitionMode->currentData().toInt()==int(AcquisitionMode::Psd);
    if(psd) {
        QString error;if(!m_psdPage->validate(&error)){QMessageBox::warning(this,QStringLiteral("PSD 窗口错误"),error);return;}
        if(m_connectionType->currentData().toInt()==1){QMessageBox::warning(this,QStringLiteral("PSD"),QStringLiteral("PSD 仅支持 UDP"));return;}
        if(m_deviceCapabilities.formatFlagsValid && !m_deviceCapabilities.supportsPsd()) {
            QMessageBox::warning(this,QStringLiteral("PSD"),QStringLiteral("当前固件不支持 PSD"));return;
        }
    }
    auto *ip = findChild<QLineEdit *>("deviceIp");
    auto *dataPort = findChild<QSpinBox *>("dataPort");
    auto *controlPort = findChild<QSpinBox *>("controlPort");
    if (!ip || !dataPort || !controlPort)
        return;

    commitIntegrationWindowEditor();

    const bool serial = m_connectionType &&
                        m_connectionType->currentData().toInt() == 1;
    QHostAddress address;
    if (!serial && (!address.setAddress(ip->text().trimmed()) ||
        address.protocol() != QAbstractSocket::IPv4Protocol)) {
        QMessageBox::warning(this, QStringLiteral("连接参数错误"),
                             QStringLiteral("请输入有效的 FPGA IPv4 地址。"));
        m_connectionDock->show();
        m_connectionDock->raise();
        ip->setFocus();
        return;
    }

    QString validationError;
    if (!validateAcquisitionConfiguration(&validationError)) {
        QMessageBox::warning(this, QStringLiteral("采集配置无效"),
                             validationError);
        return;
    }

    QString path;
    if (!psd && !serial && m_saveEventText->isChecked()) {
        if (m_eventTextPath->text().isEmpty())
            chooseEventTextFile();
        path = m_eventTextPath->text();
        if (path.isEmpty())
            return;
    }
    for (int row = 0; row < m_channels->rowCount(); ++row)
        saveChannelConfig(row);
    if (m_modules)
        for (int row = 0; row < m_modules->rowCount(); ++row)
            m_acq.setModuleConfig(row, moduleConfigFromRow(row));
    if (m_pairs)
        for (int row = 0; row < m_pairs->rowCount(); ++row)
            m_acq.setPairConfig(row, pairConfigFromRow(row, false));

    if(psd) {
        auto cfg=m_psdPage->channelConfig(channelConfigFromRow(0));
        m_acq.setChannelConfig(0,cfg);m_psdPage->begin(cfg,m_bandwidthLimit->isChecked());
    }
    const auto preset = m_presetWidget->preset();
    const auto selectedMode = AcquisitionMode(
        m_acquisitionMode->currentData().toInt());
    m_acq.setAcquisitionMode(selectedMode);
    if (m_externalDurationUs)
        m_acq.setExternalTriggerDurationUs(
            quint32(m_externalDurationUs->value()));
    if (serial) {
        const QString portName = m_serialPort
            ? m_serialPort->currentData().toString() : QString();
        if (portName.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("连接参数错误"),
                                 QStringLiteral("请选择可用串口。"));
            return;
        }
        m_acq.setIncludePileupInSpectrum(true);
        m_acq.startSerial(portName, preset,
                          m_baselineTracking->isChecked(),
                          quint16(m_recoveryTolerance->value()),
                          m_bandwidthLimit->isChecked(),
                          quint8(m_inputRange->currentData().toInt()),
                          m_peakInterpolation->isChecked());
    } else {
        m_acq.startUdp(dataPort->value(), address,
                       controlPort->value(), preset, path,
                       m_baselineTracking->isChecked(),
                       quint16(m_recoveryTolerance->value()),
                       m_bandwidthLimit->isChecked(),
                       quint8(m_inputRange->currentData().toInt()),
                       m_peakInterpolation->isChecked());
    }
}

void MainWindow::chooseEventTextFile() {
    const auto mode = AcquisitionMode(m_acquisitionMode->currentData().toInt());
    if (mode != AcquisitionMode::AutomaticTrigger &&
        mode != AcquisitionMode::ExternalTrigger) {
        statusBar()->showMessage(
            QStringLiteral("事件 TXT 仅支持单通道独立事件模式"), 4000);
        return;
    }
    const QString initial = m_eventTextPath->text().isEmpty()
        ? m_lastDirectory : m_eventTextPath->text();
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("选择事件 TXT 文件"), initial,
        QStringLiteral("事件文本 (*.txt)"));
    if (path.isEmpty()) {
        if (m_eventTextPath->text().isEmpty()) {
            const QSignalBlocker blocker(m_saveEventText);
            m_saveEventText->setChecked(false);
        }
        return;
    }
    if (!path.endsWith(QStringLiteral(".txt"), Qt::CaseInsensitive))
        path += QStringLiteral(".txt");
    m_eventTextPath->setText(path);
    m_lastDirectory = QFileInfo(path).absolutePath();
    {
        const QSignalBlocker blocker(m_saveEventText);
        m_saveEventText->setChecked(true);
    }
    statusBar()->showMessage(
        QStringLiteral("事件将实时保存到 %1").arg(path), 4000);
}

void MainWindow::updateEventTextAvailability() {
    const auto mode = AcquisitionMode(m_acquisitionMode->currentData().toInt());
    const bool single = mode == AcquisitionMode::AutomaticTrigger ||
                        mode == AcquisitionMode::ExternalTrigger;
    const bool editable = m_acquisitionState == AcquisitionState::Idle ||
                          m_acquisitionState == AcquisitionState::Error;
    if (!single && m_saveEventText->isChecked()) {
        const QSignalBlocker blocker(m_saveEventText);
        m_saveEventText->setChecked(false);
    }
    m_saveEventText->setEnabled(single && editable);
    m_chooseEventText->setEnabled(single && editable);
    m_actSetEventText->setEnabled(single && editable);
    const QString tip = single
        ? QStringLiteral("采集时逐事件写入 UTF-8、TAB 分隔的 TXT")
        : QStringLiteral("事件 TXT 仅支持单通道独立事件模式");
    m_saveEventText->setToolTip(tip);
    m_chooseEventText->setToolTip(tip);
}

bool MainWindow::validateAcquisitionConfiguration(QString *error) const {
    const auto mode = AcquisitionMode(m_acquisitionMode->currentData().toInt());
    if (mode == AcquisitionMode::AutomaticTrigger ||
        mode == AcquisitionMode::ExternalTrigger ||
        mode == AcquisitionMode::Waveform || mode==AcquisitionMode::Psd)
        return true;

    // Legacy module/coincidence modes are retained only for read-only file
    // compatibility and are not exposed by the live single-channel UI.
    if (!m_modules || !m_pairs)
        return true;
    quint8 enabledModules = 0;
    for (int row = 0; row < m_modules->rowCount() && row < 8; ++row) {
        if (m_modules->item(row, 0)->checkState() == Qt::Checked)
            enabledModules |= quint8(1u << row);
    }
    if (!enabledModules) {
        if (error)
            *error = QStringLiteral("请先在“SiPM 四通道模块”面板中启用至少一个模块。");
        m_moduleDock->show();
        m_moduleDock->raise();
        return false;
    }
    if (mode == AcquisitionMode::FourChannel)
        return true;

    if (m_pairs->rowCount() < 1 ||
        m_pairs->item(0, 0)->checkState() != Qt::Checked) {
        if (error)
            *error = QStringLiteral("请先在“散射/吸收配对”面板中启用配对 0。");
        m_pairDock->show();
        m_pairDock->raise();
        return false;
    }
    const auto readMask = [this](int column) {
        quint8 mask = 0;
        for (auto *box : m_pairs->cellWidget(0, column)
                              ->findChildren<QCheckBox *>()) {
            if (box->isChecked())
                mask |= quint8(1u << box->property("module").toInt());
        }
        return mask;
    };
    const quint8 scatter = readMask(2);
    const quint8 absorber = readMask(3);
    if (!scatter || !absorber || (scatter & absorber) ||
        (scatter & ~enabledModules) || (absorber & ~enabledModules)) {
        if (error)
            *error = QStringLiteral(
                "散射和吸收掩码必须非空、互不重叠，并且只能引用已启用模块。");
        m_pairDock->show();
        m_pairDock->raise();
        return false;
    }
    return true;
}

void MainWindow::togglePause() {
    if (m_acquisitionState == AcquisitionState::Paused)
        m_acq.resume();
    else if (m_acquisitionState == AcquisitionState::Running)
        m_acq.pause();
}

void MainWindow::stopAcquisition() {
    m_acq.stop();
    m_presetWidget->reset();
}

void MainWindow::clearSpectrum() {
    if(m_psdPage)m_psdPage->clear();
    if (m_acquisitionState == AcquisitionState::Running) return;
    m_acq.clear();
    m_spectrum->resetStableY();
    for (auto *v : m_gridViews) v->resetStableY();
    m_presetWidget->reset();
}

// ═══════════════════════════════════════════════════════════════════════════
// Spectrum display
// ═══════════════════════════════════════════════════════════════════════════

void MainWindow::updateSpectrum(std::shared_ptr<const SpectrumSnapshot> snapshot) {
    const bool unchanged=m_snapshot && snapshot && m_snapshot->version==snapshot->version &&
        m_snapshot->channelCount==snapshot->channelCount && m_snapshot->adcBits==snapshot->adcBits &&
        m_snapshot->peakFractionBits==snapshot->peakFractionBits && m_snapshot->totalEvents==snapshot->totalEvents;
    m_snapshot = std::move(snapshot);
    if (m_snapshot && m_kind && !m_snapshot->peak.isEmpty()) {
        const int peakBins = m_snapshot->peak.front().size();
        if (peakBins > 0)
            m_kind->setItemText(
                0, QStringLiteral("峰值谱 %1 道").arg(peakBins));
    }
    const auto mode = AcquisitionMode(m_acquisitionMode->currentData().toInt());
    if (mode == AcquisitionMode::Waveform) {
        // Status updates are still allowed to refresh the shared snapshot,
        // but waveform mode owns the centre page and must not be redirected
        // to the scatter/absorption event map.
        if (m_waveformPageIndex >= 0)
            m_center->setCurrentIndex(m_waveformPageIndex);
        updateActionsForState();
        return;
    }
    if (!unchanged) m_spectrum->setSnapshot(m_snapshot);
    if (!unchanged && m_eventMap->isVisible()) m_eventMap->setSnapshot(m_snapshot, mode);
    if (mode == AcquisitionMode::LegacyFourChannel ||
        mode == AcquisitionMode::LegacyCoincidence)
        m_center->setCurrentIndex(2);
    else if (m_center->currentIndex() == 1)
        for (auto *view : m_gridViews) if (!unchanged) view->setSnapshot(m_snapshot);
    updateActionsForState();
    updateRoiAvailability();
}

void MainWindow::updateStatus(const AcquisitionSnapshot &snapshot) {
    m_acquisitionState = snapshot.state;
    if(m_psdPage)m_psdPage->setTransportStatus(snapshot.packets.lost,m_lastDeviceStatus.droppedEvents);
    if(m_psdPage)m_psdPage->setRunning(snapshot.state!=AcquisitionState::Idle && snapshot.state!=AcquisitionState::Error);

    // Status bar
    static const QString stateNames[] = {QStringLiteral("空闲"), QStringLiteral("准备中"), QStringLiteral("采集中"), QStringLiteral("暂停"), QStringLiteral("停止中"), QStringLiteral("错误")};
    static const char *stateKeys[] = {
        "idle", "starting", "running", "paused", "stopping", "error"
    };

    const int si = int(snapshot.state);
    QString stateText = stateNames[si];
    if (snapshot.mode == AcquisitionMode::ExternalTrigger &&
        snapshot.state == AcquisitionState::Running) {
        stateText = snapshot.externalWindowActive
            ? QStringLiteral("外触发采集中")
            : QStringLiteral("等待EXTRI");
        stateText += QStringLiteral("（次数 %1）")
                         .arg(snapshot.externalTriggerCount);
    }
    m_statusState->setText(stateText);
    m_statusState->setProperty("state", QString::fromLatin1(stateKeys[si]));
    m_statusState->style()->unpolish(m_statusState);
    m_statusState->style()->polish(m_statusState);

    m_statusLive->setText(QStringLiteral("LIVE %1").arg(formatSeconds(snapshot.liveSeconds)));
    if (snapshot.liveTimeEstimated)
        m_statusLive->setToolTip(QStringLiteral("活时为估计值，非硬件测量"));

    const double dead = snapshot.realSeconds > 0
        ? (snapshot.realSeconds - snapshot.liveSeconds) / snapshot.realSeconds * 100.0 : 0.0;
    m_statusDead->setText(QStringLiteral("DEAD %1%").arg(dead, 0, 'f', 1));

    m_statusRate->setText(formatRate(snapshot.eventsPerSecond));
    m_statusPkt->setText(snapshot.serialTransport
        ? QStringLiteral("UART 2M")
        : QStringLiteral("%1 pkt/s").arg(snapshot.packetsPerSecond, 0, 'f', 0));
    m_statusPkt->setToolTip(QStringLiteral(
        "UDP %1 Mbps；有效事件 %2；报文 %3；重复 %4；乱序 %5；"
        "非法报文 %6；接收缓冲 %7 字节")
        .arg(snapshot.udpMegabitsPerSecond, 0, 'f', 2)
        .arg(snapshot.packets.validEventCount)
        .arg(snapshot.packets.eventDatagrams)
        .arg(snapshot.packets.duplicate)
        .arg(snapshot.packets.reordered)
        .arg(snapshot.packets.malformed)
        .arg(snapshot.packets.receiveBufferBytes));

    if (snapshot.serialTransport) {
        m_statusLoss->setText(QStringLiteral("CRC %1 / RETRY %2")
                              .arg(snapshot.serialCrcErrors)
                              .arg(snapshot.serialRetries));
        m_statusLoss->setToolTip(QStringLiteral(
            "FPGA串口CRC错误 %1；帧/停止位错误 %2；重发 %3；Host解析错误 %4")
            .arg(snapshot.serialCrcErrors)
            .arg(snapshot.serialFrameErrors)
            .arg(snapshot.serialRetries)
            .arg(snapshot.packets.parseErrors));
    } else {
        m_statusLoss->setText(QStringLiteral("LOSS %1 +%2")
                              .arg(snapshot.packets.confirmedMissing)
                              .arg(snapshot.packets.pendingMissing));
        m_statusLoss->setToolTip(QStringLiteral(
            "LOSS 已确认 %1；窗口内待确认 %2；重复事件 %3；解析错误 %4")
            .arg(snapshot.packets.confirmedMissing)
            .arg(snapshot.packets.pendingMissing)
            .arg(snapshot.packets.duplicateEvents)
            .arg(snapshot.packets.parseErrors));
    }
    m_statusLoss->setProperty("alert", !snapshot.complete);
    m_statusLoss->style()->unpolish(m_statusLoss);
    m_statusLoss->style()->polish(m_statusLoss);

    const double pileupPercent = snapshot.pileupFraction * 100.0;
    m_statusPileup->setText(
        snapshot.pileupEvents > 0 && pileupPercent < 0.01
            ? QStringLiteral("PILEUP <0.01%")
            : QStringLiteral("PILEUP %1%").arg(pileupPercent, 0, 'f', 2));
    m_statusPileup->setToolTip(QStringLiteral("%1 %2 / 总事件 %3；%4能谱")
        .arg(snapshot.serialTransport ? QStringLiteral("拒收越阈")
                                      : QStringLiteral("堆积事件"))
        .arg(snapshot.pileupEvents)
        .arg(snapshot.totalEvents)
        .arg(snapshot.includePileupInSpectrum ? QStringLiteral("计入") : QStringLiteral("不计入")));

    if (!snapshot.eventTextError.isEmpty()) {
        m_statusTextFile->setText(QStringLiteral("TXT 错误"));
        m_statusTextFile->setProperty("state", QStringLiteral("error"));
        m_statusTextFile->setToolTip(snapshot.eventTextError);
    } else if (snapshot.eventTextEnabled) {
        m_statusTextFile->setText(
            QStringLiteral("TXT %1").arg(snapshot.eventTextEvents));
        m_statusTextFile->setProperty("state", QStringLiteral("active"));
        m_statusTextFile->setToolTip(snapshot.eventTextPath);
    } else {
        m_statusTextFile->setText(QStringLiteral("TXT 关闭"));
        m_statusTextFile->setProperty("state", QStringLiteral("idle"));
        m_statusTextFile->setToolTip(QStringLiteral("未记录事件 TXT"));
    }
    m_statusTextFile->style()->unpolish(m_statusTextFile);
    m_statusTextFile->style()->polish(m_statusTextFile);

    // Update preset widget
    m_presetWidget->setElapsed(snapshot.realSeconds);
    const auto preset = m_presetWidget->preset();
    if (preset.useRealTime)
        m_presetWidget->setRemaining(qMax(0.0, preset.realTimeSeconds - snapshot.realSeconds));
    else if (preset.useLiveTime)
        m_presetWidget->setRemaining(qMax(0.0, preset.liveTimeSeconds - snapshot.liveSeconds));

    // Update pause/resume label
    if (snapshot.state == AcquisitionState::Paused)
        m_actPause->setText(QStringLiteral("恢复"));
    else
        m_actPause->setText(QStringLiteral("暂停"));

    updateActionsForState();
    updateEventTextAvailability();

    if (m_closeAfterStop &&
        (snapshot.state == AcquisitionState::Idle ||
         snapshot.state == AcquisitionState::Error)) {
        m_closeAfterStop = false;
        QTimer::singleShot(0, this, &QWidget::close);
    }
}

void MainWindow::applyBaselineTracking() {
    if ((m_acquisitionState != AcquisitionState::Idle &&
         m_acquisitionState != AcquisitionState::Error) ||
        m_controlBusy)
        return;
    const bool serial = m_connectionType &&
                        m_connectionType->currentData().toInt() == 1;
    if (serial) {
        const QString portName = m_serialPort
            ? m_serialPort->currentData().toString() : QString();
        if (portName.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("基线设置未应用：请选择串口"), 8000);
            return;
        }
        m_acq.applyBaselineTrackingSerial(
            portName, m_baselineTracking->isChecked(),
            quint16(m_manualBaseline->value()),
            quint16(m_recoveryTolerance->value()),
            m_bandwidthLimit->isChecked(),
            quint8(m_inputRange->currentData().toInt()),
            m_peakInterpolation->isChecked());
        m_manualBaselineTelemetryTarget = quint16(m_manualBaseline->value());
        m_manualBaselineTelemetrySyncPending = true;
        m_frontendTelemetryRecoveryTarget = quint16(m_recoveryTolerance->value());
        m_frontendTelemetryRangeTarget = quint8(m_inputRange->currentData().toInt());
        m_frontendTelemetryBandwidthTarget = m_bandwidthLimit->isChecked();
        m_frontendTelemetryPeakInterpolationTarget = m_peakInterpolation->isChecked();
        m_frontendTelemetrySyncPending =
            m_deviceCapabilities.supportsFrontendState();
        statusBar()->showMessage(QStringLiteral("正在通过串口应用基线设置…"));
        return;
    }
    auto *ip = findChild<QLineEdit *>("deviceIp");
    auto *dataPort = findChild<QSpinBox *>("dataPort");
    auto *controlPort = findChild<QSpinBox *>("controlPort");
    if (!ip || !dataPort || !controlPort) return;
    QHostAddress address;
    if (!address.setAddress(ip->text().trimmed()) ||
        address.protocol() != QAbstractSocket::IPv4Protocol) {
        statusBar()->showMessage(
            QStringLiteral("基线设置未应用：请输入有效的 FPGA IPv4 地址"), 8000);
        m_connectionDock->show();
        m_connectionDock->raise();
        ip->setFocus();
        return;
    }
    m_acq.applyBaselineTracking(dataPort->value(), address,
                                controlPort->value(), m_baselineTracking->isChecked(),
                                quint16(m_manualBaseline->value()),
                                quint16(m_recoveryTolerance->value()),
                                m_bandwidthLimit->isChecked(),
                                quint8(m_inputRange->currentData().toInt()),
                                m_peakInterpolation->isChecked());
    m_manualBaselineTelemetryTarget = quint16(m_manualBaseline->value());
    m_manualBaselineTelemetrySyncPending = true;
    m_frontendTelemetryRecoveryTarget = quint16(m_recoveryTolerance->value());
    m_frontendTelemetryRangeTarget = quint8(m_inputRange->currentData().toInt());
    m_frontendTelemetryBandwidthTarget = m_bandwidthLimit->isChecked();
    m_frontendTelemetryPeakInterpolationTarget = m_peakInterpolation->isChecked();
    m_frontendTelemetrySyncPending =
        m_deviceCapabilities.supportsFrontendState();
    statusBar()->showMessage(QStringLiteral("正在应用基线设置…"));
}

void MainWindow::updateBaselineStatus(const BaselineSnapshot &snapshot) {
    Q_UNUSED(snapshot.sequence);
    bool allTracking = !snapshot.channels.isEmpty();
    for (const auto &status : snapshot.channels) {
        allTracking = allTracking && status.trackingEnabled;
        if (status.channel >= m_channels->rowCount()) continue;
        const int row = status.channel;
        if (auto *baseline = channelBaselineValueWidget(row))
            baseline->setText(QStringLiteral("基线 %1").arg(
                status.baselineValid ? QString::number(status.baseline)
                                     : QStringLiteral("—")));
        QString text;
        if (!status.trackingEnabled)
            text = QStringLiteral("固定");
        else if (status.ready)
            text = QStringLiteral("跟踪");
        else
            text = QStringLiteral("训练/恢复（未就绪）");
        if (auto *state = channelBaselineStateWidget(row))
            state->setText(QStringLiteral("状态 %1").arg(text));
    }
    if (!snapshot.channels.isEmpty()) {
        const bool pendingTracking = m_baselineTracking->isChecked();
        if (m_baselineTrackingDirty) {
            // 状态包反映的是 FPGA 当前已应用值。在用户修改复选框后，
            // 旧状态包不能覆盖尚待“应用”的选择；当硬件值最终与待应用值
            // 一致时，再把该选择视为已确认。
            if (!m_controlBusy && pendingTracking == allTracking) {
                m_baselineTrackingDirty = false;
                m_baselineTelemetrySyncPending = false;
                m_confirmedBaselineTracking = allTracking;
            }
        } else if (m_baselineTelemetrySyncPending) {
            // A status packet already in the socket can describe the state
            // before the just-completed commit.  Do not make the checkbox
            // oscillate back to that stale value.  The first matching packet
            // completes synchronization.
            if (!m_controlBusy &&
                allTracking == m_baselineTelemetryTarget) {
                m_baselineTelemetrySyncPending = false;
                m_confirmedBaselineTracking = allTracking;
                const QSignalBlocker blocker(m_baselineTracking);
                m_baselineTracking->setChecked(allTracking);
            }
        } else if (!m_controlBusy) {
            m_confirmedBaselineTracking = allTracking;
            const QSignalBlocker blocker(m_baselineTracking);
            m_baselineTracking->setChecked(allTracking);
        }
    }
    if (snapshot.manualBaselineValid && !m_controlBusy &&
        !m_manualBaseline->hasFocus() &&
        !(m_manualBaseline->findChild<QLineEdit *>() &&
          m_manualBaseline->findChild<QLineEdit *>()->hasFocus())) {
        // A status datagram may have been queued before the commit that just
        // applied the user's value.  Keep the entered value until a packet
        // confirms that exact value; never snap it back to the reset default.
        if (m_manualBaselineDirty) {
            // The user has edited the field but has not successfully applied
            // it yet.  Leave the editor untouched.
        } else if (m_manualBaselineTelemetrySyncPending) {
            if (snapshot.manualBaseline == m_manualBaselineTelemetryTarget) {
                m_manualBaselineTelemetrySyncPending = false;
                m_confirmedManualBaseline = snapshot.manualBaseline;
                const QSignalBlocker blocker(m_manualBaseline);
                m_manualBaseline->setValue(snapshot.manualBaseline);
            }
        } else {
            m_confirmedManualBaseline = snapshot.manualBaseline;
            const QSignalBlocker blocker(m_manualBaseline);
            m_manualBaseline->setValue(snapshot.manualBaseline);
        }
    }
}

void MainWindow::changeView() {
    const int nextChannel = activeRoiChannel();
    const auto nextKind = m_kind->currentIndex()
        ? SpectrumKind::Integral : SpectrumKind::Peak;
    const bool contextChanged = nextChannel != m_activeRoiChannel ||
                                nextKind != m_activeRoiKind;
    // The waveform page is a separate centre page, not another spectrum
    // layout, and must never be replaced by the event map.
    const auto currentMode = m_acquisitionMode
        ? AcquisitionMode(m_acquisitionMode->currentData().toInt())
        : AcquisitionMode::AutomaticTrigger;
    if(currentMode==AcquisitionMode::Psd) {
        m_center->setCurrentIndex(m_psdPageIndex);updateRoiAvailability();return;
    }
    if (currentMode == AcquisitionMode::Waveform) {
        if (contextChanged)
            storeActiveRoiContext();
        if (m_waveformPageIndex >= 0)
            m_center->setCurrentIndex(m_waveformPageIndex);
        updateRoiAvailability();
        refreshRoiInfo();
        return;
    }
    if (contextChanged)
        storeActiveRoiContext();

    if (currentMode != AcquisitionMode::AutomaticTrigger &&
        currentMode != AcquisitionMode::ExternalTrigger) {
        m_center->setCurrentIndex(2);
        m_eventMap->setSnapshot(m_snapshot,currentMode);
        updateRoiAvailability();
        return;
    }
    m_spectrum->setKind(nextKind);
    for (auto *view : m_gridViews) view->setKind(nextKind);

    const auto &cal = m_kind->currentIndex() ? m_integralCal : m_peakCal;
    m_spectrum->setCalibration(cal);
    for (auto *view : m_gridViews) view->setCalibration(cal);

    if (m_layout->currentIndex() == 0)
        m_spectrum->setChannels({nextChannel});
    else if (m_layout->currentIndex() == 1)
        m_spectrum->setChannels(selectedChannels());

    m_center->setCurrentIndex(m_layout->currentIndex() == 2 ? 1 : 0);
    if (m_layout->currentIndex() == 2)
        statusBar()->showMessage(
            QStringLiteral("8 通道宫格仅用于总览；请切换到单通道或叠加视图编辑 ROI。"),
            5000);
    if (contextChanged)
        loadRoiContext(nextChannel, nextKind);
    updateRoiAvailability();
    refreshRoiInfo();
}

void MainWindow::applyDisplaySettings() {
    const bool log = m_actLogY->isChecked();
    m_spectrum->setLogarithmic(log);
    for (auto *v : m_gridViews) v->setLogarithmic(log);
}

// ═══════════════════════════════════════════════════════════════════════════
// Waveform mode
// ═══════════════════════════════════════════════════════════════════════════

void MainWindow::startWaveform(WaveformTriggerMode mode, ChannelConfig config) {
    if (m_deviceCapabilitiesKnown &&
        m_deviceCapabilities.formatFlagsValid &&
        !m_deviceCapabilities.supportsWaveform()) {
        statusBar()->showMessage(
            QStringLiteral("该固件不支持波形模式（缺少能力位 bit5），"
                           "波形入口已禁用"), 8000);
        QMessageBox::information(
            this, QStringLiteral("波形模式不可用"),
            QStringLiteral("当前 FPGA 固件没有报告波形协议能力位 bit5。\n"
                           "请升级固件后再使用波形模式；上位机不会向旧固件"
                           "发送新寄存器配置。"));
        return;
    }
    // Starting from the global toolbar must be equivalent to starting from
    // the waveform page itself.  Keep the page selected before asynchronous
    // capability/status packets begin updating the UI.
    if (m_waveformPageIndex >= 0)
        m_center->setCurrentIndex(m_waveformPageIndex);
    auto *ip = findChild<QLineEdit *>("deviceIp");
    auto *dataPort = findChild<QSpinBox *>("dataPort");
    auto *controlPort = findChild<QSpinBox *>("controlPort");
    if (!ip || !dataPort || !controlPort)
        return;
    const QHostAddress address(ip->text().trimmed());
    if (address.isNull()) {
        statusBar()->showMessage(QStringLiteral("FPGA 地址无效"), 6000);
        return;
    }
    m_acquisitionState = AcquisitionState::Running;
    m_waveformPage->setRunning(true);
    m_waveformPage->setBusy(true);
    updateActionsForState();
    m_acq.startWaveform(quint16(dataPort->value()), address,
                        quint16(controlPort->value()), mode, config);
}

void MainWindow::applyWaveformConfig(ChannelConfig config) {
    if (m_acquisitionState == AcquisitionState::Running) {
        m_waveformPage->setBusy(true);
        updateActionsForState();
    }
    m_acq.updateWaveformConfig(config);
}

void MainWindow::exportWaveformCsv() {
    auto capture = m_waveformPage->capture();
    if (!capture || !capture->valid()) {
        statusBar()->showMessage(QStringLiteral("没有完整波形可导出"), 6000);
        return;
    }
    const QString suggested = QStringLiteral("waveform_%1.csv")
                                  .arg(capture->captureId);
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出波形 CSV"),
        m_lastDirectory.isEmpty() ? suggested
                                  : QDir(m_lastDirectory).filePath(suggested),
        QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!writeWaveformCsv(path, *capture, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        statusBar()->showMessage(
            QStringLiteral("波形导出失败，界面数据已保留：%1").arg(error), 8000);
        return;
    }
    m_lastDirectory = QFileInfo(path).absolutePath();
    statusBar()->showMessage(
        QStringLiteral("已导出 %1（%2 点）")
            .arg(path).arg(capture->totalSamples), 8000);
}

void MainWindow::updateWaveformStatus(const WaveformStatus &status) {
    m_waveformPage->setStatus(status);
    const bool active = status.state != WaveformState::Stopped;
    m_waveformPage->setRunning(active);
    if (!m_controlBusy)
        m_waveformPage->setBusy(false);
    if (!active && m_acquisitionState == AcquisitionState::Running
        && m_acquisitionMode &&
        AcquisitionMode(m_acquisitionMode->currentData().toInt()) ==
            AcquisitionMode::Waveform) {
        // The device has released the buffer and reports "stopped".
        m_acquisitionState = AcquisitionState::Idle;
    } else if (active) {
        m_acquisitionState = AcquisitionState::Running;
    }
    updateActionsForState();
}

// ═══════════════════════════════════════════════════════════════════════════
// Channel config
// ═══════════════════════════════════════════════════════════════════════════

QSpinBox *MainWindow::channelThresholdWidget(int row) const {
    if (!m_channels || row < 0 || row >= m_channels->rowCount())
        return nullptr;
    auto *panel = m_channels->cellWidget(row, 2);
    return panel ? panel->findChild<QSpinBox *>() : nullptr;
}

QComboBox *MainWindow::channelPolarityWidget(int row) const {
    if (!m_channels || row < 0 || row >= m_channels->rowCount())
        return nullptr;
    auto *panel = m_channels->cellWidget(row, 4);
    return panel ? panel->findChild<QComboBox *>() : nullptr;
}

QLabel *MainWindow::channelBaselineValueWidget(int row) const {
    if (!m_channels || row < 0 || row >= m_channels->rowCount())
        return nullptr;
    auto *panel = m_channels->cellWidget(row, 6);
    return panel ? panel->findChild<QLabel *>(
        QStringLiteral("channelBaselineValue")) : nullptr;
}

QLabel *MainWindow::channelBaselineStateWidget(int row) const {
    if (!m_channels || row < 0 || row >= m_channels->rowCount())
        return nullptr;
    auto *panel = m_channels->cellWidget(row, 6);
    return panel ? panel->findChild<QLabel *>(
        QStringLiteral("channelBaselineState")) : nullptr;
}

void MainWindow::ensureChannelRows(int count) {
    count = qBound(1, count, MaxChannels);
    const int first = m_channels->rowCount();
    if (count <= first) {
        m_channels->setRowCount(count);
        m_displayedChannelRow =
            qBound(0, m_displayedChannelRow, count - 1);
        m_displayedChannelRows.erase(
            std::remove_if(m_displayedChannelRows.begin(),
                           m_displayedChannelRows.end(),
                           [count](int row) { return row >= count; }),
            m_displayedChannelRows.end());
        if (m_displayedChannelRows.isEmpty())
            m_displayedChannelRows.push_back(m_displayedChannelRow);
        if (m_channels->currentRow() < 0 ||
            m_channels->currentRow() >= count)
            m_channels->selectRow(m_displayedChannelRow);
        return;
    }
    const QSignalBlocker blocker(m_channels);
    m_channels->setRowCount(count);
    for (int row = first; row < count; ++row) {
        auto *enabled = new QTableWidgetItem;
        enabled->setCheckState(Qt::Checked);
        m_channels->setItem(row, 0, enabled);
        auto *channel = new QTableWidgetItem(panelChannelName(row));
        channel->setFlags(channel->flags() & ~Qt::ItemIsEditable);
        m_channels->setItem(row, 1, channel);
        auto *threshold = new QSpinBox;
        threshold->setRange(1, 4095);
        threshold->setValue(64);
        auto *send = new QPushButton(QStringLiteral("发送"));
        threshold->setPrefix(QStringLiteral("阈值 "));
        auto *thresholdPanel = new QWidget;
        auto *thresholdLayout = new QVBoxLayout(thresholdPanel);
        thresholdLayout->setContentsMargins(2, 3, 2, 3);
        thresholdLayout->setSpacing(4);
        thresholdLayout->addWidget(threshold);
        thresholdLayout->addWidget(send);
        m_channels->setCellWidget(row, 2, thresholdPanel);
        auto *polarity = new QComboBox;
        polarity->addItems({QStringLiteral("正"), QStringLiteral("负")});
        polarity->setMinimumWidth(54);
        auto *shapePanel = new QWidget;
        auto *shapeLayout = new QVBoxLayout(shapePanel);
        shapeLayout->setContentsMargins(2, 6, 2, 6);
        shapeLayout->setSpacing(0);
        shapeLayout->addWidget(polarity);
        m_channels->setCellWidget(row, 4, shapePanel);
        auto *baselineValue = new QLabel(QStringLiteral("基线 2048"));
        baselineValue->setObjectName(QStringLiteral("channelBaselineValue"));
        baselineValue->setAlignment(Qt::AlignCenter);
        auto *baselineState = new QLabel(QStringLiteral("状态 固定"));
        baselineState->setObjectName(QStringLiteral("channelBaselineState"));
        baselineState->setAlignment(Qt::AlignCenter);
        auto *baselinePanel = new QWidget;
        auto *baselineLayout = new QVBoxLayout(baselinePanel);
        baselineLayout->setContentsMargins(2, 3, 2, 3);
        baselineLayout->setSpacing(4);
        baselineLayout->addWidget(baselineValue);
        baselineLayout->addWidget(baselineState);
        m_channels->setCellWidget(row, 6, baselinePanel);
        m_channels->setRowHeight(row, 68);
        connect(threshold, qOverload<int>(&QSpinBox::valueChanged),
                this, [this, row] { saveChannelConfig(row); });
        connect(send, &QPushButton::clicked,
                this, [this, row] { sendChannelThreshold(row); });
        connect(polarity, &QComboBox::currentIndexChanged,
                this, [this, row] { saveChannelConfig(row); });
    }
    m_displayedChannelRow =
        qBound(0, m_displayedChannelRow, count - 1);
}

void MainWindow::ensureModuleRows(int count) {
    if (!m_modules)
        return;
    count = qBound(1, count, 8);
    const int first = m_modules->rowCount();
    const QSignalBlocker blocker(m_modules);
    m_modules->setRowCount(count);
    for (int row = first; row < count; ++row) {
        auto *enabled = new QTableWidgetItem;
        enabled->setCheckState(Qt::Unchecked);
        m_modules->setItem(row, 0, enabled);
        auto *id = new QTableWidgetItem(panelModuleName(row));
        id->setFlags(id->flags() & ~Qt::ItemIsEditable);
        m_modules->setItem(row, 1, id);
        auto *window = new QSpinBox;
        window->setRange(1, 65535);
        window->setValue(8);
        window->setToolTip(QStringLiteral("8 个采样 = 100 ns"));
        m_modules->setCellWidget(row, 2, window);
        auto *gate = new QTableWidgetItem;
        gate->setCheckState(Qt::Unchecked);
        m_modules->setItem(row, 3, gate);
        auto *minimum = new QLineEdit(QStringLiteral("0"));
        auto *maximum = new QLineEdit(QStringLiteral("4294967295"));
        m_modules->setCellWidget(row, 4, minimum);
        m_modules->setCellWidget(row, 5, maximum);
        auto *quality = new QSpinBox;
        quality->setRange(0, 255);
        quality->setDisplayIntegerBase(16);
        quality->setPrefix("0x");
        m_modules->setCellWidget(row, 6, quality);
        connect(window, qOverload<int>(&QSpinBox::valueChanged),
                this, [this, row] { applyModuleConfig(row); });
        connect(minimum, &QLineEdit::editingFinished,
                this, [this, row] { applyModuleConfig(row); });
        connect(maximum, &QLineEdit::editingFinished,
                this, [this, row] { applyModuleConfig(row); });
        connect(quality, qOverload<int>(&QSpinBox::valueChanged),
                this, [this, row] { applyModuleConfig(row); });
    }
}

void MainWindow::ensurePairRows(int count) {
    if (!m_pairs || !m_modules)
        return;
    Q_UNUSED(count);
    const QSignalBlocker blocker(m_pairs);
    m_pairs->clearContents();
    m_pairs->setRowCount(1);
    for (int row = 0; row < 1; ++row) {
        auto *enabled = new QTableWidgetItem;
        enabled->setCheckState(Qt::Unchecked);
        m_pairs->setItem(row, 0, enabled);
        auto *id = new QTableWidgetItem(QStringLiteral("0 (1×1)"));
        id->setFlags(id->flags() & ~Qt::ItemIsEditable);
        m_pairs->setItem(row, 1, id);
        auto makeMask = [this, row](quint8 initialMask) {
            auto *container = new QWidget;
            auto *layout = new QHBoxLayout(container);
            layout->setContentsMargins(2, 0, 2, 0);
            layout->setSpacing(3);
            for (int module = 0; module < m_modules->rowCount(); ++module) {
                auto *box = new QCheckBox(panelModuleName(module));
                box->setProperty("module", module);
                box->setChecked((initialMask & (1u << module)) != 0);
                connect(box, &QCheckBox::toggled, this,
                        [this, row] { applyPairConfig(row); });
                layout->addWidget(box);
            }
            layout->addStretch();
            return container;
        };
        auto *scatter = makeMask(0x01);
        auto *absorber = makeMask(
            m_modules->rowCount() > 1 ? quint8(0x02) : quint8(0));
        m_pairs->setCellWidget(row, 2, scatter);
        m_pairs->setCellWidget(row, 3, absorber);
        auto makeTime = [](int value) {
            auto *spin = new QSpinBox;
            spin->setRange(-32768, 32767);
            spin->setValue(value);
            spin->setSuffix(QStringLiteral(" 采样"));
            return spin;
        };
        auto *minimumTime = makeTime(-8);
        auto *maximumTime = makeTime(8);
        m_pairs->setCellWidget(row, 4, minimumTime);
        m_pairs->setCellWidget(row, 5, maximumTime);
        auto *scatterGate = new QTableWidgetItem;
        scatterGate->setCheckState(Qt::Unchecked);
        m_pairs->setItem(row, 6, scatterGate);
        auto *scatterMin = new QLineEdit(QStringLiteral("0"));
        auto *scatterMax = new QLineEdit(QStringLiteral("4294967295"));
        m_pairs->setCellWidget(row, 7, scatterMin);
        m_pairs->setCellWidget(row, 8, scatterMax);
        auto *absorberGate = new QTableWidgetItem;
        absorberGate->setCheckState(Qt::Unchecked);
        m_pairs->setItem(row, 9, absorberGate);
        auto *absorberMin = new QLineEdit(QStringLiteral("0"));
        auto *absorberMax = new QLineEdit(QStringLiteral("4294967295"));
        m_pairs->setCellWidget(row, 10, absorberMin);
        m_pairs->setCellWidget(row, 11, absorberMax);
        auto *quality = new QSpinBox;
        quality->setRange(0, 255);
        quality->setDisplayIntegerBase(16);
        quality->setPrefix("0x");
        m_pairs->setCellWidget(row, 12, quality);
        for (auto *spin : {minimumTime, maximumTime, quality})
            connect(spin, qOverload<int>(&QSpinBox::valueChanged),
                    this, [this, row] { applyPairConfig(row); });
        for (auto *edit : {scatterMin, scatterMax, absorberMin, absorberMax})
            connect(edit, &QLineEdit::editingFinished,
                    this, [this, row] { applyPairConfig(row); });
    }
}

ModuleConfig MainWindow::moduleConfigFromRow(int row) const {
    ModuleConfig config;
    if (!m_modules || row < 0 || row >= m_modules->rowCount())
        return config;
    config.enabled = m_modules->item(row, 0)->checkState() == Qt::Checked;
    config.windowSamples = quint16(
        qobject_cast<QSpinBox *>(m_modules->cellWidget(row, 2))->value());
    config.energyGateEnabled =
        m_modules->item(row, 3)->checkState() == Qt::Checked;
    config.energyMin =
        qobject_cast<QLineEdit *>(m_modules->cellWidget(row, 4))->text().toUInt();
    config.energyMax =
        qobject_cast<QLineEdit *>(m_modules->cellWidget(row, 5))->text().toUInt();
    config.qualityRejectMask = quint8(
        qobject_cast<QSpinBox *>(m_modules->cellWidget(row, 6))->value());
    return config;
}

void MainWindow::applyModuleConfig(int row) {
    if (!m_modules || row < 0 || row >= m_modules->rowCount())
        return;
    m_acq.applyModuleConfig(row, moduleConfigFromRow(row));
}

PairConfig MainWindow::pairConfigFromRow(int row, bool updateUi) {
    PairConfig config;
    if (!m_pairs || row < 0 || row >= m_pairs->rowCount())
        return config;
    config.enabled = m_pairs->item(row, 0)->checkState() == Qt::Checked;
    const auto readMask = [this, row](int column) {
        quint8 mask = 0;
        const auto boxes =
            m_pairs->cellWidget(row, column)->findChildren<QCheckBox *>();
        for (auto *box : boxes) {
            const int module = box->property("module").toInt();
            if (box->isChecked() && module >= 0 && module < 8)
                mask |= quint8(1u << module);
        }
        return mask;
    };
    config.scatterModuleMask = readMask(2);
    config.absorberModuleMask = readMask(3);
    const auto bitCount = [](quint8 mask) {
        int total = 0;
        for (int bit = 0; bit < 8; ++bit)
            total += (mask >> bit) & 1u;
        return total;
    };
    const int scatterCount = bitCount(config.scatterModuleMask);
    const int absorberCount = bitCount(config.absorberModuleMask);
    if (updateUi) {
        for (auto *box :
             m_pairs->cellWidget(row, 2)->findChildren<QCheckBox *>()) {
            const quint8 bit =
                quint8(1u << box->property("module").toInt());
            box->setEnabled(box->isChecked() ||
                (!(config.absorberModuleMask & bit) && scatterCount < 4));
        }
        for (auto *box :
             m_pairs->cellWidget(row, 3)->findChildren<QCheckBox *>()) {
            const quint8 bit =
                quint8(1u << box->property("module").toInt());
            box->setEnabled(box->isChecked() ||
                (!(config.scatterModuleMask & bit) && absorberCount < 4));
        }
        m_pairs->item(row, 1)->setText(
            QStringLiteral("0 (%1×%2)")
                .arg(scatterCount).arg(absorberCount));
    }
    config.minimumTimeSamples = qint16(
        qobject_cast<QSpinBox *>(m_pairs->cellWidget(row, 4))->value());
    config.maximumTimeSamples = qint16(
        qobject_cast<QSpinBox *>(m_pairs->cellWidget(row, 5))->value());
    config.scatterEnergyGateEnabled =
        m_pairs->item(row, 6)->checkState() == Qt::Checked;
    config.scatterEnergyMin =
        qobject_cast<QLineEdit *>(m_pairs->cellWidget(row, 7))->text().toUInt();
    config.scatterEnergyMax =
        qobject_cast<QLineEdit *>(m_pairs->cellWidget(row, 8))->text().toUInt();
    config.absorberEnergyGateEnabled =
        m_pairs->item(row, 9)->checkState() == Qt::Checked;
    config.absorberEnergyMin =
        qobject_cast<QLineEdit *>(m_pairs->cellWidget(row, 10))->text().toUInt();
    config.absorberEnergyMax =
        qobject_cast<QLineEdit *>(m_pairs->cellWidget(row, 11))->text().toUInt();
    config.qualityRejectMask = quint8(
        qobject_cast<QSpinBox *>(m_pairs->cellWidget(row, 12))->value());
    return config;
}

void MainWindow::applyPairConfig(int row) {
    if (!m_pairs || row < 0 || row >= m_pairs->rowCount())
        return;
    m_acq.applyPairConfig(row, pairConfigFromRow(row));
}

void MainWindow::updateCapabilities(const DeviceCapabilities &capabilities) {
    m_deviceCapabilities = capabilities;
    m_deviceCapabilitiesKnown = true;
    ensureChannelRows(capabilities.channelCount);
    if (m_modules)
        ensureModuleRows(capabilities.moduleCount);
    if (m_pairs)
        ensurePairRows(capabilities.pairCount);
    const int thresholdMaximum = capabilities.adcBits >= 14 ? 16383 : 4095;
    m_globalThreshold->setMaximum(thresholdMaximum);
    for (int row = 0; row < m_channels->rowCount(); ++row)
        if (auto *threshold = channelThresholdWidget(row))
            threshold->setMaximum(thresholdMaximum);
    m_manualBaseline->setVisible(capabilities.supportsManualBaseline());
    m_manualBaseline->setEnabled(capabilities.supportsManualBaseline() &&
                                 !m_baselineTracking->isChecked());
    {
        const bool supported = !capabilities.formatFlagsValid ||
                               capabilities.supportsPeakInterpolation();
        m_peakInterpolation->setVisible(supported);
        m_peakInterpolation->setEnabled(m_acquisitionMode->currentData().toInt()!=int(AcquisitionMode::Psd) && supported &&
                                        m_acquisitionState == AcquisitionState::Idle &&
                                        !m_controlBusy);
        if (!supported) {
            const QSignalBlocker blocker(m_peakInterpolation);
            m_peakInterpolation->setChecked(false);
            m_confirmedPeakInterpolationEnabled = false;
        }
    }
    const double stepUs = capabilities.timebasePs / 1.0e6;
    m_integrationPostUs->setRange(
        capabilities.integrationWindowMinNs / 1000.0,
        capabilities.integrationWindowMaxNs / 1000.0);
    m_integrationPostUs->setSingleStep(stepUs);
    m_integrationPreUs->setRange(
        0.0, qMax(0.0, capabilities.integrationWindowMaxNs / 1000.0 - stepUs));
    m_integrationPreUs->setSingleStep(stepUs);
    updateIntegrationWindowSummary();
    // The waveform entry point is only usable on firmware that advertises the
    // waveform protocol capability bit; an old device never receives the new
    // register writes.
    if(m_acquisitionMode) {
        auto model=qobject_cast<QStandardItemModel*>(m_acquisitionMode->model());
        if(model)if(auto item=model->item(m_acquisitionMode->findData(int(AcquisitionMode::Psd)))) {
            const bool supported=(!capabilities.formatFlagsValid||capabilities.supportsPsd()) && m_connectionType->currentData().toInt()!=1;
            item->setEnabled(supported);
            if(!supported && m_acquisitionMode->currentData().toInt()==int(AcquisitionMode::Psd))m_acquisitionMode->setCurrentIndex(0);
        }
    }
    const bool waveformSupported = !capabilities.formatFlagsValid ||
                                   capabilities.supportsWaveform();
    const bool externalSupported = !capabilities.formatFlagsValid ||
                                   capabilities.supportsExternalTrigger();
    if (m_waveformPage) {
        m_waveformPage->setCapability(
            waveformSupported,
            QStringLiteral("当前固件不支持波形模式（缺少能力位 bit5），"
                           "请升级 FPGA 固件"));
    }
    if (m_acquisitionMode) {
        auto *model = qobject_cast<QStandardItemModel *>(
            m_acquisitionMode->model());
        if (model) {
            if (auto *item = model->item(
                    m_acquisitionMode->findData(int(AcquisitionMode::ExternalTrigger)))) {
                item->setEnabled(externalSupported);
                if (!externalSupported &&
                    m_acquisitionMode->currentData().toInt() ==
                        int(AcquisitionMode::ExternalTrigger)) {
                    const int automaticIndex = m_acquisitionMode->findData(
                        int(AcquisitionMode::AutomaticTrigger));
                    if (automaticIndex >= 0)
                        m_acquisitionMode->setCurrentIndex(automaticIndex);
                }
            }
            if (auto *item = model->item(
                    m_acquisitionMode->findData(int(AcquisitionMode::Waveform)))) {
                item->setEnabled(waveformSupported);
                if (!waveformSupported &&
                    m_acquisitionMode->currentData().toInt()
                        == int(AcquisitionMode::Waveform)) {
                    const int automaticIndex = m_acquisitionMode->findData(
                        int(AcquisitionMode::AutomaticTrigger));
                    if (automaticIndex >= 0)
                        m_acquisitionMode->setCurrentIndex(automaticIndex);
                }
            }
        }
    }
    statusBar()->showMessage(
        QStringLiteral("发现设备：%1 ADC、%2 通道、%3 模块、%4 配对项")
            .arg(capabilities.adcCount).arg(capabilities.channelCount)
            .arg(capabilities.moduleCount).arg(capabilities.pairCount),
        5000);
}

void MainWindow::updateDeviceStatus(const DeviceStatus &status) {
    m_lastDeviceStatus = status;
    if(m_psdPage)m_psdPage->setTransportStatus(0,status.droppedEvents);
    if (status.capabilitiesValid && m_channels) {
        const int reportedChannelCount = qBound(
            1, int(status.capabilities.channelCount), MaxChannels);
        if (m_channels->rowCount() != reportedChannelCount)
            ensureChannelRows(reportedChannelCount);
    }
    m_deviceStatusValid = status.frontendStateValid;
    if (status.frontendStateValid && !m_controlBusy) {
        const bool matches =
            !m_frontendTelemetrySyncPending ||
            (status.actualRange == m_frontendTelemetryRangeTarget &&
             status.filterEnabled == m_frontendTelemetryBandwidthTarget &&
             (!m_deviceCapabilities.supportsPeakInterpolation() ||
              status.peakInterpolationEnabled ==
                  m_frontendTelemetryPeakInterpolationTarget) &&
             status.recoveryToleranceLsb == m_frontendTelemetryRecoveryTarget);
        if (matches) {
            m_confirmedBandwidthEnabled = status.filterEnabled;
            m_confirmedPeakInterpolationEnabled =
                status.peakInterpolationEnabled;
            m_frontendTelemetrySyncPending = false;
            const QSignalBlocker bandwidthBlocker(m_bandwidthLimit);
            const QSignalBlocker interpolationBlocker(m_peakInterpolation);
            m_bandwidthLimit->setChecked(status.filterEnabled);
            if (m_deviceCapabilities.supportsPeakInterpolation())
                m_peakInterpolation->setChecked(status.peakInterpolationEnabled);
        }
    }
    if (m_statusDevice && status.frontendStateValid) {
        m_statusDevice->setToolTip(QStringLiteral(
            "FPGA 输出事件 %1；模块拒绝 %2；丢弃 %3；FIFO %4；回压 %5")
            .arg(status.outputEvents)
            .arg(status.moduleRejected)
            .arg(status.droppedEvents)
            .arg(status.fifoOverflow)
            .arg(status.outputBackpressure));
    }
    if (!m_frontendStatus)
        return;
    const QString range = status.actualRange == 1
        ? QStringLiteral("Q1 / 1 V")
        : status.actualRange == 2
            ? QStringLiteral("Q2 / 5 V")
            : QStringLiteral("未知");
    const QString configState = m_frontendTelemetrySyncPending
        ? QStringLiteral("待点击“应用”")
        : QStringLiteral("已生效");
    if (!status.frontendStateValid) {
        m_frontendStatus->setText(QStringLiteral(
            "输入状态　前端尚未确认\n"
            "处理状态　FIR / 峰值插值：等待 FPGA 回报\n"
            "配置状态　%1").arg(configState));
    } else {
        const QString baselineState = status.baselineAutomatic
            ? QStringLiteral("自动跟踪")
            : status.manualBaselineValid
                ? QStringLiteral("手动 %1 LSB").arg(status.manualBaseline)
                : QStringLiteral("手动（数值未回报）");
        m_frontendStatus->setText(QStringLiteral(
            "输入状态　量程：%1　初始化：%2%3\n"
            "处理状态　15 MHz FIR：%4　峰值插值：%5\n"
            "基线状态　%6　配置：%7")
            .arg(range)
            .arg(status.frontendInitDone ? QStringLiteral("完成")
                                         : QStringLiteral("进行中"))
            .arg(status.rangeBusy ? QStringLiteral("（量程切换中）")
                                  : QString())
            .arg(status.filterEnabled ? QStringLiteral("开")
                                      : QStringLiteral("关"))
            .arg(status.peakInterpolationEnabled ? QStringLiteral("开")
                                                  : QStringLiteral("关"))
            .arg(baselineState)
            .arg(configState));
    }
    m_frontendStatus->setToolTip(QStringLiteral(
        "FPGA 实际量程：%1；前端忙：%2；15 MHz 通带 FIR：%3；"
        "三点峰值插值：%4；初始化：%5；恢复容限：%6 LSB")
        .arg(range)
        .arg(status.rangeBusy ? QStringLiteral("是") : QStringLiteral("否"))
        .arg(status.filterEnabled ? QStringLiteral("开") : QStringLiteral("关"))
        .arg(status.peakInterpolationEnabled ? QStringLiteral("开") : QStringLiteral("关"))
        .arg(status.frontendInitDone ? QStringLiteral("完成") : QStringLiteral("未完成"))
        .arg(status.recoveryToleranceLsb));
}

ChannelConfig MainWindow::channelConfigFromRow(int row) const {
    ChannelConfig config;
    config.enabled = m_channels->item(row, 0)->checkState() == Qt::Checked;
    const auto *threshold = channelThresholdWidget(row);
    const auto *polarity = channelPolarityWidget(row);
    config.thresholdLsb = threshold ? quint16(threshold->value()) : 64;
    config.polarity = polarity
        ? static_cast<Polarity>(polarity->currentIndex())
        : Polarity::Positive;
    config.gainQ4_12 = 0x1000;
    const double samplesPerUs = m_deviceCapabilities.sampleRateHz / 1.0e6;
    config.integrationPostSamples = quint16(qRound(
        m_integrationPostUs->value() * samplesPerUs));
    config.integrationPreSamples = quint16(qRound(
        m_integrationPreUs->value() * samplesPerUs));
    return m_psdPage && m_acquisitionMode->currentData().toInt()==int(AcquisitionMode::Psd) ? m_psdPage->channelConfig(config) : config;
}

void MainWindow::commitIntegrationWindowEditor() {
    if (!m_integrationPostUs || !m_integrationPreUs)
        return;

    {
        const QSignalBlocker postBlocker(m_integrationPostUs);
        const QSignalBlocker preBlocker(m_integrationPreUs);
        m_integrationPostUs->interpretText();
        m_integrationPreUs->interpretText();
    }

    updateIntegrationWindowSummary();
}

void MainWindow::updateIntegrationWindowSummary() {
    const double samplesPerUs = qMax(1.0,
        m_deviceCapabilities.sampleRateHz / 1.0e6);
    const int minimumSamples = qMax(1, qRound(
        m_deviceCapabilities.integrationWindowMinNs * samplesPerUs / 1000.0));
    const int maximumSamples = qMax(minimumSamples, qRound(
        m_deviceCapabilities.integrationWindowMaxNs * samplesPerUs / 1000.0));
    const int postSamples = qBound(minimumSamples,
        qRound(m_integrationPostUs->value() * samplesPerUs), maximumSamples);
    const int preSamples = qBound(0,
        qRound(m_integrationPreUs->value() * samplesPerUs), postSamples - 1);
    const double postUs = postSamples / samplesPerUs;
    const double preUs = preSamples / samplesPerUs;

    // Snap typed values to the 12.5 ns hardware grid.
    if (!qFuzzyCompare(m_integrationPostUs->value() + 1.0, postUs + 1.0)) {
        const QSignalBlocker blocker(m_integrationPostUs);
        m_integrationPostUs->setValue(postUs);
    }
    if (!qFuzzyCompare(m_integrationPreUs->value() + 1.0, preUs + 1.0)) {
        const QSignalBlocker blocker(m_integrationPreUs);
        m_integrationPreUs->setValue(preUs);
    }
    m_integrationSummary->setText(
        QStringLiteral("总窗口 %1 点（%2 μs），其中前置 %3 点")
            .arg(postSamples).arg(postUs, 0, 'f', 4).arg(preSamples));
    for (int row = 0; row < m_channels->rowCount(); ++row)
        saveChannelConfig(row);
}

void MainWindow::saveChannelConfig(int row) {
    if (row >= 0 && row < m_channels->rowCount())
        m_acq.setChannelConfig(row, channelConfigFromRow(row));
}

void MainWindow::applyChannelConfig(int row, int) { saveChannelConfig(row); }

void MainWindow::sendChannelThreshold(int row) {
    if (row < 0 || row >= m_channels->rowCount()) return;
    const auto config = channelConfigFromRow(row);
    m_acq.setChannelConfig(row, config);
    m_acq.sendChannelThreshold(row, config.thresholdLsb);
    statusBar()->showMessage(
        m_hardwareResponsive
            ? QStringLiteral("%1 阈值正在应用").arg(panelChannelName(row))
            : QStringLiteral("%1 阈值已保存，开始采集时发送")
                  .arg(panelChannelName(row)),
        3000);
}

void MainWindow::applyGlobalThreshold() {
    const quint16 value = quint16(m_globalThreshold->value());
    for (int row = 0; row < m_channels->rowCount(); ++row) {
        if (auto *threshold = channelThresholdWidget(row))
            threshold->setValue(value);
        saveChannelConfig(row);
    }
    m_acq.applyAllThresholds(value);
    statusBar()->showMessage(
        m_hardwareResponsive ? QStringLiteral("八通道阈值正在应用")
                             : QStringLiteral("八通道阈值已保存"),
        3000);
}

QVector<int> MainWindow::selectedChannels() const {
    return m_displayedChannelRows.isEmpty()
        ? QVector<int>{m_displayedChannelRow}
        : m_displayedChannelRows;
}

void MainWindow::activateChannelRow(int row) {
    if (!m_channels || row < 0 || row >= m_channels->rowCount())
        return;
    m_displayedChannelRow = row;
    m_displayedChannelRows.clear();
    for (const auto &index :
         m_channels->selectionModel()->selectedRows())
        m_displayedChannelRows.push_back(index.row());
    if (!m_displayedChannelRows.contains(row))
        m_displayedChannelRows.push_back(row);
    std::sort(m_displayedChannelRows.begin(),
              m_displayedChannelRows.end());
    changeView();
}

int MainWindow::activeRoiChannel() const {
    if (!m_channels || m_channels->rowCount() == 0)
        return 0;
    return qBound(0, m_displayedChannelRow,
                  m_channels->rowCount() - 1);
}

int MainWindow::roiContextIndex(int channel, SpectrumKind kind) const {
    channel = qBound(0, channel, MaxChannels - 1);
    return channel * 2 + (kind == SpectrumKind::Integral ? 1 : 0);
}

void MainWindow::storeActiveRoiContext() {
    if (m_loadingRoiContext || !m_roiTable || !m_spectrum)
        return;
    RoiContext &context = m_roiContexts[size_t(
        roiContextIndex(m_activeRoiChannel, m_activeRoiKind))];
    context.rois.clear();
    const auto displayed = m_spectrum->rois();
    for (int row = 0; row < m_roiTable->rowCount(); ++row) {
        Roi roi;
        roi.name = m_roiTable->item(row, 0)->text();
        roi.left = m_roiTable->item(row, 1)->text().toInt();
        roi.right = m_roiTable->item(row, 2)->text().toInt();
        if (row < displayed.size())
            roi.color = displayed[row].color;
        context.rois.push_back(roi);
    }
    context.selected = m_roiTable->currentRow();
}

void MainWindow::loadRoiContext(int channel, SpectrumKind kind) {
    channel = qBound(0, channel, MaxChannels - 1);
    m_activeRoiChannel = channel;
    m_activeRoiKind = kind;
    const RoiContext &context = m_roiContexts[size_t(
        roiContextIndex(channel, kind))];

    m_loadingRoiContext = true;
    m_roiTable->clearContents();
    m_roiTable->setRowCount(context.rois.size());
    for (int row = 0; row < context.rois.size(); ++row) {
        const auto &roi = context.rois[row];
        m_roiTable->setItem(row, 0, new QTableWidgetItem(roi.name));
        m_roiTable->setItem(row, 1,
                            new QTableWidgetItem(QString::number(roi.left)));
        m_roiTable->setItem(row, 2,
                            new QTableWidgetItem(QString::number(roi.right)));
    }
    m_spectrum->setRois(context.rois);
    if (context.selected >= 0 && context.selected < context.rois.size()) {
        m_roiTable->selectRow(context.selected);
        m_spectrum->setSelectedRoi(context.selected);
    } else {
        m_roiTable->clearSelection();
        m_spectrum->setSelectedRoi(-1);
    }
    const int maximum = maximumSpectrumChannel(kind, channel);
    m_roiLeft->setRange(0, maximum);
    m_roiRight->setRange(0, maximum);
    m_analysisDock->setWindowTitle(
        QStringLiteral("ROI 与峰分析 — %1 / %2")
            .arg(panelChannelName(channel))
            .arg(kind == SpectrumKind::Peak
                     ? QStringLiteral("峰值谱")
                     : QStringLiteral("积分谱")));
    m_loadingRoiContext = false;
    clearFit();
    refreshRoiInfo();
}

void MainWindow::updateRoiAvailability() {
    const auto mode = AcquisitionMode(m_acquisitionMode->currentData().toInt());
    const bool spectrumMode = mode == AcquisitionMode::AutomaticTrigger ||
                              mode == AcquisitionMode::ExternalTrigger;
    const bool available = spectrumMode && m_layout->currentIndex() != 2;
    const bool hasRoi = m_roiTable && m_roiTable->rowCount() > 0;
    const bool hasSelection = hasRoi && m_roiTable->currentRow() >= 0;
    m_actNewRoi->setEnabled(available);
    m_actCreateRoiMode->setEnabled(available);
    m_actEditRoiMode->setEnabled(available && hasRoi);
    m_actDeleteRoi->setEnabled(available && hasSelection);
    m_actClearRois->setEnabled(available && hasRoi);
    m_actPrevRoi->setEnabled(available && hasSelection &&
                             m_roiTable->currentRow() > 0);
    m_actNextRoi->setEnabled(available && hasSelection &&
                             m_roiTable->currentRow() + 1 <
                                 m_roiTable->rowCount());
    m_actSingleFit->setEnabled(available && hasSelection && bool(m_snapshot));
    m_actClearFit->setEnabled(available && m_spectrum->hasFitCurve());
    if (m_analysisDock && m_analysisDock->widget())
        m_analysisDock->widget()->setEnabled(available);
    if (!available)
        m_analysisDock->setToolTip(QStringLiteral(
            "ROI 分析仅适用于单通道或多通道叠加谱图；"
            "8 通道宫格和模块事件图中不可编辑 ROI。"));
    else
        m_analysisDock->setToolTip(QString());
}

// ═══════════════════════════════════════════════════════════════════════════
// ROI management
// ═══════════════════════════════════════════════════════════════════════════

void MainWindow::addRoi() {
    int left = m_roiLeft->value(), right = m_roiRight->value();
    if (left > right) std::swap(left, right);
    addRoiFromGraph(left, right);
}

void MainWindow::addRoiFromGraph(int left, int right) {
    if (left > right) std::swap(left, right);
    const int maxCh = maximumSpectrumChannel(
        m_kind->currentIndex() ? SpectrumKind::Integral : SpectrumKind::Peak,
        activeRoiChannel());
    left = qBound(0, left, maxCh);
    right = qBound(0, right, maxCh);
    if (left == right) {
        if (right < maxCh)
            ++right;
        else
            --left;
    }

    const int row = m_roiTable->rowCount();
    {
        const QSignalBlocker blocker(m_roiTable);
        m_roiTable->insertRow(row);
        m_roiTable->setItem(
            row, 0,
            new QTableWidgetItem(QStringLiteral("ROI %1").arg(row + 1)));
        m_roiTable->setItem(
            row, 1, new QTableWidgetItem(QString::number(left)));
        m_roiTable->setItem(
            row, 2, new QTableWidgetItem(QString::number(right)));
    }
    m_roiTable->selectRow(row);

    syncRoisToView();
    storeActiveRoiContext();
}

void MainWindow::deleteSelectedRoi() {
    const int row = m_roiTable->currentRow();
    if (row < 0) return;
    m_roiTable->removeRow(row);
    syncRoisToView();
    storeActiveRoiContext();
}

void MainWindow::clearAllRois() {
    m_roiTable->setRowCount(0);
    m_spectrum->setRois({});
    m_roiInfo->clear();
    storeActiveRoiContext();
}

void MainWindow::modifyRoi(int index, int left, int right) {
    if (index < 0 || index >= m_roiTable->rowCount()) return;
    m_roiTable->item(index, 1)->setText(QString::number(left));
    m_roiTable->item(index, 2)->setText(QString::number(right));
    storeActiveRoiContext();
}

void MainWindow::selectRoi(int index) {
    if (index >= 0 && index < m_roiTable->rowCount()) {
        m_roiTable->selectRow(index);
        storeActiveRoiContext();
    }
}

void MainWindow::syncRoisToView() {
    QVector<Roi> allRois;
    const auto existing = m_spectrum->rois();
    for (int i = 0; i < m_roiTable->rowCount(); ++i) {
        Roi r;
        r.name = m_roiTable->item(i, 0)->text();
        r.left = m_roiTable->item(i, 1)->text().toInt();
        r.right = m_roiTable->item(i, 2)->text().toInt();
        if (i < existing.size())
            r.color = existing[i].color;
        allRois.push_back(r);
    }
    m_spectrum->setRois(allRois);
    if (!m_loadingRoiContext) {
        auto &context = m_roiContexts[size_t(
            roiContextIndex(m_activeRoiChannel, m_activeRoiKind))];
        context.rois = allRois;
        context.selected = m_roiTable->currentRow();
    }
}

void MainWindow::refreshRoiTable() {
    if (m_loadingRoiContext)
        return;
    const int maximum = maximumSpectrumChannel(
        m_activeRoiKind, m_activeRoiChannel);
    const QSignalBlocker blocker(m_roiTable);
    for (int row = 0; row < m_roiTable->rowCount(); ++row) {
        auto *name = m_roiTable->item(row, 0);
        auto *leftItem = m_roiTable->item(row, 1);
        auto *rightItem = m_roiTable->item(row, 2);
        if (!name || !leftItem || !rightItem)
            return;
        if (name->text().trimmed().isEmpty())
            name->setText(QStringLiteral("ROI %1").arg(row + 1));
        int left = qBound(0, leftItem->text().toInt(), maximum);
        int right = qBound(0, rightItem->text().toInt(), maximum);
        if (left > right)
            std::swap(left, right);
        if (left == right) {
            if (right < maximum)
                ++right;
            else
                --left;
        }
        leftItem->setText(QString::number(left));
        rightItem->setText(QString::number(right));
    }
    syncRoisToView();
    storeActiveRoiContext();
    refreshRoiInfo();
}

void MainWindow::refreshRoiInfo() {
    const int row = m_roiTable->currentRow();
    if (row < 0 || !m_snapshot) {
        m_spectrum->setSelectedRoi(-1);
        if (!m_loadingRoiContext)
            m_roiContexts[size_t(roiContextIndex(
                m_activeRoiChannel, m_activeRoiKind))].selected = -1;
        updateRoiAvailability();
        return;
    }
    m_spectrum->setSelectedRoi(row);
    if (!m_loadingRoiContext)
        m_roiContexts[size_t(roiContextIndex(
            m_activeRoiChannel, m_activeRoiKind))].selected = row;

    const int left = m_roiTable->item(row, 1)->text().toInt();
    const int right = m_roiTable->item(row, 2)->text().toInt();
    const auto stats = AnalysisService::roiStats(currentData(), left, right);

    m_roiInfo->setText(QStringLiteral(
        "总计数: %1\n背景: %2\n净计数: %3\n质心: %4\n峰位: %5")
        .arg(stats.gross)
        .arg(stats.background, 0, 'f', 1)
        .arg(stats.net, 0, 'f', 1)
        .arg(stats.centroid, 0, 'f', 3)
        .arg(stats.maximumChannel));
    updateRoiAvailability();
}

// ═══════════════════════════════════════════════════════════════════════════
// Fitting
// ═══════════════════════════════════════════════════════════════════════════

void MainWindow::fitCurrentRoi() {
    const int row = m_roiTable->currentRow();
    if (row < 0) {
        statusBar()->showMessage(QStringLiteral("请先选择一个 ROI"), 3000);
        return;
    }
    fitRoi();
}

void MainWindow::fitRoi() {
    const int row = m_roiTable->currentRow();
    if (row < 0) return;

    const auto data = currentData();
    const int left = m_roiTable->item(row, 1)->text().toInt();
    const int right = m_roiTable->item(row, 2)->text().toInt();
    const int peaks = m_peakCount->value();
    const bool linear = m_background->currentIndex() == 0;

    m_fitInfo->setText(QStringLiteral("拟合中…"));
    const quint64 requestId = ++m_fitRequestId;

    auto *watcher = new QFutureWatcher<FitResult>(this);
    connect(watcher, &QFutureWatcher<FitResult>::finished, this, [this, watcher, requestId] {
        if (requestId != m_fitRequestId) { watcher->deleteLater(); return; }
        const auto result = watcher->result();
        if (result.valid) {
            QString text = QStringLiteral("χ²/ν = %1").arg(result.reducedChiSquare, 0, 'f', 3);
            for (int i = 0; i < result.peaks.size(); ++i)
                text += QStringLiteral("\n峰%1: μ=%2  FWHM=%3\n  面积=%4")
                    .arg(i + 1)
                    .arg(result.peaks[i].centroid, 0, 'f', 3)
                    .arg(result.peaks[i].fwhm, 0, 'f', 3)
                    .arg(result.peaks[i].area, 0, 'f', 1);
            m_fitInfo->setText(text);
            m_spectrum->setFitCurve(result.curve);
        } else {
            m_fitInfo->setText(result.message);
        }
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([=] {
        return AnalysisService::fitGaussian(data, left, right, peaks, linear);
    }));
}

void MainWindow::clearFit() {
    m_spectrum->clearFitCurve();
    m_fitInfo->setText(QStringLiteral("未拟合"));
    ++m_fitRequestId; // cancel pending fits
    updateRoiAvailability();
}

// ═══════════════════════════════════════════════════════════════════════════
// Calibration
// ═══════════════════════════════════════════════════════════════════════════

void MainWindow::calibrate() {
    bool ok = false;
    const QString text = QInputDialog::getMultiLineText(
        this, QStringLiteral("能量标定"),
        QStringLiteral("每行输入：道址,能量\n至少 2 点线性，3 点二次\n\n示例:\n500,511\n1000,1022"),
        QStringLiteral("500,511\n1000,1022"), &ok);
    if (!ok) return;

    QVector<QPointF> points;
    for (const auto &line : text.split('\n')) {
        const auto parts = line.split(',');
        if (parts.size() == 2)
            points.push_back({parts[0].trimmed().toDouble(), parts[1].trimmed().toDouble()});
    }

    const auto order = points.size() >= 3 ? Calibration::Order::Quadratic : Calibration::Order::Linear;
    auto calibration = AnalysisService::calibrate(points, order, &ok);
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("标定失败"), QStringLiteral("标定点不足或矩阵不可解"));
        return;
    }

    if (m_kind->currentIndex())
        m_integralCal = calibration;
    else
        m_peakCal = calibration;

    m_spectrum->setCalibration(calibration);
    for (auto *view : m_gridViews) view->setCalibration(calibration);

    QMessageBox::information(this, QStringLiteral("标定完成"),
        QStringLiteral("E = %1 + %2·ch + %3·ch² %4")
            .arg(calibration.coefficients[0], 0, 'g', 6)
            .arg(calibration.coefficients[1], 0, 'g', 6)
            .arg(calibration.coefficients[2], 0, 'g', 6)
            .arg(calibration.unit));
}

// ═══════════════════════════════════════════════════════════════════════════
// File I/O
// ═══════════════════════════════════════════════════════════════════════════

QVector<quint64> MainWindow::currentData() const {
    if (!m_snapshot) return {};
    const int channel = m_activeRoiChannel;
    return m_kind->currentIndex()
        ? m_snapshot->integral.value(channel)
        : m_snapshot->peak.value(channel);
}

int MainWindow::maximumSpectrumChannel(SpectrumKind kind, int channel) const {
    if (m_snapshot && channel >= 0 && channel < m_snapshot->channelCount) {
        const auto &spectra = kind == SpectrumKind::Peak
            ? m_snapshot->peak : m_snapshot->integral;
        if (channel < spectra.size() && !spectra[channel].isEmpty())
            return spectra[channel].size() - 1;
    }
    return (kind == SpectrumKind::Peak
        ? DefaultPeakBins : DefaultIntegralBins) - 1;
}

void MainWindow::saveSpectrum() {
    if (!m_snapshot) return;
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存能谱"), m_lastDirectory,
        QStringLiteral("MCA Spectrum (*.mcaspec)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".mcaspec"), Qt::CaseInsensitive))
        path += QStringLiteral(".mcaspec");

    QString error;
    if (!SpectrumFile::writeV1(path, *m_snapshot, &error)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), error);
        return;
    }
    m_lastDirectory = QFileInfo(path).absolutePath();
    addRecentFile(path);
    statusBar()->showMessage(QStringLiteral("能谱已保存"), 3000);
}

void MainWindow::openSpectrum() {
    if (m_acquisitionState == AcquisitionState::Running ||
        m_acquisitionState == AcquisitionState::Paused ||
        m_acquisitionState == AcquisitionState::Stopping) {
        QMessageBox::information(this, QStringLiteral("正在采集"),
                                 QStringLiteral("请先停止采集，再打开静态能谱。"));
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开能谱"), m_lastDirectory,
        QStringLiteral("MCA Spectrum (*.mcaspec)"));
    if (path.isEmpty())
        return;
    QString error;
    const auto snapshot = SpectrumFile::readV1(path, &error);
    if (!snapshot) {
        QMessageBox::warning(this, QStringLiteral("打开失败"), error);
        return;
    }
    m_lastDirectory = QFileInfo(path).absolutePath();
    addRecentFile(path);
    ensureChannelRows(snapshot->channelCount);
    const int automaticIndex = m_acquisitionMode->findData(
        int(AcquisitionMode::AutomaticTrigger));
    if (automaticIndex >= 0)
        m_acquisitionMode->setCurrentIndex(automaticIndex);
    updateSpectrum(snapshot);
    statusBar()->showMessage(
        QStringLiteral("已打开静态能谱：%1").arg(QFileInfo(path).fileName()),
        4000);
}

void MainWindow::openList() {
    if (m_acquisitionState == AcquisitionState::Running ||
        m_acquisitionState == AcquisitionState::Paused ||
        m_acquisitionState == AcquisitionState::Stopping) {
        QMessageBox::information(this, QStringLiteral("正在采集"),
                                 QStringLiteral("请先停止采集，再回放历史 List。"));
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开 List"), m_lastDirectory,
        QStringLiteral("MCA List (*.mcalist)"));
    if (!path.isEmpty()) {
        m_lastDirectory = QFileInfo(path).absolutePath();
        addRecentFile(path);
        m_acq.replay(path);
    }
}

void MainWindow::addRecentFile(const QString &path) {
    const QString absolute = QFileInfo(path).absoluteFilePath();
    m_recentFiles.removeAll(absolute);
    m_recentFiles.prepend(absolute);
    while (m_recentFiles.size() > MaxRecentFiles)
        m_recentFiles.removeLast();
    rebuildRecentFilesMenu();
}

void MainWindow::rebuildRecentFilesMenu() {
    if (!m_recentFilesMenu)
        return;
    m_recentFilesMenu->clear();
    if (m_recentFiles.isEmpty()) {
        auto *empty = m_recentFilesMenu->addAction(QStringLiteral("（无）"));
        empty->setEnabled(false);
        empty->setToolTip(QStringLiteral("尚无最近文件"));
        return;
    }
    for (const QString &path : std::as_const(m_recentFiles)) {
        auto *action = m_recentFilesMenu->addAction(
            QFileInfo(path).fileName());
        action->setToolTip(path);
        connect(action, &QAction::triggered, this, [this, path] {
            if (m_acquisitionState == AcquisitionState::Running ||
                m_acquisitionState == AcquisitionState::Paused ||
                m_acquisitionState == AcquisitionState::Stopping) {
                QMessageBox::information(
                    this, QStringLiteral("正在采集"),
                    QStringLiteral("请先停止采集，再打开历史文件。"));
                return;
            }
            if (!QFileInfo::exists(path)) {
                m_recentFiles.removeAll(path);
                rebuildRecentFilesMenu();
                QMessageBox::warning(this, QStringLiteral("文件不存在"), path);
                return;
            }
            m_lastDirectory = QFileInfo(path).absolutePath();
            if (path.endsWith(QStringLiteral(".mcalist"),
                              Qt::CaseInsensitive)) {
                m_acq.replay(path);
                return;
            }
            QString error;
            const auto snapshot = SpectrumFile::readV1(path, &error);
            if (!snapshot) {
                QMessageBox::warning(this, QStringLiteral("打开失败"), error);
                return;
            }
            ensureChannelRows(snapshot->channelCount);
            const int automaticIndex = m_acquisitionMode->findData(
                int(AcquisitionMode::AutomaticTrigger));
            if (automaticIndex >= 0)
                m_acquisitionMode->setCurrentIndex(automaticIndex);
            updateSpectrum(snapshot);
            addRecentFile(path);
        });
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Window layout
// ═══════════════════════════════════════════════════════════════════════════

void MainWindow::restoreDefaultLayout() {
    applyDefaultDockLayout();
    m_spectrum->resetZoom();
    m_spectrum->setInteractionMode(SpectrumInteractionMode::Cursor);
    m_actCursorMode->setChecked(true);
    m_layout->setCurrentIndex(0);
    m_kind->setCurrentIndex(0);
    statusBar()->showMessage(QStringLiteral("已恢复默认布局"), 3000);
}

void MainWindow::applyDefaultDockLayout() {
    resize(1500, 900);
    // The live single-channel layout has no legacy module/pair docks.  It is
    // already arranged by buildDockWidgets; avoid replaying a stale layout
    // transaction against the removed docks.
    if (!m_moduleDock && !m_pairDock) {
        m_connectionDock->show();
        m_channelDock->show();
        m_acquisitionDock->show();
        m_analysisDock->show();
        resizeDocks({m_channelDock}, {350}, Qt::Horizontal);
        resizeDocks({m_analysisDock}, {240}, Qt::Horizontal);
        return;
    }
    for (auto *dock : {m_connectionDock, m_channelDock, m_acquisitionDock,
                       m_moduleDock, m_pairDock})
        if (dock)
            addDockWidget(Qt::LeftDockWidgetArea, dock);
    addDockWidget(Qt::RightDockWidgetArea, m_analysisDock);
    m_connectionDock->show();
    m_channelDock->show();
    m_acquisitionDock->show();
    m_analysisDock->show();
    if (m_moduleDock)
        m_moduleDock->hide();
    if (m_pairDock)
        m_pairDock->hide();
    tabifyDockWidget(m_connectionDock, m_channelDock);
    tabifyDockWidget(m_channelDock, m_acquisitionDock);
    m_channelDock->raise();
    resizeDocks({m_channelDock}, {300}, Qt::Horizontal);
    resizeDocks({m_analysisDock}, {240}, Qt::Horizontal);
    updateModeDependentPanels();
}

void MainWindow::updateModeDependentPanels() {
    if (!m_acquisitionMode)
        return;
    const auto mode = AcquisitionMode(
        m_acquisitionMode->currentData().toInt());
    const bool usesModules = mode == AcquisitionMode::LegacyFourChannel ||
                             mode == AcquisitionMode::LegacyCoincidence;
    const bool usesPairs = mode == AcquisitionMode::LegacyCoincidence;
    if (m_moduleDock)
        m_moduleDock->setVisible(usesModules);
    if (m_pairDock)
        m_pairDock->setVisible(usesPairs);
    if (usesPairs && m_pairDock)
        m_pairDock->raise();
    else if (usesModules && m_moduleDock)
        m_moduleDock->raise();
    // The waveform page keeps the shared threshold, polarity and baseline
    // controls visible, so the channel dock stays available; only the module
    // and pair docks are mode specific.
    if (m_kind)
        m_kind->setEnabled(mode != AcquisitionMode::Waveform && mode!=AcquisitionMode::Psd);
    if (m_layout)
        m_layout->setEnabled(mode != AcquisitionMode::Waveform);
    if (m_externalDurationUs)
        m_externalDurationUs->setVisible(mode == AcquisitionMode::ExternalTrigger);
}

void MainWindow::ensureWindowOnScreen() {
    const QRect windowRect = frameGeometry();
    for (const auto *screen : QGuiApplication::screens()) {
        if (screen->availableGeometry().intersects(windowRect))
            return;
    }
    if (const auto *screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        resize(qMin(width(), available.width()),
               qMin(height(), available.height()));
        move(available.topLeft() + QPoint(24, 24));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Settings persistence
// ═══════════════════════════════════════════════════════════════════════════

void MainWindow::saveSettings() {
    QSettings settings;
    settings.setValue("uiV6/geometry", saveGeometry());
    settings.setValue("uiV6/windowState", saveState(3));
    settings.setValue("recentFiles", m_recentFiles);
    settings.setValue("lastDirectory", m_lastDirectory);
    settings.setValue("display/overview", m_spectrum->isOverviewVisible());
    settings.setValue("display/fillMode", m_spectrum->fillMode());
    settings.setValue("display/legend", m_spectrum->isLegendVisible());
    settings.setValue("display/grid", m_spectrum->isGridVisible());
    settings.setValue("display/roiLabels", m_spectrum->areRoiLabelsVisible());
    settings.setValue("display/logY", m_actLogY->isChecked());
    if (auto *ip = findChild<QLineEdit *>("deviceIp"))
        settings.setValue("device/ip", ip->text());
    if (auto *port = findChild<QSpinBox *>("dataPort"))
        settings.setValue("device/dataPort", port->value());
    if (auto *port = findChild<QSpinBox *>("controlPort"))
        settings.setValue("device/controlPort", port->value());
    settings.setValue("acquisition/integrationPostUs",
                      m_integrationPostUs->value());
    settings.setValue("acquisition/integrationPreUs",
                      m_integrationPreUs->value());
    settings.setValue("acquisition/externalDurationUs",
                      m_externalDurationUs ? m_externalDurationUs->value() : 1000);
    settings.setValue("acquisition/peakInterpolation",
                      m_peakInterpolation && m_peakInterpolation->isChecked());
}

void MainWindow::restoreSettings() {
    QSettings settings;
    m_recentFiles = settings.value("recentFiles").toStringList();
    m_lastDirectory = settings.value("lastDirectory").toString();
    rebuildRecentFilesMenu();
    const auto geom = settings.value("uiV6/geometry").toByteArray();
    const auto state = settings.value("uiV6/windowState").toByteArray();
    if (!geom.isEmpty())
        restoreGeometry(geom);
    if (state.isEmpty() || !restoreState(state, 3))
        applyDefaultDockLayout();
    ensureWindowOnScreen();
    const bool overview = settings.value("display/overview", true).toBool();
    const bool fill = settings.value("display/fillMode", true).toBool();
    const bool legend = settings.value("display/legend", true).toBool();
    const bool grid = settings.value("display/grid", true).toBool();
    const bool roiLabels =
        settings.value("display/roiLabels", true).toBool();
    const bool logY = settings.value("display/logY", false).toBool();
    m_spectrum->setOverviewVisible(overview);
    m_actOverview->setChecked(overview);
    m_spectrum->setFillMode(fill);
    for (auto *view : m_gridViews)
        view->setFillMode(fill);
    m_actFill->setChecked(fill);
    m_spectrum->setLegendVisible(legend);
    m_actLegend->setChecked(legend);
    m_spectrum->setGridVisible(grid);
    m_actGrid->setChecked(grid);
    m_spectrum->setRoiLabelsVisible(roiLabels);
    m_actRoiLabels->setChecked(roiLabels);
    m_actLogY->setChecked(logY);
    m_actLinearY->setChecked(!logY);
    applyDisplaySettings();
    if (auto *ip = findChild<QLineEdit *>("deviceIp"))
        ip->setText(settings.value("device/ip", ip->text()).toString());
    if (auto *port = findChild<QSpinBox *>("dataPort"))
        port->setValue(settings.value("device/dataPort", port->value()).toInt());
    if (auto *port = findChild<QSpinBox *>("controlPort"))
        port->setValue(
            settings.value("device/controlPort", port->value()).toInt());
    m_integrationPostUs->setValue(
        settings.value("acquisition/integrationPostUs",
                       m_integrationPostUs->value()).toDouble());
    m_integrationPreUs->setValue(
        settings.value("acquisition/integrationPreUs",
                       m_integrationPreUs->value()).toDouble());
    if (m_externalDurationUs)
        m_externalDurationUs->setValue(qBound(
            1, settings.value("acquisition/externalDurationUs", 1000).toInt(),
            1'000'000));
    if (m_peakInterpolation)
        m_peakInterpolation->setChecked(
            settings.value("acquisition/peakInterpolation", false).toBool());
    updateModeDependentPanels();
}

} // namespace mca

#include "ui/mainwindow.moc"
