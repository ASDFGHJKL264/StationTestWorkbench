#pragma once
#include <QObject>
#include <QSqlDatabase>
#include <QJsonObject>
#include <QJsonArray>
#include <QDate>

// Invalid dates leave that bound open. Both dates refer to the local calendar day.
struct HistoryFilter
{
    // 保存需要精确匹配的产品 SN，空字符串表示不限。
    QString sn;
    // startDate、endDate 分别限定开始和结束的本地日期，无效日期表示不限。
    QDate startDate, endDate;
    // 限定查询的工位编号，0 表示全部工位。
    int stationId = 0;
    // 限定质量判定，空字符串表示全部判定。
    QString verdict;

    // 比较 other 与当前筛选条件是否完全一致。
    bool operator==(const HistoryFilter &other) const
    {
        return sn == other.sn && startDate == other.startDate && endDate == other.endDate &&
               stationId == other.stationId && verdict == other.verdict;
    }
};

// Every method runs on the database thread; no connection is shared with the GUI.
class Repository final : public QObject
{
    Q_OBJECT
  public:
    // 创建追溯存储对象，由 parent 管理生命周期。
    explicit Repository(QObject *parent = nullptr);
    // 销毁前关闭并移除本线程的数据库连接。
    ~Repository() override;
  public slots:
    // 打开 databasePath 数据库，初始化表结构并恢复待核对占用。
    void initialize(const QString &databasePath);
    // 持久化 request 的测试身份，同时独占产品 SN 和工位。
    void reserveRun(const QJsonObject &request);
    // 校验 result 并在同一事务中保存终态和步骤，重复结果按编号核对。
    void saveResult(const QJsonObject &result);
    // 按 snFilter 精确查询历史，beforeId 为上一页末尾编号。
    void queryHistory(const QString &snFilter, int beforeId = 0);
    // 按 filter 查询历史页，beforeId 为分页游标，requestId 用于关联异步回报。
    void queryFilteredHistory(const HistoryFilter &filter, int beforeId = 0, quint64 requestId = 0);
    // 将 runId 对应的绑定配方和结果导出为 path 报告。
    void exportReport(const QString &runId, const QString &path);
    // 将历史上出现过 FAIL 的去重 SN 导出到 path 清单。
    void exportFailedSns(const QString &path);
    // 读取全部已发布配方，供界面选择和预览。
    void listRecipes();
    // 发布 recipe 新版本，相同版本仅允许相同内容。
    void publishRecipe(const QJsonObject &recipe);
    // 按 reason 将 runId 记为中断，并解除已核对的恢复占用。
    void markInterrupted(const QString &runId, const QString &reason);
    // 读取仍需核对的测试、停用工位及最近操作记录。
    void queryMaintenance();
    // 在确认设备停止后，审计结算 runId 并停用所属工位。
    void resolveManually(int stationId, const QString &runId, const QString &actor,
                         const QString &reason, bool confirmedStopped);
    // 记录 actor 的复位确认，并解除 stationId 工位的持久停用。
    void resumeStation(int stationId, const QString &actor, const QString &reason);
    // 保存 runId 的本地超时记录，不替代设备终态和质量判定。
    void recordTimeout(const QString &runId, int stationId, const QString &reason);
    // 因已人工处置的旧结果重新出现，持久停用 stationId 工位。
    void recordStationSuspension(int stationId, const QString &runId, const QString &reason);
    // 保存维护操作的操作者、动作、关联测试和原因。
    void recordAudit(const QString &actor, const QString &action, const QString &runId,
                     int stationId, const QString &reason);
    // 关闭数据库连接并清理连接名，允许安全退出线程。
    void shutdown();
  signals:
    // 回报重启后仍需与工位核对的 requests 请求列表。
    void recoverableRuns(const QJsonArray &requests);
    // 回报已人工结算测试和停用工位，供网络引擎恢复安全隔离。
    void safetyStateReady(const QJsonArray &closedRuns, const QJsonArray &suspendedStations);
    // 回报异常列表、停用工位和最近审计记录。
    void maintenanceReady(const QJsonObject &overview);
    // 回报 stationId/runId 人工结算事务是否成功。
    void manualDispositionSaved(int stationId, const QString &runId, bool ok, const QString &error);
    // 回报工位重新启用事务是否成功。
    void stationResumed(int stationId, bool ok, const QString &error);
    // 回报 runId 中断事务的 ok 成功状态及 error 失败原因。
    void runInterrupted(const QString &runId, bool ok, const QString &error);
    // 回报初始化的 ok 成功状态及 error 失败原因。
    void initialized(bool ok, const QString &error);
    // 回报 request 预约的 ok 成功状态及 error 失败原因。
    void runReserved(const QJsonObject &request, bool ok, const QString &error);
    // 回报 result 归档状态，ok 为成功，duplicate 为重复，error 为失败原因。
    void resultSaved(const QJsonObject &result, bool ok, bool duplicate, const QString &error);
    // 回报 rows 历史记录，more 表示还有下一页。
    void historyReady(const QJsonArray &rows, bool more);
    // 携带 requestId 回报 rows 历史页，more 表示还有下一页。
    void historyQueryReady(quint64 requestId, const QJsonArray &rows, bool more);
    // 携带 requestId 回报 error，供界面忽略过时查询的失败。
    void historyQueryFailed(quint64 requestId, const QString &error);
    // 回报 path 报告导出的 ok 状态及 error 失败原因。
    void reportReady(const QString &path, bool ok, const QString &error);
    // 回报 path 清单导出的 ok 状态、count 数量及 error 失败原因。
    void failedSnsExported(const QString &path, bool ok, qint64 count, const QString &error);
    // 回报可用的 recipes 已发布配方列表。
    void recipesReady(const QJsonArray &recipes);
    // 回报 recipe 已实际提交或发布失败，error 保留可显示的失败原因。
    void recipePublishFinished(const QJsonObject &recipe, bool ok, const QString &error);
    // 发布 message 数据库操作错误，供控制器记录日志。
    void operationError(const QString &message);

  private:
    // 确认当前调用发生在数据库对象所属线程。
    bool checkThread() const;
    // 检查连接是否已就绪，未就绪时填写 error。
    bool ready(QString *error) const;
    // 开启立即写事务，失败时填写 error。
    bool begin(QString *error);
    // 提交当前事务，失败时回滚并填写 error。
    bool commit(QString *error);
    // 将一条操作记录加入当前事务，失败时填写 error。
    bool insertAudit(const QString &actor, const QString &action, const QString &runId,
                     int stationId, const QString &reason, QString *error);
    // 按 runId 读取已归档步骤，查询失败时填写 error。
    QJsonArray loadSteps(const QString &runId, QString *error);
    // 持有仅允许数据库线程访问的 SQLite 连接。
    QSqlDatabase m_db;
    // 保存 Qt 数据库连接的唯一名称，供关闭时移除。
    QString m_connectionName;
    // 标记表结构、默认配方和恢复记录均已初始化。
    bool m_ready = false;
};
