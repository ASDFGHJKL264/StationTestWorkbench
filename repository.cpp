#include "repository.h"
#include "domain.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QUuid>
#include <QVariant>
#include <QSaveFile>
#include <QTimer>

namespace
{
// 生成带毫秒的 UTC 时间文本，统一记录数据库操作时间。
QString now()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}
// 将 o 对象转为紧凑 JSON 文本，供数据库保存。
QString json(const QJsonObject &o)
{
    return QString::fromUtf8(Domain::canonical(o));
}
// 把 v 数据库字段中的 JSON 文本解析为对象。
QJsonObject parse(const QVariant &v)
{
    return QJsonDocument::fromJson(v.toString().toUtf8()).object();
}
// 执行 q 查询，失败时通过 error 返回数据库原因。
bool exec(QSqlQuery &q, QString *error)
{
    if (q.exec())
        return true;
    if (error)
        *error = q.lastError().text();
    return false;
}
// 限制维护输入长度并拒绝控制字符，避免审计内容被截断或混淆。
bool maintenanceText(const QString &text, int minimum, int maximum)
{
    if (text.trimmed().size() < minimum || text.size() > maximum)
        return false;
    for (const auto character : text)
        if (!character.isPrint())
            return false;
    return true;
}
// 从 r 终态提取测量与身份字段，形成用于幂等核对的结果内容。
// Only the protocol's measurement fields define terminal identity. Remote verdicts
// cannot override the local evaluation, and UI metadata is not part of deduplication.
QJsonObject envelope(const QJsonObject &r)
{
    // 收集决定终态身份和测量内容的规范字段。
    QJsonObject out;
    for (const char *key : {"runId", "stationId", "sn", "recipeVersion", "outcome"})
        out[key] = r[key];
    // 收集仅含步骤编号和测量值的原始步骤列表。
    QJsonArray steps;
    for (const auto &v : r["steps"].toArray())
    {
        // 取出单个测量步骤，去除不参与终态核对的附加字段。
        const auto s = v.toObject();
        steps.append(QJsonObject{{"stepId", s["stepId"]}, {"value", s["value"]}});
    }
    out["steps"] = steps;
    return out;
}
} // namespace

Repository::Repository(QObject *parent) : QObject(parent) {}
Repository::~Repository()
{
    shutdown();
}
bool Repository::checkThread() const
{
    Q_ASSERT(QThread::currentThread() == thread());
    return QThread::currentThread() == thread();
}
bool Repository::ready(QString *error) const
{
    if (m_ready && m_db.isOpen())
        return true;
    if (error)
        *error = QStringLiteral("数据库尚未就绪");
    return false;
}
bool Repository::begin(QString *error)
{
    if (!ready(error))
        return false;
    // 执行立即写事务命令，尽早发现数据库写锁。
    QSqlQuery q(m_db);
    q.prepare("BEGIN IMMEDIATE");
    return exec(q, error);
}
bool Repository::commit(QString *error)
{
    if (m_db.commit())
        return true;
    if (error)
        *error = m_db.lastError().text();
    m_db.rollback();
    return false;
}

