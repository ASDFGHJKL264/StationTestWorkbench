#pragma once
#include "stationendpoint.h"
#include "repository.h"
#include <QObject>
#include <QThread>
#include <QJsonObject>
#include <QJsonArray>
#include <QList>
#include <QHash>
#include <memory>
#include "runtimeconfiguration.h"

class TestEngine;
class Repository;
class AsyncFileLogger;
class AdminAccess;
class DatabaseMaintenance;
class QTimer;

class WorkbenchController final : public QObject
{
    Q_OBJECT
  public:
    // 创建界面与后台之间的控制器，由 parent 管理生命周期。
    explicit WorkbenchController(QObject *parent = nullptr);
    // 销毁前停止网络、数据库和日志工作线程。
    ~WorkbenchController() override;
    // 以 dataDirectory 保存数据和日志，按 endpoints 配置启动后台。
    void initialize(const QString &dataDirectory, const QList<StationEndpoint> &endpoints);
    // 依次停止网络接入、排空数据库操作并关闭日志线程。
    void shutdown();
    // 判断本机管理员是否已设置，损坏配置也禁止重新覆盖。
    bool adminConfigured() const;
    // 判断当前管理员会话是否仍有效。
    bool adminAuthorized() const;
    // 返回当前已认证管理员的账号。
    QString adminActor() const;
    // 首次设置管理员账号并建立会话，密码仅交给认证对象。
    bool configureAdministrator(const QString &user, const QString &password,
                                const QString &confirmation, QString *error);
    // 登录本机管理员账号，失败时填写原因。
    bool loginAdministrator(const QString &user, const QString &password, QString *error);
    // 清除当前管理员会话。
    void logoutAdministrator();
  public slots:
    // 以 sn 产品和 recipeVersion 配方在 stationId 工位启动一次新测试。
    void startTest(int stationId, const QString &sn, const QString &recipeVersion);
    // 请求取消 stationId 工位当前测试，等待工位确认终态。
    void cancelTest(int stationId);
    // 转发 stationId 工位的手动只读核对请求。
    void retryReconciliation(int stationId);
    // 按 snFilter 查询历史，beforeId 指定上一页末尾编号。
    void refreshHistory(const QString &snFilter = {}, int beforeId = 0);
    // 按 filter 复合条件查询历史，beforeId 为稳定分页游标。
    void refreshFilteredHistory(const HistoryFilter &filter, int beforeId = 0);
    // 将 recipe 配方交给数据库发布为不可变版本。
    void publishRecipe(const QJsonObject &recipe);
    // 将 recipeVersion 对应配方发送为 stationId 工位预览。
    void previewRecipe(int stationId, const QString &recipeVersion);
    // 请求重新读取已发布配方列表。
    void requestRecipes();
    // 将 runId 测试报告异步导出到 path。
    void exportReport(const QString &runId, const QString &path);
    // 将全部历史异常 SN 异步导出到 path。
    void exportFailedSns(const QString &path);
    // 读取待处置任务、停用工位和最近审计记录。
    void refreshMaintenance();
    // 在独立维护线程生成一致性备份。
    void backupDatabase(const QString &destinationPath);
    // 检查备份的完整性、模式和追溯内容。
    void validateDatabaseBackup(const QString &path);
    // 将备份恢复到新空目录，保留当前数据。
    void restoreDatabase(const QString &backupPath, const QString &newDataDirectory);
    // 为已验证的新库保存启动指向并请求重启。
    void activateRestoredDatabase(const QString &newDataDirectory);
    // 经设备停止确认后，冻结工位再事务化人工结算。
    void resolveException(int stationId, const QString &runId, const QString &reason,
                          bool confirmedStopped);
    // 经设备复位确认后，恢复已停用工位的连接资格。
    void resumeStation(int stationId, const QString &reason, bool confirmedReset);
    // 保存之后新测试使用的整轮和步骤期限。
    void setExecutionTimeouts(int runSeconds, int stepSeconds);
  signals:
    // 向界面转发 station 工位状态快照。
    void stationUpdated(const QJsonObject &station);
    // 向界面发布 rows 历史页，more 表示还有下一页。
    void historyReady(const QJsonArray &rows, bool more);
    // 向界面发布当前历史查询的 error 失败原因。
    void historyFailed(const QString &error);
    // 向界面发布 recipes 可选配方列表。
    void recipesReady(const QJsonArray &recipes);
    // 向编辑窗口回报 recipe 的实际发布结果，失败时填写 error。
    void recipePublishFinished(const QJsonObject &recipe, bool ok, const QString &error);
    // 向界面发布 message 日志，warning 标记警告级别。
    void logMessage(const QString &message, bool warning = false);
    // 通知后台初始化是否成功，ok 为成功状态。
    void ready(bool ok);
    // 通知 path 报告导出的 ok 状态和 error 失败原因。
    void reportReady(const QString &path, bool ok, const QString &error);
    // 通知 path 清单导出的 ok 状态、count 数量及 error 失败原因。
    void failedSnsExported(const QString &path, bool ok, qint64 count, const QString &error);
    // 发布维护数据和当前设置。
    void maintenanceReady(const QJsonObject &overview);
    // 提示维护操作的结果，error 标明失败。
    void maintenanceMessage(const QString &message, bool error);
    // 回报备份完成状态。
    void backupFinished(const QString &path, bool ok, const QString &error);
    // 回报备份验证结果。
    void validationFinished(const QString &path, bool ok, const QString &error);
    // 回报新数据目录恢复结果。
    void restoreFinished(const QString &newDirectory, bool ok, const QString &error);
    // 请求主程序关闭后台后切换到新数据目录。
    void restartRequested(const QString &newDirectory);

