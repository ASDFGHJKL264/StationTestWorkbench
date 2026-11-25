#include "testengine.h"
#include "domain.h"
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <QThread>
#include <QJsonDocument>
#include <cmath>

TestEngine::TestEngine(QObject *parent) : QObject(parent)
{
    m_clock.start();
}

void TestEngine::configureTimeouts(qint64 runMs, qint64 stepMs)
{
    if (runMs < 100 || runMs > 86400000 || stepMs < 100 || stepMs > 86400000)
    {
        emit logMessage(QStringLiteral("测试超时配置超出允许范围"), true);
        return;
    }
    m_runTimeoutMs = runMs;
    m_stepTimeoutMs = stepMs;
}

void TestEngine::loadSafetyState(const QJsonArray &closedRuns, const QJsonArray &suspendedStations)
{
    if (m_timer)
    {
        emit logMessage(QStringLiteral("工位接入后不能覆盖安全处置状态"), true);
        return;
    }
    m_closedRunIds.clear();
    m_suspendedStations.clear();
    for (const auto &value : closedRuns)
    {
        // 保存需要隔离的已人工结算运行编号。
        const auto id = value.toObject().value("runId").toString();
        if (!id.isEmpty() && id.size() <= 64)
            m_closedRunIds.insert(id);
    }
    for (const auto &value : suspendedStations)
    {
        // 取出持久化停用工位编号。
        const int station = value.toObject().value("stationId").toInt();
        if (station >= 1 && station <= 4)
            m_suspendedStations.insert(station);
    }
}

void TestEngine::initialize(const QList<StationEndpoint> &endpoints, const QJsonArray &recovery)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_timer)
        return;
    for (const auto &ep : endpoints)
    {
        if (ep.stationId < 1 || ep.stationId > 4 || ep.host.isEmpty() || !ep.port ||
            m_links.count(ep.stationId))
            continue;
        // 创建并独占管理该工位的连接状态对象。
        auto owned = std::make_unique<Link>();
        // 临时引用连接对象，供后续 socket 回调访问。
        auto *l = owned.get();
        l->endpoint = ep;
        l->safetySuspended = m_suspendedStations.contains(ep.stationId);
        l->socket = new QTcpSocket(this);
        l->socket->setReadBufferSize(128 * 1024);
        l->view = {{"stationId", ep.stationId},
                   {"execution", QStringLiteral("待连接")},
                   {"verdict", "PENDING"},
                   {"stored", false}};
        for (const auto &item : recovery)
        {
            // 保存从数据库恢复的单次测试请求快照。
            const auto run = item.toObject();
            if (run["stationId"].toInt() != ep.stationId || !Domain::validateRun(run) ||
                m_closedRunIds.contains(run.value("runId").toString()))
                continue;
            l->run = run;
            l->dispatched = true;
            l->view = {{"stationId", ep.stationId},
                       {"runId", run["runId"]},
                       {"sn", run["sn"]},
                       {"recipeVersion", run["recipeVersion"]},
                       {"recipe", run["recipe"]},
                       {"execution", QStringLiteral("恢复后待对账")},
                       {"verdict", "UNKNOWN"},
                       {"stored", false},
                       {"detail", QStringLiteral("保留上次产品占用，查询工位后再决定终态")}};
            for (const auto &value : run["recipe"].toObject()["steps"].toArray())
            {
                // 将配方步骤转换为尚无测量值的进度行。
                auto row = value.toObject();
                row["stepId"] = row.take("id");
                l->progress.append(row);
            }
            break;
        }
        // 连接成功后为 l 工位建立新会话，并发送身份握手。
        connect(l->socket, &QTcpSocket::connected, this,
                [this, l]
                {
                    if (l->safetySuspended || l->manualPending || m_restorePaused)
                    {
                        l->socket->abort();
                        return;
                    }
                    l->session = QUuid::createUuid().toString(QUuid::WithoutBraces);
                    l->parser.reset();
                    l->lastRx = m_clock.elapsed();
                    l->lastPing = l->lastRx;
                    publish(*l, QStringLiteral("验证工位身份"));
                    send(*l, "hello");
                });
        // 收到可读通知时，继续解析 l 工位的网络报文。
        connect(l->socket, &QTcpSocket::readyRead, this, [this, l] { read(*l); });
        // 断线后保留 l 的测试身份，并安排退避重连。
        connect(l->socket, &QTcpSocket::disconnected, this,
                [this, l]
                {
                    l->online = false;
                    l->parser.reset();
                    l->queryDeadline = 0;
                    l->queryRetryAt = 0;
                    l->cancelDeadline = 0;
                    l->reconnectAt = m_clock.elapsed() + l->backoff;
                    l->backoff = qMin(8000, l->backoff * 2);
                    if (l->safetySuspended || l->manualPending || m_restorePaused)
                    {
                        publish(*l);
                        return;
                    }
                    publish(*l,
                            l->run.isEmpty() ? QStringLiteral("连接中断")
                                             : QStringLiteral("断线，结果待对账"),
                            QStringLiteral("保留测试身份；重连后查询，不自动重发测试动作"));
                });
        // 收到 socket 错误类型后，读取 l 的错误详情并安排重连。
        connect(l->socket, &QTcpSocket::errorOccurred, this,
                [this, l](QAbstractSocket::SocketError)
                {
                    if (m_stopping || l->safetySuspended || l->manualPending || m_restorePaused)
                        return;
                    if (l->socket->state() == QAbstractSocket::UnconnectedState)
                    {
                        l->reconnectAt = m_clock.elapsed() + l->backoff;
                        l->backoff = qMin(8000, l->backoff * 2);
                        publish(*l,
                                l->run.isEmpty() ? QStringLiteral("连接失败")
                                                 : QStringLiteral("断线，结果待对账"),
                                l->socket->errorString());
                    }
                });
        m_links.emplace(ep.stationId, std::move(owned));
        publish(*l, l->safetySuspended ? QStringLiteral("工位已停用") : QString(),
                l->safetySuspended ? QStringLiteral("管理员确认设备复位后，才能恢复工位接入")
                                   : QString());
    }
    m_timer = new QTimer(this);
    m_timer->setInterval(100);
    connect(m_timer, &QTimer::timeout, this, &TestEngine::tick);
    m_timer->start();
    tick();
}

