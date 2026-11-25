#include "workbenchcontroller.h"
#include "testengine.h"
#include "repository.h"
#include "asyncfilelogger.h"
#include "domain.h"
#include "adminaccess.h"
#include <QTimer>
#include <QFile>
#include <QJsonDocument>
#include <QDir>
#include <QDateTime>
#include <QUuid>
#include <QCoreApplication>
#include <QEvent>

WorkbenchController::WorkbenchController(QObject *parent) : QObject(parent)
{
    setObjectName("workbenchController");
}
WorkbenchController::~WorkbenchController()
{
    shutdown();
}

void WorkbenchController::initialize(const QString &dataDirectory,
                                     const QList<StationEndpoint> &endpoints)
{
    if (m_started || m_stopping)
        return;
    m_started = true;
    m_dataDirectory = QDir(dataDirectory).absolutePath();
    m_databasePath = QDir(m_dataDirectory).filePath("data/workbench.db");
    m_backupDirectory = QDir(m_dataDirectory).filePath("backups");
    // 读取本地时限设置，损坏设置使用有界默认值。
    QString settingsError;
    m_executionLimits = RuntimeConfiguration::loadLimits(m_dataDirectory, &settingsError);
    m_logger = new AsyncFileLogger(QDir(dataDirectory).filePath("logs/workbench.log"), this);
    // 将日志器回报的 error 文件错误转发到界面。
    connect(m_logger, &AsyncFileLogger::writeFailed, this, [this](const QString &error)
            { emit logMessage(QStringLiteral("日志文件错误：") + error, true); });
    // 接收过载计数：a 为普通日志丢弃条数，b 为警告丢弃条数。
    connect(m_logger, &AsyncFileLogger::overflow, this,
            [this](quint64 a, quint64 b)
            {
                emit logMessage(QStringLiteral("日志过载：普通%1条、警告%2条未保存").arg(a).arg(b),
                                true);
            });
    m_engine = new TestEngine;
    m_repository = new Repository;
    m_engineThread.setObjectName("WorkbenchNetworkThread");
    m_databaseThread.setObjectName("WorkbenchDatabaseThread");
    m_engine->moveToThread(&m_engineThread);
    m_repository->moveToThread(&m_databaseThread);
    initializeMaintenance();
    connect(&m_engineThread, &QThread::finished, m_engine, &QObject::deleteLater);
    connect(&m_databaseThread, &QThread::finished, m_repository, &QObject::deleteLater);
    connect(m_engine, &TestEngine::stationUpdated, this,
            [this](const QJsonObject &view)
            {
                m_stationViews.insert(view["stationId"].toInt(), view);
                emit stationUpdated(view);
            });
    connect(m_engine, &TestEngine::logMessage, this, &WorkbenchController::log);
    connect(m_engine, &TestEngine::reserveRequested, m_repository, &Repository::reserveRun);
    connect(m_repository, &Repository::runReserved, m_engine, &TestEngine::reserved);
    connect(m_engine, &TestEngine::saveRequested, m_repository, &Repository::saveResult);
    connect(m_repository, &Repository::resultSaved, m_engine, &TestEngine::resultSaved);
    connect(m_engine, &TestEngine::interrupted, m_repository, &Repository::markInterrupted);
    connect(m_repository, &Repository::runInterrupted, m_engine, &TestEngine::interruptionRecorded);
    // 接收 runs 待恢复请求列表，缓存后交给测试引擎。
    connect(m_repository, &Repository::recoverableRuns, this,
            [this](const QJsonArray &runs) { m_recovery = runs; });
    // 将数据库回报的 message 失败原因写入业务日志。
    connect(m_repository, &Repository::operationError, this,
            [this](const QString &message) { log(message, true); });
    // 按 requestId 核对查询新旧，转发 rows 历史页和 more 下一页标志。
    connect(m_repository, &Repository::historyQueryReady, this,
            [this](quint64 requestId, const QJsonArray &rows, bool more)
            {
                if (requestId == m_historyRequestId)
                    emit historyReady(rows, more);
            });
    // 仅转发当前 requestId 对应的 error 查询失败原因。
    connect(m_repository, &Repository::historyQueryFailed, this,
            [this](quint64 requestId, const QString &error)
            {
                if (requestId == m_historyRequestId)
                    emit historyFailed(error);
            });
    // 接收 path 报告导出路径、ok 成功状态和 error 失败原因。
    connect(m_repository, &Repository::reportReady, this,
            [this](const QString &path, bool ok, const QString &error)
            {
                emit reportReady(path, ok, error);
                log(ok ? QStringLiteral("测试报告已导出：") + path
                       : QStringLiteral("测试报告导出失败：") + error,
                    !ok);
            });
    // 接收 path 清单路径、ok 成功状态、count 导出数量和 error 失败原因。
    connect(m_repository, &Repository::failedSnsExported, this,
            [this](const QString &path, bool ok, qint64 count, const QString &error)
            {
                emit failedSnsExported(path, ok, count, error);
                log(ok ? QStringLiteral("异常 SN 清单已导出，共 %1 个：%2").arg(count).arg(path)
                       : QStringLiteral("异常 SN 导出失败：") + error,
                    !ok);
            });
    // 将 recipes 配方列表缓存并通知界面更新选择。
    connect(m_repository, &Repository::recipesReady, this,
            [this](const QJsonArray &recipes)
            {
                m_recipes = recipes;
                emit recipesReady(recipes);
            });
    // 接收初始化的 ok 和 error，并捕获 endpoints 以启动网络连接。
    connect(
        m_repository, &Repository::initialized, this,
        [this, endpoints](bool ok, const QString &error)
        {
            if (m_stopping)
                return;
            m_ready = ok;
            if (ok)
            {
                // 捕获 e 引擎指针、endpoints 配置和 recovery 恢复请求副本，交给网络线程。
                QMetaObject::invokeMethod(m_engine,
                                          [e = m_engine, endpoints, recovery = m_recovery,
                                           closed = m_closedRuns, suspended = m_suspendedStations,
                                           limits = m_executionLimits]
                                          {
                                              e->configureTimeouts(limits.runSeconds * 1000LL,
                                                                   limits.stepSeconds * 1000LL);
                                              e->loadSafetyState(closed, suspended);
                                              e->initialize(endpoints, recovery);
                                          });
                requestRecipes();
                refreshHistory();
                log(QStringLiteral("数据库就绪；工位测试使用模拟数据，配方限值仅为演示"));
                // 在恢复后的库中记录打开事件，原库不可读时仍有恢复来源可查。
                QFile restored(QDir(m_dataDirectory).filePath("maintenance/restored-from.json"));
                if (restored.open(QIODevice::ReadOnly) && restored.size() <= 65536)
                {
                    // 读取恢复操作者和原数据目录。
                    const auto metadata = QJsonDocument::fromJson(restored.readAll()).object();
                    if (metadata["version"].toInt() == 1)
                        audit(QStringLiteral("系统"), "RESTORED_DATABASE_OPENED",
                              QStringLiteral("恢复操作者：%1；原目录：%2；切换时间：%3")
                                  .arg(metadata["actor"].toString(),
                                       metadata["sourceDirectory"].toString(),
                                       metadata["activatedAt"].toString()));
                }
                QTimer::singleShot(500, this, &WorkbenchController::checkAutomaticMaintenance);
            }
            else
                log(QStringLiteral("后台初始化失败：") + error, true);
            emit ready(ok);
        });
    // 根据 ok 归档状态和 duplicate 重复标志，决定是否刷新历史首页。
    connect(m_repository, &Repository::resultSaved, this,
            [this](const QJsonObject &, bool ok, bool duplicate, const QString &)
            {
                if (ok && !duplicate && !m_stopping && m_historyCursor == 0)
                    refreshFilteredHistory(m_historyFilter);
            });
    m_engineThread.start();
    m_databaseThread.start();
    // 生成当前数据目录下的 SQLite 数据库完整路径。
    const auto database = QDir(dataDirectory).filePath("data/workbench.db");
    // 捕获 r 存储对象和 database 路径，在数据库线程初始化连接。
    QMetaObject::invokeMethod(m_repository,
                              [r = m_repository, database] { r->initialize(database); });
    if (!settingsError.isEmpty())
        log(settingsError, true);
}