  private:
    // 将 text 日志同步到界面和文件，warning 标记警告级别。
    void log(const QString &text, bool warning = false);
    // 建立维护线程和后台回报连接。
    void initializeMaintenance();
    // 统一核对管理员会话和后台可用性。
    bool requireAdministrator(QString *error, bool requireDatabase = true) const;
    // 在数据库线程写入不含敏感认证内容的操作审计。
    void audit(const QString &actor, const QString &action, const QString &reason);
    // 每日备份和周期磁盘容量检查。
    void checkAutomaticMaintenance();
    // 将维护提示同时发布到面板和运行日志。
    void maintenanceNotice(const QString &message, bool error);
    // m_engineThread 承载网络测试，m_databaseThread 承载追溯存储。
    QThread m_engineThread, m_databaseThread;
    // 指向网络线程中的测试引擎。
    TestEngine *m_engine = nullptr;
    // 指向数据库线程中的追溯存储对象。
    Repository *m_repository = nullptr;
    // 管理写入文件日志的独立后台队列。
    AsyncFileLogger *m_logger = nullptr;
    // 缓存已发布配方，供启动和预览时按版本查找。
    QJsonArray m_recipes;
    // 缓存数据库启动时读取的待核对测试请求。
    QJsonArray m_recovery;
    // 保留当前历史复合筛选条件，供归档后自动刷新。
    HistoryFilter m_historyFilter;
    // 保存当前历史页的末尾游标，0 表示首页。
    int m_historyCursor = 0;
    // 递增标识历史查询，防止旧回报覆盖新筛选。
    quint64 m_historyRequestId = 0;
    // m_started 防止重复初始化，m_ready 表示后台可用，m_stopping 标记关闭中。
    bool m_started = false, m_ready = false, m_stopping = false;
    // 独立执行大文件备份和校验，避免占用结果归档线程。
    QThread m_maintenanceThread;
    // 指向维护线程中的备份恢复对象。
    DatabaseMaintenance *m_maintenance = nullptr;
    // 持有本机管理员摘要校验及短时会话。
    std::unique_ptr<AdminAccess> m_adminAccess;
    // 保存当前数据目录、主库路径和自动备份目录。
    QString m_dataDirectory, m_databasePath, m_backupDirectory;
    // 保存启动时读取的人工结算及工位停用信息。
    QJsonArray m_closedRuns, m_suspendedStations;
    // 保存之后新测试使用的期限。
    RuntimeConfiguration::ExecutionLimits m_executionLimits;
    // 周期触发每日备份和磁盘容量检查。
    QTimer *m_maintenanceTimer = nullptr;
    // 缓存最近工位视图，供维护界面显示。
    QHash<int, QJsonObject> m_stationViews;
    struct PendingDisposition
    {
        // 保存提交处置时已认证的账号及原因。
        QString actor, reason;
    };
    // 保存引擎冻结与数据库事务之间的处置上下文。
    QHash<QString, PendingDisposition> m_pendingDispositions;
    // 保存本次备份身份及是否为自动备份。
    QString m_backupActor;
    // 保存等待闲置门禁通过的恢复源、目标及发起账号。
    QString m_restoreSource, m_restoreTarget, m_restoreActor;
    // 保存本次成功恢复且允许切换的目录。
    QString m_restoredDirectory;
    // 记录维护任务在途状态，防止重复提交。
    bool m_backupInFlight = false, m_restoreInFlight = false, m_validationInFlight = false;
    // 标记当前备份是否使用每日保留策略。
    bool m_automaticBackup = false;
    // 区分新库生成与已恢复目录的激活门禁。
    bool m_switchingDatabase = false;
    // 对低空间警告限频，避免刷屏。
    qint64 m_lastSpaceWarning = 0;
};
