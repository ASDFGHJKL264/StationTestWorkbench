#include "maintenancedialog.h"
#include "workbenchcontroller.h"
#include "toolstyle.h"
#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QGuiApplication>
#include <QScreen>
#include <QSplitter>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>

namespace
{
// 创建可换行的状态或字段标签。
QLabel *note(const QString &text, QWidget *parent)
{
    // 保存需要加入页面的说明控件。
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    return label;
}

// 创建独立的非模态维护窗口，并适配屏幕可用区域。
QDialog *childWindow(QWidget *parent, const char *name, const QString &title,
                     const QSize &preferred, const QSize &minimum)
{
    // 保存关闭后复用的子窗口。
    auto *window = new QDialog(parent);
    window->setObjectName(name);
    window->setWindowTitle(title);
    window->setWindowModality(Qt::NonModal);
    window->setAttribute(Qt::WA_DeleteOnClose, false);
    window->setPalette(ToolStyle::lightPalette());
    window->setStyleSheet(ToolStyle::stylesheet());
    // 保存当前屏幕允许使用的窗口大小。
    QSize available(1280, 900);
    if (auto *screen = QGuiApplication::primaryScreen())
        available = screen->availableGeometry().size() - QSize(40, 60);
    window->setMinimumSize(minimum.boundedTo(available));
    window->resize(preferred.boundedTo(available));
    return window;
}

// 创建按整行选择、只读且无本页排序的维护表格。
QTableWidget *makeTable(const QStringList &headers, QWidget *parent)
{
    // 保存用于展示后台身份快照的表格。
    auto *table = new QTableWidget(0, headers.size(), parent);
    table->setHorizontalHeaderLabels(headers);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->setSortingEnabled(false);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    return table;
}

// 将后台记录中的操作类型转成易读的中文名称。
QString actionLabel(const QString &action)
{
    // 统一比较大小写不同的操作代码。
    const auto name = action.toLower();
    if (name == "timeout" || name == "run_timeout")
        return QStringLiteral("测试超时");
    if (name == "manual_resolution" || name == "manual_resolve" || name == "manual_disposition")
        return QStringLiteral("人工结算");
    if (name == "station_suspended" || name == "station_suspend")
        return QStringLiteral("停用工位");
    if (name == "station_resumed" || name == "station_resume" || name == "resume_station")
        return QStringLiteral("重新启用工位");
    if (name == "admin_configured")
        return QStringLiteral("首次设置管理员");
    if (name == "login_failed")
        return QStringLiteral("管理员登录失败");
    if (name == "login")
        return QStringLiteral("管理员登录");
    if (name == "logout")
        return QStringLiteral("管理员退出");
    if (name == "backup_failed")
        return QStringLiteral("数据库备份失败");
    if (name == "backup")
        return QStringLiteral("数据库备份");
    if (name == "restore_failed")
        return QStringLiteral("恢复失败");
    if (name == "restore_staged")
        return QStringLiteral("恢复到新目录");
    if (name == "restore_activated")
        return QStringLiteral("切换恢复数据");
    if (name == "restored_database_opened")
        return QStringLiteral("打开恢复数据库");
    if (name == "timeout_settings")
        return QStringLiteral("修改测试时限");
    return action;
}

// 将 UTC 事件时间转换为本地显示，保留无法解析的原文本。
QString localTime(const QString &value)
{
    // 保存后台传来的 ISO 时间。
    const auto parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    return parsed.isValid() ? parsed.toLocalTime().toString("yyyy-MM-dd HH:mm:ss") : value;
}
} // namespace

