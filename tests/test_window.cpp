#include "mainwindow.h"
#include "workbenchcontroller.h"
#include "repository.h"
#include "domain.h"
#include "toolstyle.h"
#include "maintenancedialog.h"
#include <QAction>
#include <QSpinBox>
#include <QFileInfo>
#include <QDialog>
#include <QtTest>
#include <QTemporaryDir>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <QDir>
#include <QFontDatabase>
#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QTimer>
#include <QComboBox>
#include <QCheckBox>
#include <QDateEdit>
#include <QLabel>
#include <QPlainTextEdit>
#include <QListView>
#include <cmath>

namespace
{
class RestoreApplicationPalette
{
  public:
    // 在离开验证作用域时恢复进入前的应用调色板。
    ~RestoreApplicationPalette()
    {
        QApplication::setPalette(m_palette);
    }

  private:
    // 保存验证前的应用调色板，结束时恢复。
    QPalette m_palette = QApplication::palette();
};

// 按 dark 开关构造用于主题切换验证的系统调色板。
QPalette systemPalette(bool dark)
{
    // 保存待设置或检查的应用及控件调色板。
    QPalette palette = QApplication::palette();
    // 提供模拟系统主题使用的背景颜色。
    const QColor background(dark ? "#202020" : "#f3f3f3");
    // 提供模拟系统主题使用的文字颜色。
    const QColor foreground(dark ? "#ffffff" : "#222222");
    for (auto role : {QPalette::Window, QPalette::Base, QPalette::Button})
        palette.setColor(QPalette::All, role, background);
    for (auto role :
         {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::PlaceholderText})
        palette.setColor(QPalette::All, role, foreground);
    return palette;
}

// 计算 foreground 和 background 的文字对比度。
double contrast(const QColor &foreground, const QColor &background)
{
    // 把 color 转为计算文字对比度所需的相对亮度。
    const auto luminance = [](const QColor &color)
    {
        // 将 value 从 sRGB 通道值换算为线性亮度值。
        const auto linear = [](double value)
        { return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4); };
        return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) +
               0.0722 * linear(color.blueF());
    };
    // 保存前景亮度，b 保存背景亮度以计算对比度。
    const double a = luminance(foreground), b = luminance(background);
    return (qMax(a, b) + 0.05) / (qMin(a, b) + 0.05);
}

// 按 widget 控件类型返回需要检查的文字及背景配色角色。
QList<QPair<QPalette::ColorRole, QPalette::ColorRole>> textRoles(QWidget *widget)
{
    if (qobject_cast<QLineEdit *>(widget))
        return {{QPalette::Text, QPalette::Base},
                {QPalette::PlaceholderText, QPalette::Base},
                {QPalette::HighlightedText, QPalette::Highlight}};
    if (qobject_cast<QPlainTextEdit *>(widget))
        return {{QPalette::Text, QPalette::Base}, {QPalette::HighlightedText, QPalette::Highlight}};
    if (qobject_cast<QAbstractItemView *>(widget))
        return {{QPalette::Text, QPalette::Base},
                {QPalette::Text, QPalette::AlternateBase},
                {QPalette::HighlightedText, QPalette::Highlight}};
    if (qobject_cast<QPushButton *>(widget) || qobject_cast<QComboBox *>(widget))
        return {{QPalette::ButtonText, QPalette::Button}};
    return {{QPalette::WindowText, QPalette::Window}};
}

// 从主窗入口打开并定位指定的独立工具窗口。
QDialog *openWindow(MainWindow &window, const char *actionName, const char *dialogName)
{
    // 使用实际入口触发子窗口显示。
    auto *action = window.findChild<QAction *>(actionName);
    if (!action)
        return nullptr;
    action->trigger();
    return window.findChild<QDialog *>(dialogName);
}

// 按需保存指定窗口的截图，未配置证据目录时直接通过。
bool saveWindowEvidence(QWidget *window, const QString &name)
{
    // 保存本次截图使用的证据目录。
    const auto directory = qEnvironmentVariable("STATION_UI_EVIDENCE_DIR");
    return directory.isEmpty() || (window && QDir().mkpath(directory) &&
                                   window->grab().save(QDir(directory).filePath(name)));
}
} // namespace

