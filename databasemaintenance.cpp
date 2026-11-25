#include "databasemaintenance.h"
#include "domain.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QThread>
#include <QUuid>
#include <QVariant>

namespace
{
// 按本机文件系统规则比较已经规范化的路径。
bool samePath(const QString &left, const QString &right)
{
#ifdef Q_OS_WIN
    return QDir::cleanPath(left).compare(QDir::cleanPath(right), Qt::CaseInsensitive) == 0;
#else
    return QDir::cleanPath(left) == QDir::cleanPath(right);
#endif
}

// 拒绝链接或路径重定向，避免备份和恢复写入其他目录。
bool ordinaryPath(const QString &path, QString *error)
{
    if (path.isEmpty() || path.contains(QChar::Null))
    {
        *error = QStringLiteral("维护路径不能为空或包含空字符");
        return false;
    }
    // 从目标逐级检查已有父目录的真实位置。
    QString current = QFileInfo(path).absoluteFilePath();
    while (!current.isEmpty())
    {
        // 取得当前路径的类型和解析后的真实路径。
        const QFileInfo entry(current);
        if (entry.isSymLink() ||
            (entry.exists() && !samePath(entry.absoluteFilePath(), entry.canonicalFilePath())))
        {
            *error = QStringLiteral("维护路径不能经过链接或重定向目录");
            return false;
        }
        // 逐级向上，磁盘根目录处结束。
        const auto parent = entry.absolutePath();
        if (samePath(parent, current))
            break;
        current = parent;
    }
    return true;
}

// 管理本函数作用域内的 SQLite 连接，析构时关闭并移除连接名。
class ReadDatabase
{
  public:
    // 以只读模式打开已有 path，不会创建空数据库。
    explicit ReadDatabase(const QString &path, QString *error)
    {
        m_name = "maintenance-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        db = QSqlDatabase::addDatabase("QSQLITE", m_name);
        db.setDatabaseName(path);
        db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=1500");
        if (!db.open())
            *error = db.lastError().text();
    }
    // 在查询对象均销毁后释放独立连接。
    ~ReadDatabase()
    {
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_name);
    }
    // 只在当前维护线程内使用的连接。
    QSqlDatabase db;

  private:
    // 区分并发维护和主仓储连接的唯一名称。
    QString m_name;
};

// 对固定维护查询执行并保存数据库错误。
bool execute(QSqlQuery &query, const QString &sql, QString *error)
{
    if (query.exec(sql))
        return true;
    *error = query.lastError().text();
    return false;
}

// 检查可信表名对应的真实表及必须列，拒绝以视图冒充数据表。
bool tableColumns(QSqlDatabase &database, const QString &table, const QStringList &required,
                  QString *error)
{
    // 检查 sqlite_master 中对象的实际类型。
    QSqlQuery query(database);
    query.prepare("SELECT type FROM sqlite_master WHERE name=?");
    query.addBindValue(table);
    if (!query.exec() || !query.next() || query.value(0).toString() != "table")
    {
        *error = QStringLiteral("备份缺少有效数据表：") + table;
        return false;
    }
    query.finish();
    if (!execute(query, "PRAGMA table_info(" + table + ")", error))
        return false;
    // 收集实际列名，允许旧版本存在兼容性扩展列。
    QSet<QString> columns;
    while (query.next())
        columns.insert(query.value(1).toString());
    for (const auto &column : required)
        if (!columns.contains(column))
        {
            *error = QStringLiteral("备份表缺少字段：") + table + "." + column;
            return false;
        }
    return true;
}

// 解析单条受限 JSON 对象，避免损坏文件中的超大记录进入内存。
QJsonObject objectValue(const QVariant &value, QString *error)
{
    // 保存数据库中的原始 JSON 字节。
    const auto bytes = value.toString().toUtf8();
    if (bytes.size() > 128 * 1024)
    {
        *error = QStringLiteral("备份中的单条测试数据过大");
        return {};
    }
    // 接收 JSON 语法检查结果。
    QJsonParseError parseError;
    // 保存解析出的完整 JSON 文档。
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        *error = QStringLiteral("备份含有损坏的 JSON 数据");
        return {};
    }
    return document.object();
}

