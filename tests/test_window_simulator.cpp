#include "mainwindow.h"
#include "workbenchcontroller.h"
#include "simulation/device/stationserver.h"
#include "repository.h"
#include "domain.h"
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
#include <QAction>
#include <QDialog>
#include <QListView>

class WindowSimulatorTests final : public QObject
{
    Q_OBJECT
  private slots:
    // 准备非原生文件对话框并在离屏模式加载系统字体。
    void initTestCase()
    {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        // The Windows offscreen plugin does not discover system fonts automatically.
        if (QGuiApplication::platformName() == "offscreen")
        {
            // 保存加载微软雅黑字体后返回的字体编号。
            const auto id = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
            if (id >= 0)
                QApplication::setFont(
                    QFont(QFontDatabase::applicationFontFamilies(id).first(), 10));
        }
    }
    // 验证实际模拟器与工作台界面的测试、历史、报告及取消流程。
    void actualSimulatorThroughWindowAndHistory()
    {
        // 隔离本次验证的数据库、日志和工位状态。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 配置模拟工位编号、端口、步骤耗时和状态目录。
        StationOptions options;
        options.port = 0;
        options.stepMs = 150;
        options.dataDirectory = directory.filePath("station");
        // 提供真实本机 TCP 通信的模拟工位服务。
        StationServer station(options);
        // 接收模拟工位启动失败的原因。
        QString error;
        QVERIFY2(station.start(&error), qPrintable(error));
        // 缓存客户端收到的最新工位状态。
        QJsonObject view;
        // 提供本次验证操作和观察的图形窗口。
        MainWindow window(directory.filePath("client"), {{1, "127.0.0.1", station.port()}});
        // 协调客户端连接、测试执行和历史查询。
        auto *controller = window.findChild<WorkbenchController *>("workbenchController");
        QVERIFY(controller);
        // 接收工位快照并更新本次验证保存的状态。
        connect(controller, &WorkbenchController::stationUpdated, &window,
                [&](const QJsonObject &v) { view = v; });
        // 输入或筛选本次验证使用的产品序列号。
        auto *sn = window.findChild<QLineEdit *>("snInput");
        // 操作当前窗口的测试或工位启动按钮。
        auto *start = window.findChild<QPushButton *>("startButton");
        // 操作工作台请求取消当前测试的按钮。
        auto *cancel = window.findChild<QPushButton *>("cancelButton");
        // 选择并观察工位状态表。
        auto *stations = window.findChild<QTableView *>("stationView");
        // 观察工作台当前测试步骤表的行数和结果。
        auto *steps = window.findChild<QTableView *>("stepView");
        // 观察测试完成后历史结果表是否新增记录。
        auto *history = window.findChild<QTableView *>("historyView");
        QVERIFY(sn && start && cancel && stations && steps && history);
        window.show();
        // 从独立追溯入口打开历史记录。
        auto *historyAction = window.findChild<QAction *>("openHistoryAction");
        QVERIFY(historyAction);
        historyAction->trigger();
        QTRY_VERIFY(view["canStart"].toBool());
        stations->selectRow(0);
        sn->setText("UI-BOARD-001");
        QTRY_VERIFY(start->isEnabled());
        QTest::mouseClick(start, Qt::LeftButton);
        QTRY_VERIFY(view["stored"].toBool() && view["canStart"].toBool());
        QCOMPARE(view["verdict"].toString(), QString("PASS"));
        QCOMPARE(steps->model()->rowCount(), 3);
        QTRY_VERIFY(history->model()->rowCount() >= 1);
        // 记录单次测试报告导出的完成通知。
        QSignalSpy report(controller, &WorkbenchController::reportReady);
        // 指定本次 HTML 测试报告的输出位置。
        const auto reportPath = directory.filePath("window-report.html");
        controller->exportReport(view["runId"].toString(), reportPath);
        QTRY_VERIFY(!report.isEmpty());
        QVERIFY(report.last()[1].toBool());
        // 打开已导出的 HTML 报告以核对产品序列号。
        QFile exported(reportPath);
        QVERIFY(exported.open(QIODevice::ReadOnly));
        QVERIFY(exported.readAll().contains("UI-BOARD-001"));
        // 读取可选的界面截图输出路径。
        const auto capture = qEnvironmentVariable("WORKBENCH_CAPTURE");
        if (!capture.isEmpty())
            QVERIFY(window.grab().save(capture));
        // Periodic station snapshots must not erase a new operator entry.
        sn->setText("UI-BOARD-002");
        QTest::qWait(100);
        QCOMPARE(sn->text(), QString("UI-BOARD-002"));
        // 保存上一轮测试编号，以确认后续结果属于新测试。
        const auto previous = view["runId"].toString();
        QTest::mouseClick(start, Qt::LeftButton);
        QTRY_VERIFY(cancel->isEnabled());
        QTest::mouseClick(cancel, Qt::LeftButton);
        QTRY_VERIFY(view["runId"].toString() != previous && view["stored"].toBool());
        QCOMPARE(view["verdict"].toString(), QString("CANCELLED"));
        window.close();
    }

