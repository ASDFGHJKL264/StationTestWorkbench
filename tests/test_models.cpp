#include <QtTest>
#include <QAbstractItemModelTester>
#include <QPersistentModelIndex>
#include <QSortFilterProxyModel>
#include <QImage>
#include <QPainter>
#include <QStandardItemModel>
#include <QTableView>
#include "models.h"

class ModelTests : public QObject
{
    Q_OBJECT
  private slots:
    // 验证工位更新后选中索引仍对应原工位身份。
    void stationUpdatesPreserveSelectionIdentity();
    // 验证工位按编号排序且只接受允许范围内的工位。
    void stationRowsAreSortedAndBounded();
    // 验证执行阶段、质量判定和归档状态分别显示。
    void executionVerdictAndArchiveAreIndependent();
    // 验证未测、实测零值和超限失败显示不同。
    void stepsDistinguishUnmeasuredZeroAndFailure();
    // 验证同一步骤更新保留索引，步骤身份变化才重置模型。
    void stepUpdatesKeepPersistentIndexes();
    // 验证历史分页游标不随界面排序改变。
    void historyCursorDoesNotFollowViewSort();
    // 验证历史按完整年份和数字工位编号排序。
    void historySortUsesYearAndNumericStation();
    // 验证日志容量受限且淘汰记录时索引和信号正确。
    void logsHaveBoundedMemoryAndCorrectRemoval();
    // 验证普通说明使用默认绘制，且绘制不修改模型。
    void qualityDelegateFallsBackWithoutChangingModel();
    // 验证质量徽标保留行选中效果和可复制文字。
    void qualityDelegatePreservesRowSelectionAndAccessibleText();
};

