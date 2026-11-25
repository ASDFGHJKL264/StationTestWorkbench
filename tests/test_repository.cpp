#include "repository.h"
#include "domain.h"
#include <QtTest>
#include <QSqlQuery>
#include <QSqlError>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QFile>

namespace
{
// 用 id、station、sn 和 recipe 构造完整的测试运行请求。
QJsonObject request(const QString &id = "run-1", int station = 1, const QString &sn = "SN-001",
                    QJsonObject recipe = Domain::defaultRecipe())
{
    return {{"runId", id},
            {"stationId", station},
            {"sn", sn},
            {"recipeVersion", recipe["version"]},
            {"recipe", recipe}};
}
// 根据请求 r 构造完成终态，fail 控制首个测量项是否超限。
QJsonObject terminal(const QJsonObject &r, bool fail = false)
{
    // 保存按配方顺序生成的测量步骤。
    QJsonArray values;
    int i = 0;
    for (const auto &v : r["recipe"].toObject()["steps"].toArray())
    {
        // 提供当前测量项身份、限值和标称值的配方规则。
        const auto s = v.toObject();
        values.append(QJsonObject{
            {"stepId", s["id"]},
            {"value", fail && i == 0 ? s["max"].toDouble() + 1 : s["nominal"].toDouble()}});
        ++i;
    }
    return {{"runId", r["runId"]},    {"stationId", r["stationId"]},
            {"sn", r["sn"]},          {"recipeVersion", r["recipeVersion"]},
            {"outcome", "completed"}, {"steps", values}};
}
class ExternalConnection
{
  public:
    // 为 path 打开独立连接，供测试核对数据或注入写入故障。
    explicit ExternalConnection(const QString &path)
    {
        name = "repository-test-" + QUuid::createUuid().toString();
        db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(path);
        db.open();
    }
    // 关闭并移除独立测试连接，避免影响其他用例。
    ~ExternalConnection()
    {
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }
    // 读取 table 表的总行数，失败时返回负数。
    int count(const QString &table)
    {
        // 执行当前行数统计的 SQL 查询。
        QSqlQuery q(db);
        if (!q.exec("SELECT count(*) FROM " + table) || !q.next())
            return -1;
        return q.value(0).toInt();
    }
    // 执行 sql 并读取第一行第一列的文本。
    QString text(const QString &sql)
    {
        // 读取指定标量查询结果的 SQL 查询。
        QSqlQuery q(db);
        if (!q.exec(sql) || !q.next())
            return {};
        return q.value(0).toString();
    }
    // 执行 sql 语句，返回执行是否成功。
    bool execute(const QString &sql)
    {
        // 执行本次测试 SQL 指令的查询对象。
        QSqlQuery q(db);
        return q.exec(sql);
    }
    // 用于直接检查或操作测试数据库的独立连接。
    QSqlDatabase db;

  private:
    // 确保各测试连接互不冲突的唯一连接名称。
    QString name;
};
} // namespace

class RepositoryTests final : public QObject
{
    Q_OBJECT
  private slots:
    // 验证数据库启用 WAL、记录模式版本并安装默认配方。
    void initializesWalAndDefaultRecipe();
    // 验证预约使用已发布配方，且产品与工位占用互斥。
    void reservationsRequirePublishedIdentityAndExclusiveAdmission();
    // 验证本地重新判定终态、去重重放并拒绝冲突结果。
    void finalEvaluationDeduplicatesAndRejectsConflicts();
    // 验证无效终态不会破坏原运行预约和占用。
    void invalidTerminalLeavesReservationUntouched();
    // 验证任一步骤写入失败都回滚整个结果事务。
    void stepInsertionFailureRollsBackEntireResult();
    // 验证数据库锁冲突后可重试且无部分结果残留。
    void databaseLockCanRetryWithoutPartialRows();
    // 验证重启保留待核对身份，并能按真实终态完成归档。
    void restartPreservesAndReconcilesInterruptedRun();
    // 验证远端明确无记录后，持久化中断确认才能释放占用。
    void unknownResolutionReleasesPersistedHold();
    // 验证旧模式升级保留历史和未核对运行。
    void versionOneMigratesWithoutLosingHistory();
    // 验证已发布配方不能原地修改，新版本仍可正常发布。
    void publishedRecipeVersionIsImmutable();
    // 验证每页条数有界且新记录插入不改变旧页游标。
    void keysetHistoryIsBoundedAndStable();
    // 验证本地日期边界、组合条件和完整 SN 精确匹配。
    void filteredHistoryUsesLocalDayBoundariesAndExactIdentity();
    // 验证组合筛选翻页不受新增结果或不匹配记录干扰。
    void filteredHistoryKeepsCursorAcrossNewResults();
    // 验证非法工位、判定、日期或游标会被拒绝。
    void filteredHistoryRejectsInvalidConditions();
    // 验证取消与 PASS、FAIL 分别保存为独立判定。
    void cancellationHasSeparateVerdict();
    // 验证未知的更高数据库版本被拒绝且原内容不被改写。
    void futureSchemaIsRejectedWithoutChanges();
    // 验证数据库连接的创建和使用均发生在所属工作线程。
    void connectionLivesOnWorkerThread();
    // 验证报告使用运行绑定的配方快照并转义身份文本。
    void reportUsesBoundSnapshotAndEscapesIdentity();
    // 验证报告标明未执行步骤并正确回报文件写入失败。
    void reportShowsUnexecutedStepsAndPropagatesFileFailure();
    // 验证异常 SN 从全历史去重导出并正确处理 CSV 转义。
    void failedSnExportDeduplicatesWholeHistoryAndEscapesCsv();
    // 验证空历史和导出故障不会覆盖已有目标文件。
    void failedSnExportHandlesEmptyHistoryAndErrorsWithoutOverwriting();
    // 验证人工结算保留 UNKNOWN、审计与停用，并拒绝迟到覆盖。
    void manualResolutionPreservesEvidenceAndSuspendsStation();
    // 验证审计写入失败回滚人工结算、隔离和占用释放。
    void manualResolutionAuditFailureRollsBackAllState();
    // 验证超时记录在终态和重启后仍可追溯，停用工位不能带任务启用。
    void timeoutAuditAndSafetyStateSurviveRestart();
    // 验证模式2完整升级且缺失模式3安全表时拒绝静默修复。
    void versionTwoMigrationAndMissingSafetyTableAreChecked();
};

void RepositoryTests::initializesWalAndDefaultRecipe()
{
    // 隔离保存本用例数据库和输出文件的临时目录。
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 用于验证自动创建父目录的数据库文件路径。
    const auto path = dir.filePath("nested/data.db");
    // 执行数据库初始化和持久化操作的待测仓储。
    Repository r;
    // initialized 记录初始化回报，recipes 记录配方列表回报。
    QSignalSpy initialized(&r, &Repository::initialized), recipes(&r, &Repository::recipesReady);
    r.initialize(path);
    QCOMPARE(initialized.count(), 1);
    QVERIFY(initialized.last()[0].toBool());
    r.listRecipes();
    QCOMPARE(recipes.count(), 1);
    // 仓储初始化后返回的已发布配方列表。
    const auto list = qvariant_cast<QJsonArray>(recipes.last()[0]);
    QCOMPARE(list.size(), 1);
    QCOMPARE(list[0].toObject(), Domain::defaultRecipe());
    // 用于直接核对 WAL、模式版本和记录数量的独立连接。
    ExternalConnection db(path);
    QVERIFY(db.db.isOpen());
    QCOMPARE(db.text("PRAGMA journal_mode"), "wal");
    QCOMPARE(db.text("PRAGMA user_version"), "3");
    QCOMPARE(db.count("runs"), 0);
    QCOMPARE(db.count("recipes"), 1);
}