    // 验证关闭历史和日志子窗口不影响进行中的真实TCP测试。
    void closingToolWindowsKeepsTestRunningAndResultsAvailable()
    {
        // 隔离本次验证的主程序和模拟工位状态。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 为关闭子窗操作保留足够的在途测试时间。
        StationOptions options;
        options.port = 0;
        options.stepMs = 350;
        options.dataDirectory = directory.filePath("station");
        // 提供本机实际TCP工位。
        StationServer station(options);
        // 接收工位初始化失败原因。
        QString error;
        QVERIFY2(station.start(&error), qPrintable(error));
        // 缓存后台最新工位状态。
        QJsonObject view;
        // 创建本次独立工具窗口验证的工作台。
        MainWindow window(directory.filePath("client"), {{1, "127.0.0.1", station.port()}});
        connect(window.controller(), &WorkbenchController::stationUpdated, &window,
                [&](const QJsonObject &snapshot) { view = snapshot; });
        window.show();
        // 定位两个工具入口，保证操作实际打开的窗口。
        auto *historyAction = window.findChild<QAction *>("openHistoryAction");
        // 定位运行日志入口。
        auto *logAction = window.findChild<QAction *>("openLogAction");
        QVERIFY(historyAction && logAction);
        historyAction->trigger();
        logAction->trigger();
        // 保存关闭前的子窗口对象，之后应复用同一对象。
        auto *historyDialog = window.findChild<QDialog *>("historyDialog");
        // 保存运行日志窗口对象。
        auto *logDialog = window.findChild<QDialog *>("logDialog");
        // 读取独立历史窗口的结果表。
        auto *history = window.findChild<QTableView *>("historyView");
        // 读取持续采集后台日志的列表。
        auto *logs = window.findChild<QListView *>("logView");
        // 填写在途测试使用的产品序列号。
        auto *sn = window.findChild<QLineEdit *>("snInput");
        // 使用工作台的实际开始按钮执行测试。
        auto *start = window.findChild<QPushButton *>("startButton");
        QVERIFY(historyDialog && logDialog && history && logs && sn && start);
        QTRY_VERIFY(view["canStart"].toBool());
        sn->setText("CHILD-WINDOW-RUN");
        QTRY_VERIFY(start->isEnabled());
        QTest::mouseClick(start, Qt::LeftButton);
        QTRY_VERIFY(view["canCancel"].toBool());
        QVERIFY(!view["stored"].toBool());
        historyDialog->close();
        logDialog->close();
        QVERIFY(!historyDialog->isVisible());
        QVERIFY(!logDialog->isVisible());
        QVERIFY(window.isVisible());
        window.controller()->logMessage("hidden tool window logging probe", false);
        QTRY_VERIFY(!logs->model()
                         ->match(logs->model()->index(0, 0), Qt::DisplayRole,
                                 "hidden tool window logging probe", 1, Qt::MatchContains)
                         .isEmpty());
        QTRY_VERIFY(view["stored"].toBool() && view["canStart"].toBool());
        QCOMPARE(view["verdict"].toString(), QString("PASS"));
        QTRY_COMPARE(history->model()->rowCount(), 1);
        historyAction->trigger();
        logAction->trigger();
        QCOMPARE(window.findChild<QDialog *>("historyDialog"), historyDialog);
        QCOMPARE(window.findChild<QDialog *>("logDialog"), logDialog);
        QVERIFY(historyDialog->isVisible() && logDialog->isVisible());
        QCOMPARE(history->model()->index(0, 2).data().toString(), "CHILD-WINDOW-RUN");
        window.close();
        QVERIFY(!historyDialog->isVisible());
        QVERIFY(!logDialog->isVisible());
    }
};
QTEST_MAIN(WindowSimulatorTests)
#include "test_window_simulator.moc"
