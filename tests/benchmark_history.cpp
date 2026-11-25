#include "repository.h"
#include "domain.h"
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
// 按 id 生成固定宽度的唯一产品 SN。
QString serial(int id)
{
    return QStringLiteral("BENCH-%1").arg(id, 7, 10, QLatin1Char('0'));
}
// 将 o 转成规范化且紧凑的 JSON 文本。
QString compact(const QJsonObject &o)
{
    return QString::fromUtf8(Domain::canonical(o));
}

// This synthetic fixture deliberately bypasses the production write path to
// prepare a large history cheaply. Its generation time is NOT a write benchmark.
// 按 path、count 批量生成合成历史，失败原因写入 error。
bool buildFixture(const QString &path, int count, QString *error)
{
    // 合成数据专用的独立数据库连接名称。
    const auto connection =
        QStringLiteral("history-fixture-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    // 用于批量写入合成历史的 SQLite 连接。
    auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setDatabaseName(path);
    db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=1000");
    // 在同一事务内创建历史记录和步骤的操作。
    const auto build = [&]() -> bool
    {
        if (!db.open())
        {
            *error = db.lastError().text();
            return false;
        }
        // 执行事务及记录数校验的控制查询。
        QSqlQuery control(db);
        if (!control.exec("PRAGMA foreign_keys=ON") || !control.exec("BEGIN IMMEDIATE"))
        {
            *error = control.lastError().text();
            return false;
        }
        // run 写运行身份，result 写终态，step 写步骤明细。
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
        // 合成历史绑定的默认测试配方。
        const auto recipe = Domain::defaultRecipe();
        // 构造测量值和步骤记录使用的配方规则。
        const auto rules = recipe["steps"].toArray();
        // started、finished 为合成测试统一的开始和结束时间。
        const QString started = QStringLiteral("2026-11-02T00:00:00.000Z"),
                      finished = QStringLiteral("2026-11-02T00:00:01.000Z");
        for (int i = 1; i <= count; ++i)
        {
            // 当前合成测试的唯一运行编号。
            const auto id = QStringLiteral("benchmark-run-%1").arg(i);
            // 当前合成测试使用的产品 SN。
            const auto sn = serial(i);
            // 当前合成记录所属的工位编号。
            const int station = (i - 1) % 4 + 1;
            // 准备写入数据库的完整运行请求快照。
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
            // 按配方标称值构造的有序测量结果。
            QJsonArray measured;
            for (const auto &v : rules)
            {
                // 当前测量项采用的配方规则对象。
                const auto rule = v.toObject();
                measured.append(QJsonObject{{"stepId", rule["id"]}, {"value", rule["nominal"]}});
            }
            // 与运行身份关联的完整合成终态。
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
                // 写入步骤表所需的当前规则明细。
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
    // 标记合成数据事务是否成功完成。
    const bool ok = build();
    if (!ok)
        db.rollback();
    db.close();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
    return ok;
}

// 汇总 samples 耗时样本的均值和分位数。
QJsonObject statistics(QList<double> samples)
{
    std::sort(samples.begin(), samples.end());
    if (samples.isEmpty())
        return {{"sampleCount", 0}};
    // 计算 p 指定比例对应的最近秩分位数。
    const auto percentile = [&](double p)
    {
        // 分位数在排序后样本中的位置。
        const auto index =
            qBound<qsizetype>(0, qsizetype(std::ceil(p * samples.size())) - 1, samples.size() - 1);
        return samples[index];
    };
    // 累计样本耗时，用于计算平均值。
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

// 将 report 原子写入 path，失败原因写入 error。
bool writeReport(const QString &path, const QJsonObject &report, QString *error)
{
    // 需要写入文件或标准输出的 JSON 字节。
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
    // 保证报告成功写完后才替换目标的输出文件。
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    {
        *error = file.errorString();
        return false;
    }
    return true;
}
} // namespace

// 读取命令行并执行隔离的历史查询性能测量。
int main(int argc, char **argv)
{
    // 承载命令行参数和主线程事件循环的应用对象。
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("HistoryBenchmark");
    // 读取历史条数及报告输出位置的参数解析器。
    QCommandLineParser parser;
    parser.setApplicationDescription("Synthetic large-history query benchmark; not a production "
                                     "write-path or GUI rendering benchmark.");
    parser.addHelpOption();
    parser.addOption({"output", "JSON result file (default: stdout).", "path"});
    parser.addOption({"rows", "Synthetic completed run count, 1000..1000000.", "count", "100000"});
    parser.process(app);
    // 标记历史条数参数是否成功转成整数。
    bool countOk = false;
    // 本次测量需要生成的合成历史总数。
    const int rows = parser.value("rows").toInt(&countOk);
    if (!countOk || rows < 1000 || rows > 1000000)
    {
        QTextStream(stderr) << "--rows must be between 1000 and 1000000\n";
        return 2;
    }
    // 隔离保存合成数据库的临时目录。
    QTemporaryDir temporary;
    if (!temporary.isValid())
    {
        QTextStream(stderr) << "Cannot create isolated fixture directory\n";
        return 2;
    }
    // 性能测量使用的临时 SQLite 文件路径。
    const auto database = temporary.filePath("history.db");
    // 汇总初始化、查询或报告写入失败的原因。
    QString error;
    // 在工作线程执行真实历史查询的仓储对象。
    auto *repository = new Repository;
    // 独立承载数据库连接和查询工作的线程。
    QThread databaseThread;
    databaseThread.setObjectName("HistoryBenchmarkDatabaseThread");
    repository->moveToThread(&databaseThread);
    QObject::connect(&databaseThread, &QThread::finished, repository, &QObject::deleteLater);
    databaseThread.start();
    // 在数据库线程关闭仓储并等待线程退出的清理操作。
    const auto shutdown = [&]
    {
        QMetaObject::invokeMethod(
            repository,
            // 在仓储所属线程完成连接关闭并退出事件循环。
            [repository]
            {
                repository->shutdown();
                QThread::currentThread()->quit();
            },
            Qt::QueuedConnection);
        databaseThread.wait();
    };
    // 记录仓储初始化是否成功。
    bool initialized = false;
    // 等待数据库初始化回报的主线程事件循环。
    QEventLoop initializeLoop;
    // 限制等待仓储初始化的最长时间。
    QTimer initializeTimeout;
    initializeTimeout.setSingleShot(true);
    // 保存初始化信号连接，初始化完成后断开。
    const auto initializedConnection =
        QObject::connect(repository, &Repository::initialized, &initializeLoop,
                         // 用 ok 和 message 接收初始化结果并结束等待。
                         [&](bool ok, const QString &message)
                         {
                             initialized = ok;
                             error = message;
                             initializeLoop.quit();
                         });
    QObject::connect(&initializeTimeout, &QTimer::timeout, &initializeLoop,
                     // 初始化等待超时时记录原因并结束等待。
                     [&]
                     {
                         error = "Repository initialization timed out";
                         initializeLoop.quit();
                     });
    initializeTimeout.start(10000);
    // 将临时数据库的初始化请求派发到数据库线程。
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
    // 测量准备合成历史所需时间的计时器。
    QElapsedTimer fixtureClock;
    fixtureClock.start();
    // 记录合成历史是否成功生成并校验。
    const bool built = buildFixture(database, rows, &error);
    // 合成数据准备阶段消耗的毫秒数。
    const double fixtureMs = fixtureClock.nsecsElapsed() / 1e6;
    if (!built)
    {
        shutdown();
        QTextStream(stderr) << error << '\n';
        return 1;
    }
    // 数据库主文件占用的字节数。
    const qint64 mainBytes = QFileInfo(database).size();
    // 数据库及 WAL、SHM 附属文件合计的字节数。
    const qint64 totalBytes =
        mainBytes + QFileInfo(database + "-wal").size() + QFileInfo(database + "-shm").size();

    // exactCount、cursorCount 为两类查询次数，pageSize 为分页大小。
    constexpr int exactCount = 200, cursorCount = 100, pageSize = 100;
    // sample 记录测量进度，cursor 保存分页边界。
    // expectedRows、expectedFirstId 为本次查询的预期条数及首行编号。
    int sample = 0, cursor = 0, expectedRows = 0, expectedFirstId = 0;
    // 精确查询期望返回的产品 SN，分页查询时为空。
    QString expectedSn;
    // exactSamples、cursorSamples 保存查询耗时，timerGaps 保存事件循环间隔。
    QList<double> exactSamples, cursorSamples, timerGaps;
    // requestClock 测单次查询，measurementClock 测总耗时，eventClock 测事件循环间隔。
    QElapsedTimer requestClock, measurementClock, eventClock;
    // 运行查询测量并在完成或失败时退出的事件循环。
    QEventLoop measurements;
    // queryTimeout 限制单次查询时间，eventTimer 采样主线程响应间隔。
    QTimer queryTimeout, eventTimer;
    queryTimeout.setSingleShot(true);
    eventTimer.setInterval(10);
    // 标记全部预定查询是否已完成。
    bool completed = false;
    // 上一次事件循环采样相对测量起点的毫秒数。
    double previousTick = 0;
    // 记录 message 失败原因并终止本轮测量。
    const auto fail = [&](const QString &message)
    {
        error = message;
        measurements.quit();
    };
    // 负责串行派发下一次查询的回调。
    std::function<void()> nextQuery;
    nextQuery = [&]
    {
        if (sample >= exactCount + cursorCount)
        {
            completed = true;
            measurements.quit();
            return;
        }
        // 本次精确查询的 SN，分页查询时留空。
        QString filter;
        // 本次分页查询使用的记录编号上界。
        int before = 0;
        if (sample < exactCount)
        {
            // 用于分散精确查询位置的合成记录编号。
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
        // 在数据库线程按当前 SN 和游标执行历史查询。
        QMetaObject::invokeMethod(repository, [repository, filter, before]
                                  { repository->queryHistory(filter, before); });
    };
    QObject::connect(&queryTimeout, &QTimer::timeout, &measurements,
                     [&] { fail("A history query exceeded 5 seconds"); });
    QObject::connect(repository, &Repository::operationError, &measurements,
                     // 接收仓储的 message 错误并终止测量。
                     [&](const QString &message) { fail(message); });
    QObject::connect(repository, &Repository::historyReady, &measurements,
                     // 校验 page 查询页和 more 翻页状态并记录本次耗时。
                     [&](const QJsonArray &page, bool more)
                     {
                         // 当前历史查询从派发到收到结果的毫秒数。
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
                             // 用于校验身份、判定和步骤的单条历史记录。
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
                     // 采样主线程事件循环的实际定时器触发间隔。
                     [&]
                     {
                         // 当前事件循环采样相对起点的毫秒数。
                         const double current = eventClock.nsecsElapsed() / 1e6;
                         timerGaps.append(current - previousTick);
                         previousTick = current;
                     });
    measurementClock.start();
    eventClock.start();
    eventTimer.start();
    QTimer::singleShot(20, &measurements, nextQuery);
    measurements.exec();
    // 整个查询测量阶段消耗的毫秒数。
    const double measurementMs = measurementClock.nsecsElapsed() / 1e6;
    eventTimer.stop();
    queryTimeout.stop();
    // 汇总数据规模、查询延迟和响应性指标的最终报告。
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
    // 读取进程工作集和峰值工作集的系统计数结构。
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
