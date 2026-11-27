#include "recipeeditordialog.h"
#include "domain.h"
#include "repository.h"
#include "toolstyle.h"
#include "workbenchcontroller.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QtTest>

namespace
{
// 定位编辑窗口的输入和发布结果，测试使用实际可见控件。
struct EditorFields
{
    // 从指定窗口获取稳定命名的控件。
    explicit EditorFields(RecipeEditorDialog &dialog)
        : name(dialog.findChild<QLineEdit *>("recipeNameInput")),
          version(dialog.findChild<QLineEdit *>("recipeVersionInput")),
          steps(dialog.findChild<QTableWidget *>("recipeStepsTable")),
          publish(dialog.findChild<QPushButton *>("publishRecipeButton")),
          status(dialog.findChild<QLabel *>("recipeEditorStatus"))
    {
    }
    // 判断测试所需的实际操作入口是否存在。
    bool valid() const
    {
        return name && version && steps && publish && status;
    }
    // 编辑配方在界面显示的名称。
    QLineEdit *name;
    // 编辑新配方的不可覆盖版本号。
    QLineEdit *version;
    // 编辑有序的项目身份、名称、单位和三个数值。
    QTableWidget *steps;
    // 触发真实的异步发布操作。
    QPushButton *publish;
    // 显示验证、保存或发布失败原因。
    QLabel *status;
};

// 读取本次临时文件，检查失败操作是否保留原内容。
QByteArray readFile(const QString &path)
{
    // 保存待读取的临时文件句柄。
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// 写入仅供本回归导入或故障注入使用的临时内容。
bool writeFile(const QString &path, const QByteArray &bytes)
{
    // 保存待写入的临时文件句柄。
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

// 从实际配方列表中按版本查找完整对象。
QJsonObject findRecipe(const QJsonArray &recipes, const QString &version)
{
    for (const auto &value : recipes)
    {
        // 读取当前可选配方，保留其原始内容用于比较。
        const auto recipe = value.toObject();
        if (recipe["version"].toString() == version)
            return recipe;
    }
    return {};
}

// 将实际表格输入序列化为文本快照，不依赖编辑器内部草稿状态。
QStringList draftSnapshot(const EditorFields &fields)
{
    // 保存字段和每个单元格的当前输入。
    QStringList snapshot{fields.name->text(), fields.version->text()};
    for (int row = 0; row < fields.steps->rowCount(); ++row)
        for (int column = 0; column < fields.steps->columnCount(); ++column)
            snapshot.append(fields.steps->item(row, column)
                                ? fields.steps->item(row, column)->text()
                                : QStringLiteral("<missing>"));
    return snapshot;
}

// 用一项有效电压规则填入真实界面，返回其应发布的完整配方。
QJsonObject fillVoltageDraft(const EditorFields &fields, const QString &version)
{
    fields.name->setText(QStringLiteral("可视化电压测试"));
    fields.version->setText(version);
    fields.steps->setRowCount(1);
    // 将每个输入字段对应到现有的六列表格契约。
    const QStringList values{"voltage", QStringLiteral("输出电压"), "V", "4.85", "5.15", "5"};
    for (int column = 0; column < values.size(); ++column)
    {
        if (!fields.steps->item(0, column))
            fields.steps->setItem(0, column, new QTableWidgetItem);
        fields.steps->item(0, column)->setText(values[column]);
    }
    return {{"version", version},
            {"name", fields.name->text()},
            {"steps", QJsonArray{QJsonObject{{"id", "voltage"},
                                             {"label", QStringLiteral("输出电压")},
                                             {"unit", "V"},
                                             {"min", 4.85},
                                             {"max", 5.15},
                                             {"nominal", 5.0}}}}};
}

// 为有效文件导入选择指定答复，验证草稿保护并避免模态确认阻塞。
bool importWithAnswer(RecipeEditorDialog &dialog, const QString &path, int response)
{
    // 在确认框的嵌套事件循环中选择同意，导入结束即停止。
    QTimer answer;
    QObject::connect(&answer, &QTimer::timeout, &answer,
                     [response]
                     {
                         for (auto *widget : QApplication::topLevelWidgets())
                             if (auto *message = qobject_cast<QMessageBox *>(widget))
                                 message->button(static_cast<QMessageBox::StandardButton>(response))->click();
                     });
    answer.start(5);
    // 保存实际导入结果，便于在停止应答计时器后返回。
    const bool imported = dialog.importFile(path);
    answer.stop();
    return imported;
}

// 在指定证据目录保存真实窗口截图，未要求证据时跳过。
bool saveEvidence(RecipeEditorDialog &dialog)
{
    // 定位本轮原生界面检查使用的截图输出目录。
    const auto directory = qEnvironmentVariable("STATION_UI_EVIDENCE_DIR");
    return directory.isEmpty() ||
           (QDir().mkpath(directory) &&
            dialog.grab().save(QDir(directory).filePath("recipe-editor.png")));
}

// 在独立连接持有真实写锁，验证发布失败不会被列表刷新误报为成功。
class DatabaseWriteLock
{
  public:
    // 为临时数据库创建一条不属于工作台线程的测试连接。
    explicit DatabaseWriteLock(const QString &path)
        : m_name("recipe-editor-lock-" + QUuid::createUuid().toString(QUuid::WithoutBraces)),
          m_database(QSqlDatabase::addDatabase("QSQLITE", m_name))
    {
        m_database.setDatabaseName(path);
        m_database.open();
    }
    // 离开测试作用域时释放写锁并移除连接。
    ~DatabaseWriteLock()
    {
        m_database.rollback();
        m_database.close();
        m_database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_name);
    }
    // 以SQLite真实事务持有写入资格，阻止配方发布提交。
    bool acquire()
    {
        // 执行即时写事务，确保发布遇到真实存储冲突。
        QSqlQuery query(m_database);
        return m_database.isOpen() && query.exec("BEGIN IMMEDIATE");
    }

  private:
    // 为独立写锁分配唯一连接名。
    QString m_name;
    // 持有临时数据库的独立写入连接。
    QSqlDatabase m_database;
};
} // namespace

class RecipeEditorTests final : public QObject
{
    Q_OBJECT
  private slots:
    // 与正式程序使用相同浅色样式，并为离屏平台准备中文字体。
    void initTestCase()
    {
        ToolStyle::configureApplication(*qApp);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        if (QGuiApplication::platformName() == "offscreen")
        {
            // 从系统字体加载中文，避免离屏平台丢失字形。
            const auto font = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
            if (font >= 0)
                QApplication::setFont(
                    QFont(QFontDatabase::applicationFontFamilies(font).first(), 10));
        }
    }

