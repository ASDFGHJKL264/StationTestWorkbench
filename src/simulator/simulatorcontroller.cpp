#include "simulatorcontroller.h"
#include <QMap>
#include <QThread>
#include <QDebug>

class SimulatorWorker final : public QObject
{
    Q_OBJECT
  public:
    explicit SimulatorWorker(QObject *parent = nullptr) : QObject(parent) {}
    ~SimulatorWorker() override
    {
        shutdown();
    }

    void startStation(const StationOptions &options)
    {
        if (options.stationId < 1 || options.stationId > 4)
        {
            emit operationFinished(options.stationId, "start", false, tr("工位编号必须为 1～4。"));
            return;
        }
        auto *existing = m_servers.value(options.stationId);
        if (existing && existing->snapshot()["listening"].toBool())
        {
            emit operationFinished(options.stationId, "start", false, tr("工位已经启动。"));
            return;
        }
        delete m_servers.take(options.stationId);
        auto *server = new StationServer(options, this);
        m_servers.insert(options.stationId, server);
        connect(server, &StationServer::snapshotChanged, this, &SimulatorWorker::stationChanged);
        connect(server, &StationServer::diagnostic, this,
                [this, id = options.stationId](const QString &message)
                { emit diagnostic(id, message.left(2048)); });
        QString error;
        const bool ok = server->start(&error);
        emit stationChanged(server->snapshot());
        emit operationFinished(options.stationId, "start", ok, error);
    }

    void stopStation(int id)
    {
        auto *server = m_servers.value(id);
        if (server)
        {
            server->stop();
            emit stationChanged(server->snapshot());
        }
        emit operationFinished(id, "stop", true, {});
    }

    void disconnectStation(int id)
    {
        auto *server = m_servers.value(id);
        if (!server || !server->snapshot()["connected"].toBool())
        {
            emit operationFinished(id, "disconnect", false, tr("当前没有已连接的客户端。"));
            return;
        }
        server->disconnectClient();
        emit stationChanged(server->snapshot());
        emit operationFinished(id, "disconnect", true, {});
    }

    void configureFaults(int id, const QString &failStep, int stepMs, bool dropFirstResultAck)
    {
        auto *server = m_servers.value(id);
        QString error;
        const bool ok =
            server && server->setFaultOptions(failStep, stepMs, dropFirstResultAck, &error);
        if (!server)
            error = tr("请先启动工位。");
        if (server)
            emit stationChanged(server->snapshot());
        emit operationFinished(id, "configure", ok, error);
    }

    void shutdown()
    {
        const auto servers = m_servers;
        m_servers.clear();
        qDeleteAll(servers);
    }

  signals:
    void stationChanged(const QJsonObject &snapshot);
    void diagnostic(int stationId, const QString &message);
    void operationFinished(int stationId, const QString &operation, bool ok, const QString &error);

  private:
    QMap<int, StationServer *> m_servers;
};

SimulatorController::SimulatorController(QObject *parent)
    : QObject(parent), m_thread(new QThread), m_worker(new SimulatorWorker)
{
    m_thread->setObjectName("SimulatorDeviceWorker");
    m_worker->moveToThread(m_thread);
    connect(m_worker, &SimulatorWorker::stationChanged, this, &SimulatorController::stationChanged);
    connect(m_worker, &SimulatorWorker::diagnostic, this, &SimulatorController::diagnostic);
    connect(m_worker, &SimulatorWorker::operationFinished, this,
            &SimulatorController::operationFinished);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    m_thread->start();
}

SimulatorController::~SimulatorController()
{
    // The cleanup command captures only worker-owned objects. If storage stalls,
    // the independently owned thread finishes cleanup later, without touching UI objects.
    auto *worker = m_worker;
    QMetaObject::invokeMethod(
        worker,
        [worker]
        {
            worker->shutdown();
            QThread::currentThread()->quit();
        },
        Qt::QueuedConnection);
    if (m_thread->wait(5000))
        delete m_thread;
    else
        qWarning() << "Simulator worker is still completing persistent device shutdown.";
}

void SimulatorController::startStation(const StationOptions &options)
{
    auto *worker = m_worker;
    QMetaObject::invokeMethod(
        worker, [worker, options] { worker->startStation(options); }, Qt::QueuedConnection);
}

void SimulatorController::stopStation(int stationId)
{
    auto *worker = m_worker;
    QMetaObject::invokeMethod(
        worker, [worker, stationId] { worker->stopStation(stationId); }, Qt::QueuedConnection);
}

void SimulatorController::disconnectStation(int stationId)
{
    auto *worker = m_worker;
    QMetaObject::invokeMethod(
        worker, [worker, stationId] { worker->disconnectStation(stationId); },
        Qt::QueuedConnection);
}

void SimulatorController::configureFaults(int stationId, const QString &failStep, int stepMs,
                                          bool dropFirstResultAck)
{
    auto *worker = m_worker;
    QMetaObject::invokeMethod(
        worker, [worker, stationId, failStep, stepMs, dropFirstResultAck]
        { worker->configureFaults(stationId, failStep, stepMs, dropFirstResultAck); },
        Qt::QueuedConnection);
}

#include "simulatorcontroller.moc"