void ModelTests::stationUpdatesPreserveSelectionIdentity()
{
    // 保存并更新工位状态，供行顺序和身份验证。
    StationModel model;
    // 持续检查模型的索引、信号和行列操作是否符合 Qt 约定。
    QAbstractItemModelTester checker(&model,
                                     QAbstractItemModelTester::FailureReportingMode::QtTest);
    model.updateStation({{"stationId", 2}, {"execution", "ready"}});
    // 保留选中项的持久索引，检查更新后的身份。
    QPersistentModelIndex selected = model.index(0, 0);
    // 记录模型重置信号以检查更新范围。
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    model.updateStation({{"stationId", 1}, {"execution", "offline"}});
    QCOMPARE(selected.row(), 1);
    QCOMPARE(selected.data(StationModel::StationIdRole).toInt(), 2);
    for (int n = 0; n < 100; ++n)
        model.updateStation({{"stationId", 2}, {"execution", QString::number(n)}});
    QVERIFY(selected.isValid());
    QCOMPARE(selected.data(StationModel::StationIdRole).toInt(), 2);
    QCOMPARE(model.index(selected.row(), 2).data().toString(), QString("99"));
    QCOMPARE(resets.count(), 0);
}
void ModelTests::stationRowsAreSortedAndBounded()
{
    // 保存并更新工位状态，供行顺序和身份验证。
    StationModel model;
    // 持续检查模型的索引、信号和行列操作是否符合 Qt 约定。
    QAbstractItemModelTester checker(&model,
                                     QAbstractItemModelTester::FailureReportingMode::QtTest);
    for (int id : {4, 2, 1, 3, 0, 5, -1})
        model.updateStation({{"stationId", id}});
    QCOMPARE(model.rowCount(), 4);
    for (int row = 0; row < 4; ++row)
    {
        QCOMPARE(model.stationAt(row)["stationId"].toInt(), row + 1);
        QCOMPARE(model.rowForStation(row + 1), row);
    }
    QVERIFY(model.stationAt(-1).isEmpty());
    QVERIFY(model.stationAt(4).isEmpty());
    QVERIFY(!model.data(QModelIndex()).isValid());
    QCOMPARE(model.rowCount(model.index(0, 0)), 0);
}
void ModelTests::executionVerdictAndArchiveAreIndependent()
{
    // 保存并更新工位状态，供行顺序和身份验证。
    StationModel model;
    model.updateStation({{"stationId", 1},
                         {"online", true},
                         {"execution", QStringLiteral("等待归档")},
                         {"verdict", "FAIL"},
                         {"stored", false}});
    QCOMPARE(model.index(0, 2).data().toString(), QStringLiteral("等待归档"));
    QCOMPARE(model.index(0, 5).data().toString(), QString("FAIL"));
    QCOMPARE(model.index(0, 6).data().toString(), QStringLiteral("未归档"));
    model.updateStation({{"stationId", 1},
                         {"online", true},
                         {"execution", QStringLiteral("已完成并归档")},
                         {"verdict", "FAIL"},
                         {"stored", true}});
    QCOMPARE(model.index(0, 5).data().toString(), QString("FAIL"));
    QCOMPARE(model.index(0, 6).data().toString(), QStringLiteral("已提交"));
}
void ModelTests::stepsDistinguishUnmeasuredZeroAndFailure()
{
    // 保存测试步骤，供实测值和步骤索引验证。
    StepModel model;
    // 持续检查模型的索引、信号和行列操作是否符合 Qt 约定。
    QAbstractItemModelTester checker(&model,
                                     QAbstractItemModelTester::FailureReportingMode::QtTest);
    // 构造尚未包含实测值的测试步骤。
    QJsonObject unmeasured{
        {"stepId", "voltage"}, {"label", "Voltage"}, {"min", 0.0}, {"max", 1.0}, {"unit", "V"}};
    // 构造实测为零且判定通过的测试步骤。
    QJsonObject measured = unmeasured;
    measured["value"] = 0.0;
    measured["passed"] = true;
    // 构造实测超限且判定失败的测试步骤。
    QJsonObject failed = unmeasured;
    failed["value"] = 1.1;
    failed["passed"] = false;
    model.setSteps({unmeasured, measured, failed});
    QCOMPARE(model.index(0, 3).data().toString(), QStringLiteral("—"));
    QCOMPARE(model.index(0, 5).data().toString(), QStringLiteral("待测"));
    QCOMPARE(model.index(1, 3).data().toString(), QString("0"));
    QCOMPARE(model.index(1, 5).data().toString(), QString("PASS"));
    QCOMPARE(model.index(2, 5).data().toString(), QString("FAIL"));
    model.setSteps({});
    QCOMPARE(model.rowCount(), 0);
}
void ModelTests::stepUpdatesKeepPersistentIndexes()
{
    // 保存测试步骤，供实测值和步骤索引验证。
    StepModel model;
    // 保存需要修改并重新提交的步骤对象。
    QJsonObject step{{"stepId", "one"}, {"label", "One"}, {"min", 0}, {"max", 1}};
    model.setSteps({step});
    // 保留选中项的持久索引，检查更新后的身份。
    QPersistentModelIndex selected = model.index(0, 1);
    // 记录模型重置信号以检查更新范围。
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    step["value"] = 1;
    step["passed"] = true;
    model.setSteps({step});
    QVERIFY(selected.isValid());
    QCOMPARE(resets.count(), 0);
    step["stepId"] = "two";
    model.setSteps({step});
    QCOMPARE(resets.count(), 1);
    QVERIFY(!selected.isValid());
}
void ModelTests::historyCursorDoesNotFollowViewSort()
{
    // 保存历史记录，供排序与分页游标验证。
    HistoryModel model;
    // 提供界面排序与原始模型之间的索引映射。
    QSortFilterProxyModel proxy;
    proxy.setSourceModel(&model);
    // 持续检查模型的索引、信号和行列操作是否符合 Qt 约定。
    QAbstractItemModelTester checker(&model,
                                     QAbstractItemModelTester::FailureReportingMode::QtTest);
    model.setRows(
        {QJsonObject{{"id", 103}, {"sn", "C"}, {"execution", "COMPLETED"}, {"verdict", "PASS"}},
         QJsonObject{
             {"id", 101}, {"sn", "A"}, {"execution", "CANCELLED"}, {"verdict", "CANCELLED"}},
         QJsonObject{
             {"id", 102}, {"sn", "B"}, {"execution", "INTERRUPTED"}, {"verdict", "UNKNOWN"}}});
    proxy.sort(2, Qt::DescendingOrder);
    QCOMPARE(proxy.index(0, 2).data().toString(), QString("C"));
    QCOMPARE(model.lastId(), 101);
    // 保存代理索引映射回原始模型后的索引。
    const auto source = proxy.mapToSource(proxy.index(1, 0));
    QCOMPARE(model.recordAt(source.row())["sn"].toString(), QString("B"));
    QVERIFY(model.recordAt(9).isEmpty());
    model.setRows({});
    QCOMPARE(model.lastId(), 0);
    QCOMPARE(proxy.rowCount(), 0);
}
void ModelTests::logsHaveBoundedMemoryAndCorrectRemoval()
{
    // 保存容量受限的运行日志，供淘汰行为验证。
    BoundedLogModel model(3);
    // 持续检查模型的索引、信号和行列操作是否符合 Qt 约定。
    QAbstractItemModelTester checker(&model,
                                     QAbstractItemModelTester::FailureReportingMode::QtTest);
    // 记录有界日志淘汰旧记录时的删行信号。
    QSignalSpy removals(&model, &QAbstractItemModel::rowsRemoved);
    model.append("first");
    model.append("second");
    model.append("third");
    // 保留第二条日志的持久索引以检查淘汰影响。
    QPersistentModelIndex second = model.index(1, 0);
    model.append("fourth", true);
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(removals.count(), 1);
    QVERIFY(second.isValid());
    QCOMPARE(second.row(), 0);
    QVERIFY(second.data().toString().endsWith("second"));
    QVERIFY(model.index(2, 0).data().toString().contains(QStringLiteral("[警告]")));
    for (int i = 0; i < 1000; ++i)
        model.append(QString::number(i));
    QCOMPARE(model.rowCount(), 3);
    QVERIFY(!second.isValid());
    model.append(QString(100000, 'x'));
    QVERIFY(model.index(2, 0).data().toString().size() < 4200);
    // 验证零容量配置会被限制为至少一条日志。
    BoundedLogModel minCapacity(0);
    minCapacity.append("a");
    minCapacity.append("b");
    QCOMPARE(minCapacity.rowCount(), 1);
}