MaintenanceDialog::MaintenanceDialog(WorkbenchController *controller, QWidget *parent)
    : QDialog(parent), m_controller(controller)
{
    setObjectName("maintenanceDialog");
    setWindowTitle(QStringLiteral("管理员维护"));
    setStyleSheet(ToolStyle::stylesheet());
    setPalette(ToolStyle::lightPalette());
    resize(620, 390);
    // 限制窗口尺寸，适配当前屏幕可用区域。
    if (auto *screen = QGuiApplication::primaryScreen())
        resize(qMin(620, screen->availableGeometry().width() - 40),
               qMin(390, screen->availableGeometry().height() - 60));
    // 保存维护窗口的整体布局。
    auto *layout = new QVBoxLayout(this);
    buildAuthentication(layout);
    buildDataPage();
    buildTimeoutPage();
    buildExceptionPage();
    buildAuditPage();
    // 保存各维护子窗口的独立入口。
    auto *navigation = new QGridLayout;
    m_openData = new QPushButton(QStringLiteral("备份与恢复"), this);
    m_openData->setObjectName("openDataMaintenanceButton");
    m_openTimeouts = new QPushButton(QStringLiteral("测试超时"), this);
    m_openTimeouts->setObjectName("openTimeoutSettingsButton");
    m_openExceptions = new QPushButton(QStringLiteral("异常处置"), this);
    m_openExceptions->setObjectName("openExceptionMaintenanceButton");
    m_openAudit = new QPushButton(QStringLiteral("操作记录"), this);
    m_openAudit->setObjectName("openMaintenanceAuditButton");
    for (auto *button : {m_openData, m_openTimeouts, m_openExceptions, m_openAudit})
        button->setMinimumHeight(40);
    navigation->addWidget(m_openData, 0, 0);
    navigation->addWidget(m_openTimeouts, 0, 1);
    navigation->addWidget(m_openExceptions, 1, 0);
    navigation->addWidget(m_openAudit, 1, 1);
    layout->addLayout(navigation);
    connect(m_openData, &QPushButton::clicked, this, [this] { openWindow(m_dataWindow); });
    connect(m_openTimeouts, &QPushButton::clicked, this, [this] { openWindow(m_timeoutWindow); });
    connect(m_openExceptions, &QPushButton::clicked, this,
            [this] { openWindow(m_exceptionWindow); });
    connect(m_openAudit, &QPushButton::clicked, this, [this] { openWindow(m_auditWindow); });
    m_status = note({}, this);
    m_status->setObjectName("maintenanceStatusLabel");
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);
    // 保存窗口底部刷新和关闭按钮的布局。
    auto *footer = new QHBoxLayout;
    m_refresh = new QPushButton(QStringLiteral("刷新维护记录"), this);
    m_refresh->setObjectName("maintenanceRefreshButton");
    footer->addWidget(m_refresh);
    footer->addStretch();
    // 使用关闭角色退出维护窗口，不影响工作台运行。
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    footer->addWidget(buttons);
    layout->addLayout(footer);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_refresh, &QPushButton::clicked, m_controller,
            &WorkbenchController::refreshMaintenance);
    connect(m_controller, &WorkbenchController::maintenanceReady, this,
            &MaintenanceDialog::showOverview);
    // 后台在窗口打开后才就绪时，补读当前维护快照。
    connect(m_controller, &WorkbenchController::ready, this,
            [this](bool ok)
            {
                if (ok && m_controller->adminAuthorized())
                    m_controller->refreshMaintenance();
            });
    // 后台提示不解除仍在执行的备份任务。
    connect(m_controller, &WorkbenchController::maintenanceMessage, this,
            [this](const QString &message, bool error)
            {
                if (error)
                    m_restartRequested = false;
                showMessage(message, error);
                updateAuthorization();
            });
    // 备份任务无论成功或失败都恢复操作按钮。
    connect(m_controller, &WorkbenchController::backupFinished, this,
            [this](const QString &path, bool ok, const QString &error)
            {
                setBusy(false);
                showMessage(ok ? QStringLiteral("备份已完成并通过验证：") + path
                               : QStringLiteral("备份失败：") + error,
                            !ok);
                if (m_controller->adminAuthorized())
                    m_controller->refreshMaintenance();
            });
    // 验证结束后只回报备份内容，不更改当前数据库。
    connect(m_controller, &WorkbenchController::validationFinished, this,
            [this](const QString &path, bool ok, const QString &error)
            {
                setBusy(false);
                showMessage(ok ? QStringLiteral("备份验证通过：") + path
                               : QStringLiteral("备份验证失败：") + error,
                            !ok);
            });
    // 捕获成功恢复的目录，只有该目录能用于本窗口的切换入口。
    connect(m_controller, &WorkbenchController::restoreFinished, this,
            [this](const QString &directory, bool ok, const QString &error)
            {
                setBusy(false);
                if (ok)
                {
                    m_restoredDirectory = directory;
                    showMessage(QStringLiteral("恢复完成：%1").arg(directory));
                }
                else
                    showMessage(QStringLiteral("恢复失败：") + error, true);
                updateAuthorization();
            });
    // 定时反映会话到期，后台接口仍会逐次核验权限。
    auto *authorizationTimer = new QTimer(this);
    authorizationTimer->setInterval(1000);
    connect(authorizationTimer, &QTimer::timeout, this, &MaintenanceDialog::updateAuthorization);
    authorizationTimer->start();
    updateAuthorization();
    if (m_authorized)
        m_controller->refreshMaintenance();
}

