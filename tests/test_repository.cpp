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
QJsonObject request(const QString &id = "run-1", int station = 1, const QString &sn = "SN-001",
                    QJsonObject recipe = Domain::defaultRecipe())
{
    return {{"runId", id},
            {"stationId", station},
            {"sn", sn},
            {"recipeVersion", recipe["version"]},
            {"recipe", recipe}};
}
QJsonObject terminal(const QJsonObject &r, bool fail = false)
{
    QJsonArray values;
    int i = 0;
    for (const auto &v : r["recipe"].toObject()["steps"].toArray())
    {
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
    explicit ExternalConnection(const QString &path)
    {
        name = "repository-test-" + QUuid::createUuid().toString();
        db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(path);
        db.open();
    }
    ~ExternalConnection()
    {
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }
    int count(const QString &table)
    {
        QSqlQuery q(db);
        if (!q.exec("SELECT count(*) FROM " + table) || !q.next())
            return -1;
        return q.value(0).toInt();
    }
    QString text(const QString &sql)
    {
        QSqlQuery q(db);
        if (!q.exec(sql) || !q.next())
            return {};
        return q.value(0).toString();
    }
    bool execute(const QString &sql)
    {
        QSqlQuery q(db);
        return q.exec(sql);
    }
    QSqlDatabase db;

  private:
    QString name;
};
} // namespace

class RepositoryTests final : public QObject
{
    Q_OBJECT
  private slots:
    void initializesWalAndDefaultRecipe();
    void reservationsRequirePublishedIdentityAndExclusiveAdmission();
    void finalEvaluationDeduplicatesAndRejectsConflicts();
    void invalidTerminalLeavesReservationUntouched();
    void stepInsertionFailureRollsBackEntireResult();
    void databaseLockCanRetryWithoutPartialRows();
    void restartPreservesAndReconcilesInterruptedRun();
    void unknownResolutionReleasesPersistedHold();
    void versionOneMigratesWithoutLosingHistory();
    void publishedRecipeVersionIsImmutable();
    void keysetHistoryIsBoundedAndStable();
    void cancellationHasSeparateVerdict();
    void futureSchemaIsRejectedWithoutChanges();
    void connectionLivesOnWorkerThread();
    void reportUsesBoundSnapshotAndEscapesIdentity();
    void reportShowsUnexecutedStepsAndPropagatesFileFailure();
    void failedSnExportDeduplicatesWholeHistoryAndEscapesCsv();
    void failedSnExportHandlesEmptyHistoryAndErrorsWithoutOverwriting();
};

void RepositoryTests::initializesWalAndDefaultRecipe()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = dir.filePath("nested/data.db");
    Repository r;
    QSignalSpy initialized(&r, &Repository::initialized), recipes(&r, &Repository::recipesReady);
    r.initialize(path);
    QCOMPARE(initialized.count(), 1);
    QVERIFY(initialized.last()[0].toBool());
    r.listRecipes();
    QCOMPARE(recipes.count(), 1);
    const auto list = qvariant_cast<QJsonArray>(recipes.last()[0]);
    QCOMPARE(list.size(), 1);
    QCOMPARE(list[0].toObject(), Domain::defaultRecipe());
    ExternalConnection db(path);
    QVERIFY(db.db.isOpen());
    QCOMPARE(db.text("PRAGMA journal_mode"), "wal");
    QCOMPARE(db.text("PRAGMA user_version"), "2");
    QCOMPARE(db.count("runs"), 0);
    QCOMPARE(db.count("recipes"), 1);
}

