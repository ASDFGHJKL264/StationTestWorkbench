#include "recipeeditordialog.h"
#include "domain.h"
#include "toolstyle.h"
#include "workbenchcontroller.h"
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScreen>
#include <QSet>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

namespace
{
// 使用JSON最短往返表示，保留双精度值并避免把4.9显示成浮点尾差。
QString numberText(double value)
{
    // 序列化单个数字，去掉数组外壳后直接用于可编辑文本。
    const auto bytes = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(bytes.mid(1, bytes.size() - 2));
}
} // namespace

RecipeEditorDialog::RecipeEditorDialog(WorkbenchController *controller, QWidget *parent)
    : QDialog(parent), m_controller(controller)
{
    setObjectName("recipeEditorDialog");
    setWindowTitle(QStringLiteral("配方编辑"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setPalette(ToolStyle::lightPalette());
    setStyleSheet(ToolStyle::stylesheet());
    resize(1000, 590);
    if (auto *screen = QGuiApplication::primaryScreen())
        resize(qMin(1000, screen->availableGeometry().width() - 40),
               qMin(590, screen->availableGeometry().height() - 60));
    // 依次排列来源选择、配方身份、步骤和操作结果。
    auto *layout = new QVBoxLayout(this);
    // 将新建和复制入口集中在同一行。
    auto *sources = new QHBoxLayout;
    m_new = new QPushButton(QStringLiteral("新建"), this);
    m_new->setObjectName("newRecipeButton");
    sources->addWidget(m_new);
    sources->addWidget(new QLabel(QStringLiteral("参考配方"), this));
    m_source = new QComboBox(this);
    m_source->setObjectName("recipeSourceCombo");
    m_source->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_source->setMinimumContentsLength(18);
    sources->addWidget(m_source, 1);
    m_copy = new QPushButton(QStringLiteral("复制所选配方"), this);
    m_copy->setObjectName("copyRecipeButton");
    sources->addWidget(m_copy);
    layout->addLayout(sources);
    // 单独显示可编辑的名称与新版本编号。
    auto *identity = new QFormLayout;
    m_name = new QLineEdit(this);
    m_name->setObjectName("recipeNameInput");
    m_name->setMaxLength(100);
    m_name->setClearButtonEnabled(true);
    m_version = new QLineEdit(this);
    m_version->setObjectName("recipeVersionInput");
    m_version->setMaxLength(64);
    m_version->setPlaceholderText(QStringLiteral("例如 product-v2"));
    m_version->setClearButtonEnabled(true);
    identity->addRow(QStringLiteral("配方名称"), m_name);
    identity->addRow(QStringLiteral("版本号"), m_version);
    layout->addLayout(identity);
    m_steps = new QTableWidget(0, 6, this);
    m_steps->setObjectName("recipeStepsTable");
    m_steps->setHorizontalHeaderLabels({QStringLiteral("项目编号"), QStringLiteral("测试项目"),
                                        QStringLiteral("单位"), QStringLiteral("下限"),
                                        QStringLiteral("上限"), QStringLiteral("正常参考值")});
    m_steps->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_steps->setSelectionMode(QAbstractItemView::SingleSelection);
    m_steps->setAlternatingRowColors(true);
    m_steps->setSortingEnabled(false);
    m_steps->setWordWrap(false);
    m_steps->verticalHeader()->setDefaultSectionSize(34);
    m_steps->horizontalHeader()->setMinimumSectionSize(65);
    m_steps->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_steps->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_steps->setColumnWidth(0, 130);
    m_steps->setColumnWidth(2, 85);
    for (int column = 3; column < 6; ++column)
        m_steps->setColumnWidth(column, 145);
    layout->addWidget(m_steps, 1);
    // 将只影响步骤顺序和数量的动作放在表格下方。
    auto *stepActions = new QHBoxLayout;
    m_add = new QPushButton(QStringLiteral("添加项目"), this);
    m_add->setObjectName("addRecipeStepButton");
    m_remove = new QPushButton(QStringLiteral("删除项目"), this);
    m_remove->setObjectName("removeRecipeStepButton");
    m_up = new QPushButton(QStringLiteral("上移"), this);
    m_up->setObjectName("moveRecipeStepUpButton");
    m_down = new QPushButton(QStringLiteral("下移"), this);
    m_down->setObjectName("moveRecipeStepDownButton");
    for (auto *button : {m_add, m_remove, m_up, m_down})
        stepActions->addWidget(button);
    stepActions->addStretch();
    layout->addLayout(stepActions);
    m_status = new QLabel(this);
    m_status->setObjectName("recipeEditorStatus");
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->hide();
    layout->addWidget(m_status);
    // 文件操作与数据库发布各有独立入口。
    auto *publication = new QHBoxLayout;
    m_import = new QPushButton(QStringLiteral("导入 JSON"), this);
    m_import->setObjectName("importRecipeJsonButton");
    m_export = new QPushButton(QStringLiteral("导出 JSON"), this);
    m_export->setObjectName("exportRecipeJsonButton");
    m_publish = new QPushButton(QStringLiteral("发布新版本"), this);
    m_publish->setObjectName("publishRecipeButton");
    for (auto *button :
         {m_new, m_copy, m_add, m_remove, m_up, m_down, m_import, m_export, m_publish})
        button->setAutoDefault(false);
    publication->addWidget(m_import);
    publication->addWidget(m_export);
    publication->addStretch();
    publication->addWidget(m_publish);
    layout->addLayout(publication);
    connect(m_name, &QLineEdit::textChanged, this, &RecipeEditorDialog::markDirty);
    connect(m_version, &QLineEdit::textChanged, this, &RecipeEditorDialog::markDirty);
    connect(m_steps, &QTableWidget::itemChanged, this, &RecipeEditorDialog::markDirty);
    connect(m_steps, &QTableWidget::itemSelectionChanged, this, &RecipeEditorDialog::updateActions);
    connect(m_source, &QComboBox::currentIndexChanged, this, &RecipeEditorDialog::updateActions);
    connect(m_new, &QPushButton::clicked, this,
            [this]
            {
                if (confirmReplace())
                    resetDraft();
            });
    connect(m_copy, &QPushButton::clicked, this, &RecipeEditorDialog::copySelectedRecipe);
    connect(m_add, &QPushButton::clicked, this, &RecipeEditorDialog::addStep);
    connect(m_remove, &QPushButton::clicked, this, &RecipeEditorDialog::removeStep);
    connect(m_up, &QPushButton::clicked, this, [this] { moveStep(-1); });
    connect(m_down, &QPushButton::clicked, this, [this] { moveStep(1); });
    connect(m_import, &QPushButton::clicked, this,
            [this]
            {
                // 默认打开发布包中的示例目录，用户仍可选择其他位置。
                const auto directory =
                    QDir(QCoreApplication::applicationDirPath()).filePath("recipes");
                // 保存用户选择的输入路径，取消时保留草稿。
                const auto path =
                    QFileDialog::getOpenFileName(this, QStringLiteral("导入配方草稿"), directory,
                                                 QStringLiteral("JSON 配方 (*.json)"));
                if (!path.isEmpty())
                    importFile(path);
            });
    connect(m_export, &QPushButton::clicked, this,
            [this]
            {
                // 保存导出前校验失败的原因。
                QString error;
                // 先校验完整草稿，避免用户选定文件后才发现内容无效。
                QJsonObject recipe;
                if (!buildRecipe(&recipe, &error))
                {
                    showMessage(error, true);
                    return;
                }
                // 将允许用于版本号、但不适合文件名的字符替换为下划线。
                auto filename = recipe["version"].toString();
                filename.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), "_");
                // 保存用户明确选择的输出位置。
                const auto path = QFileDialog::getSaveFileName(
                    this, QStringLiteral("导出配方草稿"), filename + ".json",
                    QStringLiteral("JSON 配方 (*.json)"));
                if (!path.isEmpty())
                    exportFile(path);
            });
    connect(m_publish, &QPushButton::clicked, this, &RecipeEditorDialog::publish);
    connect(m_controller, &WorkbenchController::recipesReady, this,
            &RecipeEditorDialog::setRecipes);
    connect(m_controller, &WorkbenchController::ready, this,
            [this](bool ok)
            {
                m_backendReady = ok;
                updateActions();
            });
    connect(m_controller, &WorkbenchController::recipePublishFinished, this,
            [this](const QJsonObject &recipe, bool ok, const QString &error)
            {
                if (!m_busy || recipe != m_pending)
                    return;
                m_busy = false;
                m_pending = {};
                if (ok)
                {
                    m_dirty = false;
                    if (!versionExists(recipe["version"].toString()))
                        m_recipes.append(recipe);
                    showMessage(QStringLiteral("配方已发布：") + recipe["version"].toString());
                    emit recipeCommitted(recipe["version"].toString());
                }
                else
                    showMessage(QStringLiteral("配方未发布：") + error, true);
                updateActions();
            });
    resetDraft();
    m_initialized = false;
    m_controller->requestRecipes();
}

