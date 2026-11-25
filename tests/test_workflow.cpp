#include "workbenchcontroller.h"
#include "framecodec.h"
#include "testengine.h"
#include "domain.h"
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
    // station 为模拟工位编号，delayMs 为自动完成延迟。
    // starts、acks 记录动作及归档确认次数，dropAcks 指定丢弃次数。
    int station = 1, delayMs = 30, starts = 0, acks = 0, dropAcks = 0;
    // automatic 控制自动完成，acknowledged 标记归档确认，reportUnknown 注入未知状态。
    bool automatic = true, acknowledged = false, reportUnknown = false;
    // connections、pings、queries、cancels 分别统计连接、心跳、查询和取消次数。
    // dropQueries 指定前几次状态查询不作应答。
    int connections = 0, pings = 0, queries = 0, cancels = 0, dropQueries = 0;
    // dropAccepted 丢弃受理应答，ignoreCancellation 忽略取消动作以模拟异常。
    bool dropAccepted = false, ignoreCancellation = false;
    // 测量查询退避和测试阶段耗时的单调计时器。
    QElapsedTimer clock;
    // 记录每次状态查询到达的时间以核对退避间隔。
    QList<qint64> queryTimes;
    // 当前工作台客户端建立的 TCP 连接。
    QTcpSocket *socket = nullptr;
    // 当前连接握手确认的会话标识。
    QString session;
    // 从客户端接收字节中恢复完整业务报文的解析器。
    Wire::Parser parser;
    // run 保存受理请求，result 保存可供查询和重传的测试终态。
    QJsonObject run, result;
    // 创建 id 对应的故障注入工位，并安装连接处理回调。
    explicit Peer(int id) : station(id)
    {
        clock.start();
        connect(this, &QTcpServer::newConnection, this,
                // 接收新客户端并重置本次会话的报文解析状态。
                [this]
                {
                    ++connections;
                    socket = nextPendingConnection();
                    parser.reset();
                    connect(socket, &QTcpSocket::readyRead, this,
                            // 读取客户端报文并交给故障注入处理逻辑。
                            [this]
                            {
                                for (const auto &m : parser.append(socket->readAll()))
                                    handle(m);
                            });
                });
    }
    // 发送 kind 类型的报文，m 提供附加业务字段。
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
    // 按 outcome 构造终态，transmit 控制是否立即发给客户端。
    void finish(const QString &outcome = "completed", bool transmit = true)
    {
        // 按配方顺序保存需要回传的测量值。
        QJsonArray values;
        if (outcome == "completed")
            for (const auto &v : run["recipe"].toObject()["steps"].toArray())
            {
                // 为当前测量值提供步骤身份和标称值的规则。
                const auto s = v.toObject();
                values.append(QJsonObject{{"stepId", s["id"]}, {"value", s["nominal"]}});
            }
        result = {{"runId", run["runId"]}, {"stationId", station},
                  {"sn", run["sn"]},       {"recipeVersion", run["recipeVersion"]},
                  {"outcome", outcome},    {"steps", values}};
        if (transmit)
            send("terminal", {{"result", result}});
    }
    // 按 index 发送一次有效步骤进度，供超时与重复进度验证。
    void progress(int index)
    {
        // 从本次运行配方中取得对应步骤身份和标称值。
        const auto step = run["recipe"].toObject()["steps"].toArray()[index].toObject();
        send("progress", {{"runId", run["runId"]}, {"stepIndex", index},
                          {"stepId", step["id"]}, {"value", step["nominal"]}});
    }
    // 根据 m 的业务类型模拟握手、测试、查询及归档应答。
    void handle(const QJsonObject &m)
    {
        // 本次客户端报文要求执行的业务类型。
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
        {
            ++pings;
            send("pong", {{"sequence", m["sequence"]}});
        }
        else if (kind == "start")
        {
            run = m["run"].toObject();
            result = {};
            acknowledged = false;
            ++starts;
            if (!dropAccepted)
                send("accepted",
                     {{"sequence", m["sequence"]}, {"runId", run["runId"]}, {"accepted", true}});
            if (automatic)
                QTimer::singleShot(delayMs, this, [this] { finish(); });
        }
        else if (kind == "query")
        {
            ++queries;
            queryTimes.append(clock.elapsed());
            if (queries <= dropQueries)
                return;
            // 客户端要求核对的原测试编号。
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
        {
            ++cancels;
            if (!ignoreCancellation)
                finish("cancelled");
        }
    }
};

// 构造直接引擎验证使用的完整测试身份和配方快照。
QJsonObject engineRequest(const QString &runId)
{
    // 绑定内置配方，确保故障注入仍经过实际身份与质量校验。
    const auto recipe = Domain::defaultRecipe();
    return {{"runId", runId}, {"stationId", 1}, {"sn", runId},
            {"recipeVersion", recipe["version"]}, {"recipe", recipe}};
}