void MaintenanceDialog::buildAuthentication(QVBoxLayout *layout)
{
    // 保存首次设置和登录控件所在分组。
    auto *group = new QGroupBox(QStringLiteral("管理员身份"), this);
    // 排列管理员名、密码和首次确认输入框。
    auto *grid = new QGridLayout(group);
    m_user = new QLineEdit(group);
    m_user->setObjectName("adminUserInput");
    m_user->setMaxLength(40);
    m_user->setPlaceholderText(QStringLiteral("首次设置管理员名"));
    m_password = new QLineEdit(group);
    m_password->setObjectName("adminPasswordInput");
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setMaxLength(128);
    m_password->setPlaceholderText(QStringLiteral("至少10个字符"));
    m_confirmation = new QLineEdit(group);
    m_confirmation->setObjectName("adminConfirmInput");
    m_confirmation->setEchoMode(QLineEdit::Password);
    m_confirmation->setMaxLength(128);
    m_confirmation->setPlaceholderText(QStringLiteral("再次输入密码"));
    grid->addWidget(new QLabel(QStringLiteral("管理员名"), group), 0, 0);
    grid->addWidget(m_user, 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("密码"), group), 1, 0);
    grid->addWidget(m_password, 1, 1);
    m_confirmationLabel = new QLabel(QStringLiteral("确认密码"), group);
    grid->addWidget(m_confirmationLabel, 2, 0);
    grid->addWidget(m_confirmation, 2, 1);
    m_authState = note({}, group);
    m_authState->setObjectName("adminStateLabel");
    grid->addWidget(m_authState, 3, 0, 1, 2);
    // 排列登录相关按钮，首次设置不会提供默认密码。
    auto *actions = new QHBoxLayout;
    m_setup = new QPushButton(QStringLiteral("首次设置管理员"), group);
    m_setup->setObjectName("adminSetupButton");
    m_login = new QPushButton(QStringLiteral("登录"), group);
    m_login->setObjectName("adminLoginButton");
    m_logout = new QPushButton(QStringLiteral("退出登录"), group);
    m_logout->setObjectName("adminLogoutButton");
    actions->addStretch();
    actions->addWidget(m_setup);
    actions->addWidget(m_login);
    actions->addWidget(m_logout);
    grid->addLayout(actions, 4, 0, 1, 2);
    layout->addWidget(group);
    // 首次设置完成或失败后都清空两个密码输入框。
    connect(m_setup, &QPushButton::clicked, this,
            [this]
            {
                // 保存本次首次设置失败的具体原因。
                QString error;
                // 捕获当前输入，交给控制器创建本机管理员。
                const bool ok = m_controller->configureAdministrator(
                    m_user->text(), m_password->text(), m_confirmation->text(), &error);
                m_password->clear();
                m_confirmation->clear();
                showMessage(ok ? QStringLiteral("管理员已设置") : error, !ok);
                updateAuthorization();
                if (ok)
                    m_controller->refreshMaintenance();
            });
    // 登录结果只显示成功状态或原因，不保留输入的密码。
    connect(m_login, &QPushButton::clicked, this,
            [this]
            {
                // 保存本次登录失败的原因。
                QString error;
                // 保存控制器核验管理员后的结果。
                const bool ok =
                    m_controller->loginAdministrator(m_user->text(), m_password->text(), &error);
                m_password->clear();
                m_confirmation->clear();
                showMessage(ok ? QStringLiteral("已登录") : error, !ok);
                updateAuthorization();
                if (ok)
                    m_controller->refreshMaintenance();
            });
    connect(m_password, &QLineEdit::returnPressed, this,
            [this]
            {
                if (m_login->isEnabled())
                    m_login->click();
            });
    connect(m_logout, &QPushButton::clicked, this,
            [this]
            {
                m_controller->logoutAdministrator();
                m_password->clear();
                m_confirmation->clear();
                updateAuthorization();
                showMessage(QStringLiteral("已退出登录"));
            });
}

