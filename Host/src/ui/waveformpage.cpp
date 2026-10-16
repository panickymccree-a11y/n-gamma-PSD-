#include "ui/waveformpage.h"
#include "core/protocol.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

namespace mca {
namespace {

QString stateName(WaveformState state) {
    switch (state) {
    case WaveformState::Stopped:    return QStringLiteral("已停止");
    case WaveformState::Filling:    return QStringLiteral("填充前窗");
    case WaveformState::Waiting:    return QStringLiteral("等待触发");
    case WaveformState::Capturing:  return QStringLiteral("捕获后窗");
    case WaveformState::Uploading:  return QStringLiteral("上传中");
    case WaveformState::SingleDone: return QStringLiteral("单次完成");
    }
    return QStringLiteral("未知");
}

QString timebaseLabel(double ns) {
    if (ns >= 1000.0)
        return QStringLiteral("%1 μs").arg(ns / 1000.0, 0, 'g', 4);
    return QStringLiteral("%1 ns").arg(ns, 0, 'g', 4);
}

} // namespace

WaveformPage::WaveformPage(QWidget *parent) : QWidget(parent) {
    buildUi();
    setStatus(WaveformStatus{});
}

void WaveformPage::buildUi() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(6);

    // ---------------- control row ----------------
    auto *controls = new QGroupBox(QStringLiteral("波形采集"));
    auto *controlLayout = new QHBoxLayout(controls);
    m_threshold = new QSpinBox;
    m_threshold->setRange(3, 16383);
    m_threshold->setValue(64);
    m_threshold->setToolTip(
        QStringLiteral("相对锁存基线的正幅度阈值 H（14 位 ADC 码）；"
                       "点击开始时提交到 FPGA"));
    m_polarity = new QComboBox;
    m_polarity->addItems({QStringLiteral("正极性"),
                          QStringLiteral("负极性")});
    m_triggerMode = new QComboBox;
    m_triggerMode->addItems({QStringLiteral("连续"),
                             QStringLiteral("单次")});
    m_start = new QPushButton(QStringLiteral("开始"));
    m_stop = new QPushButton(QStringLiteral("停止"));
    controlLayout->addWidget(new QLabel(QStringLiteral("阈值 H")));
    controlLayout->addWidget(m_threshold);
    controlLayout->addSpacing(8);
    controlLayout->addWidget(new QLabel(QStringLiteral("极性")));
    controlLayout->addWidget(m_polarity);
    controlLayout->addSpacing(8);
    controlLayout->addWidget(new QLabel(QStringLiteral("触发")));
    controlLayout->addWidget(m_triggerMode);
    controlLayout->addStretch(1);
    controlLayout->addWidget(m_start);
    controlLayout->addWidget(m_stop);
    root->addWidget(controls);

    // ---------------- display ----------------
    m_view = new WaveformView;
    root->addWidget(m_view, 1);

    // ---------------- tools row ----------------
    auto *tools = new QGroupBox(QStringLiteral("显示"));
    auto *toolLayout = new QHBoxLayout(tools);
    m_timebase = new QComboBox;
    for (double step : WaveformView::timebaseSteps())
        m_timebase->addItem(timebaseLabel(step), step);
    m_timebase->setCurrentIndex(WaveformView::timebaseSteps().size() - 1);
    m_vertical = new QComboBox;
    for (int step : WaveformView::adcSteps())
        m_vertical->addItem(QStringLiteral("%1 ADC/div").arg(step), step);
    m_vertical->setCurrentIndex(WaveformView::adcSteps().size() - 1);
    m_horizontal = new QSlider(Qt::Horizontal);
    m_horizontal->setRange(0, 1000);
    m_horizontal->setToolTip(QStringLiteral("水平位置"));
    m_verticalOffset = new QSlider(Qt::Horizontal);
    m_verticalOffset->setRange(0, 1000);
    m_verticalOffset->setToolTip(QStringLiteral("垂直位置"));
    m_autoScale = new QCheckBox(QStringLiteral("Y 自适应"));
    m_fullView = new QPushButton(QStringLiteral("全视图"));
    m_export = new QPushButton(QStringLiteral("导出 CSV"));
    m_export->setEnabled(false);
    toolLayout->addWidget(new QLabel(QStringLiteral("Time/div")));
    toolLayout->addWidget(m_timebase);
    toolLayout->addWidget(new QLabel(QStringLiteral("ADC/div")));
    toolLayout->addWidget(m_vertical);
    toolLayout->addWidget(new QLabel(QStringLiteral("水平")));
    toolLayout->addWidget(m_horizontal, 1);
    toolLayout->addWidget(new QLabel(QStringLiteral("垂直")));
    toolLayout->addWidget(m_verticalOffset, 1);
    toolLayout->addWidget(m_autoScale);
    toolLayout->addWidget(m_fullView);
    toolLayout->addWidget(m_export);
    root->addWidget(tools);