void TestEngine::publish(Link &l, const QString &execution, const QString &detail)
{
    if (!execution.isEmpty())
        l.view["execution"] = execution;
    if (!detail.isNull())
        l.view["detail"] = detail;
    l.view["online"] = l.online;
    // 标记该工位是否仍有等待数据库提交的终态。
    bool pending = false;
    for (const auto &item : m_results)
        if (item.result["stationId"].toInt() == l.endpoint.stationId)
            pending = true;
    l.view["canStart"] = l.online && l.run.isEmpty() && l.remoteRun.isEmpty() &&
                         l.ackPending.isEmpty() && !pending && !l.interrupting &&
                         !l.safetySuspended && !l.manualPending && !m_restorePaused;
    l.view["canCancel"] = l.online && !l.run.isEmpty() && l.dispatched && !l.cancelling &&
                          !m_results.contains(l.run.value("runId").toString()) && !l.interrupting &&
                          !l.safetySuspended && !l.manualPending && !m_restorePaused;
    l.view["canReconcile"] = canReconcile(l);
    l.view["canManualDisposition"] = canManuallyDispose(l);
    l.view["safetySuspended"] = l.safetySuspended;
    l.view["manualPending"] = l.manualPending;
    l.view["timedOut"] = l.timedOut;
    l.view["timeoutReason"] = l.timeoutReason;
    l.view["busy"] = !l.run.isEmpty() || !l.remoteRun.isEmpty() || !l.ackPending.isEmpty() ||
                     pending || l.interrupting || l.manualPending;
    l.view["stepResults"] = l.progress;
    emit stationUpdated(l.view);
}

quint32 TestEngine::send(Link &l, const QString &kind, QJsonObject message)
{
    if (m_stopping || m_restorePaused || l.safetySuspended || l.manualPending ||
        l.socket->state() != QAbstractSocket::ConnectedState)
        return 0;
    if (++l.sequence == 0)
        ++l.sequence;
    message["kind"] = kind;
    message["session"] = l.session;
    message["sequence"] = double(l.sequence);
    // 保存编码后的完整传输帧，并检查发送缓冲上限。
    const auto bytes = Wire::encode(message);
    if (bytes.isEmpty() || l.socket->bytesToWrite() + bytes.size() > 128 * 1024 ||
        l.socket->write(bytes) != bytes.size())
    {
        l.socket->abort();
        return 0;
    }
    return l.sequence;
}

void TestEngine::read(Link &l)
{
    // 收集本轮读取并解析出的完整业务报文。
    const auto packets = l.parser.append(l.socket->read(64 * 1024));
    // 取走解析器最近一次格式或校验失败原因。
    const auto error = l.parser.takeError();
    if (!error.isEmpty())
        emit logMessage(QStringLiteral("工位%1 协议拒绝：%2").arg(l.endpoint.stationId).arg(error),
                        true);
    for (const auto &message : packets)
        receive(l, message);
    if (l.socket->bytesAvailable() > 0)
        // 安排下一轮读取 l，避免一次读完大量数据长时间占用事件循环。
        QTimer::singleShot(0, this, [this, &l] { read(l); });
}

