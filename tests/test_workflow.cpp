#include "runtime/workbenchcontroller.h"
#include "protocol/framecodec.h"
#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTimer>

// Controlled fault-injection peer. Production simulator is exercised separately.
class Peer : public QTcpServer
{
  public:
    int station = 1, delayMs = 30, starts = 0, acks = 0, dropAcks = 0;
    bool automatic = true, acknowledged = false, reportUnknown = false;
    QTcpSocket *socket = nullptr;
    QString session;
    Wire::Parser parser;
    QJsonObject run, result;
    explicit Peer(int id) : station(id)
    {
        connect(this, &QTcpServer::newConnection, this,
                [this]
                {
                    socket = nextPendingConnection();
                    parser.reset();
                    connect(socket, &QTcpSocket::readyRead, this,
                            [this]
                            {
                                for (const auto &m : parser.append(socket->readAll()))
                                    handle(m);
                            });
                });
    }
    void send(const QString &kind, QJsonObject m = {})
    {
        if (!socket || socket->state() != QAbstractSocket::ConnectedState)
            return;
        m["kind"] = kind;
        m["session"] = session;
        if (!m.contains("sequence"))
            m["sequence"] = 1;
        socket->write(Wire::encode(m));
    }
    void finish(const QString &outcome = "completed")
    {
        QJsonArray values;
        if (outcome == "completed")
            for (const auto &v : run["recipe"].toObject()["steps"].toArray())
            {
                const auto s = v.toObject();
                values.append(QJsonObject{{"stepId", s["id"]}, {"value", s["nominal"]}});
            }
        result = {{"runId", run["runId"]}, {"stationId", station},
                  {"sn", run["sn"]},       {"recipeVersion", run["recipeVersion"]},
                  {"outcome", outcome},    {"steps", values}};
        send("terminal", {{"result", result}});
    }
    void handle(const QJsonObject &m)
    {
        const auto kind = m["kind"].toString();
        if (kind == "hello")
        {
            session = m["session"].toString();
            send("helloAck", {{"sequence", m["sequence"]},
                              {"stationId", station},
                              {"busy", !run.isEmpty() && !acknowledged},
                              {"runId", run["runId"]}});
        }
        else if (m["session"].toString() != session)
            return;
        else if (kind == "ping")
            send("pong", {{"sequence", m["sequence"]}});
        else if (kind == "start")
        {
            run = m["run"].toObject();
            result = {};
            acknowledged = false;
            ++starts;
            send("accepted",
                 {{"sequence", m["sequence"]}, {"runId", run["runId"]}, {"accepted", true}});
            if (automatic)
                QTimer::singleShot(delayMs, this, [this] { finish(); });
        }
        else if (kind == "query")
        {
            const auto id = m["runId"];
            if (reportUnknown || id != run["runId"])
                send("runState", {{"runId", id}, {"state", "unknown"}});
            else if (!result.isEmpty())
                send("runState", {{"runId", id}, {"state", "terminal"}, {"result", result}});
            else
                send("runState", {{"runId", id}, {"state", "executing"}});
        }
        else if (kind == "resultAck")
        {
            ++acks;
            if (acks > dropAcks)
            {
                acknowledged = true;
                send("resultConfirmed", {{"runId", m["runId"]}, {"sequence", m["sequence"]}});
            }
        }
        else if (kind == "cancel")
            finish("cancelled");
    }
};

