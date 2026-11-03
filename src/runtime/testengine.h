#pragma once
#include "workbenchcontroller.h"
#include "protocol/framecodec.h"
#include <QElapsedTimer>
#include <QHash>
#include <memory>
#include <map>
class QTcpSocket;
class QTimer;

class TestEngine final : public QObject
{
    Q_OBJECT
  public:
    explicit TestEngine(QObject *parent = nullptr);
    void initialize(const QList<StationEndpoint> &endpoints, const QJsonArray &recovery = {});
    void requestStart(const QJsonObject &run);
    void previewRecipe(int stationId, const QJsonObject &recipe);
    void reserved(const QJsonObject &run, bool ok, const QString &error);
    void resultSaved(const QJsonObject &result, bool ok, bool duplicate, const QString &error);
    void interruptionRecorded(const QString &runId, bool ok, const QString &error);
    void cancel(int stationId);
    void shutdown();
  signals:
    void stationUpdated(const QJsonObject &view);
    void logMessage(const QString &message, bool warning);
    void reserveRequested(const QJsonObject &run);
    void saveRequested(const QJsonObject &result);
    void interrupted(const QString &runId, const QString &reason);

  private:
    struct Link
    {
        StationEndpoint endpoint;
        QTcpSocket *socket = nullptr;
        Wire::Parser parser;
        QString session, remoteRun, ackPending;
        QJsonObject run, view, archivedResult, recipePreview;
        QJsonArray progress;
        QString interruptionReason;
        qint64 interruptionRetryAt = 0;
        qint64 reconnectAt = 0, connectStarted = 0, lastRx = 0, lastPing = 0, startDeadline = 0,
               queryDeadline = 0, ackDeadline = 0;
        quint32 sequence = 0, startSequence = 0;
        int backoff = 1000;
        bool online = false, dispatched = false, awaitingAck = false, cancelling = false,
             interrupting = false, interruptionWriting = false, remoteUnknown = false;
    };
    struct PendingResult
    {
        QJsonObject result;
        bool writing = false;
        qint64 retryAt = 0;
    };
    std::map<int, std::unique_ptr<Link>> m_links;
    QHash<QString, PendingResult> m_results;
    QElapsedTimer m_clock;
    QTimer *m_timer = nullptr;
    bool m_stopping = false;
    void tick();
    quint32 send(Link &link, const QString &kind, QJsonObject extra = {});
    void read(Link &link);
    void receive(Link &link, const QJsonObject &message);
    void terminal(Link &link, const QJsonObject &result);
    void query(Link &link, const QString &runId);
    void publish(Link &link, const QString &execution = {}, const QString &detail = {});
    void clearUnknown(Link &link, const QString &reason);
};