void TestEngine::receive(Link &l, const QJsonObject &m)
{
    if (l.safetySuspended || l.manualPending || m_restorePaused)
        return;
    if (m["session"].toString() != l.session)
        return;
    // 取出收到的报文序号，校验整数范围和有效性。
    const auto sequence = m["sequence"].toDouble();
    if (!m["sequence"].isDouble() || !std::isfinite(sequence) || sequence < 1 ||
        sequence != std::floor(sequence) || sequence > 4294967295.0)
        return;
    // 取出报文类型，决定握手、进度、终态或核对的处理分支。
    const auto kind = m["kind"].toString();
    if (kind == "helloAck")
    {
        if (m["stationId"].toInt() != l.endpoint.stationId)
        {
            emit logMessage(QStringLiteral("端口返回的工位身份与配置不一致，拒绝连接"), true);
            l.socket->abort();
            return;
        }
        if (m["busy"].toBool() && m_closedRunIds.contains(m["runId"].toString()))
        {
            suspendForClosedRun(l, m["runId"].toString());
            return;
        }
        l.online = true;
        l.backoff = 1000;
        l.queryBackoff = 1000;
        l.lastRx = m_clock.elapsed();
        // Preview has no execution side effects; restore it after each verified connection.
        if (!l.recipePreview.isEmpty())
            send(l, "recipePreview", {{"recipe", l.recipePreview}});
        l.remoteRun = m["busy"].toBool() ? m["runId"].toString() : QString();
        if (!l.remoteRun.isEmpty())
            l.remoteUnknown = false;
        if (l.remoteRun.isEmpty())
            l.ackPending.clear();
        if (!l.run.isEmpty() && l.dispatched)
        {
            armExecutionTimeouts(l);
            query(l, l.run.value("runId").toString());
        }
        else if (!l.remoteRun.isEmpty())
            query(l, l.remoteRun);
        else
            publish(l, QStringLiteral("就绪"), QStringLiteral("独立TCP工位已连接，等待产品SN"));
        return;
    }
    if (!l.online)
        return;
    l.lastRx = m_clock.elapsed();
    if (kind == "pong")
        return;
    if (kind == "resultConfirmed")
    {
        if (m["runId"].toString() != l.ackPending || l.ackPending.isEmpty())
            return;
        l.ackPending.clear();
        l.remoteRun.clear();
        publish(l, {}, QStringLiteral("归档已提交，工位已持久化确认，可进行下一次测试"));
        return;
    }
    if (kind == "accepted")
    {
        if (l.run.isEmpty() || !l.dispatched || m["runId"] != l.run.value("runId") ||
            m["sequence"].toDouble() != l.startSequence || !l.awaitingAck)
            return;
        l.awaitingAck = false;
        if (!m["accepted"].toBool())
        {
            clearUnknown(l, QStringLiteral("工位明确拒绝测试：") + m["reason"].toString());
            return;
        }
        publish(l, l.cancelling ? QStringLiteral("取消处理中") : QStringLiteral("执行中"),
                QStringLiteral("命令已受理，等待实际步骤和终态"));
        return;
    }
    if (kind == "progress")
    {
        if (l.run.isEmpty() || !l.dispatched || m["runId"] != l.run.value("runId") ||
            m_results.contains(l.run.value("runId").toString()))
            return;
        // 保存进度报文中的步骤位置，用于对应配方规则。
        const int index = m["stepIndex"].toInt(-1);
        // 读取当前运行绑定的步骤规则，防止按其他配方解释测量值。
        const auto rules = l.run["recipe"].toObject()["steps"].toArray();
        if (!m["stepIndex"].isDouble() || m["stepIndex"].toDouble() != index || index < 0 ||
            index >= rules.size() || m["stepId"] != rules[index].toObject()["id"] ||
            !m["value"].isDouble() || !std::isfinite(m["value"].toDouble()))
            return;
        if (!l.progressedSteps.contains(index))
        {
            l.progressedSteps.insert(index);
            if (!l.timedOut && l.stepDeadline)
                l.stepDeadline = m_clock.elapsed() + l.stepLimitMs;
        }
        // 取出对应进度行，准备更新测量值及本地判定。
        auto row = l.progress[index].toObject();
        row["value"] = m["value"];
        row["passed"] = m["value"].toDouble() >= row["min"].toDouble() &&
                        m["value"].toDouble() <= row["max"].toDouble();
        l.progress[index] = row;
        publish(l, l.cancelling ? QStringLiteral("取消处理中") : QStringLiteral("执行中"));
        return;
    }
    if (kind == "terminal")
    {
        terminal(l, m["result"].toObject());
        return;
    }
    if (kind == "runState")
    {
        // 保存状态应答中的测试编号，拒绝无关运行的回报。
        const auto id = m["runId"].toString();
        if (id.isEmpty() ||
            id != (l.run.isEmpty() ? l.remoteRun : l.run.value("runId").toString()))
            return;
        if (m["state"] == "terminal")
        {
            terminal(l, m["result"].toObject());
            return;
        }
        if (m["state"] == "unknown")
        {
            l.remoteUnknown = true;
            clearUnknown(l, QStringLiteral("工位无可恢复执行记录，记为中断/未知；未自动重发"));
            return;
        }
        if (m["state"] == "executing")
        {
            if (m_results.contains(id) || !l.ackPending.isEmpty() || l.interrupting)
                return;
            resetReconciliation(l);
            l.remoteUnknown = false;
            l.awaitingAck = false;
            if (l.cancelling)
            {
                l.reconciling = true;
                l.cancelDeadline = m_clock.elapsed() + 3000;
            }
            publish(l, l.cancelling ? QStringLiteral("取消处理中") : QStringLiteral("执行中"),
                    l.cancelling ? QStringLiteral("工位仍在执行，保留占用并继续核对取消结果")
                                 : QStringLiteral("已核对原测试仍在执行，未重发测试动作"));
            return;
        }
    }
    if (kind == "error")
    {
        emit logMessage(
            QStringLiteral("工位%1：%2").arg(l.endpoint.stationId).arg(m["message"].toString()),
            true);
    }
}

void TestEngine::previewRecipe(int stationId, const QJsonObject &recipe)
{
    if (m_stopping || !Domain::validateRecipe(recipe))
        return;
    // 查找需要更新只读配方预览的工位连接。
    const auto found = m_links.find(stationId);
    if (found == m_links.end())
        return;
    // 引用目标工位连接，更新其预览缓存并按需发送。
    auto &link = *found->second;
    if (link.recipePreview == recipe)
        return;
    link.recipePreview = recipe;
    if (link.online)
        send(link, "recipePreview", {{"recipe", recipe}});
}