void RepositoryTests::reservationsRequirePublishedIdentityAndExclusiveAdmission()
{
    // 隔离保存本用例数据库和输出文件的临时目录。
    QTemporaryDir dir;
    // 本用例独占的 SQLite 文件路径。
    const auto path = dir.filePath("test.db");
    // 执行数据库预约和终态归档的待测仓储。
    Repository r;
    r.initialize(path);
    // reserved 记录占用申请回报，saved 记录终态归档回报。
    QSignalSpy reserved(&r, &Repository::runReserved), saved(&r, &Repository::resultSaved);
    // 用于验证重入和重复身份拒绝的首个合法运行请求。
    const auto first = request();
    r.reserveRun(first);
    QVERIFY(reserved.last()[1].toBool());
    r.reserveRun(request("same-station", 1, "SN-002"));
    QVERIFY(!reserved.last()[1].toBool());
    r.reserveRun(request("same-sn", 2, "SN-001"));
    QVERIFY(!reserved.last()[1].toBool());
    r.reserveRun(first);
    QVERIFY(!reserved.last()[1].toBool());
    // 修改名称或版本以构造未发布配方的副本。
    auto changed = Domain::defaultRecipe();
    changed["name"] = "changed";
    r.reserveRun(request("tampered", 2, "SN-002", changed));
    QVERIFY(!reserved.last()[1].toBool());
    changed["version"] = "unpublished";
    r.reserveRun(request("unpublished", 2, "SN-002", changed));
    QVERIFY(!reserved.last()[1].toBool());
    r.saveResult(terminal(request("unknown", 2, "SN-002")));
    QVERIFY(!saved.last()[1].toBool());
    // 用于直接核对预约和结果数量的独立连接。
    ExternalConnection db(path);
    QCOMPARE(db.count("runs"), 1);
    QCOMPARE(db.count("results"), 0);
    r.reserveRun(request("other", 2, "SN-002"));
    QVERIFY(reserved.last()[1].toBool());
    QCOMPARE(db.count("runs"), 2);
}

void RepositoryTests::finalEvaluationDeduplicatesAndRejectsConflicts()
{
    // 隔离保存本用例数据库和输出文件的临时目录。
    QTemporaryDir dir;
    // 本用例独占的 SQLite 文件路径。
    const auto path = dir.filePath("test.db");
    // 执行终态重新判定和幂等保存的待测仓储。
    Repository r;
    r.initialize(path);
    // 记录终态归档是否成功、是否重复及失败原因。
    QSignalSpy saved(&r, &Repository::resultSaved);
    // 本次终态所绑定的原始测试请求。
    const auto run = request();
    r.reserveRun(run);
    // 首个测量项超限且可伪造远端判定的原始终态。
    auto result = terminal(run, true);
    result["verdict"] = "PASS";
    r.saveResult(result);
    QVERIFY(saved.last()[1].toBool());
    QVERIFY(!saved.last()[2].toBool());
    // 仓储重新判定并补充步骤后的归档回报对象。
    auto output = qvariant_cast<QJsonObject>(saved.last()[0]);
    QCOMPARE(output["verdict"].toString(), "FAIL");
    QCOMPARE(output["evaluatedSteps"].toArray().size(), 3);
    // 用于直接核对事务记录和最终判定的独立连接。
    ExternalConnection db(path);
    QCOMPARE(db.count("results"), 1);
    QCOMPARE(db.count("steps"), 3);
    QCOMPARE(db.text("SELECT execution FROM runs"), "COMPLETED");
    QCOMPARE(db.text("SELECT verdict FROM runs"), "FAIL");
    // Result signal enrichment must not affect wire-terminal idempotency.
    for (int i = 0; i < 30; ++i)
    {
        r.saveResult(i % 2 ? result : output);
        QVERIFY(saved.last()[1].toBool());
        QVERIFY(saved.last()[2].toBool());
    }
    QCOMPARE(db.count("results"), 1);
    QCOMPARE(db.count("steps"), 3);
    r.saveResult(terminal(run));
    QVERIFY(!saved.last()[1].toBool());
    QCOMPARE(db.text("SELECT verdict FROM runs"), "FAIL");
    // A retest of the same SN is a separate run, retaining the original result.
    // 记录同一产品完成后重新预约的处理结果。
    QSignalSpy reserved(&r, &Repository::runReserved);
    r.reserveRun(request("retest", 1, "SN-001"));
    QVERIFY(reserved.last()[1].toBool());
    QCOMPARE(db.count("runs"), 2);
}

void RepositoryTests::invalidTerminalLeavesReservationUntouched()
{
    // 隔离保存本用例数据库和输出文件的临时目录。
    QTemporaryDir dir;
    // 本用例独占的 SQLite 文件路径。
    const auto path = dir.filePath("test.db");
    // 验证异常终态身份和步骤的待测仓储。
    Repository r;
    r.initialize(path);
    // 必须保持占用不变的原始测试请求。
    const auto run = request();
    r.reserveRun(run);
    // 记录每个异常终态的归档拒绝回报。
    QSignalSpy saved(&r, &Repository::resultSaved);
    // 用于派生各种异常情况的合法终态。
    const auto good = terminal(run);
    // 保存需要逐个验证的身份和步骤异常终态。
    QList<QJsonObject> cases;
    // 当前用于修改身份、步骤或执行结果的终态副本。
    auto bad = good;
    bad["sn"] = "OTHER";
    cases.append(bad);
    bad = good;
    bad["stationId"] = 2;
    cases.append(bad);
    bad = good;
    bad["recipeVersion"] = "wrong";
    cases.append(bad);
    bad = good;
    // 用于构造缺项、乱序及无效测量值的步骤数组。
    auto steps = good["steps"].toArray();
    steps.removeLast();
    bad["steps"] = steps;
    cases.append(bad);
    bad = good;
    steps = good["steps"].toArray();
    steps[0] = steps[1];
    bad["steps"] = steps;
    cases.append(bad);
    bad = good;
    steps = good["steps"].toArray();
    // 用于注入空测量值的单个步骤对象。
    auto step = steps[0].toObject();
    step["value"] = QJsonValue::Null;
    steps[0] = step;
    bad["steps"] = steps;
    cases.append(bad);
    bad = good;
    bad["outcome"] = "device-says-pass";
    cases.append(bad);
    // 用于核对拒绝终态后数据库仍无结果的独立连接。
    ExternalConnection db(path);
    for (const auto &item : cases)
    {
        r.saveResult(item);
        QVERIFY(!saved.last()[1].toBool());
        QVERIFY(!saved.last()[3].toString().isEmpty());
        QCOMPARE(db.count("results"), 0);
        QCOMPARE(db.count("steps"), 0);
        QCOMPARE(db.text("SELECT execution FROM runs"), "RESERVED");
    }
    r.saveResult(good);
    QVERIFY(saved.last()[1].toBool());
}

void RepositoryTests::stepInsertionFailureRollsBackEntireResult()
{
    // 隔离保存本用例数据库和输出文件的临时目录。
    QTemporaryDir dir;
    // 本用例独占的 SQLite 文件路径。
    const auto path = dir.filePath("test.db");
    // 验证步骤失败事务回滚的待测仓储。
    Repository r;
    r.initialize(path);
    // 用于预约和生成终态的原始测试请求。
    const auto run = request();
    r.reserveRun(run);
    // 用于安装失败触发器及核对回滚结果的独立连接。
    ExternalConnection db(path);
    QVERIFY(
        db.execute("CREATE TRIGGER fail_second_step BEFORE INSERT ON steps WHEN NEW.step_index=1 "
                   "BEGIN SELECT RAISE(ABORT,'injected disk/write failure'); END"));
    // 记录步骤写入失败与重试成功的归档回报。
    QSignalSpy saved(&r, &Repository::resultSaved);
    r.saveResult(terminal(run));
    QVERIFY(!saved.last()[1].toBool());
    QCOMPARE(db.count("results"), 0);
    QCOMPARE(db.count("steps"), 0);
    QCOMPARE(db.text("SELECT execution FROM runs"), "RESERVED");
    QVERIFY(db.execute("DROP TRIGGER fail_second_step"));
    r.saveResult(terminal(run));
    QVERIFY(saved.last()[1].toBool());
    QCOMPARE(db.count("results"), 1);
    QCOMPARE(db.count("steps"), 3);
    QCOMPARE(db.text("SELECT execution FROM runs"), "COMPLETED");
}

