#pragma once
#include "stationendpoint.h"
#include <QObject>
#include <QThread>
#include <QJsonObject>
#include <QJsonArray>
#include <QList>

class TestEngine;
class Repository;
class AsyncFileLogger;

class WorkbenchController final : public QObject
{
    Q_OBJECT
  public:
    explicit WorkbenchController(QObject *parent = nullptr);
    ~WorkbenchController() override;
    void initialize(const QString &dataDirectory, const QList<StationEndpoint> &endpoints);
    void shutdown();
  public slots:
    void startTest(int stationId, const QString &sn, const QString &recipeVersion);
    void cancelTest(int stationId);
    void refreshHistory(const QString &snFilter = {}, int beforeId = 0);
    void publishRecipe(const QJsonObject &recipe);
    void previewRecipe(int stationId, const QString &recipeVersion);
    void requestRecipes();
    void exportReport(const QString &runId, const QString &path);
    void exportFailedSns(const QString &path);
  signals:
    void stationUpdated(const QJsonObject &station);
    void historyReady(const QJsonArray &rows, bool more);
    void recipesReady(const QJsonArray &recipes);
    void logMessage(const QString &message, bool warning = false);
    void ready(bool ok);
    void reportReady(const QString &path, bool ok, const QString &error);
    void failedSnsExported(const QString &path, bool ok, qint64 count, const QString &error);

  private:
    void log(const QString &text, bool warning = false);
    QThread m_engineThread, m_databaseThread;
    TestEngine *m_engine = nullptr;
    Repository *m_repository = nullptr;
    AsyncFileLogger *m_logger = nullptr;
    QJsonArray m_recipes;
    QJsonArray m_recovery;
    QString m_historyFilter;
    int m_historyCursor = 0;
    bool m_started = false, m_ready = false, m_stopping = false;
};
