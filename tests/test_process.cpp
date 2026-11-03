#include "runtime/workbenchcontroller.h"
#include <QtTest>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QDir>
#include <array>

class StationProcess final : public QProcess
{
  public:
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
    void fourIndependentExecutablesPassFailAndAckRetry()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        std::array<StationProcess, 4> children;
        QList<StationEndpoint> endpoints;
        for (int i = 0; i < 4; ++i)
        {
            QTcpServer probe;
            QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
            const auto port = probe.serverPort();
            probe.close();
            auto &process = children[i];
            process.setProgram(
                QDir(QCoreApplication::applicationDirPath()).filePath("StationSimulator.exe"));
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
        QHash<int, QJsonObject> views;
        WorkbenchController controller;
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