void RepositoryTests::databaseLockCanRetryWithoutPartialRows()
{
    // 隔离保存本用例数据库和输出文件的临时目录。
    QTemporaryDir dir;
    // 本用例独占的 SQLite 文件路径。
    const auto path = dir.filePath("test.db");
    // 验证数据库锁冲突及重试的待测仓储。
    Repository r;
    r.initialize(path);
    // 写锁测试中等待归档的原始运行请求。
    const auto run = request();
    r.reserveRun(run);
    // 用于持有写锁并检查是否有部分结果的独立连接。
    ExternalConnection db(path);
    QVERIFY(db.execute("BEGIN IMMEDIATE"));
    // 记录写锁期间失败和解锁后重试的归档回报。
    QSignalSpy saved(&r, &Repository::resultSaved);
    // 测量锁冲突返回时间，验证等待时间有界。
    QElapsedTimer elapsed;
    elapsed.start();
    r.saveResult(terminal(run));
    QVERIFY(!saved.last()[1].toBool());
    QVERIFY(elapsed.elapsed() < 2000);
    QCOMPARE(db.count("results"), 0);
    QCOMPARE(db.count("steps"), 0);
    QVERIFY(db.execute("ROLLBACK"));
    r.saveResult(terminal(run));
    QVERIFY(saved.last()[1].toBool());
    QCOMPARE(db.count("results"), 1);
}

void RepositoryTests::restartPreservesAndReconcilesInterruptedRun()
{
    // 供仓储重启前后共用的临时数据目录。
    QTemporaryDir dir;
    // 仓储重启前后共同使用的 SQLite 文件路径。
    const auto path = dir.filePath("test.db");
    // 重启前已经完成归档的运行请求。
    const auto completed = request("finished", 1, "DONE");
    // 重启后仍需核对并保持占用的运行请求。
    const auto active = request("interrupted", 2, "RECOVER");
    {
        // 模拟重启前写入完成结果和活动预约的仓储。
        Repository r;
        r.initialize(path);
        r.reserveRun(completed);
        r.saveResult(terminal(completed));
        r.reserveRun(active);
        r.shutdown();
    }
    // 使用原数据库重新初始化的恢复仓储。
    Repository r;
    // 记录重启后需要重新核对的运行身份快照。
    QSignalSpy recovery(&r, &Repository::recoverableRuns);
    r.initialize(path);
    // 用于核对中断标记、占用和历史结果的独立连接。
    ExternalConnection db(path);
    QCOMPARE(recovery.count(), 1);
    // 仓储恢复时返回的待核对运行请求列表。
    const auto snapshots = qvariant_cast<QJsonArray>(recovery.last()[0]);
    QCOMPARE(snapshots.size(), 1);
    QCOMPARE(snapshots[0].toObject(), active);
    QCOMPARE(db.text("SELECT execution FROM runs WHERE run_id='interrupted'"), "INTERRUPTED");
    QCOMPARE(db.text("SELECT verdict FROM runs WHERE run_id='interrupted'"), "UNKNOWN");
    QCOMPARE(db.text("SELECT verdict FROM runs WHERE run_id='finished'"), "PASS");
    QCOMPARE(db.text("SELECT recovery_pending FROM runs WHERE run_id='interrupted'"), "1");
    // 记录恢复占用期间重复产品或工位的预约拒绝。
    QSignalSpy reserved(&r, &Repository::runReserved);
    r.reserveRun(request("same-product", 3, "RECOVER"));
    QVERIFY(!reserved.last()[1].toBool());
    r.reserveRun(request("same-station", 2, "DIFFERENT"));
    QVERIFY(!reserved.last()[1].toBool());
    // 记录真实终态对原中断记录的核对归档结果。
    QSignalSpy saved(&r, &Repository::resultSaved);
    r.saveResult(terminal(active));
    QVERIFY(saved.last()[1].toBool());
    QCOMPARE(qvariant_cast<QJsonObject>(saved.last()[0])["verdict"].toString(), "PASS");
    QCOMPARE(db.text("SELECT execution FROM runs WHERE run_id='interrupted'"), "COMPLETED");
    QCOMPARE(db.count("results"), 2);
    QCOMPARE(db.text("SELECT recovery_pending FROM runs WHERE run_id='interrupted'"), "0");
    r.reserveRun(request("retest-after-recovery", 3, "RECOVER"));
    QVERIFY(reserved.last()[1].toBool());
}

void RepositoryTests::unknownResolutionReleasesPersistedHold()
{
    // 供反复重启共用的临时数据目录。
    QTemporaryDir dir;
    // 各次重启共同使用的 SQLite 文件路径。
    const auto path = dir.filePath("test.db");
    // 必须跨重启保留占用的原始活动请求。
    const auto active = request();
    {
        // 首次创建并保存活动预约的仓储。
        Repository r;
        r.initialize(path);
        r.reserveRun(active);
    }
    // Repeated restarts must not silently release an unresolved SN.
    {
        // 验证重复重启仍保留占用的恢复仓储。
        Repository r;
        // 记录本次恢复时尚未结算的运行身份。
        QSignalSpy recovery(&r, &Repository::recoverableRuns);
        r.initialize(path);
        QCOMPARE(qvariant_cast<QJsonArray>(recovery.last()[0]).size(), 1);
    }
    // 负责验证中断确认成功后释放占用的仓储。
    Repository r;
    // recovery 记录恢复身份，interrupted 记录中断提交，reserved 记录新预约回报。
    QSignalSpy recovery(&r, &Repository::recoverableRuns),
        interrupted(&r, &Repository::runInterrupted), reserved(&r, &Repository::runReserved);
    r.initialize(path);
    QCOMPARE(qvariant_cast<QJsonArray>(recovery.last()[0]).size(), 1);
    // 用于阻塞中断事务并核对占用标记的独立连接。
    ExternalConnection db(path);
    QVERIFY(db.execute("BEGIN IMMEDIATE"));
    r.markInterrupted(active["runId"].toString(), "station reports unknown");
    QVERIFY(!interrupted.last()[1].toBool());
    QCOMPARE(db.text("SELECT recovery_pending FROM runs"), "1");
    QVERIFY(db.execute("ROLLBACK"));
    r.markInterrupted(active["runId"].toString(), "station reports unknown");
    QVERIFY(interrupted.last()[1].toBool());
    QCOMPARE(db.text("SELECT recovery_pending FROM runs"), "0");
    r.reserveRun(request("new-product-run", 2, "SN-001"));
    QVERIFY(reserved.last()[1].toBool());
}

void RepositoryTests::versionOneMigratesWithoutLosingHistory()
{
    // 隔离保存模式升级测试数据库的临时目录。
    QTemporaryDir dir;
    // 需要从 v1 升级的 SQLite 文件路径。
    const auto path = dir.filePath("v1.db");
    // 模式升级后仍必须保留占用的运行请求。
    const auto active = request("active", 2, "ACTIVE");
    {
        // 准备旧版数据库内容的仓储。
        Repository r;
        r.initialize(path);
        // 用于校验升级后历史仍完整的已完成请求。
        const auto done = request("done", 1, "DONE");
        r.reserveRun(done);
        r.saveResult(terminal(done));
        r.reserveRun(active);
    }
    {
        // Recreate the previous version's shape while preserving real history.
        // 用于改造旧模式结构的独立数据库连接。
        ExternalConnection db(path);
        QVERIFY(db.execute("DROP INDEX one_active_station"));
        QVERIFY(db.execute("DROP INDEX one_active_sn"));
        QVERIFY(db.execute("ALTER TABLE runs DROP COLUMN recovery_pending"));
        QVERIFY(db.execute("PRAGMA user_version=1"));
    }
    // 验证原地模式升级和恢复行为的仓储。
    Repository r;
    // initialized 记录升级初始化，recovery 记录恢复身份，reserved 记录占用回报。
    QSignalSpy initialized(&r, &Repository::initialized),
        recovery(&r, &Repository::recoverableRuns), reserved(&r, &Repository::runReserved);
    r.initialize(path);
    QVERIFY(initialized.last()[0].toBool());
    // 用于核对升级版本和保留记录的独立连接。
    ExternalConnection db(path);
    QCOMPARE(db.text("PRAGMA user_version"), "3");
    QCOMPARE(db.count("runs"), 2);
    QCOMPARE(db.count("results"), 1);
    QCOMPARE(db.count("steps"), 3);
    QCOMPARE(qvariant_cast<QJsonArray>(recovery.last()[0]).size(), 1);
    QCOMPARE(db.text("SELECT recovery_pending FROM runs WHERE run_id='active'"), "1");
    r.reserveRun(request("blocked", 3, "ACTIVE"));
    QVERIFY(!reserved.last()[1].toBool());
}