class WindowTests final : public QObject
{
    Q_OBJECT
  private slots:
    // 准备界面验证所需的样式、对话框或离屏字体环境。
    void initTestCase()
    {
        ToolStyle::configureApplication(*qApp);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        // The Windows offscreen plugin does not discover system fonts automatically.
        if (QGuiApplication::platformName() == "offscreen")
        {
            // 保存加载微软雅黑字体后返回的字体编号。
            const auto id = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
            if (id >= 0)
                QApplication::setFont(
                    QFont(QFontDatabase::applicationFontFamilies(id).first(), 10));
        }
    }
    // 验证维护入口的认证门禁、密码隐藏和时限保存。
    void maintenanceRequiresLoginAndKeepsCredentialsOutOfViews()
    {
        // 隔离本次维护界面验证的数据与凭据。
        QTemporaryDir directory;
        // 创建隔离数据的维护界面验证窗口。
        MainWindow window(directory.path(), {{1, "127.0.0.1", 48592}});
        window.show();
        // 从实际菜单打开维护面板。
        auto *action = window.findChild<QAction *>("openMaintenanceAction");
        // 等待后台可用，历史窗口初始保持隐藏。
        auto *query = window.findChild<QPushButton *>("queryHistoryButton");
        QVERIFY(action && query);
        QTRY_VERIFY(query->isEnabled());
        action->trigger();
        // 获取认证与维护操作控件。
        auto *dialog = window.findChild<MaintenanceDialog *>();
        QVERIFY(dialog);
        // 输入本次设置的管理员账号。
        auto *user = dialog->findChild<QLineEdit *>("adminUserInput");
        // 输入管理员密码并验证隐藏显示。
        auto *password = dialog->findChild<QLineEdit *>("adminPasswordInput");
        // 确认首次设置时的管理员密码。
        auto *confirm = dialog->findChild<QLineEdit *>("adminConfirmInput");
        // 触发首次管理员设置。
        auto *setup = dialog->findChild<QPushButton *>("adminSetupButton");
        // 检查备份操作的认证门禁。
        auto *backup = dialog->findChild<QPushButton *>("maintenanceBackupButton");
        // 保存测试期限设置。
        auto *save = dialog->findChild<QPushButton *>("saveTimeoutsButton");
        // 设置整轮测试的秒数上限。
        auto *run = dialog->findChild<QSpinBox *>("runTimeoutSeconds");
        // 设置每个步骤的秒数上限。
        auto *step = dialog->findChild<QSpinBox *>("stepTimeoutSeconds");
        // 退出会话并检查敏感操作重新禁用。
        auto *logout = dialog->findChild<QPushButton *>("adminLogoutButton");
        QVERIFY(user && password && confirm && setup && backup && save && run && step && logout);
        QCOMPARE(password->echoMode(), QLineEdit::Password);
        QCOMPARE(confirm->echoMode(), QLineEdit::Password);
        QVERIFY(!backup->isEnabled());
        QVERIFY(!save->isEnabled());
        user->setText("engineer");
        password->setText("WindowAdmin#2026");
        confirm->setText("WindowAdmin#2026");
        QTest::mouseClick(setup, Qt::LeftButton);
        QTRY_VERIFY(window.controller()->adminAuthorized());
        QTRY_VERIFY(save->isEnabled());
        QVERIFY(password->text().isEmpty());
        QVERIFY(confirm->text().isEmpty());
        QTRY_VERIFY(dialog->findChild<QLabel *>("backupDirectoryLabel")
                        ->text()
                        .contains(directory.filePath("backups")));
        QVERIFY(!dialog->findChild<QWidget *>("maintenanceTabs"));
        QVERIFY(saveWindowEvidence(dialog, "maintenance-center.png"));
        for (const auto &entry : QList<QPair<const char *, const char *>>{
                 {"openDataMaintenanceButton", "maintenanceDataDialog"},
                 {"openTimeoutSettingsButton", "maintenanceTimeoutDialog"},
                 {"openExceptionMaintenanceButton", "maintenanceExceptionDialog"},
                 {"openMaintenanceAuditButton", "maintenanceAuditDialog"}})
        {
            // 使用维护中心实际入口打开独立功能窗口。
            auto *navigation = dialog->findChild<QPushButton *>(entry.first);
            // 定位该入口应显示的功能子窗口。
            auto *child = dialog->findChild<QDialog *>(entry.second);
            QVERIFY(navigation && child);
            QVERIFY(navigation->isEnabled());
            QVERIFY(!child->isVisible());
            QTest::mouseClick(navigation, Qt::LeftButton);
            QTRY_VERIFY(child->isVisible());
            QVERIFY(child->isWindow());
            QVERIFY(!child->isModal());
            QVERIFY(saveWindowEvidence(child, QString::fromLatin1(entry.second) + ".png"));
            child->close();
            QVERIFY(!child->isVisible());
            QVERIFY(window.controller()->adminAuthorized());
        }
        QTest::mouseClick(dialog->findChild<QPushButton *>("openTimeoutSettingsButton"),
                          Qt::LeftButton);
        run->setValue(180);
        step->setValue(45);
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_VERIFY(QFileInfo::exists(directory.filePath("maintenance/runtime.json")));
        QTest::mouseClick(logout, Qt::LeftButton);
        QTRY_VERIFY(!window.controller()->adminAuthorized());
        QTRY_VERIFY(!save->isEnabled());
        QVERIFY(!backup->isEnabled());
        password->setText("DiscardedOnClose#2026");
        confirm->setText("DiscardedOnClose#2026");
        dialog->close();
        QVERIFY(password->text().isEmpty());
        QVERIFY(confirm->text().isEmpty());
        for (auto *child : dialog->findChildren<QDialog *>())
            QVERIFY(!child->isVisible());
        QCOMPARE(openWindow(window, "openMaintenanceAction", "maintenanceDialog"), dialog);
        QVERIFY(password->text().isEmpty());
        QVERIFY(confirm->text().isEmpty());
    }

