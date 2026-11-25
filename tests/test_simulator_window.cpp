#include "simulation/ui/simulatorwindow.h"
#include "simulation/application/simulatorcontroller.h"
#include "workbenchcontroller.h"
#include "mainwindow.h"
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
// 为 root 目录下 id 工位分配空闲端口并生成独立配置。
StationOptions optionsFor(const QString &root, int id = 1)
{
    // 申请空闲的本机 TCP 端口以避免测试冲突。
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0))
        return {};
    // 配置模拟工位编号、端口、步骤耗时和状态目录。
    StationOptions options;
    options.stationId = id;
    options.port = probe.serverPort();
    options.dataDirectory = root + QString("/station-%1").arg(id);
    options.stepMs = 120;
    return options;
}
// 按 version 生成环境配方，includeTemperature 决定是否含温度步骤。
QJsonObject environmentRecipe(const QString &version, bool includeTemperature = true)
{
    // 收集环境配方的温度和压力步骤。
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
    // 在离屏模式加载系统字体，保证模拟器窗口可绘制。
    void initTestCase()
    {
        if (QGuiApplication::platformName() == "offscreen")
        {
            // 保存加载微软雅黑字体后返回的字体编号。
            const int id = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
            if (id >= 0)
                QApplication::setFont(
                    QFont(QFontDatabase::applicationFontFamilies(id).first(), 10));
        }
    }
    // 验证模拟器按钮实际驱动通过、失败和归档确认重试。
    void graphicalControlsDriveRealPassFailAndAckRetry()
    {
        // 隔离本次验证的数据库、日志和工位状态。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 配置模拟工位编号、端口、步骤耗时和状态目录。
        QList<StationOptions> options;
        for (int id = 1; id <= 4; ++id)
            options.append(optionsFor(directory.path(), id));
        // 提供本次验证操作和观察的图形窗口。
        SimulatorWindow window(options, false);
        // 操作模拟器的启动全部工位按钮。
        auto *startAll = window.findChild<QPushButton *>("startAllButton");
        // 操作模拟器的停止全部工位按钮。
        auto *stopAll = window.findChild<QPushButton *>("stopAllButton");
        // 选择并观察工位状态表。
        auto *stations = window.findChild<QTableWidget *>("stationTable");
        // 展示模拟器实际执行的测试步骤。
        auto *steps = window.findChild<QTableWidget *>("stepTable");
        // 选择模拟器需要注入异常的测试步骤。
        auto *fail = window.findChild<QComboBox *>("failStepCombo");
        // 设置模拟器是否丢弃首次归档确认。
        auto *drop = window.findChild<QCheckBox *>("dropAckCheck");
        // 设置模拟器每个测试步骤的执行耗时。
        auto *interval = window.findChild<QSpinBox *>("stepMsSpin");
        // 将当前故障配置应用到所选模拟工位。
        auto *apply = window.findChild<QPushButton *>("applyFaultButton");
        QVERIFY(startAll && stopAll && stations && steps && fail && drop && interval && apply);
        window.show();
        QTest::mouseClick(startAll, Qt::LeftButton);
        for (int id = 1; id <= 4; ++id)
            QTRY_VERIFY_WITH_TIMEOUT(window.stationSnapshot(id)["listening"].toBool(), 5000);
        QCOMPARE(stations->rowCount(), 4);
        stations->selectRow(0);
        // 缓存客户端收到的最新工位状态。
        QJsonObject view;
        // 驱动测试请求并观察客户端恢复和归档行为。
        WorkbenchController client;
        // 接收工位快照并更新本次验证保存的状态。
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
        // 保存上一轮测试编号，以确认后续结果属于新测试。
        const auto previous = view["runId"].toString();
        // 收集模拟器故障注入及确认重试诊断。
        QSignalSpy diagnostics(window.controller(), &SimulatorController::diagnostic);
        client.startTest(1, "GUI-SIM-FAIL-002", "demo-5v-v1");
        QTRY_VERIFY_WITH_TIMEOUT(view["runId"].toString() != previous && view["stored"].toBool() &&
                                     view["canStart"].toBool(),
                                 6000);
        QCOMPARE(view["verdict"].toString(), "FAIL");
        QTRY_COMPARE(window.stationSnapshot(1)["state"].toString(), "acked");
        QVERIFY(!diagnostics.isEmpty());
        // 读取可选的界面截图输出路径。
        const auto capture = qEnvironmentVariable("SIMULATOR_CAPTURE");
        if (!capture.isEmpty())
            QVERIFY(window.grab().save(capture));
        client.shutdown();
        QTest::mouseClick(stopAll, Qt::LeftButton);
        for (int id = 1; id <= 4; ++id)
            QTRY_VERIFY(!window.stationSnapshot(id)["listening"].toBool());
        window.close();
    }
    // 验证主动断开连接后测试身份保留且客户端能够恢复。
    void disconnectButtonPreservesRunAndClientRecovers()
    {
        // 隔离本次验证的数据库、日志和工位状态。
        QTemporaryDir directory;
        // 配置模拟工位编号、端口、步骤耗时和状态目录。
        auto options = optionsFor(directory.path());
        options.stepMs = 400;
        // 提供本次验证操作和观察的图形窗口。
        SimulatorWindow window({options}, true);
        window.show();
        QTRY_VERIFY(window.stationSnapshot(1)["listening"].toBool());
        // 记录执行中是否实际观察到客户端断开。
        bool sawDisconnectedDuringRun = false;
        // 接收工位快照并更新本次验证保存的状态。
        connect(window.controller(), &SimulatorController::stationChanged, &window,
                [&](const QJsonObject &row)
                {
                    if (row["state"] == "executing" && !row["connected"].toBool())
                        sawDisconnectedDuringRun = true;
                });
        // 缓存客户端收到的最新工位状态。
        QJsonObject view;
        // 驱动测试请求并观察客户端恢复和归档行为。
        WorkbenchController client;
        // 接收工位快照并更新本次验证保存的状态。
        connect(&client, &WorkbenchController::stationUpdated, &window,
                [&](const QJsonObject &row) { view = row; });
        client.initialize(directory.filePath("client"), {{1, "127.0.0.1", options.port}});
        QTRY_VERIFY(view["canStart"].toBool());
        client.startTest(1, "GUI-SIM-RECONNECT-001", "demo-5v-v1");
        QTRY_COMPARE(window.stationSnapshot(1)["state"].toString(), "executing");
        // 保存当前测试编号以检查重连后执行身份保持一致。
        const auto runId = window.stationSnapshot(1)["runId"].toString();
        QVERIFY(!runId.isEmpty());
        // 操作模拟器主动断开客户端的按钮。
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
    // 验证工作台开始前同步配方预览，并在工位重建后重新同步。
    void mainWindowPreviewsFaultsBeforeStartAndResendsAfterReconnect()
    {
        // 隔离本次验证的数据库、日志和工位状态。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 保存首个模拟工位的独立连接配置。
        auto first = optionsFor(directory.path(), 1);
        // 保存第二个模拟工位的独立连接配置。
        auto second = optionsFor(directory.path(), 2);
        first.port = second.port = 0;
        // 提供用于测试执行、配方预览和故障注入的模拟器窗口。
        SimulatorWindow simulator({first, second}, true);
        simulator.show();
        QTRY_VERIFY(simulator.stationSnapshot(1)["listening"].toBool());
        QTRY_VERIFY(simulator.stationSnapshot(2)["listening"].toBool());
        // 按工位编号缓存客户端收到的最新工位快照。
        QMap<int, QJsonObject> views;
        // 记录模拟工位重建时是否清空了临时配方预览。
        bool sawEmptyPreviewOnRestart = false;
        // 提供工作台界面以操作配方和测试请求。
        MainWindow main(directory.filePath("client"),
                        {{1, "127.0.0.1", quint16(simulator.stationSnapshot(1)["port"].toInt())},
                         {2, "127.0.0.1", quint16(simulator.stationSnapshot(2)["port"].toInt())}});
        // 接收工位快照并更新本次验证保存的状态。
        connect(main.controller(), &WorkbenchController::stationUpdated, &main,
                [&](const QJsonObject &row) { views[row["stationId"].toInt()] = row; });
        // 选择工作台当前使用的配方版本。
        auto *recipes = main.findChild<QComboBox *>("recipeCombo");
        // 选择工作台当前操作的工位。
        auto *mainStations = main.findChild<QTableView *>("stationView");
        // 输入或筛选本次验证使用的产品序列号。
        auto *sn = main.findChild<QLineEdit *>("snInput");
        // 操作当前窗口的测试或工位启动按钮。
        auto *start = main.findChild<QPushButton *>("startButton");
        // 选择并观察工位状态表。
        auto *stations = simulator.findChild<QTableWidget *>("stationTable");
        // 选择模拟器需要注入异常的测试步骤。
        auto *fail = simulator.findChild<QComboBox *>("failStepCombo");
        // 将当前故障配置应用到所选模拟工位。
        auto *apply = simulator.findChild<QPushButton *>("applyFaultButton");
        // 操作模拟器停止当前工位的按钮。
        auto *stopStation = simulator.findChild<QPushButton *>("stopStationButton");
        // 操作模拟器启动当前工位的按钮。
        auto *startStation = simulator.findChild<QPushButton *>("startStationButton");
        QVERIFY(recipes && mainStations && sn && start && stations && fail && apply &&
                stopStation && startStation);
        main.show();
        QTRY_VERIFY(views.value(1)["canStart"].toBool() && views.value(2)["canStart"].toBool());
        QTRY_VERIFY(recipes->findData("demo-5v-v1") >= 0);
        // 保存同时包含温度与压力步骤的环境检测配方。
        const auto temperature = environmentRecipe("environment-v1");
        // 保存仅含压力步骤的另一配方版本。
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
        // 接收工位快照并更新本次验证保存的状态。
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
        // 保存归档后各测试步骤的测量值与判定。
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
    // 验证执行中更换预览不改当前配方或未应用的故障选择。
    void previewChangedDuringRunPreservesActiveRecipeAndUnappliedFault()
    {
        // 隔离本次验证的数据库、日志和工位状态。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 配置模拟工位编号、端口、步骤耗时和状态目录。
        auto options = optionsFor(directory.path());
        options.port = 0;
        options.stepMs = 1500;
        // 缓存客户端收到的最新工位状态。
        QJsonObject view;
        // 提供用于测试执行、配方预览和故障注入的模拟器窗口。
        SimulatorWindow simulator({options}, true);
        simulator.show();
        QTRY_VERIFY(simulator.stationSnapshot(1)["listening"].toBool());
        // 提供工作台界面以操作配方和测试请求。
        MainWindow main(directory.filePath("client"),
                        {{1, "127.0.0.1", quint16(simulator.stationSnapshot(1)["port"].toInt())}});
        // 接收工位快照并更新本次验证保存的状态。
        connect(main.controller(), &WorkbenchController::stationUpdated, &main,
                [&](const QJsonObject &row) { view = row; });
        // 选择工作台当前使用的配方版本。
        auto *recipes = main.findChild<QComboBox *>("recipeCombo");
        // 输入或筛选本次验证使用的产品序列号。
        auto *sn = main.findChild<QLineEdit *>("snInput");
        // 操作当前窗口的测试或工位启动按钮。
        auto *start = main.findChild<QPushButton *>("startButton");
        // 选择模拟器需要注入异常的测试步骤。
        auto *fail = simulator.findChild<QComboBox *>("failStepCombo");
        // 将当前故障配置应用到所选模拟工位。
        auto *apply = simulator.findChild<QPushButton *>("applyFaultButton");
        QVERIFY(recipes && sn && start && fail && apply);
        main.show();
        QTRY_VERIFY(view["canStart"].toBool());
        QTRY_VERIFY(recipes->findData("demo-5v-v1") >= 0);
        // 保存同时包含温度与压力步骤的环境检测配方。
        const auto temperature = environmentRecipe("running-environment-v1");
        // 保存仅含压力步骤的另一配方版本。
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
        // 保存当前测试编号以检查重连后执行身份保持一致。
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
    // 验证端口被占用时报告失败，释放后允许重新启动。
    void occupiedPortReportsFailureAndCanRetry()
    {
        // 隔离本次验证的数据库、日志和工位状态。
        QTemporaryDir directory;
        // 预先占用 TCP 端口以构造启动失败。
        QTcpServer occupied;
        QVERIFY(occupied.listen(QHostAddress::LocalHost, 0));
        // 配置模拟工位编号、端口、步骤耗时和状态目录。
        StationOptions options;
        options.dataDirectory = directory.path();
        options.port = occupied.serverPort();
        // 提供本次验证操作和观察的图形窗口。
        SimulatorWindow window({options}, false);
        // 记录模拟器启动操作的完成状态和失败原因。
        QSignalSpy operations(window.controller(), &SimulatorController::operationFinished);
        // 操作当前窗口的测试或工位启动按钮。
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