void RepositoryTests::publishedRecipeVersionIsImmutable()
{
    // 隔离保存配方版本测试数据的临时目录。
    QTemporaryDir dir;
    // 本用例独占的 SQLite 文件路径。
    const auto path = dir.filePath("test.db");
    // 验证发布配方不可变规则的待测仓储。
    Repository r;
    r.initialize(path);
    // 记录同版本冲突或无效配方引起的操作错误。
    QSignalSpy error(&r, &Repository::operationError);
    // 已发布且不允许原地修改的原始配方。
    const auto original = Domain::defaultRecipe();
    r.publishRecipe(original);
    QCOMPARE(error.count(), 0);
    // 用于测试内容冲突及新版本发布的可编辑配方副本。
    auto edited = original;
    edited["name"] = "silent-change";
    r.publishRecipe(edited);
    QCOMPARE(error.count(), 1);
    // 直接核对配方数量和原始哈希的独立连接。
    ExternalConnection db(path);
    QCOMPARE(db.count("recipes"), 1);
    QCOMPARE(db.text("SELECT sha256 FROM recipes"), Domain::recipeHash(original));
    edited["version"] = "demo-5v-v2";
    r.publishRecipe(edited);
    QCOMPARE(error.count(), 1);
    QCOMPARE(db.count("recipes"), 2);
    // 记录使用新发布版本进行预约的处理结果。
    QSignalSpy reserved(&r, &Repository::runReserved);
    r.reserveRun(request("v2", 1, "V2-SN", edited));
    QVERIFY(reserved.last()[1].toBool());
    edited["version"] = "bad-v3";
    edited["steps"] = QJsonArray{};
    r.publishRecipe(edited);
    QCOMPARE(error.count(), 2);
    QCOMPARE(db.count("recipes"), 2);
}

void RepositoryTests::keysetHistoryIsBoundedAndStable()
{
    // 隔离保存分页测试数据库的临时目录。
    QTemporaryDir dir;
    // 本用例独占的 SQLite 文件路径。
    const auto path = dir.filePath("test.db");
    // 执行真实游标分页查询的待测仓储。
    Repository r;
    r.initialize(path);
    // reserved、saved 记录数据准备结果，history 记录分页查询回报。
    QSignalSpy reserved(&r, &Repository::runReserved), saved(&r, &Repository::resultSaved),
        history(&r, &Repository::historyReady);
    for (int i = 1; i <= 205; ++i)
    {
        // 准备分页历史时需要预约和归档的完整运行请求。
        const auto run = request(QString("run-%1").arg(i), i % 4 + 1, i % 2 ? "ODD" : "EVEN");
        r.reserveRun(run);
        QVERIFY(reserved.last()[1].toBool());
        r.saveResult(terminal(run));
        QVERIFY(saved.last()[1].toBool());
    }
    r.queryHistory({}, 0);
    // 当前历史查询返回的有序分页记录。
    auto rows = qvariant_cast<QJsonArray>(history.last()[0]);
    QCOMPARE(rows.size(), 100);
    QVERIFY(history.last()[1].toBool());
    QCOMPARE(rows.first().toObject()["id"].toInt(), 205);
    // 当前页最早的记录编号，用作下一页上界。
    const int cursor = rows.last().toObject()["id"].toInt();
    // An inserted latest run cannot shift the second page or create duplicates.
    // 分页期间新插入的最新运行请求。
    const auto newest = request("new", 1, "NEW");
    r.reserveRun(newest);
    r.saveResult(terminal(newest));
    r.queryHistory({}, cursor);
    rows = qvariant_cast<QJsonArray>(history.last()[0]);
    QCOMPARE(rows.size(), 100);
    QCOMPARE(rows.first().toObject()["id"].toInt(), 105);
    QCOMPARE(rows.last().toObject()["id"].toInt(), 6);
    QVERIFY(history.last()[1].toBool());
    r.queryHistory({}, 6);
    rows = qvariant_cast<QJsonArray>(history.last()[0]);
    QCOMPARE(rows.size(), 5);
    QVERIFY(!history.last()[1].toBool());
    r.queryHistory("EVEN", 0);
    rows = qvariant_cast<QJsonArray>(history.last()[0]);
    QCOMPARE(rows.size(), 100);
    QVERIFY(history.last()[1].toBool());
    for (const auto &v : rows)
    {
        // 用于核对 SN 精确匹配及步骤数量的历史记录。
        const auto row = v.toObject();
        QCOMPARE(row["sn"].toString(), "EVEN");
        QCOMPARE(row["steps"].toArray().size(), 3);
    }
    // 用于核对数据准备总量的独立连接。
    ExternalConnection db(path);
    QCOMPARE(db.count("runs"), 206);
    QCOMPARE(db.count("results"), 206);
    QCOMPARE(db.count("steps"), 618);
}

