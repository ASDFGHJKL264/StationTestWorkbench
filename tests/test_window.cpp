#include "mainwindow.h"
#include "workbenchcontroller.h"
#include "repository.h"
#include "domain.h"
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

class WindowTests final : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase()
    {
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
