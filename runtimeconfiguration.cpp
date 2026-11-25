#include "runtimeconfiguration.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <cmath>

namespace
{
// 原子写入维护设置，失败时保留原有配置。
bool writeSettings(const QString &path, const QJsonObject &object, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    {
        if (error)
            *error = QStringLiteral("无法创建维护设置目录");
        return false;
    }
    // 先写临时文件，完整提交后才替换正式设置。
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    // 保存需要写入的完整设置内容。
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}
// 读取大小有界的设置对象，避免将损坏文件当作默认配置覆盖。
bool readSettings(const QString &path, QJsonObject *object, QString *error)
{
    // 打开本地维护设置文件。
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
    {
        if (error)
            *error = QStringLiteral("无法读取维护设置：") + path;
        return false;
    }
    // 接收 JSON 格式错误信息。
    QJsonParseError parseError;
    // 解析完整设置对象。
    const auto doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject() ||
        doc.object()["version"].toInt() != 1)
    {
        if (error)
            *error = QStringLiteral("维护设置格式或版本无效：") + path;
        return false;
    }
    *object = doc.object();
    return true;
}
// 检查秒数是否为一天以内的正整数。
bool validSeconds(const QJsonValue &value)
{
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 1 &&
           value.toDouble() <= 86400 && value.toDouble() == std::floor(value.toDouble());
}
} // namespace

RuntimeConfiguration::ExecutionLimits RuntimeConfiguration::loadLimits(const QString &directory,
                                                                       QString *error)
{
    // 保存默认或已验证的运行期限。
    ExecutionLimits limits;
    // 定位该数据目录自己的期限设置。
    const auto path = QDir(directory).filePath("maintenance/runtime.json");
    if (!QFileInfo::exists(path))
        return limits;
    // 保存设置文件中的字段。
    QJsonObject object;
    if (!readSettings(path, &object, error))
        return limits;
    if (!validSeconds(object["runTimeoutSeconds"]) || !validSeconds(object["stepTimeoutSeconds"]) ||
        object["stepTimeoutSeconds"].toInt() > object["runTimeoutSeconds"].toInt())
    {
        if (error)
            *error = QStringLiteral("测试时限设置无效，已使用默认的120秒/30秒");
        return limits;
    }
    limits.runSeconds = object["runTimeoutSeconds"].toInt();
    limits.stepSeconds = object["stepTimeoutSeconds"].toInt();
    return limits;
}

bool RuntimeConfiguration::saveLimits(const QString &directory, const ExecutionLimits &limits,
                                      QString *error)
{
    if (limits.runSeconds < 1 || limits.runSeconds > 86400 || limits.stepSeconds < 1 ||
        limits.stepSeconds > limits.runSeconds)
    {
        if (error)
            *error = QStringLiteral("时限须在1至86400秒内，且步骤时限不能超过整轮时限");
        return false;
    }
    return writeSettings(QDir(directory).filePath("maintenance/runtime.json"),
                         {{"version", 1},
                          {"runTimeoutSeconds", limits.runSeconds},
                          {"stepTimeoutSeconds", limits.stepSeconds}},
                         error);
}

QString RuntimeConfiguration::resolveDataDirectory(const QString &directory, QString *error)
{
    // 保存当前查找的数据目录，逐次跟随成功恢复后的指向。
    QString current = QDir(directory).absolutePath();
    // 记录走过的目录，拒绝循环配置。
    QSet<QString> visited;
    for (int depth = 0; depth < 32; ++depth)
    {
        // 使用规范路径识别同一个目录的不同写法。
        const auto canonical = QFileInfo(current).canonicalFilePath();
        // Windows 目录比较不区分大小写。
        const auto identity = (canonical.isEmpty() ? current : canonical).toCaseFolded();
        if (visited.contains(identity))
            break;
        visited.insert(identity);
        // 定位当前目录保存的恢复切换记录。
        const auto pointer = QDir(current).filePath("maintenance/active-data-dir.json");
        if (!QFileInfo::exists(pointer))
            return current;
        // 保存下一数据目录的设置对象。
        QJsonObject object;
        if (!readSettings(pointer, &object, error))
            return {};
        // 保存已经完成恢复的新数据目录。
        const auto next = object["activeDataDirectory"].toString();
        if (!QDir::isAbsolutePath(next) ||
            !QFileInfo(QDir(next).filePath("data/workbench.db")).isFile())
        {
            if (error)
                *error = QStringLiteral("恢复后的数据目录不存在，请检查路径：") + next;
            return {};
        }
        current = QDir(next).absolutePath();
    }
    if (error)
        *error = QStringLiteral("数据目录切换记录循环或层数过多，已停止启动");
    return {};
}

bool RuntimeConfiguration::activateDataDirectory(const QString &directory,
                                                 const QString &restoredDirectory, QString *error)
{
    // 保存恢复目录的实际路径，避免与原目录形成循环。
    const auto target = QFileInfo(restoredDirectory).canonicalFilePath();
    // 保存原数据目录的实际路径。
    const auto original = QFileInfo(directory).canonicalFilePath();
    if (target.isEmpty() || original.isEmpty() ||
        target.compare(original, Qt::CaseInsensitive) == 0 ||
        !QFileInfo(QDir(target).filePath("data/workbench.db")).isFile() ||
        QFileInfo::exists(QDir(target).filePath("maintenance/active-data-dir.json")))
    {
        if (error)
            *error = QStringLiteral("恢复数据目录无效或已有其他切换记录");
        return false;
    }
    return writeSettings(QDir(directory).filePath("maintenance/active-data-dir.json"),
                         {{"version", 1}, {"activeDataDirectory", target}}, error);
}