void RecipeEditorDialog::prepare(const QString &sourceVersion)
{
    if (m_initialized || m_dirty || m_busy)
        return;
    for (const auto &value : m_recipes)
    {
        // 保留原配方名称和规则，仅为编辑副本建立新版本。
        auto recipe = value.toObject();
        if (recipe["version"].toString() != sourceVersion)
            continue;
        recipe["version"] = suggestedVersion(sourceVersion);
        loadRecipe(recipe, false);
        m_source->setCurrentIndex(m_source->findData(sourceVersion));
        return;
    }
}

void RecipeEditorDialog::setRecipes(const QJsonArray &recipes)
{
    // 保留用户当前选择的复制来源，刷新不操作编辑区。
    const auto selected = m_source->currentData().toString();
    // 阻止来源列表重建时触发中间选择变化。
    const QSignalBlocker blocker(m_source);
    m_recipes = recipes;
    m_source->clear();
    for (const auto &value : recipes)
    {
        // 用名称和版本号区分同名的已发布配方。
        const auto recipe = value.toObject();
        m_source->addItem(recipe["name"].toString() + " · " + recipe["version"].toString(),
                          recipe["version"].toString());
    }
    // 列表来自已就绪的数据库，也兼容在后台启动后才创建编辑窗口。
    const auto index = m_source->findData(selected);
    if (index >= 0)
        m_source->setCurrentIndex(index);
    m_backendReady = true;
    updateActions();
}

