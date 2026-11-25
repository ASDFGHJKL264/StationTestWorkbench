#include "workbenchcontroller.h"
#include <QtTest>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QDir>
#include <array>

class StationProcess final : public QProcess
{
  public:
    // 退出验证时终止并回收尚未结束的模拟器子进程。
    ~StationProcess() override
    {
        if (state() != NotRunning)
        {
            terminate();
            if (!waitForFinished(500))
            {
                kill();
                waitForFinished(2000);
            }
        }
    }
};
class ProcessTests final : public QObject
{
    Q_OBJECT
  private slots:
    // 验证四个独立模拟器的通过、失败和归档确认重试流程。
    void fourIndependentExecutablesPassFailAndAckRetry()
    {
        // 隔离本次验证的数据库、日志和工位状态。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 持有四个独立模拟器子进程并负责退出清理。
        std::array<StationProcess, 4> children;
        // 收集各模拟工位供客户端连接的地址。
        QList<StationEndpoint> endpoints;
        for (int i = 0; i < 4; ++i)
        {
            // 申请空闲的本机 TCP 端口以避免测试冲突。
            QTcpServer probe;
            QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
            // 保存为当前模拟工位分配的临时端口。
            const auto port = probe.serverPort();
            probe.close();
            // 引用当前要配置并启动的模拟器子进程。
            auto &process = children[i];
            process.setProgram(
                QDir(QCoreApplication::applicationDirPath()).filePath("StationSimulator.exe"));
            // 保存模拟器的端口、工位和故障启动参数。
            QStringList args{"--headless",
                             "--port",
                             QString::number(port),
                             "--station-id",
                             QString::number(i + 1),
                             "--data-dir",
                             directory.filePath(QString("station-%1").arg(i + 1)),
                             "--step-ms",
                             "10"};
            if (i == 2)
                args << "--fail-step" << "current";
            if (i == 3)
                args << "--drop-first-result-ack";
            process.setArguments(args);
            process.start();
            QVERIFY(process.waitForStarted(3000));
            endpoints.append({i + 1, "127.0.0.1", port});
        }
        // 按工位编号缓存客户端收到的最新工位快照。
        QHash<int, QJsonObject> views;
        // 协调客户端连接、测试执行和历史查询。
        WorkbenchController controller;
        // 接收工位快照并更新本次验证保存的状态。
        connect(&controller, &WorkbenchController::stationUpdated, this,
                [&](const QJsonObject &row) { views[row["stationId"].toInt()] = row; });
        controller.initialize(directory.filePath("client"), endpoints);
        for (int i = 1; i <= 4; ++i)
            QTRY_VERIFY_WITH_TIMEOUT(views[i]["canStart"].toBool(), 6000);
        for (int i = 1; i <= 4; ++i)
            controller.startTest(i, QString("PROCESS-BOARD-%1").arg(i), "demo-5v-v1");
        for (int i = 1; i <= 4; ++i)
        {
            QTRY_VERIFY_WITH_TIMEOUT(views[i]["stored"].toBool() && views[i]["canStart"].toBool(),
                                     6000);
            qInfo() << "station final" << i << views.value(i).value("verdict").toString();
            QCOMPARE(views[i]["verdict"].toString(), i == 3 ? QString("FAIL") : QString("PASS"));
            QCOMPARE(views[i]["stepResults"].toArray().size(), 3);
            QCOMPARE(children[i - 1].state(), QProcess::Running);
        }
        // 收集历史查询应答，核对四个独立工位均已归档。
        QSignalSpy history(&controller, &WorkbenchController::historyReady);
        controller.refreshHistory();
        QTRY_VERIFY(!history.isEmpty());
        QCOMPARE(history.last()[0].toJsonArray().size(), 4);
        controller.shutdown();
        qInfo() << "four independent TCP simulator processes completed, station3 FAIL, station4 "
                   "lost ACK recovered";
    }
};
QTEST_GUILESS_MAIN(ProcessTests)
#include "test_process.moc"