void MaintenanceDialog::buildDataPage()
{
    m_dataWindow = childWindow(this, "maintenanceDataDialog", QStringLiteral("备份与恢复"),
                               QSize(700, 300), QSize(520, 240));
    // 排列备份路径和操作入口。
    auto *layout = new QVBoxLayout(m_dataWindow);
    // 保存备份文件位置及维护操作。
    auto *backupGroup = new QGroupBox(QStringLiteral("数据库备份"), m_dataWindow);
    // 排列备份位置和操作按钮。
    auto *backupLayout = new QVBoxLayout(backupGroup);
    m_backupDirectoryLabel = note(QStringLiteral("—"), backupGroup);
    m_backupDirectoryLabel->setObjectName("backupDirectoryLabel");
    m_backupDirectoryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    backupLayout->addWidget(m_backupDirectoryLabel);
    // 分行排列备份与恢复操作。
    auto *actions = new QGridLayout;
    m_backup = new QPushButton(QStringLiteral("立即备份"), backupGroup);
    m_backup->setObjectName("maintenanceBackupButton");
    m_validate = new QPushButton(QStringLiteral("验证备份"), backupGroup);
    m_validate->setObjectName("maintenanceValidateButton");
    m_restore = new QPushButton(QStringLiteral("恢复到新目录"), backupGroup);
    m_restore->setObjectName("maintenanceRestoreButton");
    m_activate = new QPushButton(QStringLiteral("切换并重启"), backupGroup);
    m_activate->setObjectName("maintenanceActivateButton");
    actions->addWidget(m_backup, 0, 0);
    actions->addWidget(m_validate, 0, 1);
    actions->addWidget(m_restore, 1, 0);
    actions->addWidget(m_activate, 1, 1);
    backupLayout->addLayout(actions);
    layout->addWidget(backupGroup);
    layout->addStretch();
    addWindowFooter(m_dataWindow, layout);
    connect(m_backup, &QPushButton::clicked, this, &MaintenanceDialog::backup);
    connect(m_validate, &QPushButton::clicked, this, &MaintenanceDialog::validateBackup);
    connect(m_restore, &QPushButton::clicked, this, &MaintenanceDialog::restore);
    connect(m_activate, &QPushButton::clicked, this,
            [this]
            {
                // 固定本次成功恢复的新目录，避免后续刷新改变重启目标。
                const auto directory = m_restoredDirectory;
                if (!m_controller->adminAuthorized() || directory.isEmpty())
                    return;
                if (QMessageBox::question(
                        m_dataWindow, QStringLiteral("切换数据库"),
                        QStringLiteral("重启工作台并使用以下目录？\n%1\n原数据库保留。")
                            .arg(directory)) != QMessageBox::Yes)
                    return;
                m_restartRequested = true;
                updateAuthorization();
                m_controller->activateRestoredDatabase(directory);
            });
}

void MaintenanceDialog::buildTimeoutPage()
{
    m_timeoutWindow = childWindow(this, "maintenanceTimeoutDialog", QStringLiteral("测试超时"),
                                  QSize(460, 250), QSize(380, 220));
    // 排列时限设置和关闭入口。
    auto *layout = new QVBoxLayout(m_timeoutWindow);
    // 保存之后新测试所使用的超时设置。
    auto *timeoutGroup = new QGroupBox(QStringLiteral("新测试时限"), m_timeoutWindow);
    // 排列整轮及单步时限。
    auto *form = new QFormLayout(timeoutGroup);
    m_runSeconds = new QSpinBox(timeoutGroup);
    m_runSeconds->setObjectName("runTimeoutSeconds");
    m_runSeconds->setRange(1, 86400);
    m_runSeconds->setValue(120);
    m_runSeconds->setSuffix(QStringLiteral(" 秒"));
    m_stepSeconds = new QSpinBox(timeoutGroup);
    m_stepSeconds->setObjectName("stepTimeoutSeconds");
    m_stepSeconds->setRange(1, 86400);
    m_stepSeconds->setValue(30);
    m_stepSeconds->setSuffix(QStringLiteral(" 秒"));
    form->addRow(QStringLiteral("整轮测试上限"), m_runSeconds);
    form->addRow(QStringLiteral("每个步骤上限"), m_stepSeconds);
    m_saveTimeouts = new QPushButton(QStringLiteral("保存超时规则"), timeoutGroup);
    m_saveTimeouts->setObjectName("saveTimeoutsButton");
    form->addRow(m_saveTimeouts);
    layout->addWidget(timeoutGroup);
    layout->addStretch();
    addWindowFooter(m_timeoutWindow, layout);
    connect(m_runSeconds, &QSpinBox::valueChanged, this, [this] { m_timeoutDirty = true; });
    connect(m_stepSeconds, &QSpinBox::valueChanged, this, [this] { m_timeoutDirty = true; });
    connect(m_saveTimeouts, &QPushButton::clicked, this,
            [this]
            {
                if (m_stepSeconds->value() > m_runSeconds->value())
                {
                    showMessage(QStringLiteral("每步骤上限不能超过整轮测试上限。"), true);
                    return;
                }
                m_timeoutDirty = false;
                m_controller->setExecutionTimeouts(m_runSeconds->value(), m_stepSeconds->value());
            });
}