void RepositoryTests::filteredHistoryUsesLocalDayBoundariesAndExactIdentity()
{
    // 隔离保存日期及组合筛选测试数据的临时目录。
    QTemporaryDir dir;
    // 保存组合筛选测试历史的 SQLite 文件路径。
    const auto path = dir.filePath("filtered.db");
    // 执行真实日期和身份筛选的待测仓储。
    Repository repository;
    repository.initialize(path);
    // 用于设置开始时间等边界数据的独立连接。
    ExternalConnection db(path);
    // reserved、saved 记录准备结果，history 记录组合筛选回报。
    QSignalSpy reserved(&repository, &Repository::runReserved),
        saved(&repository, &Repository::resultSaved),
        history(&repository, &Repository::historyReady);
    // 用于构造本地起止边界的目标日历日期。
    const QDate day(2026, 2, 14);
    // 所选日期的本地零点，作为包含的开始边界。
    const auto midnight = day.startOfDay();
    // 次日本地零点，作为排除的结束边界。
    const auto nextMidnight = day.addDays(1).startOfDay();
    // 包含 SQL 特殊字符、必须按完整文本匹配的产品 SN。
    const QString exactSn = "SN_%'001";
    // 按 id、time、station、sn 和 verdict 准备指定边界历史。
    const auto add = [&](const QString &id, const QDateTime &time, int station, const QString &sn,
                         const QString &verdict)
    {
        // 准备写入指定身份及工位的运行请求。
        const auto run = request(id, station, sn);
        repository.reserveRun(run);
        QVERIFY(reserved.last()[1].toBool());
        if (verdict == "UNKNOWN")
            repository.markInterrupted(id, "station could not identify the run");
        else
        {
            repository.saveResult(terminal(run, verdict == "FAIL"));
            QVERIFY(saved.last()[1].toBool());
        }
        // 将运行开始时间精确设置到边界时刻的更新查询。
        QSqlQuery update(db.db);
        update.prepare("UPDATE runs SET started_at=? WHERE run_id=?");
        update.addBindValue(time.toUTC().toString(Qt::ISODateWithMs));
        update.addBindValue(id);
        QVERIFY(update.exec());
    };
    add("before", midnight.addMSecs(-1), 2, exactSn, "FAIL");
    add("start", midnight, 2, exactSn, "FAIL");
    add("last-ms", nextMidnight.addMSecs(-1), 2, exactSn, "FAIL");
    add("next-day", nextMidnight, 2, exactSn, "FAIL");
    add("other-station", midnight, 3, exactSn, "FAIL");
    add("other-sn", midnight, 2, exactSn + "-suffix", "FAIL");
    add("pass", midnight, 2, exactSn, "PASS");
    add("unknown", midnight, 2, exactSn, "UNKNOWN");

    // 组合 SN、工位、质量判定及可选日期的查询条件。
    HistoryFilter filter;
    filter.sn = exactSn;
    filter.stationId = 2;
    filter.verdict = "FAIL";
    filter.startDate = filter.endDate = day;
    repository.queryFilteredHistory(filter);
    // 当前组合筛选返回的完整历史记录列表。
    auto rows = qvariant_cast<QJsonArray>(history.last()[0]);
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].toObject()["runId"].toString(), "last-ms");
    QCOMPARE(rows[1].toObject()["runId"].toString(), "start");
    QVERIFY(!history.last()[1].toBool());

    filter.startDate = {};
    repository.queryFilteredHistory(filter);
    QCOMPARE(qvariant_cast<QJsonArray>(history.last()[0]).size(), 3);
    filter.startDate = day;
    filter.endDate = {};
    repository.queryFilteredHistory(filter);
    QCOMPARE(qvariant_cast<QJsonArray>(history.last()[0]).size(), 3);
    filter.endDate = day;
    filter.verdict = "UNKNOWN";
    repository.queryFilteredHistory(filter);
    rows = qvariant_cast<QJsonArray>(history.last()[0]);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows[0].toObject()["runId"].toString(), "unknown");
    filter.verdict = "PASS";
    repository.queryFilteredHistory(filter);
    rows = qvariant_cast<QJsonArray>(history.last()[0]);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows[0].toObject()["runId"].toString(), "pass");
    repository.queryFilteredHistory({});
    QCOMPARE(qvariant_cast<QJsonArray>(history.last()[0]).size(), 8);
    repository.queryHistory(exactSn);
    QCOMPARE(qvariant_cast<QJsonArray>(history.last()[0]).size(), 7);
}

void RepositoryTests::filteredHistoryKeepsCursorAcrossNewResults()
{
    // 隔离保存组合分页测试数据的临时目录。
    QTemporaryDir dir;
    // 验证先筛选后分页规则的待测仓储。
    Repository repository;
    repository.initialize(dir.filePath("filtered.db"));
    // reserved、saved 记录数据准备结果，history 记录分页回报。
    QSignalSpy reserved(&repository, &Repository::runReserved),
        saved(&repository, &Repository::resultSaved),
        history(&repository, &Repository::historyReady);
    // 按 id 和 station 保存同一产品的 FAIL 历史供分页验证。
    const auto add = [&](const QString &id, int station)
    {
        // 当前准备归档到指定工位的运行请求。
        const auto run = request(id, station, "SAME-SN");
        repository.reserveRun(run);
        QVERIFY(reserved.last()[1].toBool());
        repository.saveResult(terminal(run, true));
        QVERIFY(saved.last()[1].toBool());
    };
    for (int i = 1; i <= 103; ++i)
        add(QString("match-%1").arg(i), 2);
    // More than one page of newer, nonmatching rows must not hide matching history.
    for (int i = 0; i < 105; ++i)
        add(QString("other-%1").arg(i), 3);
    // 限定同一产品、工位2及 FAIL 的组合查询条件。
    HistoryFilter filter;
    filter.sn = "SAME-SN";
    filter.stationId = 2;
    filter.verdict = "FAIL";
    repository.queryFilteredHistory(filter);
    // 当前匹配组合条件的分页记录。
    auto rows = qvariant_cast<QJsonArray>(history.last()[0]);
    QCOMPARE(rows.size(), 100);
    QCOMPARE(rows[0].toObject()["runId"].toString(), "match-103");
    QVERIFY(history.last()[1].toBool());
    // 第一页最早的记录编号，用于验证新增结果不扰动旧页。
    const auto cursor = rows.last().toObject()["id"].toInt();
    QCOMPARE(cursor, 4);
    add("new-matching-result", 2);
    repository.queryFilteredHistory(filter, cursor);
    rows = qvariant_cast<QJsonArray>(history.last()[0]);
    QCOMPARE(rows.size(), 3);
    QVERIFY(!history.last()[1].toBool());
    for (int i = 0; i < rows.size(); ++i)
        QCOMPARE(rows[i].toObject()["runId"].toString(), QString("match-%1").arg(3 - i));
}

void RepositoryTests::filteredHistoryRejectsInvalidConditions()
{
    // 隔离保存无效筛选参数测试数据的临时目录。
    QTemporaryDir dir;
    // 验证筛选参数边界和合法性的待测仓储。
    Repository repository;
    repository.initialize(dir.filePath("filtered.db"));
    // errors 记录查询拒绝原因，history 记录有效查询结果。
    QSignalSpy errors(&repository, &Repository::operationError),
        history(&repository, &Repository::historyReady);
    // 逐项注入非法工位、判定、日期、SN 和游标的查询条件。
    HistoryFilter filter;
    filter.stationId = 5;
    repository.queryFilteredHistory(filter);
    filter.stationId = -1;
    repository.queryFilteredHistory(filter);
    filter.stationId = 0;
    filter.verdict = "FAIL' OR 1=1 --";
    repository.queryFilteredHistory(filter);
    filter.verdict.clear();
    filter.startDate = QDate(2026, 2, 15);
    filter.endDate = QDate(2026, 2, 14);
    repository.queryFilteredHistory(filter);
    filter = {};
    filter.sn = QString(65, 'A');
    repository.queryFilteredHistory(filter);
    repository.queryFilteredHistory({}, -1);
    QCOMPARE(errors.size(), 6);
    QCOMPARE(history.size(), 0);
    repository.queryHistory({});
    QCOMPARE(history.size(), 1);
}

void RepositoryTests::cancellationHasSeparateVerdict()
{
    // 隔离保存取消归档测试数据的临时目录。
    QTemporaryDir dir;
    // 本用例独占的 SQLite 文件路径。
    const auto path = dir.filePath("test.db");
    // 验证取消执行与质量判定分离的待测仓储。
    Repository r;
    r.initialize(path);
    // 本次取消终态对应的原始预约请求。
    const auto run = request();
    r.reserveRun(run);
    // 用于改造为仅执行部分步骤的取消终态。
    auto result = terminal(run);
    result["outcome"] = "cancelled";
    // 取消前实际完成的步骤前缀。
    auto steps = result["steps"].toArray();
    while (steps.size() > 1)
        steps.removeLast();
    result["steps"] = steps;
    // 记录取消终态保存后的执行结果和质量判定。
    QSignalSpy saved(&r, &Repository::resultSaved);
    r.saveResult(result);
    QVERIFY(saved.last()[1].toBool());
    QCOMPARE(qvariant_cast<QJsonObject>(saved.last()[0])["verdict"].toString(), "CANCELLED");
    // 用于直接核对取消记录及步骤数量的独立连接。
    ExternalConnection db(path);
    QCOMPARE(db.text("SELECT execution FROM runs"), "CANCELLED");
    QCOMPARE(db.count("steps"), 1);
}

