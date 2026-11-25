#include "asyncfilelogger.h"

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <algorithm>
#include <utility>

class LogFileWorker final : public QObject
{
    Q_OBJECT
  public:
    // 保存 path 日志路径，供工作线程独占文件操作。
    explicit LogFileWorker(QString path) : m_path(std::move(path)) {}
    // 将 lines 日志批量追加到文件，并在达到大小上限时轮转。
    void write(const QStringList &lines)
    {
        Q_ASSERT(QThread::currentThread() == thread());
        // 统计本批次已经成功写入的日志条数。
        int written = 0;
        // 保存本批次目录、文件或轮转操作的失败原因。
        QString error;
        if (!QDir().mkpath(QFileInfo(m_path).absolutePath()))
        {
            emit completed(lines.size(), 0, QStringLiteral("无法创建日志目录"));
            return;
        }
        // 打开日志路径对应的追加写入文件。
        QFile file(m_path);
        for (const auto &line : lines)
        {
            // 保存当前日志的 UTF-8 字节及末尾换行。
            const QByteArray bytes = line.toUtf8() + '\n';
            if (!file.isOpen() && !file.open(QIODevice::WriteOnly | QIODevice::Append))
            {
                error = file.errorString();
                break;
            }
            if (file.size() + bytes.size() > 5 * 1024 * 1024)
            {
                file.close();
                // 累计标记旧日志删除和编号迁移是否全部成功。
                bool rotated = !QFile::exists(m_path + ".9") || QFile::remove(m_path + ".9");
                for (int n = 8; rotated && n >= 1; --n)
                {
                    // 保存当前待迁移的归档日志完整路径。
                    const QString from = m_path + "." + QString::number(n);
                    if (QFile::exists(from))
                        rotated = QFile::rename(from, m_path + "." + QString::number(n + 1));
                }
                if (rotated)
                    rotated = QFile::rename(m_path, m_path + ".1");
                if (!rotated || !file.open(QIODevice::WriteOnly | QIODevice::Append))
                {
                    error = QStringLiteral("日志轮转或重新打开失败，停止本批写入");
                    break;
                }
            }
            if (file.write(bytes) != bytes.size())
            {
                error = file.errorString();
                break;
            }
            ++written;
        }
        // A flush failure makes this batch unsuccessful; it is not durable storage.
        if (file.isOpen() && !file.flush())
        {
            written = 0;
            error = file.errorString();
        }
        emit completed(lines.size(), written, error);
    }
  signals:
    // 回报 submitted 提交条数、written 成功条数和 error 失败原因。
    void completed(int submitted, int written, const QString &error);

  private:
    // 保存工作线程负责写入的日志文件路径。
    QString m_path;
};

AsyncFileLogger::AsyncFileLogger(const QString &path, QObject *parent)
    : QObject(parent), m_worker(new LogFileWorker(path))
{
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    // 接收写入回报：submitted 为提交条数，written 为成功条数，error 为失败原因。
    connect(m_worker, &LogFileWorker::completed, this,
            [this](int submitted, int written, const QString &error)
            {
                m_inFlight = qMax(0, m_inFlight - submitted);
                m_written += written;
                m_failed += submitted - written;
                if (!error.isEmpty())
                    emit writeFailed(error);
                schedule();
            });
    m_thread.setObjectName(QStringLiteral("WorkbenchFileLogThread"));
    m_thread.start();
}

AsyncFileLogger::~AsyncFileLogger()
{
    shutdown();
}

bool AsyncFileLogger::append(const QString &line, bool warning)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_stopping)
        return false;
    // 记录日志丢弃；important 标明是否增加警告丢弃计数。
    const auto drop = [this](bool important)
    {
        important ? ++m_droppedWarnings : ++m_droppedNormal;
        if (m_droppedNormal + m_droppedWarnings == 1 ||
            (m_droppedNormal + m_droppedWarnings) % 100 == 0 || important)
            emit overflow(m_droppedNormal, m_droppedWarnings);
    };
    if (!warning && pendingLines() >= MaximumPendingLines - WarningReserve)
    {
        drop(false);
        return false;
    }
    if (pendingLines() >= MaximumPendingLines)
    {
        // 定位可腾出名额的普通日志，entry 为候选队列条目。
        auto it = std::find_if(m_queue.begin(), m_queue.end(),
                               [](const Entry &entry) { return !entry.warning; });
        if (it == m_queue.end())
        {
            drop(warning);
            return false;
        }
        m_queue.erase(it);
        drop(false);
    }
    // 保存限制长度并清理换行后的单条日志文本。
    QString bounded = line.left(4096);
    bounded.replace('\r', ' ');
    bounded.replace('\n', ' ');
    if (line.size() > 4096)
        bounded += QStringLiteral(" [日志已截断]");
    m_queue.append({bounded, warning});
    schedule();
    return true;
}

void AsyncFileLogger::schedule()
{
    if (m_stopping || m_scheduled || m_inFlight || m_queue.isEmpty())
        return;
    m_scheduled = true;
    QTimer::singleShot(0, this, &AsyncFileLogger::dispatch);
}

void AsyncFileLogger::dispatch()
{
    m_scheduled = false;
    if (m_stopping || m_inFlight || m_queue.isEmpty())
        return;
    // 收集本轮准备交给文件线程的日志批次。
    QStringList batch;
    while (!m_queue.isEmpty() && batch.size() < BatchSize)
        batch.append(m_queue.takeFirst().line);
    m_inFlight += batch.size();
    // 捕获 worker 工作对象和 batch 日志副本，排队到文件线程写入。
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, batch] { worker->write(batch); }, Qt::QueuedConnection);
}

void AsyncFileLogger::shutdown()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_stopping)
        return;
    m_stopping = true;
    // 收集关闭时尚未派发的全部日志，保证退出前排空。
    QStringList remaining;
    while (!m_queue.isEmpty())
        remaining.append(m_queue.takeFirst().line);
    m_inFlight += remaining.size();
    // 捕获 worker 工作对象和 remaining 日志副本，写完后退出日志线程。
    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker, remaining]
        {
            if (!remaining.isEmpty())
                worker->write(remaining);
            QThread::currentThread()->quit();
        },
        Qt::QueuedConnection);
    // The worker does not wait for GUI callbacks. Already dispatched work precedes this.
    m_thread.wait();
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    m_worker = nullptr;
}

#include "asyncfilelogger.moc"