// 逐条核对配方、运行、终态和测量明细的业务一致性。
bool validateContent(QSqlDatabase &database, int version, QString *error)
{
    // 读取发布配方并检查其摘要没有被破坏。
    QSqlQuery recipes(database);
    if (!execute(recipes, "SELECT version,name,recipe_json,sha256 FROM recipes", error))
        return false;
    while (recipes.next())
    {
        // 保存当前发布配方的完整内容。
        const auto recipe = objectValue(recipes.value(2), error);
        if (!error->isEmpty() || !Domain::validateRecipe(recipe, error) ||
            recipe["version"].toString() != recipes.value(0).toString() ||
            recipe["name"].toString() != recipes.value(1).toString() ||
            Domain::recipeHash(recipe) != recipes.value(3).toString())
        {
            if (error->isEmpty())
                *error = QStringLiteral("备份配方身份或摘要不一致");
            return false;
        }
    }
    if (recipes.lastError().isValid())
    {
        *error = recipes.lastError().text();
        return false;
    }
    recipes.finish();
    // 使用顺序读取，历史数量增长时也不把全部测试加载到内存。
    QSqlQuery runs(database);
    if (!execute(runs,
                 "SELECT run_id,station_id,sn,recipe_version,request_json,execution,"
                 "verdict FROM runs",
                 error))
        return false;
    while (runs.next())
    {
        // 保存当前运行请求并核对表字段与请求快照的身份。
        const auto request = objectValue(runs.value(4), error);
        if (!error->isEmpty() || !Domain::validateRun(request, error) ||
            request["runId"].toString() != runs.value(0).toString() ||
            request["stationId"].toInt() != runs.value(1).toInt() ||
            request["sn"].toString() != runs.value(2).toString() ||
            request["recipeVersion"].toString() != runs.value(3).toString())
        {
            if (error->isEmpty())
                *error = QStringLiteral("备份中的测试身份不一致");
            return false;
        }
        // 取出需要一致检查的执行和质量状态。
        const auto execution = runs.value(5).toString(), verdict = runs.value(6).toString();
        if (!((execution == "RESERVED" && verdict == "PENDING") ||
              (execution == "INTERRUPTED" && verdict == "UNKNOWN") ||
              (execution == "COMPLETED" && (verdict == "PASS" || verdict == "FAIL")) ||
              (execution == "CANCELLED" && verdict == "CANCELLED")))
        {
            *error = QStringLiteral("备份中的运行状态组合无效");
            return false;
        }
        // 检查请求中绑定的配方确实已发布且内容未被改写。
        QSqlQuery recipe(database);
        recipe.prepare("SELECT sha256 FROM recipes WHERE version=?");
        recipe.addBindValue(request["recipeVersion"].toString());
        if (!recipe.exec() || !recipe.next() ||
            recipe.value(0).toString() != Domain::recipeHash(request["recipe"].toObject()))
        {
            *error = QStringLiteral("备份运行的配方快照与发布版本不一致");
            return false;
        }
        recipe.finish();
        // 核对已完成运行确有经过本地判定的终态。
        QSqlQuery result(database);
        result.prepare("SELECT envelope_json,verdict FROM results WHERE run_id=?");
        result.addBindValue(request["runId"].toString());
        if (!result.exec())
        {
            *error = result.lastError().text();
            return false;
        }
        if (!result.next())
        {
            if (execution == "COMPLETED" || execution == "CANCELLED")
            {
                *error = QStringLiteral("备份缺少已结束测试的终态");
                return false;
            }
            continue;
        }
        // 按当时配方重新计算原始终态，拒绝被改写的质量判定。
        const auto terminal = objectValue(result.value(0), error);
        // 保存按绑定配方重新判定的步骤明细。
        QJsonArray evaluated;
        // 保存按本地规则重新计算的总判定。
        QString calculated;
        if (!error->isEmpty() ||
            !Domain::evaluate(request, terminal, &evaluated, &calculated, error) ||
            calculated != verdict || result.value(1).toString() != verdict ||
            (terminal["outcome"] == "completed" ? execution != "COMPLETED"
                                                : execution != "CANCELLED"))
        {
            if (error->isEmpty())
                *error = QStringLiteral("备份终态或质量判定不一致");
            return false;
        }
        result.finish();
        // 对照全部已归档步骤，检查顺序、限值、测量值及判定。
        QSqlQuery steps(database);
        steps.prepare("SELECT step_index,step_id,label,unit,min_value,max_value,value,passed "
                      "FROM steps WHERE run_id=? ORDER BY step_index");
        steps.addBindValue(request["runId"].toString());
        if (!steps.exec())
        {
            *error = steps.lastError().text();
            return false;
        }
        int index = 0;
        while (steps.next())
        {
            // 保存当前位置对应的本地计算步骤。
            const auto expected =
                index < evaluated.size() ? evaluated[index].toObject() : QJsonObject{};
            if (index >= evaluated.size() || steps.value(0).toInt() != index ||
                steps.value(0).isNull() || steps.value(1).isNull() || steps.value(2).isNull() ||
                steps.value(3).isNull() || steps.value(4).isNull() || steps.value(5).isNull() ||
                steps.value(6).isNull() || steps.value(7).isNull() ||
                steps.value(1).toString() != expected["stepId"].toString() ||
                steps.value(2).toString() != expected["label"].toString() ||
                steps.value(3).toString() != expected["unit"].toString() ||
                steps.value(4).toDouble() != expected["min"].toDouble() ||
                steps.value(5).toDouble() != expected["max"].toDouble() ||
                steps.value(6).toDouble() != expected["value"].toDouble() ||
                steps.value(7).toBool() != expected["passed"].toBool())
            {
                *error = QStringLiteral("备份测量明细与原终态不一致");
                return false;
            }
            ++index;
        }
        if (steps.lastError().isValid())
        {
            *error = steps.lastError().text();
            return false;
        }
        if (index != evaluated.size())
        {
            *error = QStringLiteral("备份缺少已执行步骤");
            return false;
        }
    }
    if (runs.lastError().isValid())
    {
        *error = runs.lastError().text();
        return false;
    }
    if (version >= 3)
    {
        // 人工结算只允许保持未知，并且不能存在真实测量结果。
        QSqlQuery resolutions(database);
        if (!execute(resolutions,
                     "SELECT 1 FROM manual_resolutions m JOIN runs r ON r.run_id=m.run_id "
                     "WHERE r.execution!='INTERRUPTED' OR r.verdict!='UNKNOWN' OR "
                     "r.recovery_pending!=0 OR EXISTS(SELECT 1 FROM results t WHERE "
                     "t.run_id=m.run_id) LIMIT 1",
                     error))
            return false;
        if (resolutions.next())
        {
            *error = QStringLiteral("备份中的人工结算状态不一致");
            return false;
        }
    }
    return true;
}