void RepositoryTests::futureSchemaIsRejectedWithoutChanges()
{
    // 隔离保存未知模式测试数据的临时目录。
    QTemporaryDir dir;
    // 包含不支持的高版本模式的 SQLite 文件路径。
    const auto path = dir.filePath("future.db");
    {
        // 用于准备哨兵内容和高版本号的独立连接。
        ExternalConnection db(path);
        QVERIFY(db.execute("PRAGMA user_version=99"));
        QVERIFY(db.execute("CREATE TABLE sentinel(value TEXT)"));
        QVERIFY(db.execute("INSERT INTO sentinel VALUES('keep')"));
    }
    // 验证高版本数据库拒绝打开规则的待测仓储。
    Repository r;
    // 记录高版本数据库初始化被拒绝的回报。
    QSignalSpy initialized(&r, &Repository::initialized);
    r.initialize(path);
    QVERIFY(!initialized.last()[0].toBool());
    // 检查拒绝初始化后原数据未改写的独立连接。
    ExternalConnection db(path);
    QCOMPARE(db.text("PRAGMA user_version"), "99");
    QCOMPARE(db.text("PRAGMA journal_mode"), "delete");
    QCOMPARE(db.text("SELECT value FROM sentinel"), "keep");
}

void RepositoryTests::connectionLivesOnWorkerThread()
{
    // 隔离保存跨线程连接测试数据的临时目录。
    QTemporaryDir dir;
    // 由数据库工作线程打开的 SQLite 文件路径。
    const auto path = dir.filePath("worker.db");
    // 承载仓储连接和写入操作的工作线程。
    QThread thread;
    // 迁移到工作线程后执行数据库操作的仓储对象。
    auto *r = new Repository;
    r->moveToThread(&thread);
    connect(&thread, &QThread::finished, r, &QObject::deleteLater);
    // initialized 记录线程内初始化，saved 记录线程内归档回报。
    QSignalSpy initialized(r, &Repository::initialized), saved(r, &Repository::resultSaved);
    thread.start();
    // 将 path 数据库的初始化派发到仓储所属线程。
    QMetaObject::invokeMethod(r, [r, path] { r->initialize(path); });
    // Always stop the worker before checking assertions so failures cannot leave a running QThread.
    // 记录工作线程是否在限定时间内完成初始化。
    const bool initializedInTime = initialized.wait(3000);
    if (initializedInTime && initialized.last()[0].toBool())
    {
        // 派发给数据库线程预约和归档的运行请求。
        const auto run = request();
        QMetaObject::invokeMethod(r,
                                  // 在仓储所属线程先预约 run 再保存对应终态。
                                  [r, run]
                                  {
                                      r->reserveRun(run);
                                      r->saveResult(terminal(run));
                                  });
        saved.wait(3000);
    }
    QMetaObject::invokeMethod(r,
                              // 在工作线程关闭数据库并退出线程事件循环。
                              [r]
                              {
                                  r->shutdown();
                                  QThread::currentThread()->quit();
                              });
    thread.wait();
    QVERIFY(initializedInTime);
    QVERIFY(initialized.last()[0].toBool());
    QCOMPARE(saved.count(), 1);
    QVERIFY(saved.last()[1].toBool());
}

void RepositoryTests::reportUsesBoundSnapshotAndEscapesIdentity()
{
    // 隔离保存报告及配方快照测试数据的临时目录。
    QTemporaryDir dir;
    // 验证绑定快照和报告转义的待测仓储。
    Repository r;
    r.initialize(dir.filePath("data.db"));
    // SN 含 HTML 特殊字符且绑定原配方的运行请求。
    const auto run = request("report-run", 1, "<script>&SN");
    r.reserveRun(run);
    r.saveResult(terminal(run));
    // 运行归档后新发布的配方版本，用于验证历史快照稳定。
    auto newer = Domain::defaultRecipe();
    newer["version"] = "report-v2";
    // 用于修改新配方限值的规则数组。
    auto rules = newer["steps"].toArray();
    // 用于抬高新配方限值的电压步骤规则。
    auto voltage = rules[0].toObject();
    voltage["min"] = 4.9;
    rules[0] = voltage;
    newer["steps"] = rules;
    r.publishRecipe(newer);
    // 记录报告导出的路径、结果和失败原因。
    QSignalSpy exported(&r, &Repository::reportReady);
    // 导出包含 HTML 转义测试内容的报告文件路径。
    const auto path = dir.filePath("report.html");
    r.exportReport("report-run", path);
    QCOMPARE(exported.size(), 1);
    QVERIFY(exported.last()[1].toBool());
    // 用于读取并检查导出报告内容的文件。
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    // 待核对配方快照、限值和转义结果的报告文本。
    const auto html = QString::fromUtf8(file.readAll());
    QVERIFY(html.contains("&lt;script&gt;&amp;SN"));
    QVERIFY(!html.contains("<script>"));
    QVERIFY(html.contains("demo-5v-v1"));
    QVERIFY(!html.contains("report-v2"));
    QVERIFY(html.contains(Domain::recipeHash(Domain::defaultRecipe())));
    QVERIFY(html.contains("4.75"));
    QVERIFY(html.contains("PASS"));
    QVERIFY(html.contains(QStringLiteral("非真实电气测量")));
}

void RepositoryTests::reportShowsUnexecutedStepsAndPropagatesFileFailure()
{
    // 隔离保存部分执行报告测试数据的临时目录。
    QTemporaryDir dir;
    // 验证未执行步骤标识及导出错误回报的待测仓储。
    Repository r;
    r.initialize(dir.filePath("data.db"));
    // 与部分执行终态绑定的原始运行请求。
    const auto run = request();
    r.reserveRun(run);
    // 用于构造取消且只完成前缀步骤的终态。
    auto result = terminal(run);
    result["outcome"] = "cancelled";
    // 取消前实际完成的测量步骤数组。
    auto values = result["steps"].toArray();
    while (values.size() > 1)
        values.removeLast();
    result["steps"] = values;
    r.saveResult(result);
    // 记录报告导出成功及目标文件故障的回报。
    QSignalSpy exported(&r, &Repository::reportReady);
    // 保存部分执行和未执行步骤说明的报告路径。
    const auto path = dir.filePath("cancelled.html");
    r.exportReport("run-1", path);
    QVERIFY(exported.last()[1].toBool());
    // 读取部分执行报告内容的文件对象。
    QFile report(path);
    QVERIFY(report.open(QIODevice::ReadOnly));
    // 用于核对取消和未执行说明的报告文本。
    const auto html = QString::fromUtf8(report.readAll());
    QVERIFY(html.contains("CANCELLED"));
    QCOMPARE(html.count(QStringLiteral("未执行/未归档")), 2);
    r.exportReport("not-existing", dir.filePath("missing.html"));
    QVERIFY(!exported.last()[1].toBool());
    // 占据目标父目录位置以触发文件写入失败的普通文件。
    QFile blocker(dir.filePath("blocker"));
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    r.exportReport("run-1", dir.filePath("blocker/report.html"));
    QVERIFY(!exported.last()[1].toBool());
    QVERIFY(!exported.last()[2].toString().isEmpty());
}