class WorkflowTests : public QObject
{
    Q_OBJECT
  private slots:
    // 验证心跳正常的步骤停滞会超时，迟到真实终态仍须归档及确认。
    void stalledExecutionTimesOutButLateTerminalRemainsAuthoritative()
    {
        // 保持设备在线并忽略取消，模拟检测流程卡住。
        Peer peer(1);
        peer.automatic = false;
        peer.ignoreCancellation = true;
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        // 直接驱动引擎以明确区分收到终态和事务提交。
        TestEngine engine;
        engine.configureTimeouts(5000, 1200);
        // 缓存用于检查占用和真实判定的界面快照。
        QJsonObject view;
        connect(&engine, &TestEngine::stationUpdated, this,
                [&](const QJsonObject &value) { view = value; });
        connect(&engine, &TestEngine::reserveRequested, &engine,
                [&](const QJsonObject &run) { engine.reserved(run, true, {}); });
        // timeout、saved 分别记录超时事件和设备真实终态。
        QSignalSpy timeout(&engine, &TestEngine::runTimedOut),
            saved(&engine, &TestEngine::saveRequested);
        engine.initialize({{1, "127.0.0.1", peer.serverPort()}});
        QTRY_VERIFY(view["canStart"].toBool());
        engine.requestStart(engineRequest("LATE-AFTER-TIMEOUT"));
        QTRY_VERIFY_WITH_TIMEOUT(view["timedOut"].toBool(), 3500);
        QCOMPARE(timeout.size(), 1);
        QVERIFY(timeout.first()[2].toString().contains(QStringLiteral("步骤")));
        QVERIFY(view["online"].toBool());
        QVERIFY(peer.pings >= 1);
        QCOMPARE(peer.connections, 1);
        QCOMPARE(peer.starts, 1);
        QTRY_COMPARE(peer.cancels, 1);
        QTRY_VERIFY(peer.queries >= 1);
        QVERIFY(!view["canStart"].toBool());
        QVERIFY(!view["stored"].toBool());
        QCOMPARE(view["verdict"].toString(), QString("PENDING"));
        QCOMPARE(saved.size(), 0);
        QTest::qWait(500);
        QCOMPARE(peer.cancels, 1);
        peer.finish();
        QTRY_COMPARE(saved.size(), 1);
        QVERIFY(!view["canStart"].toBool());
        // 终态已收到仍不能绕过归档门禁进行恢复切换。
        QSignalSpy restore(&engine, &TestEngine::restorePrepared);
        engine.prepareRestore();
        QCOMPARE(restore.size(), 1);
        QVERIFY(!restore.first()[0].toBool());
        engine.resultSaved(saved.first()[0].toJsonObject(), true, false, {});
        QTRY_VERIFY(view["canStart"].toBool());
        QVERIFY(view["stored"].toBool());
        QVERIFY(view["timedOut"].toBool());
        QCOMPARE(view["verdict"].toString(), QString("PASS"));
        QCOMPARE(timeout.size(), 1);
        QCOMPARE(peer.starts, 1);
        engine.shutdown();
    }

    // 验证重复步骤和运行期间修改配置都不能延后已捕获的步骤期限。
    void repeatedProgressAndNewSettingsCannotExtendCurrentStepDeadline()
    {
        // 反复回报同一测量值，但不返回检测终态。
        Peer peer(1);
        peer.automatic = false;
        peer.ignoreCancellation = true;
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        // 在首个新步骤后采用本轮固定的短步骤期限。
        TestEngine engine;
        engine.configureTimeouts(5000, 500);
        // 缓存引擎是否仍占用原测试的状态。
        QJsonObject view;
        connect(&engine, &TestEngine::stationUpdated, this,
                [&](const QJsonObject &value) { view = value; });
        connect(&engine, &TestEngine::reserveRequested, &engine,
                [&](const QJsonObject &run) { engine.reserved(run, true, {}); });
        // 只接受一次步骤超时事件。
        QSignalSpy timeout(&engine, &TestEngine::runTimedOut);
        engine.initialize({{1, "127.0.0.1", peer.serverPort()}});
        QTRY_VERIFY(view["canStart"].toBool());
        engine.requestStart(engineRequest("REPEAT-PROGRESS"));
        QTRY_COMPARE(peer.starts, 1);
        peer.progress(0);
        QTRY_VERIFY(view["stepResults"].toArray()[0].toObject().contains("value"));
        engine.configureTimeouts(20000, 20000);
        // 每80毫秒重复同一步，验证重复报文不会无限续期。
        QTimer repeats;
        connect(&repeats, &QTimer::timeout, &peer, [&] { peer.progress(0); });
        repeats.start(80);
        QTRY_VERIFY_WITH_TIMEOUT(view["timedOut"].toBool(), 2200);
        repeats.stop();
        QCOMPARE(timeout.size(), 1);
        QVERIFY(timeout.first()[2].toString().contains("500"));
        QCOMPARE(peer.starts, 1);
        QTRY_COMPARE(peer.cancels, 1);
        QVERIFY(!view["stored"].toBool());
        QVERIFY(!view["canStart"].toBool());
        engine.shutdown();
    }