// 只读验证数据库的物理结构、模式和业务数据。
bool validate(const QString &path, QString *error)
{
    if (!ordinaryPath(path, error) || !QFileInfo(path).isFile())
    {
        if (error->isEmpty())
            *error = QStringLiteral("备份文件不存在");
        return false;
    }
    // 维护连接仅在本函数所属线程中存在。
    ReadDatabase connection(QFileInfo(path).absoluteFilePath(), error);
    if (!connection.db.isOpen())
        return false;
    // 在同一只读事务中验证各表，避免验证期间看到不同时间的数据。
    QSqlQuery query(connection.db);
    if (!execute(query, "PRAGMA trusted_schema=OFF", error) || !execute(query, "BEGIN", error) ||
        !execute(query, "PRAGMA quick_check", error))
        return false;
    if (!query.next() || query.value(0).toString() != "ok" || query.next())
    {
        *error = QStringLiteral("备份数据库完整性检查失败");
        return false;
    }
    query.finish();
    if (!execute(query, "PRAGMA user_version", error) || !query.next())
        return false;
    // 仅接受本程序支持的旧模式或当前模式。
    const auto version = query.value(0).toInt();
    query.finish();
    if (version < 1 || version > 3)
    {
        *error = QStringLiteral("备份数据库版本不受支持");
        return false;
    }
    if (!execute(query, "SELECT 1 FROM sqlite_master WHERE type IN ('view','trigger') LIMIT 1",
                 error))
        return false;
    if (query.next())
    {
        *error = QStringLiteral("备份包含非标准视图或触发器，拒绝恢复");
        return false;
    }
    query.finish();
    if (!tableColumns(connection.db, "recipes",
                      {"version", "name", "recipe_json", "sha256", "published_at"}, error) ||
        !tableColumns(connection.db, "runs",
                      {"id", "run_id", "station_id", "sn", "recipe_version", "request_json",
                       "execution", "verdict", "started_at", "finished_at", "reason"},
                      error) ||
        !tableColumns(connection.db, "results", {"run_id", "envelope_json", "verdict"}, error) ||
        !tableColumns(connection.db, "steps",
                      {"run_id", "step_index", "step_id", "label", "unit", "min_value", "max_value",
                       "value", "passed"},
                      error))
        return false;
    if (version >= 2 && !tableColumns(connection.db, "runs", {"recovery_pending"}, error))
        return false;
    if (version >= 3 &&
        (!tableColumns(connection.db, "manual_resolutions",
                       {"run_id", "actor", "reason", "resolved_at"}, error) ||
         !tableColumns(connection.db, "station_suspensions",
                       {"station_id", "run_id", "actor", "reason", "created_at"}, error) ||
         !tableColumns(connection.db, "operational_events",
                       {"id", "occurred_at", "actor", "action", "run_id", "station_id", "reason"},
                       error)))
        return false;
    if (!execute(query, "PRAGMA foreign_key_check", error))
        return false;
    if (query.next())
    {
        *error = QStringLiteral("备份存在断开的数据关联");
        return false;
    }
    query.finish();
    return validateContent(connection.db, version, error);
}