void MaintenanceDialog::buildExceptionPage()
{
    m_exceptionWindow = childWindow(this, "maintenanceExceptionDialog", QStringLiteral("异常处置"),
                                    QSize(980, 690), QSize(720, 500));
    // 排列可调整大小的异常任务和停用工位区域。
    auto *layout = new QVBoxLayout(m_exceptionWindow);
    // 允许操作员按需要分配两组表格的空间。
    auto *splitter = new QSplitter(Qt::Vertical, m_exceptionWindow);
    splitter->setChildrenCollapsible(false);
    // 保存待处置任务列表及操作原因。
    auto *exceptions = new QGroupBox(QStringLiteral("待核对任务"), splitter);
    // 排列待处置任务和确认控件。
    auto *exceptionLayout = new QVBoxLayout(exceptions);
    m_exceptions = makeTable({QStringLiteral("工位"), QStringLiteral("产品 SN"),
                              QStringLiteral("测试编号"), QStringLiteral("当前说明")},
                             exceptions);
    m_exceptions->setObjectName("maintenanceExceptionTable");
    exceptionLayout->addWidget(m_exceptions, 1);
    m_manualReason = new QPlainTextEdit(exceptions);
    m_manualReason->setObjectName("manualReasonInput");
    m_manualReason->setPlaceholderText(QStringLiteral("人工处置原因"));
    m_manualReason->setMaximumHeight(60);
    exceptionLayout->addWidget(m_manualReason);
    // 保存停止确认和人工结算按钮行。
    auto *manualActions = new QHBoxLayout;
    m_stopped = new QCheckBox(QStringLiteral("已确认旧设备停止执行"), exceptions);
    m_stopped->setObjectName("stoppedConfirmed");
    m_resolve = new QPushButton(QStringLiteral("人工结算并停用工位"), exceptions);
    m_resolve->setObjectName("resolveExceptionButton");
    manualActions->addWidget(m_stopped);
    manualActions->addStretch();
    manualActions->addWidget(m_resolve);
    exceptionLayout->addLayout(manualActions);
    splitter->addWidget(exceptions);
    // 保存人工处置后或安全检查所停用的工位。
    auto *suspended = new QGroupBox(QStringLiteral("停用工位"), splitter);
    // 排列停用列表和重新启用条件。
    auto *suspendedLayout = new QVBoxLayout(suspended);
    m_suspended = makeTable(
        {QStringLiteral("工位"), QStringLiteral("关联测试编号"), QStringLiteral("停用原因")},
        suspended);
    m_suspended->setObjectName("maintenanceSuspendedTable");
    suspendedLayout->addWidget(m_suspended, 1);
    m_resumeReason = new QPlainTextEdit(suspended);
    m_resumeReason->setObjectName("resumeReasonInput");
    m_resumeReason->setPlaceholderText(QStringLiteral("重新启用原因"));
    m_resumeReason->setMaximumHeight(56);
    suspendedLayout->addWidget(m_resumeReason);
    // 保存复位确认和启用按钮行。
    auto *resumeActions = new QHBoxLayout;
    m_reset = new QCheckBox(QStringLiteral("旧设备已复位或更换，可重新核对"), suspended);
    m_reset->setObjectName("resetConfirmed");
    m_resume = new QPushButton(QStringLiteral("重新启用工位"), suspended);
    m_resume->setObjectName("resumeStationButton");
    resumeActions->addWidget(m_reset);
    resumeActions->addStretch();
    resumeActions->addWidget(m_resume);
    suspendedLayout->addLayout(resumeActions);
    splitter->addWidget(suspended);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);
    addWindowFooter(m_exceptionWindow, layout);
    connect(m_exceptions, &QTableWidget::itemSelectionChanged, this,
            &MaintenanceDialog::updateSelectionActions);
    connect(m_suspended, &QTableWidget::itemSelectionChanged, this,
            &MaintenanceDialog::updateSelectionActions);
    connect(m_manualReason, &QPlainTextEdit::textChanged, this,
            &MaintenanceDialog::updateSelectionActions);
    connect(m_resumeReason, &QPlainTextEdit::textChanged, this,
            &MaintenanceDialog::updateSelectionActions);
    connect(m_stopped, &QCheckBox::toggled, this, &MaintenanceDialog::updateSelectionActions);
    connect(m_reset, &QCheckBox::toggled, this, &MaintenanceDialog::updateSelectionActions);
    connect(m_resolve, &QPushButton::clicked, this, &MaintenanceDialog::resolveSelected);
    connect(m_resume, &QPushButton::clicked, this, &MaintenanceDialog::resumeSelected);
}

void MaintenanceDialog::buildAuditPage()
{
    m_auditWindow = childWindow(this, "maintenanceAuditDialog", QStringLiteral("操作记录"),
                                QSize(1000, 580), QSize(700, 360));
    // 将可滚动的审计表格作为窗口主体。
    auto *layout = new QVBoxLayout(m_auditWindow);
    m_audit =
        makeTable({QStringLiteral("时间（本地）"), QStringLiteral("操作人"), QStringLiteral("操作"),
                   QStringLiteral("测试编号"), QStringLiteral("工位"), QStringLiteral("原因")},
                  m_auditWindow);
    m_audit->setObjectName("maintenanceAuditTable");
    layout->addWidget(m_audit, 1);
    addWindowFooter(m_auditWindow, layout);
}

void MaintenanceDialog::addWindowFooter(QDialog *window, QVBoxLayout *layout)
{
    // 显示当前维护动作的完成状态或错误。
    auto *status = note({}, window);
    status->setObjectName(window->objectName() + "StatusLabel");
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status);
    m_windowStatuses.append(status);
    // 关闭当前子窗口，保留已输入的数据和后台任务。
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, window);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, window, &QDialog::reject);
}

