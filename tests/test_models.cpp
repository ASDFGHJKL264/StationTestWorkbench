#include <QtTest>
#include <QAbstractItemModelTester>
#include <QPersistentModelIndex>
#include <QSortFilterProxyModel>
#include <QImage>
#include <QPainter>
#include <QStandardItemModel>
#include <QTableView>
#include "ui/models.h"

class ModelTests : public QObject
{
    Q_OBJECT
  private slots:
    void stationUpdatesPreserveSelectionIdentity();
    void stationRowsAreSortedAndBounded();
    void executionVerdictAndArchiveAreIndependent();
    void stepsDistinguishUnmeasuredZeroAndFailure();
    void stepUpdatesKeepPersistentIndexes();
    void historyCursorDoesNotFollowViewSort();
    void historySortUsesYearAndNumericStation();
    void logsHaveBoundedMemoryAndCorrectRemoval();
    void qualityDelegateFallsBackWithoutChangingModel();
    void qualityDelegatePreservesRowSelectionAndAccessibleText();
};

void ModelTests::stationUpdatesPreserveSelectionIdentity()
{
    StationModel model;
    QAbstractItemModelTester checker(&model,
                                     QAbstractItemModelTester::FailureReportingMode::QtTest);
    model.updateStation({{"stationId", 2}, {"execution", "ready"}});
    QPersistentModelIndex selected = model.index(0, 0);
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
    StationModel model;
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
    StepModel model;
    QAbstractItemModelTester checker(&model,
                                     QAbstractItemModelTester::FailureReportingMode::QtTest);
    QJsonObject unmeasured{
        {"stepId", "voltage"}, {"label", "Voltage"}, {"min", 0.0}, {"max", 1.0}, {"unit", "V"}};
    QJsonObject measured = unmeasured;
    measured["value"] = 0.0;
    measured["passed"] = true;
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
    StepModel model;
    QJsonObject step{{"stepId", "one"}, {"label", "One"}, {"min", 0}, {"max", 1}};
    model.setSteps({step});
    QPersistentModelIndex selected = model.index(0, 1);
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
    HistoryModel model;
    QSortFilterProxyModel proxy;
    proxy.setSourceModel(&model);
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
    const auto source = proxy.mapToSource(proxy.index(1, 0));
    QCOMPARE(model.recordAt(source.row())["sn"].toString(), QString("B"));
    QVERIFY(model.recordAt(9).isEmpty());
    model.setRows({});
    QCOMPARE(model.lastId(), 0);
    QCOMPARE(proxy.rowCount(), 0);
}
void ModelTests::logsHaveBoundedMemoryAndCorrectRemoval()
{
    BoundedLogModel model(3);
    QAbstractItemModelTester checker(&model,
                                     QAbstractItemModelTester::FailureReportingMode::QtTest);
    QSignalSpy removals(&model, &QAbstractItemModel::rowsRemoved);
    model.append("first");
    model.append("second");
    model.append("third");
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
    BoundedLogModel minCapacity(0);
    minCapacity.append("a");
    minCapacity.append("b");
    QCOMPARE(minCapacity.rowCount(), 1);
}

void ModelTests::historySortUsesYearAndNumericStation()
{
    HistoryModel model;
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
    QTableView host;
    QStandardItemModel model(1, 1);
    model.setData(model.index(0, 0), QStringLiteral("普通说明 / 待测"));
    QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
    QualityDelegate custom;
    QStyledItemDelegate ordinary;
    QStyleOptionViewItem option;
    option.initFrom(&host);
    option.rect = QRect(0, 0, 180, 42);
    option.widget = &host;
    auto render = [&](QStyledItemDelegate &delegate)
    {
        QImage image(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
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
    QTableView host;
    QStandardItemModel model(1, 1);
    QualityDelegate delegate;
    QStyleOptionViewItem option;
    option.initFrom(&host);
    option.rect = QRect(0, 0, 180, 42);
    option.widget = &host;
    option.showDecorationSelected = true;
    option.palette.setColor(QPalette::Highlight, QColor("#153e72"));
    option.state = QStyle::State_Enabled | QStyle::State_Active;
    auto render = [&]
    {
        QImage image(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QPainter painter(&image);
        delegate.paint(&painter, option, model.index(0, 0));
        return image;
    };
    for (const QString &text : QStringList{"PASS", "FAIL", "CANCELLED", "UNKNOWN", "PENDING"})
    {
        model.setData(model.index(0, 0), text);
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        option.state &= ~QStyle::State_Selected;
        const auto unselected = render();
        option.state |= QStyle::State_Selected;
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
