#include "mainwindow.h"
#include "stationendpoint.h"
#include "toolstyle.h"
#include "appversion.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QStandardPaths>
#include <QDir>
#include <QTimer>
#include <QFont>
#include <QMenuBar>
#include <QMessageBox>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    ToolStyle::configureApplication(app);
    QCoreApplication::setOrganizationName("QtPortfolio");
    QCoreApplication::setApplicationName("StationTestWorkbench");
    QCoreApplication::setApplicationVersion(QString::fromLatin1(AppVersion::version));
    app.setFont(QFont("Microsoft YaHei UI", 10));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("电子模块多工位测试与质量追溯（模拟工位）"));
    parser.addHelpOption();
    parser.addVersionOption();
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
    window.setWindowTitle(QStringLiteral("多工位功能测试与追溯 · v%1")
                              .arg(QCoreApplication::applicationVersion()));
    auto *about = window.menuBar()->addMenu(QStringLiteral("帮助"))
                      ->addAction(QStringLiteral("版本信息"));
    QObject::connect(about, &QAction::triggered, &window, [&window]
                     {
                         QMessageBox::information(
                             &window, QStringLiteral("版本信息"),
                             QStringLiteral("多工位功能测试与追溯\n版本：%1\n提交：%2\n构建时间（UTC）：%3")
                                 .arg(QCoreApplication::applicationVersion(),
                                      QString::fromLatin1(AppVersion::revision),
                                      QString::fromLatin1(AppVersion::builtAtUtc)));
                     });
    window.show();
    const int closeMs = parser.value("smoke-exit-ms").toInt();
    if (closeMs > 0)
        QTimer::singleShot(closeMs, &window, &QWidget::close);
    return app.exec();
}
