#include "domain.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSet>
#include <cmath>

namespace
{
bool reject(QString *error, const QString &text)
{
    if (error)
        *error = text;
    return false;
}
bool textValid(const QString &s, int maximum)
{
    if (s.isEmpty() || s.size() > maximum || s.trimmed() != s)
        return false;
    for (const auto ch : s)
        if (!ch.isPrint())
            return false;
    return true;
}
bool number(const QJsonValue &v)
{
    return v.isDouble() && std::isfinite(v.toDouble());
}
} // namespace

QJsonObject Domain::defaultRecipe()
{
    return {{"version", "demo-5v-v1"},
            {"name", QStringLiteral("5V电子模块功能测试（模拟配方）")},
            {"steps", QJsonArray{QJsonObject{{"id", "voltage"},
                                             {"label", QStringLiteral("输出电压")},
                                             {"unit", "V"},
                                             {"min", 4.75},
                                             {"max", 5.25},
                                             {"nominal", 5.0}},
                                 QJsonObject{{"id", "current"},
                                             {"label", QStringLiteral("待机电流")},
                                             {"unit", "mA"},
                                             {"min", 10},
                                             {"max", 80},
                                             {"nominal", 35}},
                                 QJsonObject{{"id", "io"},
                                             {"label", QStringLiteral("IO回环")},
                                             {"unit", "bool"},
                                             {"min", 1},
                                             {"max", 1},
                                             {"nominal", 1}}}}};
}

QByteArray Domain::canonical(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
QString Domain::recipeHash(const QJsonObject &recipe)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(canonical(recipe), QCryptographicHash::Sha256).toHex());
}

bool Domain::validateRecipe(const QJsonObject &r, QString *error)
{
    if (!textValid(r["version"].toString(), 64) || !textValid(r["name"].toString(), 100))
        return reject(error, QStringLiteral("配方名称/版本不能为空，且不能含控制字符或首尾空白"));
    if (!r["steps"].isArray())
        return reject(error, QStringLiteral("配方steps必须是数组"));
    const auto steps = r["steps"].toArray();
    if (steps.isEmpty() || steps.size() > 16)
        return reject(error, QStringLiteral("配方要求1至16个步骤"));
    QSet<QString> seen;
    for (const auto &value : steps)
    {
        const auto s = value.toObject();
        const auto id = s["id"].toString();
        if (!textValid(id, 40) || seen.contains(id) || !textValid(s["label"].toString(), 80) ||
            s["unit"].toString().size() > 20 || !s["unit"].isString())
            return reject(error, QStringLiteral("步骤身份、名称、单位无效或重复"));
        seen.insert(id);
        if (!number(s["min"]) || !number(s["max"]) || !number(s["nominal"]) ||
            s["min"].toDouble() > s["nominal"].toDouble() ||
            s["nominal"].toDouble() > s["max"].toDouble() || std::abs(s["min"].toDouble()) > 1e12 ||
            std::abs(s["max"].toDouble()) > 1e12)
            return reject(
                error, QStringLiteral("步骤限值必须有限，且min <= nominal <= max，幅值不超过1e12"));
    }
    return true;
}

bool Domain::validateRun(const QJsonObject &r, QString *error)
{
    const auto station = r["stationId"];
    if (!textValid(r["runId"].toString(), 64) || !textValid(r["sn"].toString(), 64) ||
        !station.isDouble() || station.toDouble() != station.toInt() || station.toInt() < 1 ||
        station.toInt() > 4)
        return reject(error, QStringLiteral("测试编号、SN或工位ID无效"));
    const auto recipe = r["recipe"].toObject();
    if (!validateRecipe(recipe, error))
        return false;
    if (r["recipeVersion"].toString() != recipe["version"].toString())
        return reject(error, QStringLiteral("运行绑定的配方版本不一致"));
    return true;
}

bool Domain::evaluate(const QJsonObject &request, const QJsonObject &result,
                      QJsonArray *evaluatedSteps, QString *verdict, QString *error)
{
    if (!validateRun(request, error))
        return false;
    for (const auto *key : {"runId", "sn", "recipeVersion"})
        if (request[key] != result[key])
            return reject(error, QStringLiteral("终态身份不匹配：") + key);
    if (!result["stationId"].isDouble() || request["stationId"] != result["stationId"])
        return reject(error, QStringLiteral("终态工位不匹配"));
    const auto outcome = result["outcome"].toString();
    if (outcome != "completed" && outcome != "cancelled")
        return reject(error, QStringLiteral("未知终态类型"));
    if (!result["steps"].isArray())
        return reject(error, QStringLiteral("终态缺少步骤数组"));
    const auto values = result["steps"].toArray();
    const auto rules = request["recipe"].toObject()["steps"].toArray();
    if ((outcome == "completed" && values.size() != rules.size()) || values.size() > rules.size())
        return reject(error, QStringLiteral("终态步骤数量与配方不符"));
    QJsonArray evaluated;
    bool all = true;
    for (qsizetype i = 0; i < values.size(); ++i)
    {
        const auto item = values[i].toObject();
        const auto rule = rules[i].toObject();
        if (item["stepId"] != rule["id"] || !number(item["value"]))
            return reject(error, QStringLiteral("步骤顺序、身份或测量值无效"));
        const auto v = item["value"].toDouble();
        const bool passed = v >= rule["min"].toDouble() && v <= rule["max"].toDouble();
        all &= passed;
        evaluated.append(QJsonObject{{"stepId", rule["id"]},
                                     {"label", rule["label"]},
                                     {"unit", rule["unit"]},
                                     {"min", rule["min"]},
                                     {"max", rule["max"]},
                                     {"value", v},
                                     {"passed", passed}});
    }
    if (evaluatedSteps)
        *evaluatedSteps = evaluated;
    if (verdict)
        *verdict = outcome == "cancelled" ? "CANCELLED" : all ? "PASS" : "FAIL";
    return true;
}