    // 验证关闭重开保留草稿，成功回报对应真实提交且重启数据库后仍可读取。
    void publishedDraftPersistsAfterWindowAndDatabaseRestart()
    {
        // 隔离本次界面、数据库和导出文件。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 保存应持久化的配方，供完全重开数据库后核对。
        QJsonObject expected;
        {
            // 提供真实数据库线程和配方发布服务。
            WorkbenchController controller;
            // 在初始化前建立窗口，使它接收完整就绪和配方列表事件。
            RecipeEditorDialog dialog(&controller);
            // 观察实际后台初始化结果。
            QSignalSpy ready(&controller, &WorkbenchController::ready);
            // 观察发布后重新读取的真实配方列表。
            QSignalSpy lists(&controller, &WorkbenchController::recipesReady);
            // 只统计编辑器确认提交后的成功事件。
            QSignalSpy committed(&dialog, &RecipeEditorDialog::recipeCommitted);
            controller.initialize(directory.path(), {});
            QTRY_VERIFY(!ready.isEmpty() && !lists.isEmpty());
            QVERIFY(ready.last()[0].toBool());
            dialog.show();
            // 使用界面实际控件填写新版本。
            const EditorFields fields(dialog);
            QVERIFY(fields.valid());
            expected = fillVoltageDraft(fields, "editor-voltage-v1");
            // 保存关闭前的用户输入，隐藏窗口不得丢失它。
            const auto draft = draftSnapshot(fields);
            dialog.close();
            QVERIFY(!dialog.isVisible());
            dialog.show();
            QCOMPARE(draftSnapshot(fields), draft);
            QVERIFY(fields.publish->isEnabled());
            QTest::mouseClick(fields.publish, Qt::LeftButton);
            QTRY_COMPARE(committed.count(), 1);
            QCOMPARE(committed.last()[0].toString(), expected["version"].toString());
            QTRY_VERIFY(findRecipe(lists.last()[0].toJsonArray(), "editor-voltage-v1") == expected);
            QVERIFY(saveEvidence(dialog));
            dialog.close();
            controller.shutdown();
        }
        // 完全重建存储连接，排除只在界面或内存中添加配方的假成功。
        Repository reopened;
        // 读取重新打开后的数据库配方列表。
        QSignalSpy stored(&reopened, &Repository::recipesReady);
        reopened.initialize(directory.filePath("data/workbench.db"));
        reopened.listRecipes();
        QVERIFY(!stored.isEmpty());
        QCOMPARE(findRecipe(stored.last()[0].toJsonArray(), "editor-voltage-v1"), expected);
        QCOMPARE(findRecipe(stored.last()[0].toJsonArray(), "demo-5v-v1"), Domain::defaultRecipe());
    }