void RepositoryTests::reservationsRequirePublishedIdentityAndExclusiveAdmission()
{
    QTemporaryDir dir;
    const auto path = dir.filePath("test.db");
    Repository r;
    r.initialize(path);
    QSignalSpy reserved(&r, &Repository::runReserved), saved(&r, &Repository::resultSaved);
    const auto first = request();
    r.reserveRun(first);
    QVERIFY(reserved.last()[1].toBool());
    r.reserveRun(request("same-station", 1, "SN-002"));
    QVERIFY(!reserved.last()[1].toBool());
    r.reserveRun(request("same-sn", 2, "SN-001"));
    QVERIFY(!reserved.last()[1].toBool());
    r.reserveRun(first);
    QVERIFY(!reserved.last()[1].toBool());
    auto changed = Domain::defaultRecipe();
    changed["name"] = "changed";
    r.reserveRun(request("tampered", 2, "SN-002", changed));
    QVERIFY(!reserved.last()[1].toBool());
    changed["version"] = "unpublished";
    r.reserveRun(request("unpublished", 2, "SN-002", changed));
    QVERIFY(!reserved.last()[1].toBool());
    r.saveResult(terminal(request("unknown", 2, "SN-002")));
    QVERIFY(!saved.last()[1].toBool());
    ExternalConnection db(path);
    QCOMPARE(db.count("runs"), 1);
    QCOMPARE(db.count("results"), 0);
    r.reserveRun(request("other", 2, "SN-002"));
    QVERIFY(reserved.last()[1].toBool());
    QCOMPARE(db.count("runs"), 2);
}

void RepositoryTests::finalEvaluationDeduplicatesAndRejectsConflicts()
{
    QTemporaryDir dir;
    const auto path = dir.filePath("test.db");
    Repository r;
    r.initialize(path);
    QSignalSpy saved(&r, &Repository::resultSaved);
    const auto run = request();
    r.reserveRun(run);
    auto result = terminal(run, true);
    result["verdict"] = "PASS";
    r.saveResult(result);
    QVERIFY(saved.last()[1].toBool());
    QVERIFY(!saved.last()[2].toBool());
    auto output = qvariant_cast<QJsonObject>(saved.last()[0]);
    QCOMPARE(output["verdict"].toString(), "FAIL");
    QCOMPARE(output["evaluatedSteps"].toArray().size(), 3);
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
    QSignalSpy reserved(&r, &Repository::runReserved);
    r.reserveRun(request("retest", 1, "SN-001"));
    QVERIFY(reserved.last()[1].toBool());
    QCOMPARE(db.count("runs"), 2);
}