void Repository::initialize(const QString &path)
{
    if (!checkThread())
        return;
    if (m_ready)
    {
        emit initialized(true, {});
        return;
    }
    shutdown();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    {
        emit initialized(false, QStringLiteral("无法创建数据库目录"));
        return;
    }
    m_connectionName = "station-db-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    m_db.setDatabaseName(path);
    m_db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=250");
    // 保存连接、建表和恢复操作中的失败原因。
    QString error;
    if (!m_db.open())
    {
        error = m_db.lastError().text();
        shutdown();
        emit initialized(false, error);
        return;
    }
    // 收集启动后仍需与工位核对的测试请求。
    QJsonArray recovery;
    // 封装建表、升级和恢复读取，使用 error 与 recovery 接收输出。
    const auto initializeSchema = [this, &error, &recovery]() -> bool
    {
        // 执行初始化所需的设置、建表及恢复查询。
        QSqlQuery q(m_db);
        for (const auto &sql : {"PRAGMA foreign_keys=ON", "PRAGMA busy_timeout=250"})
        {
            if (!q.exec(sql))
            {
                error = q.lastError().text();
                return false;
            }
            q.finish();
        }
        if (!q.exec("PRAGMA user_version") || !q.next())
        {
            error = q.lastError().text();
            return false;
        }
        // 保存数据库当前模式版本，防止旧程序修改未来版本。
        const int version = q.value(0).toInt();
        q.finish();
        if (version > 3)
        {
            error = QStringLiteral("数据库版本高于程序支持版本，拒绝修改");
            return false;
        }
        if (version >= 3)
        {
            // 当前模式缺失安全记录表时停止启动，不能静默重建后丢失隔离。
            for (const auto *table :
                 {"manual_resolutions", "station_suspensions", "operational_events"})
            {
                q.prepare("SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
                q.addBindValue(QString::fromLatin1(table));
                if (!q.exec() || !q.next())
                {
                    error = QStringLiteral("版本3数据库缺少维护安全记录表");
                    return false;
                }
                q.finish();
            }
        }
        // 确认支持该库后才切换日志模式，拒绝未来库时保持文件不变。
        if (!q.exec("PRAGMA journal_mode=WAL"))
        {
            error = q.lastError().text();
            return false;
        }
        q.finish();
        if (!q.exec("BEGIN IMMEDIATE"))
        {
            error = q.lastError().text();
            return false;
        }
        // 保存配方、运行、结果和步骤表的建表语句。
        const QStringList schema{
            "CREATE TABLE IF NOT EXISTS recipes(version TEXT PRIMARY KEY, name TEXT NOT NULL, "
            "recipe_json TEXT NOT NULL, sha256 TEXT NOT NULL, published_at TEXT NOT NULL)",
            "CREATE TABLE IF NOT EXISTS runs(id INTEGER PRIMARY KEY AUTOINCREMENT,run_id TEXT NOT "
            "NULL UNIQUE,station_id INTEGER NOT NULL CHECK(station_id BETWEEN 1 AND 4),sn TEXT NOT "
            "NULL,recipe_version TEXT NOT NULL REFERENCES recipes(version),request_json TEXT NOT "
            "NULL,execution TEXT NOT NULL CHECK(execution IN "
            "('RESERVED','COMPLETED','CANCELLED','INTERRUPTED')),verdict TEXT NOT NULL "
            "CHECK(verdict IN ('PENDING','PASS','FAIL','CANCELLED','UNKNOWN')),started_at TEXT NOT "
            "NULL,finished_at TEXT NOT NULL DEFAULT '',reason TEXT NOT NULL DEFAULT "
            "'',recovery_pending INTEGER NOT NULL DEFAULT 0 CHECK(recovery_pending IN(0,1)))",
            "CREATE INDEX IF NOT EXISTS history_sn_cursor ON runs(sn,id DESC)",
            "CREATE TABLE IF NOT EXISTS results(run_id TEXT PRIMARY KEY REFERENCES "
            "runs(run_id),envelope_json TEXT NOT NULL,verdict TEXT NOT NULL)",
            "CREATE TABLE IF NOT EXISTS steps(run_id TEXT NOT NULL REFERENCES "
            "results(run_id),step_index INTEGER NOT NULL,step_id TEXT NOT NULL,label TEXT NOT "
            "NULL,unit TEXT NOT NULL,min_value REAL NOT NULL,max_value REAL NOT NULL,value REAL "
            "NOT NULL,passed INTEGER NOT NULL CHECK(passed IN(0,1)),PRIMARY "
            "KEY(run_id,step_index))",
            "CREATE TABLE IF NOT EXISTS manual_resolutions("
            "run_id TEXT PRIMARY KEY REFERENCES runs(run_id),actor TEXT NOT NULL,"
            "reason TEXT NOT NULL,resolved_at TEXT NOT NULL)",
            "CREATE TABLE IF NOT EXISTS station_suspensions("
            "station_id INTEGER PRIMARY KEY CHECK(station_id BETWEEN 1 AND 4),"
            "run_id TEXT NOT NULL,actor TEXT NOT NULL,reason TEXT NOT NULL,created_at TEXT NOT "
            "NULL)",
            "CREATE TABLE IF NOT EXISTS operational_events("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,occurred_at TEXT NOT NULL,actor TEXT NOT NULL,"
            "action TEXT NOT NULL,run_id TEXT NOT NULL,station_id INTEGER NOT NULL,reason TEXT NOT "
            "NULL)",
            "CREATE INDEX IF NOT EXISTS operational_run_events ON operational_events(run_id,id)"};
        for (const auto &sql : schema)
            if (!q.exec(sql))
            {
                error = q.lastError().text();
                return false;
            }
        if (!q.exec("PRAGMA table_info(runs)"))
        {
            error = q.lastError().text();
            return false;
        }
        // 记录 runs 表是否已包含恢复占用字段。
        bool hasRecovery = false;
        while (q.next())
            if (q.value(1).toString() == "recovery_pending")
                hasRecovery = true;
        q.finish();
        if (!hasRecovery)
        {
            if (version >= 2)
            {
                error = QStringLiteral("版本2数据库缺少对账占用字段");
                return false;
            }
            if (!q.exec("ALTER TABLE runs ADD COLUMN recovery_pending INTEGER NOT NULL DEFAULT 0 "
                        "CHECK(recovery_pending IN(0,1))"))
            {
                error = q.lastError().text();
                return false;
            }
        }
        // 保存占用唯一索引的重建语句及模式版本更新命令。
        // A restart cannot prove that a station stopped executing. Admission
        // stays locked until an authoritative query or terminal resolves it.
        const QStringList indexes{"DROP INDEX IF EXISTS one_active_station",
                                  "DROP INDEX IF EXISTS one_active_sn",
                                  "CREATE UNIQUE INDEX one_active_station ON runs(station_id) "
                                  "WHERE execution='RESERVED' OR recovery_pending=1",
                                  "CREATE UNIQUE INDEX one_active_sn ON runs(sn) WHERE "
                                  "execution='RESERVED' OR recovery_pending=1",
                                  "PRAGMA user_version=3"};
        for (const auto &sql : indexes)
            if (!q.exec(sql))
            {
                error = q.lastError().text();
                return false;
            }
        // 取得内置配方，首次启动时发布并核对已有版本内容。
        const auto recipe = Domain::defaultRecipe();
        q.prepare("INSERT OR IGNORE INTO recipes VALUES(?,?,?,?,?)");
        q.addBindValue(recipe["version"].toString());
        q.addBindValue(recipe["name"].toString());
        q.addBindValue(json(recipe));
        q.addBindValue(Domain::recipeHash(recipe));
        q.addBindValue(now());
        if (!exec(q, &error))
            return false;
        q.prepare("SELECT sha256 FROM recipes WHERE version=?");
        q.addBindValue(recipe["version"].toString());
        if (!exec(q, &error) || !q.next() || q.value(0).toString() != Domain::recipeHash(recipe))
        {
            if (error.isEmpty())
                error = QStringLiteral("内置配方版本存在不同内容，拒绝覆盖");
            return false;
        }
        q.finish();
        q.prepare("UPDATE runs SET "
                  "execution='INTERRUPTED',verdict='UNKNOWN',finished_at=?,reason=?,recovery_"
                  "pending=1 WHERE execution='RESERVED'");
        q.addBindValue(now());
        q.addBindValue(QStringLiteral("程序重新启动；原测试需要与工位对账，未自动重发动作"));
        if (!exec(q, &error))
            return false;
        q.finish();
        if (!q.exec("SELECT request_json FROM runs WHERE recovery_pending=1 ORDER BY id"))
        {
            error = q.lastError().text();
            return false;
        }
        while (q.next())
        {
            // 解析当前待恢复记录的完整测试请求。
            const auto request = parse(q.value(0));
            if (!Domain::validateRun(request, &error))
                return false;
            recovery.append(request);
        }
        q.finish();
        if (!m_db.commit())
        {
            error = m_db.lastError().text();
            return false;
        }
        return true;
    };
    // All QSqlQuery instances must be destroyed before removeDatabase().
    if (!initializeSchema())
    {
        m_db.rollback();
        shutdown();
        emit initialized(false, error);
        return;
    }
    m_ready = true;
    // 将人工结算编号和停用状态交给引擎，先恢复隔离再连接设备。
    QJsonArray closedRuns, suspendedStations;
    // 将查询限制在独立作用域内，失败清理连接时不再有查询句柄。
    const bool safetyLoaded = [&]() -> bool
    {
        // 查询重启后必须继续拒绝的人工结算运行。
        QSqlQuery closed(m_db);
        if (!closed.exec("SELECT run_id FROM manual_resolutions ORDER BY resolved_at"))
        {
            error = closed.lastError().text();
            return false;
        }
        while (closed.next())
            closedRuns.append(QJsonObject{{"runId", closed.value(0).toString()}});
        if (closed.lastError().isValid())
        {
            error = closed.lastError().text();
            return false;
        }
        closed.finish();
        // 查询重启后仍须停用的工位。
        QSqlQuery suspended(m_db);
        if (!suspended.exec("SELECT station_id,run_id,reason FROM station_suspensions"))
        {
            error = suspended.lastError().text();
            return false;
        }
        while (suspended.next())
            suspendedStations.append(QJsonObject{{"stationId", suspended.value(0).toInt()},
                                                 {"runId", suspended.value(1).toString()},
                                                 {"reason", suspended.value(2).toString()}});
        if (suspended.lastError().isValid())
        {
            error = suspended.lastError().text();
            return false;
        }
        return true;
    }();
    if (!safetyLoaded)
    {
        shutdown();
        emit initialized(false, error);
        return;
    }
    emit safetyStateReady(closedRuns, suspendedStations);
    emit recoverableRuns(recovery);
    emit initialized(true, {});
}

void Repository::reserveRun(const QJsonObject &request)
{
    if (!checkThread())
        return;
    // 保存测试预约校验或数据库写入失败的原因。
    QString error;
    if (!ready(&error) || !Domain::validateRun(request, &error) || !begin(&error))
    {
        emit runReserved(request, false, error);
        return;
    }
    // 记录本次预约事务是否可以提交。
    bool ok = false;
    {
        // 读取已发布配方的摘要，核对请求绑定的内容。
        QSqlQuery recipe(m_db);
        recipe.prepare("SELECT sha256 FROM recipes WHERE version=?");
        recipe.addBindValue(request["recipeVersion"].toString());
        // 停用工位即使被其他调用绕过界面也不能预约。
        QSqlQuery suspension(m_db);
        suspension.prepare("SELECT 1 FROM station_suspensions WHERE station_id=?");
        suspension.addBindValue(request["stationId"].toInt());
        if (!exec(suspension, &error))
        {
            m_db.rollback();
            emit runReserved(request, false, error);
            return;
        }
        if (suspension.next())
        {
            m_db.rollback();
            emit runReserved(request, false, QStringLiteral("工位已停用，需管理员确认复位后启用"));
            return;
        }
        suspension.finish();
        if (exec(recipe, &error) && recipe.next() &&
            recipe.value(0).toString() == Domain::recipeHash(request["recipe"].toObject()))
        {
            recipe.finish();
            // 插入测试身份和预约占用记录。
            QSqlQuery q(m_db);
            q.prepare("INSERT INTO "
                      "runs(run_id,station_id,sn,recipe_version,request_json,execution,verdict,"
                      "started_at) VALUES(?,?,?,?,?,'RESERVED','PENDING',?)");
            q.addBindValue(request["runId"].toString());
            q.addBindValue(request["stationId"].toInt());
            q.addBindValue(request["sn"].toString());
            q.addBindValue(request["recipeVersion"].toString());
            q.addBindValue(json(request));
            q.addBindValue(now());
            ok = exec(q, &error);
        }
        else if (error.isEmpty())
            error = QStringLiteral("配方未发布或其版本内容与发布记录不一致");
    }
    if (ok)
        ok = commit(&error);
    else
        m_db.rollback();
    emit runReserved(request, ok, error);
}

void Repository::saveResult(const QJsonObject &result)
{
    if (!checkThread())
        return;
    // error 保存失败原因，verdict 保存按绑定配方计算的总判定。
    QString error, verdict;
    // 保存通过本地规则复核后的步骤明细。
    QJsonArray evaluated;
    // 在原始终态上附加本地判定，作为归档成功的回报。
    QJsonObject enriched = result;
    // ok 表示事务成功，duplicate 表示该 runId 的相同终态已存在。
    bool ok = false, duplicate = false;
    if (!begin(&error))
    {
        emit resultSaved(result, false, false, error);
        return;
    }
    {
        // 封装一次终态事务的校验、去重以及各表写入操作。
        const auto write = [&]() -> bool
        {
            // 人工结算的运行保持 UNKNOWN，迟到终态不能覆盖处置记录。
            QSqlQuery resolution(m_db);
            resolution.prepare("SELECT 1 FROM manual_resolutions WHERE run_id=?");
            resolution.addBindValue(result["runId"].toString());
            if (!exec(resolution, &error))
                return false;
            if (resolution.next())
            {
                error = QStringLiteral("该测试已人工结算，拒绝迟到终态覆盖");
                return false;
            }
            resolution.finish();
            // 读取 runId 对应的原始测试请求快照。
            QSqlQuery run(m_db);
            run.prepare("SELECT request_json FROM runs WHERE run_id=?");
            run.addBindValue(result["runId"].toString());
            if (!exec(run, &error))
                return false;
            if (!run.next())
            {
                error = QStringLiteral("终态没有对应的已持久化测试身份");
                return false;
            }
            // 还原已预约的测试身份和配方，防止使用当前配方重新解释旧结果。
            const auto request = parse(run.value(0));
            run.finish();
            if (!Domain::evaluate(request, result, &evaluated, &verdict, &error))
                return false;
            // 保存规范终态文本，用于比较同一 runId 的重复或冲突结果。
            const auto bytes = json(envelope(result));
            // 查询该测试编号是否已经保存过结果。
            QSqlQuery existing(m_db);
            existing.prepare("SELECT envelope_json FROM results WHERE run_id=?");
            existing.addBindValue(result["runId"].toString());
            if (!exec(existing, &error))
                return false;
            if (existing.next())
            {
                duplicate = existing.value(0).toString() == bytes;
                if (!duplicate)
                    error = QStringLiteral("同一测试编号的终态内容冲突，拒绝覆盖原始结果");
                return duplicate;
            }
            existing.finish();
            // 写入终态内容并更新运行的执行和质量状态。
            QSqlQuery q(m_db);
            q.prepare("INSERT INTO results(run_id,envelope_json,verdict) VALUES(?,?,?)");
            q.addBindValue(result["runId"].toString());
            q.addBindValue(bytes);
            q.addBindValue(verdict);
            if (!exec(q, &error))
                return false;
            for (qsizetype i = 0; i < evaluated.size(); ++i)
            {
                // 保存当前已复核步骤的限值、测量值与判定。
                const auto s = evaluated[i].toObject();
                // 向步骤表插入一条完整追溯明细。
                QSqlQuery step(m_db);
                step.prepare("INSERT INTO steps VALUES(?,?,?,?,?,?,?,?,?)");
                step.addBindValue(result["runId"].toString());
                step.addBindValue(int(i));
                step.addBindValue(s["stepId"].toString());
                step.addBindValue(s["label"].toString());
                step.addBindValue(s["unit"].toString());
                step.addBindValue(s["min"].toDouble());
                step.addBindValue(s["max"].toDouble());
                step.addBindValue(s["value"].toDouble());
                step.addBindValue(s["passed"].toBool() ? 1 : 0);
                if (!exec(step, &error))
                    return false;
            }
            q.prepare(
                "UPDATE runs SET execution=?,verdict=?,finished_at=?,reason=CASE WHEN EXISTS("
                "SELECT 1 FROM operational_events WHERE operational_events.run_id=runs.run_id "
                "AND action='TIMEOUT') THEN reason ELSE '' END,recovery_pending=0 "
                "WHERE run_id=?");
            q.addBindValue(result["outcome"] == "cancelled" ? "CANCELLED" : "COMPLETED");
            q.addBindValue(verdict);
            q.addBindValue(now());
            q.addBindValue(result["runId"].toString());
            return exec(q, &error);
        };
        ok = write();
    }
    if (ok)
        ok = commit(&error);
    else
        m_db.rollback();
    if (ok)
    {
        enriched["evaluatedSteps"] = evaluated;
        enriched["verdict"] = verdict;
    }
    emit resultSaved(enriched, ok, ok && duplicate, error);
}

QJsonArray Repository::loadSteps(const QString &runId, QString *error)
{
    // 按测试编号读取已经归档的步骤，保持执行顺序。
    QSqlQuery q(m_db);
    q.prepare("SELECT step_id,label,unit,min_value,max_value,value,passed FROM steps WHERE "
              "run_id=? ORDER BY step_index");
    q.addBindValue(runId);
    // 收集当前运行的归档步骤明细。
    QJsonArray rows;
    if (!exec(q, error))
        return rows;
    while (q.next())
        rows.append(QJsonObject{{"stepId", q.value(0).toString()},
                                {"label", q.value(1).toString()},
                                {"unit", q.value(2).toString()},
                                {"min", q.value(3).toDouble()},
                                {"max", q.value(4).toDouble()},
                                {"value", q.value(5).toDouble()},
                                {"passed", q.value(6).toBool()}});
    if (q.lastError().isValid() && error)
        *error = q.lastError().text();
    return rows;
}

void Repository::queryHistory(const QString &snFilter, int beforeId)
{
    // 将旧的精确 SN 查询接口转换为复合筛选条件。
    HistoryFilter filter;
    filter.sn = snFilter;
    queryFilteredHistory(filter, beforeId);
}

void Repository::queryFilteredHistory(const HistoryFilter &filter, int beforeId, quint64 requestId)
{
    if (!checkThread())
        return;
    // 回报当前 requestId 的查询失败，error 为具体失败原因。
    const auto fail = [this, requestId](const QString &error)
    {
        emit operationError(error);
        emit historyQueryFailed(requestId, error);
    };
    // 保存历史查询及步骤补全失败的原因。
    QString error;
    if (!ready(&error))
    {
        fail(error);
        return;
    }
    // 将起始本地日期转换为包含边界的 UTC 起点。
    const auto startUtc =
        filter.startDate.isValid() ? filter.startDate.startOfDay().toUTC() : QDateTime{};
    // 将结束日期的次日零点转换为不包含边界的 UTC 终点。
    // Advance the local calendar date before converting: a local day need not be 24 hours.
    const auto endUtc =
        filter.endDate.isValid() ? filter.endDate.addDays(1).startOfDay().toUTC() : QDateTime{};
    if (filter.sn.size() > 64 || beforeId < 0 || filter.stationId < 0 || filter.stationId > 4 ||
        (!filter.verdict.isEmpty() && filter.verdict != "PASS" && filter.verdict != "FAIL" &&
         filter.verdict != "UNKNOWN") ||
        (filter.startDate.isValid() && !startUtc.isValid()) ||
        (filter.endDate.isValid() && !endUtc.isValid()) ||
        (filter.startDate.isValid() && filter.endDate.isValid() &&
         filter.startDate > filter.endDate))
    {
        fail(QStringLiteral("历史查询参数无效"));
        return;
    }
    // 执行带复合筛选和稳定游标的历史查询。
    QSqlQuery q(m_db);
    // 逐项组装历史查询条件，所有筛选值均通过参数绑定。
    // Exact SN matches use the (sn,id) index. Cursor pagination remains stable when
    // a new result is inserted while the user is browsing later pages.
    QString sql = "SELECT "
                  "id,run_id,station_id,sn,recipe_version,execution,verdict,started_at,finished_at,"
                  "reason,recovery_pending FROM runs WHERE 1=1";
    if (!filter.sn.isEmpty())
        sql += " AND sn=?";
    if (filter.stationId > 0)
        sql += " AND station_id=?";
    if (!filter.verdict.isEmpty())
        sql += " AND verdict=?";
    if (startUtc.isValid())
        sql += " AND started_at>=?";
    if (endUtc.isValid())
        sql += " AND started_at<?";
    if (beforeId > 0)
        sql += " AND id<?";
    sql += " ORDER BY id DESC LIMIT 101";
    q.prepare(sql);
    if (!filter.sn.isEmpty())
        q.addBindValue(filter.sn);
    if (filter.stationId > 0)
        q.addBindValue(filter.stationId);
    if (!filter.verdict.isEmpty())
        q.addBindValue(filter.verdict);
    if (startUtc.isValid())
        q.addBindValue(startUtc.toString(Qt::ISODateWithMs));
    if (endUtc.isValid())
        q.addBindValue(endUtc.toString(Qt::ISODateWithMs));
    if (beforeId > 0)
        q.addBindValue(beforeId);
    if (!exec(q, &error))
    {
        fail(error);
        return;
    }
    // 收集本页最多一百条历史运行记录。
    QJsonArray rows;
    // 记录本页之后是否仍有满足条件的历史记录。
    bool more = false;
    while (q.next())
    {
        if (rows.size() == 100)
        {
            more = true;
            break;
        }
        rows.append(QJsonObject{{"id", q.value(0).toInt()},
                                {"runId", q.value(1).toString()},
                                {"stationId", q.value(2).toInt()},
                                {"sn", q.value(3).toString()},
                                {"recipeVersion", q.value(4).toString()},
                                {"execution", q.value(5).toString()},
                                {"verdict", q.value(6).toString()},
                                {"startedAt", q.value(7).toString()},
                                {"finishedAt", q.value(8).toString()},
                                {"reason", q.value(9).toString()},
                                {"recoveryPending", q.value(10).toBool()}});
    }
    if (q.lastError().isValid())
    {
        fail(q.lastError().text());
        return;
    }
    q.finish();
    for (qsizetype i = 0; i < rows.size(); ++i)
    {
        // 取出当前历史记录，补充其归档步骤明细。
        auto row = rows[i].toObject();
        row["steps"] = loadSteps(row["runId"].toString(), &error);
        rows[i] = row;
    }
    if (!error.isEmpty())
    {
        fail(error);
        return;
    }
    emit historyReady(rows, more);
    emit historyQueryReady(requestId, rows, more);
}

void Repository::exportFailedSns(const QString &path)
{
    if (!checkThread())
        return;
    // 保存补全 CSV 扩展名后的最终输出路径。
    const QString output =
        path.isEmpty() || path.endsWith(".csv", Qt::CaseInsensitive) ? path : path + ".csv";
    // 保存异常 SN 查询、文件写入或提交失败的原因。
    QString error;
    // 统一回报异常 SN 清单导出失败，并保留原目标文件。
    const auto fail = [&] { emit failedSnsExported(output, false, 0, error); };
    if (!ready(&error))
    {
        fail();
        return;
    }
    if (path.isEmpty())
    {
        error = QStringLiteral("异常 SN 清单路径不能为空");
        fail();
        return;
    }
    // 按历史 FAIL 去重读取全部异常产品 SN。
    // Export committed failures from the whole history, independently of UI paging.
    // A later passing retest does not erase the product's historical failure.
    QSqlQuery query(m_db);
    query.setForwardOnly(true);
    query.prepare("SELECT DISTINCT sn FROM runs WHERE execution='COMPLETED' AND verdict='FAIL' "
                  "ORDER BY sn");
    if (!exec(query, &error))
    {
        fail();
        return;
    }
    // 通过临时文件和原子提交输出 CSV 清单。
    QSaveFile file(output);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
    {
        error = file.errorString();
        fail();
        return;
    }
    // 写入 bytes 字节，失败时记录文件错误原因。
    const auto write = [&](const QByteArray &bytes)
    {
        if (file.write(bytes) == bytes.size())
            return true;
        error = file.errorString();
        return false;
    };
    // UTF-8 BOM keeps Chinese serial numbers readable in common spreadsheet tools.
    if (!write(QByteArray("\xEF\xBB\xBF"
                          "SN\r\n")))
    {
        fail();
        return;
    }
    // 统计已成功写入临时清单的去重 SN 数量。
    qint64 count = 0;
    while (query.next())
    {
        // 保存当前异常 SN，随后转义 CSV 引号。
        QString sn = query.value(0).toString();
        sn.replace('"', "\"\"");
        if (!write((QStringLiteral("\"") + sn + QStringLiteral("\"\r\n")).toUtf8()))
        {
            fail();
            return;
        }
        ++count;
    }
    if (query.lastError().isValid())
    {
        error = query.lastError().text();
        fail();
        return;
    }
    query.finish();
    if (!file.commit())
    {
        error = file.errorString();
        fail();
        return;
    }
    emit failedSnsExported(output, true, count, {});
}

void Repository::exportReport(const QString &runId, const QString &path)
{
    if (!checkThread())
        return;
    // 保存报告查询、内容校验或文件输出失败的原因。
    QString error;
    // 保存补全 HTML 扩展名后的最终报告路径。
    const QString output =
        path.endsWith(".html", Qt::CaseInsensitive) || path.endsWith(".htm", Qt::CaseInsensitive)
            ? path
            : path + ".html";
    // 统一回报 HTML 报告导出失败及具体原因。
    const auto fail = [&] { emit reportReady(output, false, error); };
    if (!ready(&error))
    {
        fail();
        return;
    }
    if (runId.isEmpty() || runId.size() > 64 || path.isEmpty())
    {
        error = QStringLiteral("测试编号或报告路径无效");
        fail();
        return;
    }
    // 读取报告所需的运行快照、状态、时间和恢复标志。
    QSqlQuery query(m_db);
    query.prepare(
        "SELECT request_json,execution,verdict,started_at,finished_at,reason,recovery_pending FROM "
        "runs WHERE run_id=?");
    query.addBindValue(runId);
    if (!exec(query, &error))
    {
        fail();
        return;
    }
    if (!query.next())
    {
        error = QStringLiteral("未找到指定测试运行");
        fail();
        return;
    }
    // 还原报告对应的原始测试身份和绑定配方。
    const auto request = parse(query.value(0));
    if (!Domain::validateRun(request, &error))
    {
        fail();
        return;
    }
    // execution 保存执行状态，verdict 保存质量判定，started、finished 保存起止时间，reason
    // 保存处置说明。
    const QString execution = query.value(1).toString(), verdict = query.value(2).toString(),
                  started = query.value(3).toString(), finished = query.value(4).toString(),
                  reason = query.value(5).toString();
    // 标记该运行是否仍需恢复核对并保持占用。
    const bool recovery = query.value(6).toBool();
    query.finish();
    // 读取该运行已经归档的步骤测量和判定。
    const auto measured = loadSteps(runId, &error);
    if (!error.isEmpty())
    {
        fail();
        return;
    }
    // 将 text 转为 HTML 安全文本，避免身份内容被解释成标签。
    const auto escape = [](const QString &text) { return text.toHtmlEscaped(); };
    // 将 value 数值按十二位有效数字转换为报告文本。
    const auto numeric = [](const QJsonValue &value)
    { return QString::number(value.toDouble(), 'g', 12); };
    // 逐段组装最终导出的 HTML 报告内容。
    QString html = QStringLiteral(
        "<!doctype html><html lang='zh-CN'><meta charset='utf-8'><meta name='viewport' "
        "content='width=device-width'><title>测试报告</title><style>body{font:15px 'Microsoft "
        "YaHei UI',sans-serif;color:#243449;max-width:1000px;margin:40px auto;padding:0 "
        "20px}h1{color:#132c46}table{width:100%;border-collapse:collapse}td,th{padding:12px;border:"
        "1px solid "
        "#dce4ee;text-align:left}th{background:#edf3f9}.notice{background:#fff3d9;padding:14px}."
        "pass{color:#087f65}.fail{color:#c43b47}dt{font-weight:bold;float:left;clear:left;width:"
        "180px}dd{margin-left:190px;margin-bottom:10px}</style><h1>电子模块功能测试报告</h1><p "
        "class='notice'>模拟工位数据，非真实电气测量或计量证明。报告为导出时的状态快照。</p><dl>");
    // 收集报告中的产品、运行、配方、时间及处置信息字段。
    const QList<QPair<QString, QString>> identity{
        {QStringLiteral("产品 SN"), request["sn"].toString()},
        {QStringLiteral("测试编号 runId"), runId},
        {QStringLiteral("工位"), QString::number(request["stationId"].toInt())},
        {QStringLiteral("配方名称"), request["recipe"].toObject()["name"].toString()},
        {QStringLiteral("配方版本"), request["recipeVersion"].toString()},
        {QStringLiteral("配方 SHA-256"), Domain::recipeHash(request["recipe"].toObject())},
        {QStringLiteral("执行状态"), execution},
        {QStringLiteral("质量判定"), verdict},
        {QStringLiteral("开始时间 UTC"), started},
        {QStringLiteral("结束时间 UTC"), finished},
        {QStringLiteral("待恢复对账"),
         recovery ? QStringLiteral("是，保持占用") : QStringLiteral("否")},
        {QStringLiteral("处置说明"), reason}};
    for (const auto &field : identity)
        html += "<dt>" + escape(field.first) + "</dt><dd>" + escape(field.second) + "</dd>";
    html += QStringLiteral(
        "</dl><h2>步骤与绑定限值</h2><table><thead><tr><th>序号</th><th>测试项</th><th>下限</"
        "th><th>上限</th><th>实测值</th><th>单位</th><th>判定</th></tr></thead><tbody>");
    // 累计生成报告表格中面向读者显示的步骤序号。
    int index = 0;
    for (const auto &value : request["recipe"].toObject()["steps"].toArray())
    {
        // 保存运行快照中的步骤限值与显示信息。
        const auto rule = value.toObject();
        // 保存与当前规则匹配的已归档测量，未执行时保持为空。
        QJsonObject sample;
        for (const auto &step : measured)
            if (step.toObject()["stepId"] == rule["id"])
            {
                sample = step.toObject();
                break;
            }
        // 标记该步骤是否有已归档测量值。
        const bool available = sample["value"].isDouble();
        // 保存该步骤的 PASS、FAIL 或未执行说明文字。
        const QString judgment = available ? (sample["passed"].toBool() ? "PASS" : "FAIL")
                                           : QStringLiteral("未执行/未归档");
        html += "<tr><td>" + QString::number(++index) + "</td><td>" +
                escape(rule["label"].toString()) + "</td><td>" + numeric(rule["min"]) +
                "</td><td>" + numeric(rule["max"]) + "</td><td>" +
                (available ? numeric(sample["value"]) : QStringLiteral("—")) + "</td><td>" +
                escape(rule["unit"].toString()) + "</td><td class='" +
                (available ? (sample["passed"].toBool() ? QString("pass") : QString("fail"))
                           : QString()) +
                "'>" + judgment + "</td></tr>";
    }
    html += QStringLiteral("</tbody></table><p>导出时间 UTC：") + escape(now()) +
            QStringLiteral("</p></html>");
    // 通过临时文件和原子提交输出完整 HTML 报告。
    QSaveFile file(output);
    file.setDirectWriteFallback(false);
    // 保存报告的 UTF-8 字节，供完整写入校验。
    const auto bytes = html.toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    {
        error = file.errorString();
        fail();
        return;
    }
    emit reportReady(output, true, {});
}

void Repository::listRecipes()
{
    if (!checkThread())
        return;
    // 保存读取已发布配方失败的具体原因。
    QString error;
    if (!ready(&error))
    {
        emit operationError(error);
        return;
    }
    // 查询全部已发布配方，按发布时间和版本排序。
    QSqlQuery q(m_db);
    q.prepare("SELECT recipe_json FROM recipes ORDER BY published_at,version");
    if (!exec(q, &error))
    {
        emit operationError(error);
        return;
    }
    // 收集提供给界面和引擎的完整配方对象。
    QJsonArray rows;
    while (q.next())
        rows.append(parse(q.value(0)));
    if (q.lastError().isValid())
    {
        emit operationError(q.lastError().text());
        return;
    }
    emit recipesReady(rows);
}

void Repository::publishRecipe(const QJsonObject &recipe)
{
    // 保存配方校验、版本冲突或事务失败的原因。
    QString error;
    if (!checkThread())
    {
        error = QStringLiteral("配方发布必须在数据库所属线程执行");
        emit operationError(error);
        emit recipePublishFinished(recipe, false, error);
        return;
    }
    if (!ready(&error) || !Domain::validateRecipe(recipe, &error) || !begin(&error))
    {
        emit operationError(error);
        emit recipePublishFinished(recipe, false, error);
        return;
    }
    // 记录配方内容核对或新版本插入是否成功。
    bool ok = false;
    {
        // 读取同版本的已有摘要，确保发布版本不可原地更改。
        QSqlQuery existing(m_db);
        existing.prepare("SELECT sha256 FROM recipes WHERE version=?");
        existing.addBindValue(recipe["version"].toString());
        if (exec(existing, &error))
        {
            if (existing.next())
            {
                ok = existing.value(0).toString() == Domain::recipeHash(recipe);
                if (!ok)
                    error = QStringLiteral("已发布配方版本不可修改；请使用新的版本号");
            }
            else
            {
                existing.finish();
                // 插入尚未发布过的新配方版本及内容摘要。
                QSqlQuery q(m_db);
                q.prepare("INSERT INTO recipes VALUES(?,?,?,?,?)");
                q.addBindValue(recipe["version"].toString());
                q.addBindValue(recipe["name"].toString());
                q.addBindValue(json(recipe));
                q.addBindValue(Domain::recipeHash(recipe));
                q.addBindValue(now());
                ok = exec(q, &error);
            }
        }
    }
    if (ok)
        ok = commit(&error);
    else
        m_db.rollback();
    if (!ok)
        emit operationError(error);
    emit recipePublishFinished(recipe, ok, error);
}

bool Repository::insertAudit(const QString &actor, const QString &action, const QString &runId,
                             int stationId, const QString &reason, QString *error)
{
    // 审计与业务变更复用调用者事务，保证不会只改状态而漏记操作。
    QSqlQuery query(m_db);
    query.prepare("INSERT INTO operational_events(occurred_at,actor,action,run_id,station_id,"
                  "reason) VALUES(?,?,?,?,?,?)");
    query.addBindValue(now());
    query.addBindValue(actor);
    query.addBindValue(action);
    query.addBindValue(runId.isNull() ? QStringLiteral("") : runId);
    query.addBindValue(stationId);
    query.addBindValue(reason);
    return exec(query, error);
}

void Repository::queryMaintenance()
{
    if (!checkThread())
        return;
    // 保存维护读取失败原因及待回报的三个列表。
    QString error;
    // 分别收集待核对任务、审计记录和停用工位。
    QJsonArray exceptions, audit, suspended;
    if (!ready(&error))
    {
        emit operationError(error);
        return;
    }
    // 统一读取未结算运行，已归档结果不进入人工处置候选。
    QSqlQuery query(m_db);
    if (!query.exec("SELECT station_id,run_id,sn,execution,verdict,reason,recovery_pending,"
                    "started_at FROM runs WHERE execution='RESERVED' OR recovery_pending=1 "
                    "ORDER BY id DESC"))
    {
        emit operationError(query.lastError().text());
        return;
    }
    while (query.next())
        exceptions.append(QJsonObject{{"stationId", query.value(0).toInt()},
                                      {"runId", query.value(1).toString()},
                                      {"sn", query.value(2).toString()},
                                      {"execution", query.value(3).toString()},
                                      {"verdict", query.value(4).toString()},
                                      {"reason", query.value(5).toString()},
                                      {"recoveryPending", query.value(6).toBool()},
                                      {"startedAt", query.value(7).toString()}});
    query.finish();
    if (!query.exec("SELECT station_id,run_id,reason FROM station_suspensions ORDER BY station_id"))
    {
        emit operationError(query.lastError().text());
        return;
    }
    while (query.next())
        suspended.append(QJsonObject{{"stationId", query.value(0).toInt()},
                                     {"runId", query.value(1).toString()},
                                     {"reason", query.value(2).toString()}});
    query.finish();
    if (!query.exec("SELECT occurred_at,actor,action,run_id,station_id,reason FROM "
                    "operational_events ORDER BY id DESC LIMIT 100"))
    {
        emit operationError(query.lastError().text());
        return;
    }
    while (query.next())
        audit.append(QJsonObject{{"occurredAt", query.value(0).toString()},
                                 {"actor", query.value(1).toString()},
                                 {"action", query.value(2).toString()},
                                 {"runId", query.value(3).toString()},
                                 {"stationId", query.value(4).toInt()},
                                 {"reason", query.value(5).toString()}});
    emit maintenanceReady(QJsonObject{
        {"exceptions", exceptions}, {"audit", audit}, {"suspendedStations", suspended}});
}

void Repository::resolveManually(int stationId, const QString &runId, const QString &actor,
                                 const QString &reason, bool confirmedStopped)
{
    if (!checkThread())
        return;
    // 接收校验或事务失败原因，失败时保留全部占用与原记录。
    QString error;
    if (stationId < 1 || stationId > 4 || !maintenanceText(runId, 1, 64) ||
        !maintenanceText(actor, 1, 80) || !maintenanceText(reason, 5, 1000) || !confirmedStopped)
    {
        emit manualDispositionSaved(
            stationId, runId, false,
            QStringLiteral("人工处置须确认设备已停止，并填写操作者及至少5字原因"));
        return;
    }
    if (!begin(&error))
    {
        emit manualDispositionSaved(stationId, runId, false, error);
        return;
    }
    // 将状态、人工结算、停用和审计作为不可分割的事务。
    const auto resolve = [&]() -> bool
    {
        // 只允许处置尚未归档且身份一致的未结算运行。
        QSqlQuery query(m_db);
        query.prepare("SELECT execution,recovery_pending,EXISTS(SELECT 1 FROM results WHERE "
                      "results.run_id=runs.run_id) FROM runs WHERE run_id=? AND station_id=?");
        query.addBindValue(runId);
        query.addBindValue(stationId);
        if (!exec(query, &error))
            return false;
        if (!query.next() ||
            (query.value(0).toString() != "RESERVED" && !query.value(1).toBool()) ||
            query.value(2).toBool())
        {
            error = QStringLiteral("测试已结算或已有真实结果，拒绝人工覆盖");
            return false;
        }
        query.finish();
        query.prepare("UPDATE runs SET execution='INTERRUPTED',verdict='UNKNOWN',finished_at=?,"
                      "reason=?,recovery_pending=0 WHERE run_id=? AND station_id=?");
        query.addBindValue(now());
        query.addBindValue(QStringLiteral("人工结算：") + reason);
        query.addBindValue(runId);
        query.addBindValue(stationId);
        if (!exec(query, &error))
            return false;
        query.prepare("INSERT INTO manual_resolutions(run_id,actor,reason,resolved_at) "
                      "VALUES(?,?,?,?)");
        query.addBindValue(runId);
        query.addBindValue(actor);
        query.addBindValue(reason);
        query.addBindValue(now());
        if (!exec(query, &error))
            return false;
        query.prepare(
            "INSERT INTO station_suspensions(station_id,run_id,actor,reason,created_at) "
            "VALUES(?,?,?,?,?) ON CONFLICT(station_id) DO UPDATE SET run_id=excluded."
            "run_id,actor=excluded.actor,reason=excluded.reason,created_at=excluded.created_at");
        query.addBindValue(stationId);
        query.addBindValue(runId);
        query.addBindValue(actor);
        query.addBindValue(reason);
        query.addBindValue(now());
        return exec(query, &error) &&
               insertAudit(actor, "MANUAL_RESOLUTION", runId, stationId, reason, &error);
    };
    // 仅在四部分全部成功后提交并通知网络线程释放旧运行。
    bool ok = resolve();
    if (ok)
        ok = commit(&error);
    else
        m_db.rollback();
    emit manualDispositionSaved(stationId, runId, ok, error);
}

void Repository::resumeStation(int stationId, const QString &actor, const QString &reason)
{
    if (!checkThread())
        return;
    // 保存启用校验或事务的失败原因。
    QString error;
    if (stationId < 1 || stationId > 4 || !maintenanceText(actor, 1, 80) ||
        !maintenanceText(reason, 5, 1000))
    {
        emit stationResumed(stationId, false,
                            QStringLiteral("启用工位须填写操作者及至少5字复位说明"));
        return;
    }
    if (!begin(&error))
    {
        emit stationResumed(stationId, false, error);
        return;
    }
    // 在同一事务中读出原关联、解除停用并保存审计。
    bool ok = false;
    {
        // 工位尚有未结算运行时不能单独解除停用，避免界面与设备准入失配。
        QSqlQuery active(m_db);
        active.prepare("SELECT 1 FROM runs WHERE station_id=? AND "
                       "(execution='RESERVED' OR recovery_pending=1) LIMIT 1");
        active.addBindValue(stationId);
        if (!exec(active, &error) || active.next())
        {
            if (error.isEmpty())
                error = QStringLiteral("该工位仍有未结算测试，请先核对或人工结算");
            active.finish();
            m_db.rollback();
            emit stationResumed(stationId, false, error);
            return;
        }
        active.finish();
        // 取得停用关联的旧运行编号，保留启用动作的追溯身份。
        QSqlQuery query(m_db);
        query.prepare("SELECT run_id FROM station_suspensions WHERE station_id=?");
        query.addBindValue(stationId);
        if (exec(query, &error) && query.next())
        {
            // 保存恢复启用所对应的原停用运行编号。
            const auto runId = query.value(0).toString();
            query.finish();
            query.prepare("DELETE FROM station_suspensions WHERE station_id=?");
            query.addBindValue(stationId);
            ok = exec(query, &error) &&
                 insertAudit(actor, "RESUME_STATION", runId, stationId, reason, &error);
        }
        else if (error.isEmpty())
            error = QStringLiteral("该工位未被停用");
    }
    if (ok)
        ok = commit(&error);
    else
        m_db.rollback();
    emit stationResumed(stationId, ok, error);
}

void Repository::recordTimeout(const QString &runId, int stationId, const QString &reason)
{
    if (!checkThread() || !m_ready)
        return;
    // 保存超时审计失败原因；短暂写锁冲突会稍后重试。
    QString error;
    if (!maintenanceText(runId, 1, 64) || stationId < 1 || stationId > 4 ||
        !maintenanceText(reason, 1, 1000))
    {
        emit operationError(QStringLiteral("超时审计参数无效"));
        return;
    }
    // 确认同一次超时没有重复审计，并关联到真实预约身份。
    bool ok = begin(&error);
    if (ok)
    {
        // 查询超时事件是否已在前次尝试中提交。
        QSqlQuery query(m_db);
        query.prepare("SELECT 1 FROM operational_events WHERE run_id=? AND action='TIMEOUT'");
        query.addBindValue(runId);
        ok = exec(query, &error);
        if (ok && query.next())
        {
            query.finish();
            m_db.rollback();
            return;
        }
        query.finish();
        if (ok)
        {
            query.prepare("UPDATE runs SET reason=? WHERE run_id=? AND station_id=? AND NOT "
                          "EXISTS(SELECT 1 FROM manual_resolutions WHERE run_id=?)");
            query.addBindValue(reason);
            query.addBindValue(runId);
            query.addBindValue(stationId);
            query.addBindValue(runId);
            ok = exec(query, &error);
            if (ok && query.numRowsAffected() != 1)
            {
                query.finish();
                m_db.rollback();
                return;
            }
        }
        if (ok)
            ok = insertAudit("system", "TIMEOUT", runId, stationId, reason, &error);
        if (ok)
            ok = commit(&error);
        else
            m_db.rollback();
    }
    if (!ok)
    {
        emit operationError(QStringLiteral("超时审计待重试：") + error);
        QTimer::singleShot(1000, this, [this, runId, stationId, reason]
                           { recordTimeout(runId, stationId, reason); });
    }
}

void Repository::recordStationSuspension(int stationId, const QString &runId, const QString &reason)
{
    if (!checkThread() || !m_ready)
        return;
    // 保存自动隔离持久化的失败原因。
    QString error;
    if (stationId < 1 || stationId > 4 || !maintenanceText(runId, 1, 64) ||
        !maintenanceText(reason, 1, 1000))
        return;
    // 保存安全隔离和审计，确保重启后仍不能向旧设备准入。
    bool ok = begin(&error);
    if (ok)
    {
        // 避免同一旧设备反复连接时产生重复隔离记录。
        QSqlQuery query(m_db);
        query.prepare("INSERT OR IGNORE INTO station_suspensions(station_id,run_id,actor,reason,"
                      "created_at) VALUES(?,?,'system',?,?)");
        query.addBindValue(stationId);
        query.addBindValue(runId);
        query.addBindValue(reason);
        query.addBindValue(now());
        ok = exec(query, &error);
        if (ok && query.numRowsAffected() > 0)
            ok = insertAudit("system", "STATION_SUSPENDED", runId, stationId, reason, &error);
        if (ok)
            ok = commit(&error);
        else
            m_db.rollback();
    }
    if (!ok)
    {
        emit operationError(QStringLiteral("工位隔离待重试：") + error);
        QTimer::singleShot(1000, this, [this, stationId, runId, reason]
                           { recordStationSuspension(stationId, runId, reason); });
    }
}

void Repository::recordAudit(const QString &actor, const QString &action, const QString &runId,
                             int stationId, const QString &reason)
{
    if (!checkThread())
        return;
    // 接收维护审计校验或写入失败原因。
    QString error;
    if (!maintenanceText(actor, 1, 80) || !maintenanceText(action, 1, 80) ||
        (!runId.isEmpty() && !maintenanceText(runId, 1, 64)) || stationId < 0 || stationId > 4 ||
        !maintenanceText(reason, 1, 1000))
    {
        emit operationError(QStringLiteral("维护审计参数无效"));
        return;
    }
    if (!begin(&error))
    {
        emit operationError(error);
        return;
    }
    if (!insertAudit(actor, action, runId, stationId, reason, &error))
        m_db.rollback();
    else if (commit(&error))
        return;
    emit operationError(error);
}

void Repository::markInterrupted(const QString &id, const QString &reason)
{
    if (!checkThread())
        return;
    // 保存中断记录事务失败的原因，供保持占用并重试。
    QString error;
    if (!begin(&error))
    {
        emit operationError(error);
        emit runInterrupted(id, false, error);
        return;
    }
    // 记录中断状态更新是否成功并可提交。
    bool ok;
    {
        // 将目标运行更新为中断，并解除已核对的恢复占用。
        QSqlQuery q(m_db);
        q.prepare("UPDATE runs SET "
                  "execution='INTERRUPTED',verdict='UNKNOWN',finished_at=?,reason=?,recovery_"
                  "pending=0 WHERE run_id=? AND (execution='RESERVED' OR recovery_pending=1)");
        q.addBindValue(now());
        q.addBindValue(reason.left(1000));
        q.addBindValue(id);
        ok = exec(q, &error);
    }
    if (ok)
        ok = commit(&error);
    else
        m_db.rollback();
    if (!ok)
        emit operationError(error);
    emit runInterrupted(id, ok, error);
}

void Repository::shutdown()
{
    if (!checkThread())
        return;
    m_ready = false;
    if (m_db.isValid())
    {
        m_db.close();
        m_db = QSqlDatabase();
    }
    if (!m_connectionName.isEmpty())
    {
        QSqlDatabase::removeDatabase(m_connectionName);
        m_connectionName.clear();
    }
}