class WorkflowTests : public QObject
{
    Q_OBJECT
  private slots:
    void parallelRunsAndDeduplicatedTerminal()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Peer one(1), two(2);
        QVERIFY(one.listen(QHostAddress::LocalHost, 0));
        QVERIFY(two.listen(QHostAddress::LocalHost, 0));
        QHash<int, QJsonObject> views;
        WorkbenchController c;
        connect(&c, &WorkbenchController::stationUpdated, this,
                [&](const QJsonObject &v) { views[v["stationId"].toInt()] = v; });
        QSignalSpy ready(&c, &WorkbenchController::ready),
            history(&c, &WorkbenchController::historyReady);
        c.initialize(dir.path(),
                     {{1, "127.0.0.1", one.serverPort()}, {2, "127.0.0.1", two.serverPort()}});
        QTRY_VERIFY(!ready.isEmpty());
        QVERIFY(ready.first()[0].toBool());
        QTRY_VERIFY(views[1]["canStart"].toBool() && views[2]["canStart"].toBool());
        c.startTest(1, "BOARD-A", "demo-5v-v1");
        c.startTest(2, "BOARD-B", "demo-5v-v1");
        QTRY_VERIFY(views[1]["stored"].toBool() && views[2]["stored"].toBool());
        QCOMPARE(views[1]["verdict"].toString(), QString("PASS"));
        QCOMPARE(one.starts, 1);
        QCOMPARE(two.starts, 1);
        QTRY_COMPARE(one.acks, 1);
        for (int i = 0; i < 100; ++i)
            one.send("terminal", {{"result", one.result}});
        QTest::qWait(100);
        history.clear();
        c.refreshHistory();
        QTRY_VERIFY(!history.isEmpty());
        QCOMPARE(history.last()[0].toJsonArray().size(), 2);
        // A conflicting replay of an already archived run cannot poison admission
        // or replace the original measurements.
        auto conflict = one.result;
        auto values = conflict["steps"].toArray();
        auto value = values[0].toObject();
        value["value"] = 5.1;
        values[0] = value;
        conflict["steps"] = values;
        one.send("terminal", {{"result", conflict}});
        QTest::qWait(100);
        QVERIFY(views[1]["stored"].toBool());
        QVERIFY(views[1]["canStart"].toBool());
        c.startTest(1, "AFTER-CONFLICT", "demo-5v-v1");
        QTRY_COMPARE(one.starts, 2);
        QTRY_VERIFY(views[1]["stored"].toBool());
        c.shutdown();
    }
    void lockedStorageMustNotAcknowledgeOrReleaseStation()
    {
        QTemporaryDir dir;
        Peer peer(1);
        peer.automatic = false;
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        QJsonObject view;
        WorkbenchController c;
        connect(&c, &WorkbenchController::stationUpdated, this,
                [&](const QJsonObject &v) { view = v; });
        c.initialize(dir.path(), {{1, "127.0.0.1", peer.serverPort()}});
        QTRY_VERIFY(view["canStart"].toBool());
        c.startTest(1, "LOCKED-SN", "demo-5v-v1");
        QTRY_COMPARE(peer.starts, 1);
        QElapsedTimer uiClock;
        uiClock.start();
        qint64 previousTick = 0, maximumGap = 0;
        int ticks = 0;
        QTimer uiProbe;
        uiProbe.setInterval(10);
        connect(&uiProbe, &QTimer::timeout, this,
                [&]
                {
                    const auto now = uiClock.elapsed();
                    maximumGap = qMax(maximumGap, now - previousTick);
                    previousTick = now;
                    ++ticks;
                });
        uiProbe.start();
        const QString name = "workflow-lock";
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(dir.filePath("data/workbench.db"));
            QVERIFY(db.open());
            QSqlQuery lock(db);
            QVERIFY(lock.exec("BEGIN IMMEDIATE"));
            peer.finish();
            QTRY_COMPARE(view["execution"].toString(), QStringLiteral("保存失败，待重试"));
            QCOMPARE(peer.acks, 0);
            QVERIFY(!view["canStart"].toBool());
            QVERIFY(!view["stored"].toBool());
            QVERIFY(ticks >= 5);
            QVERIFY2(maximumGap < 200, "GUI event processing blocked during database lock");
            QVERIFY(lock.exec("ROLLBACK"));
            QTRY_VERIFY_WITH_TIMEOUT(view["stored"].toBool(), 5000);
            QTRY_COMPARE(peer.acks, 1);
        }
        uiProbe.stop();
        qInfo() << "metrics GUI 10ms timer maximum gap during DB lock" << maximumGap << "ms ticks"
                << ticks;
        QSqlDatabase::removeDatabase(name);
        c.shutdown();
    }
    void reconnectQueriesWithoutRepeatingStart()
    {
        QTemporaryDir dir;
        Peer peer(1);
        peer.automatic = false;
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        QJsonObject view;
        WorkbenchController c;
        connect(&c, &WorkbenchController::stationUpdated, this,
                [&](const QJsonObject &v) { view = v; });
        c.initialize(dir.path(), {{1, "127.0.0.1", peer.serverPort()}});
        QTRY_VERIFY(view["canStart"].toBool());
        c.startTest(1, "RECONNECT", "demo-5v-v1");
        QTRY_COMPARE(peer.starts, 1);
        peer.socket->abort();
        peer.finish();
        QTRY_VERIFY_WITH_TIMEOUT(view["stored"].toBool(), 7000);
        QCOMPARE(peer.starts, 1);
        QCOMPARE(view["verdict"].toString(), QString("PASS"));
        c.shutdown();
    }
    void wrongRunCannotCompleteAndCancelNeedsTerminal()
    {
        QTemporaryDir dir;
        Peer peer(1);
        peer.automatic = false;
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        QJsonObject view;
        WorkbenchController c;
        connect(&c, &WorkbenchController::stationUpdated, this,
                [&](const QJsonObject &v) { view = v; });
        c.initialize(dir.path(), {{1, "127.0.0.1", peer.serverPort()}});
        QTRY_VERIFY(view["canStart"].toBool());
        c.startTest(1, "CANCEL", "demo-5v-v1");
        QTRY_COMPARE(peer.starts, 1);
        auto old = peer.run;
        peer.run["runId"] = "OLD-RUN";
        peer.finish();
        peer.run = old;
        peer.result = {};
        QTest::qWait(100);
        QVERIFY(!view["stored"].toBool());
        QCOMPARE(peer.acks, 0);
        c.cancelTest(1);
        QTRY_VERIFY(view["stored"].toBool());
        QCOMPARE(view["verdict"].toString(), QString("CANCELLED"));
        c.shutdown();
    }
    void lostArchiveAckDoesNotReleaseStationEarly()
    {
        QTemporaryDir dir;
        Peer peer(1);
        peer.dropAcks = 1;
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        QJsonObject view;
        WorkbenchController c;
        connect(&c, &WorkbenchController::stationUpdated, this,
                [&](const QJsonObject &v) { view = v; });
        c.initialize(dir.path(), {{1, "127.0.0.1", peer.serverPort()}});
        QTRY_VERIFY(view["canStart"].toBool());
        c.startTest(1, "ACK-LOSS", "demo-5v-v1");
        QTRY_VERIFY(view["stored"].toBool());
        QVERIFY(!view["canStart"].toBool());
        QCOMPARE(peer.starts, 1);
        c.startTest(1, "MUST-NOT-DISPATCH", "demo-5v-v1");
        QTRY_VERIFY_WITH_TIMEOUT(view["canStart"].toBool(), 3000);
        QVERIFY(peer.acks >= 2);
        QCOMPARE(peer.starts, 1);
        c.shutdown();
    }
    void sameProductCannotExecuteAtTwoStationsSimultaneously()
    {
        QTemporaryDir dir;
        Peer one(1), two(2);
        one.automatic = two.automatic = false;
        QVERIFY(one.listen(QHostAddress::LocalHost, 0));
        QVERIFY(two.listen(QHostAddress::LocalHost, 0));
        QHash<int, QJsonObject> views;
        WorkbenchController c;
        connect(&c, &WorkbenchController::stationUpdated, this,
                [&](const QJsonObject &v) { views[v["stationId"].toInt()] = v; });
        c.initialize(dir.path(),
                     {{1, "127.0.0.1", one.serverPort()}, {2, "127.0.0.1", two.serverPort()}});
        QTRY_VERIFY(views[1]["canStart"].toBool() && views[2]["canStart"].toBool());
        c.startTest(1, "ONE-PRODUCT", "demo-5v-v1");
        c.startTest(2, "ONE-PRODUCT", "demo-5v-v1");
        QTRY_VERIFY(one.starts + two.starts == 1);
        QTRY_VERIFY(views[1]["execution"].toString() == QStringLiteral("创建失败") ||
                    views[2]["execution"].toString() == QStringLiteral("创建失败"));
        const int first = one.starts ? 1 : 2;
        c.cancelTest(first);
        QTRY_VERIFY(views[first]["stored"].toBool() && views[first]["canStart"].toBool());
        // A retest is a new run, with the same SN and a new immutable identity.
        const auto original = views[first]["runId"].toString();
        c.startTest(first, "ONE-PRODUCT", "demo-5v-v1");
        QTRY_VERIFY(views[first]["runId"].toString() != original && one.starts + two.starts == 2);
        c.cancelTest(first);
        QTRY_VERIFY(views[first]["stored"].toBool());
        c.shutdown();
    }
    void clientRestartHoldsIdentityAndAllowsCancellationAfterReconciliation()
    {
        QTemporaryDir dir;
        Peer one(1), two(2);
        one.automatic = two.automatic = false;
        QVERIFY(one.listen(QHostAddress::LocalHost, 0));
        QVERIFY(two.listen(QHostAddress::LocalHost, 0));
        const QList<StationEndpoint> endpoints{{1, "127.0.0.1", one.serverPort()},
                                               {2, "127.0.0.1", two.serverPort()}};
        {
            QHash<int, QJsonObject> views;
            WorkbenchController first;
            connect(&first, &WorkbenchController::stationUpdated, this,
                    [&](const QJsonObject &v) { views[v["stationId"].toInt()] = v; });
            first.initialize(dir.path(), endpoints);
            QTRY_VERIFY(views[1]["canStart"].toBool());
            first.startTest(1, "RESTART-HELD-SN", "demo-5v-v1");
            QTRY_COMPARE(one.starts, 1);
            first.shutdown();
        }
        QHash<int, QJsonObject> views;
        WorkbenchController recovered;
        connect(&recovered, &WorkbenchController::stationUpdated, this,
                [&](const QJsonObject &v) { views[v["stationId"].toInt()] = v; });
        recovered.initialize(dir.path(), endpoints);
        QTRY_VERIFY(views[1]["canCancel"].toBool() && views[2]["canStart"].toBool());
        QCOMPARE(views[1]["sn"].toString(), QString("RESTART-HELD-SN"));
        recovered.startTest(2, "RESTART-HELD-SN", "demo-5v-v1");
        QTRY_COMPARE(views[2]["execution"].toString(), QStringLiteral("创建失败"));
        QCOMPARE(two.starts, 0);
        recovered.cancelTest(1);
        QTRY_VERIFY(views[1]["stored"].toBool() && views[1]["canStart"].toBool());
        QCOMPARE(views[1]["verdict"].toString(), QString("CANCELLED"));
        QCOMPARE(one.starts, 1);
        recovered.startTest(2, "RESTART-HELD-SN", "demo-5v-v1");
        QTRY_COMPARE(two.starts, 1);
        two.finish();
        QTRY_VERIFY(views[2]["stored"].toBool());
        recovered.shutdown();
    }
    void lateTerminalWinsOverFailedInterruptionRelease()
    {
        QTemporaryDir dir;
        Peer peer(1);
        peer.automatic = false;
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        QJsonObject view;
        WorkbenchController c;
        connect(&c, &WorkbenchController::stationUpdated, this,
                [&](const QJsonObject &row) { view = row; });
        c.initialize(dir.path(), {{1, "127.0.0.1", peer.serverPort()}});
        QTRY_VERIFY(view["canStart"].toBool());
        c.startTest(1, "LATE-TERMINAL", "demo-5v-v1");
        QTRY_COMPARE(peer.starts, 1);
        const QString connection = "interruption-lock";
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(dir.filePath("data/workbench.db"));
            QVERIFY(db.open());
            QSqlQuery lock(db);
            QVERIFY(lock.exec("BEGIN IMMEDIATE"));
            peer.reportUnknown = true;
            peer.socket->abort();
            QTRY_COMPARE_WITH_TIMEOUT(view["execution"].toString(),
                                      QStringLiteral("中断归档失败，保持占用"), 7000);
            QVERIFY(!view["canStart"].toBool());
            QVERIFY(lock.exec("ROLLBACK"));
            peer.finish();
            QTRY_VERIFY(view["stored"].toBool() && view["canStart"].toBool());
            QCOMPARE(view["verdict"].toString(), QString("PASS"));
            QTest::qWait(1200);
            QCOMPARE(view["verdict"].toString(), QString("PASS"));
            QVERIFY(view["stored"].toBool());
            QVERIFY(view["canStart"].toBool());
            QCOMPARE(peer.starts, 1);
        }
        QSqlDatabase::removeDatabase(connection);
        c.shutdown();
    }
    void batchRunsHaveOneTerminalEach()
    {
        QTemporaryDir dir;
        Peer one(1), two(2);
        one.delayMs = two.delayMs = 1;
        QVERIFY(one.listen(QHostAddress::LocalHost, 0));
        QVERIFY(two.listen(QHostAddress::LocalHost, 0));
        QHash<int, QJsonObject> views;
        WorkbenchController c;
        connect(&c, &WorkbenchController::stationUpdated, this,
                [&](const QJsonObject &v) { views[v["stationId"].toInt()] = v; });
        c.initialize(dir.path(),
                     {{1, "127.0.0.1", one.serverPort()}, {2, "127.0.0.1", two.serverPort()}});
        QTRY_VERIFY(views[1]["canStart"].toBool() && views[2]["canStart"].toBool());
        bool countOk = false;
        const auto configured = qEnvironmentVariableIntValue("WORKBENCH_BATCH_RUNS", &countOk);
        const int runs = countOk ? qBound(1, configured, 10000) : 100;
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < runs; ++i)
        {
            const int station = i % 2 + 1;
            const auto previous = views[station]["runId"].toString();
            c.startTest(station, QString("BATCH-%1").arg(i), "demo-5v-v1");
            QTRY_VERIFY(views[station]["runId"].toString() != previous &&
                        views[station]["stored"].toBool() && views[station]["canStart"].toBool());
        }
        QSignalSpy history(&c, &WorkbenchController::historyReady);
        int cursor = 0, total = 0;
        QSet<QString> unique;
        bool more = true;
        while (more)
        {
            history.clear();
            c.refreshHistory({}, cursor);
            QTRY_VERIFY(!history.isEmpty());
            const auto rows = history.last()[0].toJsonArray();
            more = history.last()[1].toBool();
            QVERIFY(!rows.isEmpty());
            for (const auto &value : rows)
            {
                const auto row = value.toObject();
                QCOMPARE(row["verdict"].toString(), QString("PASS"));
                QCOMPARE(row["steps"].toArray().size(), 3);
                unique.insert(row["runId"].toString());
                ++total;
            }
            cursor = rows.last().toObject()["id"].toInt();
        }
        QCOMPARE(total, runs);
        QCOMPARE(unique.size(), runs);
        QCOMPARE(one.starts + two.starts, runs);
        qInfo() << "batch runs" << runs << "elapsed ms" << timer.elapsed();
        c.shutdown();
    }
};
QTEST_GUILESS_MAIN(WorkflowTests)
#include "test_workflow.moc"
