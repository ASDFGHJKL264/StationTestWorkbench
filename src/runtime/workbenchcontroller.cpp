#include "workbenchcontroller.h"
#include "testengine.h"
#include "storage/repository.h"
#include "asyncfilelogger.h"
#include "core/domain.h"
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
    m_logger = new AsyncFileLogger(QDir(dataDirectory).filePath("logs/workbench.log"), this);
    connect(m_logger, &AsyncFileLogger::writeFailed, this, [this](const QString &error)
            { emit logMessage(QStringLiteral("日志文件错误：") + error, true); });
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
    connect(&m_engineThread, &QThread::finished, m_engine, &QObject::deleteLater);
    connect(&m_databaseThread, &QThread::finished, m_repository, &QObject::deleteLater);
    connect(m_engine, &TestEngine::stationUpdated, this, &WorkbenchController::stationUpdated);
    connect(m_engine, &TestEngine::logMessage, this, &WorkbenchController::log);
    connect(m_engine, &TestEngine::reserveRequested, m_repository, &Repository::reserveRun);
    connect(m_repository, &Repository::runReserved, m_engine, &TestEngine::reserved);
    connect(m_engine, &TestEngine::saveRequested, m_repository, &Repository::saveResult);
    connect(m_repository, &Repository::resultSaved, m_engine, &TestEngine::resultSaved);
    connect(m_engine, &TestEngine::interrupted, m_repository, &Repository::markInterrupted);
    connect(m_repository, &Repository::runInterrupted, m_engine, &TestEngine::interruptionRecorded);
    connect(m_repository, &Repository::recoverableRuns, this,
            [this](const QJsonArray &runs) { m_recovery = runs; });
    connect(m_repository, &Repository::operationError, this,
            [this](const QString &message) { log(message, true); });
    connect(m_repository, &Repository::historyReady, this, &WorkbenchController::historyReady);
    connect(m_repository, &Repository::reportReady, this,
            [this](const QString &path, bool ok, const QString &error)
            {
                emit reportReady(path, ok, error);
                log(ok ? QStringLiteral("测试报告已导出：") + path
                       : QStringLiteral("测试报告导出失败：") + error,
                    !ok);
            });
    connect(m_repository, &Repository::recipesReady, this,
            [this](const QJsonArray &recipes)
            {
                m_recipes = recipes;
                emit recipesReady(recipes);
            });
    connect(m_repository, &Repository::initialized, this,
            [this, endpoints](bool ok, const QString &error)
            {
                if (m_stopping)
                    return;
                m_ready = ok;
                if (ok)
                {
                    QMetaObject::invokeMethod(m_engine,
                                              [e = m_engine, endpoints, recovery = m_recovery]
                                              { e->initialize(endpoints, recovery); });
                    requestRecipes();
                    refreshHistory();
                    log(QStringLiteral("数据库就绪；工位测试使用模拟数据，配方限值仅为演示"));
                }
                else
                    log(QStringLiteral("后台初始化失败：") + error, true);
                emit ready(ok);
            });
    connect(m_repository, &Repository::resultSaved, this,
            [this](const QJsonObject &, bool ok, bool duplicate, const QString &)
            {
                if (ok && !duplicate && !m_stopping && m_historyCursor == 0)
                    refreshHistory(m_historyFilter);
            });
    m_engineThread.start();
    m_databaseThread.start();
    const auto database = QDir(dataDirectory).filePath("data/workbench.db");
    QMetaObject::invokeMethod(m_repository,
                              [r = m_repository, database] { r->initialize(database); });
}

void WorkbenchController::startTest(int stationId, const QString &sn, const QString &version)
{
    if (!m_ready || m_stopping)
    {
        log(QStringLiteral("后台尚未就绪"), true);
        return;
    }
    QJsonObject recipe;
    for (const auto &v : m_recipes)
        if (v.toObject()["version"] == version)
        {
            recipe = v.toObject();
            break;
        }
    QJsonObject run{{"runId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                    {"stationId", stationId},
                    {"sn", sn.trimmed()},
                    {"recipeVersion", version},
                    {"recipe", recipe}};
    QString error;
    if (!Domain::validateRun(run, &error))
    {
        log(error, true);
        return;
    }
    QMetaObject::invokeMethod(m_engine, [e = m_engine, run] { e->requestStart(run); });
}

void WorkbenchController::previewRecipe(int stationId, const QString &version)
{
    if (!m_ready || m_stopping || stationId < 1 || stationId > 4)
        return;
    for (const auto &value : m_recipes)
    {
        const auto recipe = value.toObject();
        if (recipe["version"].toString() != version)
            continue;
        QMetaObject::invokeMethod(m_engine, [engine = m_engine, stationId, recipe]
                                  { engine->previewRecipe(stationId, recipe); });
        return;
    }
}

void WorkbenchController::cancelTest(int id)
{
    if (m_ready && !m_stopping)
        QMetaObject::invokeMethod(m_engine, [e = m_engine, id] { e->cancel(id); });
}
void WorkbenchController::refreshHistory(const QString &filter, int beforeId)
{
    if (!m_ready || m_stopping)
        return;
    m_historyFilter = filter;
    m_historyCursor = qMax(0, beforeId);
    QMetaObject::invokeMethod(m_repository, [r = m_repository, filter, beforeId]
                              { r->queryHistory(filter, beforeId); });
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
    QMetaObject::invokeMethod(m_repository,
                              [r = m_repository, runId, path] { r->exportReport(runId, path); });
}
void WorkbenchController::publishRecipe(const QJsonObject &recipe)
{
    if (!m_ready || m_stopping)
        return;
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
    if (m_engineThread.isRunning())
        QMetaObject::invokeMethod(m_engine, &TestEngine::shutdown, Qt::BlockingQueuedConnection);
    if (m_databaseThread.isRunning())
    {
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
        QMetaObject::invokeMethod(m_engine, [] { QThread::currentThread()->quit(); });
        m_engineThread.wait();
        m_engine = nullptr;
    }
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    if (m_logger)
        m_logger->shutdown();
}
