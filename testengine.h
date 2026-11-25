#pragma once
#include "stationendpoint.h"
#include "framecodec.h"
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <memory>
#include <map>
class QTcpSocket;
class QTimer;

class TestEngine final : public QObject
{
    Q_OBJECT
  public:
    // 创建测试引擎，由 parent 管理对象生命周期。
    explicit TestEngine(QObject *parent = nullptr);
    // 设置整轮和步骤的超时毫秒数，下一次新测试或恢复计时使用该值。
    void configureTimeouts(qint64 runMs, qint64 stepMs);
    // 在接入工位前加载人工结算编号和持久化停用工位。
    void loadSafetyState(const QJsonArray &closedRuns, const QJsonArray &suspendedStations);
    // 按 endpoints 建立工位连接，并从 recovery 恢复待核对测试。
    void initialize(const QList<StationEndpoint> &endpoints, const QJsonArray &recovery = {});
    // 为 run 申请持久化占用，成功后才向工位下发测试。
    void requestStart(const QJsonObject &run);
    // 将 recipe 作为 stationId 工位的只读配方预览发送。
    void previewRecipe(int stationId, const QJsonObject &recipe);
    // 处理 run 的预约结果；ok 表示成功，error 保存失败原因。
    void reserved(const QJsonObject &run, bool ok, const QString &error);
    // 处理 result 的归档回报；ok、duplicate 和 error 表示成功、重复及失败原因。
    void resultSaved(const QJsonObject &result, bool ok, bool duplicate, const QString &error);
    // 处理 runId 的中断落库回报，依据 ok 和 error 决定释放或重试。
    void interruptionRecorded(const QString &runId, bool ok, const QString &error);
    // 向 stationId 工位请求取消，继续等待真实终态。
    void cancel(int stationId);
    // 停止计时器和网络接入，为工作线程退出做准备。
    void shutdown();
  public slots:
    // 为 stationId 工位手动发起限频的只读状态核对。
    void retryReconciliation(int stationId);
    // 冻结离线的 runId，等待管理员处置事务提交。
    void prepareManualDisposition(int stationId, const QString &runId);
    // 处理人工处置事务结果，成功后停用工位，失败时恢复原占用。
    void manualDispositionRecorded(int stationId, const QString &runId, bool ok,
                                   const QString &error);
    // 管理员确认设备复位后，恢复 stationId 的身份核对与连接。
    void resumeStation(int stationId);
    // 仅在全部工位闲置时冻结接入，为恢复数据切换建立原子门禁。
    void prepareRestore();
    // 恢复失败或暂不切换时，解除闲置门禁并重新连接工位。
    void finishRestorePause();
  signals:
    // 向界面发布 view 工位状态和按钮可用性快照。
    void stationUpdated(const QJsonObject &view);
    // 发布 message 日志，warning 标明是否为警告。
    void logMessage(const QString &message, bool warning);
    // 请求数据库先保存 run 身份并占用产品和工位。
    void reserveRequested(const QJsonObject &run);
    // 请求数据库校验并持久化 result 测试终态。
    void saveRequested(const QJsonObject &result);
    // 请求将 runId 记录为中断，reason 说明中断原因。
    void interrupted(const QString &runId, const QString &reason);
    // 请求持久化 runId 的超时事件，测试终态仍由设备提供。
    void runTimedOut(const QString &runId, int stationId, const QString &reason);
    // 回报人工处置的冻结结果，只有成功才可提交数据库处置事务。
    void manualDispositionPrepared(int stationId, const QString &runId, bool ok,
                                   const QString &error);
    // 检出已人工结算的旧运行，要求持久化停用并隔离该工位。
    void stationSuspensionRequired(int stationId, const QString &runId, const QString &reason);
    // 回报是否已在所有工位闲置时成功冻结网络和新测试。
    void restorePrepared(bool ok, const QString &error);

