#include "testengine.h"
#include "core/domain.h"
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
        auto owned = std::make_unique<Link>();
        auto *l = owned.get();
        l->endpoint = ep;
        l->socket = new QTcpSocket(this);
        l->socket->setReadBufferSize(128 * 1024);
        l->view = {{"stationId", ep.stationId},
                   {"execution", QStringLiteral("待连接")},
                   {"verdict", "PENDING"},
                   {"stored", false}};
        for (const auto &item : recovery)
        {
            const auto run = item.toObject();
            if (run["stationId"].toInt() != ep.stationId || !Domain::validateRun(run))
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
                auto row = value.toObject();
                row["stepId"] = row.take("id");
                l->progress.append(row);
            }
            break;
        }
        connect(l->socket, &QTcpSocket::connected, this,
                [this, l]
                {
                    l->session = QUuid::createUuid().toString(QUuid::WithoutBraces);
                    l->parser.reset();
                    l->lastRx = m_clock.elapsed();
                    l->lastPing = l->lastRx;
                    publish(*l, QStringLiteral("验证工位身份"));
                    send(*l, "hello");
                });
        connect(l->socket, &QTcpSocket::readyRead, this, [this, l] { read(*l); });
        connect(l->socket, &QTcpSocket::disconnected, this,
                [this, l]
                {
                    l->online = false;
                    l->parser.reset();
                    l->queryDeadline = 0;
                    l->reconnectAt = m_clock.elapsed() + l->backoff;
                    l->backoff = qMin(8000, l->backoff * 2);
                    publish(*l,
                            l->run.isEmpty() ? QStringLiteral("连接中断")
                                             : QStringLiteral("断线，结果待对账"),
                            QStringLiteral("保留测试身份；重连后查询，不自动重发测试动作"));
                });
        connect(l->socket, &QTcpSocket::errorOccurred, this,
                [this, l](QAbstractSocket::SocketError)
                {
                    if (m_stopping)
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
        publish(*l);
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
    bool pending = false;
    for (const auto &item : m_results)
        if (item.result["stationId"].toInt() == l.endpoint.stationId)
            pending = true;
    l.view["canStart"] = l.online && l.run.isEmpty() && l.remoteRun.isEmpty() &&
                         l.ackPending.isEmpty() && !pending && !l.interrupting;
    l.view["canCancel"] = l.online && !l.run.isEmpty() && l.dispatched && !l.cancelling &&
                          !m_results.contains(l.run.value("runId").toString()) && !l.interrupting;
    l.view["stepResults"] = l.progress;
    emit stationUpdated(l.view);
}

quint32 TestEngine::send(Link &l, const QString &kind, QJsonObject message)
{
    if (m_stopping || l.socket->state() != QAbstractSocket::ConnectedState)
        return 0;
    if (++l.sequence == 0)
        ++l.sequence;
    message["kind"] = kind;
    message["session"] = l.session;
    message["sequence"] = double(l.sequence);
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
    const auto packets = l.parser.append(l.socket->read(64 * 1024));
    const auto error = l.parser.takeError();
    if (!error.isEmpty())
        emit logMessage(QStringLiteral("工位%1 协议拒绝：%2").arg(l.endpoint.stationId).arg(error),
                        true);
    for (const auto &message : packets)
        receive(l, message);
    if (l.socket->bytesAvailable() > 0)
        QTimer::singleShot(0, this, [this, &l] { read(l); });
}

void TestEngine::receive(Link &l, const QJsonObject &m)
{
    if (m["session"].toString() != l.session)
        return;
    const auto sequence = m["sequence"].toDouble();
    if (!m["sequence"].isDouble() || !std::isfinite(sequence) || sequence < 1 ||
        sequence != std::floor(sequence) || sequence > 4294967295.0)
        return;
    const auto kind = m["kind"].toString();
    if (kind == "helloAck")
    {
        if (m["stationId"].toInt() != l.endpoint.stationId)
        {
            emit logMessage(QStringLiteral("端口返回的工位身份与配置不一致，拒绝连接"), true);
            l.socket->abort();
            return;
        }
        l.online = true;
        l.backoff = 1000;
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
            query(l, l.run.value("runId").toString());
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
        if (l.run.isEmpty() || m["runId"] != l.run.value("runId") ||
            m["sequence"].toDouble() != l.startSequence || !l.awaitingAck)
            return;
        l.awaitingAck = false;
        if (!m["accepted"].toBool())
        {
            clearUnknown(l, QStringLiteral("工位明确拒绝测试：") + m["reason"].toString());
            return;
        }
        publish(l, QStringLiteral("执行中"), QStringLiteral("命令已受理，等待实际步骤和终态"));
        return;
    }
    if (kind == "progress")
    {
        if (l.run.isEmpty() || m["runId"] != l.run.value("runId") ||
            m_results.contains(l.run.value("runId").toString()))
            return;
        const int index = m["stepIndex"].toInt(-1);
        const auto rules = l.run["recipe"].toObject()["steps"].toArray();
        if (index < 0 || index >= rules.size() || m["stepId"] != rules[index].toObject()["id"] ||
            !m["value"].isDouble() || !std::isfinite(m["value"].toDouble()))
            return;
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
        const auto id = m["runId"].toString();
        if (id != (l.run.isEmpty() ? l.remoteRun : l.run.value("runId").toString()))
            return;
        l.queryDeadline = 0;
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
            l.remoteUnknown = false;
            publish(l, QStringLiteral("执行中"), QStringLiteral("重连已确认原测试仍在执行"));
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
    const auto found = m_links.find(stationId);
    if (found == m_links.end())
        return;
    auto &link = *found->second;
    if (link.recipePreview == recipe)
        return;
    link.recipePreview = recipe;
    if (link.online)
        send(link, "recipePreview", {{"recipe", recipe}});
}

void TestEngine::requestStart(const QJsonObject &run)
{
    QString error;
    if (!Domain::validateRun(run, &error))
    {
        emit logMessage(error, true);
        return;
    }
    const auto found = m_links.find(run["stationId"].toInt());
    if (found == m_links.end())
        return;
    auto &l = *found->second;
    bool pending = false;
    for (const auto &item : m_results)
        if (item.result["stationId"].toInt() == l.endpoint.stationId)
            pending = true;
    if (!l.online || !l.run.isEmpty() || !l.remoteRun.isEmpty() || !l.ackPending.isEmpty() ||
        l.interrupting || pending)
    {
        emit logMessage(QStringLiteral("工位未就绪或仍有未结算测试"), true);
        return;
    }
    l.run = run;
    l.dispatched = false;
    l.cancelling = false;
    l.awaitingAck = false;
    l.remoteUnknown = false;
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
        auto row = v.toObject();
        row["stepId"] = row.take("id");
        l.progress.append(row);
    }
    publish(l, QStringLiteral("准备测试"), QStringLiteral("先持久化产品与配方身份，再下发动作"));
    emit reserveRequested(run);
}

void TestEngine::reserved(const QJsonObject &run, bool ok, const QString &error)
{
    const auto found = m_links.find(run["stationId"].toInt());
    if (found == m_links.end())
        return;
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
    publish(l, QStringLiteral("等待受理"), QStringLiteral("已记录测试身份，等待工位ACK"));
}

void TestEngine::query(Link &l, const QString &id)
{
    send(l, "query", {{"runId", id}});
    l.queryDeadline = m_clock.elapsed() + 3000;
    publish(l, QStringLiteral("重连对账中"), QStringLiteral("按测试编号查询，不自动重启测试"));
}

void TestEngine::clearUnknown(Link &l, const QString &reason)
{
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
        const auto reason = l.interruptionReason;
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
    // value() is intentional: non-const operator[] inserts a missing key and
    // would turn the empty run (idle state) into a non-empty object on replay.
    const auto id = result["runId"].toString();
    if (id.isEmpty() || id.size() > 64 || result["stationId"].toInt() != l.endpoint.stationId)
        return;
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
        QJsonArray evaluated;
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
        publish(l, QStringLiteral("等待归档"),
                QStringLiteral("已收到测试终态，事务提交前不确认结果"));
    }
    m_results.insert(id, {result, true, 0});
    emit saveRequested(result);
}