    // 验证不合法规则和已发布版本被拒绝，界面输入及原配方保持完整。
    void invalidDraftAndPublishedVersionCannotChangeStoredRecipes()
    {
        // 隔离被拒绝的输入与后台数据库。
        QTemporaryDir directory;
        // 提供真实发布服务，验证拒绝不会造成落库。
        WorkbenchController controller;
        // 创建本轮规则校验使用的真实界面。
        RecipeEditorDialog dialog(&controller);
        // 等待完整就绪和初始配方列表。
        QSignalSpy ready(&controller, &WorkbenchController::ready),
            lists(&controller, &WorkbenchController::recipesReady);
        // 记录编辑器成功事件，所有非法提交都不得产生它。
        QSignalSpy committed(&dialog, &RecipeEditorDialog::recipeCommitted);
        controller.initialize(directory.path(), {});
        QTRY_VERIFY(!ready.isEmpty() && !lists.isEmpty());
        QVERIFY(ready.last()[0].toBool());
        dialog.show();
        // 定位表格和发布按钮。
        const EditorFields fields(dialog);
        QVERIFY(fields.valid());
        fillVoltageDraft(fields, "invalid-editor-v1");
        // 用真实添加入口建立第二个项目，再注入重复身份。
        auto *add = dialog.findChild<QPushButton *>("addRecipeStepButton");
        // 通过真实移除入口恢复有效单项目规则。
        auto *remove = dialog.findChild<QPushButton *>("removeRecipeStepButton");
        QVERIFY(add && remove);
        QTest::mouseClick(add, Qt::LeftButton);
        QCOMPARE(fields.steps->rowCount(), 2);
        fields.steps->item(1, 0)->setText("voltage");
        // 比较非法提交前后的完整输入，不能因校验失败被清空。
        const auto duplicate = draftSnapshot(fields);
        QTest::mouseClick(fields.publish, Qt::LeftButton);
        QCOMPARE(committed.count(), 0);
        QCOMPARE(draftSnapshot(fields), duplicate);
        QVERIFY(!fields.status->text().isEmpty());
        fields.steps->selectRow(1);
        QTest::mouseClick(remove, Qt::LeftButton);
        QCOMPARE(fields.steps->rowCount(), 1);
        for (const auto &invalidMaximum : {QString("4.0"), QString("not-a-number")})
        {
            fields.steps->item(0, 4)->setText(invalidMaximum);
            // 当前无效上下限或非数字输入必须原样保留。
            const auto invalid = draftSnapshot(fields);
            QTest::mouseClick(fields.publish, Qt::LeftButton);
            QCOMPARE(committed.count(), 0);
            QCOMPARE(draftSnapshot(fields), invalid);
        }
        fields.steps->item(0, 4)->setText("5.15");
        fields.version->setText("demo-5v-v1");
        QTest::mouseClick(fields.publish, Qt::LeftButton);
        QCOMPARE(committed.count(), 0);
        QCOMPARE(fields.version->text(), QString("demo-5v-v1"));
        // 再读后台列表，证明旧版本没有被覆盖，非法新版本没有被创建。
        const auto before = lists.count();
        controller.requestRecipes();
        QTRY_VERIFY(lists.count() > before);
        QCOMPARE(findRecipe(lists.last()[0].toJsonArray(), "demo-5v-v1"), Domain::defaultRecipe());
        QVERIFY(findRecipe(lists.last()[0].toJsonArray(), "invalid-editor-v1").isEmpty());
    }