void TestEngine::requestStart(const QJsonObject &run)
{
    if (m_stopping || m_restorePaused)
        return;
    // 接收测试请求身份及配方校验失败的原因。
    QString error;
    if (!Domain::validateRun(run, &error))
    {
        emit logMessage(error, true);
        return;
    }
    // 按测试请求中的工位编号定位连接状态。
    const auto found = m_links.find(run["stationId"].toInt());
    if (found == m_links.end())
        return;
    // 引用目标工位的连接和运行状态。
    auto &l = *found->second;
    // 标记该工位是否存在尚未提交的结果，防止重复准入。
    bool pending = false;
    for (const auto &item : m_results)
        if (item.result["stationId"].toInt() == l.endpoint.stationId)
            pending = true;
    if (!l.online || !l.run.isEmpty() || !l.remoteRun.isEmpty() || !l.ackPending.isEmpty() ||
        l.interrupting || pending || l.safetySuspended || l.manualPending || m_restorePaused)
    {
        emit logMessage(QStringLiteral("工位未就绪或仍有未结算测试"), true);
        return;
    }
    l.run = run;
    l.dispatched = false;
    l.cancelling = false;
    l.awaitingAck = false;
    l.remoteUnknown = false;
    clearExecutionTimeouts(l);
    l.progressedSteps.clear();
    l.timedOut = false;
    l.timeoutCancelSent = false;
    l.timeoutReason.clear();
    resetReconciliation(l);
    l.lastQueryAt = -1000;
    l.progress = {};
    l.archivedResult = {};
    l.view = {{"stationId", l.endpoint.stationId},
              {"runId", run["runId"]},
              {"sn", run["sn"]},
              {"recipeVersion", run["recipeVersion"]},
              {"recipe", run["recipe"]},
              {"stored", false},
              {"verdict", "PENDING"}};
    for (const auto &v : run["recipe"].toObject()["steps"].toArray())
    {
        // 将配方步骤转换为新测试的初始进度行。
        auto row = v.toObject();
        row["stepId"] = row.take("id");
        l.progress.append(row);
    }
    publish(l, QStringLiteral("准备测试"), QStringLiteral("先持久化产品与配方身份，再下发动作"));
    emit reserveRequested(run);
}

void TestEngine::reserved(const QJsonObject &run, bool ok, const QString &error)
{
    // 查找预约回报所属的工位连接。
    const auto found = m_links.find(run["stationId"].toInt());
    if (found == m_links.end())
        return;
    // 引用预约回报对应的连接，核对 runId 后决定是否下发。
    auto &l = *found->second;
    if (l.run.value("runId") != run["runId"])
        return;
    if (!ok)
    {
        l.run = {};
        publish(l, QStringLiteral("创建失败"), error);
        emit logMessage(error, true);
        return;
    }
    if (!l.online)
    {
        clearUnknown(l, QStringLiteral("持久化后工位已断开，未发送测试动作"));
        return;
    }
    l.dispatched = true;
    l.awaitingAck = true;
    l.startDeadline = m_clock.elapsed() + 3000;
    l.startSequence = send(l, "start", {{"run", run}});
    if (l.startSequence)
        armExecutionTimeouts(l);
    publish(l, QStringLiteral("等待受理"), QStringLiteral("已记录测试身份，等待工位ACK"));
}

QString TestEngine::reconciliationRunId(const Link &l) const
{
    // Storage and archive confirmation own the task once a terminal has arrived.
    if (m_stopping || m_restorePaused || l.safetySuspended || l.manualPending || !l.online ||
        l.interrupting || !l.ackPending.isEmpty() ||
        (!l.run.isEmpty() && !l.dispatched))
        return {};
    // 优先取本地运行编号，无本地运行时使用远端占用编号。
    const auto id = l.run.isEmpty() ? l.remoteRun : l.run.value("runId").toString();
    return m_results.contains(id) ? QString() : id;
}

bool TestEngine::canReconcile(const Link &l) const
{
    return (l.reconciling || l.cancelling) && !l.queryDeadline &&
           m_clock.elapsed() - l.lastQueryAt >= 1000 && !reconciliationRunId(l).isEmpty();
}

void TestEngine::resetReconciliation(Link &l)
{
    l.reconciling = false;
    l.queryDeadline = 0;
    l.queryRetryAt = 0;
    l.cancelDeadline = 0;
    l.queryBackoff = 1000;
}

void TestEngine::query(Link &l, const QString &id)
{
    if (id.isEmpty() || id != reconciliationRunId(l) || l.queryDeadline)
        return;
    // 取得单调计时时刻，计算查询限频和应答期限。
    const auto now = m_clock.elapsed();
    l.reconciling = true;
    if (now - l.lastQueryAt < 1000)
    {
        l.queryRetryAt = l.lastQueryAt + 1000;
        return;
    }
    l.queryRetryAt = 0;
    l.cancelDeadline = 0;
    l.lastQueryAt = now;
    l.queryDeadline = now + 3000;
    if (!send(l, "query", {{"runId", id}}))
        return;
    publish(l, QStringLiteral("状态核对中"), QStringLiteral("按测试编号查询，不自动重启测试"));
}

