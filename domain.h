#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QString>

namespace Domain
{
// 生成内置演示配方及默认测试限值。
QJsonObject defaultRecipe();
// 校验 recipe 配方格式，失败时通过 error 返回原因。
bool validateRecipe(const QJsonObject &recipe, QString *error = nullptr);
// 计算 recipe 配方内容的 SHA-256 摘要，用于核对版本。
QString recipeHash(const QJsonObject &recipe);
// 校验 run 测试身份及配方绑定，失败时填写 error。
bool validateRun(const QJsonObject &run, QString *error = nullptr);
// 按 request 的配方复核 result，输出 evaluatedSteps 步骤、verdict 判定及 error 原因。
bool evaluate(const QJsonObject &request, const QJsonObject &result, QJsonArray *evaluatedSteps,
              QString *verdict, QString *error = nullptr);
// 将 object 转成紧凑 JSON 字节，供存储和内容比较使用。
QByteArray canonical(const QJsonObject &object);
} // namespace Domain