void MaintenanceDialog::openWindow(QDialog *window)
{
    updateAuthorization();
    if (!m_controller->adminAuthorized() || m_restartRequested)
        return;
    window->show();
    window->raise();
    window->activateWindow();
    m_controller->refreshMaintenance();
}

void MaintenanceDialog::done(int result)
{
    for (auto *window : {m_dataWindow, m_timeoutWindow, m_exceptionWindow, m_auditWindow})
        window->close();
    m_controller->logoutAdministrator();
    m_password->clear();
    m_confirmation->clear();
    m_stopped->setChecked(false);
    m_reset->setChecked(false);
    updateAuthorization();
    QDialog::done(result);
}

void MaintenanceDialog::showOverview(const QJsonObject &overview)
{
    if (!m_controller->adminAuthorized())
        return;
    // 填充快照时不发布短暂的空行选择，避免旧确认状态影响按钮。
    const QSignalBlocker exceptionBlocker(m_exceptions), suspendedBlocker(m_suspended);
    // 保留正在查看的测试身份，后台刷新不能改选另一条运行。
    const auto selectedRun = m_exceptions->currentRow() >= 0
                                 ? m_exceptions->item(m_exceptions->currentRow(), 2)->text()
                                 : QString();
    // 保留正在查看的停用工位编号。
    const auto selectedStation = m_suspended->currentRow() >= 0
                                     ? m_suspended->item(m_suspended->currentRow(), 0)->text()
                                     : QString();
    m_exceptions->setRowCount(0);
    for (const auto &value : overview.value("exceptions").toArray())
    {
        // 保存当前待核对运行的完整快照。
        const auto record = value.toObject();
        // 保存当前快照在表格中的显示行。
        const int row = m_exceptions->rowCount();
        m_exceptions->insertRow(row);
        m_exceptions->setItem(
            row, 0, new QTableWidgetItem(QString::number(record.value("stationId").toInt())));
        m_exceptions->setItem(row, 1, new QTableWidgetItem(record.value("sn").toString()));
        m_exceptions->setItem(row, 2, new QTableWidgetItem(record.value("runId").toString()));
        m_exceptions->setItem(row, 3, new QTableWidgetItem(record.value("reason").toString()));
        m_exceptions->item(row, 0)->setData(Qt::UserRole, QVariant::fromValue(record));
        if (record.value("runId").toString() == selectedRun)
            m_exceptions->selectRow(row);
    }
    m_suspended->setRowCount(0);
    for (const auto &value : overview.value("suspendedStations").toArray())
    {
        // 保存当前停用工位及停用原因。
        const auto record = value.toObject();
        // 保存当前停用工位的显示行。
        const int row = m_suspended->rowCount();
        m_suspended->insertRow(row);
        m_suspended->setItem(
            row, 0, new QTableWidgetItem(QString::number(record.value("stationId").toInt())));
        m_suspended->setItem(row, 1, new QTableWidgetItem(record.value("runId").toString()));
        m_suspended->setItem(row, 2, new QTableWidgetItem(record.value("reason").toString()));
        if (m_suspended->item(row, 0)->text() == selectedStation)
            m_suspended->selectRow(row);
    }
    m_audit->setRowCount(0);
    for (const auto &value : overview.value("audit").toArray())
    {
        // 保存当前需要展示的维护事件。
        const auto record = value.toObject();
        // 保存当前维护事件的显示行。
        const int row = m_audit->rowCount();
        m_audit->insertRow(row);
        m_audit->setItem(row, 0,
                         new QTableWidgetItem(localTime(record.value("occurredAt").toString())));
        m_audit->setItem(row, 1, new QTableWidgetItem(record.value("actor").toString()));
        m_audit->setItem(row, 2,
                         new QTableWidgetItem(actionLabel(record.value("action").toString())));
        m_audit->setItem(row, 3, new QTableWidgetItem(record.value("runId").toString()));
        m_audit->setItem(
            row, 4,
            new QTableWidgetItem(record.value("stationId").toInt() > 0
                                     ? QString::number(record.value("stationId").toInt())
                                     : QString()));
        m_audit->setItem(row, 5, new QTableWidgetItem(record.value("reason").toString()));
    }
    m_backupDirectory = overview.value("backupDirectory").toString();
    m_backupDirectoryLabel->setText(m_backupDirectory.isEmpty() ? QStringLiteral("—")
                                                                : m_backupDirectory);
    m_restoredDirectory = overview.value("restoredDirectory").toString();
    if (!m_settingsLoaded || !m_timeoutDirty)
    {
        // 设置后台快照时不把控件变动误记为未保存编辑。
        const QSignalBlocker runBlocker(m_runSeconds), stepBlocker(m_stepSeconds);
        m_runSeconds->setValue(overview.value("runTimeoutSeconds").toInt(120));
        m_stepSeconds->setValue(overview.value("stepTimeoutSeconds").toInt(30));
        m_settingsLoaded = true;
    }
    updateAuthorization();
}