void ModelTests::historySortUsesYearAndNumericStation()
{
    // 保存历史记录，供排序与分页游标验证。
    HistoryModel model;
    // 提供界面排序与原始模型之间的索引映射。
    QSortFilterProxyModel proxy;
    proxy.setSourceModel(&model);
    proxy.setSortRole(HistoryModel::SortRole);
    model.setRows(
        {QJsonObject{{"id", 1}, {"startedAt", "2025-12-31T23:00:00Z"}, {"stationId", 2}},
         QJsonObject{{"id", 2}, {"startedAt", "2026-01-01T01:00:00Z"}, {"stationId", 1}}});
    proxy.sort(0, Qt::DescendingOrder);
    QCOMPARE(model.recordAt(proxy.mapToSource(proxy.index(0, 0)).row())["id"].toInt(), 2);
    proxy.sort(1, Qt::AscendingOrder);
    QCOMPARE(proxy.index(0, 1).data().toInt(), 1);
    QCOMPARE(model.lastId(), 1);
}

void ModelTests::qualityDelegateFallsBackWithoutChangingModel()
{
    // 提供单元格绘制所需的表格宿主控件。
    QTableView host;
    // 提供用于绘制验证的单元格数据。
    QStandardItemModel model(1, 1);
    model.setData(model.index(0, 0), QStringLiteral("普通说明 / 待测"));
    // 记录绘制过程中是否误改了模型数据。
    QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
    // 提供待验证的质量判定绘制委托。
    QualityDelegate custom;
    // 提供用于比较的 Qt 默认绘制委托。
    QStyledItemDelegate ordinary;
    // 配置单元格尺寸、配色和选中状态等绘制条件。
    QStyleOptionViewItem option;
    option.initFrom(&host);
    option.rect = QRect(0, 0, 180, 42);
    option.widget = &host;
    // 按当前绘制条件生成单元格图像供比较。
    auto render = [&](QStyledItemDelegate &delegate)
    {
        // 保存本次单元格绘制产生的图像。
        QImage image(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        // 将单元格绘制到验证图像中。
        QPainter painter(&image);
        delegate.paint(&painter, option, model.index(0, 0));
        return image;
    };
    // A non-verdict cell retains the platform's ordinary item rendering.
    QCOMPARE(render(custom), render(ordinary));
    QCOMPARE(changes.count(), 0);
    QCOMPARE(model.index(0, 0).data().toString(), QStringLiteral("普通说明 / 待测"));
}

void ModelTests::qualityDelegatePreservesRowSelectionAndAccessibleText()
{
    // 提供单元格绘制所需的表格宿主控件。
    QTableView host;
    // 提供用于绘制验证的单元格数据。
    QStandardItemModel model(1, 1);
    // 执行待验证的质量判定单元格绘制。
    QualityDelegate delegate;
    // 配置单元格尺寸、配色和选中状态等绘制条件。
    QStyleOptionViewItem option;
    option.initFrom(&host);
    option.rect = QRect(0, 0, 180, 42);
    option.widget = &host;
    option.showDecorationSelected = true;
    option.palette.setColor(QPalette::Highlight, QColor("#153e72"));
    option.state = QStyle::State_Enabled | QStyle::State_Active;
    // 按当前绘制条件生成单元格图像供比较。
    auto render = [&]
    {
        // 保存本次单元格绘制产生的图像。
        QImage image(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        // 将单元格绘制到验证图像中。
        QPainter painter(&image);
        delegate.paint(&painter, option, model.index(0, 0));
        return image;
    };
    for (const QString &text : QStringList{"PASS", "FAIL", "CANCELLED", "UNKNOWN", "PENDING"})
    {
        model.setData(model.index(0, 0), text);
        // 记录绘制过程中是否误改了模型数据。
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        option.state &= ~QStyle::State_Selected;
        // 保存未选中单元格的绘制图像。
        const auto unselected = render();
        option.state |= QStyle::State_Selected;
        // 保存选中单元格的绘制图像。
        const auto selected = render();
        // Selection remains visible outside the badge, independently of semantic colors.
        QVERIFY(selected.pixelColor(2, 20) != unselected.pixelColor(2, 20));
        QCOMPARE(model.index(0, 0).data(Qt::DisplayRole).toString(), text);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(changes.count(), 0);
    }
}

QTEST_MAIN(ModelTests)
#include "test_models.moc"