void TestEngine::retryReconciliation(int stationId)
{
    // 定位用户请求重新核对的工位连接。
    const auto found = m_links.find(stationId);
    if (found == m_links.end() || !canReconcile(*found->second))
        return;
    // 引用可核对工位，提交一次只读状态查询。
    auto &l = *found->second;
    query(l, reconciliationRunId(l));
}

bool TestEngine::canManuallyDispose(const Link &l) const
{
    if (m_stopping || m_restorePaused || l.online || l.manualPending || l.run.isEmpty() ||
        !l.dispatched ||
        l.run.value("runId").toString().isEmpty() || !l.ackPending.isEmpty() || l.interrupting ||
        l.interruptionWriting || l.socket->state() == QAbstractSocket::ConnectedState)
        return false;
    for (const auto &pending : m_results)
        if (pending.result.value("stationId").toInt() == l.endpoint.stationId)
            return false;
    return true;
}

void TestEngine::prepareManualDisposition(int stationId, const QString &runId)
{
    // 定位需要冻结并人工核对的离线测试。
    const auto found = m_links.find(stationId);
    if (found == m_links.end() || !canManuallyDispose(*found->second) || runId.isEmpty() ||
        found->second->run.value("runId").toString() != runId)
    {
        emit manualDispositionPrepared(stationId, runId, false,
                                       QStringLiteral("仅能处置离线且没有终态或归档事务在途的原测试"));
        return;
    }
    // 冻结后断开尚未建立的连接，避免处置事务与重连回报竞争。
    auto &l = *found->second;
    l.manualPending = true;
    l.socket->abort();
    l.online = false;
    publish(l, QStringLiteral("人工处置提交中"), QStringLiteral("保留测试占用，等待处置事务提交"));
    emit manualDispositionPrepared(stationId, runId, true, {});
}

void TestEngine::manualDispositionRecorded(int stationId, const QString &runId, bool ok,
                                          const QString &error)
{
    // 只接受对应冻结运行的处置事务回报。
    const auto found = m_links.find(stationId);
    if (found == m_links.end() || !found->second->manualPending ||
        found->second->run.value("runId").toString() != runId)
        return;
    // 保持失败时的原运行身份，只有成功后才解除产品占用。
    auto &l = *found->second;
    l.manualPending = false;
    if (!ok)
    {
        l.reconnectAt = m_clock.elapsed() + l.backoff;
        publish(l, QStringLiteral("人工处置失败，保持占用"), error);
        return;
    }
    m_closedRunIds.insert(runId);
    m_suspendedStations.insert(stationId);
    l.safetySuspended = true;
    clearExecutionTimeouts(l);
    resetReconciliation(l);
    l.run = {};
    l.remoteRun.clear();
    l.ackPending.clear();
    l.archivedResult = {};
    l.dispatched = false;
    l.awaitingAck = false;
    l.cancelling = false;
    l.view["verdict"] = "UNKNOWN";
    l.view["stored"] = false;
    l.view["manualResolved"] = true;
    publish(l, QStringLiteral("人工结算，工位停用"),
            QStringLiteral("人工结算不代表测试通过；确认设备停止并复位后，由管理员恢复工位"));
}

void TestEngine::resumeStation(int stationId)
{
    // 定位已由管理员解除持久化停用的工位。
    const auto found = m_links.find(stationId);
    if (found == m_links.end())
        return;
    // 尚有实际测试或归档工作时，不能通过恢复入口绕过占用。
    auto &l = *found->second;
    if (m_stopping || m_restorePaused || !l.safetySuspended || l.manualPending ||
        !l.run.isEmpty() || !l.remoteRun.isEmpty() || !l.ackPending.isEmpty() || l.interrupting)
        return;
    for (const auto &pending : m_results)
        if (pending.result.value("stationId").toInt() == stationId)
            return;
    l.safetySuspended = false;
    m_suspendedStations.remove(stationId);
    l.reconnectAt = m_clock.elapsed();
    l.view["manualResolved"] = false;
    l.view["runId"] = QString();
    l.view["sn"] = QString();
    publish(l, QStringLiteral("等待重新核对设备"),
            QStringLiteral("恢复接入后重新验证身份，旧人工结算结果仍会被隔离"));
}

void TestEngine::prepareRestore()
{
    if (m_stopping || m_restorePaused || !m_results.isEmpty())
    {
        emit restorePrepared(false, QStringLiteral("仍有测试结果待归档或恢复切换已在进行"));
        return;
    }
    for (const auto &[station, owned] : m_links)
    {
        Q_UNUSED(station);
        // 检查工位实际运行及数据库事务，避免只依赖界面快照判断闲置。
        const auto &l = *owned;
        if (!l.run.isEmpty() || !l.remoteRun.isEmpty() || !l.ackPending.isEmpty() ||
            l.interrupting || l.interruptionWriting || l.manualPending)
        {
            emit restorePrepared(false, QStringLiteral("所有工位完成测试、归档和核对后才能切换数据"));
            return;
        }
    }
    m_restorePaused = true;
    for (auto &[station, owned] : m_links)
    {
        Q_UNUSED(station);
        // 全部检查通过后一次性冻结接入，再断开已确认闲置的连接。
        auto &l = *owned;
        l.socket->abort();
        l.online = false;
        publish(l, QStringLiteral("恢复数据切换中"), QStringLiteral("已冻结新测试和工位连接"));
    }
    emit restorePrepared(true, {});
}

