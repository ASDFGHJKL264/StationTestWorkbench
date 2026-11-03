#include "stationserver.h"
#include "simulatorwindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFont>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <cstring>
#include <memory>

int main(int argc, char **argv)
{
    bool headless = false;
    for (int i = 1; i < argc; ++i)
        headless = headless || std::strcmp(argv[i], "--headless") == 0;
    std::unique_ptr<QCoreApplication> application;
    if (headless)
        application = std::make_unique<QCoreApplication>(argc, argv);
    else
    {
        auto gui = std::make_unique<QApplication>(argc, argv);
        gui->setFont(QFont("Microsoft YaHei UI", 10));
        application = std::move(gui);
    }
    QCoreApplication::setOrganizationName("QtPortfolio");
    QCoreApplication::setApplicationName("StationSimulator");
    QCoreApplication::setApplicationVersion("0.1.0");
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("图形工位模拟器 · 所有数值均由程序生成"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"headless", "Run one station without a graphical interface (automation)."});
    parser.addOption({"auto-start", QStringLiteral("打开面板后自动启动配置的工位")});
    parser.addOption(
        {"port", QStringLiteral("首个工位监听端口，后续工位依次递增"), "port", "46501"});
    parser.addOption(
        {"station-id", QStringLiteral("仅显示指定工位（1..4）；省略时显示多工位"), "id", "1"});
    parser.addOption({"stations", QStringLiteral("图形面板工位数量（1..4）"), "count", "4"});
    parser.addOption(
        {"data-dir", QStringLiteral("模拟器状态目录（多工位模式自动创建station-N子目录）"), "path",
         headless ? QDir::current().filePath("simulator-data")
                  : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)});
    parser.addOption(
        {"step-ms", QStringLiteral("每个步骤的模拟耗时（1..60000毫秒）"), "ms", "400"});
    parser.addOption({"fail-step", QStringLiteral("指定超限步骤标识，例如current"), "id"});
    parser.addOption(
        {"fail-station", QStringLiteral("多工位演示中注入电流超限的工位，0关闭"), "id", "0"});
    parser.addOption(
        {"drop-first-result-ack", QStringLiteral("每次测试丢弃第一次归档确认，用于验证重试")});
    parser.addOption({"demo-faults", QStringLiteral("多工位演示：工位4丢弃首次归档确认")});
    parser.addOption(
        {"smoke-exit-ms", QStringLiteral("部署验证：指定毫秒后自动正常退出"), "ms", "0"});
    parser.process(*application);
    bool portOk = false, idOk = false, countOk = false, intervalOk = false, failOk = false,
         closeOk = false;
    const uint port = parser.value("port").toUInt(&portOk);
    const int firstId = parser.value("station-id").toInt(&idOk);
    const int count =
        headless || parser.isSet("station-id") ? 1 : parser.value("stations").toInt(&countOk);
    if (headless || parser.isSet("station-id"))
        countOk = true;
    const int stepMs = parser.value("step-ms").toInt(&intervalOk);
    const int failStation = parser.value("fail-station").toInt(&failOk);
    const int closeMs = parser.value("smoke-exit-ms").toInt(&closeOk);
    if (!portOk || port < 1 || port > 65535 || !idOk || firstId < 1 || firstId > 4 || !countOk ||
        count < 1 || count > 4 || port + uint(count - 1) > 65535 || !intervalOk || stepMs < 1 ||
        stepMs > 60000 || !failOk || failStation < 0 || failStation > 4 || !closeOk ||
        closeMs < 0 || closeMs > 60000 ||
        (headless && parser.isSet("stations") && parser.value("stations") != "1"))
    {
        QTextStream(stderr) << "Invalid simulator command-line option.\n";
        return 2;
    }
    QList<StationOptions> options;
    for (int i = 0; i < count; ++i)
    {
        StationOptions station;
        station.stationId = count == 1 ? firstId : i + 1;
        station.port = quint16(port + uint(i));
        station.stepMs = stepMs;
        station.dataDirectory = QDir(parser.value("data-dir")).absolutePath();
        if (!headless && !parser.isSet("station-id"))
            station.dataDirectory =
                QDir(station.dataDirectory).filePath(QString("station-%1").arg(station.stationId));
        station.failStep = parser.value("fail-step");
        if (station.stationId == failStation && station.failStep.isEmpty())
            station.failStep = "current";
        station.dropFirstResultAck = parser.isSet("drop-first-result-ack") ||
                                     (parser.isSet("demo-faults") && station.stationId == 4);
        options.append(station);
    }
    if (headless)
    {
        StationServer server(options.first());
        QObject::connect(&server, &StationServer::diagnostic, application.get(),
                         [](const QString &message) { QTextStream(stderr) << message << '\n'; });
        QString error;
        if (!server.start(&error))
        {
            QTextStream(stderr) << error << '\n';
            return 1;
        }
        QTextStream(stdout) << "SIMULATED station " << options.first().stationId
                            << " listening on 127.0.0.1:" << server.port() << '\n';
        if (closeMs > 0)
            QTimer::singleShot(closeMs, application.get(), &QCoreApplication::quit);
        return application->exec();
    }
    SimulatorWindow window(options, parser.isSet("auto-start"));
    window.show();
    if (closeMs > 0)
        QTimer::singleShot(closeMs, &window, &QWidget::close);
    return application->exec();
}
