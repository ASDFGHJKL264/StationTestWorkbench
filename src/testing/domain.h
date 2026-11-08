#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QString>

namespace Domain
{
QJsonObject defaultRecipe();
bool validateRecipe(const QJsonObject &recipe, QString *error = nullptr);
QString recipeHash(const QJsonObject &recipe);
bool validateRun(const QJsonObject &run, QString *error = nullptr);
bool evaluate(const QJsonObject &request, const QJsonObject &result, QJsonArray *evaluatedSteps,
              QString *verdict, QString *error = nullptr);
QByteArray canonical(const QJsonObject &object);
} // namespace Domain
