#include "mainwindow.h"
#include "workbenchcontroller.h"
#include "repository.h"
#include "domain.h"
#include "toolstyle.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <QDir>
#include <QFontDatabase>
#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QTimer>
#include <QComboBox>
#include <QCheckBox>
#include <QDateEdit>
#include <QLabel>
#include <QTabBar>
#include <QPlainTextEdit>
#include <cmath>

namespace
{
class RestoreApplicationPalette
{
  public:
    ~RestoreApplicationPalette()
    {
        QApplication::setPalette(m_palette);
    }

  private:
    QPalette m_palette = QApplication::palette();
};

QPalette systemPalette(bool dark)
{
    QPalette palette = QApplication::palette();
    const QColor background(dark ? "#202020" : "#f3f3f3");
    const QColor foreground(dark ? "#ffffff" : "#222222");
    for (auto role : {QPalette::Window, QPalette::Base, QPalette::Button})
        palette.setColor(QPalette::All, role, background);
    for (auto role :
         {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::PlaceholderText})
        palette.setColor(QPalette::All, role, foreground);
    return palette;
}

double contrast(const QColor &foreground, const QColor &background)
{
    const auto luminance = [](const QColor &color)
    {
        const auto linear = [](double value)
        { return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4); };
        return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) +
               0.0722 * linear(color.blueF());
    };
    const double a = luminance(foreground), b = luminance(background);
    return (qMax(a, b) + 0.05) / (qMin(a, b) + 0.05);
}

QList<QPair<QPalette::ColorRole, QPalette::ColorRole>> textRoles(QWidget *widget)
{
    if (qobject_cast<QLineEdit *>(widget))
        return {{QPalette::Text, QPalette::Base},
                {QPalette::PlaceholderText, QPalette::Base},
                {QPalette::HighlightedText, QPalette::Highlight}};
    if (qobject_cast<QPlainTextEdit *>(widget))
        return {{QPalette::Text, QPalette::Base}, {QPalette::HighlightedText, QPalette::Highlight}};
    if (qobject_cast<QAbstractItemView *>(widget))
        return {{QPalette::Text, QPalette::Base},
                {QPalette::Text, QPalette::AlternateBase},
                {QPalette::HighlightedText, QPalette::Highlight}};
    if (qobject_cast<QPushButton *>(widget) || qobject_cast<QComboBox *>(widget))
        return {{QPalette::ButtonText, QPalette::Button}};
    return {{QPalette::WindowText, QPalette::Window}};
}
} // namespace