    // 验证工具窗口独立复用，主窗口关闭时同步关闭各子窗口。
    void toolWindowsStaySeparateAndCloseWithMainWindow()
    {
        // 隔离本次窗口生命周期验证的数据。
        QTemporaryDir directory;
        // 创建只显示工位与当前测试的主窗口。
        MainWindow window(directory.path(), {});
        window.show();
        // 定位构造时已存在但尚未打开的工具窗口。
        auto *history = window.findChild<QDialog *>("historyDialog");
        // 定位持续接收后台日志的独立窗口。
        auto *logs = window.findChild<QDialog *>("logDialog");
        QVERIFY(history && logs);
        QVERIFY(!history->isVisible());
        QVERIFY(!logs->isVisible());
        QVERIFY(history->isWindow() && logs->isWindow());
        QVERIFY(!window.centralWidget()->isAncestorOf(history));
        QVERIFY(!window.centralWidget()->isAncestorOf(logs));
        QVERIFY(!window.findChild<QWidget *>("detailTabs"));
        QVERIFY(!window.findChild<QLabel *>("hint"));
        QCOMPARE(openWindow(window, "openHistoryAction", "historyDialog"), history);
        QCOMPARE(openWindow(window, "openLogAction", "logDialog"), logs);
        // 定位管理员入口打开的维护中心。
        auto *maintenance = openWindow(window, "openMaintenanceAction", "maintenanceDialog");
        QVERIFY(maintenance);
        QVERIFY(history->isVisible() && logs->isVisible() && maintenance->isVisible());
        history->close();
        logs->close();
        QVERIFY(window.isVisible());
        QCOMPARE(openWindow(window, "openHistoryAction", "historyDialog"), history);
        QCOMPARE(openWindow(window, "openLogAction", "logDialog"), logs);
        QVERIFY(history->isVisible() && logs->isVisible());
        window.close();
        QTRY_VERIFY(!window.isVisible());
        QVERIFY(!history->isVisible());
        QVERIFY(!logs->isVisible());
        QVERIFY(!maintenance->isVisible());
    }

