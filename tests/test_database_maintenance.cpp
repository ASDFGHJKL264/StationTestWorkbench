#include "databasemaintenance.h"
#include "repository.h"
#include "domain.h"
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

namespace
{
// 为备份测试生成完整身份和默认配方请求。
QJsonObject makeRun(const QString &id, int station = 1)
{
    // 绑定本次测试应保存的不可变配方。
    const auto recipe = Domain::defaultRecipe();
    return {{"runId", id},
            {"stationId", station},
            {"sn", "SN-" + id},
            {"recipeVersion", recipe["version"]},
            {"recipe", recipe}};
}

// 为 run 生成真实可被本地规则判定为 PASS 的原始终态。
QJsonObject finish(const QJsonObject &run)
{
    // 收集与配方严格同序的测量值。
    QJsonArray steps;
    for (const auto &value : run["recipe"].toObject()["steps"].toArray())
    {
        // 读取当前步骤身份和标称测量值。
        const auto step = value.toObject();
        steps.append(QJsonObject{{"stepId", step["id"]}, {"value", step["nominal"]}});
    }
    return {{"runId", run["runId"]},  {"stationId", run["stationId"]},
            {"sn", run["sn"]},        {"recipeVersion", run["recipeVersion"]},
            {"outcome", "completed"}, {"steps", steps}};
}

// 为测试读取或故障注入提供独立连接。
class Probe
{
  public:
    // 打开 path，测试可显式检查 db 是否正常。
    explicit Probe(const QString &path)
    {
        name = "backup-test-" + QUuid::createUuid().toString();
        db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(path);
        db.open();
    }
    // 关闭测试连接，允许后续恢复或目录清理。
    ~Probe()
    {
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }
    // 执行固定测试查询，供核对或注入数据故障。
    bool execute(const QString &sql)
    {
        // 保存本次测试指令的执行结果。
        QSqlQuery query(db);
        return query.exec(sql);
    }
    // 读取固定测试查询的首个标量。
    QString value(const QString &sql)
    {
        // 取得当前要验证的标量结果。
        QSqlQuery query(db);
        if (!query.exec(sql) || !query.next())
            return {};
        return query.value(0).toString();
    }
    // 当前测试专用数据库连接。
    QSqlDatabase db;

  private:
    // 区分其他测试和正式连接的唯一名称。
    QString name;
};

// 对文件原始内容计算摘要，验证失败操作没有覆盖原件。
QByteArray hashFile(const QString &path)
{
    // 读取要验证的文件内容。
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}
} // namespace

class DatabaseMaintenanceTests final : public QObject
{
    Q_OBJECT
  private slots:
    // 验证备份包含已提交 WAL，忽略在途写入，且回报前释放文件句柄。
    void walBackupIsConsistentAndConnectionsAreClosed()
    {
        // 隔离源库、WAL 和目标备份。
        QTemporaryDir directory;
        // 保存必须保留的源数据库路径。
        const auto source = directory.filePath("live.db");
        // 保存本次验证应生成的新备份或恢复位置。
        const auto target = directory.filePath("snapshot.sqlite");
        // 创建真实配方、运行预约和终态的测试仓储。
        Repository repository;
        repository.initialize(source);
        // 先提交一份原始测量结果，再保留源连接和 WAL。
        const auto run = makeRun("committed");
        repository.reserveRun(run);
        repository.saveResult(finish(run));
        QVERIFY(QFileInfo(source + "-wal").size() > 32);
        // 在另一个写事务中制造不能进入备份的临时值。
        Probe writer(source);
        QVERIFY(writer.execute("BEGIN IMMEDIATE"));
        QVERIFY(writer.execute("UPDATE runs SET sn='UNCOMMITTED'"));
        // 执行独立快照、验证和恢复的待测维护对象。
        DatabaseMaintenance maintenance;
        // 记录快照发布的完成状态和失败原因。
        QSignalSpy backedUp(&maintenance, &DatabaseMaintenance::backupFinished);
        // 在同步回调中改名，证明验证和源快照连接已关闭。
        bool renameWorked = false;
        connect(&maintenance, &DatabaseMaintenance::backupFinished, &maintenance,
                [&](const QString &path, bool ok, const QString &)
                {
                    if (ok)
                    {
                        renameWorked = QFile::rename(path, path + ".renamed");
                        if (renameWorked)
                            renameWorked = QFile::rename(path + ".renamed", path);
                    }
                });
        maintenance.backupDatabase(source, target);
        QCOMPARE(backedUp.size(), 1);
        QVERIFY2(backedUp.last()[1].toBool(), qPrintable(backedUp.last()[2].toString()));
        QVERIFY(renameWorked);
        QVERIFY(writer.execute("ROLLBACK"));
        // 使用独立连接检查备份中已提交的数据。
        Probe snapshot(target);
        QCOMPARE(snapshot.value("SELECT sn FROM runs"), "SN-committed");
        QCOMPARE(snapshot.value("SELECT count(*) FROM results"), "1");
        QCOMPARE(snapshot.value("SELECT count(*) FROM steps"), "3");
        // 备份后继续写入，不会让已发布的快照发生变化。
        const auto later = makeRun("later", 2);
        repository.reserveRun(later);
        repository.saveResult(finish(later));
        QCOMPARE(snapshot.value("SELECT count(*) FROM runs"), "1");
        QCOMPARE(writer.value("SELECT count(*) FROM runs"), "2");
    }