    // ---------------- status row ----------------
    auto *status = new QWidget;
    auto *statusLayout = new QHBoxLayout(status);
    statusLayout->setContentsMargins(2, 0, 2, 0);
    m_state = new QLabel;
    m_counts = new QLabel;
    m_captureInfo = new QLabel;
    m_capability = new QLabel;
    m_capability->setProperty("state", QStringLiteral("error"));
    statusLayout->addWidget(m_state);
    statusLayout->addSpacing(12);
    statusLayout->addWidget(m_counts);
    statusLayout->addSpacing(12);
    statusLayout->addWidget(m_captureInfo, 1);
    statusLayout->addWidget(m_capability);
    root->addWidget(status);

    connect(m_start, &QPushButton::clicked,
            this, &WaveformPage::onStartClicked);
    connect(m_stop, &QPushButton::clicked,
            this, &WaveformPage::onStopClicked);
    connect(m_export, &QPushButton::clicked,
            this, &WaveformPage::exportRequested);
    // Threshold and polarity are staged locally.  Starting acquisition sends
    // the complete configuration once; editing a spin box must not silently
    // run a STOP/COMMIT/START transaction.
    connect(m_triggerMode, &QComboBox::currentIndexChanged,
            this, [this](int) {
                emit triggerModeChanged(triggerMode());
                refreshStateText();
            });
    connect(m_timebase, &QComboBox::currentIndexChanged, this, [this](int) {
        m_view->setTimebaseNsPerDiv(m_timebase->currentData().toDouble());
    });
    connect(m_vertical, &QComboBox::currentIndexChanged, this, [this](int) {
        m_view->setAdcPerDiv(m_vertical->currentData().toInt());
    });
    connect(m_autoScale, &QCheckBox::toggled, this, [this](bool on) {
        m_view->setAutoScale(on);
        if (on) {
            m_vertical->setEnabled(false);
            m_verticalOffset->setEnabled(false);
        } else {
            m_vertical->setEnabled(true);
            m_verticalOffset->setEnabled(true);
        }
    });
    connect(m_fullView, &QPushButton::clicked, this, [this] {
        m_view->showFullRecord();
        const QSignalBlocker timebaseBlocker(m_timebase);
        const QSignalBlocker verticalBlocker(m_vertical);
        m_timebase->setCurrentIndex(WaveformView::timebaseSteps().size() - 1);
        m_vertical->setCurrentIndex(WaveformView::adcSteps().size() - 1);
        m_autoScale->setChecked(false);
        m_horizontal->setValue(0);
        m_verticalOffset->setValue(0);
    });
    connect(m_horizontal, &QSlider::valueChanged, this, [this](int value) {
        if (m_updating)
            return;
        auto capture = m_view->capture();
        if (!capture || !capture->valid())
            return;
        const double span = m_view->timebaseNsPerDiv() * 10.0
                          / double(capture->samplePeriodNs);
        const double recordLast = double(capture->size() - 1);
        const double maximum = qMax(0.0, recordLast - span + 1.0);
        m_view->setHorizontalPosition(maximum * value / 1000.0);
    });
    connect(m_verticalOffset, &QSlider::valueChanged, this, [this](int value) {
        if (m_updating)
            return;
        const int visible = m_view->adcPerDiv() * 8;
        const int maximum = qMax(0, WaveformAdcFullScale - visible);
        m_view->setVerticalPosition(maximum * value / 1000);
    });
    connect(m_view, &WaveformView::captureInfoChanged,
            this, [this](const QString &text) {
                m_captureInfo->setText(text);
            });
    connect(m_view, &WaveformView::cursorMoved, this,
            [this](int index, int adc, qint64 timeNs, bool overrange) {
                m_captureInfo->setText(
                    QStringLiteral("i=%1  t=%2 ns  ADC=%3%4")
                        .arg(index).arg(timeNs).arg(adc)
                        .arg(overrange ? QStringLiteral("  过载") : QString()));
            });

