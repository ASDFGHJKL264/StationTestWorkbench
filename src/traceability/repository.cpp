#include "repository.h"
#include "testing/domain.h"
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

namespace
{
QString now()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}
QString json(const QJsonObject &o)
{
    return QString::fromUtf8(Domain::canonical(o));
}
QJsonObject parse(const QVariant &v)
{
    return QJsonDocument::fromJson(v.toString().toUtf8()).object();
}
bool exec(QSqlQuery &q, QString *error)
{
    if (q.exec())
        return true;
    if (error)
        *error = q.lastError().text();
    return false;
}
// Only the protocol's measurement fields define terminal identity. Remote verdicts
// cannot override the local evaluation, and UI metadata is not part of deduplication.
QJsonObject envelope(const QJsonObject &r)
{
    QJsonObject out;
    for (const char *key : {"runId", "stationId", "sn", "recipeVersion", "outcome"})
        out[key] = r[key];
    QJsonArray steps;
    for (const auto &v : r["steps"].toArray())
    {
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
    QString error;
    if (!m_db.open())
    {
        error = m_db.lastError().text();
        shutdown();
        emit initialized(false, error);
        return;
    }
    QJsonArray recovery;
    const auto initializeSchema = [this, &error, &recovery]() -> bool
    {
        QSqlQuery q(m_db);
        for (const auto &sql :
             {"PRAGMA foreign_keys=ON", "PRAGMA journal_mode=WAL", "PRAGMA busy_timeout=250"})
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
        const int version = q.value(0).toInt();
        q.finish();
        if (version > 2)
        {
            error = QStringLiteral("数据库版本高于程序支持版本，拒绝修改");
            return false;
        }
        if (!q.exec("BEGIN IMMEDIATE"))
        {
            error = q.lastError().text();
            return false;
        }
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
            "KEY(run_id,step_index))"};
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
        // A restart cannot prove that a station stopped executing. Admission
        // stays locked until an authoritative query or terminal resolves it.
        const QStringList indexes{"DROP INDEX IF EXISTS one_active_station",
                                  "DROP INDEX IF EXISTS one_active_sn",
                                  "CREATE UNIQUE INDEX one_active_station ON runs(station_id) "
                                  "WHERE execution='RESERVED' OR recovery_pending=1",
                                  "CREATE UNIQUE INDEX one_active_sn ON runs(sn) WHERE "
                                  "execution='RESERVED' OR recovery_pending=1",
                                  "PRAGMA user_version=2"};
        for (const auto &sql : indexes)
            if (!q.exec(sql))
            {
                error = q.lastError().text();
                return false;
            }
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
    emit recoverableRuns(recovery);
    emit initialized(true, {});
}

void Repository::reserveRun(const QJsonObject &request)
{
    if (!checkThread())
        return;
    QString error;
    if (!ready(&error) || !Domain::validateRun(request, &error) || !begin(&error))
    {
        emit runReserved(request, false, error);
        return;
    }
    bool ok = false;
    {
        QSqlQuery recipe(m_db);
        recipe.prepare("SELECT sha256 FROM recipes WHERE version=?");
        recipe.addBindValue(request["recipeVersion"].toString());
        if (exec(recipe, &error) && recipe.next() &&
            recipe.value(0).toString() == Domain::recipeHash(request["recipe"].toObject()))
        {
            recipe.finish();
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
    QString error, verdict;
    QJsonArray evaluated;
    QJsonObject enriched = result;
    bool ok = false, duplicate = false;
    if (!begin(&error))
    {
        emit resultSaved(result, false, false, error);
        return;
    }
    {
        const auto write = [&]() -> bool
        {
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
            const auto request = parse(run.value(0));
            run.finish();
            if (!Domain::evaluate(request, result, &evaluated, &verdict, &error))
                return false;
            const auto bytes = json(envelope(result));
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
            QSqlQuery q(m_db);
            q.prepare("INSERT INTO results(run_id,envelope_json,verdict) VALUES(?,?,?)");
            q.addBindValue(result["runId"].toString());
            q.addBindValue(bytes);
            q.addBindValue(verdict);
            if (!exec(q, &error))
                return false;
            for (qsizetype i = 0; i < evaluated.size(); ++i)
            {
                const auto s = evaluated[i].toObject();
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
                "UPDATE runs SET execution=?,verdict=?,finished_at=?,reason='',recovery_pending=0 "
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
    QSqlQuery q(m_db);
    q.prepare("SELECT step_id,label,unit,min_value,max_value,value,passed FROM steps WHERE "
              "run_id=? ORDER BY step_index");
    q.addBindValue(runId);
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

void Repository::queryHistory(const QString &filter, int beforeId)
{
    if (!checkThread())
        return;
    QString error;
    if (!ready(&error))
    {
        emit operationError(error);
        return;
    }
    if (filter.size() > 64 || beforeId < 0)
    {
        emit operationError(QStringLiteral("历史查询参数无效"));
        return;
    }
    QSqlQuery q(m_db);
    // Exact SN matches use the (sn,id) index. Cursor pagination remains stable when
    // a new result is inserted while the user is browsing later pages.
    QString sql = "SELECT "
                  "id,run_id,station_id,sn,recipe_version,execution,verdict,started_at,finished_at,"
                  "reason,recovery_pending FROM runs WHERE 1=1";
    if (!filter.isEmpty())
        sql += " AND sn=?";
    if (beforeId > 0)
        sql += " AND id<?";
    sql += " ORDER BY id DESC LIMIT 101";
    q.prepare(sql);
    if (!filter.isEmpty())
        q.addBindValue(filter);
    if (beforeId > 0)
        q.addBindValue(beforeId);
    if (!exec(q, &error))
    {
        emit operationError(error);
        return;
    }
    QJsonArray rows;
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
        emit operationError(q.lastError().text());
        return;
    }
    q.finish();
    for (qsizetype i = 0; i < rows.size(); ++i)
    {
        auto row = rows[i].toObject();
        row["steps"] = loadSteps(row["runId"].toString(), &error);
        rows[i] = row;
    }
    if (!error.isEmpty())
    {
        emit operationError(error);
        return;
    }
    emit historyReady(rows, more);
}

void Repository::exportFailedSns(const QString &path)
{
    if (!checkThread())
        return;
    const QString output =
        path.isEmpty() || path.endsWith(".csv", Qt::CaseInsensitive) ? path : path + ".csv";
    QString error;
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
    QSaveFile file(output);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
    {
        error = file.errorString();
        fail();
        return;
    }
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
    qint64 count = 0;
    while (query.next())
    {
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
    QString error;
    const QString output =
        path.endsWith(".html", Qt::CaseInsensitive) || path.endsWith(".htm", Qt::CaseInsensitive)
            ? path
            : path + ".html";
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
    const auto request = parse(query.value(0));
    if (!Domain::validateRun(request, &error))
    {
        fail();
        return;
    }
    const QString execution = query.value(1).toString(), verdict = query.value(2).toString(),
                  started = query.value(3).toString(), finished = query.value(4).toString(),
                  reason = query.value(5).toString();
    const bool recovery = query.value(6).toBool();
    query.finish();
    const auto measured = loadSteps(runId, &error);
    if (!error.isEmpty())
    {
        fail();
        return;
    }
    const auto escape = [](const QString &text) { return text.toHtmlEscaped(); };
    const auto numeric = [](const QJsonValue &value)
    { return QString::number(value.toDouble(), 'g', 12); };
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
    int index = 0;
    for (const auto &value : request["recipe"].toObject()["steps"].toArray())
    {
        const auto rule = value.toObject();
        QJsonObject sample;
        for (const auto &step : measured)
            if (step.toObject()["stepId"] == rule["id"])
            {
                sample = step.toObject();
                break;
            }
        const bool available = sample["value"].isDouble();
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
    QSaveFile file(output);
    file.setDirectWriteFallback(false);
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
    QString error;
    if (!ready(&error))
    {
        emit operationError(error);
        return;
    }
    QSqlQuery q(m_db);
    q.prepare("SELECT recipe_json FROM recipes ORDER BY published_at,version");
    if (!exec(q, &error))
    {
        emit operationError(error);
        return;
    }
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
    if (!checkThread())
        return;
    QString error;
    if (!ready(&error) || !Domain::validateRecipe(recipe, &error) || !begin(&error))
    {
        emit operationError(error);
        return;
    }
    bool ok = false;
    {
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
}

void Repository::markInterrupted(const QString &id, const QString &reason)
{
    if (!checkThread())
        return;
    QString error;
    if (!begin(&error))
    {
        emit operationError(error);
        emit runInterrupted(id, false, error);
        return;
    }
    bool ok;
    {
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