    // 验证复制、步骤排列和数量边界，以及JSON导出和导入保留数值精度且不发布。
    void copyReorderAndJsonRoundTripPreserveDraftWithoutPublishing()
    {
        // 隔离导出、导入及步骤编辑生成的文件。
        QTemporaryDir directory;
        // 提供已发布参考配方和存储服务。
        WorkbenchController controller;
        // 创建实际步骤编辑和JSON往返界面。
        RecipeEditorDialog dialog(&controller);
        // 等待初始配方可用，稍后用新列表排除导出误发布。
        QSignalSpy ready(&controller, &WorkbenchController::ready),
            lists(&controller, &WorkbenchController::recipesReady);
        // 统计所有发布成功事件，纯文件操作不得触发。
        QSignalSpy committed(&dialog, &RecipeEditorDialog::recipeCommitted);
        controller.initialize(directory.path(), {});
        QTRY_VERIFY(!ready.isEmpty() && !lists.isEmpty());
        dialog.prepare("demo-5v-v1");
        dialog.show();
        // 编辑复制出的全新版本，原对象仍保存在后台。
        const EditorFields fields(dialog);
        QVERIFY(fields.valid());
        QVERIFY(!fields.version->text().isEmpty());
        QVERIFY(fields.version->text() != "demo-5v-v1");
        QVERIFY(fields.version->text().size() <= 64);
        QCOMPARE(fields.steps->rowCount(), 3);
        // 用实际按钮调整两项的相对执行顺序。
        auto *up = dialog.findChild<QPushButton *>("moveRecipeStepUpButton");
        // 用实际按钮将选中项移回原位置。
        auto *down = dialog.findChild<QPushButton *>("moveRecipeStepDownButton");
        // 验证最多允许十六个项目。
        auto *add = dialog.findChild<QPushButton *>("addRecipeStepButton");
        // 验证至少保留一个项目。
        auto *remove = dialog.findChild<QPushButton *>("removeRecipeStepButton");
        QVERIFY(up && down && add && remove);
        fields.steps->selectRow(1);
        QTest::mouseClick(up, Qt::LeftButton);
        QCOMPARE(fields.steps->item(0, 0)->text(), QString("current"));
        QCOMPARE(fields.steps->item(1, 0)->text(), QString("voltage"));
        QTest::mouseClick(down, Qt::LeftButton);
        QCOMPARE(fields.steps->item(0, 0)->text(), QString("voltage"));
        for (int count = fields.steps->rowCount(); count < 16; ++count)
            QTest::mouseClick(add, Qt::LeftButton);
        QCOMPARE(fields.steps->rowCount(), 16);
        QVERIFY(!add->isEnabled());
        QTest::mouseClick(add, Qt::LeftButton);
        QCOMPARE(fields.steps->rowCount(), 16);
        while (fields.steps->rowCount() > 1)
        {
            fields.steps->selectRow(fields.steps->rowCount() - 1);
            QTest::mouseClick(remove, Qt::LeftButton);
        }
        QVERIFY(!remove->isEnabled());
        fields.steps->item(0, 3)->setText("0.123456789012345");
        fields.steps->item(0, 4)->setText("0.223456789012345");
        fields.steps->item(0, 5)->setText("0.173456789012345");
        // 保存该复制版本的身份，导出后它仍只能存在于草稿中。
        const auto version = fields.version->text();
        // 保存JSON导出的实际位置。
        const auto path = directory.filePath("editable-draft.json");
        QVERIFY(dialog.exportFile(path));
        // 解析实际文件，精度不能被固定小数位编辑控件舍入。
        const auto exported = QJsonDocument::fromJson(readFile(path)).object();
        QVERIFY(Domain::validateRecipe(exported));
        QCOMPARE(exported["steps"].toArray()[0].toObject()["min"].toDouble(), 0.123456789012345);
        QCOMPARE(exported["steps"].toArray()[0].toObject()["nominal"].toDouble(),
                 0.173456789012345);
        // 取消替换时保留手工输入，包括尚未规范化的数值文本。
        const auto beforeImport = draftSnapshot(fields);
        QVERIFY(!importWithAnswer(dialog, path, QMessageBox::No));
        QCOMPARE(draftSnapshot(fields), beforeImport);
        QVERIFY(importWithAnswer(dialog, path, QMessageBox::Yes));
        // 再次导出并比较完整对象，覆盖所有项目字段的JSON往返。
        const auto roundTrip = directory.filePath("round-trip.json");
        QVERIFY(dialog.exportFile(roundTrip));
        QCOMPARE(QJsonDocument::fromJson(readFile(roundTrip)).object(), exported);
        QCOMPARE(committed.count(), 0);
        // 重新读取真实后台，导入和导出都不能隐式发布配方。
        const auto before = lists.count();
        // 后台刷新来源列表不能覆盖当前未发布草稿。
        const auto beforeRefresh = draftSnapshot(fields);
        controller.requestRecipes();
        QTRY_VERIFY(lists.count() > before);
        QCOMPARE(draftSnapshot(fields), beforeRefresh);
        QVERIFY(findRecipe(lists.last()[0].toJsonArray(), version).isEmpty());
        QCOMPARE(findRecipe(lists.last()[0].toJsonArray(), "demo-5v-v1"), Domain::defaultRecipe());
    }