void TestEngine::resultSaved(const QJsonObject &result, bool ok, bool duplicate,
                             const QString &error)
{
    const auto id = result["runId"].toString();
    auto pending = m_results.find(id);
    if (pending == m_results.end())
        return;
    const auto found = m_links.find(result["stationId"].toInt());
    if (found == m_links.end())
        return;
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
    const auto archived = pending->result;
    send(l, "resultAck", {{"runId", id}});
    m_results.erase(pending);
    if (l.run.value("runId") == result["runId"] || l.remoteRun == id)
    {
        QJsonArray evaluated;
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
    const auto found = m_links.find(stationId);
    if (found == m_links.end())
        return;
    auto &l = *found->second;
    if (!l.online || l.run.isEmpty() || !l.dispatched || l.cancelling || l.interrupting ||
        m_results.contains(l.run.value("runId").toString()))
        return;
    l.cancelling = true;
    send(l, "cancel", {{"runId", l.run.value("runId")}});
    publish(l, QStringLiteral("取消处理中"),
            QStringLiteral("已请求取消，等待工位真实终态；不把点击当作停止完成"));
}

void TestEngine::tick()
{
    if (m_stopping)
        return;
    const auto now = m_clock.elapsed();
    for (auto &[id, owned] : m_links)
    {
        Q_UNUSED(id);
        auto &l = *owned;
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
        if (l.online && l.queryDeadline && now >= l.queryDeadline)
        {
            l.queryDeadline = 0;
            publish(l, QStringLiteral("结果未知，保持占用"),
                    QStringLiteral("查询未获响应，禁止新测试和盲目重发"));
        }
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
        auto &l = *owned;
        disconnect(l.socket, nullptr, this, nullptr);
        l.socket->abort();
    }
}