void RepositoryTests::failedSnExportDeduplicatesWholeHistoryAndEscapesCsv()
{
    // 隔离保存异常 SN 历史和 CSV 输出的临时目录。
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 验证全历史异常 SN 去重及转义的待测仓储。
    Repository r;
    r.initialize(dir.filePath("data.db"));
    // reserved、saved 记录历史准备结果，exported 记录导出，history 记录查询回报。
    QSignalSpy reserved(&r, &Repository::runReserved), saved(&r, &Repository::resultSaved),
        exported(&r, &Repository::failedSnsExported), history(&r, &Repository::historyReady);
    // 先预约 run 再按 fail 构造终态，检查每次准备均成功。
    const auto save = [&](const QJsonObject &run, bool fail)
    {
        r.reserveRun(run);
        if (!reserved.last()[1].toBool())
            return false;
        r.saveResult(terminal(run, fail));
        return saved.last()[1].toBool();
    };
    for (int i = 0; i < 105; ++i)
        QVERIFY(save(request(QString("fail-%1").arg(i), i % 4 + 1,
                             QString("FAIL-%1").arg(i, 3, 10, QChar('0'))),
                     true));
    QVERIFY(save(request("duplicate-fail", 1, "FAIL-000"), true));
    QVERIFY(save(request("passing-retest", 1, "FAIL-000"), false));
    QVERIFY(save(request("special", 1, QStringLiteral("00042,板\"A\"")), true));
    QVERIFY(save(request("pass-only", 1, "PASS-ONLY"), false));
    // 用于验证取消运行不计入异常 SN 的请求。
    const auto cancelled = request("cancelled", 2, "CANCELLED-ONLY");
    r.reserveRun(cancelled);
    QVERIFY(reserved.last()[1].toBool());
    // 由失败终态改造而来的取消记录。
    auto result = terminal(cancelled, true);
    result["outcome"] = "cancelled";
    r.saveResult(result);
    QVERIFY(saved.last()[1].toBool());
    r.reserveRun(request("unknown", 3, "UNKNOWN-ONLY"));
    QVERIFY(reserved.last()[1].toBool());
    r.markInterrupted("unknown", "station lost its execution record");
    r.reserveRun(request("pending", 4, "PENDING-ONLY"));
    QVERIFY(reserved.last()[1].toBool());
    r.queryHistory({});
    QCOMPARE(qvariant_cast<QJsonArray>(history.last()[0]).size(), 100);
    QVERIFY(history.last()[1].toBool());

    // 不带扩展名的导出目标，用于验证自动补充 CSV 后缀。
    const auto basePath = dir.filePath("failed-sns");
    r.exportFailedSns(basePath);
    QCOMPARE(exported.size(), 1);
    QCOMPARE(exported.last()[0].toString(), basePath + ".csv");
    QVERIFY(exported.last()[1].toBool());
    QCOMPARE(exported.last()[2].toLongLong(), 106);
    QVERIFY(exported.last()[3].toString().isEmpty());
    // 用于检查异常 SN 导出结果的 CSV 文件。
    QFile file(basePath + ".csv");
    QVERIFY(file.open(QIODevice::ReadOnly));
    // 待核对 BOM、去重、转义及原始 SN 文本的 CSV 字节。
    const auto csv = file.readAll();
    QVERIFY(csv.startsWith(QByteArray("\xEF\xBB\xBF"
                                      "SN\r\n")));
    QCOMPARE(csv.count('\n'), 107);
    QCOMPARE(csv.count(QStringLiteral("\"00042,板\"\"A\"\"\"\r\n").toUtf8()), 1);
    for (int i = 0; i < 105; ++i)
        QCOMPARE(csv.count(QString("\"FAIL-%1\"\r\n").arg(i, 3, 10, QChar('0')).toUtf8()), 1);
    QVERIFY(!csv.contains("PASS-ONLY"));
    QVERIFY(!csv.contains("CANCELLED-ONLY"));
    QVERIFY(!csv.contains("UNKNOWN-ONLY"));
    QVERIFY(!csv.contains("PENDING-ONLY"));
}

void RepositoryTests::failedSnExportHandlesEmptyHistoryAndErrorsWithoutOverwriting()
{
    // 隔离保存导出故障测试数据的临时目录。
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 导出失败时必须保持原内容的目标文件路径。
    const auto keepPath = dir.filePath("keep.csv");
    // 写入并保留原内容的目标文件。
    QFile keep(keepPath);
    QVERIFY(keep.open(QIODevice::WriteOnly));
    QCOMPARE(keep.write("keep this file"), 14);
    keep.close();
    // 核对失败导出没有破坏 keepPath 原文件的检查操作。
    const auto verifyKept = [&]
    {
        // 用于重新读取并确认原内容仍在的目标文件。
        QFile file(keepPath);
        return file.open(QIODevice::ReadOnly) && file.readAll() == "keep this file";
    };
    // 验证空历史和失败导出行为的待测仓储。
    Repository r;
    // 记录异常 SN 导出的成功标记、数量和错误原因。
    QSignalSpy exported(&r, &Repository::failedSnsExported);
    r.exportFailedSns(keepPath);
    QCOMPARE(exported.size(), 1);
    QVERIFY(!exported.last()[1].toBool());
    QVERIFY(verifyKept());

    // 初始化空历史以及构造数据库故障的文件路径。
    const auto databasePath = dir.filePath("data.db");
    r.initialize(databasePath);
    r.exportFailedSns({});
    QCOMPARE(exported.size(), 2);
    QVERIFY(!exported.last()[1].toBool());
    QVERIFY(!exported.last()[3].toString().isEmpty());
    // 用于验证空历史仍输出 CSV 表头的目标路径。
    const auto emptyPath = dir.filePath("empty.CSV");
    r.exportFailedSns(emptyPath);
    QCOMPARE(exported.size(), 3);
    QCOMPARE(exported.last()[0].toString(), emptyPath);
    QVERIFY(exported.last()[1].toBool());
    QCOMPARE(exported.last()[2].toLongLong(), 0);
    // 读取空历史导出结果的 CSV 文件。
    QFile empty(emptyPath);
    QVERIFY(empty.open(QIODevice::ReadOnly));
    QCOMPARE(empty.readAll(), QByteArray("\xEF\xBB\xBF"
                                         "SN\r\n"));

    // 占据目标父目录位置以模拟输出路径故障的普通文件。
    QFile blocker(dir.filePath("blocker"));
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    r.exportFailedSns(dir.filePath("blocker/failed.csv"));
    QCOMPARE(exported.size(), 4);
    QVERIFY(!exported.last()[1].toBool());
    QVERIFY(!exported.last()[3].toString().isEmpty());
    // 用于破坏 runs 表以模拟查询失败的独立数据库连接。
    ExternalConnection db(databasePath);
    QVERIFY(db.execute("DROP TABLE runs"));
    r.exportFailedSns(keepPath);
    QCOMPARE(exported.size(), 5);
    QVERIFY(!exported.last()[1].toBool());
    QVERIFY(!exported.last()[3].toString().isEmpty());
    QVERIFY(verifyKept());
}

void RepositoryTests::manualResolutionPreservesEvidenceAndSuspendsStation()
{
    // 为人工结算隔离数据库与审计状态。
    QTemporaryDir directory;
    // 保存本用例待验证数据库的独立路径。
    const auto path = directory.filePath("manual.db");
    // 创建真实配方、运行预约和终态的测试仓储。
    Repository repository;
    repository.initialize(path);
    // 记录原始运行身份、处置回报和后续准入回报。
    const auto run = request("manual-run", 1, "MANUAL-SN");
    // 记录人工处置事务的完成回报。
    QSignalSpy disposed(&repository, &Repository::manualDispositionSaved);
    // 记录工位重新启用事务的回报。
    QSignalSpy resumed(&repository, &Repository::stationResumed);
    // 记录终态保存或迟到覆盖拒绝的回报。
    QSignalSpy saved(&repository, &Repository::resultSaved);
    // 记录后续工位或产品准入回报。
    QSignalSpy reserved(&repository, &Repository::runReserved);
    // 记录维护列表和审计概览回报。
    QSignalSpy overview(&repository, &Repository::maintenanceReady);
    repository.reserveRun(run);
    repository.resolveManually(1, "manual-run", "engineer", "设备断电并隔离", false);
    QVERIFY(!disposed.last()[2].toBool());
    // 独立连接核对没有伪造测量，也没有提前释放占用。
    ExternalConnection database(path);
    QCOMPARE(database.text("SELECT execution FROM runs"), "RESERVED");
    QCOMPARE(database.count("manual_resolutions"), 0);
    repository.resolveManually(1, "manual-run", "engineer", "设备断电并隔离", true);
    QVERIFY(disposed.last()[2].toBool());
    QCOMPARE(database.text("SELECT verdict FROM runs WHERE run_id='manual-run'"), "UNKNOWN");
    QCOMPARE(database.text("SELECT recovery_pending FROM runs WHERE run_id='manual-run'"), "0");
    QCOMPARE(database.count("results"), 0);
    QCOMPARE(database.count("steps"), 0);
    QCOMPARE(database.count("manual_resolutions"), 1);
    QCOMPARE(database.count("station_suspensions"), 1);
    QCOMPARE(database.text("SELECT actor FROM operational_events"), "engineer");
    repository.saveResult(terminal(run));
    QVERIFY(!saved.last()[1].toBool());
    QCOMPARE(database.count("results"), 0);
    repository.resolveManually(1, "manual-run", "engineer", "重复人工结算原因", true);
    QVERIFY(!disposed.last()[2].toBool());
    QCOMPARE(database.count("operational_events"), 1);
    repository.reserveRun(request("blocked-station", 1, "OTHER-SN"));
    QVERIFY(!reserved.last()[1].toBool());
    repository.reserveRun(request("released-sn", 2, "MANUAL-SN"));
    QVERIFY(reserved.last()[1].toBool());
    repository.queryMaintenance();
    QCOMPARE(overview.size(), 1);
    // 维护列表展示停用工位和操作者的原始处置记录。
    const auto state = qvariant_cast<QJsonObject>(overview.last()[0]);
    QCOMPARE(state["suspendedStations"].toArray().size(), 1);
    QCOMPARE(state["audit"].toArray()[0].toObject()["action"].toString(), "MANUAL_RESOLUTION");
    repository.resumeStation(1, "engineer", "设备已更换并确认复位");
    QVERIFY(resumed.last()[1].toBool());
    QCOMPARE(database.count("station_suspensions"), 0);
    QCOMPARE(database.count("manual_resolutions"), 1);
    repository.reserveRun(request("new-station-run", 1, "NEW-SN"));
    QVERIFY(reserved.last()[1].toBool());
}

