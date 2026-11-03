#include "simulator/simulatorwindow.h"
#include "simulator/simulatorcontroller.h"
#include "runtime/workbenchcontroller.h"
#include "ui/mainwindow.h"
#include <QtTest>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFontDatabase>
#include <QPushButton>
#include <QLineEdit>
#include <QMap>
#include <QTableView>
#include <QSpinBox>
#include <QTableWidget>
#include <QTcpServer>
#include <QTemporaryDir>

namespace
{
StationOptions optionsFor(const QString &root, int id = 1)
{
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0))
        return {};
    StationOptions options;
    options.stationId = id;
    options.port = probe.serverPort();
    options.dataDirectory = root + QString("/station-%1").arg(id);
    options.stepMs = 120;
    return options;
}
QJsonObject environmentRecipe(const QString &version, bool includeTemperature = true)
{
    QJsonArray steps;
    if (includeTemperature)
        steps.append(QJsonObject{{"id", "temperature"},
                                 {"label", QStringLiteral("温度")},
                                 {"unit", "C"},
                                 {"min", 15},
                                 {"max", 35},
                                 {"nominal", 25}});
    steps.append(QJsonObject{
        {"id", "pressure"},
        {"label", includeTemperature ? QStringLiteral("压力") : QStringLiteral("保压压力")},
        {"unit", "kPa"},
        {"min", 90},
        {"max", 110},
        {"nominal", 100}});
    return {{"version", version}, {"name", QStringLiteral("环境检测")}, {"steps", steps}};
}
} // namespace

