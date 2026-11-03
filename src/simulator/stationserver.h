#pragma once
#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QTcpServer>
#include <QTimer>
#include "protocol/framecodec.h"

class QTcpSocket;
struct StationOptions
{
    int stationId = 1;
    quint16 port = 46501; // zero selects a free port, useful for isolated tests
    QString dataDirectory;
    int stepMs = 200;
    QString failStep;
    bool dropFirstResultAck = false;
};
class StationServer final : public QObject
{
    Q_OBJECT
  public:
    explicit StationServer(const StationOptions &options, QObject *parent = nullptr);
    ~StationServer() override;
    bool start(QString *error = nullptr);
    quint16 port() const;
    void stop();
    // These APIs are called on the server's owning thread. GUI consumers use queued delivery.
    QJsonObject snapshot() const;
    bool setFaultOptions(const QString &failStep, int stepMs, bool dropFirstResultAck,
                         QString *error = nullptr);
    void disconnectClient();
  signals:
    void diagnostic(const QString &message);
    void snapshotChanged(const QJsonObject &snapshot);

  private:
    void report(const QString &message);
    bool faultsEditable() const;
    void acceptClient();
    void readClient();
    void receive(const QJsonObject &message);
    void send(const QString &kind, QJsonObject object = {}, double sequence = 0);
    bool persist(const QJsonObject &state, QString *error = nullptr);
    bool load(QString *error);
    void advanceStep();
    void finish(const QString &outcome);
    void resendTerminal();
    StationOptions m_options;
    QTcpServer m_server;
    QTimer m_stepTimer, m_resendTimer;
    QTcpSocket *m_client = nullptr;
    Wire::Parser m_parser;
    QString m_session, m_file, m_pendingOutcome;
    QJsonObject m_state;
    QJsonObject m_recipePreview; // Non-executing UI preview, never replaces a stored run.
    quint32 m_sequence = 0;
    bool m_ignoredAck = false;
};