void RepositoryTests::invalidTerminalLeavesReservationUntouched()
{
    QTemporaryDir dir;
    const auto path = dir.filePath("test.db");
    Repository r;
    r.initialize(path);
    const auto run = request();
    r.reserveRun(run);
    QSignalSpy saved(&r, &Repository::resultSaved);
    const auto good = terminal(run);
    QList<QJsonObject> cases;
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
    auto step = steps[0].toObject();
    step["value"] = QJsonValue::Null;
    steps[0] = step;
    bad["steps"] = steps;
    cases.append(bad);
    bad = good;
    bad["outcome"] = "device-says-pass";
    cases.append(bad);
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
    QTemporaryDir dir;
    const auto path = dir.filePath("test.db");
    Repository r;
    r.initialize(path);
    const auto run = request();
    r.reserveRun(run);
    ExternalConnection db(path);
    QVERIFY(
        db.execute("CREATE TRIGGER fail_second_step BEFORE INSERT ON steps WHEN NEW.step_index=1 "
                   "BEGIN SELECT RAISE(ABORT,'injected disk/write failure'); END"));
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
    QTemporaryDir dir;
    const auto path = dir.filePath("test.db");
    Repository r;
    r.initialize(path);
    const auto run = request();
    r.reserveRun(run);
    ExternalConnection db(path);
    QVERIFY(db.execute("BEGIN IMMEDIATE"));
    QSignalSpy saved(&r, &Repository::resultSaved);
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
    QTemporaryDir dir;
    const auto path = dir.filePath("test.db");
    const auto completed = request("finished", 1, "DONE");
    const auto active = request("interrupted", 2, "RECOVER");
    {
        Repository r;
        r.initialize(path);
        r.reserveRun(completed);
        r.saveResult(terminal(completed));
        r.reserveRun(active);
        r.shutdown();
    }
    Repository r;
    QSignalSpy recovery(&r, &Repository::recoverableRuns);
    r.initialize(path);
    ExternalConnection db(path);
    QCOMPARE(recovery.count(), 1);
    const auto snapshots = qvariant_cast<QJsonArray>(recovery.last()[0]);
    QCOMPARE(snapshots.size(), 1);
    QCOMPARE(snapshots[0].toObject(), active);
    QCOMPARE(db.text("SELECT execution FROM runs WHERE run_id='interrupted'"), "INTERRUPTED");
    QCOMPARE(db.text("SELECT verdict FROM runs WHERE run_id='interrupted'"), "UNKNOWN");
    QCOMPARE(db.text("SELECT verdict FROM runs WHERE run_id='finished'"), "PASS");
    QCOMPARE(db.text("SELECT recovery_pending FROM runs WHERE run_id='interrupted'"), "1");
    QSignalSpy reserved(&r, &Repository::runReserved);
    r.reserveRun(request("same-product", 3, "RECOVER"));
    QVERIFY(!reserved.last()[1].toBool());
    r.reserveRun(request("same-station", 2, "DIFFERENT"));
    QVERIFY(!reserved.last()[1].toBool());
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
    QTemporaryDir dir;
    const auto path = dir.filePath("test.db");
    const auto active = request();
    {
        Repository r;
        r.initialize(path);
        r.reserveRun(active);
    }
    // Repeated restarts must not silently release an unresolved SN.
    {
        Repository r;
        QSignalSpy recovery(&r, &Repository::recoverableRuns);
        r.initialize(path);
        QCOMPARE(qvariant_cast<QJsonArray>(recovery.last()[0]).size(), 1);
    }
    Repository r;
    QSignalSpy recovery(&r, &Repository::recoverableRuns),
        interrupted(&r, &Repository::runInterrupted), reserved(&r, &Repository::runReserved);
    r.initialize(path);
    QCOMPARE(qvariant_cast<QJsonArray>(recovery.last()[0]).size(), 1);
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
    QTemporaryDir dir;
    const auto path = dir.filePath("v1.db");
    const auto active = request("active", 2, "ACTIVE");
    {
        Repository r;
        r.initialize(path);
        const auto done = request("done", 1, "DONE");
        r.reserveRun(done);
        r.saveResult(terminal(done));
        r.reserveRun(active);
    }
    {
        // Recreate the previous version's shape while preserving real history.
        ExternalConnection db(path);
        QVERIFY(db.execute("DROP INDEX one_active_station"));
        QVERIFY(db.execute("DROP INDEX one_active_sn"));
        QVERIFY(db.execute("ALTER TABLE runs DROP COLUMN recovery_pending"));
        QVERIFY(db.execute("PRAGMA user_version=1"));
    }
    Repository r;
    QSignalSpy initialized(&r, &Repository::initialized),
        recovery(&r, &Repository::recoverableRuns), reserved(&r, &Repository::runReserved);
    r.initialize(path);
    QVERIFY(initialized.last()[0].toBool());
    ExternalConnection db(path);
    QCOMPARE(db.text("PRAGMA user_version"), "2");
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
    QTemporaryDir dir;
    const auto path = dir.filePath("test.db");
    Repository r;
    r.initialize(path);
    QSignalSpy error(&r, &Repository::operationError);
    const auto original = Domain::defaultRecipe();
    r.publishRecipe(original);
    QCOMPARE(error.count(), 0);
    auto edited = original;
    edited["name"] = "silent-change";
    r.publishRecipe(edited);
    QCOMPARE(error.count(), 1);
    ExternalConnection db(path);
    QCOMPARE(db.count("recipes"), 1);
    QCOMPARE(db.text("SELECT sha256 FROM recipes"), Domain::recipeHash(original));
    edited["version"] = "demo-5v-v2";
    r.publishRecipe(edited);
    QCOMPARE(error.count(), 1);
    QCOMPARE(db.count("recipes"), 2);
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
    QTemporaryDir dir;
    const auto path = dir.filePath("test.db");
    Repository r;
    r.initialize(path);
    QSignalSpy reserved(&r, &Repository::runReserved), saved(&r, &Repository::resultSaved),
        history(&r, &Repository::historyReady);
    for (int i = 1; i <= 205; ++i)
    {
        const auto run = request(QString("run-%1").arg(i), i % 4 + 1, i % 2 ? "ODD" : "EVEN");
        r.reserveRun(run);
        QVERIFY(reserved.last()[1].toBool());
        r.saveResult(terminal(run));
        QVERIFY(saved.last()[1].toBool());
    }
    r.queryHistory({}, 0);
    auto rows = qvariant_cast<QJsonArray>(history.last()[0]);
    QCOMPARE(rows.size(), 100);
    QVERIFY(history.last()[1].toBool());
    QCOMPARE(rows.first().toObject()["id"].toInt(), 205);
    const int cursor = rows.last().toObject()["id"].toInt();
    // An inserted latest run cannot shift the second page or create duplicates.
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
        const auto row = v.toObject();
        QCOMPARE(row["sn"].toString(), "EVEN");
        QCOMPARE(row["steps"].toArray().size(), 3);
    }
    ExternalConnection db(path);
    QCOMPARE(db.count("runs"), 206);
    QCOMPARE(db.count("results"), 206);
    QCOMPARE(db.count("steps"), 618);
}