void RecipeEditorDialog::resetDraft()
{
    if (m_busy)
        return;
    // 为手动新建提供一个可修改的项目，名称和版本由用户填写。
    const QJsonObject recipe{
        {"name", ""},
        {"version", ""},
        {"steps", QJsonArray{QJsonObject{{"id", "step_1"},
                                         {"label", QStringLiteral("测试项目1")},
                                         {"unit", ""},
                                         {"min", 0},
                                         {"max", 1},
                                         {"nominal", 0.5}}}}};
    loadRecipe(recipe, false);
}

void RecipeEditorDialog::loadRecipe(const QJsonObject &recipe, bool dirty)
{
    m_loading = true;
    m_name->setText(recipe["name"].toString());
    m_version->setText(recipe["version"].toString());
    // 按原配方顺序和双精度值填表，不把数值截成固定小数位。
    const auto steps = recipe["steps"].toArray();
    m_steps->setRowCount(steps.size());
    for (int row = 0; row < steps.size(); ++row)
    {
        // 当前行的完整规则对象。
        const auto step = steps[row].toObject();
        // 表格字段顺序与导入导出约定保持一致。
        const QStringList values{step["id"].toString(),
                                 step["label"].toString(),
                                 step["unit"].toString(),
                                 numberText(step["min"].toDouble()),
                                 numberText(step["max"].toDouble()),
                                 numberText(step["nominal"].toDouble())};
        for (int column = 0; column < values.size(); ++column)
            m_steps->setItem(row, column, new QTableWidgetItem(values[column]));
    }
    if (!steps.isEmpty())
        m_steps->selectRow(0);
    m_loading = false;
    m_dirty = dirty;
    m_initialized = true;
    showMessage({});
    updateActions();
}

QString RecipeEditorDialog::suggestedVersion(const QString &sourceVersion) const
{
    for (quint64 attempt = 1;; ++attempt)
    {
        // 将递增后缀保留在64字符的版本号限制内。
        const auto suffix = attempt == 1 ? QStringLiteral("-copy")
                                         : QStringLiteral("-copy") + QString::number(attempt);
        // 长来源版本只截断副本的前缀，原版本保持不变。
        const auto candidate = sourceVersion.left(64 - suffix.size()) + suffix;
        if (!versionExists(candidate))
            return candidate;
    }
}

bool RecipeEditorDialog::versionExists(const QString &version) const
{
    for (const auto &value : m_recipes)
        if (value.toObject()["version"].toString() == version)
            return true;
    return false;
}

void RecipeEditorDialog::copySelectedRecipe()
{
    if (m_busy || !confirmReplace())
        return;
    // 只复制用户明确选择的来源，不修改数据库原对象。
    const auto source = m_source->currentData().toString();
    for (const auto &value : m_recipes)
    {
        // 复制选中配方后建立一个未使用的版本号。
        auto recipe = value.toObject();
        if (recipe["version"].toString() == source)
        {
            recipe["version"] = suggestedVersion(source);
            loadRecipe(recipe, false);
            return;
        }
    }
}

