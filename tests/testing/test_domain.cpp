#include "testing/domain.h"
#include <QtTest>
#include <limits>

class DomainTests final : public QObject
{
    Q_OBJECT
    QJsonObject request() const
    {
        return {{"runId", "test-run"},
                {"stationId", 1},
                {"sn", "PCB-01"},
                {"recipeVersion", "demo-5v-v1"},
                {"recipe", Domain::defaultRecipe()}};
    }
    QJsonObject result() const
    {
        return {{"runId", "test-run"},
                {"stationId", 1},
                {"sn", "PCB-01"},
                {"recipeVersion", "demo-5v-v1"},
                {"outcome", "completed"},
                {"steps", QJsonArray{QJsonObject{{"stepId", "voltage"}, {"value", 4.75}},
                                     QJsonObject{{"stepId", "current"}, {"value", 80}},
                                     QJsonObject{{"stepId", "io"}, {"value", 1}}}}};
    }
  private slots:
    void inclusiveLimitsAndLocallyDerivedVerdict()
    {
        auto r = result();
        r["verdict"] = "REMOTE_LIE";
        QString verdict;
        QVERIFY(Domain::evaluate(request(), r, nullptr, &verdict));
        QCOMPARE(verdict, QString("PASS"));
        auto values = r["steps"].toArray();
        auto voltage = values[0].toObject();
        voltage["value"] = 4.749;
        values[0] = voltage;
        r["steps"] = values;
        QVERIFY(Domain::evaluate(request(), r, nullptr, &verdict));
        QCOMPARE(verdict, QString("FAIL"));
    }
    void missingReorderedAndWrongIdentityRejected()
    {
        const auto good = result();
        auto r = good;
        auto values = r["steps"].toArray();
        values.removeLast();
        r["steps"] = values;
        QVERIFY(!Domain::evaluate(request(), r, nullptr, nullptr));
        r = good;
        values = good["steps"].toArray();
        const auto first = values[0];
        values[0] = values[1];
        values[1] = first;
        r["steps"] = values;
        QVERIFY(!Domain::evaluate(request(), r, nullptr, nullptr));
        for (const auto &key : {"runId", "stationId", "sn", "recipeVersion"})
        {
            r = good;
            r[key] = "unrelated";
            QVERIFY(!Domain::evaluate(request(), r, nullptr, nullptr));
        }
    }
    void cancelledResultRetainsPrefixWithoutPretendingPass()
    {
        auto r = result();
        r["outcome"] = "cancelled";
        auto values = r["steps"].toArray();
        values.removeLast();
        r["steps"] = values;
        QJsonArray rows;
        QString verdict;
        QVERIFY(Domain::evaluate(request(), r, &rows, &verdict));
        QCOMPARE(rows.size(), 2);
        QCOMPARE(verdict, QString("CANCELLED"));
        r["steps"] = QJsonArray{values[1]};
        QVERIFY(!Domain::evaluate(request(), r, nullptr, nullptr));
    }
    void invalidRecipeAndStationRejectedBeforeDispatch()
    {
        auto recipe = Domain::defaultRecipe();
        auto steps = recipe["steps"].toArray();
        steps.append(steps[0]);
        recipe["steps"] = steps;
        QVERIFY(!Domain::validateRecipe(recipe));
        recipe = Domain::defaultRecipe();
        steps = recipe["steps"].toArray();
        auto step = steps[0].toObject();
        step["nominal"] = 7;
        steps[0] = step;
        recipe["steps"] = steps;
        QVERIFY(!Domain::validateRecipe(recipe));
        auto run = request();
        run["stationId"] = 1.5;
        QVERIFY(!Domain::validateRun(run));
        run = request();
        run["sn"] = QString("bad\nsn");
        QVERIFY(!Domain::validateRun(run));
        run = request();
        run["sn"] = QString(65, 'x');
        QVERIFY(!Domain::validateRun(run));
    }
    void nonFiniteMeasurementCannotBecomePass()
    {
        auto r = result();
        auto values = r["steps"].toArray();
        auto first = values[0].toObject();
        first["value"] = std::numeric_limits<double>::infinity();
        values[0] = first;
        r["steps"] = values;
        QVERIFY(!Domain::evaluate(request(), r, nullptr, nullptr));
    }
};
QTEST_GUILESS_MAIN(DomainTests)
#include "test_domain.moc"