    // 提供浅转深和深转浅两种系统配色切换场景。
    void interfaceRemainsReadableAcrossSystemThemes_data()
    {
        QTest::addColumn<bool>("initialDark");
        QTest::newRow("light-to-dark") << false;
        QTest::newRow("dark-to-light") << true;
    }
    // 验证主题双向切换后文字、输入框和禁用控件仍清晰。
    void interfaceRemainsReadableAcrossSystemThemes()
    {
        // 指定本组测试开始时是否采用深色系统配色。
        QFETCH(bool, initialDark);
        // 在验证结束时自动恢复原应用调色板。
        RestoreApplicationPalette restore;
        QApplication::setPalette(systemPalette(initialDark));
        // 隔离本次验证的数据库、日志和工位状态。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 提供本次验证操作和观察的图形窗口。
        MainWindow window(directory.path(), {{1, "127.0.0.1", 48591}});
        window.show();
        // 打开历史追溯窗口，验证独立窗口的实际控件配色。
        auto *historyDialog = openWindow(window, "openHistoryAction", "historyDialog");
        // 打开运行日志窗口，验证日志与主窗分别显示。
        auto *logDialog = openWindow(window, "openLogAction", "logDialog");
        QVERIFY(historyDialog && logDialog);
        // 输入或筛选本次验证使用的产品序列号。
        auto *sn = window.findChild<QLineEdit *>("snInput");
        // 输入历史记录的完整 SN 筛选条件。
        auto *filter = window.findChild<QLineEdit *>("historyFilter");
        // 观察工作台测试配方选择框的主题显示。
        auto *recipe = window.findChild<QComboBox *>("recipeCombo");
        // 操作当前窗口的测试或工位启动按钮。
        auto *start = window.findChild<QPushButton *>("startButton");
        // 操作历史记录查询并观察后台就绪状态。
        auto *query = window.findChild<QPushButton *>("queryHistoryButton");
        // 设置历史查询包含的本地起始日期。
        auto *from = window.findChild<QDateEdit *>("historyFromDate");
        // 设置历史查询包含的本地结束日期。
        auto *through = window.findChild<QDateEdit *>("historyToDate");
        // 选择历史查询需要匹配的工位。
        auto *stationFilter = window.findChild<QComboBox *>("historyStationFilter");
        // 选择历史查询需要匹配的质量判定。
        auto *verdictFilter = window.findChild<QComboBox *>("historyVerdictFilter");
        // 操作所选工位的只读状态核对按钮。
        auto *reconcile = window.findChild<QPushButton *>("reconcileButton");
        // 选择并观察工位状态表。
        auto *stations = window.findChild<QTableView *>("stationView");
        // 观察工作台当前测试步骤表的主题显示。
        auto *steps = window.findChild<QTableView *>("stepView");
        // 观察历史结果表的行数、产品序列号和主题显示。
        auto *history = window.findChild<QTableView *>("historyView");
        // 观察独立日志列表在主题变化后的显示。
        auto *logs = window.findChild<QListView *>("logView");
        QVERIFY(sn && filter && recipe && start && query && from && through && stationFilter &&
                verdictFilter && reconcile && stations && steps && history && logs);
        QTRY_VERIFY(query->isEnabled());
        for (int phase = 0; phase < 2; ++phase)
        {
            if (phase)
                QApplication::setPalette(systemPalette(!initialDark));
            QCoreApplication::processEvents();
            QVERIFY(saveWindowEvidence(
                &window, QString("workbench-%1-%2.png").arg(QTest::currentDataTag()).arg(phase)));
            QVERIFY(saveWindowEvidence(
                historyDialog,
                QString("history-%1-%2.png").arg(QTest::currentDataTag()).arg(phase)));
            QVERIFY(saveWindowEvidence(
                logDialog, QString("logs-%1-%2.png").arg(QTest::currentDataTag()).arg(phase)));
            for (auto *widget :
                 QList<QWidget *>{&window, sn, filter, recipe, start, query, from, through,
                                  stationFilter, verdictFilter, reconcile, stations, steps, history,
                                  historyDialog, logDialog, logs})
            {
                // 保存待设置或检查的应用及控件调色板。
                const auto palette = widget->palette();
                for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
                {
                    // 指定当前控件状态要求达到的最小文字对比度。
                    const double minimum = group == QPalette::Disabled ? 3.0 : 4.5;
                    for (const auto pair : textRoles(widget))
                    {
                        // 组装控件、配色角色和切换阶段信息，便于定位对比度失败。
                        const auto detail = QString("%1 / group %2 / role %3 / phase %4")
                                                .arg(widget->metaObject()->className())
                                                .arg(group)
                                                .arg(pair.first)
                                                .arg(phase)
                                                .toUtf8();
                        QVERIFY2(contrast(palette.color(group, pair.first),
                                          palette.color(group, pair.second)) >= minimum,
                                 detail.constData());
                    }
                }
            }
        }
        window.close();
    }
    // 验证组合筛选重置分页，且日期限制默认关闭。
    void historyFiltersResetPaginationAndKeepDatesOptional()
    {
        // 隔离本次验证的数据库、日志和工位状态。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 指定工作台验证使用的独立数据目录。
        const auto clientDirectory = directory.filePath("client");
        {
            // 准备本次窗口查询和导出所需的数据库记录。
            Repository repository;
            repository.initialize(clientDirectory + "/data/workbench.db");
            // 记录测试预约结果，saved 记录终态归档结果。
            QSignalSpy reserved(&repository, &Repository::runReserved),
                saved(&repository, &Repository::resultSaved);
            // 准备填充窗口测试数据库的默认配方。
            const auto recipe = Domain::defaultRecipe();
            for (int i = 0; i < 103; ++i)
            {
                // 保存本次验证使用的测试身份和配方请求。
                QJsonObject run{{"runId", QString("run-%1").arg(i)},
                                {"stationId", 2},
                                {"sn", QString("SN-%1").arg(i)},
                                {"recipeVersion", recipe["version"]},
                                {"recipe", recipe}};
                repository.reserveRun(run);
                QVERIFY(reserved.last()[1].toBool());
                // 收集本次构造终态的实测步骤。
                QJsonArray steps;
                for (const auto &value : recipe["steps"].toArray())
                {
                    // 读取当前步骤的限值和标称值以构造测量结果。
                    const auto rule = value.toObject();
                    steps.append(QJsonObject{{"stepId", rule["id"]}, {"value", rule["nominal"]}});
                }
                // 构造用于写入测试数据库的终态结果。
                auto result = run;
                result["outcome"] = "completed";
                result["steps"] = steps;
                repository.saveResult(result);
                QVERIFY(saved.last()[1].toBool());
            }
        }
        // 提供本次验证操作和观察的图形窗口。
        MainWindow window(clientDirectory, {});
        window.show();
        QVERIFY(openWindow(window, "openHistoryAction", "historyDialog"));
        // 操作历史记录查询并观察后台就绪状态。
        auto *query = window.findChild<QPushButton *>("queryHistoryButton");
        // 操作历史记录的更早一页按钮。
        auto *older = window.findChild<QPushButton *>("olderHistoryButton");
        // 观察历史结果表的行数、产品序列号和主题显示。
        auto *history = window.findChild<QTableView *>("historyView");
        // 输入或筛选本次验证使用的产品序列号。
        auto *sn = window.findChild<QLineEdit *>("historyFilter");
        // 选择历史记录筛选使用的工位条件。
        auto *station = window.findChild<QComboBox *>("historyStationFilter");
        // 选择历史记录筛选使用的质量判定。
        auto *verdict = window.findChild<QComboBox *>("historyVerdictFilter");
        // 控制是否启用历史起始日期限制。
        auto *fromEnabled = window.findChild<QCheckBox *>("historyFromEnabled");
        // 控制是否启用历史结束日期限制。
        auto *toEnabled = window.findChild<QCheckBox *>("historyToEnabled");
        // 设置历史查询包含的本地起始日期。
        auto *from = window.findChild<QDateEdit *>("historyFromDate");
        // 设置历史查询包含的本地结束日期。
        auto *through = window.findChild<QDateEdit *>("historyToDate");
        QVERIFY(query && older && history && sn && station && verdict && fromEnabled && toEnabled &&
                from && through);
        QVERIFY(!fromEnabled->isChecked() && !toEnabled->isChecked());
        QVERIFY(!from->isEnabled() && !through->isEnabled());
        QTRY_COMPARE(history->model()->rowCount(), 100);
        QTRY_VERIFY(older->isEnabled());
        QTest::mouseClick(older, Qt::LeftButton);
        QTRY_COMPARE(history->model()->rowCount(), 3);
        QTRY_VERIFY(query->isEnabled());
        station->setCurrentIndex(station->findData(2));
        QVERIFY(!older->isEnabled());
        QTest::mouseClick(query, Qt::LeftButton);
        QTRY_COMPARE(history->model()->rowCount(), 100);
        QTRY_VERIFY(older->isEnabled());
        sn->setText("SN-7");
        QVERIFY(!older->isEnabled());
        QTest::mouseClick(query, Qt::LeftButton);
        QTRY_COMPARE(history->model()->rowCount(), 1);
        QCOMPARE(history->model()->index(0, 2).data().toString(), "SN-7");
        verdict->setCurrentIndex(verdict->findData("FAIL"));
        QTest::mouseClick(query, Qt::LeftButton);
        QTRY_COMPARE(history->model()->rowCount(), 0);
        QTRY_VERIFY(query->isEnabled());
        verdict->setCurrentIndex(verdict->findData("PASS"));
        QTest::mouseClick(query, Qt::LeftButton);
        QTRY_COMPARE(history->model()->rowCount(), 1);
        QTRY_VERIFY(query->isEnabled());

        from->setDate(QDate::currentDate().addDays(1));
        fromEnabled->setChecked(true);
        QVERIFY(from->isEnabled());
        QTest::mouseClick(query, Qt::LeftButton);
        QTRY_COMPARE(history->model()->rowCount(), 0);
        QTRY_VERIFY(query->isEnabled());
        fromEnabled->setChecked(false);
        through->setDate(QDate::currentDate().addDays(-1));
        toEnabled->setChecked(true);
        QVERIFY(through->isEnabled());
        QTest::mouseClick(query, Qt::LeftButton);
        QTRY_VERIFY(query->isEnabled());
        QCOMPARE(history->model()->rowCount(), 0);
        toEnabled->setChecked(false);
        QTest::mouseClick(query, Qt::LeftButton);
        QTRY_COMPARE(history->model()->rowCount(), 1);
        QTRY_VERIFY(query->isEnabled());
        // 记录历史查询应答，确认非法日期区间不会发出查询。
        QSignalSpy ready(window.controller(), &WorkbenchController::historyReady);
        fromEnabled->setChecked(true);
        toEnabled->setChecked(true);
        QTest::mouseClick(query, Qt::LeftButton);
        QCOMPARE(ready.size(), 0);
        QVERIFY(query->isEnabled());
        window.close();
    }
    // 验证旧查询应答和无关警告不会干扰当前查询与游标。
    void obsoleteHistoryRepliesAndWarningsDoNotChangeCurrentQuery()
    {
        // 隔离本次验证的数据库、日志和工位状态。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 提供本次验证操作和观察的图形窗口。
        MainWindow window(directory.path(), {});
        // 操作历史记录查询并观察后台就绪状态。
        auto *query = window.findChild<QPushButton *>("queryHistoryButton");
        // 选择历史记录筛选使用的工位条件。
        auto *station = window.findChild<QComboBox *>("historyStationFilter");
        // 操作历史记录的更早一页按钮。
        auto *older = window.findChild<QPushButton *>("olderHistoryButton");
        QVERIFY(query && station && older);
        // history 记录当前查询的成功应答，failed 收集失败通知。
        QSignalSpy history(window.controller(), &WorkbenchController::historyReady),
            failed(window.controller(), &WorkbenchController::historyFailed);
        window.show();
        QVERIFY(openWindow(window, "openHistoryAction", "historyDialog"));
        QTRY_COMPARE(history.size(), 1);
        history.clear();
        // 构造工位编号非法的历史筛选条件。
        HistoryFilter invalid;
        invalid.stationId = 5;
        window.controller()->refreshFilteredHistory({});
        window.controller()->refreshFilteredHistory(invalid);
        // 保存需要最终生效的历史查询条件。
        HistoryFilter current;
        current.stationId = 2;
        window.controller()->refreshFilteredHistory(current);
        QTRY_COMPARE(history.size(), 1);
        QCOMPARE(failed.size(), 0);

        // A late response for unchanged backend conditions must not revive a stale page cursor
        // after the user has edited the controls without submitting another query.
        window.controller()->historyReady({QJsonObject{{"id", 150}}}, true);
        QVERIFY(older->isEnabled());
        station->setCurrentIndex(station->findData(3));
        QVERIFY(!older->isEnabled());
        window.controller()->historyReady({QJsonObject{{"id", 149}}}, true);
        QVERIFY(!older->isEnabled());
        QTest::mouseClick(query, Qt::LeftButton);
        QVERIFY(!query->isEnabled());
        window.controller()->logMessage("unrelated network warning", true);
        QVERIFY(!query->isEnabled());
        QTRY_VERIFY(query->isEnabled());
        window.close();
    }
    // 验证重新核对按钮只服从当前所选工位的核对权限。
    void reconciliationButtonFollowsSelectedStationCapability()
    {
        // 隔离本次验证的数据库、日志和工位状态。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 提供本次验证操作和观察的图形窗口。
        MainWindow window(directory.path(), {{1, "127.0.0.1", 48592}, {2, "127.0.0.1", 48593}});
        window.show();
        // 操作历史记录查询并观察后台就绪状态。
        auto *query = window.findChild<QPushButton *>("queryHistoryButton");
        // 操作所选工位的只读状态核对按钮。
        auto *reconcile = window.findChild<QPushButton *>("reconcileButton");
        // 操作当前窗口的测试或工位启动按钮。
        auto *start = window.findChild<QPushButton *>("startButton");
        // 选择并观察工位状态表。
        auto *stations = window.findChild<QTableView *>("stationView");
        QVERIFY(query && reconcile && start && stations);
        QVERIFY(!reconcile->isEnabled());
        QTRY_VERIFY(query->isEnabled());
        window.controller()->stationUpdated(
            {{"stationId", 1}, {"canReconcile", true}, {"canStart", false}, {"runId", "held-run"}});
        QVERIFY(reconcile->isEnabled());
        QVERIFY(!start->isEnabled());
        stations->selectRow(1);
        QVERIFY(!reconcile->isEnabled());
        stations->selectRow(0);
        QVERIFY(reconcile->isEnabled());
        QTest::mouseClick(reconcile, Qt::LeftButton);
        QVERIFY(!reconcile->isEnabled());
        QVERIFY(!start->isEnabled());
        window.controller()->stationUpdated(
            {{"stationId", 1}, {"canReconcile", false}, {"canStart", false}});
        QVERIFY(!reconcile->isEnabled());
        window.close();
    }
    // 验证异常 SN 导出覆盖全部历史，并支持取消文件选择。
    void failedSnExportButtonUsesWholeHistoryAndCanBeCancelled()
    {
        // 隔离本次验证的数据库、日志和工位状态。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 指定工作台验证使用的独立数据目录。
        const auto clientDirectory = directory.filePath("client");
        {
            // 准备本次窗口查询和导出所需的数据库记录。
            Repository repository;
            repository.initialize(clientDirectory + "/data/workbench.db");
            // 记录测试预约结果，saved 记录终态归档结果。
            QSignalSpy reserved(&repository, &Repository::runReserved),
                saved(&repository, &Repository::resultSaved);
            // 准备填充窗口测试数据库的默认配方。
            const auto recipe = Domain::defaultRecipe();
            for (const auto &sn : {QString("FAILED-SN"), QString("PASS-ONLY")})
            {
                // 保存本次验证使用的测试身份和配方请求。
                QJsonObject run{{"runId", sn},
                                {"stationId", 1},
                                {"sn", sn},
                                {"recipeVersion", recipe["version"]},
                                {"recipe", recipe}};
                repository.reserveRun(run);
                QVERIFY(reserved.last()[1].toBool());
                // 收集本次构造终态的实测步骤。
                QJsonArray steps;
                for (const auto &value : recipe["steps"].toArray())
                {
                    // 读取当前步骤的限值和标称值以构造测量结果。
                    const auto rule = value.toObject();
                    steps.append(
                        QJsonObject{{"stepId", rule["id"]},
                                    {"value", sn == "FAILED-SN" ? rule["max"].toDouble() + 1
                                                                : rule["nominal"].toDouble()}});
                }
                // 构造用于写入测试数据库的终态结果。
                auto result = run;
                result["outcome"] = "completed";
                result["steps"] = steps;
                repository.saveResult(result);
                QVERIFY(saved.last()[1].toBool());
            }
        }
        // 提供本次验证操作和观察的图形窗口。
        MainWindow window(clientDirectory, {});
        // 操作全部历史异常 SN 的导出按钮。
        auto *button = window.findChild<QPushButton *>("exportFailedSnsButton");
        // 输入历史记录的完整 SN 筛选条件。
        auto *filter = window.findChild<QLineEdit *>("historyFilter");
        // 操作历史记录查询并观察后台就绪状态。
        auto *query = window.findChild<QPushButton *>("queryHistoryButton");
        // 观察历史结果表的行数、产品序列号和主题显示。
        auto *history = window.findChild<QTableView *>("historyView");
        QVERIFY(button && filter && query && history);
        QVERIFY(!button->isEnabled());
        window.show();
        QVERIFY(openWindow(window, "openHistoryAction", "historyDialog"));
        QTRY_VERIFY(button->isEnabled() && query->isEnabled());
        filter->setText("PASS-ONLY");
        QTest::mouseClick(query, Qt::LeftButton);
        QTRY_COMPARE(history->model()->rowCount(), 1);
        QCOMPARE(history->model()->index(0, 2).data().toString(), "PASS-ONLY");
        // 选择历史记录筛选使用的工位条件。
        auto *station = window.findChild<QComboBox *>("historyStationFilter");
        // 选择历史记录筛选使用的质量判定。
        auto *verdict = window.findChild<QComboBox *>("historyVerdictFilter");
        // 控制是否启用历史起始日期限制。
        auto *fromEnabled = window.findChild<QCheckBox *>("historyFromEnabled");
        // 设置历史查询包含的本地起始日期。
        auto *from = window.findChild<QDateEdit *>("historyFromDate");
        QVERIFY(station && verdict && fromEnabled && from);
        station->setCurrentIndex(station->findData(4));
        verdict->setCurrentIndex(verdict->findData("UNKNOWN"));
        from->setDate(QDate::currentDate().addDays(1));
        fromEnabled->setChecked(true);
        QTest::mouseClick(query, Qt::LeftButton);
        QTRY_COMPARE(history->model()->rowCount(), 0);
        QTRY_VERIFY(query->isEnabled());
        // 记录异常 SN 清单导出的完成状态和数量。
        QSignalSpy exported(window.controller(), &WorkbenchController::failedSnsExported);
        // 指定本次异常 SN 清单的 CSV 输出路径。
        const auto output = directory.filePath("failed-sns.csv");
        // 在文件选择超时时自动关闭对话框，避免验证挂起。
        QTimer dialogTimeout;
        dialogTimeout.setSingleShot(true);
        // 超时后拒绝文件对话框，避免导出验证挂起。
        connect(&dialogTimeout, &QTimer::timeout, &window,
                [&window]
                {
                    // 定位当前文件对话框，以模拟选择或取消。
                    if (auto *dialog = window.findChild<QFileDialog *>())
                        dialog->reject();
                });
        // 记录是否已在文件对话框中模拟选择导出路径。
        bool selected = false;
        // 在文件对话框打开后模拟选择路径或取消操作。
        QTimer::singleShot(0, &window,
                           [&]
                           {
                               // 定位当前文件对话框，以模拟选择或取消。
                               if (auto *dialog = window.findChild<QFileDialog *>())
                               {
                                   selected = true;
                                   dialog->selectFile(output);
                                   QMetaObject::invokeMethod(dialog, "accept");
                               }
                           });
        dialogTimeout.start(3000);
        QTest::mouseClick(button, Qt::LeftButton);
        dialogTimeout.stop();
        QVERIFY(selected);
        QTRY_COMPARE(exported.size(), 1);
        QVERIFY(exported.last()[1].toBool());
        QCOMPARE(exported.last()[2].toLongLong(), 1);
        QVERIFY(button->isEnabled());
        // 读写本次验证所需的临时文件。
        QFile file(output);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("\xEF\xBB\xBF"
                                            "SN\r\n\"FAILED-SN\"\r\n"));
        // 记录是否已模拟取消文件选择操作。
        bool cancelled = false;
        // 在文件对话框打开后模拟选择路径或取消操作。
        QTimer::singleShot(0, &window,
                           [&]
                           {
                               // 定位当前文件对话框，以模拟选择或取消。
                               if (auto *dialog = window.findChild<QFileDialog *>())
                               {
                                   cancelled = true;
                                   dialog->reject();
                               }
                           });
        dialogTimeout.start(3000);
        QTest::mouseClick(button, Qt::LeftButton);
        dialogTimeout.stop();
        QVERIFY(cancelled);
        QCOMPARE(exported.size(), 1);
        QVERIFY(button->isEnabled());
        window.close();
    }
};
QTEST_MAIN(WindowTests)
#include "test_window.moc"