    // 验证整轮期限独立于步骤进度，且断线后不会重新开始执行计时。
    void runDeadlineAndReconnectPreserveOriginalExecutionBudget()
    {
        // 在心跳持续的情况下使整轮时间先于步骤期限到期。
        Peer peer(1);
        peer.automatic = false;
        peer.ignoreCancellation = true;
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        // 为本轮设置固定整轮期限，并给步骤更长的等待时间。
        TestEngine engine;
        engine.configureTimeouts(650, 10000);
        // 保存最新状态，观察断线期间的超时记录。
        QJsonObject view;
        connect(&engine, &TestEngine::stationUpdated, this,
                [&](const QJsonObject &value) { view = value; });
        connect(&engine, &TestEngine::reserveRequested, &engine,
                [&](const QJsonObject &run) { engine.reserved(run, true, {}); });
        // 收集整轮超时事件，断线和重连不得重复记录。
        QSignalSpy timeout(&engine, &TestEngine::runTimedOut);
        engine.initialize({{1, "127.0.0.1", peer.serverPort()}});
        QTRY_VERIFY(view["canStart"].toBool());
        engine.requestStart(engineRequest("RUN-TIMEOUT-RECONNECT"));
        QTRY_COMPARE(peer.starts, 1);
        peer.progress(0);
        engine.configureTimeouts(20000, 20000);
        peer.socket->abort();
        QTRY_VERIFY(!view["online"].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(view["timedOut"].toBool(), 2000);
        QCOMPARE(timeout.size(), 1);
        QVERIFY(timeout.first()[2].toString().contains(QStringLiteral("整轮")));
        QVERIFY(!view["canStart"].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(peer.connections >= 2 && view["online"].toBool(), 3500);
        QTRY_COMPARE(peer.cancels, 1);
        QCOMPARE(peer.starts, 1);
        QCOMPARE(timeout.size(), 1);
        engine.shutdown();
    }

    // 验证人工处置冻结的事务失败不丢占用，成功后停用并隔离旧设备。
    void manualDispositionFreezesReconnectAndQuarantinesLateHello()
    {
        // 预置重启前的原运行，设备恢复时仍宣称该运行占用。
        Peer peer(1);
        peer.automatic = false;
        peer.run = engineRequest("MANUAL-OFFLINE");
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        // 保存监听端口，断开后可在同一地址恢复原设备。
        const auto port = peer.serverPort();
        // 以恢复运行初始化引擎，模拟原设备后来彻底离线。
        TestEngine engine;
        // 缓存人工处置与占用状态。
        QJsonObject view;
        connect(&engine, &TestEngine::stationUpdated, this,
                [&](const QJsonObject &value) { view = value; });
        // prepared 记录冻结，suspended 记录迟到原运行引起的再次停用。
        QSignalSpy prepared(&engine, &TestEngine::manualDispositionPrepared),
            suspended(&engine, &TestEngine::stationSuspensionRequired);
        engine.initialize({{1, "127.0.0.1", port}}, {peer.run});
        QTRY_VERIFY(view["online"].toBool());
        engine.prepareManualDisposition(1, "MANUAL-OFFLINE");
        QCOMPARE(prepared.size(), 1);
        QVERIFY(!prepared.last()[2].toBool());
        peer.close();
        peer.socket->abort();
        QTRY_VERIFY(view["canManualDisposition"].toBool());
        engine.prepareManualDisposition(1, "MANUAL-OFFLINE");
        QVERIFY(prepared.last()[2].toBool());
        QVERIFY(view["manualPending"].toBool());
        QVERIFY(view["busy"].toBool());
        QVERIFY(peer.listen(QHostAddress::LocalHost, port));
        QTest::qWait(1300);
        QCOMPARE(peer.connections, 1);
        engine.manualDispositionRecorded(1, "MANUAL-OFFLINE", false, "storage failure");
        QVERIFY(!view["manualPending"].toBool());
        QCOMPARE(view["runId"].toString(), QString("MANUAL-OFFLINE"));
        QVERIFY(!view["canStart"].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(peer.connections == 2 && view["online"].toBool(), 3000);
        peer.close();
        peer.socket->abort();
        QTRY_VERIFY(view["canManualDisposition"].toBool());
        engine.prepareManualDisposition(1, "MANUAL-OFFLINE");
        engine.manualDispositionRecorded(1, "MANUAL-OFFLINE", true, {});
        QVERIFY(view["safetySuspended"].toBool());
        QVERIFY(!view["busy"].toBool());
        QCOMPARE(view["verdict"].toString(), QString("UNKNOWN"));
        QVERIFY(!view["stored"].toBool());
        QVERIFY(peer.listen(QHostAddress::LocalHost, port));
        engine.resumeStation(1);
        QTRY_COMPARE_WITH_TIMEOUT(suspended.size(), 1, 3000);
        QVERIFY(view["safetySuspended"].toBool());
        QVERIFY(!view["canStart"].toBool());
        QCOMPARE(peer.acks, 0);
        QCOMPARE(peer.starts, 0);
        engine.shutdown();
    }

    // 验证停用在重启后生效，人工结算的迟到终态不会进入结果保存队列。
    void persistedSuspensionAndLateTerminalCannotReopenClosedRun()
    {
        // 已人工结算的设备结果留存供恢复接入后注入。
        Peer peer(1);
        peer.automatic = false;
        peer.run = engineRequest("CLOSED-AFTER-RESTART");
        peer.finish("completed", false);
        peer.run = {};
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        // 启动前加载管理员处置与停用状态。
        TestEngine engine;
        engine.loadSafetyState({QJsonObject{{"runId", "CLOSED-AFTER-RESTART"}}},
                               {QJsonObject{{"stationId", 1}}});
        // 缓存是否隔离旧运行并保持工位停用。
        QJsonObject view;
        connect(&engine, &TestEngine::stationUpdated, this,
                [&](const QJsonObject &value) { view = value; });
        // saved 必须为空，suspended 应记录迟到终态的安全停用。
        QSignalSpy saved(&engine, &TestEngine::saveRequested),
            suspended(&engine, &TestEngine::stationSuspensionRequired);
        engine.initialize({{1, "127.0.0.1", peer.serverPort()}});
        QTest::qWait(250);
        QCOMPARE(peer.connections, 0);
        QVERIFY(view["safetySuspended"].toBool());
        engine.resumeStation(1);
        QTRY_VERIFY(view["canStart"].toBool());
        peer.send("terminal", {{"result", peer.result}});
        QTRY_COMPARE(suspended.size(), 1);
        QVERIFY(view["safetySuspended"].toBool());
        QVERIFY(!view["canStart"].toBool());
        QCOMPARE(saved.size(), 0);
        QCOMPARE(peer.acks, 0);
        engine.shutdown();
    }

    // 验证尚未落库的真实终态不能被管理员处置丢弃。
    void manualDispositionRejectsTerminalAwaitingStorage()
    {
        // 设备正常返回终态，但测试故意不确认数据库提交。
        Peer peer(1);
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        // 使用直接引擎控制终态待保存阶段。
        TestEngine engine;
        // 保存离线与人工处置按钮状态。
        QJsonObject view;
        connect(&engine, &TestEngine::stationUpdated, this,
                [&](const QJsonObject &value) { view = value; });
        connect(&engine, &TestEngine::reserveRequested, &engine,
                [&](const QJsonObject &run) { engine.reserved(run, true, {}); });
        // 收集终态及人工冻结回报，确认没有绕过归档。
        QSignalSpy saved(&engine, &TestEngine::saveRequested),
            prepared(&engine, &TestEngine::manualDispositionPrepared);
        engine.initialize({{1, "127.0.0.1", peer.serverPort()}});
        QTRY_VERIFY(view["canStart"].toBool());
        engine.requestStart(engineRequest("STORAGE-MUST-WIN"));
        QTRY_COMPARE(saved.size(), 1);
        peer.close();
        peer.socket->abort();
        QTRY_VERIFY(!view["online"].toBool());
        QVERIFY(!view["canManualDisposition"].toBool());
        engine.prepareManualDisposition(1, "STORAGE-MUST-WIN");
        QCOMPARE(prepared.size(), 1);
        QVERIFY(!prepared.first()[2].toBool());
        QVERIFY(!view["manualPending"].toBool());
        QVERIFY(view["busy"].toBool());
        QCOMPARE(peer.acks, 0);
        engine.shutdown();
    }

    // 验证恢复数据门禁会冻结闲置连接，并拒绝正在运行的工位。
    void restorePauseIsAtomicAndRejectsActiveRun()
    {
        // 保持测试运行，便于核对恢复切换的拒绝条件。
        Peer peer(1);
        peer.automatic = false;
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        // 直接检查新测试请求是否被门禁阻止。
        TestEngine engine;
        // 缓存接入暂停前后的按钮状态。
        QJsonObject view;
        connect(&engine, &TestEngine::stationUpdated, this,
                [&](const QJsonObject &value) { view = value; });
        connect(&engine, &TestEngine::reserveRequested, &engine,
                [&](const QJsonObject &run) { engine.reserved(run, true, {}); });
        // 跟踪恢复门禁和预约请求，保证暂停期间完全没有新动作。
        QSignalSpy restore(&engine, &TestEngine::restorePrepared),
            reserved(&engine, &TestEngine::reserveRequested);
        engine.initialize({{1, "127.0.0.1", peer.serverPort()}});
        QTRY_VERIFY(view["canStart"].toBool());
        engine.prepareRestore();
        QCOMPARE(restore.size(), 1);
        QVERIFY(restore.last()[0].toBool());
        QVERIFY(!view["canStart"].toBool());
        engine.requestStart(engineRequest("BLOCKED-DURING-RESTORE"));
        QTest::qWait(150);
        QCOMPARE(reserved.size(), 0);
        QCOMPARE(peer.starts, 0);
        engine.finishRestorePause();
        QTRY_VERIFY(view["canStart"].toBool());
        engine.requestStart(engineRequest("RESTORE-ACTIVE"));
        QTRY_COMPARE(peer.starts, 1);
        engine.prepareRestore();
        QCOMPARE(restore.size(), 2);
        QVERIFY(!restore.last()[0].toBool());
        QVERIFY(view["online"].toBool());
        QVERIFY(view["busy"].toBool());
        engine.shutdown();
    }

    // 验证两工位并行测试及重复、冲突终态的幂等处理。
    void parallelRunsAndDeduplicatedTerminal()
    {
        // 隔离保存本用例数据库和日志的临时目录。
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // one、two 分别模拟工位1和工位2的独立服务。
        Peer one(1), two(2);
        QVERIFY(one.listen(QHostAddress::LocalHost, 0));
        QVERIFY(two.listen(QHostAddress::LocalHost, 0));
        // 按工位编号缓存工作台最新状态快照。
        QHash<int, QJsonObject> views;
        // 协调测试流程及后台线程的待测工作台控制器。
        WorkbenchController c;
        connect(&c, &WorkbenchController::stationUpdated, this,
                // 接收 v 工位快照并更新对应工位的测试状态。
                [&](const QJsonObject &v) { views[v["stationId"].toInt()] = v; });
        // ready 记录后台就绪回报，history 记录历史查询结果。
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
        // 修改已归档终态构造冲突重放的副本。
        auto conflict = one.result;
        // 待修改的冲突终态步骤数组。
        auto values = conflict["steps"].toArray();
        // 用于注入不同测量值的首个步骤结果。
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
    // 验证数据库被锁时不确认结果、不释放工位且界面仍响应。
    void lockedStorageMustNotAcknowledgeOrReleaseStation()
    {
        // 隔离保存本用例数据库和日志的临时目录。
        QTemporaryDir dir;
        // 提供单工位真实 TCP 交互及受控故障的测试端点。
        Peer peer(1);
        peer.automatic = false;
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        // 保存当前工位状态与操作可用性的最新快照。
        QJsonObject view;
        // 协调测试流程及后台线程的待测工作台控制器。
        WorkbenchController c;
        connect(&c, &WorkbenchController::stationUpdated, this,
                // 接收 v 工位快照以跟踪等待归档和恢复过程。
                [&](const QJsonObject &v) { view = v; });
        c.initialize(dir.path(), {{1, "127.0.0.1", peer.serverPort()}});
        QTRY_VERIFY(view["canStart"].toBool());
        c.startTest(1, "LOCKED-SN", "demo-5v-v1");
        QTRY_COMPARE(peer.starts, 1);
        // 测量数据库阻塞期间主线程定时器响应的计时基准。
        QElapsedTimer uiClock;
        uiClock.start();
        // previousTick 记录上次触发时刻，maximumGap 保存最大触发间隔。
        qint64 previousTick = 0, maximumGap = 0;
        // 累计数据库阻塞期间主线程仍处理的定时器次数。
        int ticks = 0;
        // 用10毫秒间隔探测主线程事件循环是否被阻塞。
        QTimer uiProbe;
        uiProbe.setInterval(10);
        connect(&uiProbe, &QTimer::timeout, this,
                // 采样当前触发时刻并更新最大间隔和触发次数。
                [&]
                {
                    // 本次主线程响应采样相对起点的毫秒数。
                    const auto now = uiClock.elapsed();
                    maximumGap = qMax(maximumGap, now - previousTick);
                    previousTick = now;
                    ++ticks;
                });
        uiProbe.start();
        // 用于注入数据库写锁的独立连接名称。
        const QString name = "workflow-lock";
        {
            // 在测试线程中持有写锁的独立数据库连接。
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(dir.filePath("data/workbench.db"));
            QVERIFY(db.open());
            // 申请及释放数据库写锁的控制查询。
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
    // 验证断线后查询原测试，避免重复执行 start。
    void reconnectQueriesWithoutRepeatingStart()
    {
        // 隔离保存本用例数据库和日志的临时目录。
        QTemporaryDir dir;
        // 提供单工位真实 TCP 交互及受控故障的测试端点。
        Peer peer(1);
        peer.automatic = false;
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        // 保存当前工位状态与操作可用性的最新快照。
        QJsonObject view;
        // 协调测试流程及后台线程的待测工作台控制器。
        WorkbenchController c;
        connect(&c, &WorkbenchController::stationUpdated, this,
                // 接收 v 工位快照以观察断线及重新核对结果。
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
    // 验证心跳正常但查询应答丢失时按退避间隔继续只读核对。
    void missingQueryRepliesRetryWithBackoffWhileHeartbeatRemainsHealthy()
    {
        // 隔离保存本用例数据库和日志的临时目录。
        QTemporaryDir dir;
        // 可丢弃受理和查询应答的故障注入工位。
        Peer peer(1);
        peer.automatic = false;
        peer.dropAccepted = true;
        peer.dropQueries = 2;
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        // 保存当前工位状态与操作可用性的最新快照。
        QJsonObject view;
        // 协调测试流程及后台线程的待测工作台控制器。
        WorkbenchController c;
        connect(&c, &WorkbenchController::stationUpdated, this,
                // 接收 v 工位快照以确认未知结果始终保留占用。
                [&](const QJsonObject &v) { view = v; });
        c.initialize(dir.path(), {{1, "127.0.0.1", peer.serverPort()}});
        QTRY_VERIFY(view["canStart"].toBool());
        c.startTest(1, "QUERY-RETRY", "demo-5v-v1");
        QTRY_COMPARE(peer.starts, 1);
        QTRY_COMPARE_WITH_TIMEOUT(peer.queries, 2, 12000);
        QVERIFY(view["online"].toBool());
        QVERIFY(!view["canStart"].toBool());
        QVERIFY(!view["stored"].toBool());
        QCOMPARE(view["verdict"].toString(), QString("PENDING"));
        QCOMPARE(peer.acks, 0);
        QCOMPARE(peer.starts, 1);
        QCOMPARE(peer.connections, 1);
        QVERIFY(peer.pings >= 5);
        // The terminal is available only through the next read-only query.
        peer.finish("completed", false);
        QTRY_VERIFY_WITH_TIMEOUT(view["stored"].toBool() && view["canStart"].toBool(), 10000);
        QCOMPARE(peer.queries, 3);
        QVERIFY(peer.queryTimes[1] - peer.queryTimes[0] >= 3900);
        QVERIFY(peer.queryTimes[2] - peer.queryTimes[1] >= 4900);
        QCOMPARE(peer.starts, 1);
        QCOMPARE(peer.connections, 1);
        QCOMPARE(view["verdict"].toString(), QString("PASS"));
        QVERIFY(!view["canReconcile"].toBool());
        c.shutdown();
    }

    // 验证取消终态缺失时持续查状态且不重复发送测试或取消。
    void missingCancellationTerminalIsQueriedWithoutRepeatingActions()
    {
        // 隔离保存本用例数据库和日志的临时目录。
        QTemporaryDir dir;
        // 可忽略取消动作的故障注入工位。
        Peer peer(1);
        peer.automatic = false;
        peer.ignoreCancellation = true;
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        // 保存当前工位状态与操作可用性的最新快照。
        QJsonObject view;
        // 协调测试流程及后台线程的待测工作台控制器。
        WorkbenchController c;
        connect(&c, &WorkbenchController::stationUpdated, this,
                // 接收 v 工位快照以跟踪取消核对和真实归档。
                [&](const QJsonObject &v) { view = v; });
        c.initialize(dir.path(), {{1, "127.0.0.1", peer.serverPort()}});
        QTRY_VERIFY(view["canStart"].toBool());
        c.startTest(1, "CANCEL-RECONCILE", "demo-5v-v1");
        QTRY_COMPARE(view["execution"].toString(), QStringLiteral("执行中"));
        c.cancelTest(1);
        QTRY_COMPARE(peer.cancels, 1);
        QTRY_VERIFY_WITH_TIMEOUT(peer.queries >= 2, 10000);
        QTRY_COMPARE(view["execution"].toString(), QStringLiteral("取消处理中"));
        QVERIFY(view["online"].toBool());
        QVERIFY(!view["canStart"].toBool());
        QVERIFY(!view["canCancel"].toBool());
        QVERIFY(!view["stored"].toBool());
        QCOMPARE(peer.acks, 0);
        QCOMPARE(peer.cancels, 1);
        QCOMPARE(peer.starts, 1);
        QVERIFY(peer.pings >= 4);
        peer.finish("cancelled", false);
        QTRY_VERIFY_WITH_TIMEOUT(view["stored"].toBool() && view["canStart"].toBool(), 6000);
        QCOMPARE(view["verdict"].toString(), QString("CANCELLED"));
        QCOMPARE(peer.cancels, 1);
        QCOMPARE(peer.starts, 1);
        QCOMPARE(peer.connections, 1);
        QVERIFY(!view["canReconcile"].toBool());
        c.shutdown();
    }

    // 验证手动核对拒绝空闲请求并限制同一任务的查询频率。
    void manualReconciliationRejectsIdleAndThrottlesPendingRunQueries()
    {
        // 提供受控查询及取消终态的单工位测试端点。
        Peer peer(1);
        peer.automatic = false;
        peer.ignoreCancellation = true;
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        // 直接验证手动状态核对规则的测试引擎。
        TestEngine engine;
        // 保存引擎发布的最新工位状态快照。
        QJsonObject view;
        connect(&engine, &TestEngine::stationUpdated, this,
                // 接收 v 工位快照以判断核对按钮是否应可用。
                [&](const QJsonObject &v) { view = v; });
        connect(&engine, &TestEngine::reserveRequested, &engine,
                // 模拟 run 身份已持久化成功，允许测试引擎继续下发动作。
                [&](const QJsonObject &run) { engine.reserved(run, true, {}); });
        // 记录终态落库请求，核对持久化前不会提前解锁。
        QSignalSpy saveRequested(&engine, &TestEngine::saveRequested);
        engine.initialize({{1, "127.0.0.1", peer.serverPort()}});
        QTRY_VERIFY(view["canStart"].toBool());
        QVERIFY(!view["canReconcile"].toBool());
        engine.retryReconciliation(0);
        engine.retryReconciliation(1);
        engine.retryReconciliation(2);
        QTest::qWait(100);
        QCOMPARE(peer.queries, 0);
        // 直接创建测试请求使用的完整默认配方。
        const auto recipe = Domain::defaultRecipe();
        engine.requestStart({{"runId", "manual-reconciliation"},
                             {"stationId", 1},
                             {"sn", "MANUAL-QUERY"},
                             {"recipeVersion", recipe["version"]},
                             {"recipe", recipe}});
        QTRY_COMPARE(view["execution"].toString(), QStringLiteral("执行中"));
        engine.retryReconciliation(1);
        QTest::qWait(100);
        QCOMPARE(peer.queries, 0);
        engine.cancel(1);
        QVERIFY(view["canReconcile"].toBool());
        engine.retryReconciliation(1);
        QTRY_COMPARE(peer.queries, 1);
        QTRY_COMPARE(view["execution"].toString(), QStringLiteral("取消处理中"));
        QVERIFY(!view["canReconcile"].toBool());
        for (int i = 0; i < 20; ++i)
            engine.retryReconciliation(1);
        QTest::qWait(200);
        QCOMPARE(peer.queries, 1);
        QTRY_VERIFY_WITH_TIMEOUT(view["canReconcile"].toBool(), 2000);
        peer.finish("cancelled", false);
        engine.retryReconciliation(1);
        QTRY_COMPARE(peer.queries, 2);
        QTRY_COMPARE(saveRequested.size(), 1);
        QVERIFY(peer.queryTimes[1] - peer.queryTimes[0] >= 1000);
        QVERIFY(!view["canStart"].toBool());
        QVERIFY(!view["canReconcile"].toBool());
        QVERIFY(!view["stored"].toBool());
        // Receiving a terminal still cannot unlock the station before storage commits.
        engine.retryReconciliation(1);
        QTest::qWait(100);
        QCOMPARE(peer.queries, 2);
        QCOMPARE(peer.acks, 0);
        QCOMPARE(peer.starts, 1);
        QCOMPARE(peer.cancels, 1);
        engine.shutdown();
    }

    // 验证其他测试的终态不会结束当前运行，取消必须等待真实终态。
    void wrongRunCannotCompleteAndCancelNeedsTerminal()
    {
        // 隔离保存本用例数据库和日志的临时目录。
        QTemporaryDir dir;
        // 提供单工位真实 TCP 交互及受控故障的测试端点。
        Peer peer(1);
        peer.automatic = false;
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        // 保存当前工位状态与操作可用性的最新快照。
        QJsonObject view;
        // 协调测试流程及后台线程的待测工作台控制器。
        WorkbenchController c;
        connect(&c, &WorkbenchController::stationUpdated, this,
                // 接收 v 工位快照以跟踪身份校验和取消归档。
                [&](const QJsonObject &v) { view = v; });
        c.initialize(dir.path(), {{1, "127.0.0.1", peer.serverPort()}});
        QTRY_VERIFY(view["canStart"].toBool());
        c.startTest(1, "CANCEL", "demo-5v-v1");
        QTRY_COMPARE(peer.starts, 1);
        // 用于伪造其他运行编号终态的原请求副本。
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
    // 验证归档确认应答丢失时工位继续占用直至持久确认。
    void lostArchiveAckDoesNotReleaseStationEarly()
    {
        // 隔离保存本用例数据库和日志的临时目录。
        QTemporaryDir dir;
        // 可丢弃首次归档确认应答的测试工位。
        Peer peer(1);
        peer.dropAcks = 1;
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        // 保存当前工位状态与操作可用性的最新快照。
        QJsonObject view;
        // 协调测试流程及后台线程的待测工作台控制器。
        WorkbenchController c;
        connect(&c, &WorkbenchController::stationUpdated, this,
                // 接收 v 工位快照以观察归档确认等待和释放。
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
    // 验证同一产品不能在两个工位同时执行，复测需创建新身份。
    void sameProductCannotExecuteAtTwoStationsSimultaneously()
    {
        // 隔离保存本用例数据库和日志的临时目录。
        QTemporaryDir dir;
        // one、two 分别模拟竞争同一产品的两个工位。
        Peer one(1), two(2);
        one.automatic = two.automatic = false;
        QVERIFY(one.listen(QHostAddress::LocalHost, 0));
        QVERIFY(two.listen(QHostAddress::LocalHost, 0));
        // 按工位编号缓存工作台最新状态快照。
        QHash<int, QJsonObject> views;
        // 协调测试流程及后台线程的待测工作台控制器。
        WorkbenchController c;
        connect(&c, &WorkbenchController::stationUpdated, this,
                // 接收 v 工位快照以比较两工位的准入结果。
                [&](const QJsonObject &v) { views[v["stationId"].toInt()] = v; });
        c.initialize(dir.path(),
                     {{1, "127.0.0.1", one.serverPort()}, {2, "127.0.0.1", two.serverPort()}});
        QTRY_VERIFY(views[1]["canStart"].toBool() && views[2]["canStart"].toBool());
        c.startTest(1, "ONE-PRODUCT", "demo-5v-v1");
        c.startTest(2, "ONE-PRODUCT", "demo-5v-v1");
        QTRY_VERIFY(one.starts + two.starts == 1);
        QTRY_VERIFY(views[1]["execution"].toString() == QStringLiteral("创建失败") ||
                    views[2]["execution"].toString() == QStringLiteral("创建失败"));
        // 成功获得同一产品测试占用的工位编号。
        const int first = one.starts ? 1 : 2;
        c.cancelTest(first);
        QTRY_VERIFY(views[first]["stored"].toBool() && views[first]["canStart"].toBool());
        // A retest is a new run, with the same SN and a new immutable identity.
        // 复测前的原运行编号，用于确认复测生成新身份。
        const auto original = views[first]["runId"].toString();
        c.startTest(first, "ONE-PRODUCT", "demo-5v-v1");
        QTRY_VERIFY(views[first]["runId"].toString() != original && one.starts + two.starts == 2);
        c.cancelTest(first);
        QTRY_VERIFY(views[first]["stored"].toBool());
        c.shutdown();
    }
    // 验证客户端重启保留占用，并在核对原运行后仍可取消。
    void clientRestartHoldsIdentityAndAllowsCancellationAfterReconciliation()
    {
        // 供前后两次客户端启动共享的临时数据目录。
        QTemporaryDir dir;
        // one、two 分别模拟重启前后持续存在的工位服务。
        Peer one(1), two(2);
        one.automatic = two.automatic = false;
        QVERIFY(one.listen(QHostAddress::LocalHost, 0));
        QVERIFY(two.listen(QHostAddress::LocalHost, 0));
        // 两次客户端初始化共同使用的工位连接配置。
        const QList<StationEndpoint> endpoints{{1, "127.0.0.1", one.serverPort()},
                                               {2, "127.0.0.1", two.serverPort()}};
        {
            // 按工位编号保存首次客户端运行的状态快照。
            QHash<int, QJsonObject> views;
            // 重启场景中首次启动的工作台控制器。
            WorkbenchController first;
            connect(&first, &WorkbenchController::stationUpdated, this,
                    // 接收 v 工位快照以记录重启前的未完成测试。
                    [&](const QJsonObject &v) { views[v["stationId"].toInt()] = v; });
            first.initialize(dir.path(), endpoints);
            QTRY_VERIFY(views[1]["canStart"].toBool());
            first.startTest(1, "RESTART-HELD-SN", "demo-5v-v1");
            QTRY_COMPARE(one.starts, 1);
            first.shutdown();
        }
        // 按工位编号保存恢复后客户端的状态快照。
        QHash<int, QJsonObject> views;
        // 使用原数据目录恢复未完成身份的工作台控制器。
        WorkbenchController recovered;
        connect(&recovered, &WorkbenchController::stationUpdated, this,
                // 接收 v 工位快照以跟踪恢复、占用校验和取消结果。
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
    // 验证中断落库失败后到达的真实终态仍能完成归档。
    void lateTerminalWinsOverFailedInterruptionRelease()
    {
        // 隔离保存本用例数据库和日志的临时目录。
        QTemporaryDir dir;
        // 可报告未知状态并补发真实终态的测试工位。
        Peer peer(1);
        peer.automatic = false;
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        // 保存当前工位状态与操作可用性的最新快照。
        QJsonObject view;
        // 协调测试流程及后台线程的待测工作台控制器。
        WorkbenchController c;
        connect(&c, &WorkbenchController::stationUpdated, this,
                // 接收 row 快照以观察中断落库失败及真实终态恢复。
                [&](const QJsonObject &row) { view = row; });
        c.initialize(dir.path(), {{1, "127.0.0.1", peer.serverPort()}});
        QTRY_VERIFY(view["canStart"].toBool());
        c.startTest(1, "LATE-TERMINAL", "demo-5v-v1");
        QTRY_COMPARE(peer.starts, 1);
        // 用于阻止中断事务写入的独立连接名称。
        const QString connection = "interruption-lock";
        {
            // 在测试线程中持有写锁的独立数据库连接。
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(dir.filePath("data/workbench.db"));
            QVERIFY(db.open());
            // 申请及释放数据库写锁的控制查询。
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
    // 验证连续批量测试每轮仅产生一条完整归档终态。
    void batchRunsHaveOneTerminalEach()
    {
        // 隔离保存批量测试数据库和日志的临时目录。
        QTemporaryDir dir;
        // one、two 分别承担交替执行的两个测试工位。
        Peer one(1), two(2);
        one.delayMs = two.delayMs = 1;
        QVERIFY(one.listen(QHostAddress::LocalHost, 0));
        QVERIFY(two.listen(QHostAddress::LocalHost, 0));
        // 按工位编号缓存最新运行身份和归档状态。
        QHash<int, QJsonObject> views;
        // 协调批量流程及后台线程的待测工作台控制器。
        WorkbenchController c;
        connect(&c, &WorkbenchController::stationUpdated, this,
                // 接收 v 工位快照以等待每轮归档及工位确认完成。
                [&](const QJsonObject &v) { views[v["stationId"].toInt()] = v; });
        c.initialize(dir.path(),
                     {{1, "127.0.0.1", one.serverPort()}, {2, "127.0.0.1", two.serverPort()}});
        QTRY_VERIFY(views[1]["canStart"].toBool() && views[2]["canStart"].toBool());
        // 标记环境中的批量轮数是否能正确读取。
        bool countOk = false;
        // 从验证环境读取的期望连续归档轮数。
        const auto configured = qEnvironmentVariableIntValue("WORKBENCH_BATCH_RUNS", &countOk);
        // 经边界限制后实际执行的连续测试轮数。
        const int runs = countOk ? qBound(1, configured, 10000) : 100;
        // 统计整批连续归档总耗时的计时器。
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < runs; ++i)
        {
            // 当前批次选用的实际工位编号。
            const int station = i % 2 + 1;
            // 启动新一轮前的原运行编号，用于等待新身份完成。
            const auto previous = views[station]["runId"].toString();
            c.startTest(station, QString("BATCH-%1").arg(i), "demo-5v-v1");
            QTRY_VERIFY(views[station]["runId"].toString() != previous &&
                        views[station]["stored"].toBool() && views[station]["canStart"].toBool());
        }
        // 记录每次分页查询得到的历史记录和翻页状态。
        QSignalSpy history(&c, &WorkbenchController::historyReady);
        // cursor 保存下一页边界，total 累计已读取的历史条数。
        int cursor = 0, total = 0;
        // 收集已归档运行编号以核对整批测试没有重复记录。
        QSet<QString> unique;
        bool more = true;
        while (more)
        {
            history.clear();
            c.refreshHistory({}, cursor);
            QTRY_VERIFY(!history.isEmpty());
            // 本次历史查询返回的完整分页记录。
            const auto rows = history.last()[0].toJsonArray();
            more = history.last()[1].toBool();
            QVERIFY(!rows.isEmpty());
            for (const auto &value : rows)
            {
                // 用于核对质量判定、步骤明细和唯一身份的历史记录。
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