class WindowTests final : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        ToolStyle::configureApplication(*qApp);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        // The Windows offscreen plugin does not discover system fonts automatically.
        if (QGuiApplication::platformName() == "offscreen")
        {
            const auto id = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
            if (id >= 0)
                QApplication::setFont(
                    QFont(QFontDatabase::applicationFontFamilies(id).first(), 10));
        }
    }
    void interfaceRemainsReadableAcrossSystemThemes_data()
    {
        QTest::addColumn<bool>("initialDark");
        QTest::newRow("light-to-dark") << false;
        QTest::newRow("dark-to-light") << true;
    }
    void interfaceRemainsReadableAcrossSystemThemes()
    {
        QFETCH(bool, initialDark);
        RestoreApplicationPalette restore;
        QApplication::setPalette(systemPalette(initialDark));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        MainWindow window(directory.path(), {{1, "127.0.0.1", 48591}});
        window.show();
        auto *sn = window.findChild<QLineEdit *>("snInput");
        auto *filter = window.findChild<QLineEdit *>("historyFilter");
        auto *recipe = window.findChild<QComboBox *>("recipeCombo");
        auto *start = window.findChild<QPushButton *>("startButton");
        auto *query = window.findChild<QPushButton *>("queryHistoryButton");
        auto *from = window.findChild<QDateEdit *>("historyFromDate");
        auto *through = window.findChild<QDateEdit *>("historyToDate");
        auto *stationFilter = window.findChild<QComboBox *>("historyStationFilter");
        auto *verdictFilter = window.findChild<QComboBox *>("historyVerdictFilter");
        auto *reconcile = window.findChild<QPushButton *>("reconcileButton");
        auto *stations = window.findChild<QTableView *>("stationView");
        auto *steps = window.findChild<QTableView *>("stepView");
        auto *history = window.findChild<QTableView *>("historyView");
        auto *tabs = window.findChild<QTabBar *>();
        QVERIFY(sn && filter && recipe && start && query && from && through && stationFilter &&
                verdictFilter && reconcile && stations && steps && history && tabs);
        QTRY_VERIFY(query->isEnabled());
        for (int phase = 0; phase < 2; ++phase)
        {
            if (phase)
                QApplication::setPalette(systemPalette(!initialDark));
            QCoreApplication::processEvents();
            const auto evidence = qEnvironmentVariable("STATION_UI_EVIDENCE_DIR");
            if (!evidence.isEmpty())
            {
                QVERIFY(QDir().mkpath(evidence));
                QVERIFY(window.grab().save(QDir(evidence).filePath(
                    QString("workbench-%1-%2.png").arg(QTest::currentDataTag()).arg(phase))));
            }
            for (auto *widget : QList<QWidget *>{&window, sn, filter, recipe, start, query, from,
                                                 through, stationFilter, verdictFilter, reconcile,
                                                 stations, steps, history, tabs})
            {
                const auto palette = widget->palette();
                for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
                {
                    const double minimum = group == QPalette::Disabled ? 3.0 : 4.5;
                    for (const auto pair : textRoles(widget))
                    {
                        const auto detail = QString("%1 / group %2 / role %3 / phase %4")
                                                .arg(widget->metaObject()->className())
                                                .arg(group)
                                                .arg(pair.first)
                                                .arg(phase)
                                                .toUtf8();
                        QVERIFY2(contrast(palette.color(group, pair.first),
                                          palette.color(group, pair.second)) >= minimum,
                                 detail.constData());
                    }
                }
            }
        }
        window.close();
    }
    void historyFiltersResetPaginationAndKeepDatesOptional()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto clientDirectory = directory.filePath("client");
        {
            Repository repository;
            repository.initialize(clientDirectory + "/data/workbench.db");
            QSignalSpy reserved(&repository, &Repository::runReserved),
                saved(&repository, &Repository::resultSaved);
            const auto recipe = Domain::defaultRecipe();
            for (int i = 0; i < 103; ++i)
            {
                QJsonObject run{{"runId", QString("run-%1").arg(i)},
                                {"stationId", 2},
                                {"sn", QString("SN-%1").arg(i)},
                                {"recipeVersion", recipe["version"]},
                                {"recipe", recipe}};
                repository.reserveRun(run);
                QVERIFY(reserved.last()[1].toBool());
                QJsonArray steps;
                for (const auto &value : recipe["steps"].toArray())
                {
                    const auto rule = value.toObject();
                    steps.append(QJsonObject{{"stepId", rule["id"]}, {"value", rule["nominal"]}});
                }
                auto result = run;
                result["outcome"] = "completed";
                result["steps"] = steps;
                repository.saveResult(result);
                QVERIFY(saved.last()[1].toBool());
            }
        }
        MainWindow window(clientDirectory, {});
        window.show();
        auto *query = window.findChild<QPushButton *>("queryHistoryButton");
        auto *older = window.findChild<QPushButton *>("olderHistoryButton");
        auto *history = window.findChild<QTableView *>("historyView");
        auto *sn = window.findChild<QLineEdit *>("historyFilter");
        auto *station = window.findChild<QComboBox *>("historyStationFilter");
        auto *verdict = window.findChild<QComboBox *>("historyVerdictFilter");
        auto *fromEnabled = window.findChild<QCheckBox *>("historyFromEnabled");
        auto *toEnabled = window.findChild<QCheckBox *>("historyToEnabled");
        auto *from = window.findChild<QDateEdit *>("historyFromDate");
        auto *through = window.findChild<QDateEdit *>("historyToDate");
        QVERIFY(query && older && history && sn && station && verdict && fromEnabled && toEnabled &&
                from && through);
        QVERIFY(!fromEnabled->isChecked() && !toEnabled->isChecked());
        QVERIFY(!from->isEnabled() && !through->isEnabled());
        QTRY_COMPARE(history->model()->rowCount(), 100);
        QTRY_VERIFY(older->isEnabled());
        QTest::mouseClick(older, Qt::LeftButton);
        QTRY_COMPARE(history->model()->rowCount(), 3);
        QTRY_VERIFY(query->isEnabled());
        station->setCurrentIndex(station->findData(2));
        QVERIFY(!older->isEnabled());
        QTest::mouseClick(query, Qt::LeftButton);
        QTRY_COMPARE(history->model()->rowCount(), 100);
        QTRY_VERIFY(older->isEnabled());
        sn->setText("SN-7");
        QVERIFY(!older->isEnabled());
        QTest::mouseClick(query, Qt::LeftButton);
        QTRY_COMPARE(history->model()->rowCount(), 1);
        QCOMPARE(history->model()->index(0, 2).data().toString(), "SN-7");
        verdict->setCurrentIndex(verdict->findData("FAIL"));
        QTest::mouseClick(query, Qt::LeftButton);
        QTRY_COMPARE(history->model()->rowCount(), 0);
        QTRY_VERIFY(query->isEnabled());
        verdict->setCurrentIndex(verdict->findData("PASS"));
        QTest::mouseClick(query, Qt::LeftButton);
        QTRY_COMPARE(history->model()->rowCount(), 1);
        QTRY_VERIFY(query->isEnabled());

        from->setDate(QDate::currentDate().addDays(1));
        fromEnabled->setChecked(true);
        QVERIFY(from->isEnabled());
        QTest::mouseClick(query, Qt::LeftButton);
        QTRY_COMPARE(history->model()->rowCount(), 0);
        QTRY_VERIFY(query->isEnabled());
        fromEnabled->setChecked(false);
        through->setDate(QDate::currentDate().addDays(-1));
        toEnabled->setChecked(true);
        QVERIFY(through->isEnabled());
        QTest::mouseClick(query, Qt::LeftButton);
        QTRY_VERIFY(query->isEnabled());
        QCOMPARE(history->model()->rowCount(), 0);
        toEnabled->setChecked(false);
        QTest::mouseClick(query, Qt::LeftButton);
        QTRY_COMPARE(history->model()->rowCount(), 1);
        QTRY_VERIFY(query->isEnabled());
        QSignalSpy ready(window.controller(), &WorkbenchController::historyReady);
        fromEnabled->setChecked(true);
        toEnabled->setChecked(true);
        QTest::mouseClick(query, Qt::LeftButton);
        QCOMPARE(ready.size(), 0);
        QVERIFY(query->isEnabled());
        window.close();
    }
    void obsoleteHistoryRepliesAndWarningsDoNotChangeCurrentQuery()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        MainWindow window(directory.path(), {});
        auto *query = window.findChild<QPushButton *>("queryHistoryButton");
        auto *station = window.findChild<QComboBox *>("historyStationFilter");
        auto *older = window.findChild<QPushButton *>("olderHistoryButton");
        QVERIFY(query && station && older);
        QSignalSpy history(window.controller(), &WorkbenchController::historyReady),
            failed(window.controller(), &WorkbenchController::historyFailed);
        QTRY_COMPARE(history.size(), 1);
        history.clear();
        HistoryFilter invalid;
        invalid.stationId = 5;
        window.controller()->refreshFilteredHistory({});
        window.controller()->refreshFilteredHistory(invalid);
        HistoryFilter current;
        current.stationId = 2;
        window.controller()->refreshFilteredHistory(current);
        QTRY_COMPARE(history.size(), 1);
        QCOMPARE(failed.size(), 0);

        // A late response for unchanged backend conditions must not revive a stale page cursor
        // after the user has edited the controls without submitting another query.
        window.controller()->historyReady({QJsonObject{{"id", 150}}}, true);
        QVERIFY(older->isEnabled());
        station->setCurrentIndex(station->findData(3));
        QVERIFY(!older->isEnabled());
        window.controller()->historyReady({QJsonObject{{"id", 149}}}, true);
        QVERIFY(!older->isEnabled());
        QTest::mouseClick(query, Qt::LeftButton);
        QVERIFY(!query->isEnabled());
        window.controller()->logMessage("unrelated network warning", true);
        QVERIFY(!query->isEnabled());
        QTRY_VERIFY(query->isEnabled());
        window.close();
    }
    void reconciliationButtonFollowsSelectedStationCapability()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        MainWindow window(directory.path(), {{1, "127.0.0.1", 48592}, {2, "127.0.0.1", 48593}});
        window.show();
        auto *query = window.findChild<QPushButton *>("queryHistoryButton");
        auto *reconcile = window.findChild<QPushButton *>("reconcileButton");
        auto *start = window.findChild<QPushButton *>("startButton");
        auto *stations = window.findChild<QTableView *>("stationView");
        QVERIFY(query && reconcile && start && stations);
        QVERIFY(!reconcile->isEnabled());
        QTRY_VERIFY(query->isEnabled());
        window.controller()->stationUpdated({{"stationId", 1}, {"canReconcile", true},
                                              {"canStart", false}, {"runId", "held-run"}});
        QVERIFY(reconcile->isEnabled());
        QVERIFY(!start->isEnabled());
        stations->selectRow(1);
        QVERIFY(!reconcile->isEnabled());
        stations->selectRow(0);
        QVERIFY(reconcile->isEnabled());
        QTest::mouseClick(reconcile, Qt::LeftButton);
        QVERIFY(!reconcile->isEnabled());
        QVERIFY(!start->isEnabled());
        window.controller()->stationUpdated({{"stationId", 1}, {"canReconcile", false},
                                              {"canStart", false}});
        QVERIFY(!reconcile->isEnabled());
        window.close();
    }
    void failedSnExportButtonUsesWholeHistoryAndCanBeCancelled()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto clientDirectory = directory.filePath("client");
        {
            Repository repository;
            repository.initialize(clientDirectory + "/data/workbench.db");
            QSignalSpy reserved(&repository, &Repository::runReserved),
                saved(&repository, &Repository::resultSaved);
            const auto recipe = Domain::defaultRecipe();
            for (const auto &sn : {QString("FAILED-SN"), QString("PASS-ONLY")})
            {
                QJsonObject run{{"runId", sn},
                                {"stationId", 1},
                                {"sn", sn},
                                {"recipeVersion", recipe["version"]},
                                {"recipe", recipe}};
                repository.reserveRun(run);
                QVERIFY(reserved.last()[1].toBool());
                QJsonArray steps;
                for (const auto &value : recipe["steps"].toArray())
                {
                    const auto rule = value.toObject();
                    steps.append(
                        QJsonObject{{"stepId", rule["id"]},
                                    {"value", sn == "FAILED-SN" ? rule["max"].toDouble() + 1
                                                                : rule["nominal"].toDouble()}});
                }
                auto result = run;
                result["outcome"] = "completed";
                result["steps"] = steps;
                repository.saveResult(result);
                QVERIFY(saved.last()[1].toBool());
            }
        }
        MainWindow window(clientDirectory, {});
        auto *button = window.findChild<QPushButton *>("exportFailedSnsButton");
        auto *filter = window.findChild<QLineEdit *>("historyFilter");
        auto *query = window.findChild<QPushButton *>("queryHistoryButton");
        auto *history = window.findChild<QTableView *>("historyView");
        QVERIFY(button && filter && query && history);
        QVERIFY(!button->isEnabled());
        window.show();
        QTRY_VERIFY(button->isEnabled() && query->isEnabled());
        filter->setText("PASS-ONLY");
        QTest::mouseClick(query, Qt::LeftButton);
        QTRY_COMPARE(history->model()->rowCount(), 1);
        QCOMPARE(history->model()->index(0, 2).data().toString(), "PASS-ONLY");
        auto *station = window.findChild<QComboBox *>("historyStationFilter");
        auto *verdict = window.findChild<QComboBox *>("historyVerdictFilter");
        auto *fromEnabled = window.findChild<QCheckBox *>("historyFromEnabled");
        auto *from = window.findChild<QDateEdit *>("historyFromDate");
        QVERIFY(station && verdict && fromEnabled && from);
        station->setCurrentIndex(station->findData(4));
        verdict->setCurrentIndex(verdict->findData("UNKNOWN"));
        from->setDate(QDate::currentDate().addDays(1));
        fromEnabled->setChecked(true);
        QTest::mouseClick(query, Qt::LeftButton);
        QTRY_COMPARE(history->model()->rowCount(), 0);
        QTRY_VERIFY(query->isEnabled());
        QSignalSpy exported(window.controller(), &WorkbenchController::failedSnsExported);
        const auto output = directory.filePath("failed-sns.csv");
        QTimer dialogTimeout;
        dialogTimeout.setSingleShot(true);
        connect(&dialogTimeout, &QTimer::timeout, &window,
                [&window]
                {
                    if (auto *dialog = window.findChild<QFileDialog *>())
                        dialog->reject();
                });
        bool selected = false;
        QTimer::singleShot(0, &window,
                           [&]
                           {
                               if (auto *dialog = window.findChild<QFileDialog *>())
                               {
                                   selected = true;
                                   dialog->selectFile(output);
                                   QMetaObject::invokeMethod(dialog, "accept");
                               }
                           });
        dialogTimeout.start(3000);
        QTest::mouseClick(button, Qt::LeftButton);
        dialogTimeout.stop();
        QVERIFY(selected);
        QTRY_COMPARE(exported.size(), 1);
        QVERIFY(exported.last()[1].toBool());
        QCOMPARE(exported.last()[2].toLongLong(), 1);
        QVERIFY(button->isEnabled());
        QFile file(output);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("\xEF\xBB\xBF"
                                            "SN\r\n\"FAILED-SN\"\r\n"));
        bool cancelled = false;
        QTimer::singleShot(0, &window,
                           [&]
                           {
                               if (auto *dialog = window.findChild<QFileDialog *>())
                               {
                                   cancelled = true;
                                   dialog->reject();
                               }
                           });
        dialogTimeout.start(3000);
        QTest::mouseClick(button, Qt::LeftButton);
        dialogTimeout.stop();
        QVERIFY(cancelled);
        QCOMPARE(exported.size(), 1);
        QVERIFY(button->isEnabled());
        window.close();
    }
};
QTEST_MAIN(WindowTests)
#include "test_window.moc"
