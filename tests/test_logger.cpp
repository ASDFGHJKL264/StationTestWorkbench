#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include "asyncfilelogger.h"

namespace
{
// 按行读取 path 日志文件，去掉末尾分隔产生的空项。
QList<QByteArray> readLines(const QString &path)
{
    // 读写本次验证所需的临时文件。
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    // 保存从日志文件读取的各条记录。
    auto lines = file.readAll().split('\n');
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    return lines;
}
// 将 bytes 写入 path 并刷新，用于准备日志文件或阻挡文件。
bool writeFile(const QString &path, const QByteArray &bytes)
{
    // 读写本次验证所需的临时文件。
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}
// 读取 path 文件的全部字节，供写入和轮转结果比较。
QByteArray readFile(const QString &path)
{
    // 读写本次验证所需的临时文件。
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
} // namespace

class LoggerTests : public QObject
{
    Q_OBJECT
  private slots:
    // 验证普通日志突发时保留警告名额，并准确报告丢弃数量。
    void normalBurstPreservesWarningAdmissionAndReportsActualLoss();
    // 验证警告突发仍受队列容量限制且计数准确。
    void warningBurstIsBoundedAndCounted();
    // 验证退出前会写完已接收日志，且退出后拒绝新日志。
    void shutdownDrainsPendingLinesAfterBackgroundWrite();
    // 验证中文日志、换行清理和超长截断写入正确。
    void lineBoundariesAndUtf8SurviveFileWrite();
    // 验证日志目录被普通文件占据时报告写入失败。
    void impossibleDirectorySurfacesWriteFailure();
    // 验证日志轮转保留九份归档，并将新日志写入新文件。
    void rotationRetainsNineArchivesAndWritesNewFile();
};

void LoggerTests::normalBurstPreservesWarningAdmissionAndReportsActualLoss()
{
    // 隔离本次日志验证产生的文件。
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 保存本次验证使用的日志文件路径。
    const auto path = dir.filePath("nested/logs/workbench.log");
    // 执行待验证的异步文件日志写入。
    AsyncFileLogger logger(path);
    // 记录日志队列溢出通知，供丢弃行为校验。
    QSignalSpy overflow(&logger, &AsyncFileLogger::overflow);
    // A burst without pumping the GUI event loop models producers outrunning storage.
    // 指定普通日志的突发数量，warnings 指定警告数量。
    constexpr int normals = 10000, warnings = 64;
    // 统计成功进入队列的普通日志数量。
    int admittedNormal = 0;
    for (int i = 0; i < normals; ++i)
    {
        admittedNormal += logger.append(QString("NORMAL-%1").arg(i));
        QVERIFY(logger.pendingLines() <= AsyncFileLogger::MaximumPendingLines);
    }
    QVERIFY(admittedNormal > 0);
    QVERIFY(admittedNormal <=
            AsyncFileLogger::MaximumPendingLines - AsyncFileLogger::WarningReserve);
    for (int i = 0; i < warnings; ++i)
    {
        QVERIFY(logger.append(QString("WARNING-%1").arg(i), true));
        QVERIFY(logger.pendingLines() <= AsyncFileLogger::MaximumPendingLines);
    }
    QVERIFY(!overflow.isEmpty());
    logger.shutdown();
    // 保存从日志文件读取的各条记录。
    const auto lines = readLines(path);
    // 统计实际写入的普通日志数，actualWarnings 统计警告数。
    int actualNormal = 0, actualWarnings = 0;
    // 记录已写入内容以检查重复日志。
    QSet<QByteArray> unique;
    for (const auto &line : lines)
    {
        QVERIFY(!unique.contains(line));
        unique.insert(line);
        actualNormal += line.startsWith("NORMAL-");
        actualWarnings += line.startsWith("WARNING-");
    }
    QCOMPARE(actualWarnings, warnings);
    for (int i = 0; i < warnings; ++i)
        QVERIFY(unique.contains(QString("WARNING-%1").arg(i).toUtf8()));
    QCOMPARE(quint64(normals - actualNormal), logger.droppedNormal());
    QCOMPARE(logger.droppedWarnings(), quint64(0));
    QCOMPARE(logger.writtenLines(), quint64(lines.size()));
    QCOMPARE(logger.failedLines(), quint64(0));
    QCOMPARE(logger.pendingLines(), 0);
}

void LoggerTests::warningBurstIsBoundedAndCounted()
{
    // 隔离本次日志验证产生的文件。
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 保存本次验证使用的日志文件路径。
    const auto path = dir.filePath("warnings.log");
    // 执行待验证的异步文件日志写入。
    AsyncFileLogger logger(path);
    // 统计成功进入队列的警告日志数量。
    int admitted = 0;
    // 指定本次警告突发尝试的总条数。
    constexpr int total = 1000;
    for (int i = 0; i < total; ++i)
    {
        admitted += logger.append(QString("CRITICAL-%1").arg(i), true);
        QVERIFY(logger.pendingLines() <= AsyncFileLogger::MaximumPendingLines);
    }
    QVERIFY(admitted > 0);
    QVERIFY(admitted < total);
    logger.shutdown();
    QCOMPARE(readLines(path).size(), admitted);
    QCOMPARE(logger.writtenLines(), quint64(admitted));
    QCOMPARE(logger.droppedWarnings(), quint64(total - admitted));
    QCOMPARE(logger.droppedNormal(), quint64(0));
    QCOMPARE(logger.failedLines(), quint64(0));
}

void LoggerTests::shutdownDrainsPendingLinesAfterBackgroundWrite()
{
    // 隔离本次日志验证产生的文件。
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 保存本次验证使用的日志文件路径。
    const auto path = dir.filePath("drain.log");
    // 执行待验证的异步文件日志写入。
    AsyncFileLogger logger(path);
    for (int i = 0; i < 64; ++i)
        QVERIFY(logger.append(QString("background-%1").arg(i)));
    // Verify actual asynchronous IO has happened before queuing the final shutdown batch.
    QTRY_COMPARE_WITH_TIMEOUT(logger.writtenLines(), quint64(64), 5000);
    for (int i = 0; i < 150; ++i)
        QVERIFY(logger.append(QString("drain-%1").arg(i)));
    QVERIFY(logger.pendingLines() > 0);
    logger.shutdown();
    QCOMPARE(logger.writtenLines(), quint64(214));
    QCOMPARE(logger.failedLines(), quint64(0));
    QCOMPARE(logger.pendingLines(), 0);
    // 保存从日志文件读取的各条记录。
    const auto lines = readLines(path);
    QCOMPARE(lines.size(), 214);
    for (int i = 0; i < 64; ++i)
        QCOMPARE(lines[i], QString("background-%1").arg(i).toUtf8());
    for (int i = 0; i < 150; ++i)
        QCOMPARE(lines[64 + i], QString("drain-%1").arg(i).toUtf8());
    logger.shutdown();
    QVERIFY(!logger.append("after shutdown", true));
    QCOMPARE(readLines(path).size(), 214);
}

void LoggerTests::lineBoundariesAndUtf8SurviveFileWrite()
{
    // 隔离本次日志验证产生的文件。
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 保存本次验证使用的日志文件路径。
    const auto path = dir.filePath("utf8.log");
    // 执行待验证的异步文件日志写入。
    AsyncFileLogger logger(path);
    // 提供带中文和温度符号的 UTF-8 日志样本。
    const QString unicode = QStringLiteral("工位01 产品测试完成 温度25℃");
    QVERIFY(logger.append(unicode));
    QVERIFY(logger.append("a\r\nb\nc"));
    QVERIFY(logger.append(QString(20000, QChar('X'))));
    logger.shutdown();
    // 保存从日志文件读取的各条记录。
    const auto lines = readLines(path);
    QCOMPARE(lines.size(), 3);
    QCOMPARE(QString::fromUtf8(lines[0]), unicode);
    QCOMPARE(lines[1], QByteArray("a  b c"));
    QVERIFY(!lines[2].contains('\r'));
    QVERIFY(lines[2].size() < 4300);
    QVERIFY(QString::fromUtf8(lines[2]).endsWith(QStringLiteral("[日志已截断]")));
    QCOMPARE(logger.writtenLines(), quint64(3));
    QCOMPARE(logger.failedLines(), quint64(0));
}

void LoggerTests::impossibleDirectorySurfacesWriteFailure()
{
    // 隔离本次日志验证产生的文件。
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 提供占据预期目录位置的普通文件路径。
    const auto blocker = dir.filePath("not-a-directory");
    QVERIFY(writeFile(blocker, "keep this ordinary file"));
    // 保存本次验证使用的日志文件路径。
    const auto path = blocker + "/workbench.log";
    // 执行待验证的异步文件日志写入。
    AsyncFileLogger logger(path);
    // 记录日志写入失败通知。
    QSignalSpy failure(&logger, &AsyncFileLogger::writeFailed);
    for (int i = 0; i < 5; ++i)
        QVERIFY(logger.append(QString("line-%1").arg(i)));
    logger.shutdown();
    QVERIFY(!failure.isEmpty());
    QVERIFY(!failure.first().first().toString().isEmpty());
    QCOMPARE(logger.failedLines(), quint64(5));
    QCOMPARE(logger.writtenLines(), quint64(0));
    QCOMPARE(logger.pendingLines(), 0);
    QVERIFY(!QFileInfo::exists(path));
    QCOMPARE(readFile(blocker), QByteArray("keep this ordinary file"));
}

void LoggerTests::rotationRetainsNineArchivesAndWritesNewFile()
{
    // 隔离本次日志验证产生的文件。
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 保存本次验证使用的日志文件路径。
    const auto path = dir.filePath("rotation.log");
    // 指定用于触发日志轮转的文件大小阈值。
    constexpr int limit = 5 * 1024 * 1024;
    QVERIFY(writeFile(path, QByteArray(limit - 3, 'A')));
    for (int i = 1; i <= 9; ++i)
        QVERIFY(writeFile(path + "." + QString::number(i), QString("archive-%1").arg(i).toUtf8()));
    // 执行待验证的异步文件日志写入。
    AsyncFileLogger logger(path);
    // 收集日志轮转过程中的写入错误。
    QSignalSpy errors(&logger, &AsyncFileLogger::writeFailed);
    QVERIFY(logger.append("new-file-sentinel"));
    logger.shutdown();
    QVERIFY(errors.isEmpty());
    QCOMPARE(readFile(path), QByteArray("new-file-sentinel\n"));
    QCOMPARE(QFileInfo(path + ".1").size(), qint64(limit - 3));
    for (int i = 2; i <= 9; ++i)
        QCOMPARE(readFile(path + "." + QString::number(i)),
                 QString("archive-%1").arg(i - 1).toUtf8());
    QVERIFY(!QFileInfo::exists(path + ".10"));
    QCOMPARE(logger.writtenLines(), quint64(1));
    QCOMPARE(logger.failedLines(), quint64(0));
}

QTEST_GUILESS_MAIN(LoggerTests)
#include "test_logger.moc"
