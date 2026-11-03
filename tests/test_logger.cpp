#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include "runtime/asyncfilelogger.h"

namespace
{
QList<QByteArray> readLines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    auto lines = file.readAll().split('\n');
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    return lines;
}
bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}
QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
} // namespace

class LoggerTests : public QObject
{
    Q_OBJECT
  private slots:
    void normalBurstPreservesWarningAdmissionAndReportsActualLoss();
    void warningBurstIsBoundedAndCounted();
    void shutdownDrainsPendingLinesAfterBackgroundWrite();
    void lineBoundariesAndUtf8SurviveFileWrite();
    void impossibleDirectorySurfacesWriteFailure();
    void rotationRetainsNineArchivesAndWritesNewFile();
};

void LoggerTests::normalBurstPreservesWarningAdmissionAndReportsActualLoss()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = dir.filePath("nested/logs/workbench.log");
    AsyncFileLogger logger(path);
    QSignalSpy overflow(&logger, &AsyncFileLogger::overflow);
    // A burst without pumping the GUI event loop models producers outrunning storage.
    constexpr int normals = 10000, warnings = 64;
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
    const auto lines = readLines(path);
    int actualNormal = 0, actualWarnings = 0;
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
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = dir.filePath("warnings.log");
    AsyncFileLogger logger(path);
    int admitted = 0;
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
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = dir.filePath("drain.log");
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
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = dir.filePath("utf8.log");
    AsyncFileLogger logger(path);
    const QString unicode = QStringLiteral("工位01 产品测试完成 温度25℃");
    QVERIFY(logger.append(unicode));
    QVERIFY(logger.append("a\r\nb\nc"));
    QVERIFY(logger.append(QString(20000, QChar('X'))));
    logger.shutdown();
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
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto blocker = dir.filePath("not-a-directory");
    QVERIFY(writeFile(blocker, "keep this ordinary file"));
    const auto path = blocker + "/workbench.log";
    AsyncFileLogger logger(path);
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
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = dir.filePath("rotation.log");
    constexpr int limit = 5 * 1024 * 1024;
    QVERIFY(writeFile(path, QByteArray(limit - 3, 'A')));
    for (int i = 1; i <= 9; ++i)
        QVERIFY(writeFile(path + "." + QString::number(i), QString("archive-%1").arg(i).toUtf8()));
    AsyncFileLogger logger(path);
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