class SimulatorWindowTests final : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        if (QGuiApplication::platformName() == "offscreen")
        {
            const int id = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
            if (id >= 0)
                QApplication::setFont(
                    QFont(QFontDatabase::applicationFontFamilies(id).first(), 10));
        }
    }
    void graphicalControlsDriveRealPassFailAndAckRetry()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QList<StationOptions> options;
        for (int id = 1; id <= 4; ++id)
            options.append(optionsFor(directory.path(), id));
        SimulatorWindow window(options, false);
        auto *startAll = window.findChild<QPushButton *>("startAllButton");
        auto *stopAll = window.findChild<QPushButton *>("stopAllButton");
        auto *stations = window.findChild<QTableWidget *>("stationTable");
        auto *steps = window.findChild<QTableWidget *>("stepTable");
        auto *fail = window.findChild<QComboBox *>("failStepCombo");
        auto *drop = window.findChild<QCheckBox *>("dropAckCheck");
        auto *interval = window.findChild<QSpinBox *>("stepMsSpin");
        auto *apply = window.findChild<QPushButton *>("applyFaultButton");
        QVERIFY(startAll && stopAll && stations && steps && fail && drop && interval && apply);
        window.show();
        QTest::mouseClick(startAll, Qt::LeftButton);
        for (int id = 1; id <= 4; ++id)
            QTRY_VERIFY_WITH_TIMEOUT(window.stationSnapshot(id)["listening"].toBool(), 5000);
        QCOMPARE(stations->rowCount(), 4);
        stations->selectRow(0);
        QJsonObject view;
        WorkbenchController client;
        connect(&client, &WorkbenchController::stationUpdated, &window,
                [&](const QJsonObject &row) { view = row; });
        client.initialize(directory.filePath("client"), {{1, "127.0.0.1", options.first().port}});
        QTRY_VERIFY_WITH_TIMEOUT(view["canStart"].toBool(), 6000);
        client.startTest(1, "GUI-SIM-PASS-001", "demo-5v-v1");
        QTRY_VERIFY_WITH_TIMEOUT(view["stored"].toBool() && view["canStart"].toBool(), 6000);
        QCOMPARE(view["verdict"].toString(), "PASS");
        QTRY_COMPARE(window.stationSnapshot(1)["state"].toString(), "acked");
        QCOMPARE(steps->rowCount(), 3);

        fail->setCurrentIndex(fail->findData("current"));
        interval->setValue(160);
        drop->setChecked(true);
        QTRY_VERIFY(apply->isEnabled());
        QTest::mouseClick(apply, Qt::LeftButton);
        QTRY_COMPARE(window.stationSnapshot(1)["failStep"].toString(), "current");
        QTRY_VERIFY(window.stationSnapshot(1)["dropFirstResultAck"].toBool());
        const auto previous = view["runId"].toString();
        QSignalSpy diagnostics(window.controller(), &SimulatorController::diagnostic);
        client.startTest(1, "GUI-SIM-FAIL-002", "demo-5v-v1");
        QTRY_VERIFY_WITH_TIMEOUT(view["runId"].toString() != previous && view["stored"].toBool() &&
                                     view["canStart"].toBool(),
                                 6000);
        QCOMPARE(view["verdict"].toString(), "FAIL");
        QTRY_COMPARE(window.stationSnapshot(1)["state"].toString(), "acked");
        QVERIFY(!diagnostics.isEmpty());
        const auto capture = qEnvironmentVariable("SIMULATOR_CAPTURE");
        if (!capture.isEmpty())
            QVERIFY(window.grab().save(capture));
        client.shutdown();
        QTest::mouseClick(stopAll, Qt::LeftButton);
        for (int id = 1; id <= 4; ++id)
            QTRY_VERIFY(!window.stationSnapshot(id)["listening"].toBool());
        window.close();
    }
    void disconnectButtonPreservesRunAndClientRecovers()
    {
        QTemporaryDir directory;
        auto options = optionsFor(directory.path());
        options.stepMs = 400;
        SimulatorWindow window({options}, true);
        window.show();
        QTRY_VERIFY(window.stationSnapshot(1)["listening"].toBool());
        bool sawDisconnectedDuringRun = false;
        connect(window.controller(), &SimulatorController::stationChanged, &window,
                [&](const QJsonObject &row)
                {
                    if (row["state"] == "executing" && !row["connected"].toBool())
                        sawDisconnectedDuringRun = true;
                });
        QJsonObject view;
        WorkbenchController client;
        connect(&client, &WorkbenchController::stationUpdated, &window,
                [&](const QJsonObject &row) { view = row; });
        client.initialize(directory.filePath("client"), {{1, "127.0.0.1", options.port}});
        QTRY_VERIFY(view["canStart"].toBool());
        client.startTest(1, "GUI-SIM-RECONNECT-001", "demo-5v-v1");
        QTRY_COMPARE(window.stationSnapshot(1)["state"].toString(), "executing");
        const auto runId = window.stationSnapshot(1)["runId"].toString();
        QVERIFY(!runId.isEmpty());
        auto *disconnect = window.findChild<QPushButton *>("disconnectButton");
        QVERIFY(disconnect);
        QTRY_VERIFY(disconnect->isEnabled());
        QTest::mouseClick(disconnect, Qt::LeftButton);
        QTRY_VERIFY(sawDisconnectedDuringRun);
        QTRY_VERIFY_WITH_TIMEOUT(view["stored"].toBool() && view["canStart"].toBool(), 10000);
        QCOMPARE(view["runId"].toString(), runId);
        QCOMPARE(view["verdict"].toString(), "PASS");
        QCOMPARE(view["stepResults"].toArray().size(), 3);
        client.shutdown();
        window.close();
    }
    void mainWindowPreviewsFaultsBeforeStartAndResendsAfterReconnect()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto first = optionsFor(directory.path(), 1);
        auto second = optionsFor(directory.path(), 2);
        first.port = second.port = 0;
        SimulatorWindow simulator({first, second}, true);
        simulator.show();
        QTRY_VERIFY(simulator.stationSnapshot(1)["listening"].toBool());
        QTRY_VERIFY(simulator.stationSnapshot(2)["listening"].toBool());
        QMap<int, QJsonObject> views;
        bool sawEmptyPreviewOnRestart = false;
        MainWindow main(directory.filePath("client"),
                        {{1, "127.0.0.1", quint16(simulator.stationSnapshot(1)["port"].toInt())},
                         {2, "127.0.0.1", quint16(simulator.stationSnapshot(2)["port"].toInt())}});
        connect(main.controller(), &WorkbenchController::stationUpdated, &main,
                [&](const QJsonObject &row) { views[row["stationId"].toInt()] = row; });
        auto *recipes = main.findChild<QComboBox *>("recipeCombo");
        auto *mainStations = main.findChild<QTableView *>("stationView");
        auto *sn = main.findChild<QLineEdit *>("snInput");
        auto *start = main.findChild<QPushButton *>("startButton");
        auto *stations = simulator.findChild<QTableWidget *>("stationTable");
        auto *fail = simulator.findChild<QComboBox *>("failStepCombo");
        auto *apply = simulator.findChild<QPushButton *>("applyFaultButton");
        auto *stopStation = simulator.findChild<QPushButton *>("stopStationButton");
        auto *startStation = simulator.findChild<QPushButton *>("startStationButton");
        QVERIFY(recipes && mainStations && sn && start && stations && fail && apply &&
                stopStation && startStation);
        main.show();
        QTRY_VERIFY(views.value(1)["canStart"].toBool() && views.value(2)["canStart"].toBool());
        QTRY_VERIFY(recipes->findData("demo-5v-v1") >= 0);
        const auto temperature = environmentRecipe("environment-v1");
        const auto pressureOnly = environmentRecipe("environment-v2", false);
        main.controller()->publishRecipe(temperature);
        QTRY_VERIFY(recipes->findData("environment-v1") >= 0);
        main.controller()->publishRecipe(pressureOnly);
        QTRY_VERIFY(recipes->findData("environment-v2") >= 0);
        mainStations->selectRow(0);
        stations->setCurrentCell(0, 0);
        recipes->setCurrentIndex(recipes->findData("environment-v1"));
        QTRY_COMPARE(simulator.stationSnapshot(1)["recipePreview"].toObject(), temperature);
        QTRY_COMPARE(fail->count(), 3);
        QCOMPARE(fail->itemText(fail->findData("temperature")), QStringLiteral("温度超限"));
        QCOMPARE(fail->itemText(fail->findData("pressure")), QStringLiteral("压力超限"));
        QCOMPARE(fail->findData("voltage"), -1);
        QVERIFY(simulator.stationSnapshot(1)["runId"].toString().isEmpty());
        QCOMPARE(simulator.stationSnapshot(1)["state"].toString(), QString("idle"));

        // No configured/draft fault: removed steps must disappear, and labels must refresh.
        recipes->setCurrentIndex(recipes->findData("environment-v2"));
        QTRY_COMPARE(simulator.stationSnapshot(1)["recipePreview"].toObject(), pressureOnly);
        QTRY_COMPARE(fail->count(), 2);
        QCOMPARE(fail->findData("temperature"), -1);
        QCOMPARE(fail->itemText(fail->findData("pressure")), QStringLiteral("保压压力超限"));
        recipes->setCurrentIndex(recipes->findData("environment-v1"));
        QTRY_COMPARE(simulator.stationSnapshot(1)["recipePreview"].toObject(), temperature);

        // Recreate the server, clearing its transient preview. The GUI selection stays put;
        // the network engine must resend its cached recipe after the next handshake.
        connect(simulator.controller(), &SimulatorController::stationChanged, &main,
                [&](const QJsonObject &row)
                {
                    if (row["stationId"].toInt() == 1 && row["listening"].toBool() &&
                        row["recipePreview"].toObject().isEmpty())
                        sawEmptyPreviewOnRestart = true;
                });
        QTRY_VERIFY(stopStation->isEnabled());
        QTest::mouseClick(stopStation, Qt::LeftButton);
        QTRY_VERIFY(!simulator.stationSnapshot(1)["listening"].toBool());
        QTRY_VERIFY(startStation->isEnabled());
        QTest::mouseClick(startStation, Qt::LeftButton);
        QTRY_VERIFY(simulator.stationSnapshot(1)["listening"].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(sawEmptyPreviewOnRestart, 6000);
        QTRY_COMPARE_WITH_TIMEOUT(simulator.stationSnapshot(1)["recipePreview"].toObject(),
                                  temperature, 10000);
        QVERIFY(simulator.stationSnapshot(1)["runId"].toString().isEmpty());

        // An unapplied choice remains explicit when a preview no longer contains that ID.
        fail->setCurrentIndex(fail->findData("temperature"));
        recipes->setCurrentIndex(recipes->findData("environment-v2"));
        QTRY_COMPARE(simulator.stationSnapshot(1)["recipePreview"].toObject(), pressureOnly);
        QCOMPARE(fail->currentData().toString(), QString("temperature"));
        QVERIFY(fail->currentText().contains(QStringLiteral("当前配方未包含")));
        QVERIFY(simulator.stationSnapshot(1)["failStep"].toString().isEmpty());
        recipes->setCurrentIndex(recipes->findData("environment-v1"));
        QTRY_COMPARE(simulator.stationSnapshot(1)["recipePreview"].toObject(), temperature);
        QCOMPARE(fail->currentData().toString(), QString("temperature"));
        QCOMPARE(fail->currentText(), QStringLiteral("温度超限"));
        QTRY_VERIFY(apply->isEnabled());
        QTest::mouseClick(apply, Qt::LeftButton);
        QTRY_COMPARE(simulator.stationSnapshot(1)["failStep"].toString(), QString("temperature"));
        QTRY_VERIFY(apply->isEnabled());

        mainStations->selectRow(1);
        recipes->setCurrentIndex(recipes->findData("environment-v2"));
        QTRY_COMPARE(simulator.stationSnapshot(2)["recipePreview"].toObject(), pressureOnly);
        stations->setCurrentCell(1, 0);
        QCOMPARE(fail->currentData().toString(), QString());
        QCOMPARE(fail->findData("temperature"), -1);
        fail->setCurrentIndex(fail->findData("pressure"));
        QTRY_VERIFY(apply->isEnabled());
        QTest::mouseClick(apply, Qt::LeftButton);
        QTRY_COMPARE(simulator.stationSnapshot(2)["failStep"].toString(), QString("pressure"));
        QTRY_VERIFY(apply->isEnabled());
        stations->setCurrentCell(0, 0);
        QCOMPARE(fail->currentData().toString(), QString("temperature"));
        QCOMPARE(simulator.stationSnapshot(1)["recipePreview"].toObject(), temperature);
        mainStations->selectRow(0);
        recipes->setCurrentIndex(recipes->findData("environment-v1"));
        QTRY_COMPARE(simulator.stationSnapshot(1)["recipePreview"].toObject(), temperature);
        QCOMPARE(simulator.stationSnapshot(2)["recipePreview"].toObject(), pressureOnly);
        QCOMPARE(simulator.stationSnapshot(2)["failStep"].toString(), QString("pressure"));
        QCOMPARE(fail->currentData().toString(), QString("temperature"));
        sn->setText("ENVIRONMENT-FAIL-001");
        QTRY_VERIFY(start->isEnabled());
        QTest::mouseClick(start, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(
            views.value(1)["stored"].toBool() && views.value(1)["canStart"].toBool(), 6000);
        QCOMPARE(views.value(1)["verdict"].toString(), QString("FAIL"));
        const auto results = views.value(1)["stepResults"].toArray();
        QCOMPARE(results.size(), 2);
        QCOMPARE(results.first().toObject()["stepId"].toString(), QString("temperature"));
        QVERIFY(!results.first().toObject()["passed"].toBool());
        QTRY_COMPARE(simulator.stationSnapshot(1)["state"].toString(), QString("acked"));
        QCOMPARE(simulator.stationSnapshot(1)["recipe"].toObject(), temperature);
        QVERIFY(simulator.stationSnapshot(2)["runId"].toString().isEmpty());
        main.close();
        simulator.close();
    }
    void previewChangedDuringRunPreservesActiveRecipeAndUnappliedFault()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto options = optionsFor(directory.path());
        options.port = 0;
        options.stepMs = 1500;
        QJsonObject view;
        SimulatorWindow simulator({options}, true);
        simulator.show();
        QTRY_VERIFY(simulator.stationSnapshot(1)["listening"].toBool());
        MainWindow main(directory.filePath("client"),
                        {{1, "127.0.0.1", quint16(simulator.stationSnapshot(1)["port"].toInt())}});
        connect(main.controller(), &WorkbenchController::stationUpdated, &main,
                [&](const QJsonObject &row) { view = row; });
        auto *recipes = main.findChild<QComboBox *>("recipeCombo");
        auto *sn = main.findChild<QLineEdit *>("snInput");
        auto *start = main.findChild<QPushButton *>("startButton");
        auto *fail = simulator.findChild<QComboBox *>("failStepCombo");
        auto *apply = simulator.findChild<QPushButton *>("applyFaultButton");
        QVERIFY(recipes && sn && start && fail && apply);
        main.show();
        QTRY_VERIFY(view["canStart"].toBool());
        QTRY_VERIFY(recipes->findData("demo-5v-v1") >= 0);
        const auto temperature = environmentRecipe("running-environment-v1");
        const auto pressureOnly = environmentRecipe("next-environment-v2", false);
        main.controller()->publishRecipe(temperature);
        QTRY_VERIFY(recipes->findData("running-environment-v1") >= 0);
        main.controller()->publishRecipe(pressureOnly);
        QTRY_VERIFY(recipes->findData("next-environment-v2") >= 0);
        recipes->setCurrentIndex(recipes->findData("running-environment-v1"));
        QTRY_COMPARE(simulator.stationSnapshot(1)["recipePreview"].toObject(), temperature);
        fail->setCurrentIndex(fail->findData("pressure")); // Deliberately leave this unapplied.
        sn->setText("ENVIRONMENT-ACTIVE-001");
        QTRY_VERIFY(start->isEnabled());
        QTest::mouseClick(start, Qt::LeftButton);
        QTRY_COMPARE(simulator.stationSnapshot(1)["state"].toString(), QString("executing"));
        const auto runId = simulator.stationSnapshot(1)["runId"].toString();
        QVERIFY(!runId.isEmpty());
        QVERIFY(!fail->isEnabled());
        QVERIFY(!apply->isEnabled());
        recipes->setCurrentIndex(recipes->findData("next-environment-v2"));
        QTRY_COMPARE(simulator.stationSnapshot(1)["recipePreview"].toObject(), pressureOnly);
        QCOMPARE(simulator.stationSnapshot(1)["recipe"].toObject(), temperature);
        QCOMPARE(simulator.stationSnapshot(1)["runId"].toString(), runId);
        QVERIFY(simulator.stationSnapshot(1)["failStep"].toString().isEmpty());
        QCOMPARE(fail->count(), 3);
        QVERIFY(fail->findData("temperature") >= 0);
        QCOMPARE(fail->itemText(fail->findData("pressure")), QStringLiteral("压力超限"));
        QCOMPARE(fail->currentData().toString(), QString("pressure"));
        QTRY_VERIFY(simulator.stationSnapshot(1)["state"] == "executing" &&
                    simulator.stationSnapshot(1)["steps"].toArray().size() == 1);
        QCOMPARE(fail->currentData().toString(), QString("pressure"));
        QCOMPARE(fail->count(), 3);
        QVERIFY(!fail->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(view["stored"].toBool() && view["canStart"].toBool(), 6000);
        QCOMPARE(view["verdict"].toString(), QString("PASS"));
        QCOMPARE(view["recipeVersion"], temperature["version"]);
        QCOMPARE(view["stepResults"].toArray().size(), 2);
        QTRY_COMPARE(simulator.stationSnapshot(1)["state"].toString(), QString("acked"));
        QTRY_VERIFY(fail->isEnabled());
        QCOMPARE(simulator.stationSnapshot(1)["recipe"].toObject(), temperature);
        QCOMPARE(simulator.stationSnapshot(1)["recipePreview"].toObject(), pressureOnly);
        QCOMPARE(fail->count(), 2);
        QCOMPARE(fail->findData("temperature"), -1);
        QCOMPARE(fail->currentData().toString(), QString("pressure"));
        QCOMPARE(fail->currentText(), QStringLiteral("保压压力超限"));
        QVERIFY(simulator.stationSnapshot(1)["failStep"].toString().isEmpty());
        main.close();
        simulator.close();
    }
    void occupiedPortReportsFailureAndCanRetry()
    {
        QTemporaryDir directory;
        QTcpServer occupied;
        QVERIFY(occupied.listen(QHostAddress::LocalHost, 0));
        StationOptions options;
        options.dataDirectory = directory.path();
        options.port = occupied.serverPort();
        SimulatorWindow window({options}, false);
        QSignalSpy operations(window.controller(), &SimulatorController::operationFinished);
        auto *start = window.findChild<QPushButton *>("startAllButton");
        QVERIFY(start);
        window.show();
        QTest::mouseClick(start, Qt::LeftButton);
        QTRY_VERIFY(!operations.isEmpty());
        QVERIFY(!operations.last()[2].toBool());
        QVERIFY(!window.stationSnapshot(1)["listening"].toBool());
        occupied.close();
        QTest::mouseClick(start, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(window.stationSnapshot(1)["listening"].toBool(), 5000);
        window.close();
    }
};
QTEST_MAIN(SimulatorWindowTests)
#include "test_simulator_window.moc"
