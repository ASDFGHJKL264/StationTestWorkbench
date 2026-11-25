#include "domain.h"
#include <QtTest>
#include <limits>

class DomainTests final : public QObject
{
    Q_OBJECT
    // 构造包含产品身份和默认配方的合法测试请求。
    QJsonObject request() const
    {
        return {{"runId", "test-run"},
                {"stationId", 1},
                {"sn", "PCB-01"},
                {"recipeVersion", "demo-5v-v1"},
                {"recipe", Domain::defaultRecipe()}};
    }
    // 构造限值端点均通过的合法测试终态。
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
    // 验证限值包含端点且质量判定由本地规则生成。
    void inclusiveLimitsAndLocallyDerivedVerdict()
    {
        // 保存用于注入异常或边界值的测试结果副本。
        auto r = result();
        r["verdict"] = "REMOTE_LIE";
        // 接收本地重新计算的质量判定。
        QString verdict;
        QVERIFY(Domain::evaluate(request(), r, nullptr, &verdict));
        QCOMPARE(verdict, QString("PASS"));
        // 保存测试结果中的实测步骤数组。
        auto values = r["steps"].toArray();
        // 保存用于修改电压边界值的步骤对象。
        auto voltage = values[0].toObject();
        voltage["value"] = 4.749;
        values[0] = voltage;
        r["steps"] = values;
        QVERIFY(Domain::evaluate(request(), r, nullptr, &verdict));
        QCOMPARE(verdict, QString("FAIL"));
    }
    // 验证缺失、乱序步骤和不匹配测试身份都会被拒绝。
    void missingReorderedAndWrongIdentityRejected()
    {
        // 保存修改前的合法结果，供各项异常校验恢复。
        const auto good = result();
        // 保存用于注入异常或边界值的测试结果副本。
        auto r = good;
        // 保存测试结果中的实测步骤数组。
        auto values = r["steps"].toArray();
        values.removeLast();
        r["steps"] = values;
        QVERIFY(!Domain::evaluate(request(), r, nullptr, nullptr));
        r = good;
        values = good["steps"].toArray();
        // 保留原首个测量步骤，以构造乱序或非有限值场景。
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
    // 验证取消终态保留已测前缀且不会被判为通过。
    void cancelledResultRetainsPrefixWithoutPretendingPass()
    {
        // 保存用于注入异常或边界值的测试结果副本。
        auto r = result();
        r["outcome"] = "cancelled";
        // 保存测试结果中的实测步骤数组。
        auto values = r["steps"].toArray();
        values.removeLast();
        r["steps"] = values;
        // 接收本地判定后返回的步骤明细。
        QJsonArray rows;
        // 接收本地重新计算的质量判定。
        QString verdict;
        QVERIFY(Domain::evaluate(request(), r, &rows, &verdict));
        QCOMPARE(rows.size(), 2);
        QCOMPARE(verdict, QString("CANCELLED"));
        r["steps"] = QJsonArray{values[1]};
        QVERIFY(!Domain::evaluate(request(), r, nullptr, nullptr));
    }
    // 验证重复步骤、错误限值和非法身份在下发前被拒绝。
    void invalidRecipeAndStationRejectedBeforeDispatch()
    {
        // 保存本次验证使用的测试配方。
        auto recipe = Domain::defaultRecipe();
        // 保存测试配方或结果中的步骤集合。
        auto steps = recipe["steps"].toArray();
        steps.append(steps[0]);
        recipe["steps"] = steps;
        QVERIFY(!Domain::validateRecipe(recipe));
        recipe = Domain::defaultRecipe();
        steps = recipe["steps"].toArray();
        // 保存需要修改并重新提交的步骤对象。
        auto step = steps[0].toObject();
        step["nominal"] = 7;
        steps[0] = step;
        recipe["steps"] = steps;
        QVERIFY(!Domain::validateRecipe(recipe));
        // 保存本次验证使用的测试身份和配方请求。
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
    // 验证无穷测量值无法通过结果校验。
    void nonFiniteMeasurementCannotBecomePass()
    {
        // 保存用于注入异常或边界值的测试结果副本。
        auto r = result();
        // 保存测试结果中的实测步骤数组。
        auto values = r["steps"].toArray();
        // 保留原首个测量步骤，以构造乱序或非有限值场景。
        auto first = values[0].toObject();
        first["value"] = std::numeric_limits<double>::infinity();
        values[0] = first;
        r["steps"] = values;
        QVERIFY(!Domain::evaluate(request(), r, nullptr, nullptr));
    }
};
QTEST_GUILESS_MAIN(DomainTests)
#include "test_domain.moc"