void MaintenanceDialog::updateAuthorization()
{
    // 保存当前控制器核验后的管理员权限。
    const bool authorized = m_controller->adminAuthorized();
    // 保存凭据是否已经设置，损坏配置也按已设置处理。
    const bool configured = m_controller->adminConfigured();
    if (m_authorized && !authorized)
    {
        m_exceptions->setRowCount(0);
        m_suspended->setRowCount(0);
        m_audit->setRowCount(0);
        m_stopped->setChecked(false);
        m_reset->setChecked(false);
        m_manualReason->clear();
        m_resumeReason->clear();
        m_password->clear();
        m_confirmation->clear();
        showMessage(QStringLiteral("会话已结束，请重新登录"));
    }
    m_authorized = authorized;
    m_authState->setText(authorized ? QStringLiteral("%1 · 已登录").arg(m_controller->adminActor())
                         : configured ? QStringLiteral("未登录")
                                      : QStringLiteral("未设置管理员"));
    m_user->setEnabled(!authorized);
    m_user->setPlaceholderText(configured ? QStringLiteral("管理员名")
                                          : QStringLiteral("设置管理员名"));
    m_password->setEnabled(!authorized);
    m_confirmation->setEnabled(!authorized && !configured);
    m_confirmation->setVisible(!configured);
    m_confirmationLabel->setVisible(!configured);
    m_setup->setEnabled(!authorized && !configured);
    m_login->setEnabled(!authorized && configured);
    m_logout->setEnabled(authorized);
    for (auto *button : {m_openData, m_openTimeouts, m_openExceptions, m_openAudit})
        button->setEnabled(authorized && !m_restartRequested);
    for (auto *table : {m_exceptions, m_suspended, m_audit})
        table->setEnabled(authorized);
    for (auto *reason : {m_manualReason, m_resumeReason})
        reason->setEnabled(authorized);
    m_stopped->setEnabled(authorized);
    m_reset->setEnabled(authorized);
    m_runSeconds->setEnabled(authorized && !m_busy);
    m_stepSeconds->setEnabled(authorized && !m_busy);
    m_refresh->setEnabled(authorized && !m_busy);
    m_backup->setEnabled(authorized && !m_busy);
    m_validate->setEnabled(authorized && !m_busy);
    m_restore->setEnabled(authorized && !m_busy);
    m_activate->setEnabled(authorized && !m_busy && !m_restoredDirectory.isEmpty() &&
                           !m_restartRequested);
    m_saveTimeouts->setEnabled(authorized && !m_busy);
    updateSelectionActions();
}

void MaintenanceDialog::updateSelectionActions()
{
    // 检查当前所选异常是否仍允许进入人工处置准备阶段。
    bool eligible = m_exceptions->currentRow() >= 0;
    if (eligible)
    {
        // 读取与所选运行编号绑定的最新后台快照。
        const auto record = m_exceptions->item(m_exceptions->currentRow(), 0)
                                ->data(Qt::UserRole)
                                .value<QJsonObject>();
        eligible = !record.contains("canManualDisposition") ||
                   record.value("canManualDisposition").toBool();
    }
    m_resolve->setEnabled(m_controller->adminAuthorized() && !m_busy && eligible &&
                          m_stopped->isChecked() &&
                          m_manualReason->toPlainText().trimmed().size() >= 5);
    m_resume->setEnabled(m_controller->adminAuthorized() && !m_busy &&
                         m_suspended->currentRow() >= 0 && m_reset->isChecked() &&
                         m_resumeReason->toPlainText().trimmed().size() >= 5);
}

void MaintenanceDialog::showMessage(const QString &message, bool error)
{
    m_status->setText(message);
    m_status->setStyleSheet(error ? "QLabel { color:#a82432; }" : "QLabel { color:#333333; }");
    for (auto *status : m_windowStatuses)
    {
        status->setText(message);
        status->setStyleSheet(error ? "QLabel { color:#a82432; }" : "QLabel { color:#333333; }");
    }
}

void MaintenanceDialog::setBusy(bool busy)
{
    m_busy = busy;
    updateAuthorization();
}