    // 验证存储冲突不能被无关成功回报掩盖，失败后草稿可再次提交。
    void failedPublishKeepsDraftAndIgnoresUnrelatedSuccess()
    {
        // 隔离当前发布、写锁和后续重试的数据。
        QTemporaryDir directory;
        // 使用真实数据库线程执行失败与成功两次发布。
        WorkbenchController controller;
        // 提供发布状态与草稿保留的实际界面。
        RecipeEditorDialog dialog(&controller);
        // 等待后台初始化，并观察逐次真实发布结果。
        QSignalSpy ready(&controller, &WorkbenchController::ready),
            results(&controller, &WorkbenchController::recipePublishFinished);
        // 用于确认失败和无关成功均未使编辑器宣告提交成功。
        QSignalSpy committed(&dialog, &RecipeEditorDialog::recipeCommitted);
        controller.initialize(directory.path(), {});
        QTRY_VERIFY(!ready.isEmpty());
        QVERIFY(ready.last()[0].toBool());
        dialog.show();
        // 填写需要在数据库锁释放后重试的配方。
        const EditorFields fields(dialog);
        QVERIFY(fields.valid());
        // 保留本次发布的完整内容，以匹配异步结果。
        const auto pending = fillVoltageDraft(fields, "retry-after-lock-v1");
        // 比较失败回报前后的全部用户输入。
        const auto draft = draftSnapshot(fields);
        {
            // 持有外部真实写锁，让发布事务返回明确失败。
            DatabaseWriteLock lock(directory.filePath("data/workbench.db"));
            QVERIFY(lock.acquire());
            QTest::mouseClick(fields.publish, Qt::LeftButton);
            QVERIFY(!fields.publish->isEnabled());
            controller.recipePublishFinished(Domain::defaultRecipe(), true, {});
            QCOMPARE(committed.count(), 0);
            QVERIFY(!fields.publish->isEnabled());
            QTRY_VERIFY(results.count() >= 2);
            QCOMPARE(results.last()[0].toJsonObject(), pending);
            QVERIFY(!results.last()[1].toBool());
            QCOMPARE(committed.count(), 0);
            QCOMPARE(draftSnapshot(fields), draft);
            QVERIFY(!fields.status->text().isEmpty());
            QVERIFY(fields.publish->isEnabled());
        }
        QTest::mouseClick(fields.publish, Qt::LeftButton);
        QTRY_COMPARE(committed.count(), 1);
        QCOMPARE(committed.last()[0].toString(), QString("retry-after-lock-v1"));
    }

    // 验证后台未就绪时仍能编辑和导出，坏文件或无效导出不清空输入和原文件。
    void offlineEditingAndInvalidFilesPreserveDraft()
    {
        // 隔离当前草稿的有效和损坏文件。
        QTemporaryDir directory;
        // 保持未初始化，确认发布入口受真实后端状态约束。
        WorkbenchController controller;
        // 提供离线编辑、导入和导出功能。
        RecipeEditorDialog dialog(&controller);
        // 统计未初始化时不应产生的成功发布。
        QSignalSpy committed(&dialog, &RecipeEditorDialog::recipeCommitted);
        dialog.show();
        // 填入完整有效草稿，后端离线也允许导出。
        const EditorFields fields(dialog);
        QVERIFY(fields.valid());
        fillVoltageDraft(fields, "offline-draft-v1");
        QVERIFY(!fields.publish->isEnabled());
        QTest::mouseClick(fields.publish, Qt::LeftButton);
        QCOMPARE(committed.count(), 0);
        // 保存第一份有效文件，后续故障不得破坏它。
        const auto path = directory.filePath("saved-draft.json");
        QVERIFY(dialog.exportFile(path));
        // 比较不合法导入前后的全部草稿内容。
        const auto original = draftSnapshot(fields);
        QVERIFY(!dialog.exportFile(directory.path()));
        QCOMPARE(draftSnapshot(fields), original);
        // 保存用于注入格式和大小错误的临时文件。
        const auto bad = directory.filePath("bad.json");
        QVERIFY(writeFile(bad, "not-json"));
        QVERIFY(!dialog.importFile(bad));
        QCOMPARE(draftSnapshot(fields), original);
        QVERIFY(writeFile(bad, QByteArray(256 * 1024 + 1, 'x')));
        QVERIFY(!dialog.importFile(bad));
        QCOMPARE(draftSnapshot(fields), original);
        // 已有导出文件必须在新草稿校验失败时原样保留。
        const auto savedBytes = readFile(path);
        fields.steps->item(0, 3)->setText("invalid-number");
        QVERIFY(!dialog.exportFile(path));
        QCOMPARE(readFile(path), savedBytes);
        QCOMPARE(fields.steps->item(0, 3)->text(), QString("invalid-number"));
        QCOMPARE(committed.count(), 0);
    }
};

QTEST_MAIN(RecipeEditorTests)
#include "test_recipe_editor.moc"
