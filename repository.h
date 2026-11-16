#pragma once
#include <QObject>
#include <QSqlDatabase>
#include <QJsonObject>
#include <QJsonArray>
#include <QDate>

// Invalid dates leave that bound open. Both dates refer to the local calendar day.
struct HistoryFilter
{
    QString sn;
    QDate startDate, endDate;
    int stationId = 0;
    QString verdict;

    bool operator==(const HistoryFilter &other) const
    {
        return sn == other.sn && startDate == other.startDate && endDate == other.endDate &&
               stationId == other.stationId && verdict == other.verdict;
    }
};

// Every method runs on the database thread; no connection is shared with the GUI.
class Repository final : public QObject
{
    Q_OBJECT
  public:
    explicit Repository(QObject *parent = nullptr);
    ~Repository() override;
  public slots:
    void initialize(const QString &databasePath);
    void reserveRun(const QJsonObject &request);
    void saveResult(const QJsonObject &result);
    void queryHistory(const QString &snFilter, int beforeId = 0);
    void queryFilteredHistory(const HistoryFilter &filter, int beforeId = 0, quint64 requestId = 0);
    void exportReport(const QString &runId, const QString &path);
    void exportFailedSns(const QString &path);
    void listRecipes();
    void publishRecipe(const QJsonObject &recipe);
    void markInterrupted(const QString &runId, const QString &reason);
    void shutdown();
  signals:
    void recoverableRuns(const QJsonArray &requests);
    void runInterrupted(const QString &runId, bool ok, const QString &error);
    void initialized(bool ok, const QString &error);
    void runReserved(const QJsonObject &request, bool ok, const QString &error);
    void resultSaved(const QJsonObject &result, bool ok, bool duplicate, const QString &error);
    void historyReady(const QJsonArray &rows, bool more);
    void historyQueryReady(quint64 requestId, const QJsonArray &rows, bool more);
    void historyQueryFailed(quint64 requestId, const QString &error);
    void reportReady(const QString &path, bool ok, const QString &error);
    void failedSnsExported(const QString &path, bool ok, qint64 count, const QString &error);
    void recipesReady(const QJsonArray &recipes);
    void operationError(const QString &message);

  private:
    bool checkThread() const;
    bool ready(QString *error) const;
    bool begin(QString *error);
    bool commit(QString *error);
    QJsonArray loadSteps(const QString &runId, QString *error);
    QSqlDatabase m_db;
    QString m_connectionName;
    bool m_ready = false;
};