    // 验证已有目标、源路径及坏数据库均不会被覆盖或发布。
    void invalidSourceAndExistingDestinationArePreserved()
    {
        // 隔离所有拒绝路径和原件摘要。
        QTemporaryDir directory;
        // 保存必须保留的源数据库路径。
        const auto source = directory.filePath("live.db");
        // 保存已有目标路径，验证拒绝覆盖原文件。
        const auto existing = directory.filePath("existing.sqlite");
        // 保存用于验证拒绝损坏源库的文件路径。
        const auto badSource = directory.filePath("broken.db");
        // 保存坏源库不能生成的目标路径。
        const auto rejected = directory.filePath("must-not-exist.sqlite");
        // 创建真实配方、运行预约和终态的测试仓储。
        Repository repository;
        repository.initialize(source);
        // 准备或读取用于核对保护规则的原始文件。
        QFile file(existing);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("original backup bytes"), qint64(21));
        file.close();
        // 保存已经存在的目标，拒绝覆盖后摘要应完全一致。
        const auto originalHash = hashFile(existing);
        // 执行独立快照、验证和恢复的待测维护对象。
        DatabaseMaintenance maintenance;
        // 记录快照发布的完成状态和失败原因。
        QSignalSpy backedUp(&maintenance, &DatabaseMaintenance::backupFinished);
        maintenance.backupDatabase(source, existing);
        QVERIFY(!backedUp.last()[1].toBool());
        QCOMPARE(hashFile(existing), originalHash);
        maintenance.backupDatabase(source, source);
        QVERIFY(!backedUp.last()[1].toBool());
        file.setFileName(badSource);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not a SQLite database");
        file.close();
        maintenance.backupDatabase(badSource, rejected);
        QVERIFY(!backedUp.last()[1].toBool());
        QVERIFY(!QFileInfo::exists(rejected));
        QVERIFY(QDir(directory.path())
                    .entryList({".workbench-snapshot-*"}, QDir::Files | QDir::Hidden)
                    .isEmpty());
    }

    // 验证结构完整但业务数据或模式错误的数据库被拒绝。
    void validationRejectsFutureSchemaAndChangedMeasurements()
    {
        // 保存可被直接注入故障的闭合数据库。
        QTemporaryDir directory;
        // 保存本用例待验证数据库的独立路径。
        const auto path = directory.filePath("check.db");
        {
            // 创建真实配方、运行预约和终态的测试仓储。
            Repository repository;
            repository.initialize(path);
            // 绑定本次验证的运行身份和原始配方。
            const auto run = makeRun("done");
            repository.reserveRun(run);
            repository.saveResult(finish(run));
        }
        // 执行独立快照、验证和恢复的待测维护对象。
        DatabaseMaintenance maintenance;
        // 记录备份验证的成功状态和失败原因。
        QSignalSpy checked(&maintenance, &DatabaseMaintenance::validationFinished);
        maintenance.validateBackup(path);
        QVERIFY2(checked.last()[1].toBool(), qPrintable(checked.last()[2].toString()));
        {
            // 以独立连接检查已保存的数据库内容。
            Probe probe(path);
            QVERIFY(probe.execute("PRAGMA user_version=99"));
        }
        maintenance.validateBackup(path);
        QVERIFY(!checked.last()[1].toBool());
        {
            // 以独立连接检查已保存的数据库内容。
            Probe probe(path);
            QVERIFY(probe.execute("PRAGMA user_version=3"));
            QVERIFY(probe.execute("UPDATE steps SET value=value+999 WHERE step_index=0"));
        }
        maintenance.validateBackup(path);
        QVERIFY(!checked.last()[1].toBool());
        QVERIFY(checked.last()[2].toString().contains(QStringLiteral("明细")));
    }

    // 验证恢复到新目录、原库保持且恢复占用仍需正常核对。
    void restoreUsesNewDirectoryAndReleasesItsLock()
    {
        // 保留源、备份、恢复目录和已有目标的测试数据。
        QTemporaryDir directory;
        // 保存必须保留的源数据库路径。
        const auto source = directory.filePath("live.db");
        // 保存用于新目录恢复的一致备份路径。
        const auto backup = directory.filePath("saved.sqlite");
        // 保存新恢复目录或记录恢复完成的回报。
        const auto restored = directory.filePath("restored");
        // 保存已有目标路径，验证拒绝覆盖原文件。
        const auto existing = directory.filePath("existing");
        {
            // 创建真实配方、运行预约和终态的测试仓储。
            Repository repository;
            repository.initialize(source);
            repository.reserveRun(makeRun("unresolved"));
        }
        // 执行独立快照、验证和恢复的待测维护对象。
        DatabaseMaintenance maintenance;
        // 记录快照发布的完成状态和失败原因。
        QSignalSpy backedUp(&maintenance, &DatabaseMaintenance::backupFinished);
        maintenance.backupDatabase(source, backup);
        QVERIFY2(backedUp.last()[1].toBool(), qPrintable(backedUp.last()[2].toString()));
        // 保存恢复前原库与备份的摘要。
        const auto sourceHash = hashFile(source), backupHash = hashFile(backup);
        // 记录恢复到新目录的完成状态和失败原因。
        QSignalSpy finished(&maintenance, &DatabaseMaintenance::restoreFinished);
        // 标记恢复回调前目录锁是否已经释放。
        bool lockReleased = false;
        connect(&maintenance, &DatabaseMaintenance::restoreFinished, &maintenance,
                [&](const QString &path, bool ok, const QString &)
                {
                    if (ok)
                    {
                        // 回报阶段已不持有恢复锁，可供工作台安全打开。
                        QLockFile lock(QDir(path).filePath(".workbench.lock"));
                        lockReleased = lock.tryLock(0);
                    }
                });
        maintenance.restoreToDirectory(backup, restored);
        QVERIFY2(finished.last()[1].toBool(), qPrintable(finished.last()[2].toString()));
        QVERIFY(lockReleased);
        QCOMPARE(hashFile(source), sourceHash);
        QCOMPARE(hashFile(backup), backupHash);
        QVERIFY(!QFileInfo::exists(QDir(restored).filePath(".workbench.lock")));
        // 重新打开恢复库并执行正常的启动核对。
        Repository reopened;
        // 记录恢复库启动后仍需核对的运行身份。
        QSignalSpy recovery(&reopened, &Repository::recoverableRuns);
        reopened.initialize(QDir(restored).filePath("data/workbench.db"));
        QCOMPARE(qvariant_cast<QJsonArray>(recovery.last()[0]).size(), 1);
        // 已有目录的内容与原数据目录均不能被恢复操作覆盖。
        QVERIFY(QDir().mkpath(existing));
        // 创建已有目录中必须保留的哨兵文件。
        QFile sentinel(QDir(existing).filePath("keep.txt"));
        QVERIFY(sentinel.open(QIODevice::WriteOnly));
        sentinel.write("keep existing directory");
        sentinel.close();
        // 保存原有哨兵文件的摘要以验证没有被覆盖。
        const auto sentinelHash = hashFile(sentinel.fileName());
        maintenance.restoreToDirectory(backup, existing);
        QVERIFY(!finished.last()[1].toBool());
        QCOMPARE(hashFile(sentinel.fileName()), sentinelHash);
        maintenance.restoreToDirectory(backup, directory.path());
        QVERIFY(!finished.last()[1].toBool());
        QCOMPARE(hashFile(source), sourceHash);
    }

    // 验证模式1和模式2备份均可检查并恢复，再由仓储正常升级。
    void legacyBackupsRemainRestorable()
    {
        // 保存降级构造的旧版本库和各自新恢复目录。
        QTemporaryDir directory;
        // 保存必须保留的源数据库路径。
        const auto source = directory.filePath("legacy.db");
        {
            // 创建真实配方、运行预约和终态的测试仓储。
            Repository repository;
            repository.initialize(source);
            // 绑定本次验证的运行身份和原始配方。
            const auto run = makeRun("old-completed");
            repository.reserveRun(run);
            repository.saveResult(finish(run));
        }
        {
            // 以独立连接检查已保存的数据库内容。
            Probe probe(source);
            QVERIFY(probe.execute("DROP TABLE manual_resolutions"));
            QVERIFY(probe.execute("DROP TABLE station_suspensions"));
            QVERIFY(probe.execute("DROP TABLE operational_events"));
            QVERIFY(probe.execute("PRAGMA user_version=2"));
        }
        // 执行独立快照、验证和恢复的待测维护对象。
        DatabaseMaintenance maintenance;
        // 记录备份验证的成功状态和失败原因。
        QSignalSpy checked(&maintenance, &DatabaseMaintenance::validationFinished);
        // 保存新恢复目录或记录恢复完成的回报。
        QSignalSpy restored(&maintenance, &DatabaseMaintenance::restoreFinished);
        maintenance.validateBackup(source);
        QVERIFY2(checked.last()[1].toBool(), qPrintable(checked.last()[2].toString()));
        maintenance.restoreToDirectory(source, directory.filePath("v2-restored"));
        QVERIFY2(restored.last()[1].toBool(), qPrintable(restored.last()[2].toString()));
        {
            // 以独立连接检查已保存的数据库内容。
            Probe probe(source);
            QVERIFY(probe.execute("DROP INDEX one_active_station"));
            QVERIFY(probe.execute("DROP INDEX one_active_sn"));
            QVERIFY(probe.execute("ALTER TABLE runs DROP COLUMN recovery_pending"));
            QVERIFY(probe.execute("PRAGMA user_version=1"));
        }
        maintenance.validateBackup(source);
        QVERIFY2(checked.last()[1].toBool(), qPrintable(checked.last()[2].toString()));
        // 从旧模式恢复后打开时保留历史并迁移到模式3。
        const auto target = directory.filePath("v1-restored");
        maintenance.restoreToDirectory(source, target);
        QVERIFY2(restored.last()[1].toBool(), qPrintable(restored.last()[2].toString()));
        // 验证旧备份恢复后能够迁移到当前模式。
        Repository upgraded;
        upgraded.initialize(QDir(target).filePath("data/workbench.db"));
        // 以独立连接检查已保存的数据库内容。
        Probe probe(QDir(target).filePath("data/workbench.db"));
        QCOMPARE(probe.value("PRAGMA user_version"), "3");
        QCOMPARE(probe.value("SELECT count(*) FROM results"), "1");
    }
};

QTEST_GUILESS_MAIN(DatabaseMaintenanceTests)
#include "test_database_maintenance.moc"