    updateControlState();
}

ChannelConfig WaveformPage::channelConfig() const {
    ChannelConfig config;
    config.enabled = true;
    config.thresholdLsb = quint16(m_threshold->value());
    config.polarity = m_polarity->currentIndex() == 1
                          ? Polarity::Negative : Polarity::Positive;
    return normalizeChannelConfig(config);
}

void WaveformPage::setChannelConfig(const ChannelConfig &config) {
    m_updating = true;
    m_threshold->setValue(config.thresholdLsb);
    m_polarity->setCurrentIndex(config.polarity == Polarity::Negative ? 1 : 0);
    m_updating = false;
}

WaveformTriggerMode WaveformPage::triggerMode() const {
    return m_triggerMode->currentIndex() == 1 ? WaveformTriggerMode::Single
                                              : WaveformTriggerMode::Continuous;
}

void WaveformPage::onStartClicked() {
    if (!m_supported)
        return;
    emit startRequested(triggerMode(), channelConfig());
}

void WaveformPage::onStopClicked() {
    emit stopRequested();
}

void WaveformPage::applyPendingConfig() {
    if (m_updating)
        return;
    emit configChanged(channelConfig());
}

void WaveformPage::setCapture(std::shared_ptr<const WaveformCapture> capture) {
    m_view->setCapture(std::move(capture));
    m_export->setEnabled(m_view->capture() && m_view->capture()->valid());
    updateControlState();
}

void WaveformPage::setStatus(const WaveformStatus &status) {
    m_status = status;
    refreshStateText();
}

void WaveformPage::refreshStateText() {
    QString text = QStringLiteral("状态：%1").arg(stateName(m_status.state));
    if (m_status.receiveIncomplete)
        text += QStringLiteral("（接收不完整）");
    if (!m_status.detail.isEmpty())
        text += QStringLiteral(" — %1").arg(m_status.detail);
    m_state->setText(text);
    m_counts->setText(
        QStringLiteral("完整波形 %1    丢弃 %2    片段丢失 %3")
            .arg(m_status.completedCaptures)
            .arg(m_status.discardedCaptures)
            .arg(m_status.droppedFragments));
}

void WaveformPage::setRunning(bool running) {
    m_running = running;
    updateControlState();
}

void WaveformPage::setBusy(bool busy) {
    m_busy = busy;
    updateControlState();
}

void WaveformPage::setCapability(bool supported, const QString &reason) {
    m_supported = supported;
    m_capability->setText(supported ? QString() : reason);
    m_capability->setVisible(!supported);
    updateControlState();
}

void WaveformPage::updateControlState() {
    const bool canStart = m_supported && !m_running && !m_busy;
    m_start->setEnabled(canStart);
    m_stop->setEnabled(m_running);
    // The shared threshold and polarity may only be changed while stopped.
    m_threshold->setEnabled(!m_running && !m_busy && m_supported);
    m_polarity->setEnabled(!m_running && !m_busy && m_supported);
    m_export->setEnabled(m_view->capture() && m_view->capture()->valid()
                         && !m_busy);
}

} // namespace mca