// 为完整快照、验证和原子发布预留足够空间。
bool enoughSpace(const QString &source, const QString &directory, QString *error)
{
    // 获取目标卷可用空间与源库及 WAL 的当前总大小。
    QStorageInfo storage(directory);
    // 合计源数据库及尚未归并 WAL 的当前大小。
    const auto sourceBytes = QFileInfo(source).size() + QFileInfo(source + "-wal").size();
    // 为快照与原子复制预留保守的磁盘空间下限。
    const auto required = qMax<qint64>(8 * 1024 * 1024, sourceBytes * 2) + 16 * 1024 * 1024;
    if (!storage.isValid() || !storage.isReady() || storage.bytesAvailable() < required)
    {
        *error = QStringLiteral("目标磁盘可用空间不足，未发布备份或恢复文件");
        return false;
    }
    return true;
}

// 将验证通过的临时快照原子发布，绝不覆盖已有文件。
bool publish(const QString &temporary, const QString &destination, QString *error)
{
    if (QFileInfo::exists(destination))
    {
        *error = QStringLiteral("目标文件已存在，请选择新的备份或恢复位置");
        return false;
    }
    // 以流方式复制验证后的快照，避免整库进入内存。
    QFile source(temporary);
    // 通过临时写入和原子提交发布最终文件。
    QSaveFile target(destination);
    target.setDirectWriteFallback(false);
    if (!source.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly))
    {
        *error = QStringLiteral("无法打开备份发布文件：") + target.errorString();
        return false;
    }
    while (!source.atEnd())
    {
        // 保存本次复制块，便于检测短读或磁盘写入失败。
        const auto bytes = source.read(1024 * 1024);
        if ((bytes.isEmpty() && source.error() != QFileDevice::NoError) ||
            target.write(bytes) != bytes.size())
        {
            *error = QStringLiteral("备份发布写入失败");
            return false;
        }
    }
    if (QFileInfo::exists(destination) || !target.commit())
    {
        *error = QStringLiteral("无法原子发布备份：") + target.errorString();
        return false;
    }
    return true;
}