void RepositoryTests::manualResolutionAuditFailureRollsBackAllState()
{
    // 在审计插入阶段注入错误，验证整个处置事务回滚。
    QTemporaryDir directory;
    // 保存本用例待验证数据库的独立路径。
    const auto path = directory.filePath("rollback.db");
    // 创建真实配方、运行预约和终态的测试仓储。
    Repository repository;
    repository.initialize(path);
    repository.reserveRun(request("rollback-run"));
    // 记录人工处置事务的完成回报。
    QSignalSpy disposed(&repository, &Repository::manualDispositionSaved);
    // 独立检查已持久化的安全状态和审计。
    ExternalConnection database(path);
    QVERIFY(database.execute("CREATE TRIGGER reject_audit BEFORE INSERT ON operational_events "
                             "BEGIN SELECT RAISE(ABORT,'audit unavailable'); END"));
    repository.resolveManually(1, "rollback-run", "engineer", "已确认原设备断电", true);
    QVERIFY(!disposed.last()[2].toBool());
    QCOMPARE(database.text("SELECT execution FROM runs"), "RESERVED");
    QCOMPARE(database.count("manual_resolutions"), 0);
    QCOMPARE(database.count("station_suspensions"), 0);
    QCOMPARE(database.count("operational_events"), 0);
    QVERIFY(database.execute("DROP TRIGGER reject_audit"));
    repository.resolveManually(1, "rollback-run", "engineer", "已确认原设备断电", true);
    QVERIFY(disposed.last()[2].toBool());
}

void RepositoryTests::timeoutAuditAndSafetyStateSurviveRestart()
{
    // 在真实终态到达后仍保留本地超时的时间和原因。
    QTemporaryDir directory;
    // 保存本用例待验证数据库的独立路径。
    const auto path = directory.filePath("timeout.db");
    {
        // 创建真实配方、运行预约和终态的测试仓储。
        Repository repository;
        repository.initialize(path);
        // 保存允许迟到完成的原始运行请求。
        const auto run = request("timeout-run");
        repository.reserveRun(run);
        repository.recordTimeout("timeout-run", 1, "整轮测试超时，已请求取消并核对");
        repository.recordTimeout("timeout-run", 1, "重复超时不应增加事件");
        repository.saveResult(terminal(run));
        // 独立检查已持久化的安全状态和审计。
        ExternalConnection database(path);
        QCOMPARE(database.text("SELECT verdict FROM runs"), "PASS");
        QCOMPARE(database.count("operational_events"), 1);
        QVERIFY(database.text("SELECT reason FROM runs").contains(QStringLiteral("超时")));
        repository.reserveRun(request("manual-after-timeout", 2, "SECOND-SN"));
        repository.resolveManually(2, "manual-after-timeout", "engineer", "设备已停机等待维修",
                                   true);
        // 模拟迟到旧设备导致其他有任务工位安全停用。
        repository.reserveRun(request("still-active", 3, "THIRD-SN"));
        repository.recordStationSuspension(3, "old-closed-run", "检测到已处置旧运行");
        // 记录工位重新启用事务的回报。
        QSignalSpy resumed(&repository, &Repository::stationResumed);
        repository.resumeStation(3, "engineer", "确认复位但仍有任务");
        QVERIFY(!resumed.last()[1].toBool());
    }
    // 重新打开恢复库并执行正常的启动核对。
    Repository reopened;
    // 记录重启后恢复的结算编号与停用工位。
    QSignalSpy safety(&reopened, &Repository::safetyStateReady);
    reopened.initialize(path);
    QCOMPARE(safety.size(), 1);
    QCOMPARE(qvariant_cast<QJsonArray>(safety.last()[0]).size(), 1);
    QCOMPARE(qvariant_cast<QJsonArray>(safety.last()[1]).size(), 2);
    // 独立检查已持久化的安全状态和审计。
    ExternalConnection database(path);
    QCOMPARE(database.text("SELECT verdict FROM runs WHERE run_id='manual-after-timeout'"),
             "UNKNOWN");
    QCOMPARE(database.count("manual_resolutions"), 1);
}

void RepositoryTests::versionTwoMigrationAndMissingSafetyTableAreChecked()
{
    // 去掉维护表，重现原模式2数据库的真实形状。
    QTemporaryDir directory;
    // 保存本用例待验证数据库的独立路径。
    const auto path = directory.filePath("old-v2.db");
    {
        // 创建真实配方、运行预约和终态的测试仓储。
        Repository repository;
        repository.initialize(path);
        // 绑定本次验证的运行身份和原始配方。
        const auto run = request("old-done");
        repository.reserveRun(run);
        repository.saveResult(terminal(run));
    }
    {
        // 独立检查已持久化的安全状态和审计。
        ExternalConnection database(path);
        QVERIFY(database.execute("DROP TABLE manual_resolutions"));
        QVERIFY(database.execute("DROP TABLE station_suspensions"));
        QVERIFY(database.execute("DROP TABLE operational_events"));
        QVERIFY(database.execute("PRAGMA user_version=2"));
    }
    {
        // 验证旧备份恢复后能够迁移到当前模式。
        Repository upgraded;
        // 记录模式升级或损坏库拒绝初始化的回报。
        QSignalSpy initialized(&upgraded, &Repository::initialized);
        upgraded.initialize(path);
        QVERIFY(initialized.last()[0].toBool());
        // 独立检查已持久化的安全状态和审计。
        ExternalConnection database(path);
        QCOMPARE(database.text("PRAGMA user_version"), "3");
        QCOMPARE(database.count("results"), 1);
        QCOMPARE(database.count("manual_resolutions"), 0);
    }
    {
        // 独立检查已持久化的安全状态和审计。
        ExternalConnection database(path);
        QVERIFY(database.execute("DROP TABLE manual_resolutions"));
    }
    // 验证安全表损坏时拒绝打开数据库。
    Repository damaged;
    // 记录模式升级或损坏库拒绝初始化的回报。
    QSignalSpy initialized(&damaged, &Repository::initialized);
    damaged.initialize(path);
    QVERIFY(!initialized.last()[0].toBool());
    // 独立检查已持久化的安全状态和审计。
    ExternalConnection database(path);
    QCOMPARE(database.text("PRAGMA user_version"), "3");
    QCOMPARE(database.count("results"), 1);
}
QTEST_GUILESS_MAIN(RepositoryTests)
#include "test_repository.moc"
