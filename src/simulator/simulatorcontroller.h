#pragma once
#include "stationserver.h"
#include <QObject>

class QThread;
class SimulatorWorker;

// All device I/O and persistent state writes run in the worker thread.
class SimulatorController final : public QObject
{
    Q_OBJECT
  public:
    explicit SimulatorController(QObject *parent = nullptr);
    ~SimulatorController() override;

    void startStation(const StationOptions &options);
    void stopStation(int stationId);
    void disconnectStation(int stationId);
    void configureFaults(int stationId, const QString &failStep, int stepMs,
                         bool dropFirstResultAck);

  signals:
    void stationChanged(const QJsonObject &snapshot);
    void diagnostic(int stationId, const QString &message);
    void operationFinished(int stationId, const QString &operation, bool ok, const QString &error);

  private:
    QThread *m_thread;
    SimulatorWorker *m_worker;
};