  private:
    struct Link
    {
        // 保存该工位的编号和服务器连接配置。
        StationEndpoint endpoint;
        // 持有该工位专用的 TCP 连接。
        QTcpSocket *socket = nullptr;
        // 解析该连接收到的拆分或粘连报文。
        Wire::Parser parser;
        // session 标识当前连接，remoteRun 保存远端占用编号。
        // ackPending 保存等待工位确认的归档编号。
        QString session, remoteRun, ackPending;
        // run 保存执行请求，view 保存界面快照。
        // archivedResult 保存已归档终态，recipePreview 缓存配方预览。
        QJsonObject run, view, archivedResult, recipePreview;
        // 保存各步骤的限值、测量值和当前判定。
        QJsonArray progress;
        // 保存等待数据库确认的中断原因。
        QString interruptionReason;
        // 记录下一次重试中断落库的时刻。
        qint64 interruptionRetryAt = 0;
        // reconnectAt 保存重连时刻，connectStarted 保存本次连接起点。
        // lastRx、lastPing 分别保存最近收包和发送心跳的时刻。
        // startDeadline、queryDeadline 分别限定受理和查询应答期限。
        // queryRetryAt 保存查询重试时刻，cancelDeadline 保存取消核对时刻。
        // ackDeadline 保存下一次归档确认重发时刻。
        qint64 reconnectAt = 0, connectStarted = 0, lastRx = 0, lastPing = 0, startDeadline = 0,
               queryDeadline = 0, queryRetryAt = 0, cancelDeadline = 0, ackDeadline = 0;
        // 记录上一次状态查询时刻，用于限制手动查询频率。
        qint64 lastQueryAt = -1000;
        // sequence 生成本连接报文序号，startSequence 记录测试动作序号以匹配受理应答。
        quint32 sequence = 0, startSequence = 0;
        // backoff 保存重连退避间隔，queryBackoff 保存状态查询退避间隔。
        int backoff = 1000, queryBackoff = 1000;
        // online 标记身份验证通过，dispatched 标记动作已下发。
        // awaitingAck 标记等待受理，cancelling 标记正在取消。
        // interrupting 标记中断待落库，interruptionWriting 标记写入在途。
        // remoteUnknown 标记远端无记录，reconciling 标记正在核对。
        bool online = false, dispatched = false, awaitingAck = false, cancelling = false,
             interrupting = false, interruptionWriting = false, remoteUnknown = false,
             reconciling = false;
        // runDeadline、stepDeadline 限定整轮和下一步骤完成的时刻。
        qint64 runDeadline = 0, stepDeadline = 0;
        // 保存本轮启动时捕获的超时配置，后续设置不影响在途测试。
        qint64 runLimitMs = 0, stepLimitMs = 0;
        // 记录首次收到有效测量的步骤，防止重复进度延长期限。
        QSet<int> progressedSteps;
        // timedOut 标记本轮超时，timeoutCancelSent 防止重复下发取消。
        bool timedOut = false, timeoutCancelSent = false;
        // 保存超时的具体原因，供界面和日志显示。
        QString timeoutReason;
        // safetySuspended 标记工位停用，manualPending 标记人工处置事务冻结。
        bool safetySuspended = false, manualPending = false;
    };
    struct PendingResult
    {
        // 保存尚未成功提交数据库的原始测试终态。
        QJsonObject result;
        // 标记该终态是否已有一次数据库写入在途。
        bool writing = false;
        // 记录该终态下一次允许重试落库的时刻。
        qint64 retryAt = 0;
    };
    // 按工位编号持有各工位独立的连接和执行状态。
    std::map<int, std::unique_ptr<Link>> m_links;
    // 按 runId 保存待提交终态，保证失败后继续重试。
    QHash<QString, PendingResult> m_results;
    // 提供不受系统时间调整影响的超时计时基准。
    QElapsedTimer m_clock;
    // 周期驱动连接恢复、状态核对和归档重试。
    QTimer *m_timer = nullptr;
    // 标记引擎正在停止，阻止继续发送或重试。
    bool m_stopping = false;
    // m_runTimeoutMs、m_stepTimeoutMs 为新运行采用的超时配置。
    qint64 m_runTimeoutMs = 120000, m_stepTimeoutMs = 30000;
    // 保存已人工结算的运行编号，隔离任何迟到结果。
    QSet<QString> m_closedRunIds;
    // 保存需保持停用的工位编号，覆盖后续初始化连接。
    QSet<int> m_suspendedStations;
    // 标记恢复切换已冻结新测试和工位接入。
    bool m_restorePaused = false;
    // 检查各工位期限并推进重连、核对和落库重试。
    void tick();
    // 向 link 发送 kind 类型报文，并合入 extra 附加字段，返回报文序号。
    quint32 send(Link &link, const QString &kind, QJsonObject extra = {});
    // 读取 link 的接收字节并交给协议解析器。
    void read(Link &link);
    // 校验并处理 link 收到的 message 业务报文。
    void receive(Link &link, const QJsonObject &message);
    // 处理 link 的 result 终态，校验身份后申请归档。
    void terminal(Link &link, const QJsonObject &result);
    // 返回 link 当前允许核对的测试编号，无可核对任务时返回空。
    QString reconciliationRunId(const Link &link) const;
    // 判断 link 是否存在可手动核对且已过冷却期的任务。
    bool canReconcile(const Link &link) const;
    // 清理 link 的状态查询及取消核对计时信息。
    void resetReconciliation(Link &link);
    // 只读查询 link 上的 runId 状态，保留测试占用且不重发动作。
    void query(Link &link, const QString &runId);
    // 更新 link 的 execution、detail 显示内容并发布工位快照。
    void publish(Link &link, const QString &execution = {}, const QString &detail = {});
    // 按 reason 处理 link 上远端明确无记录的情况，先落库再释放占用。
    void clearUnknown(Link &link, const QString &reason);
    // 判断当前离线运行是否可冻结以提交人工处置。
    bool canManuallyDispose(const Link &link) const;
    // 在首次下发或恢复时建立执行期限，已有期限不会被刷新。
    void armExecutionTimeouts(Link &link);
    // 只清理执行计时，保留界面上的本轮超时记录。
    void clearExecutionTimeouts(Link &link);
    // 记录超时并请求一次取消，之后只读核对实际终态。
    void checkExecutionTimeout(Link &link, qint64 now);
    // 隔离已人工结算的运行并要求持久化停用工位。
    void suspendForClosedRun(Link &link, const QString &runId);
};
