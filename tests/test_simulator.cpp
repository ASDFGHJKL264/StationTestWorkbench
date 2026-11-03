#include <QtTest>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QUuid>
#include <algorithm>
#include <memory>
#include "simulator/stationserver.h"
#include "core/domain.h"

namespace
{
struct Client
{
    QTcpSocket socket;
    Wire::Parser parser;
    QList<QJsonObject> messages;
    QString session = QUuid::createUuid().toString(QUuid::WithoutBraces);
    int sequence = 0;
    Client()
    {
        QObject::connect(&socket, &QTcpSocket::readyRead, &socket,
                         [this] { messages += parser.append(socket.readAll()); });
    }
    void connectTo(quint16 port)
    {
        socket.connectToHost(QHostAddress::LocalHost, port);
    }
    void send(QString kind, QJsonObject message = {})
    {
        message["kind"] = kind;
        message["session"] = session;
        message["sequence"] = ++sequence;
        socket.write(Wire::encode(message));
    }
    int count(const QString &kind) const
    {
        return int(std::count_if(messages.cbegin(), messages.cend(),
                                 [&](const QJsonObject &m) { return m["kind"] == kind; }));
    }
    QJsonObject last(const QString &kind) const
    {
        for (auto it = messages.crbegin(); it != messages.crend(); ++it)
            if ((*it)["kind"] == kind)
                return *it;
        return {};
    }
};
QJsonObject makeRun(const QString &id = "test-run-1")
{
    const auto recipe = Domain::defaultRecipe();
    return {{"runId", id},
            {"stationId", 1},
            {"sn", "SIM-SN-001"},
            {"recipeVersion", recipe["version"]},
            {"recipe", recipe}};
}
QJsonObject previewRecipe(const QString &version, bool temperature = true)
{
    QJsonArray steps;
    if (temperature)
        steps.append(QJsonObject{{"id", "temperature"},
                                 {"label", QStringLiteral("温度")},
                                 {"unit", "C"},
                                 {"min", 15},
                                 {"max", 35},
                                 {"nominal", 25}});
    steps.append(QJsonObject{{"id", "pressure"},
                             {"label", QStringLiteral("压力")},
                             {"unit", "kPa"},
                             {"min", 90},
                             {"max", 110},
                             {"nominal", 100}});
    return {{"version", version}, {"name", QStringLiteral("环境检测")}, {"steps", steps}};
}
QJsonObject diskState(const QString &directory)
{
    QFile file(directory + "/station-1.json");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
} // namespace
class SimulatorTests : public QObject
{
    Q_OBJECT
  private slots:
    void duplicateStartsAndDurableAck()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StationOptions options;
        options.dataDirectory = dir.path();
        options.port = 0;
        options.stepMs = 80;
        StationServer server(options);
        QString error;
        QVERIFY2(server.start(&error), qPrintable(error));
        Client c;
        c.connectTo(server.port());
        QTRY_COMPARE(c.socket.state(), QAbstractSocket::ConnectedState);
        c.send("hello");
        QTRY_COMPARE(c.count("helloAck"), 1);
        const auto run = makeRun();
        c.send("start", {{"run", run}});
        c.send("start", {{"run", run}});
        QTRY_COMPARE(c.count("accepted"), 2);
        QVERIFY(c.last("accepted")["accepted"].toBool());
        QTRY_VERIFY(c.count("terminal") >= 1);
        QCOMPARE(c.count("progress"), 3);
        QCOMPARE(diskState(dir.path())["state"].toString(), "terminal");
        c.send("start", {{"run", run}});
        QTRY_COMPARE(c.count("accepted"), 3);
        QCOMPARE(c.count("progress"), 3);
        c.send("resultAck", {{"runId", run["runId"]}});
        QTRY_COMPARE(c.count("resultConfirmed"), 1);
        QCOMPARE(diskState(dir.path())["state"].toString(), "acked");
        c.send("resultAck", {{"runId", run["runId"]}});
        QTRY_COMPARE(c.count("resultConfirmed"), 2);
    }
    void terminalSurvivesRestartAndLostAck()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StationOptions options;
        options.dataDirectory = dir.path();
        options.port = 0;
        options.stepMs = 1;
        options.dropFirstResultAck = true;
        auto server = std::make_unique<StationServer>(options);
        QVERIFY(server->start());
        Client c;
        c.connectTo(server->port());
        QTRY_COMPARE(c.socket.state(), QAbstractSocket::ConnectedState);
        c.send("hello");
        QTRY_COMPARE(c.count("helloAck"), 1);
        const auto run = makeRun();
        c.send("start", {{"run", run}});
        QTRY_VERIFY(c.count("terminal") >= 1);
        const auto expected = c.last("terminal")["result"].toObject();
        const auto count = c.count("terminal");
        c.send("resultAck", {{"runId", run["runId"]}});
        QTRY_VERIFY(c.count("terminal") > count);
        QCOMPARE(c.count("resultConfirmed"), 0);
        QCOMPARE(diskState(dir.path())["state"].toString(), "terminal");
        server.reset();
        options.dropFirstResultAck = false;
        server = std::make_unique<StationServer>(options);
        QVERIFY(server->start());
        Client recovered;
        recovered.connectTo(server->port());
        QTRY_COMPARE(recovered.socket.state(), QAbstractSocket::ConnectedState);
        recovered.send("hello");
        QTRY_COMPARE(recovered.count("helloAck"), 1);
        QVERIFY(recovered.last("helloAck")["busy"].toBool());
        recovered.send("query", {{"runId", run["runId"]}});
        QTRY_COMPARE(recovered.count("runState"), 1);
        QCOMPARE(recovered.last("runState")["result"].toObject(), expected);
        recovered.send("resultAck", {{"runId", run["runId"]}});
        QTRY_COMPARE(recovered.count("resultConfirmed"), 1);
        QCOMPARE(diskState(dir.path())["state"].toString(), "acked");
    }
    void processRestartNeverInventsPassOrReexecutes()
    {
        QTemporaryDir dir;
        StationOptions options;
        options.dataDirectory = dir.path();
        options.port = 0;
        options.stepMs = 60000;
        auto server = std::make_unique<StationServer>(options);
        QVERIFY(server->start());
        Client c;
        c.connectTo(server->port());
        QTRY_COMPARE(c.socket.state(), QAbstractSocket::ConnectedState);
        c.send("hello");
        QTRY_COMPARE(c.count("helloAck"), 1);
        const auto run = makeRun();
        c.send("start", {{"run", run}});
        QTRY_COMPARE(c.count("accepted"), 1);
        QVERIFY(c.last("accepted")["accepted"].toBool());
        server.reset();
        server = std::make_unique<StationServer>(options);
        QVERIFY(server->start());
        Client recovered;
        recovered.connectTo(server->port());
        QTRY_COMPARE(recovered.socket.state(), QAbstractSocket::ConnectedState);
        recovered.send("hello");
        QTRY_COMPARE(recovered.count("helloAck"), 1);
        recovered.send("start", {{"run", run}});
        QTRY_COMPARE(recovered.count("accepted"), 1);
        QVERIFY(!recovered.last("accepted")["accepted"].toBool());
        recovered.send("query", {{"runId", run["runId"]}});
        QTRY_COMPARE(recovered.count("runState"), 1);
        QCOMPARE(recovered.last("runState")["state"].toString(), "unknown");
        QCOMPARE(recovered.count("terminal"), 0);
        QCOMPARE(recovered.count("progress"), 0);
        recovered.send("start", {{"run", run}});
        QTRY_COMPARE(recovered.count("accepted"), 2);
        QVERIFY(!recovered.last("accepted")["accepted"].toBool());
        recovered.send("start", {{"run", makeRun("explicit-new-run")}});
        QTRY_COMPARE(recovered.count("accepted"), 3);
        QVERIFY(recovered.last("accepted")["accepted"].toBool());
    }
    void wrongSessionCannotCancelAndCancellationNeedsTerminal()
    {
        QTemporaryDir dir;
        StationOptions options;
        options.dataDirectory = dir.path();
        options.port = 0;
        options.stepMs = 60000;
        StationServer server(options);
        QVERIFY(server.start());
        Client c;
        c.connectTo(server.port());
        QTRY_COMPARE(c.socket.state(), QAbstractSocket::ConnectedState);
        c.send("hello");
        QTRY_COMPARE(c.count("helloAck"), 1);
        const auto run = makeRun();
        c.send("start", {{"run", run}});
        QTRY_COMPARE(c.count("accepted"), 1);
        const auto session = c.session;
        c.session = "stale-session";
        c.send("cancel", {{"runId", run["runId"]}});
        c.session = session;
        c.send("query", {{"runId", run["runId"]}});
        QTRY_COMPARE(c.count("runState"), 1);
        QCOMPARE(c.last("runState")["state"].toString(), "executing");
        c.send("cancel", {{"runId", run["runId"]}});
        QTRY_VERIFY(c.count("terminal") >= 1);
        QCOMPARE(c.last("terminal")["result"].toObject()["outcome"].toString(), "cancelled");
        QVERIFY(c.last("terminal")["result"].toObject()["steps"].toArray().isEmpty());
    }
    void recipePreviewIsValidatedWithoutCreatingOrPersistingARun()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StationOptions options;
        options.dataDirectory = dir.path();
        options.port = 0;
        options.failStep = "temperature";
        StationServer server(options);
        QVERIFY(server.start());
        const auto before = diskState(dir.path());
        QVERIFY(!before.isEmpty());
        const auto recipe = previewRecipe("preview-v1");
        Client c;
        c.connectTo(server.port());
        QTRY_COMPARE(c.socket.state(), QAbstractSocket::ConnectedState);
        c.send("recipePreview", {{"recipe", recipe}}); // A handshake is required first.
        c.send("hello");
        QTRY_COMPARE(c.count("helloAck"), 1);
        QVERIFY(server.snapshot()["recipePreview"].toObject().isEmpty());
        c.send("recipePreview", {{"recipe", recipe}});
        c.send("ping"); // Same TCP stream: pong is a barrier for all preceding messages.
        QTRY_COMPARE(c.count("pong"), 1);
        QCOMPARE(server.snapshot()["recipePreview"].toObject(), recipe);
        QCOMPARE(server.snapshot()["state"].toString(), QString("idle"));
        QVERIFY(server.snapshot()["runId"].toString().isEmpty());
        QVERIFY(server.snapshot()["sn"].toString().isEmpty());
        QVERIFY(server.snapshot()["recipe"].toObject().isEmpty());
        QVERIFY(server.snapshot()["steps"].toArray().isEmpty());
        QVERIFY(server.snapshot()["result"].toObject().isEmpty());
        QVERIFY(server.snapshot()["faultsEditable"].toBool());
        QCOMPARE(server.snapshot()["failStep"].toString(), QString("temperature"));
        QCOMPARE(diskState(dir.path()), before);
        QCOMPARE(c.count("accepted"), 0);
        QCOMPARE(c.count("progress"), 0);
        QCOMPARE(c.count("terminal"), 0);

        auto missingName = recipe;
        missingName.remove("name");
        auto duplicateSteps = recipe;
        const auto first = recipe["steps"].toArray().first();
        duplicateSteps["steps"] = QJsonArray{first, first};
        const QList<QJsonValue> invalid{QJsonObject{}, missingName, duplicateSteps,
                                        QJsonArray{recipe}, QString("not a recipe")};
        for (const auto &value : invalid)
        {
            const int nextPong = c.count("pong") + 1;
            c.send("recipePreview", {{"recipe", value}});
            c.send("ping");
            QTRY_COMPARE(c.count("pong"), nextPong);
            QCOMPARE(server.snapshot()["recipePreview"].toObject(), recipe);
            QCOMPARE(diskState(dir.path()), before);
        }
        const auto session = c.session;
        c.session = "stale-session";
        c.send("recipePreview", {{"recipe", previewRecipe("stale-v2", false)}});
        c.session = session;
        const int nextPong = c.count("pong") + 1;
        c.send("ping");
        QTRY_COMPARE(c.count("pong"), nextPong);
        QCOMPARE(server.snapshot()["recipePreview"].toObject(), recipe);
        QCOMPARE(diskState(dir.path()), before);
    }
    void recipePreviewDuringExecutionCannotChangeBoundRecipeOrFailure()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StationOptions options;
        options.dataDirectory = dir.path();
        options.port = 0;
        options.stepMs = 1500;
        options.failStep = "temperature";
        StationServer server(options);
        QVERIFY(server.start());
        Client c;
        c.connectTo(server.port());
        QTRY_COMPARE(c.socket.state(), QAbstractSocket::ConnectedState);
        c.send("hello");
        QTRY_COMPARE(c.count("helloAck"), 1);
        const auto activeRecipe = previewRecipe("active-v1");
        const auto nextRecipe = previewRecipe("next-v2", false);
        const auto laterRecipe = previewRecipe("next-v3", false);
        auto run = makeRun("preview-cannot-change-active-run");
        run["recipe"] = activeRecipe;
        run["recipeVersion"] = activeRecipe["version"];
        // A delayed start for the old recipe cannot overwrite a newer operator preview.
        c.send("recipePreview", {{"recipe", nextRecipe}});
        c.send("start", {{"run", run}});
        QTRY_COMPARE(c.count("accepted"), 1);
        QVERIFY(c.last("accepted")["accepted"].toBool());
        QCOMPARE(server.snapshot()["state"].toString(), QString("executing"));
        QCOMPARE(server.snapshot()["recipePreview"].toObject(), nextRecipe);
        const auto executing = diskState(dir.path());
        c.send("recipePreview", {{"recipe", laterRecipe}});
        c.send("ping");
        QTRY_COMPARE(c.count("pong"), 1);
        QCOMPARE(server.snapshot()["recipePreview"].toObject(), laterRecipe);
        QCOMPARE(server.snapshot()["recipe"].toObject(), activeRecipe);
        QCOMPARE(server.snapshot()["recipeVersion"], activeRecipe["version"]);
        QCOMPARE(server.snapshot()["runId"], run["runId"]);
        QCOMPARE(server.snapshot()["failStep"].toString(), QString("temperature"));
        QVERIFY(!server.snapshot()["faultsEditable"].toBool());
        QCOMPARE(diskState(dir.path()), executing);
        QTRY_VERIFY_WITH_TIMEOUT(c.count("terminal") >= 1, 6000);
        const auto result = c.last("terminal")["result"].toObject();
        QJsonArray steps;
        QString verdict, error;
        QVERIFY2(Domain::evaluate(run, result, &steps, &verdict, &error), qPrintable(error));
        QCOMPARE(verdict, QString("FAIL"));
        QCOMPARE(steps.size(), 2);
        QCOMPARE(steps.first().toObject()["stepId"].toString(), QString("temperature"));
        QVERIFY(!steps.first().toObject()["passed"].toBool());
        QCOMPARE(server.snapshot()["recipePreview"].toObject(), laterRecipe);
        QCOMPARE(server.snapshot()["recipe"].toObject(), activeRecipe);
        QCOMPARE(diskState(dir.path())["run"].toObject(), run);
        c.send("recipePreview", {{"recipe", nextRecipe}}); // Terminal, still waiting for ACK.
        c.send("ping");
        QTRY_COMPARE(c.count("pong"), 2);
        QCOMPARE(server.snapshot()["recipePreview"].toObject(), nextRecipe);
        QCOMPARE(server.snapshot()["result"].toObject(), result);
        QCOMPARE(server.snapshot()["state"].toString(), QString("terminal"));
        c.send("resultAck", {{"runId", run["runId"]}});
        QTRY_COMPARE(c.count("resultConfirmed"), 1);
        QCOMPARE(server.snapshot()["state"].toString(), QString("acked"));
        QCOMPARE(server.snapshot()["recipePreview"].toObject(), nextRecipe);
        QCOMPARE(server.snapshot()["recipe"].toObject(), activeRecipe);
        QCOMPARE(server.snapshot()["failStep"].toString(), QString("temperature"));
        QCOMPARE(c.count("accepted"), 1);
    }
    void failureInjectionProducesLocalFail()
    {
        QTemporaryDir dir;
        StationOptions options;
        options.dataDirectory = dir.path();
        options.port = 0;
        options.stepMs = 1;
        options.failStep = "voltage";
        StationServer server(options);
        QVERIFY(server.start());
        Client c;
        c.connectTo(server.port());
        QTRY_COMPARE(c.socket.state(), QAbstractSocket::ConnectedState);
        c.send("hello");
        QTRY_COMPARE(c.count("helloAck"), 1);
        const auto run = makeRun();
        c.send("start", {{"run", run}});
        QTRY_VERIFY(c.count("terminal") >= 1);
        QJsonArray steps;
        QString verdict, error;
        QVERIFY2(Domain::evaluate(run, c.last("terminal")["result"].toObject(), &steps, &verdict,
                                  &error),
                 qPrintable(error));
        QCOMPARE(verdict, "FAIL");
    }
    void snapshotsAndFaultControlsFollowDurableLifecycle()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StationOptions options;
        options.dataDirectory = dir.path();
        options.port = 0;
        options.stepMs = 60000;
        StationServer server(options);
        QSignalSpy snapshots(&server, &StationServer::snapshotChanged);
        QSignalSpy diagnostics(&server, &StationServer::diagnostic);
        QVERIFY(server.start());
        QVERIFY(!snapshots.isEmpty());
        auto state = server.snapshot();
        QCOMPARE(state["stationId"].toInt(), 1);
        QCOMPARE(state["port"].toInt(), int(server.port()));
        QVERIFY(state["listening"].toBool());
        QVERIFY(!state["connected"].toBool());
        QVERIFY(!state["handshaken"].toBool());
        QCOMPARE(state["state"].toString(), "idle");
        QVERIFY(state["faultsEditable"].toBool());
        QVERIFY(state.contains("sn"));
        QVERIFY(state.contains("runId"));
        QVERIFY(state.contains("recipeVersion"));
        QString error;
        QVERIFY(!server.setFaultOptions("voltage", 0, true, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!server.setFaultOptions("bad\nstep", 10, true, &error));
        QVERIFY(!server.setFaultOptions(QString(41, 'x'), 10, true, &error));
        QVERIFY(!server.setFaultOptions(" current ", 10, true, &error));
        QCOMPARE(server.snapshot()["stepMs"].toInt(), 60000);
        QVERIFY(server.setFaultOptions("voltage", 60000, true, &error));
        QVERIFY(error.isEmpty());
        QCOMPARE(server.snapshot()["failStep"].toString(), "voltage");
        QVERIFY(server.snapshot()["dropFirstResultAck"].toBool());

        Client c;
        c.connectTo(server.port());
        QTRY_VERIFY(server.snapshot()["connected"].toBool());
        QVERIFY(!server.snapshot()["handshaken"].toBool());
        c.send("hello");
        QTRY_VERIFY(server.snapshot()["handshaken"].toBool());
        const auto run = makeRun("snapshot-run");
        c.send("start", {{"run", run}});
        QTRY_COMPARE(server.snapshot()["state"].toString(), "executing");
        state = server.snapshot();
        QCOMPARE(state["sn"], run["sn"]);
        QCOMPARE(state["runId"], run["runId"]);
        QCOMPARE(state["recipeVersion"], run["recipeVersion"]);
        QCOMPARE(state["recipe"].toObject(), run["recipe"].toObject());
        QVERIFY(state["steps"].toArray().isEmpty());
        QVERIFY(!state["faultsEditable"].toBool());
        QVERIFY(!server.setFaultOptions("", 1, false, &error));
        QCOMPARE(server.snapshot()["stepMs"].toInt(), 60000);
        server.disconnectClient();
        QTRY_VERIFY(!server.snapshot()["connected"].toBool());
        QVERIFY(!server.snapshot()["handshaken"].toBool());
        QCOMPARE(server.snapshot()["state"].toString(), "executing");
        QCOMPARE(diskState(dir.path())["state"].toString(), "executing");

        Client recovered;
        recovered.connectTo(server.port());
        QTRY_COMPARE(recovered.socket.state(), QAbstractSocket::ConnectedState);
        recovered.send("hello");
        QTRY_COMPARE(recovered.count("helloAck"), 1);
        recovered.send("query", {{"runId", run["runId"]}});
        QTRY_COMPARE(recovered.count("runState"), 1);
        QCOMPARE(recovered.last("runState")["state"].toString(), "executing");
        recovered.send("cancel", {{"runId", run["runId"]}});
        QTRY_COMPARE(server.snapshot()["state"].toString(), "terminal");
        QCOMPARE(server.snapshot()["result"].toObject()["outcome"].toString(), "cancelled");
        QVERIFY(!server.setFaultOptions("", 1, false, &error));
        const auto terminalCount = recovered.count("terminal");
        recovered.send("resultAck", {{"runId", run["runId"]}});
        QTRY_VERIFY(recovered.count("terminal") > terminalCount);
        QCOMPARE(recovered.count("resultConfirmed"), 0);
        recovered.send("resultAck", {{"runId", run["runId"]}});
        QTRY_COMPARE(server.snapshot()["state"].toString(), "acked");
        QTRY_COMPARE(recovered.count("resultConfirmed"), 1);
        QVERIFY(server.snapshot()["faultsEditable"].toBool());
        QVERIFY(server.setFaultOptions("", 1, false, &error));
        server.stop();
        QVERIFY(!server.snapshot()["listening"].toBool());
        QVERIFY(!server.snapshot()["connected"].toBool());
        QCOMPARE(snapshots.last().at(0).toJsonObject(), server.snapshot());
        QVERIFY(!diagnostics.isEmpty());
        for (const auto &entry : diagnostics)
        {
            const auto text = entry.at(0).toString();
            QVERIFY(text.size() <= 512);
            QVERIFY(!text.contains('\n'));
            QVERIFY(!text.contains('\r'));
        }
    }
    void firstResultAckFaultIsRearmedForEachNewRun()
    {
        QTemporaryDir dir;
        StationOptions options;
        options.dataDirectory = dir.path();
        options.port = 0;
        options.stepMs = 1;
        options.dropFirstResultAck = true;
        StationServer server(options);
        QVERIFY(server.start());
        Client c;
        c.connectTo(server.port());
        QTRY_COMPARE(c.socket.state(), QAbstractSocket::ConnectedState);
        c.send("hello");
        QTRY_COMPARE(c.count("helloAck"), 1);
        for (int i = 0; i < 2; ++i)
        {
            const auto run = makeRun(QStringLiteral("ack-fault-run-%1").arg(i));
            c.send("start", {{"run", run}});
            QTRY_COMPARE(c.last("terminal")["result"].toObject()["runId"], run["runId"]);
            QCOMPARE(server.snapshot()["steps"].toArray().size(), 3);
            const auto terminalCount = c.count("terminal");
            c.send("resultAck", {{"runId", run["runId"]}});
            QTRY_VERIFY(c.count("terminal") > terminalCount);
            QCOMPARE(c.count("resultConfirmed"), i);
            QCOMPARE(server.snapshot()["state"].toString(), "terminal");
            c.send("resultAck", {{"runId", run["runId"]}});
            QTRY_COMPARE(c.count("resultConfirmed"), i + 1);
            QCOMPARE(server.snapshot()["state"].toString(), "acked");
        }
    }
    void occupiedPortCannotMigrateAnExecutingRun()
    {
        QTemporaryDir dir;
        StationOptions options;
        options.dataDirectory = dir.path();
        options.port = 0;
        options.stepMs = 60000;
        StationServer active(options);
        QVERIFY(active.start());
        Client c;
        c.connectTo(active.port());
        QTRY_COMPARE(c.socket.state(), QAbstractSocket::ConnectedState);
        c.send("hello");
        QTRY_COMPARE(c.count("helloAck"), 1);
        c.send("start", {{"run", makeRun("live-run")}});
        QTRY_COMPARE(c.count("accepted"), 1);
        QVERIFY(c.last("accepted")["accepted"].toBool());
        const auto before = diskState(dir.path());
        QCOMPARE(before["state"].toString(), "executing");
        options.port = active.port();
        StationServer duplicate(options);
        QString error;
        QVERIFY(!duplicate.start(&error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!duplicate.snapshot()["listening"].toBool());
        QCOMPARE(diskState(dir.path()), before);
        QCOMPARE(active.snapshot()["state"].toString(), "executing");
    }
    void failedStateLoadReleasesEndpoint()
    {
        QTemporaryDir dir;
        QFile file(dir.filePath("station-1.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{broken");
        file.close();
        QTcpServer reservation;
        QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
        const auto endpoint = reservation.serverPort();
        reservation.close();
        StationOptions options;
        options.dataDirectory = dir.path();
        options.port = endpoint;
        StationServer server(options);
        QVERIFY(!server.start());
        QVERIFY(!server.snapshot()["listening"].toBool());
        QTcpServer available;
        QVERIFY(available.listen(QHostAddress::LocalHost, endpoint));
    }
    void corruptStateFailsClosed()
    {
        QTemporaryDir dir;
        QFile file(dir.filePath("station-1.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{broken");
        file.close();
        StationOptions options;
        options.dataDirectory = dir.path();
        options.port = 0;
        StationServer server(options);
        QString error;
        QVERIFY(!server.start(&error));
        QVERIFY(!error.isEmpty());
    }
};
QTEST_GUILESS_MAIN(SimulatorTests)
#include "test_simulator.moc"
