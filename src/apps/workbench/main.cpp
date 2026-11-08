#include "workbench/ui/mainwindow.h"
#include "testing/stationendpoint.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QStandardPaths>
#include <QDir>
#include <QTimer>
#include <QFont>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("QtPortfolio");
    QCoreApplication::setApplicationName("StationTestWorkbench");
    app.setFont(QFont("Microsoft YaHei UI", 10));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("电子模块多工位测试与质量追溯（模拟工位）"));
    parser.addHelpOption();
    parser.addOption({"data-dir", QStringLiteral("数据根目录"), "directory",
                      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)});
    parser.addOption({"host", QStringLiteral("工位主机"), "host", "127.0.0.1"});
    parser.addOption({"base-port", QStringLiteral("工位1端口，后续依次递增"), "port", "46501"});
    parser.addOption({"stations", QStringLiteral("工位数量1至4"), "count", "4"});
    parser.addOption(
        {"smoke-exit-ms", QStringLiteral("仅用于部署启动验证，延迟正常关闭"), "ms", "0"});
    parser.process(app);
    bool portOk = false, countOk = false;
    const int port = parser.value("base-port").toInt(&portOk),
              count = parser.value("stations").toInt(&countOk);
    if (!portOk || !countOk || count < 1 || count > 4 || port < 1024 || port + count - 1 > 65535)
        parser.showHelp(2);
    QList<StationEndpoint> endpoints;
    for (int i = 0; i < count; ++i)
        endpoints.append({i + 1, parser.value("host"), quint16(port + i)});
    MainWindow window(QDir(parser.value("data-dir")).absolutePath(), endpoints);
    window.show();
    const int closeMs = parser.value("smoke-exit-ms").toInt();
    if (closeMs > 0)
        QTimer::singleShot(closeMs, &window, &QWidget::close);
    return app.exec();
}
