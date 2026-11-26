#include "mainwindow.h"
#include "stationendpoint.h"
#include "toolstyle.h"
#include "appversion.h"
#include "runtimeconfiguration.h"
#include "workbenchcontroller.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QStandardPaths>
#include <QDir>
#include <QTimer>
#include <QFont>
#include <QMessageBox>
#include <QLockFile>
#include <QProcess>

// 启动工作台；argc 为命令行参数个数，argv 为参数内容。
int main(int argc, char **argv)
{
    // app 管理窗口事件和程序生命周期。
    QApplication app(argc, argv);
    ToolStyle::configureApplication(app);
    QCoreApplication::setOrganizationName("QtPortfolio");
    QCoreApplication::setApplicationName("StationTestWorkbench");
    QCoreApplication::setApplicationVersion(QString::fromLatin1(AppVersion::version));
    app.setFont(QFont("Microsoft YaHei UI", 10));
    // parser 读取数据目录、工位地址等启动参数。
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("电子模块多工位测试与质量追溯（模拟工位）"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"data-dir", QStringLiteral("数据根目录"), "directory",
                      QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)});
    parser.addOption({"host", QStringLiteral("工位主机"), "host", "127.0.0.1"});
    parser.addOption({"base-port", QStringLiteral("工位1端口，后续依次递增"), "port", "46501"});
    parser.addOption({"stations", QStringLiteral("工位数量1至4"), "count", "4"});
    parser.addOption(
        {"smoke-exit-ms", QStringLiteral("仅用于部署启动验证，延迟正常关闭"), "ms", "0"});
    parser.process(app);
    // portOk 表示端口解析成功，countOk 表示工位数量解析成功。
    bool portOk = false, countOk = false;
    // port 为首个工位端口，count 为需要连接的工位数量。
    const int port = parser.value("base-port").toInt(&portOk),
              count = parser.value("stations").toInt(&countOk);
    if (!portOk || !countOk || count < 1 || count > 4 || port < 1024 || port + count - 1 > 65535)
        parser.showHelp(2);
    // endpoints 保存本次启动的各工位连接地址。
    QList<StationEndpoint> endpoints;
    for (int i = 0; i < count; ++i)
        endpoints.append({i + 1, parser.value("host"), quint16(port + i)});
    // window 显示操作员主界面。
    // 跟随已成功恢复的数据目录，桌面原入口仍可打开新库。
    QString directoryError;
    // 保存启动时实际使用的数据目录。
    const auto dataDirectory =
        RuntimeConfiguration::resolveDataDirectory(parser.value("data-dir"), &directoryError);
    if (dataDirectory.isEmpty() || !QDir().mkpath(dataDirectory))
    {
        QMessageBox::critical(nullptr, QStringLiteral("数据目录无法打开"), directoryError);
        return 2;
    }
    // 同一数据目录只允许一个工作台，备份恢复也使用相同的独占锁。
    QLockFile dataLock(QDir(dataDirectory).filePath(".workbench.lock"));
    if (!dataLock.tryLock(0))
    {
        QMessageBox::critical(
            nullptr, QStringLiteral("数据目录正在使用"),
            QStringLiteral("另一个工作台或恢复任务正在使用该数据目录，请先关闭它。"));
        return 3;
    }
    // 创建使用已解析数据目录的工作台窗口。
    MainWindow window(dataDirectory, endpoints);
    QObject::connect(
        window.controller(), &WorkbenchController::restartRequested, &window,
        [&window](const QString &target)
        {
            // 保留工位连接参数，仅替换新的数据目录。
            QStringList arguments;
            // 保存需要保留的原工位连接参数。
            const auto original = QCoreApplication::arguments().mid(1);
            for (int index = 0; index < original.size(); ++index)
            {
                if (original[index] == "--data-dir")
                {
                    ++index;
                    continue;
                }
                if (!original[index].startsWith("--data-dir="))
                    arguments.append(original[index]);
            }
            arguments.append({"--data-dir", target});
            window.controller()->shutdown();
            if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), arguments))
                QMessageBox::critical(&window, QStringLiteral("重启失败"),
                                      QStringLiteral("恢复数据已保存，请重新打开桌面工作台。"));
            QCoreApplication::quit();
        });
    window.setWindowTitle(
        QStringLiteral("多工位功能测试与追溯 · v%1").arg(QCoreApplication::applicationVersion()));
    window.show();
    // closeMs 为部署验证时自动关闭窗口的延迟毫秒数。
    const int closeMs = parser.value("smoke-exit-ms").toInt();
    if (closeMs > 0)
        QTimer::singleShot(closeMs, &window, &QWidget::close);
    return app.exec();
}