void RecipeEditorDialog::addStep()
{
    if (m_busy || m_steps->rowCount() >= 16)
        return;
    // 已有项目编号用于生成不冲突的新编号。
    QSet<QString> ids;
    for (int row = 0; row < m_steps->rowCount(); ++row)
        if (auto *item = m_steps->item(row, 0))
            ids.insert(item->text().trimmed());
    // 从最小未使用序号生成新项目，删除行后也不会撞号。
    int number = 1;
    while (ids.contains(QString("step_%1").arg(number)))
        ++number;
    // 新增行位于执行顺序的末尾。
    const auto row = m_steps->rowCount();
    m_loading = true;
    m_steps->insertRow(row);
    // 提供可直接编辑的规则初值，不预设设备专用测量内容。
    const QStringList values{QString("step_%1").arg(number),
                             QStringLiteral("测试项目%1").arg(number),
                             "",
                             "0",
                             "1",
                             "0.5"};
    for (int column = 0; column < values.size(); ++column)
        m_steps->setItem(row, column, new QTableWidgetItem(values[column]));
    m_steps->selectRow(row);
    m_loading = false;
    markDirty();
}

void RecipeEditorDialog::removeStep()
{
    if (m_busy || m_steps->rowCount() <= 1 || m_steps->currentRow() < 0)
        return;
    // 删除后仍选择相邻的有效项目。
    const auto row = m_steps->currentRow();
    m_steps->removeRow(row);
    m_steps->selectRow(qMin(row, m_steps->rowCount() - 1));
    markDirty();
}

void RecipeEditorDialog::moveStep(int direction)
{
    // 保存移动前后的位置，边界处不改变内容。
    const auto from = m_steps->currentRow(), to = from + direction;
    if (m_busy || from < 0 || to < 0 || to >= m_steps->rowCount())
        return;
    m_loading = true;
    for (int column = 0; column < m_steps->columnCount(); ++column)
    {
        // 交换完整单元格，避免只移动标签而错配测量规则。
        auto *first = m_steps->takeItem(from, column);
        // 保留目标行原有字段。
        auto *second = m_steps->takeItem(to, column);
        m_steps->setItem(from, column, second ? second : new QTableWidgetItem);
        m_steps->setItem(to, column, first ? first : new QTableWidgetItem);
    }
    m_steps->selectRow(to);
    m_loading = false;
    markDirty();
}

bool RecipeEditorDialog::buildRecipe(QJsonObject *recipe, QString *error) const
{
    // 在读取表格前提交仍处于编辑状态的单元格。
    const auto focus = QApplication::focusWidget();
    if (focus && m_steps->isAncestorOf(focus))
        m_version->setFocus(Qt::OtherFocusReason);
    // 收集规则，不使用会截断精度的固定小数位控件。
    QJsonArray steps;
    for (int row = 0; row < m_steps->rowCount(); ++row)
    {
        // 读取当前行的文字字段，空单元格作为无效输入处理。
        const auto text = [this, row](int column)
        {
            // 当前字段可能被外部模型操作移除，需要安全读取。
            const auto item = m_steps->item(row, column);
            return item ? item->text().trimmed() : QString();
        };
        // 保存解析后的三项数值，索引对应下限、上限、正常参考值。
        double numbers[3]{};
        for (int column = 3; column < 6; ++column)
        {
            // 区分有效数字与空文本或不合法的数值。
            bool ok = false;
            numbers[column - 3] = text(column).toDouble(&ok);
            if (!ok)
            {
                if (error)
                    *error = QStringLiteral("第%1项的%2须填写有效数字")
                                 .arg(row + 1)
                                 .arg(m_steps->horizontalHeaderItem(column)->text());
                return false;
            }
        }
        steps.append(QJsonObject{{"id", text(0)},
                                 {"label", text(1)},
                                 {"unit", text(2)},
                                 {"min", numbers[0]},
                                 {"max", numbers[1]},
                                 {"nominal", numbers[2]}});
    }
    *recipe = {{"name", m_name->text().trimmed()},
               {"version", m_version->text().trimmed()},
               {"steps", steps}};
    return Domain::validateRecipe(*recipe, error);
}

bool RecipeEditorDialog::confirmReplace()
{
    if (m_busy)
        return false;
    if (!m_dirty)
        return true;
    return QMessageBox::question(this, QStringLiteral("替换配方草稿"),
                                 QStringLiteral("当前编辑内容尚未发布，是否放弃并替换？"),
                                 QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::No) == QMessageBox::Yes;
}