void TestEngine::finishRestorePause()
{
    if (!m_restorePaused || m_stopping)
        return;
    m_restorePaused = false;
    for (auto &[station, owned] : m_links)
    {
        Q_UNUSED(station);
        // 恢复取消后重新安排连接；持久化停用状态继续生效。
        auto &l = *owned;
        l.reconnectAt = m_clock.elapsed();
        publish(l, l.safetySuspended ? QStringLiteral("工位已停用") : QStringLiteral("待连接"),
                QStringLiteral("已结束恢复切换暂停"));
    }
}

void TestEngine::armExecutionTimeouts(Link &l)
{
    if (l.run.isEmpty() || !l.dispatched || l.timedOut || l.runDeadline || l.stepDeadline)
        return;
    // 捕获本轮配置，后续设置或重复握手均不能延后本轮期限。
    const auto now = m_clock.elapsed();
    l.runLimitMs = m_runTimeoutMs;
    l.stepLimitMs = m_stepTimeoutMs;
    l.runDeadline = now + l.runLimitMs;
    l.stepDeadline = now + l.stepLimitMs;
}

void TestEngine::clearExecutionTimeouts(Link &l)
{
    l.runDeadline = 0;
    l.stepDeadline = 0;
    l.runLimitMs = 0;
    l.stepLimitMs = 0;
}

void TestEngine::checkExecutionTimeout(Link &l, qint64 now)
{
    if (l.run.isEmpty() || !l.dispatched || l.interrupting ||
        m_results.contains(l.run.value("runId").toString()))
        return;
    if (!l.timedOut && ((l.runDeadline && now >= l.runDeadline) ||
                       (l.stepDeadline && now >= l.stepDeadline)))
    {
        l.timedOut = true;
        l.timeoutReason = l.runDeadline && now >= l.runDeadline
                              ? QStringLiteral("整轮测试超过 %1 毫秒，结果待设备核对").arg(l.runLimitMs)
                              : QStringLiteral("步骤或终态等待超过 %1 毫秒，结果待设备核对")
                                    .arg(l.stepLimitMs);
        // 用户已经发过取消时，超时处理也不重复执行取消动作。
        l.timeoutCancelSent = l.cancelling;
        l.cancelling = true;
        l.reconciling = true;
        emit runTimedOut(l.run.value("runId").toString(), l.endpoint.stationId, l.timeoutReason);
        emit logMessage(QStringLiteral("工位%1：%2").arg(l.endpoint.stationId).arg(l.timeoutReason),
                        true);
        publish(l, QStringLiteral("测试超时，结果待核对"), l.timeoutReason);
    }
    if (l.timedOut && l.online && !l.timeoutCancelSent)
    {
        l.timeoutCancelSent = true;
        l.cancelDeadline = now + 3000;
        send(l, "cancel", {{"runId", l.run.value("runId")}});
        query(l, reconciliationRunId(l));
    }
}

void TestEngine::suspendForClosedRun(Link &l, const QString &runId)
{
    // 旧设备的迟到回报不能覆盖管理员已经结算的运行。
    const auto reason = QStringLiteral("收到已人工结算测试 %1 的回报，工位已隔离；请确认设备停止并复位")
                            .arg(runId);
    l.safetySuspended = true;
    m_suspendedStations.insert(l.endpoint.stationId);
    l.socket->abort();
    l.online = false;
    publish(l, QStringLiteral("旧测试回报，工位停用"), reason);
    emit stationSuspensionRequired(l.endpoint.stationId, runId, reason);
}

void TestEngine::clearUnknown(Link &l, const QString &reason)
{
    clearExecutionTimeouts(l);
    resetReconciliation(l);
    if (m_results.contains(l.run.value("runId").toString()) || m_results.contains(l.remoteRun))
    {
        publish(l, QStringLiteral("等待归档"),
                QStringLiteral("工位已无记录，但已收到的终态必须先提交数据库"));
        return;
    }
    if (!l.ackPending.isEmpty() && l.view["stored"].toBool())
    {
        l.ackPending.clear();
        l.remoteRun.clear();
        publish(l, {}, QStringLiteral("工位已无待确认结果；本地已归档记录保留"));
        return;
    }
    if (!l.run.isEmpty())
    {
        if (!l.interrupting)
        {
            l.interrupting = true;
            l.interruptionWriting = true;
            l.interruptionReason = reason;
            l.awaitingAck = false;
            l.queryDeadline = 0;
            emit interrupted(l.run.value("runId").toString(), reason);
        }
        publish(l, QStringLiteral("中断确认待归档"), reason);
        return;
    }
    l.view["verdict"] = "UNKNOWN";
    l.view["stored"] = false;
    l.run = {};
    l.remoteRun.clear();
    l.dispatched = false;
    l.awaitingAck = false;
    l.cancelling = false;
    l.ackPending.clear();
    publish(l, QStringLiteral("中断 / 未完成"), reason);
    emit logMessage(reason, true);
}