void WorkbenchController::startTest(int stationId, const QString &sn, const QString &version)
{
    if (!m_ready || m_stopping || m_restoreInFlight)
    {
        log(QStringLiteral("后台尚未就绪或正在恢复数据库，暂不能开始测试"), true);
        return;
    }
    // 保存按用户所选版本找到的已发布配方。
    QJsonObject recipe;
    for (const auto &v : m_recipes)
        if (v.toObject()["version"] == version)
        {
            recipe = v.toObject();
            break;
        }
    // 绑定新 runId、工位、SN 和配方快照，形成一次测试请求。
    QJsonObject run{{"runId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                    {"stationId", stationId},
                    {"sn", sn.trimmed()},
                    {"recipeVersion", version},
                    {"recipe", recipe}};
    // 接收测试请求校验失败的具体原因。
    QString error;
    if (!Domain::validateRun(run, &error))
    {
        log(error, true);
        return;
    }
    // 捕获 e 引擎指针和 run 请求副本，排队到网络线程申请开始。
    QMetaObject::invokeMethod(m_engine, [e = m_engine, run] { e->requestStart(run); });
}

void WorkbenchController::previewRecipe(int stationId, const QString &version)
{
    if (!m_ready || m_stopping || stationId < 1 || stationId > 4)
        return;
    for (const auto &value : m_recipes)
    {
        // 保存当前候选配方，供版本匹配后发送预览。
        const auto recipe = value.toObject();
        if (recipe["version"].toString() != version)
            continue;
        // 捕获 engine 引擎指针和配方副本，在网络线程更新工位预览。
        QMetaObject::invokeMethod(m_engine, [engine = m_engine, stationId, recipe]
                                  { engine->previewRecipe(stationId, recipe); });
        return;
    }
}

void WorkbenchController::cancelTest(int id)
{
    if (m_ready && !m_stopping)
        // 捕获 e 引擎指针和 id 工位编号，在网络线程申请取消。
        QMetaObject::invokeMethod(m_engine, [e = m_engine, id] { e->cancel(id); });
}
void WorkbenchController::retryReconciliation(int stationId)
{
    if (m_ready && !m_stopping)
        // 捕获 engine 引擎指针和工位编号，排队进行手动状态核对。
        QMetaObject::invokeMethod(m_engine, [engine = m_engine, stationId]
                                  { engine->retryReconciliation(stationId); });
}
void WorkbenchController::refreshHistory(const QString &snFilter, int beforeId)
{
    // 将旧的单一 SN 查询参数包装为复合筛选条件。
    HistoryFilter filter;
    filter.sn = snFilter;
    refreshFilteredHistory(filter, beforeId);
}
void WorkbenchController::refreshFilteredHistory(const HistoryFilter &filter, int beforeId)
{
    if (!m_ready || m_stopping)
        return;
    m_historyFilter = filter;
    m_historyCursor = qMax(0, beforeId);
    // 为本次查询生成递增标识，用于屏蔽过时回报。
    const auto requestId = ++m_historyRequestId;
    // 捕获 r 存储对象、filter 条件、beforeId 游标和 requestId 标识，在数据库线程查询。
    QMetaObject::invokeMethod(m_repository, [r = m_repository, filter, beforeId, requestId]
                              { r->queryFilteredHistory(filter, beforeId, requestId); });
}
void WorkbenchController::requestRecipes()
{
    if (m_ready && !m_stopping)
        QMetaObject::invokeMethod(m_repository, &Repository::listRecipes);
}
void WorkbenchController::exportReport(const QString &runId, const QString &path)
{
    if (!m_ready || m_stopping)
    {
        emit reportReady(path, false, QStringLiteral("后台尚未就绪"));
        return;
    }
    // 捕获 r 存储对象、runId 测试编号和 path 路径，排队导出报告。
    QMetaObject::invokeMethod(m_repository,
                              [r = m_repository, runId, path] { r->exportReport(runId, path); });
}
void WorkbenchController::exportFailedSns(const QString &path)
{
    if (!m_ready || m_stopping)
    {
        emit failedSnsExported(path, false, 0, QStringLiteral("后台尚未就绪"));
        return;
    }
    // 捕获 r 存储对象和 path 路径，排队导出历史异常 SN 清单。
    QMetaObject::invokeMethod(m_repository, [r = m_repository, path] { r->exportFailedSns(path); });
}
void WorkbenchController::publishRecipe(const QJsonObject &recipe)
{
    if (!m_ready || m_stopping)
        return;
    // 捕获 r 存储对象和 recipe 配方副本，发布后重新读取可选列表。
    QMetaObject::invokeMethod(m_repository,
                              [r = m_repository, recipe]
                              {
                                  r->publishRecipe(recipe);
                                  r->listRecipes();
                              });
}
void WorkbenchController::log(const QString &message, bool warning)
{
    emit logMessage(message, warning);
    if (m_logger)
        m_logger->append(
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) + " " + message, warning);
}
void WorkbenchController::shutdown()
{
    if (m_stopping)
        return;
    m_stopping = true;
    m_ready = false;
    emit ready(false);
    if (m_maintenanceTimer)
        m_maintenanceTimer->stop();
    if (m_engineThread.isRunning())
        QMetaObject::invokeMethod(m_engine, &TestEngine::shutdown, Qt::BlockingQueuedConnection);
    if (m_maintenanceThread.isRunning())
    {
        m_maintenanceThread.quit();
        m_maintenanceThread.wait();
        m_maintenance = nullptr;
    }
    if (m_databaseThread.isRunning())
    {
        // 捕获 r 存储对象，在数据库线程关闭连接后退出该线程。
        QMetaObject::invokeMethod(m_repository,
                                  [r = m_repository]
                                  {
                                      r->shutdown();
                                      QThread::currentThread()->quit();
                                  });
        m_databaseThread.wait();
        m_repository = nullptr;
    }
    if (m_engineThread.isRunning())
    {
        // 排队退出网络线程，确保已排队的清理操作先执行。
        QMetaObject::invokeMethod(m_engine, [] { QThread::currentThread()->quit(); });
        m_engineThread.wait();
        m_engine = nullptr;
    }
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    if (m_logger)
        m_logger->shutdown();
}