void RepositoryTests::cancellationHasSeparateVerdict()
{
    QTemporaryDir dir;
    const auto path = dir.filePath("test.db");
    Repository r;
    r.initialize(path);
    const auto run = request();
    r.reserveRun(run);
    auto result = terminal(run);
    result["outcome"] = "cancelled";
    auto steps = result["steps"].toArray();
    while (steps.size() > 1)
        steps.removeLast();
    result["steps"] = steps;
    QSignalSpy saved(&r, &Repository::resultSaved);
    r.saveResult(result);
    QVERIFY(saved.last()[1].toBool());
    QCOMPARE(qvariant_cast<QJsonObject>(saved.last()[0])["verdict"].toString(), "CANCELLED");
    ExternalConnection db(path);
    QCOMPARE(db.text("SELECT execution FROM runs"), "CANCELLED");
    QCOMPARE(db.count("steps"), 1);
}

void RepositoryTests::futureSchemaIsRejectedWithoutChanges()
{
    QTemporaryDir dir;
    const auto path = dir.filePath("future.db");
    {
        ExternalConnection db(path);
        QVERIFY(db.execute("PRAGMA user_version=99"));
        QVERIFY(db.execute("CREATE TABLE sentinel(value TEXT)"));
        QVERIFY(db.execute("INSERT INTO sentinel VALUES('keep')"));
    }
    Repository r;
    QSignalSpy initialized(&r, &Repository::initialized);
    r.initialize(path);
    QVERIFY(!initialized.last()[0].toBool());
    ExternalConnection db(path);
    QCOMPARE(db.text("PRAGMA user_version"), "99");
    QCOMPARE(db.text("SELECT value FROM sentinel"), "keep");
}

