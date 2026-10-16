#include "ui/mainwindow.h"
#include "ui/spectrumview.h"
#include "ui/waveformpage.h"
#include "ui/waveformview.h"
#include <QAction>
#include <QChartView>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QGridLayout>
#include <QHeaderView>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QtTest>

using namespace mca;

class UiTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName(QStringLiteral("MCA-Test"));
        QCoreApplication::setApplicationName(QStringLiteral("MCA-UI-Test"));
        QSettings().clear();
        QFile qss(QStringLiteral(":/mca.qss"));
        QVERIFY(qss.open(QIODevice::ReadOnly | QIODevice::Text));
        qApp->setStyleSheet(QString::fromUtf8(qss.readAll()));
    }

    void waveformPageExistsAndDefaultsAreSafe() {
        MainWindow window;
        window.show();
        QTest::qWait(20);
        auto *page = window.findChild<WaveformPage *>();
        QVERIFY(page);
        QVERIFY(page->capture() == nullptr);
    }

    void waveformViewFollowsTheFixedGeometry() {
        WaveformView view;
        view.resize(900, 400);
        view.show();
        QTest::qWait(20);

        // Build one complete record without going through the network.
        auto capture = std::make_shared<WaveformCapture>();
        capture->totalSamples = WaveformSampleCount;
        capture->preSamples = WaveformPreSamples;
        capture->samplePeriodNs = WaveformSamplePeriodNs;
        capture->baseline = 8192;
        capture->threshold = 200;
        capture->samples.resize(WaveformSampleCount);
        for (int i = 0; i < WaveformSampleCount; ++i)
            capture->samples[i] = quint16(8192 + (i % 64));
        capture->samples[WaveformPreSamples] = 12'000;
        view.setCapture(capture);
        QVERIFY(view.capture() == capture);
        QCOMPARE(view.timebaseNsPerDiv(), 10'000.0);
        QCOMPARE(view.adcPerDiv(), 2048);

        // The timebase list is exactly the documented one and always spans ten
        // divisions, so the full view is 100 us and the finest view 80 ns.
        const auto &steps = WaveformView::timebaseSteps();
        QCOMPARE(steps.size(), 11);
        QCOMPARE(steps.first(), 8.0);
        QCOMPARE(steps.first() * 10.0, 80.0);
        QCOMPARE(steps.last(), 10'000.0);
        QCOMPARE(steps.last() * 10.0, 100'000.0);
        for (double step : steps)
            QVERIFY(step * 10.0 >= 80.0);
        for (double step : steps)
            QVERIFY(step * 10.0 <= 100'000.0);

        const auto &adcSteps = WaveformView::adcSteps();
        QCOMPARE(adcSteps.first(), 1);
        QCOMPARE(adcSteps.last(), 2048);
        QCOMPARE(adcSteps.last() * 8, 16384);

        // Zooming in keeps the trigger inside the record and never invents
        // samples outside it.
        view.setTimebaseNsPerDiv(8.0);
        QCOMPARE(view.timebaseNsPerDiv(), 8.0);
        view.setTimebaseNsPerDiv(8.0);
        QCOMPARE(view.timebaseNsPerDiv(), 8.0);

        // Panning to the extremes stays inside the record.
        view.setHorizontalPosition(-1.0e9);
        view.setHorizontalPosition(1.0e9);
        view.showFullRecord();
        QCOMPARE(view.timebaseNsPerDiv(), 10'000.0);
        QCOMPARE(view.adcPerDiv(), 2048);

        // Y auto scale picks a fitting ADC/div step and never leaves the ADC
        // display range.
        view.setAutoScale(true);
        QVERIFY(view.autoScale());
        const int step = view.adcPerDiv();
        QVERIFY(step >= 1 && step <= 2048);
        QVERIFY(view.verticalPosition() >= 0);
        QVERIFY(view.verticalPosition() + step * 8
                <= WaveformAdcFullScale);

        // Vertical steps snap to the documented list.
        view.setAdcPerDiv(100);
        QCOMPARE(view.adcPerDiv(), 64);
        view.setAdcPerDiv(1'000'000);
        QCOMPARE(view.adcPerDiv(), 2048);
        view.setVerticalPosition(-100);
        QVERIFY(view.verticalPosition() >= 0);
        view.setVerticalPosition(100'000);
        QVERIFY(view.verticalPosition() + view.adcPerDiv() * 8
                <= WaveformAdcFullScale);

        // The widget paints without a capture and with an incomplete one.
        view.setCapture(nullptr);
        view.repaint();
        auto partial = std::make_shared<WaveformCapture>(*capture);
        partial->samples.resize(10);
        view.setCapture(partial);
        view.repaint();
        view.setCapture(capture);
        view.repaint();
        QVERIFY(!view.grab().isNull());
    }

    void docksAndProductActionsHaveSafeDefaults() {
        MainWindow window;
        window.show();
        QTest::qWait(20);
        auto *connection =
            window.findChild<QDockWidget *>(QStringLiteral("connectionDockV2"));
        auto *channel =
            window.findChild<QDockWidget *>(QStringLiteral("channelDockV2"));
        QVERIFY(connection);
        QVERIFY(channel);
        auto *channelTable = window.findChild<QTableWidget *>(
            QStringLiteral("channelTable"));
        QVERIFY(channelTable);
        QCOMPARE(channelTable->item(0, 1)->text(), QStringLiteral("ADC1-A"));
        QCOMPARE(channelTable->item(7, 1)->text(), QStringLiteral("ADC1-H"));
        QVERIFY(!window.findChild<QDockWidget *>(QStringLiteral("moduleDockV2")));
        QVERIFY(!window.findChild<QDockWidget *>(QStringLiteral("pairDockV2")));

        QComboBox *mode = nullptr;
        for (auto *combo : window.findChildren<QComboBox *>()) {
            if (combo->findText(QStringLiteral("自动触发")) >= 0 &&
                combo->findText(QStringLiteral("外置触发")) >= 0) {
                mode = combo;
                break;
            }
        }
        QVERIFY(mode);
        QCOMPARE(mode->count(), 4);
        QCOMPARE(mode->itemData(3).toInt(),int(AcquisitionMode::Psd));
        QCOMPARE(mode->itemData(0).toInt(), int(AcquisitionMode::AutomaticTrigger));
        QCOMPARE(mode->itemData(1).toInt(), int(AcquisitionMode::ExternalTrigger));
        QCOMPARE(mode->itemData(2).toInt(), int(AcquisitionMode::Waveform));
        auto *duration = window.findChild<QSpinBox *>(
            QStringLiteral("externalDurationUsSpinBox"));
        QVERIFY(duration);
        QCOMPARE(duration->minimum(), 1);
        QCOMPARE(duration->maximum(), 1'000'000);
        QCOMPARE(duration->value(), 1000);

        auto *analysis = window.findChild<QDockWidget *>(
            QStringLiteral("analysisDockV2"));
        auto *statusPanel = window.findChild<QWidget *>(
            QStringLiteral("acquisitionStatusPanel"));
        auto *roiEntry = window.findChild<QWidget *>(
            QStringLiteral("roiEntryGroup"));
        QVERIFY(analysis);
        QVERIFY(statusPanel);
        QVERIFY(roiEntry);
        QCOMPARE(statusPanel->parentWidget(), analysis->widget());
        QVERIFY(statusPanel->geometry().top() < roiEntry->geometry().top());

        auto *thresholdBox = window.findChild<QWidget *>(
            QStringLiteral("globalThresholdBox"));
        auto *thresholdLayout = thresholdBox
            ? qobject_cast<QGridLayout *>(thresholdBox->layout()) : nullptr;
        QVERIFY(thresholdBox);
        QVERIFY(thresholdLayout);
        QVERIFY(thresholdLayout->rowCount() >= 2);

        for (auto *action : window.findChildren<QAction *>()) {
            QVERIFY2(!action->text().contains(QStringLiteral("模拟采集")),
                     qPrintable(action->text()));
        }
    }

    void roisAreIsolatedByChannelAndSpectrumKind() {
        MainWindow window;
        window.show();
        QTest::qWait(20);

        QComboBox *kind = nullptr;
        for (auto *combo : window.findChildren<QComboBox *>()) {
            if (combo->findText(QStringLiteral("峰值谱 8192 道")) >= 0) {
                kind = combo;
                break;
            }
        }
        QTableWidget *channels = nullptr;
        QTableWidget *rois = nullptr;
        for (auto *table : window.findChildren<QTableWidget *>()) {
            if (table->columnCount() == 8)
                channels = table;
            else if (table->columnCount() == 3)
                rois = table;
        }
        QVERIFY(kind);
        QVERIFY(channels);
        QVERIFY(rois);

        channels->selectRow(0);
        QVERIFY(QMetaObject::invokeMethod(
            &window, "addRoiFromGraph", Q_ARG(int, 100), Q_ARG(int, 200)));
        QCOMPARE(rois->rowCount(), 1);

        kind->setCurrentIndex(1);
        QCoreApplication::processEvents();
        QCOMPARE(rois->rowCount(), 0);
        QVERIFY(QMetaObject::invokeMethod(
            &window, "addRoiFromGraph", Q_ARG(int, 1000), Q_ARG(int, 1200)));
        QCOMPARE(rois->rowCount(), 1);

        kind->setCurrentIndex(0);
        QCoreApplication::processEvents();
        QCOMPARE(rois->rowCount(), 1);
        QCOMPARE(rois->item(0, 1)->text(), QStringLiteral("100"));

        QTest::mouseClick(
            channels->viewport(), Qt::LeftButton, Qt::NoModifier,
            channels->visualItemRect(channels->item(1, 1)).center());
        QCoreApplication::processEvents();
        QCOMPARE(rois->rowCount(), 0);

        QTest::mouseClick(
            channels->viewport(), Qt::LeftButton, Qt::NoModifier,
            channels->visualItemRect(channels->item(0, 1)).center());
        kind->setCurrentIndex(0);
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(
            &window, "addRoiFromGraph", Q_ARG(int, 8191), Q_ARG(int, 8191)));
        QCOMPARE(rois->item(1, 1)->text(), QStringLiteral("8190"));
        QCOMPARE(rois->item(1, 2)->text(), QStringLiteral("8191"));
    }

    void modePanelsAndStatusIndicatorsAreContextual() {
        MainWindow window;
        window.show();
        QTest::qWait(20);

        auto *deviceStatus =
            window.findChild<QLabel *>(QStringLiteral("deviceStatusBadge"));
        auto *acquisitionStatus = window.findChild<QLabel *>(
            QStringLiteral("acquisitionStatusBadge"));
        auto *baselineApply = window.findChild<QPushButton *>(
            QStringLiteral("baselineApplyButton"));
        QVERIFY(deviceStatus);
        QVERIFY(acquisitionStatus);
        QVERIFY(baselineApply);
        QCOMPARE(deviceStatus->property("state").toString(),
                 QStringLiteral("offline"));
        QCOMPARE(acquisitionStatus->property("state").toString(),
                 QStringLiteral("idle"));

        QComboBox *mode = nullptr;
        for (auto *combo : window.findChildren<QComboBox *>()) {
            if (combo->findText(QStringLiteral("自动触发")) >= 0 &&
                combo->findText(QStringLiteral("外置触发")) >= 0) {
                mode = combo;
                break;
            }
        }
        QVERIFY(mode);
        QCOMPARE(mode->itemData(mode->currentIndex()).toInt(),
                 int(AcquisitionMode::AutomaticTrigger));
        mode->setCurrentIndex(1);
        QCoreApplication::processEvents();
        QCOMPARE(mode->currentData().toInt(), int(AcquisitionMode::ExternalTrigger));
        auto *duration = window.findChild<QSpinBox *>(
            QStringLiteral("externalDurationUsSpinBox"));
        QVERIFY(duration);
        QVERIFY(duration->isVisible());
        mode->setCurrentIndex(2);
        QCoreApplication::processEvents();
        QCOMPARE(mode->currentData().toInt(), int(AcquisitionMode::Waveform));
        mode->setCurrentIndex(0);
        QCoreApplication::processEvents();
        QCOMPARE(mode->currentData().toInt(), int(AcquisitionMode::AutomaticTrigger));
        QVERIFY(!duration->isVisible());
    }

    void channelInlineControlsRemainReadable() {
        MainWindow window;
        window.resize(1366, 768);
        window.show();
        QTest::qWait(20);

        QTableWidget *channels = nullptr;
        for (auto *table : window.findChildren<QTableWidget *>()) {
            if (table->columnCount() == 8) {
                channels = table;
                break;
            }
        }
        QVERIFY(channels);
        QCOMPARE(channels->horizontalScrollBarPolicy(),
                 Qt::ScrollBarAlwaysOff);
        QVERIFY(channels->columnWidth(2) >= 94);
        QVERIFY(channels->columnWidth(4) >= 54);
        for (int row = 0; row < channels->rowCount(); ++row) {
            auto *thresholdPanel = channels->cellWidget(row, 2);
            QVERIFY(thresholdPanel);
            auto *threshold = thresholdPanel->findChild<QSpinBox *>();
            QVERIFY(threshold);
            QCOMPARE(threshold->minimum(), 1);
            threshold->setValue(15);
            QCOMPARE(threshold->value(), 15);

            auto *button = thresholdPanel->findChild<QPushButton *>();
            QVERIFY(button);
            QCOMPARE(button->text(), QStringLiteral("发送"));
            QVERIFY(button->width() >= 54);

            auto *shapePanel = channels->cellWidget(row, 4);
            QVERIFY(shapePanel);
            auto *polarity = shapePanel->findChild<QComboBox *>();
            QVERIFY(polarity);
            QCOMPARE(polarity->currentText(), QStringLiteral("正"));
            QVERIFY(polarity->width() >= 54);
            QVERIFY(!shapePanel->findChild<QDoubleSpinBox *>());
        }
    }

    void yAxisManualRangeSurvivesSpectrumRefresh() {
        SpectrumView view;
        view.resize(900, 600);
        view.show();
        QTest::qWait(20);

        auto snapshot = std::make_shared<SpectrumSnapshot>();
        snapshot->channelCount = 1;
        snapshot->peak = {QVector<quint64>(DefaultPeakBins, 0)};
        snapshot->integral = {
            QVector<quint64>(DefaultIntegralBins, 0)};
        snapshot->peak[0][100] = 100;
        view.setSnapshot(snapshot);
        QVERIFY(view.isYAutoScaleEnabled());
        const auto automaticRange = view.visibleYRange();
        QVERIFY(automaticRange.second >= 100);

        view.zoomYBy(0.5);
        QVERIFY(!view.isYAutoScaleEnabled());
        const auto manualRange = view.visibleYRange();
        QVERIFY(manualRange.second < automaticRange.second);

        auto updated = std::make_shared<SpectrumSnapshot>(*snapshot);
        updated->peak[0][100] = 1000;
        view.setSnapshot(updated);
        QCOMPARE(view.visibleYRange(), manualRange);

        view.resetYAutoScale();
        QVERIFY(view.isYAutoScaleEnabled());
        QVERIFY(view.visibleYRange().second >= 1000);
        view.setLogarithmic(true);
        QVERIFY(view.isYAutoScaleEnabled());
        QVERIFY(view.visibleYRange().first >= 1);
    }

    void ctrlWheelAndYAxisWheelOnlyZoomY() {
        SpectrumView view;
        view.resize(900, 600);
        view.show();
        QTest::qWait(20);
        auto *chartView = view.findChild<QChartView *>(
            QStringLiteral("spectrumMainChartView"));
        QVERIFY(chartView);

        const auto initialX = view.visibleRange();
        const auto initialY = view.visibleYRange();
        const QPoint center = chartView->viewport()->rect().center();
        QWheelEvent ctrlWheel(
            center, chartView->viewport()->mapToGlobal(center),
            QPoint(), QPoint(0, 120), Qt::NoButton,
            Qt::ControlModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(chartView->viewport(), &ctrlWheel);
        QCOMPARE(view.visibleRange(), initialX);
        QVERIFY(view.visibleYRange().second < initialY.second);

        view.resetZoom();
        const auto resetX = view.visibleRange();
        const auto resetY = view.visibleYRange();
        const QRectF plot = chartView->chart()->plotArea();
        const QPoint yAxisPoint = chartView->mapFromScene(
            QPointF(plot.left() - 8, plot.center().y()));
        QWheelEvent yAxisWheel(
            yAxisPoint,
            chartView->viewport()->mapToGlobal(yAxisPoint),
            QPoint(), QPoint(0, 120), Qt::NoButton,
            Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(chartView->viewport(), &yAxisWheel);
        QCOMPARE(view.visibleRange(), resetX);
        QVERIFY(view.visibleYRange().second < resetY.second);

        view.resetZoom();
        const auto beforeNormalX = view.visibleRange();
        const auto beforeNormalY = view.visibleYRange();
        QWheelEvent normalWheel(
            center, chartView->viewport()->mapToGlobal(center),
            QPoint(), QPoint(0, 120), Qt::NoButton,
            Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(chartView->viewport(), &normalWheel);
        QVERIFY(view.visibleRange() != beforeNormalX);
        QCOMPARE(view.visibleYRange(), beforeNormalY);
    }

    void channelDisplayRequiresClickOrEnter() {
        MainWindow window;
        window.show();
        QTest::qWait(20);
        auto *channels = window.findChild<QTableWidget *>(
            QStringLiteral("channelTable"));
        auto *spectrum = window.findChild<SpectrumView *>();
        QVERIFY(channels);
        QVERIFY(spectrum);
        QCOMPARE(spectrum->displayedChannels(), QVector<int>{0});

        const QPoint rowOne = channels->visualItemRect(
            channels->item(1, 1)).center();
        QTest::mouseMove(channels->viewport(), rowOne);
        QCoreApplication::processEvents();
        QCOMPARE(spectrum->displayedChannels(), QVector<int>{0});

        channels->setCurrentCell(
            1, 1, QItemSelectionModel::ClearAndSelect |
                      QItemSelectionModel::Rows);
        QCoreApplication::processEvents();
        QCOMPARE(spectrum->displayedChannels(), QVector<int>{0});

        channels->setFocus();
        QTest::keyClick(channels, Qt::Key_Return);
        QCoreApplication::processEvents();
        QCOMPARE(spectrum->displayedChannels(), QVector<int>{1});

        const QPoint rowTwo = channels->visualItemRect(
            channels->item(2, 1)).center();
        QTest::mouseClick(channels->viewport(), Qt::LeftButton,
                          Qt::NoModifier, rowTwo);
        QCoreApplication::processEvents();
        QCOMPARE(spectrum->displayedChannels(), QVector<int>{2});
    }

    void pendingBaselineChoiceSurvivesTelemetryRefresh() {
        MainWindow window;
        window.show();
        QTest::qWait(20);

        auto *tracking = window.findChild<QCheckBox *>(
            QStringLiteral("baselineTrackingCheckBox"));
        QVERIFY(tracking);

        BaselineSnapshot enabledSnapshot;
        enabledSnapshot.sequence = 1;
        enabledSnapshot.channels.resize(2);
        for (int channel = 0; channel < enabledSnapshot.channels.size();
             ++channel) {
            enabledSnapshot.channels[channel].channel = quint8(channel);
            enabledSnapshot.channels[channel].trackingEnabled = true;
        }
        QVERIFY(QMetaObject::invokeMethod(
            &window, "updateBaselineStatus", Qt::DirectConnection,
            Q_ARG(BaselineSnapshot, enabledSnapshot)));
        QVERIFY(tracking->isChecked());

        tracking->setChecked(false);
        QVERIFY(!tracking->isChecked());

        enabledSnapshot.sequence = 2;
        QVERIFY(QMetaObject::invokeMethod(
            &window, "updateBaselineStatus", Qt::DirectConnection,
            Q_ARG(BaselineSnapshot, enabledSnapshot)));
        QVERIFY2(!tracking->isChecked(),
                 "stale FPGA telemetry must not overwrite a pending choice");

        BaselineSnapshot disabledSnapshot = enabledSnapshot;
        disabledSnapshot.sequence = 3;
        for (auto &channel : disabledSnapshot.channels)
            channel.trackingEnabled = false;
        QVERIFY(QMetaObject::invokeMethod(
            &window, "updateBaselineStatus", Qt::DirectConnection,
            Q_ARG(BaselineSnapshot, disabledSnapshot)));
        QVERIFY(!tracking->isChecked());
    }

    void pendingFrontendChoicesSurviveTelemetryRefresh() {
        MainWindow window;
        window.show();
        QTest::qWait(20);

        DeviceCapabilities capabilities;
        capabilities.channelCount = 8;
        capabilities.adcBits = 14;
        capabilities.formatFlags = 0x0000'0240u;
        QVERIFY(QMetaObject::invokeMethod(
            &window, "updateCapabilities", Qt::DirectConnection,
            Q_ARG(DeviceCapabilities, capabilities)));
        auto *filter = window.findChild<QCheckBox *>(
            QStringLiteral("bandwidthLimitCheckBox"));
        auto *interpolation = window.findChild<QCheckBox *>(
            QStringLiteral("peakInterpolationCheckBox"));
        QVERIFY(filter);
        QVERIFY(interpolation);
        QVERIFY(interpolation->isVisible());

        DeviceStatus status;
        status.frontendStateValid = true;
        status.actualRange = 1;
        status.recoveryToleranceLsb = 32;
        status.filterEnabled = false;
        status.peakInterpolationEnabled = false;
        QVERIFY(QMetaObject::invokeMethod(
            &window, "updateDeviceStatus", Qt::DirectConnection,
            Q_ARG(DeviceStatus, status)));

        filter->setChecked(true);
        interpolation->setChecked(true);
        QVERIFY(filter->isChecked());
        QVERIFY(interpolation->isChecked());

        QVERIFY(QMetaObject::invokeMethod(
            &window, "updateDeviceStatus", Qt::DirectConnection,
            Q_ARG(DeviceStatus, status)));
        QVERIFY2(filter->isChecked(),
                 "stale FPGA filter telemetry must not overwrite a pending choice");
        QVERIFY2(interpolation->isChecked(),
                 "stale FPGA interpolation telemetry must not overwrite a pending choice");

        status.filterEnabled = true;
        status.peakInterpolationEnabled = true;
        QVERIFY(QMetaObject::invokeMethod(
            &window, "updateDeviceStatus", Qt::DirectConnection,
            Q_ARG(DeviceStatus, status)));
        QVERIFY(filter->isChecked());
        QVERIFY(interpolation->isChecked());
    }

    void reportedChannelCountRemovesUnusedRows() {
        MainWindow window;
        auto *channels = window.findChild<QTableWidget *>(
            QStringLiteral("channelTable"));
        QVERIFY(channels);
        QCOMPARE(channels->rowCount(), 8);

        DeviceStatus status;
        status.capabilitiesValid = true;
        status.capabilities.channelCount = 3;
        QVERIFY(QMetaObject::invokeMethod(
            &window, "updateDeviceStatus", Qt::DirectConnection,
            Q_ARG(DeviceStatus, status)));
        QCOMPARE(channels->rowCount(), 3);
        QCOMPARE(channels->verticalHeader()->defaultSectionSize(), 68);
    }

    void visualSmokeRender() {
        MainWindow window;
        window.resize(1366, 768);
        window.show();
        QTest::qWait(50);
        const QPixmap screenshot = window.grab();
        QVERIFY(!screenshot.isNull());
        QCOMPARE(screenshot.size(), window.size() * window.devicePixelRatioF());
        const QString output =
            qEnvironmentVariable("MCA_UI_SCREENSHOT");
        if (!output.isEmpty())
            QVERIFY2(screenshot.save(output), qPrintable(output));
    }
};

QTEST_MAIN(UiTests)
#include "test_ui.moc"
