#ifndef ASYNCFILELOGGER_H
#define ASYNCFILELOGGER_H

#include <QObject>
#include <QList>
#include <QString>
#include <QThread>

class LogFileWorker;

// GUI-side bounded admission; only the worker accesses the filesystem.
class AsyncFileLogger final : public QObject
{
    Q_OBJECT
  public:
    // 创建写入 path 的异步日志器，由 parent 管理生命周期。
    explicit AsyncFileLogger(const QString &path, QObject *parent = nullptr);
    // 销毁日志器前排空待写内容并停止日志线程。
    ~AsyncFileLogger() override;
    // 限制排队和正在写入的日志总条数。
    static constexpr int MaximumPendingLines = 256;
    // 为警告日志预留队列名额。
    static constexpr int WarningReserve = 32;
    // 限制每次交给工作线程的日志批量大小。
    static constexpr int BatchSize = 32;
    // 将 line 日志加入有界队列，warning 表示优先保留的警告。
    bool append(const QString &line, bool warning = false);
    // 排空剩余日志并等待工作线程安全退出。
    void shutdown();
    // 返回排队中和正在写入的日志总条数。
    int pendingLines() const
    {
        return m_queue.size() + m_inFlight;
    }
    // 返回因队列拥挤而丢弃的普通日志累计条数。
    quint64 droppedNormal() const
    {
        return m_droppedNormal;
    }
    // 返回因队列拥挤而丢弃的警告日志累计条数。
    quint64 droppedWarnings() const
    {
        return m_droppedWarnings;
    }
    // 返回工作线程成功写入的日志累计条数。
    quint64 writtenLines() const
    {
        return m_written;
    }
    // 返回工作线程未能写入的日志累计条数。
    quint64 failedLines() const
    {
        return m_failed;
    }

  signals:
    // 通知日志写入失败，reason 为文件操作失败原因。
    void writeFailed(const QString &reason);
    // 通知队列过载，normal 和 warnings 分别为两类丢弃条数。
    void overflow(quint64 normal, quint64 warnings);

  private:
    struct Entry
    {
        // 保存已限制长度并清理换行的单条日志文本。
        QString line;
        // 标记该条日志是否为需要优先保留的警告。
        bool warning;
    };
    // 安排下一轮批量写入，避免重复排队调度。
    void schedule();
    // 从队列取出一批日志交给文件线程。
    void dispatch();
    // 承载日志文件读写及轮转操作的工作线程。
    QThread m_thread;
    // 指向仅在日志线程中访问文件的工作对象。
    LogFileWorker *m_worker = nullptr;
    // 保存尚未发送给文件线程的日志条目。
    QList<Entry> m_queue;
    // 记录已发送给文件线程但尚未回报的日志条数。
    int m_inFlight = 0;
    // 标记是否已经安排了一轮待执行的调度。
    bool m_scheduled = false;
    // 标记日志器正在关闭，阻止继续接收日志。
    bool m_stopping = false;
    // m_droppedNormal 统计丢弃的普通日志，m_droppedWarnings 统计丢弃的警告。
    quint64 m_droppedNormal = 0, m_droppedWarnings = 0;
    // m_written 统计写入成功条数，m_failed 统计写入失败条数。
    quint64 m_written = 0, m_failed = 0;
};

#endif