bool RecipeEditorDialog::confirmApplicationClose()
{
    if (!m_busy && !m_dirty)
        return true;
    show();
    raise();
    activateWindow();
    if (m_busy)
    {
        showMessage(QStringLiteral("配方正在发布，请等待结果后再退出工作台。"));
        return false;
    }
    return QMessageBox::question(
               this, QStringLiteral("退出工作台"),
               QStringLiteral("当前编辑的配方尚未发布，是否放弃草稿并退出工作台？"),
               QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
}

bool RecipeEditorDialog::importFile(const QString &path)
{
    if (m_busy)
        return false;
    // 输入仅限小型配方文本，拒绝过大文件而不读入全部内容。
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024)
    {
        showMessage(QStringLiteral("无法读取配方，或文件超过256 KiB。"), true);
        return false;
    }
    // 有界读取也防止文件在打开后继续增长。
    const auto bytes = file.read(256 * 1024 + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > 256 * 1024)
    {
        showMessage(QStringLiteral("配方读取失败，或文件超过256 KiB。"), true);
        return false;
    }
    // 保存JSON解析失败位置和原因，不显示输入文件的完整内容。
    QJsonParseError parse;
    // 解析完整输入文档，只有对象才可成为配方。
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    // 使用与数据库和测试引擎相同的校验规则。
    QString error;
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        error = QStringLiteral("必须是有效的JSON配方对象：") + parse.errorString();
    else if (!Domain::validateRecipe(document.object(), &error))
    {
    }
    if (!error.isEmpty())
    {
        showMessage(error, true);
        return false;
    }
    if (!confirmReplace())
        return false;
    loadRecipe(document.object(), true);
    showMessage(versionExists(document.object()["version"].toString())
                    ? QStringLiteral("已导入草稿，请更换版本号后发布。")
                    : QStringLiteral("已导入配方草稿，尚未发布。"));
    return true;
}

bool RecipeEditorDialog::exportFile(const QString &path)
{
    if (m_busy)
        return false;
    // 只导出已通过统一校验的完整配方。
    QJsonObject recipe;
    // 保存数值或字段校验的失败原因。
    QString error;
    if (!buildRecipe(&recipe, &error))
    {
        showMessage(error, true);
        return false;
    }
    // 先写临时文件，失败时不损坏原有JSON文件。
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    // 按UTF-8输出便于手工检查的标准JSON。
    const auto bytes = QJsonDocument(recipe).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    {
        showMessage(QStringLiteral("JSON导出失败：") + file.errorString(), true);
        return false;
    }
    showMessage(QStringLiteral("JSON已导出：") + path);
    return true;
}

void RecipeEditorDialog::publish()
{
    if (m_busy)
        return;
    if (!m_backendReady)
    {
        showMessage(QStringLiteral("后台尚未就绪，草稿仍可编辑和导出。"), true);
        return;
    }
    // 发布前再次汇总，避免导出后的修改被忽略。
    QJsonObject recipe;
    // 保存统一规则校验的错误原因。
    QString error;
    if (!buildRecipe(&recipe, &error))
    {
        showMessage(error, true);
        return;
    }
    if (versionExists(recipe["version"].toString()))
    {
        showMessage(QStringLiteral("该版本已经发布，请填写新的版本号。"), true);
        return;
    }
    m_pending = recipe;
    m_busy = true;
    updateActions();
    showMessage(QStringLiteral("正在发布配方……"));
    m_controller->publishRecipe(recipe);
}

void RecipeEditorDialog::markDirty()
{
    if (m_loading || m_busy)
        return;
    m_dirty = true;
    m_initialized = true;
    showMessage({});
    updateActions();
}

void RecipeEditorDialog::updateActions()
{
    m_name->setEnabled(!m_busy);
    m_version->setEnabled(!m_busy);
    m_steps->setEnabled(!m_busy);
    m_new->setEnabled(!m_busy);
    m_source->setEnabled(!m_busy && m_source->count() > 0);
    m_copy->setEnabled(!m_busy && m_source->currentIndex() >= 0);
    m_add->setEnabled(!m_busy && m_steps->rowCount() < 16);
    m_remove->setEnabled(!m_busy && m_steps->rowCount() > 1 && m_steps->currentRow() >= 0);
    m_up->setEnabled(!m_busy && m_steps->currentRow() > 0);
    m_down->setEnabled(!m_busy && m_steps->currentRow() >= 0 &&
                       m_steps->currentRow() + 1 < m_steps->rowCount());
    m_import->setEnabled(!m_busy);
    m_export->setEnabled(!m_busy);
    m_publish->setEnabled(!m_busy && m_backendReady);
}

void RecipeEditorDialog::showMessage(const QString &message, bool error)
{
    m_status->setText(message);
    m_status->setStyleSheet(error ? "QLabel { color:#a82432; }" : "QLabel { color:#333333; }");
    m_status->setVisible(!message.isEmpty());
}