// 从单独的只读连接生成 WAL 一致快照，验证成功后再发布。
bool snapshot(const QString &source, const QString &destination, QString *error)
{
    // 固定目标绝对路径，保证临时文件只产生在选定目录。
    const auto target = QFileInfo(destination).absoluteFilePath();
    // 保存本次操作的目标目录或隔离测试目录。
    const auto directory = QFileInfo(target).absolutePath();
    if (!ordinaryPath(source, error) || !ordinaryPath(target, error) ||
        !QFileInfo(source).isFile() || QFileInfo::exists(target) ||
        samePath(QFileInfo(source).absoluteFilePath(), target))
    {
        if (error->isEmpty())
            *error = QStringLiteral("备份源无效、目标已存在或目标与源相同");
        return false;
    }
    if (!QDir().mkpath(directory) || !enoughSpace(source, directory, error))
    {
        if (error->isEmpty())
            *error = QStringLiteral("无法创建备份目录");
        return false;
    }
    // 临时路径由本次操作独占，清理时不会匹配其他文件。
    const auto temporary = QDir(directory).filePath(
        ".workbench-snapshot-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".tmp");
    // 保存快照步骤是否执行成功，所有连接在验证和信号前关闭。
    bool ok = false;
    {
        // 单独的只读连接不会占用 Repository 的实时归档队列。
        ReadDatabase connection(QFileInfo(source).absoluteFilePath(), error);
        if (connection.db.isOpen())
        {
            // SQLite VACUUM INTO 从同一读快照包含已提交 WAL 数据。
            QSqlQuery query(connection.db);
            if (execute(query, "PRAGMA synchronous=FULL", error) &&
                execute(query, "PRAGMA trusted_schema=OFF", error))
            {
                query.prepare("VACUUM INTO ?");
                query.addBindValue(temporary);
                ok = query.exec();
                if (!ok)
                    *error = query.lastError().text();
            }
        }
    }
    if (ok)
        ok = validate(temporary, error) && publish(temporary, target, error);
    // 仅删除本次生成的精确临时路径和 SQLite 附属文件。
    for (const auto &suffix :
         {QString(), QStringLiteral("-wal"), QStringLiteral("-shm"), QStringLiteral("-journal")})
        QFile::remove(temporary + suffix);
    return ok;
}

// 持有新目录锁期间还原；退出本函数时先关闭连接再释放目录锁。
bool restore(const QString &backup, const QString &directory, QString *error)
{
    if (!ordinaryPath(directory, error) || !ordinaryPath(backup, error))
        return false;
    // 只接受新空目录，避免覆盖现有数据库、配置或业务文件。
    const QFileInfo target(directory);
    if (target.exists() && (!target.isDir() || !QDir(directory)
                                                    .entryList(QDir::AllEntries | QDir::Hidden |
                                                               QDir::System | QDir::NoDotAndDotDot)
                                                    .isEmpty()))
    {
        *error = QStringLiteral("恢复目录必须为空，原数据目录不会被覆盖");
        return false;
    }
    if (!QDir().mkpath(directory))
    {
        *error = QStringLiteral("无法创建恢复目录");
        return false;
    }
    // 与主程序使用相同锁名，避免恢复过程中启动同一数据目录。
    QLockFile lock(QDir(directory).filePath(".workbench.lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
    {
        *error = QStringLiteral("恢复目录正被另一个工作台使用");
        return false;
    }
    return snapshot(backup, QDir(directory).filePath("data/workbench.db"), error);
}
} // namespace

void DatabaseMaintenance::backupDatabase(const QString &sourcePath, const QString &destinationPath)
{
    Q_ASSERT(QThread::currentThread() == thread());
    // 保存独立快照和发布过程中可能出现的失败原因。
    QString error;
    // 保存本次维护操作是否完整成功。
    const bool ok = snapshot(sourcePath, destinationPath, &error);
    emit backupFinished(destinationPath, ok, error);
}

void DatabaseMaintenance::validateBackup(const QString &path)
{
    Q_ASSERT(QThread::currentThread() == thread());
    // 保存只读完整性、模式和业务检查的失败原因。
    QString error;
    // 保存本次维护操作是否完整成功。
    const bool ok = validate(path, &error);
    emit validationFinished(path, ok, error);
}

void DatabaseMaintenance::restoreToDirectory(const QString &backupPath,
                                             const QString &newDataDirectory)
{
    Q_ASSERT(QThread::currentThread() == thread());
    // 保存新目录恢复失败原因，完成后目录锁和连接均已释放。
    QString error;
    // 保存本次维护操作是否完整成功。
    const bool ok = restore(backupPath, newDataDirectory, &error);
    emit restoreFinished(newDataDirectory, ok, error);
}