void TestEngine::interruptionRecorded(const QString &id, bool ok, const QString &error)
{
    for (auto &[station, owned] : m_links)
    {
        Q_UNUSED(station);
        // 引用当前连接，查找需要处理该中断回报的运行。
        auto &l = *owned;
        if (!l.interrupting || l.run.value("runId").toString() != id)
            continue;
        l.interruptionWriting = false;
        if (m_results.contains(id))
        {
            l.interrupting = false;
            return;
        }
        if (!ok)
        {
            l.interruptionRetryAt = m_clock.elapsed() + 1000;
            publish(l, QStringLiteral("中断归档失败，保持占用"), error);
            return;
        }
        // 在清空运行状态前保留已持久化的中断原因。
        const auto reason = l.interruptionReason;
        resetReconciliation(l);
        l.interrupting = false;
        l.run = {};
        l.remoteRun.clear();
        l.dispatched = false;
        l.awaitingAck = false;
        l.cancelling = false;
        l.ackPending.clear();
        l.view["stored"] = false;
        l.view["verdict"] = "UNKNOWN";
        publish(l, QStringLiteral("中断 / 未完成"), reason);
        emit logMessage(reason, true);
        return;
    }
}

void TestEngine::terminal(Link &l, const QJsonObject &result)
{
    // 取出终态测试编号，用于核对归属、重复结果和冲突。
    // value() is intentional: non-const operator[] inserts a missing key and
    // would turn the empty run (idle state) into a non-empty object on replay.
    const auto id = result["runId"].toString();
    if (id.isEmpty() || id.size() > 64 || result["stationId"].toInt() != l.endpoint.stationId)
        return;
    if (m_closedRunIds.contains(id))
    {
        suspendForClosedRun(l, id);
        return;
    }
    if (id != l.run.value("runId").toString() && id != l.remoteRun &&
        id != l.view["runId"].toString())
    {
        emit logMessage(QStringLiteral("拒绝不属于当前工位会话的迟到/未知测试结果"), true);
        return;
    }
    if (!l.archivedResult.isEmpty() && id == l.archivedResult.value("runId").toString())
    {
        if (Domain::canonical(l.archivedResult) != Domain::canonical(result))
        {
            emit logMessage(QStringLiteral("已归档测试收到冲突终态，拒绝覆盖并保留原结果"), true);
            return;
        }
        send(l, "resultAck", {{"runId", id}});
        return;
    }
    if (m_results.contains(id))
    {
        if (Domain::canonical(m_results[id].result) != Domain::canonical(result))
            emit logMessage(QStringLiteral("同一测试编号收到冲突终态，拒绝覆盖"), true);
        return;
    }
    if (m_results.size() >= 8)
    {
        emit logMessage(QStringLiteral("结果暂存额度已满，保留远端待确认结果并停止准入"), true);
        return;
    }
    if (!l.run.isEmpty() && l.run.value("runId") == result["runId"])
    {
        // 接收按运行配方重新计算的步骤测量和判定。
        QJsonArray evaluated;
        // verdict 保存本地总判定，error 保存终态校验失败原因。
        QString verdict, error;
        if (!Domain::evaluate(l.run, result, &evaluated, &verdict, &error))
        {
            emit logMessage(error, true);
            return;
        }
        l.remoteUnknown = false;
        l.progress = evaluated;
        l.view["verdict"] = verdict;
        l.view["stored"] = false;
        l.awaitingAck = false;
    }
    clearExecutionTimeouts(l);
    resetReconciliation(l);
    m_results.insert(id, {result, true, 0});
    publish(l, QStringLiteral("等待归档"),
            QStringLiteral("已收到测试终态，事务提交前不确认结果"));
    emit saveRequested(result);
}

void TestEngine::resultSaved(const QJsonObject &result, bool ok, bool duplicate,
                             const QString &error)
{
    // 取出数据库回报所属的测试编号。
    const auto id = result["runId"].toString();
    // 定位尚待确认的终态缓存，失败时保留并安排重试。
    auto pending = m_results.find(id);
    if (pending == m_results.end())
        return;
    // 定位归档回报所属的工位连接。
    const auto found = m_links.find(result["stationId"].toInt());
    if (found == m_links.end())
        return;
    // 引用对应连接，更新归档状态及远端确认流程。
    auto &l = *found->second;
    if (!ok)
    {
        pending->writing = false;
        pending->retryAt = m_clock.elapsed() + 1000;
        if (l.run.value("runId") == result["runId"] || l.remoteRun == id)
            publish(l, QStringLiteral("保存失败，待重试"), error);
        emit logMessage(QStringLiteral("终态未确认，等待重新提交：") + error, true);
        return;
    }
    // 保留待提交终态原文，供后续重复终态进行内容核对。
    const auto archived = pending->result;
    send(l, "resultAck", {{"runId", id}});
    m_results.erase(pending);
    if (l.run.value("runId") == result["runId"] || l.remoteRun == id)
    {
        // 接收归档后用于界面显示的已判定步骤列表。
        QJsonArray evaluated;
        // verdict 保存最终质量判定，ignored 接收已完成校验后的备用错误输出。
        QString verdict, ignored;
        if (!l.run.isEmpty())
            Domain::evaluate(l.run, result, &evaluated, &verdict, &ignored);
        else
        {
            evaluated = result["evaluatedSteps"].toArray();
            verdict = result["verdict"].toString("UNKNOWN");
        }
        l.view["runId"] = id;
        l.view["sn"] = result["sn"];
        l.view["recipeVersion"] = result["recipeVersion"];
        l.view["verdict"] = verdict;
        l.view["stored"] = true;
        l.archivedResult = archived;
        if (!evaluated.isEmpty())
            l.progress = evaluated;
        l.interrupting = false;
        l.interruptionWriting = false;
        l.interruptionRetryAt = 0;
        l.interruptionReason.clear();
        resetReconciliation(l);
        l.run = {};
        l.remoteRun = id;
        l.ackPending = id;
        l.ackDeadline = m_clock.elapsed() + 500;
        if (l.remoteUnknown)
        {
            l.remoteRun.clear();
            l.ackPending.clear();
        }
        l.awaitingAck = false;
        l.dispatched = false;
        l.cancelling = false;
        publish(l,
                result["outcome"] == "cancelled" ? QStringLiteral("已取消并归档")
                                                 : QStringLiteral("已完成并归档"),
                duplicate ? QStringLiteral("重复结果已核对，等待工位确认")
                          : QStringLiteral("事务提交成功，等待工位持久化确认"));
    }
    emit logMessage(
        QStringLiteral("测试 %1 归档%2")
            .arg(id, duplicate ? QStringLiteral("已存在，重复确认") : QStringLiteral("成功")),
        false);
}

