#pragma once
#include <QString>

namespace RuntimeConfiguration
{
struct ExecutionLimits
{
    // 保存整轮测试允许的秒数。
    int runSeconds = 120;
    // 保存单步骤允许的秒数。
    int stepSeconds = 30;
};
// 读取测试期限；配置无效时使用默认值并填写 error。
ExecutionLimits loadLimits(const QString &directory, QString *error = nullptr);
// 原子保存之后新测试使用的期限。
bool saveLimits(const QString &directory, const ExecutionLimits &limits, QString *error);
// 沿已验证恢复记录查找当前应使用的数据目录，拒绝循环或损坏的指向。
QString resolveDataDirectory(const QString &directory, QString *error);
// 为下次启动保存恢复后的数据目录，原库文件完整保留。
bool activateDataDirectory(const QString &directory, const QString &restoredDirectory,
                           QString *error);
} // namespace RuntimeConfiguration
