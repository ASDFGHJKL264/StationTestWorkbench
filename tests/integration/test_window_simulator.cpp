#include "workbench/ui/mainwindow.h"
#include "workbench/application/workbenchcontroller.h"
#include "simulation/device/stationserver.h"
#include "traceability/repository.h"
#include "testing/domain.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <QDir>
#include <QFontDatabase>
#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QTimer>

class WindowSimulatorTests final : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        // The Windows offscreen plugin does not discover system fonts automatically.
        if (QGuiApplication::platformName() == "offscreen")
        {
            const auto id = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
            if (id >= 0)
                QApplication::setFont(
                    QFont(QFontDatabase::applicationFontFamilies(id).first(), 10));
        }
    }
    void actualSimulatorThroughWindowAndHistory()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        StationOptions options;
        options.port = 0;
        options.stepMs = 150;
        options.dataDirectory = directory.filePath("station");
        StationServer station(options);
        QString error;
        QVERIFY2(station.start(&error), qPrintable(error));
        QJsonObject view;
        MainWindow window(directory.filePath("client"), {{1, "127.0.0.1", station.port()}});
        auto *controller = window.findChild<WorkbenchController *>("workbenchController");
        QVERIFY(controller);
        connect(controller, &WorkbenchController::stationUpdated, &window,
                [&](const QJsonObject &v) { view = v; });
        auto *sn = window.findChild<QLineEdit *>("snInput");
        auto *start = window.findChild<QPushButton *>("startButton");
        auto *cancel = window.findChild<QPushButton *>("cancelButton");
        auto *stations = window.findChild<QTableView *>("stationView");
        auto *steps = window.findChild<QTableView *>("stepView");
        auto *history = window.findChild<QTableView *>("historyView");
        QVERIFY(sn && start && cancel && stations && steps && history);
        window.show();
        QTRY_VERIFY(view["canStart"].toBool());
        stations->selectRow(0);
        sn->setText("UI-BOARD-001");
        QTRY_VERIFY(start->isEnabled());
        QTest::mouseClick(start, Qt::LeftButton);
        QTRY_VERIFY(view["stored"].toBool() && view["canStart"].toBool());
        QCOMPARE(view["verdict"].toString(), QString("PASS"));
        QCOMPARE(steps->model()->rowCount(), 3);
        QTRY_VERIFY(history->model()->rowCount() >= 1);
        QSignalSpy report(controller, &WorkbenchController::reportReady);
        const auto reportPath = directory.filePath("window-report.html");
        controller->exportReport(view["runId"].toString(), reportPath);
        QTRY_VERIFY(!report.isEmpty());
        QVERIFY(report.last()[1].toBool());
        QFile exported(reportPath);
        QVERIFY(exported.open(QIODevice::ReadOnly));
        QVERIFY(exported.readAll().contains("UI-BOARD-001"));
        const auto capture = qEnvironmentVariable("WORKBENCH_CAPTURE");
        if (!capture.isEmpty())
            QVERIFY(window.grab().save(capture));
        // Periodic station snapshots must not erase a new operator entry.
        sn->setText("UI-BOARD-002");
        QTest::qWait(100);
        QCOMPARE(sn->text(), QString("UI-BOARD-002"));
        const auto previous = view["runId"].toString();
        QTest::mouseClick(start, Qt::LeftButton);
        QTRY_VERIFY(cancel->isEnabled());
        QTest::mouseClick(cancel, Qt::LeftButton);
        QTRY_VERIFY(view["runId"].toString() != previous && view["stored"].toBool());
        QCOMPARE(view["verdict"].toString(), QString("CANCELLED"));
        window.close();
    }
};
QTEST_MAIN(WindowSimulatorTests)
#include "test_window_simulator.moc"