void MaintenanceDialog::backup()
{
    if (!m_controller->adminAuthorized() || m_busy)
        return;
    // 建议使用毫秒时间戳生成尚不存在的备份文件名。
    const auto suggested =
        QDir(m_backupDirectory)
            .filePath("workbench-" +
                      QDateTime::currentDateTimeUtc().toString("yyyyMMddTHHmmsszzz") + ".sqlite");
    // 捕获管理员为本次备份选择的目标路径。
    const auto path = QFileDialog::getSaveFileName(
        m_dataWindow, QStringLiteral("备份数据库"), suggested,
        QStringLiteral("SQLite 备份 (*.sqlite)"), nullptr, QFileDialog::DontConfirmOverwrite);
    if (path.isEmpty())
        return;
    if (QFileInfo::exists(path))
    {
        showMessage(QStringLiteral("备份文件已存在，请选择新的文件名。"), true);
        return;
    }
    setBusy(true);
    showMessage(QStringLiteral("正在生成并验证数据库备份……"));
    m_controller->backupDatabase(path);
}

void MaintenanceDialog::validateBackup()
{
    if (!m_controller->adminAuthorized() || m_busy)
        return;
    // 捕获管理员需要检查的备份文件。
    const auto path = QFileDialog::getOpenFileName(m_dataWindow, QStringLiteral("验证数据库备份"),
                                                   m_backupDirectory,
                                                   QStringLiteral("SQLite 数据库 (*.sqlite *.db)"));
    if (path.isEmpty())
        return;
    setBusy(true);
    showMessage(QStringLiteral("正在检查备份内容和完整性……"));
    m_controller->validateDatabaseBackup(path);
}

void MaintenanceDialog::restore()
{
    if (!m_controller->adminAuthorized() || m_busy)
        return;
    // 固定本次恢复使用的备份来源。
    const auto backupPath = QFileDialog::getOpenFileName(
        m_dataWindow, QStringLiteral("选择要恢复的备份"), m_backupDirectory,
        QStringLiteral("SQLite 数据库 (*.sqlite *.db)"));
    if (backupPath.isEmpty())
        return;
    // 捕获新数据目录，后台还会重新检查其空目录及锁定条件。
    const auto directory =
        QFileDialog::getExistingDirectory(m_dataWindow, QStringLiteral("选择新空数据目录"));
    if (directory.isEmpty())
        return;
    if (!QDir(directory)
             .entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)
             .isEmpty())
    {
        showMessage(QStringLiteral("恢复目录必须为空，请新建一个数据目录。"), true);
        return;
    }
    if (QMessageBox::question(
            m_dataWindow, QStringLiteral("恢复备份到新目录"),
            QStringLiteral("将以下备份恢复到新目录？\n备份：%1\n新目录：%2\n"
                           "原库和备份后的新记录仍保留在原目录，恢复成功后才能切换。")
                .arg(backupPath, directory)) != QMessageBox::Yes)
        return;
    setBusy(true);
    showMessage(QStringLiteral("正在验证并恢复数据库，原库保留……"));
    m_controller->restoreDatabase(backupPath, directory);
}

void MaintenanceDialog::resolveSelected()
{
    if (!m_resolve->isEnabled())
        return;
    // 固定当前选中的运行行，刷新后也不改处置目标。
    const int row = m_exceptions->currentRow();
    // 固定所选运行所属的工位编号。
    const int stationId = m_exceptions->item(row, 0)->text().toInt();
    // 固定待结算运行编号，确认期间列表刷新也不更换目标。
    const auto runId = m_exceptions->item(row, 2)->text();
    // 保存二次确认框中需要展示的产品 SN。
    const auto sn = m_exceptions->item(row, 1)->text();
    // 固定本次人工结算的完整原因。
    const auto reason = m_manualReason->toPlainText().trimmed();
    if (QMessageBox::question(
            m_exceptionWindow, QStringLiteral("确认人工结算"),
            QStringLiteral("确认旧设备已停止，并结算下列测试？\n工位：%1\n产品 SN：%2\n"
                           "测试编号：%3\n原因：%4\n记录将保留 UNKNOWN，工位随后停用。")
                .arg(stationId)
                .arg(sn, runId, reason)) != QMessageBox::Yes)
        return;
    m_stopped->setChecked(false);
    m_controller->resolveException(stationId, runId, reason, true);
}

void MaintenanceDialog::resumeSelected()
{
    if (!m_resume->isEnabled())
        return;
    // 固定本次需要重新启用的工位编号。
    const int stationId = m_suspended->item(m_suspended->currentRow(), 0)->text().toInt();
    // 保存管理员填写的设备复位或更换原因。
    const auto reason = m_resumeReason->toPlainText().trimmed();
    if (QMessageBox::question(
            m_exceptionWindow, QStringLiteral("确认重新启用"),
            QStringLiteral("确认工位 %1 的旧设备已复位或更换，并重新建立连接？\n原因：%2\n"
                           "旧测试的迟到结果仍不能覆盖人工结算记录。")
                .arg(stationId)
                .arg(reason)) != QMessageBox::Yes)
        return;
    m_reset->setChecked(false);
    m_controller->resumeStation(stationId, reason, true);
}
