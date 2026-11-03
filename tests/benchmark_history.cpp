#include "storage/repository.h"
#include "core/domain.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <functional>
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif

namespace
{
QString serial(int id)
{
    return QStringLiteral("BENCH-%1").arg(id, 7, 10, QLatin1Char('0'));
}
QString compact(const QJsonObject &o)
{
    return QString::fromUtf8(Domain::canonical(o));
}

// This synthetic fixture deliberately bypasses the production write path to
// prepare a large history cheaply. Its generation time is NOT a write benchmark.
bool buildFixture(const QString &path, int count, QString *error)
{
    const auto connection =
        QStringLiteral("history-fixture-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setDatabaseName(path);
    db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=1000");
    const auto build = [&]() -> bool
    {
        if (!db.open())
        {
            *error = db.lastError().text();
            return false;
        }
        QSqlQuery control(db);
        if (!control.exec("PRAGMA foreign_keys=ON") || !control.exec("BEGIN IMMEDIATE"))
        {
            *error = control.lastError().text();
            return false;
        }
        QSqlQuery run(db), result(db), step(db);
        if (!run.prepare("INSERT INTO "
                         "runs(run_id,station_id,sn,recipe_version,request_json,execution,verdict,"
                         "started_at,finished_at) VALUES(?,?,?,?,?,'COMPLETED','PASS',?,?)") ||
            !result.prepare(
                "INSERT INTO results(run_id,envelope_json,verdict) VALUES(?,?,'PASS')") ||
            !step.prepare("INSERT INTO "
                          "steps(run_id,step_index,step_id,label,unit,min_value,max_value,value,"
                          "passed) VALUES(?,?,?,?,?,?,?,?,?)"))
        {
            *error = QStringLiteral("Cannot prepare synthetic fixture SQL");
            return false;
        }
        const auto recipe = Domain::defaultRecipe();
        const auto rules = recipe["steps"].toArray();
        const QString started = QStringLiteral("2026-11-02T00:00:00.000Z"),
                      finished = QStringLiteral("2026-11-02T00:00:01.000Z");
        for (int i = 1; i <= count; ++i)
        {
            const auto id = QStringLiteral("benchmark-run-%1").arg(i);
            const auto sn = serial(i);
            const int station = (i - 1) % 4 + 1;
            const QJsonObject request{{"runId", id},
                                      {"stationId", station},
                                      {"sn", sn},
                                      {"recipeVersion", recipe["version"]},
                                      {"recipe", recipe}};
            run.bindValue(0, id);
            run.bindValue(1, station);
            run.bindValue(2, sn);
            run.bindValue(3, recipe["version"].toString());
            run.bindValue(4, compact(request));
            run.bindValue(5, started);
            run.bindValue(6, finished);
            if (!run.exec())
            {
                *error = run.lastError().text();
                return false;
            }
            QJsonArray measured;
            for (const auto &v : rules)
            {
                const auto rule = v.toObject();
                measured.append(QJsonObject{{"stepId", rule["id"]}, {"value", rule["nominal"]}});
            }
            const QJsonObject terminal{{"runId", id},
                                       {"stationId", station},
                                       {"sn", sn},
                                       {"recipeVersion", recipe["version"]},
                                       {"outcome", "completed"},
                                       {"steps", measured}};
            result.bindValue(0, id);
            result.bindValue(1, compact(terminal));
            if (!result.exec())
            {
                *error = result.lastError().text();
                return false;
            }
            for (qsizetype j = 0; j < rules.size(); ++j)
            {
                const auto rule = rules[j].toObject();
                step.bindValue(0, id);
                step.bindValue(1, int(j));
                step.bindValue(2, rule["id"].toString());
                step.bindValue(3, rule["label"].toString());
                step.bindValue(4, rule["unit"].toString());
                step.bindValue(5, rule["min"].toDouble());
                step.bindValue(6, rule["max"].toDouble());
                step.bindValue(7, rule["nominal"].toDouble());
                step.bindValue(8, 1);
                if (!step.exec())
                {
                    *error = step.lastError().text();
                    return false;
                }
            }
        }
        if (!db.commit())
        {
            *error = db.lastError().text();
            return false;
        }
        if (!control.exec("SELECT (SELECT count(*) FROM runs),(SELECT count(*) FROM "
                          "results),(SELECT count(*) FROM steps)") ||
            !control.next() || control.value(0).toInt() != count ||
            control.value(1).toInt() != count || control.value(2).toInt() != 3 * count)
        {
            *error = QStringLiteral("Synthetic fixture row counts do not match the requested size");
            return false;
        }
        control.finish();
        if (!control.exec("PRAGMA wal_checkpoint(TRUNCATE)") || !control.next() ||
            control.value(0).toInt() != 0)
        {
            *error = QStringLiteral("Cannot checkpoint synthetic fixture before measurement");
            return false;
        }
        return true;
    };
    const bool ok = build();
    if (!ok)
        db.rollback();
    db.close();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
    return ok;
}

QJsonObject statistics(QList<double> samples)
{
    std::sort(samples.begin(), samples.end());
    if (samples.isEmpty())
        return {{"sampleCount", 0}};
    const auto percentile = [&](double p)
    {
        const auto index =
            qBound<qsizetype>(0, qsizetype(std::ceil(p * samples.size())) - 1, samples.size() - 1);
        return samples[index];
    };
    double sum = 0;
    for (const auto value : samples)
        sum += value;
    return {{"sampleCount", int(samples.size())},
            {"minMs", samples.first()},
            {"p50Ms", percentile(.50)},
            {"p95Ms", percentile(.95)},
            {"maxMs", samples.last()},
            {"meanMs", sum / samples.size()},
            {"percentileMethod", "nearest_rank"}};
}

bool writeReport(const QString &path, const QJsonObject &report, QString *error)
{
    const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    if (path.isEmpty())
    {
        QTextStream(stdout) << bytes;
        return true;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    {
        *error = QStringLiteral("Cannot create report output directory");
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    {
        *error = file.errorString();
        return false;
    }
    return true;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("HistoryBenchmark");
    QCommandLineParser parser;
    parser.setApplicationDescription("Synthetic large-history query benchmark; not a production "
                                     "write-path or GUI rendering benchmark.");
    parser.addHelpOption();
    parser.addOption({"output", "JSON result file (default: stdout).", "path"});
    parser.addOption({"rows", "Synthetic completed run count, 1000..1000000.", "count", "100000"});
    parser.process(app);
    bool countOk = false;
    const int rows = parser.value("rows").toInt(&countOk);
    if (!countOk || rows < 1000 || rows > 1000000)
    {
        QTextStream(stderr) << "--rows must be between 1000 and 1000000\n";
        return 2;
    }
    QTemporaryDir temporary;
    if (!temporary.isValid())
    {
        QTextStream(stderr) << "Cannot create isolated fixture directory\n";
        return 2;
    }
    const auto database = temporary.filePath("history.db");
    QString error;
    auto *repository = new Repository;
    QThread databaseThread;
    databaseThread.setObjectName("HistoryBenchmarkDatabaseThread");
    repository->moveToThread(&databaseThread);
    QObject::connect(&databaseThread, &QThread::finished, repository, &QObject::deleteLater);
    databaseThread.start();
    const auto shutdown = [&]
    {
        QMetaObject::invokeMethod(
            repository,
            [repository]
            {
                repository->shutdown();
                QThread::currentThread()->quit();
            },
            Qt::QueuedConnection);
        databaseThread.wait();
    };
    bool initialized = false;
    QEventLoop initializeLoop;
    QTimer initializeTimeout;
    initializeTimeout.setSingleShot(true);
    const auto initializedConnection =
        QObject::connect(repository, &Repository::initialized, &initializeLoop,
                         [&](bool ok, const QString &message)
                         {
                             initialized = ok;
                             error = message;
                             initializeLoop.quit();
                         });
    QObject::connect(&initializeTimeout, &QTimer::timeout, &initializeLoop,
                     [&]
                     {
                         error = "Repository initialization timed out";
                         initializeLoop.quit();
                     });
    initializeTimeout.start(10000);
    QMetaObject::invokeMethod(repository,
                              [repository, database] { repository->initialize(database); });
    initializeLoop.exec();
    initializeTimeout.stop();
    QObject::disconnect(initializedConnection);
    if (!initialized)
    {
        shutdown();
        QTextStream(stderr) << error << '\n';
        return 1;
    }
    QElapsedTimer fixtureClock;
    fixtureClock.start();
    const bool built = buildFixture(database, rows, &error);
    const double fixtureMs = fixtureClock.nsecsElapsed() / 1e6;
    if (!built)
    {
        shutdown();
        QTextStream(stderr) << error << '\n';
        return 1;
    }
    const qint64 mainBytes = QFileInfo(database).size();
    const qint64 totalBytes =
        mainBytes + QFileInfo(database + "-wal").size() + QFileInfo(database + "-shm").size();

    constexpr int exactCount = 200, cursorCount = 100, pageSize = 100;
    int sample = 0, cursor = 0, expectedRows = 0, expectedFirstId = 0;
    QString expectedSn;
    QList<double> exactSamples, cursorSamples, timerGaps;
    QElapsedTimer requestClock, measurementClock, eventClock;
    QEventLoop measurements;
    QTimer queryTimeout, eventTimer;
    queryTimeout.setSingleShot(true);
    eventTimer.setInterval(10);
    bool completed = false;
    double previousTick = 0;
    const auto fail = [&](const QString &message)
    {
        error = message;
        measurements.quit();
    };
    std::function<void()> nextQuery;
    nextQuery = [&]
    {
        if (sample >= exactCount + cursorCount)
        {
            completed = true;
            measurements.quit();
            return;
        }
        QString filter;
        int before = 0;
        if (sample < exactCount)
        {
            const int id = int((qint64(sample) * 7919) % rows) + 1;
            filter = serial(id);
            expectedSn = filter;
            expectedRows = 1;
            expectedFirstId = id;
        }
        else
        {
            before = cursor;
            expectedSn.clear();
            expectedFirstId = before > 0 ? before - 1 : rows;
            expectedRows = qMin(pageSize, expectedFirstId);
        }
        queryTimeout.start(5000);
        requestClock.start();
        QMetaObject::invokeMethod(repository, [repository, filter, before]
                                  { repository->queryHistory(filter, before); });
    };
    QObject::connect(&queryTimeout, &QTimer::timeout, &measurements,
                     [&] { fail("A history query exceeded 5 seconds"); });
    QObject::connect(repository, &Repository::operationError, &measurements,
                     [&](const QString &message) { fail(message); });
    QObject::connect(repository, &Repository::historyReady, &measurements,
                     [&](const QJsonArray &page, bool more)
                     {
                         const double elapsed = requestClock.nsecsElapsed() / 1e6;
                         queryTimeout.stop();
                         if (page.size() != expectedRows || page.isEmpty() ||
                             page.first().toObject()["id"].toInt() != expectedFirstId)
                         {
                             fail("Returned page size or cursor boundary is incorrect");
                             return;
                         }
                         for (qsizetype i = 0; i < page.size(); ++i)
                         {
                             const auto row = page[i].toObject();
                             if (row["verdict"] != "PASS" || row["execution"] != "COMPLETED" ||
                                 row["steps"].toArray().size() != 3 ||
                                 (!expectedSn.isEmpty() && row["sn"].toString() != expectedSn) ||
                                 (expectedSn.isEmpty() && row["id"].toInt() != expectedFirstId - i))
                             {
                                 fail("History returned inconsistent run identity, verdict or "
                                      "ordered step details");
                                 return;
                             }
                         }
                         if (sample < exactCount)
                         {
                             if (more)
                             {
                                 fail("An exact unique SN query unexpectedly returned more pages");
                                 return;
                             }
                             exactSamples.append(elapsed);
                         }
                         else
                         {
                             cursorSamples.append(elapsed);
                             cursor = more ? page.last().toObject()["id"].toInt() : 0;
                         }
                         ++sample;
                         QTimer::singleShot(0, &measurements, nextQuery);
                     });
    QObject::connect(&eventTimer, &QTimer::timeout, &measurements,
                     [&]
                     {
                         const double current = eventClock.nsecsElapsed() / 1e6;
                         timerGaps.append(current - previousTick);
                         previousTick = current;
                     });
    measurementClock.start();
    eventClock.start();
    eventTimer.start();
    QTimer::singleShot(20, &measurements, nextQuery);
    measurements.exec();
    const double measurementMs = measurementClock.nsecsElapsed() / 1e6;
    eventTimer.stop();
    queryTimeout.stop();
    QJsonObject report{
        {"success", completed},
        {"recordedAtUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"platform", QSysInfo::prettyProductName()},
        {"architecture", QSysInfo::currentCpuArchitecture()},
        {"qtVersion", QString::fromLatin1(qVersion())},
        {"fixture", QJsonObject{{"completedRuns", rows},
                                {"results", rows},
                                {"steps", rows * 3},
                                {"generationMs", fixtureMs},
                                {"databaseMainBytes", double(mainBytes)},
                                {"databaseAndSidecarBytes", double(totalBytes)},
                                {"writeMethod", "synthetic prepared SQL in one bulk transaction; "
                                                "not production write-path performance"}}},
        {"exactSnQuery", statistics(exactSamples)},
        {"cursor100RowQuery", statistics(cursorSamples)},
        {"mainEventLoop10msTimerGap", statistics(timerGaps)},
        {"measurementDurationMs", measurementMs},
        {"maximumOutstandingQueries", 1},
        {"measurementNotes",
         QJsonArray{"Query latency includes queued worker dispatch, real Repository::queryHistory "
                    "SQL/step loading and delivery to the main event loop.",
                    "Synthetic fixture generation and schema initialization are excluded from "
                    "query latency and timer gap samples.",
                    "Main-thread QCoreApplication timer gaps are an event-loop responsiveness "
                    "proxy; no GUI rendering is measured.",
                    "OS file cache may be warm after fixture generation; these are not cold-cache "
                    "or long-duration stability measurements.",
                    "Exactly one query is outstanding; no queue-peak or high-concurrency claim is "
                    "made."}}};
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
    {
        report["processMemory"] =
            QJsonObject{{"peakWorkingSetBytes", double(memory.PeakWorkingSetSize)},
                        {"workingSetBytesAtEnd", double(memory.WorkingSetSize)},
                        {"scope", "whole benchmark process, including fixture generation"}};
    }
#endif
    if (!error.isEmpty())
        report["error"] = error;
    shutdown();
    if (!writeReport(parser.value("output"), report, &error))
    {
        QTextStream(stderr) << error << '\n';
        return 1;
    }
    if (!completed)
    {
        QTextStream(stderr) << error << '\n';
        return 1;
    }
    QTextStream(stderr) << "History benchmark completed: " << rows << " runs, "
                        << exactSamples.size() << " exact SN queries, " << cursorSamples.size()
                        << " cursor queries\n";
    return 0;
}