void RepositoryTests::connectionLivesOnWorkerThread()
{
    QTemporaryDir dir;
    const auto path = dir.filePath("worker.db");
    QThread thread;
    auto *r = new Repository;
    r->moveToThread(&thread);
    connect(&thread, &QThread::finished, r, &QObject::deleteLater);
    QSignalSpy initialized(r, &Repository::initialized), saved(r, &Repository::resultSaved);
    thread.start();
    QMetaObject::invokeMethod(r, [r, path] { r->initialize(path); });
    // Always stop the worker before checking assertions so failures cannot leave a running QThread.
    const bool initializedInTime = initialized.wait(3000);
    if (initializedInTime && initialized.last()[0].toBool())
    {
        const auto run = request();
        QMetaObject::invokeMethod(r,
                                  [r, run]
                                  {
                                      r->reserveRun(run);
                                      r->saveResult(terminal(run));
                                  });
        saved.wait(3000);
    }
    QMetaObject::invokeMethod(r,
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
    QTemporaryDir dir;
    Repository r;
    r.initialize(dir.filePath("data.db"));
    const auto run = request("report-run", 1, "<script>&SN");
    r.reserveRun(run);
    r.saveResult(terminal(run));
    auto newer = Domain::defaultRecipe();
    newer["version"] = "report-v2";
    auto rules = newer["steps"].toArray();
    auto voltage = rules[0].toObject();
    voltage["min"] = 4.9;
    rules[0] = voltage;
    newer["steps"] = rules;
    r.publishRecipe(newer);
    QSignalSpy exported(&r, &Repository::reportReady);
    const auto path = dir.filePath("report.html");
    r.exportReport("report-run", path);
    QCOMPARE(exported.size(), 1);
    QVERIFY(exported.last()[1].toBool());
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
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
    QTemporaryDir dir;
    Repository r;
    r.initialize(dir.filePath("data.db"));
    const auto run = request();
    r.reserveRun(run);
    auto result = terminal(run);
    result["outcome"] = "cancelled";
    auto values = result["steps"].toArray();
    while (values.size() > 1)
        values.removeLast();
    result["steps"] = values;
    r.saveResult(result);
    QSignalSpy exported(&r, &Repository::reportReady);
    const auto path = dir.filePath("cancelled.html");
    r.exportReport("run-1", path);
    QVERIFY(exported.last()[1].toBool());
    QFile report(path);
    QVERIFY(report.open(QIODevice::ReadOnly));
    const auto html = QString::fromUtf8(report.readAll());
    QVERIFY(html.contains("CANCELLED"));
    QCOMPARE(html.count(QStringLiteral("未执行/未归档")), 2);
    r.exportReport("not-existing", dir.filePath("missing.html"));
    QVERIFY(!exported.last()[1].toBool());
    QFile blocker(dir.filePath("blocker"));
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    r.exportReport("run-1", dir.filePath("blocker/report.html"));
    QVERIFY(!exported.last()[1].toBool());
    QVERIFY(!exported.last()[2].toString().isEmpty());
}

void RepositoryTests::failedSnExportDeduplicatesWholeHistoryAndEscapesCsv()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Repository r;
    r.initialize(dir.filePath("data.db"));
    QSignalSpy reserved(&r, &Repository::runReserved), saved(&r, &Repository::resultSaved),
        exported(&r, &Repository::failedSnsExported), history(&r, &Repository::historyReady);
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
    const auto cancelled = request("cancelled", 2, "CANCELLED-ONLY");
    r.reserveRun(cancelled);
    QVERIFY(reserved.last()[1].toBool());
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

    const auto basePath = dir.filePath("failed-sns");
    r.exportFailedSns(basePath);
    QCOMPARE(exported.size(), 1);
    QCOMPARE(exported.last()[0].toString(), basePath + ".csv");
    QVERIFY(exported.last()[1].toBool());
    QCOMPARE(exported.last()[2].toLongLong(), 106);
    QVERIFY(exported.last()[3].toString().isEmpty());
    QFile file(basePath + ".csv");
    QVERIFY(file.open(QIODevice::ReadOnly));
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
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto keepPath = dir.filePath("keep.csv");
    QFile keep(keepPath);
    QVERIFY(keep.open(QIODevice::WriteOnly));
    QCOMPARE(keep.write("keep this file"), 14);
    keep.close();
    const auto verifyKept = [&]
    {
        QFile file(keepPath);
        return file.open(QIODevice::ReadOnly) && file.readAll() == "keep this file";
    };
    Repository r;
    QSignalSpy exported(&r, &Repository::failedSnsExported);
    r.exportFailedSns(keepPath);
    QCOMPARE(exported.size(), 1);
    QVERIFY(!exported.last()[1].toBool());
    QVERIFY(verifyKept());

    const auto databasePath = dir.filePath("data.db");
    r.initialize(databasePath);
    r.exportFailedSns({});
    QCOMPARE(exported.size(), 2);
    QVERIFY(!exported.last()[1].toBool());
    QVERIFY(!exported.last()[3].toString().isEmpty());
    const auto emptyPath = dir.filePath("empty.CSV");
    r.exportFailedSns(emptyPath);
    QCOMPARE(exported.size(), 3);
    QCOMPARE(exported.last()[0].toString(), emptyPath);
    QVERIFY(exported.last()[1].toBool());
    QCOMPARE(exported.last()[2].toLongLong(), 0);
    QFile empty(emptyPath);
    QVERIFY(empty.open(QIODevice::ReadOnly));
    QCOMPARE(empty.readAll(), QByteArray("\xEF\xBB\xBF"
                                         "SN\r\n"));

    QFile blocker(dir.filePath("blocker"));
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    r.exportFailedSns(dir.filePath("blocker/failed.csv"));
    QCOMPARE(exported.size(), 4);
    QVERIFY(!exported.last()[1].toBool());
    QVERIFY(!exported.last()[3].toString().isEmpty());
    ExternalConnection db(databasePath);
    QVERIFY(db.execute("DROP TABLE runs"));
    r.exportFailedSns(keepPath);
    QCOMPARE(exported.size(), 5);
    QVERIFY(!exported.last()[1].toBool());
    QVERIFY(!exported.last()[3].toString().isEmpty());
    QVERIFY(verifyKept());
}

QTEST_GUILESS_MAIN(RepositoryTests)
#include "test_repository.moc"
