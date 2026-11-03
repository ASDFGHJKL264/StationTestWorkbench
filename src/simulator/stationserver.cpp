#include "stationserver.h"
#include "core/domain.h"
#include <QTcpSocket>
#include <QSaveFile>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonParseError>
#include <cmath>

StationServer::StationServer(const StationOptions &options, QObject *parent)
    : QObject(parent), m_options(options)
{
    m_stepTimer.setSingleShot(true);
    m_resendTimer.setInterval(500);
    connect(&m_server, &QTcpServer::newConnection, this, &StationServer::acceptClient);
    connect(&m_stepTimer, &QTimer::timeout, this, &StationServer::advanceStep);
    connect(&m_resendTimer, &QTimer::timeout, this, &StationServer::resendTerminal);
}
StationServer::~StationServer()
{
    stop();
}
bool StationServer::start(QString *error)
{
    if (m_server.isListening())
        return true;
    if (error)
        error->clear();
    if (m_options.stationId < 1 || m_options.stationId > 4 || m_options.stepMs < 1 ||
        m_options.stepMs > 60000 || m_options.dataDirectory.isEmpty())
    {
        if (error)
            *error = QStringLiteral("invalid station id, interval or data directory");
        return false;
    }
    // Acquire the TCP endpoint before reading or migrating durable state. A second
    // instance using the same port must never mark a live run interrupted.
    if (!m_server.listen(QHostAddress::LocalHost, m_options.port))
    {
        if (error)
            *error = m_server.errorString();
        report(QStringLiteral("启动失败，监听端口被占用或不可用：%1").arg(m_server.errorString()));
        emit snapshotChanged(snapshot());
        return false;
    }
    if (!QDir().mkpath(m_options.dataDirectory))
    {
        m_server.close();
        if (error)
            *error = QStringLiteral("cannot create simulator data directory");
        report(QStringLiteral("启动失败，无法创建模拟器状态目录。"));
        emit snapshotChanged(snapshot());
        return false;
    }
    m_file = QDir(m_options.dataDirectory)
                 .filePath(QStringLiteral("station-%1.json").arg(m_options.stationId));
    QString stateError;
    if (!load(&stateError))
    {
        m_server.close();
        if (error)
            *error = stateError;
        report(QStringLiteral("启动失败，无法加载状态文件：%1").arg(stateError));
        emit snapshotChanged(snapshot());
        return false;
    }
    // A simulator process restart cannot prove that physical actions completed.
    // Preserve identity and partial steps, but never resume side effects automatically.
    if (m_state["state"] == "executing")
    {
        auto next = m_state;
        next["state"] = "interrupted";
        if (!persist(next, &stateError))
        {
            m_server.close();
            if (error)
                *error = stateError;
            emit snapshotChanged(snapshot());
            return false;
        }
        report(
            QStringLiteral("上次测试在执行中中断；保留测试身份，等待控制端对账，不自动重新执行。"));
    }
    m_pendingOutcome.clear();
    m_ignoredAck = false;
    m_resendTimer.start();
    report(QStringLiteral("工位 %1 已启动，监听 127.0.0.1:%2，等待控制端连接。")
               .arg(m_options.stationId)
               .arg(port()));
    emit snapshotChanged(snapshot());
    return true;
}
quint16 StationServer::port() const
{
    return m_server.serverPort();
}
void StationServer::stop()
{
    const bool wasActive = m_server.isListening() || m_client;
    m_stepTimer.stop();
    m_resendTimer.stop();
    m_server.close();
    if (m_client)
    {
        auto *client = m_client;
        m_client = nullptr;
        disconnect(client, nullptr, this, nullptr);
        client->abort();
        client->deleteLater();
    }
    m_session.clear();
    m_parser.reset();
    if (wasActive)
        report(QStringLiteral("工位已停止监听，连接已关闭；已持久化的测试记录继续保留。"));
    emit snapshotChanged(snapshot());
}
QJsonObject StationServer::snapshot() const
{
    const auto run = m_state["run"].toObject();
    const bool connected = m_client && m_client->state() == QAbstractSocket::ConnectedState;
    return {{"stationId", m_options.stationId},
            {"port", m_server.isListening() ? int(port()) : int(m_options.port)},
            {"listening", m_server.isListening()},
            {"connected", connected},
            {"handshaken", connected && !m_session.isEmpty()},
            {"state", m_state["state"].toString(QStringLiteral("idle"))},
            {"reconciled", m_state["reconciled"].toBool()},
            {"sn", run["sn"].toString()},
            {"runId", run["runId"].toString()},
            {"recipeVersion", run["recipeVersion"].toString()},
            {"steps", m_state["steps"].toArray()},
            {"recipe", run["recipe"].toObject()},
            {"recipePreview", m_recipePreview},
            {"result", m_state["result"].toObject()},
            {"failStep", m_options.failStep},
            {"stepMs", m_options.stepMs},
            {"dropFirstResultAck", m_options.dropFirstResultAck},
            {"faultsEditable", faultsEditable()},
            {"dataDirectory", m_options.dataDirectory}};
}
bool StationServer::faultsEditable() const
{
    const auto state = m_state["state"].toString();
    return state.isEmpty() || state == "idle" || state == "acked" ||
           (state == "interrupted" && m_state["reconciled"].toBool());
}
bool StationServer::setFaultOptions(const QString &failStep, int stepMs, bool dropFirstResultAck,
                                    QString *error)
{
    if (error)
        error->clear();
    bool validStep = failStep.size() <= 40 && failStep.trimmed() == failStep;
    for (const auto ch : failStep)
        validStep = validStep && ch.isPrint();
    if (!validStep || stepMs < 1 || stepMs > 60000)
    {
        if (error)
            *error =
                QStringLiteral("步骤标识必须为至多40个可打印字符；单步耗时范围为1～60000毫秒。");
        return false;
    }
    if (!faultsEditable())
    {
        if (error)
            *error =
                QStringLiteral("当前测试尚未结算，请等待结果确认或完成中断对账后再修改故障配置。");
        return false;
    }
    m_options.failStep = failStep;
    m_options.stepMs = stepMs;
    m_options.dropFirstResultAck = dropFirstResultAck;
    m_ignoredAck = false;
    report(
        QStringLiteral("故障配置已应用：超限步骤=%1，单步耗时=%2毫秒，每次测试丢首个归档确认=%3。")
            .arg(failStep.isEmpty() ? QStringLiteral("无") : failStep)
            .arg(stepMs)
            .arg(dropFirstResultAck ? QStringLiteral("开启") : QStringLiteral("关闭")));
    emit snapshotChanged(snapshot());
    return true;
}
void StationServer::disconnectClient()
{
    if (!m_client)
    {
        report(QStringLiteral("当前没有控制端连接，无需断开。"));
        return;
    }
    report(
        QStringLiteral("故障模拟：主动断开控制端连接；执行中的测试仍继续，等待控制端重连对账。"));
    m_client->abort();
}
void StationServer::report(const QString &message)
{
    // GUI log consumers keep a bounded row count; each individual event is bounded too.
    emit diagnostic(message.left(512).replace('\r', ' ').replace('\n', ' '));
}
bool StationServer::load(QString *error)
{
    QFile file(m_file);
    if (!file.exists())
    {
        return persist({{"schema", 1}, {"stationId", m_options.stationId}, {"state", "idle"}},
                       error);
    }
    if (!file.open(QIODevice::ReadOnly) || file.size() > 128 * 1024)
    {
        if (error)
            *error = QStringLiteral("cannot read bounded simulator state file");
        return false;
    }
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    const auto object = doc.object();
    const QString state = object["state"].toString();
    if (parseError.error != QJsonParseError::NoError || !doc.isObject() || object["schema"] != 1 ||
        object["stationId"] != m_options.stationId ||
        (state != "idle" && state != "executing" && state != "terminal" && state != "acked" &&
         state != "interrupted"))
    {
        if (error)
            *error = QStringLiteral("invalid simulator state file; refusing silent reset");
        return false;
    }
    if (state != "idle")
    {
        QString validation;
        if (!Domain::validateRun(object["run"].toObject(), &validation) ||
            object["run"].toObject()["stationId"] != m_options.stationId)
        {
            if (error)
                *error = QStringLiteral("invalid persisted run: ") + validation;
            return false;
        }
        const auto run = object["run"].toObject();
        const QJsonObject partial{{"runId", run["runId"]},  {"stationId", run["stationId"]},
                                  {"sn", run["sn"]},        {"recipeVersion", run["recipeVersion"]},
                                  {"outcome", "cancelled"}, {"steps", object["steps"]}};
        QJsonArray checked;
        QString partialVerdict;
        if (!object["steps"].isArray() ||
            !Domain::evaluate(run, partial, &checked, &partialVerdict, &validation))
        {
            if (error)
                *error = QStringLiteral("invalid persisted step prefix: ") + validation;
            return false;
        }
        if (state == "terminal" || state == "acked")
        {
            QJsonArray evaluated;
            QString verdict;
            if (!Domain::evaluate(object["run"].toObject(), object["result"].toObject(), &evaluated,
                                  &verdict, &validation))
            {
                if (error)
                    *error = QStringLiteral("invalid persisted terminal: ") + validation;
                return false;
            }
        }
    }
    m_state = object;
    emit snapshotChanged(snapshot());
    return true;
}
bool StationServer::persist(const QJsonObject &state, QString *error)
{
    QSaveFile file(m_file);
    const auto bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    {
        const auto message = QStringLiteral("simulator durable save failed: ") + file.errorString();
        if (error)
            *error = message;
        report(message);
        return false;
    }
    m_state = state;
    emit snapshotChanged(snapshot());
    return true;
}
void StationServer::acceptClient()
{
    while (m_server.hasPendingConnections())
    {
        auto *socket = m_server.nextPendingConnection();
        if (m_client)
        {
            report(QStringLiteral("拒绝第二个控制端连接：当前工位已由一个控制端占用。"));
            // There is exactly one control owner; a second connection cannot preempt it.
            socket->disconnectFromHost();
            socket->deleteLater();
            continue;
        }
        m_client = socket;
        socket->setParent(this);
        socket->setReadBufferSize(128 * 1024);
        m_session.clear();
        m_parser.reset();
        connect(socket, &QTcpSocket::readyRead, this, &StationServer::readClient);
        connect(socket, &QTcpSocket::disconnected, this,
                [this, socket]
                {
                    if (m_client == socket)
                    {
                        m_client = nullptr;
                        m_session.clear();
                        m_parser.reset();
                        report(
                            QStringLiteral("控制端连接已断开，等待重连；测试身份与结果继续保留。"));
                        emit snapshotChanged(snapshot());
                    }
                    socket->deleteLater();
                });
        report(QStringLiteral("控制端 TCP 已连接，等待协议握手。"));
        emit snapshotChanged(snapshot());
        // A connected but never handshaking peer must not own the station forever.
        QTimer::singleShot(5000, socket,
                           [this, socket]
                           {
                               if (m_client == socket && m_session.isEmpty())
                               {
                                   report(QStringLiteral("控制端握手超时，关闭连接。"));
                                   socket->abort();
                               }
                           });
    }
}
void StationServer::readClient()
{
    if (!m_client)
        return;
    const auto objects = m_parser.append(m_client->read(64 * 1024));
    const auto error = m_parser.takeError();
    if (!error.isEmpty())
        report(QStringLiteral("拒绝非法协议数据：") + error);
    for (const auto &object : objects)
        receive(object);
    if (m_client && m_client->bytesAvailable())
        QTimer::singleShot(0, this, &StationServer::readClient);
}
void StationServer::send(const QString &kind, QJsonObject object, double sequence)
{
    if (!m_client || m_client->state() != QAbstractSocket::ConnectedState || m_session.isEmpty())
        return;
    object["kind"] = kind;
    object["session"] = m_session;
    object["sequence"] = sequence > 0 ? sequence : double(++m_sequence);
    const auto bytes = Wire::encode(object);
    if (bytes.isEmpty() || m_client->bytesToWrite() + bytes.size() > 128 * 1024 ||
        m_client->write(bytes) != bytes.size())
        m_client->abort();
}
void StationServer::receive(const QJsonObject &message)
{
    const auto kind = message["kind"].toString();
    const auto session = message["session"].toString();
    const auto sequence = message["sequence"].toDouble();
    if (session.isEmpty() || session.size() > 64 || !std::isfinite(sequence) || sequence < 1 ||
        sequence != std::floor(sequence) || sequence > 4294967295.0)
        return;
    const auto run = m_state["run"].toObject();
    const auto state = m_state["state"].toString();
    if (kind == "hello")
    {
        if (!m_session.isEmpty() && m_session != session)
            return;
        const bool firstHandshake = m_session.isEmpty();
        m_session = session;
        if (firstHandshake)
        {
            report(QStringLiteral("协议握手成功，工位可接收测试或对账请求。"));
            emit snapshotChanged(snapshot());
        }
        const bool busy = state == "executing" || state == "terminal" ||
                          (state == "interrupted" && !m_state["reconciled"].toBool());
        send("helloAck",
             {{"stationId", m_options.stationId},
              {"busy", busy},
              {"runId", busy ? run["runId"] : QJsonValue()}},
             sequence);
        return;
    }
    if (session != m_session || m_session.isEmpty())
        return;
    if (kind == "ping")
    {
        send("pong", {}, sequence);
        return;
    }
    if (kind == "recipePreview")
    {
        const auto recipe = message["recipe"].toObject();
        QString error;
        if (!Domain::validateRecipe(recipe, &error))
        {
            report(QStringLiteral("配方预览无效：%1").arg(error));
            return;
        }
        if (m_recipePreview != recipe)
        {
            m_recipePreview = recipe;
            report(QStringLiteral("收到配方预览：%1；异常选项在工位空闲时更新。")
                       .arg(recipe["version"].toString()));
            emit snapshotChanged(snapshot());
        }
        return;
    }
    if (kind == "start")
    {
        const auto request = message["run"].toObject();
        const auto id = request["runId"].toString();
        QString error;
        bool accepted =
            Domain::validateRun(request, &error) && request["stationId"] == m_options.stationId;
        if (accepted && id == run["runId"].toString())
        {
            accepted =
                Domain::canonical(request) == Domain::canonical(run) && state != "interrupted";
            send("accepted",
                 {{"runId", id},
                  {"accepted", accepted},
                  {"reason", accepted ? "same run already known"
                                      : "conflicting or interrupted run identity"}},
                 sequence);
            if (accepted && (state == "terminal" || state == "acked"))
                send("terminal", {{"result", m_state["result"]}});
            report(accepted ? QStringLiteral("收到重复启动请求，复用已有测试，不重复执行。")
                            : QStringLiteral("拒绝重复启动：测试身份冲突或上次执行已中断。"));
            return;
        }
        if (accepted && (state == "executing" || state == "terminal" ||
                         (state == "interrupted" && !m_state["reconciled"].toBool())))
        {
            accepted = false;
            error = QStringLiteral("station has unresolved work");
        }
        if (accepted)
        {
            const QJsonObject next{{"schema", 1},
                                   {"stationId", m_options.stationId},
                                   {"state", "executing"},
                                   {"run", request},
                                   {"steps", QJsonArray()}};
            accepted = persist(next, &error);
        }
        send("accepted", {{"runId", id}, {"accepted", accepted}, {"reason", error}}, sequence);
        if (accepted)
        {
            m_pendingOutcome.clear();
            m_ignoredAck = false;
            report(QStringLiteral("已受理测试：SN=%1，Run ID=%2，配方=%3。")
                       .arg(request["sn"].toString(), id, request["recipeVersion"].toString()));
            m_stepTimer.start(m_options.stepMs);
        }
        else
            report(QStringLiteral("拒绝启动测试：%1").arg(error));
        return;
    }
    const auto id = message["runId"].toString();
    if (kind == "query")
    {
        QJsonObject answer{{"runId", id}, {"state", "unknown"}};
        if (!id.isEmpty() && id == run["runId"].toString())
        {
            if (state == "executing")
                answer["state"] = "executing";
            if (state == "terminal" || state == "acked")
            {
                answer["state"] = "terminal";
                answer["result"] = m_state["result"];
            }
            // Keep interrupted identity as evidence while allowing a deliberate new run.
            if (state == "interrupted")
            {
                auto next = m_state;
                next["reconciled"] = true;
                QString error;
                if (!persist(next, &error))
                {
                    send("error", {{"message", error}}, sequence);
                    return;
                }
            }
        }
        report(QStringLiteral("已响应对账请求：Run ID=%1，返回状态=%2。")
                   .arg(id.left(64), answer["state"].toString()));
        send("runState", answer, sequence);
        return;
    }
    if (id.isEmpty() || id != run["runId"].toString())
        return;
    if (kind == "cancel")
    {
        if (state == "executing")
            finish("cancelled");
        else if (state == "terminal" || state == "acked")
            send("terminal", {{"result", m_state["result"]}});
        return;
    }
    if (kind == "resultAck" && (state == "terminal" || state == "acked"))
    {
        if (m_options.dropFirstResultAck && !m_ignoredAck)
        {
            m_ignoredAck = true;
            report(QStringLiteral(
                "故障模拟：已丢弃本次测试的第一次归档确认（resultAck），等待重试。"));
            return;
        }
        if (state == "terminal")
        {
            auto next = m_state;
            next["state"] = "acked";
            QString error;
            if (!persist(next, &error))
            {
                send("error", {{"message", error}}, sequence);
                return;
            }
            report(QStringLiteral("控制端已确认归档，确认状态已持久化；工位允许下一次测试。"));
        }
        send("resultConfirmed", {{"runId", id}}, sequence);
    }
}
void StationServer::advanceStep()
{
    if (m_state["state"] != "executing")
        return;
    const auto run = m_state["run"].toObject();
    const auto rules = run["recipe"].toObject()["steps"].toArray();
    auto steps = m_state["steps"].toArray();
    if (steps.size() >= rules.size())
    {
        finish("completed");
        return;
    }
    const int index = steps.size();
    const auto rule = rules[index].toObject();
    const auto stepId = rule["id"].toString();
    double value = rule["nominal"].toDouble();
    if (!m_options.failStep.isEmpty() && stepId == m_options.failStep)
        value = rule["max"].toDouble() + qMax(1.0, std::abs(rule["max"].toDouble()) * 0.1);
    steps.append(QJsonObject{{"stepId", stepId}, {"value", value}});
    auto next = m_state;
    next["steps"] = steps;
    QString error;
    if (!persist(next, &error))
    {
        send("error", {{"message", error}});
        return;
    }
    report(QStringLiteral("步骤 %1/%2 已完成：%3=%4 %5%6。")
               .arg(index + 1)
               .arg(rules.size())
               .arg(rule["label"].toString())
               .arg(value, 0, 'g', 10)
               .arg(rule["unit"].toString(),
                    stepId == m_options.failStep ? QStringLiteral("（已注入超限）") : QString()));
    send("progress",
         {{"runId", run["runId"]}, {"stepIndex", index}, {"stepId", stepId}, {"value", value}});
    if (steps.size() == rules.size())
        finish("completed");
    else
        m_stepTimer.start(m_options.stepMs);
}
void StationServer::finish(const QString &outcome)
{
    if (m_state["state"] != "executing")
        return;
    m_stepTimer.stop();
    m_pendingOutcome = outcome;
    const auto run = m_state["run"].toObject();
    const QJsonObject result{{"runId", run["runId"]}, {"stationId", run["stationId"]},
                             {"sn", run["sn"]},       {"recipeVersion", run["recipeVersion"]},
                             {"outcome", outcome},    {"steps", m_state["steps"]}};
    auto next = m_state;
    next["state"] = "terminal";
    next["result"] = result;
    QString error;
    if (!persist(next, &error))
    {
        send("error", {{"message", error}});
        return;
    }
    m_pendingOutcome.clear();
    report(
        QStringLiteral("测试%1，终态已持久化，等待控制端保存结果并确认。")
            .arg(outcome == "completed" ? QStringLiteral("执行完成") : QStringLiteral("已取消")));
    resendTerminal();
}
void StationServer::resendTerminal()
{
    if (m_state["state"] == "terminal")
        send("terminal", {{"result", m_state["result"]}});
    // A transient local storage failure pauses side effects and retries the same
    // durable transition. No ACK/terminal is emitted before it succeeds.
    else if (m_state["state"] == "executing" && !m_stepTimer.isActive())
    {
        if (!m_pendingOutcome.isEmpty())
            finish(m_pendingOutcome);
        else
            advanceStep();
    }
}
