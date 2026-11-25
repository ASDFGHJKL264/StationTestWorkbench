#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QList>

class WorkbenchController;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QVBoxLayout;

class MaintenanceDialog final : public QDialog
{
    Q_OBJECT
  public:
    // 创建管理员维护窗口，由 controller 再次核验所有敏感操作。
    explicit MaintenanceDialog(WorkbenchController *controller, QWidget *parent = nullptr);

  protected:
    // 关闭维护中心时关闭子窗口并退出认证，后台任务继续运行。
    void done(int result) override;

  private:
    // 构建管理员首次设置和登录区域。
    void buildAuthentication(QVBoxLayout *layout);
    // 构建独立的备份和恢复窗口。
    void buildDataPage();
    // 构建独立的测试超时设置窗口。
    void buildTimeoutPage();
    // 构建异常任务人工处置及停用工位管理区域。
    void buildExceptionPage();
    // 构建最近一百条维护记录表。
    void buildAuditPage();
    // 为维护子窗口添加动态操作结果。
    void addWindowStatus(QDialog *window, QVBoxLayout *layout);
    // 核验会话后打开指定维护子窗口。
    void openWindow(QDialog *window);
    // 接收 overview 后更新异常、工位、审计和配置快照。
    void showOverview(const QJsonObject &overview);
    // 按当前登录和后台任务状态更新控件权限。
    void updateAuthorization();
    // 按选中身份、原因和设备确认状态更新处置按钮。
    void updateSelectionActions();
    // 显示维护结果，error 为错误提示。
    void showMessage(const QString &message, bool error = false);
    // 设置备份或恢复任务是否仍在执行。
    void setBusy(bool busy);
    // 选择新的备份文件名并请求一致性备份。
    void backup();
    // 选择备份并请求内容和完整性检查。
    void validateBackup();
    // 选择备份及新空目录，确认后请求还原。
    void restore();
    // 捕获当前运行身份，确认设备停止后请求人工处置。
    void resolveSelected();
    // 捕获停用工位身份，确认设备复位后请求重新启用。
    void resumeSelected();
    // 负责认证与所有后台维护操作的应用控制器。
    WorkbenchController *m_controller;
    // m_user 输入管理员名，m_password 输入密码，m_confirmation 仅用于首次确认。
    QLineEdit *m_user, *m_password, *m_confirmation;
    // 显示是否已配置、当前管理员及会话状态。
    QLabel *m_authState;
    // m_setup 首次设置，m_login 登录，m_logout 立即结束会话。
    QPushButton *m_setup, *m_login, *m_logout;
    // 分别承载备份恢复、测试时限、异常处置及审计记录。
    QDialog *m_dataWindow, *m_timeoutWindow, *m_exceptionWindow, *m_auditWindow;
    // 四个入口仅在管理员会话有效时可用。
    QPushButton *m_openData, *m_openTimeouts, *m_openExceptions, *m_openAudit;
    // 保存子窗口的动态操作结果标签。
    QList<QLabel *> m_windowStatuses;
    // 与首次确认输入框一起隐藏的标题。
    QLabel *m_confirmationLabel;
    // 显示当前数据目录下的自动备份位置。
    QLabel *m_backupDirectoryLabel;
    // m_backup 立即备份，m_validate 验证备份，m_restore 恢复，m_activate 切换新库。
    QPushButton *m_backup, *m_validate, *m_restore, *m_activate;
    // 保存最近一次后台快照中的自动备份目录。
    QString m_backupDirectory;
    // 保存本窗口成功还原的新数据目录，供重启按钮使用。
    QString m_restoredDirectory;
    // m_runSeconds 设置整轮上限，m_stepSeconds 设置单步上限。
    QSpinBox *m_runSeconds, *m_stepSeconds;
    // 保存之后新测试使用的超时设置。
    QPushButton *m_saveTimeouts;
    // m_exceptions 显示待核对任务，m_suspended 显示已停用工位。
    QTableWidget *m_exceptions, *m_suspended;
    // m_manualReason 说明人工结算原因，m_resumeReason 说明重新启用原因。
    QPlainTextEdit *m_manualReason, *m_resumeReason;
    // m_stopped 确认旧设备已停止，m_reset 确认重新启用前设备已复位。
    QCheckBox *m_stopped, *m_reset;
    // m_resolve 提交人工结算，m_resume 提交重新启用。
    QPushButton *m_resolve, *m_resume;
    // 显示最近一百条维护和异常事件。
    QTableWidget *m_audit;
    // 显示当前维护操作的结果和失败原因。
    QLabel *m_status;
    // 重新读取待处置任务和维护记录。
    QPushButton *m_refresh;
    // 标记备份或恢复任务仍在执行，避免重复提交。
    bool m_busy = false;
    // 记录上一次权限状态，识别会话过期或退出。
    bool m_authorized = false;
    // 标记超时控件中存在尚未保存的操作员修改。
    bool m_timeoutDirty = false;
    // 标记首次配置快照已经填入超时控件。
    bool m_settingsLoaded = false;
    // 标记已请求重启，避免连续点击切换新库。
    bool m_restartRequested = false;
};