void TestEngine::cancel(int stationId)
{
    // 查找用户请求取消的目标工位。
    const auto found = m_links.find(stationId);
    if (found == m_links.end())
        return;
    // 引用目标连接，检查取消资格并发送取消请求。
    auto &l = *found->second;
    if (!l.online || l.run.isEmpty() || !l.dispatched || l.cancelling || l.interrupting ||
        l.safetySuspended || l.manualPending || m_restorePaused ||
        m_results.contains(l.run.value("runId").toString()))
        return;
    l.cancelling = true;
    l.cancelDeadline = m_clock.elapsed() + 3000;
    send(l, "cancel", {{"runId", l.run.value("runId")}});
    publish(l, QStringLiteral("取消处理中"),
            QStringLiteral("已请求取消，等待工位真实终态；不把点击当作停止完成"));
}

void TestEngine::tick()
{
    if (m_stopping || m_restorePaused)
        return;
    // 保存本轮统一计时时刻，比较各类超时与重试期限。
    const auto now = m_clock.elapsed();
    for (auto &[id, owned] : m_links)
    {
        Q_UNUSED(id);
        // 引用当前工位连接，推进其超时和恢复流程。
        auto &l = *owned;
        if (l.safetySuspended || l.manualPending)
            continue;
        checkExecutionTimeout(l, now);
        if (l.interrupting && !l.interruptionWriting &&
            !l.run.value("runId").toString().isEmpty() && now >= l.interruptionRetryAt)
        {
            l.interruptionWriting = true;
            emit interrupted(l.run.value("runId").toString(), l.interruptionReason);
        }
        if (l.socket->state() == QAbstractSocket::UnconnectedState)
        {
            if (now >= l.reconnectAt)
            {
                l.connectStarted = now;
                l.socket->connectToHost(l.endpoint.host, l.endpoint.port);
            }
            continue;
        }
        if (l.socket->state() != QAbstractSocket::ConnectedState)
        {
            if (now - l.connectStarted >= 4000)
                l.socket->abort();
            continue;
        }
        if (now - l.lastRx > 6500)
        {
            l.socket->abort();
            continue;
        }
        if (l.online && now - l.lastPing >= 1000)
        {
            l.lastPing = now;
            send(l, "ping");
        }
        if (l.online && !l.ackPending.isEmpty() && now >= l.ackDeadline)
        {
            l.ackDeadline = now + 500;
            send(l, "resultAck", {{"runId", l.ackPending}});
        }
        if (l.online && l.awaitingAck && now >= l.startDeadline)
        {
            l.awaitingAck = false;
            query(l, l.run.value("runId").toString());
        }
        if (l.online && l.cancelling && l.cancelDeadline && now >= l.cancelDeadline)
        {
            // An unanswered cancel is not proof of stopping; only query its run.
            l.cancelDeadline = 0;
            query(l, reconciliationRunId(l));
        }
        if (l.online && l.queryDeadline && now >= l.queryDeadline)
        {
            l.queryDeadline = 0;
            l.queryRetryAt = now + l.queryBackoff;
            l.queryBackoff = qMin(8000, l.queryBackoff * 2);
            publish(l, QStringLiteral("结果未知，保持占用"),
                    QStringLiteral("查询未获响应，保留占用；将自动重试只读核对"));
        }
        if (l.online && l.queryRetryAt && now >= l.queryRetryAt)
            query(l, reconciliationRunId(l));
        if (l.view["canReconcile"].toBool() != canReconcile(l) ||
            l.view["canManualDisposition"].toBool() != canManuallyDispose(l))
            publish(l);
    }
    for (auto it = m_results.begin(); it != m_results.end(); ++it)
        if (!it->writing && now >= it->retryAt)
        {
            it->writing = true;
            emit saveRequested(it->result);
        }
}

void TestEngine::shutdown()
{
    if (m_stopping)
        return;
    m_stopping = true;
    if (m_timer)
        m_timer->stop();
    for (auto &[id, owned] : m_links)
    {
        Q_UNUSED(id);
        // 引用当前工位连接，断开回调并终止 socket。
        auto &l = *owned;
        disconnect(l.socket, nullptr, this, nullptr);
        l.socket->abort();
    }
}
