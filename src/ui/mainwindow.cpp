#include "mainwindow.h"
#include "models.h"
#include "ui/toolstyle.h"
#include "core/domain.h"
#include <QAbstractItemView>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QVBoxLayout>

namespace
{
QTableView *table(QWidget *parent, const char *name)
{
    auto *view = new QTableView(parent);
    view->setObjectName(name);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setAlternatingRowColors(true);
    view->setShowGrid(true);
    view->setWordWrap(false);
    view->verticalHeader()->hide();
    view->verticalHeader()->setDefaultSectionSize(28);
    view->horizontalHeader()->setStretchLastSection(true);
    view->horizontalHeader()->setMinimumSectionSize(50);
    return view;
}
QLabel *label(const QString &text, QWidget *parent = nullptr)
{
    auto *value = new QLabel(text, parent);
    value->setTextFormat(Qt::PlainText);
    return value;
}
} // namespace

MainWindow::MainWindow(const QString &dataDirectory, const QList<StationEndpoint> &endpoints,
                       QWidget *parent)
    : QMainWindow(parent), m_controller(new WorkbenchController(this)),
      m_stations(new StationModel(this)), m_steps(new StepModel(this)),
      m_historySteps(new StepModel(this)), m_history(new HistoryModel(this)),
      m_logs(new BoundedLogModel(1000, this)), m_historyProxy(new QSortFilterProxyModel(this))
{
    setWindowTitle(QStringLiteral("多工位功能测试与追溯"));
    resize(1280, 820);
    setMinimumSize(1080, 700);
    buildUi();
    for (const auto &ep : endpoints)
        m_stations->updateStation({{"stationId", ep.stationId},
                                   {"online", false},
                                   {"execution", QStringLiteral("后台初始化中")},
                                   {"verdict", "PENDING"},
                                   {"stored", false}});
    m_stationView->setFixedHeight(m_stationView->horizontalHeader()->sizeHint().height() +
                                  qBound(1, m_stations->rowCount(), 4) *
                                      m_stationView->verticalHeader()->defaultSectionSize() +
                                  4);
    if (m_stations->rowCount())
        m_stationView->selectRow(0);
    connect(m_controller, &WorkbenchController::stationUpdated, this,
            [this](const QJsonObject &snapshot)
            {
                m_stations->updateStation(snapshot);
                if (!m_stationView->currentIndex().isValid() && m_stations->rowCount())
                    m_stationView->selectRow(0);
                if (selectedStationId() == snapshot["stationId"].toInt())
                    showSelectedStation();
                int online = 0;
                for (int i = 0; i < m_stations->rowCount(); ++i)
                    if (m_stations->stationAt(i)["online"].toBool())
                        ++online;
                m_connectionSummary->setText(
                    QStringLiteral("%1 / %2 工位在线").arg(online).arg(m_stations->rowCount()));
            });
    connect(m_controller, &WorkbenchController::recipesReady, this,
            [this](const QJsonArray &recipes)
            {
                const QString selected = m_recipe->currentData().toString();
                m_recipes = recipes;
                m_recipe->blockSignals(true);
                m_recipe->clear();
                for (const auto &value : recipes)
                {
                    const auto r = value.toObject();
                    m_recipe->addItem(r["name"].toString() + " · " + r["version"].toString(),
                                      r["version"].toString());
                    m_recipe->setItemData(
                        m_recipe->count() - 1,
                        QStringLiteral("已发布版本只读。限值：\n") +
                            QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Indented)),
                        Qt::ToolTipRole);
                }
                const int old = m_recipe->findData(selected);
                if (old >= 0)
                    m_recipe->setCurrentIndex(old);
                m_recipe->blockSignals(false);
                showSelectedStation();
                syncRecipePreview();
            });
    connect(m_controller, &WorkbenchController::historyReady, this,
            [this](const QJsonArray &rows, bool more)
            {
                m_historyBusy = false;
                m_hasOlder = more;
                m_history->setRows(rows);
                m_query->setEnabled(m_backendReady);
                m_older->setEnabled(m_backendReady && more && m_history->lastId() > 0);
                m_historySummary->setText(
                    QStringLiteral("本页 %1 条 · 每页最多 100 条 · %2")
                        .arg(rows.size())
                        .arg(more ? QStringLiteral("可查看更早记录") : QStringLiteral("已到末页")));
                m_historySteps->setSteps({});
                m_historyDetail->setText(
                    QStringLiteral("选择一条记录查看绑定配方和各步骤实测值。"));
                if (!rows.isEmpty())
                    m_historyView->selectRow(0);
                else
                    showHistorySelection();
            });
    connect(m_controller, &WorkbenchController::reportReady, this,
            [this](const QString &path, bool ok, const QString &error)
            {
                m_reportBusy = false;
                showHistorySelection();
                const auto text = ok ? QStringLiteral("测试报告已导出：") + path
                                     : QStringLiteral("测试报告导出失败：") + error;
                m_logs->append(text, !ok);
                statusBar()->showMessage(text, 15000);
            });
    connect(m_controller, &WorkbenchController::logMessage, this,
            [this](const QString &text, bool warning)
            {
                const bool atBottom = m_logView->verticalScrollBar()->value() >=
                                      m_logView->verticalScrollBar()->maximum() - 2;
                m_logs->append(text, warning);
                if (atBottom)
                    m_logView->scrollToBottom();
                statusBar()->showMessage(text, warning ? 12000 : 5000);
                if (m_historyBusy && warning)
                {
                    m_historyBusy = false;
                    m_query->setEnabled(m_backendReady);
                    m_older->setEnabled(m_backendReady && m_hasOlder && m_history->lastId() > 0);
                }
            });
    connect(m_controller, &WorkbenchController::ready, this,
            [this](bool ok)
            {
                m_backendReady = ok;
                m_query->setEnabled(ok);
                m_import->setEnabled(ok);
                updateCommandAvailability();
                syncRecipePreview();
                showHistorySelection();
                if (!ok)
                    m_detail->setText(
                        QStringLiteral("后台初始化失败，请查看运行日志；已禁用测试操作。"));
            });
    m_controller->initialize(dataDirectory, endpoints);
    statusBar()->showMessage(QStringLiteral("数据目录：") + dataDirectory);
}

MainWindow::~MainWindow()
{
    // Worker notifications may still be queued when closing. Stop delivery to widgets first.
    disconnect(m_controller, nullptr, this, nullptr);
    m_controller->shutdown();
}

void MainWindow::buildUi()
{
    setStyleSheet(ToolStyle::stylesheet());
    auto *central = new QWidget(this);
    setCentralWidget(central);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(10, 8, 10, 6);
    root->setSpacing(8);
    auto *header = new QFrame(central);
    header->setObjectName("masthead");
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 2, 0, 2);
    auto *title = label(QStringLiteral("多工位功能测试与追溯"));
    title->setObjectName("appTitle");
    headerLayout->addWidget(title);
    headerLayout->addStretch();
    m_connectionSummary = label(QStringLiteral("等待后台就绪"));
    m_connectionSummary->setObjectName("connections");
    headerLayout->addWidget(m_connectionSummary);
    auto *badge = label(QStringLiteral("模拟数据"));
    badge->setToolTip(QStringLiteral("当前工位使用模拟器生成测试数值，未接入真实测量设备。"));
    badge->setObjectName("simBadge");
    headerLayout->addWidget(badge);
    root->addWidget(header);

    auto *board = new QGroupBox(QStringLiteral("工位状态"), central);
    auto *boardLayout = new QVBoxLayout(board);
    boardLayout->setContentsMargins(8, 8, 8, 6);
    m_stationView = table(board, "stationView");
    m_stationView->setModel(m_stations);
    m_stationView->setColumnHidden(7, true);
    m_stationView->setItemDelegateForColumn(5, new QualityDelegate(m_stationView));
    const QList<int> widths{64, 60, 150, 170, 140, 80, 80, 250};
    for (int c = 0; c < widths.size(); ++c)
        m_stationView->setColumnWidth(c, widths[c]);
    boardLayout->addWidget(m_stationView);
    root->addWidget(board);
    connect(m_stationView->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this]
            {
                showSelectedStation();
                syncRecipePreview();
            });

    auto *commands = new QFrame(central);
    commands->setObjectName("commandBar");
    auto *cmd = new QHBoxLayout(commands);
    cmd->setContentsMargins(0, 2, 0, 2);
    cmd->setSpacing(6);
    m_stationTitle = label(QStringLiteral("工位 —"));
    m_stationTitle->setObjectName("stationTitle");
    m_stationTitle->setMinimumWidth(64);
    cmd->addWidget(m_stationTitle);
    cmd->addWidget(label(QStringLiteral("产品 SN")));
    m_snInput = new QLineEdit(commands);
    m_snInput->setObjectName("snInput");
    m_snInput->setMaxLength(64);
    m_snInput->setMinimumWidth(140);
    m_snInput->setPlaceholderText(QStringLiteral("输入或扫码产品序列号"));
    m_snInput->setClearButtonEnabled(true);
    cmd->addWidget(m_snInput, 2);
    cmd->addWidget(label(QStringLiteral("配方")));
    m_recipe = new QComboBox(commands);
    m_recipe->setObjectName("recipeCombo");
    m_recipe->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_recipe->setMinimumContentsLength(18);
    m_recipe->setMinimumWidth(180);
    cmd->addWidget(m_recipe, 2);
    m_import = new QPushButton(QStringLiteral("发布配方"), commands);
    m_import->setEnabled(false);
    cmd->addWidget(m_import);
    m_start = new QPushButton(QStringLiteral("开始测试"), commands);
    m_start->setObjectName("startButton");
    m_start->setEnabled(false);
    cmd->addWidget(m_start);
    m_cancel = new QPushButton(QStringLiteral("请求取消"), commands);
    m_cancel->setObjectName("cancelButton");
    m_cancel->setEnabled(false);
    cmd->addWidget(m_cancel);
    root->addWidget(commands);
    connect(m_snInput, &QLineEdit::textChanged, this, &MainWindow::updateCommandAvailability);
    connect(m_snInput, &QLineEdit::returnPressed, this,
            [this]
            {
                if (m_start->isEnabled())
                    m_start->click();
            });
    connect(m_recipe, &QComboBox::currentIndexChanged, this,
            [this]
            {
                showSelectedStation();
                syncRecipePreview();
            });
    connect(m_start, &QPushButton::clicked, this,
            [this]
            {
                if (!m_start->isEnabled())
                    return;
                m_controller->startTest(selectedStationId(), m_snInput->text(),
                                        m_recipe->currentData().toString());
                // Admission is decided by the engine after durable reservation, not by this click.
                m_start->setEnabled(false);
            });
    connect(m_cancel, &QPushButton::clicked, this,
            [this]
            {
                if (m_cancel->isEnabled())
                    m_controller->cancelTest(selectedStationId());
            });
    connect(m_import, &QPushButton::clicked, this, &MainWindow::importRecipe);

    auto *content = new QSplitter(Qt::Horizontal, central);
    content->setChildrenCollapsible(false);
    auto *execution = new QGroupBox(QStringLiteral("当前测试"), content);
    auto *exec = new QVBoxLayout(execution);
    exec->setContentsMargins(8, 8, 8, 6);
    exec->setSpacing(6);
    m_detail = label(QStringLiteral("选择工位后显示测试详情。"));
    m_detail->setObjectName("detail");
    m_detail->setWordWrap(true);
    exec->addWidget(m_detail);
    m_runIdentity = label(QStringLiteral("尚无测试编号"));
    m_runIdentity->setObjectName("runIdentity");
    m_runIdentity->setWordWrap(true);
    m_runIdentity->setTextInteractionFlags(Qt::TextSelectableByMouse);
    exec->addWidget(m_runIdentity);
    m_stepView = table(execution, "stepView");
    m_stepView->setModel(m_steps);
    m_stepView->setItemDelegateForColumn(5, new QualityDelegate(m_stepView));
    m_stepView->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_stepView->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    exec->addWidget(m_stepView, 1);
    auto *hint = label(QStringLiteral("等待结果保存并确认后，再开始下一次测试。"));
    hint->setToolTip(QStringLiteral(
        "受理 ACK 仅表示任务已接收；测试完成后还需判定、数据库提交及工位归档确认。"));
    hint->setObjectName("hint");
    hint->setWordWrap(true);
    exec->addWidget(hint);
    content->addWidget(execution);

    auto *tabs = new QTabWidget(content);
    tabs->setObjectName("detailTabs");
    auto *historyPage = new QWidget(tabs);
    auto *historyLayout = new QVBoxLayout(historyPage);
    historyLayout->setContentsMargins(8, 8, 8, 8);
    auto *filterLayout = new QHBoxLayout;
    m_historyFilter = new QLineEdit(historyPage);
    m_historyFilter->setObjectName("historyFilter");
    m_historyFilter->setMaxLength(64);
    m_historyFilter->setPlaceholderText(QStringLiteral("产品 SN（留空查看全部）"));
    m_historyFilter->setClearButtonEnabled(true);
    filterLayout->addWidget(m_historyFilter, 1);
    m_query = new QPushButton(QStringLiteral("查询 / 最新"), historyPage);
    m_query->setObjectName("queryHistoryButton");
    m_query->setEnabled(false);
    filterLayout->addWidget(m_query);
    m_older = new QPushButton(QStringLiteral("更早记录"), historyPage);
    m_older->setObjectName("olderHistoryButton");
    m_older->setEnabled(false);
    filterLayout->addWidget(m_older);
    m_export = new QPushButton(QStringLiteral("导出本次报告"), historyPage);
    m_export->setObjectName("exportReportButton");
    m_export->setToolTip(
        QStringLiteral("将所选测试的身份、配方限值与步骤结果导出为 HTML；文件由后台线程写入。"));
    m_export->setEnabled(false);
    filterLayout->addWidget(m_export);
    historyLayout->addLayout(filterLayout);
    m_historySummary = label(QStringLiteral("输入产品 SN 查询，留空查看全部。"));
    m_historySummary->setToolTip(QStringLiteral("历史查询由数据库工作线程执行，每页最多100条。"));
    m_historySummary->setObjectName("hint");
    historyLayout->addWidget(m_historySummary);
    auto *historySplit = new QSplitter(Qt::Vertical, historyPage);
    historySplit->setChildrenCollapsible(false);
    m_historyView = table(historySplit, "historyView");
    m_historyProxy->setSourceModel(m_history);
    m_historyProxy->setSortRole(HistoryModel::SortRole);
    m_historyProxy->setDynamicSortFilter(false);
    m_historyView->setModel(m_historyProxy);
    m_historyView->setColumnHidden(6, true);
    m_historyView->setItemDelegateForColumn(5, new QualityDelegate(m_historyView));
    m_historyView->setSortingEnabled(true);
    m_historyView->sortByColumn(0, Qt::DescendingOrder);
    m_historyView->setColumnWidth(0, 160);
    m_historyView->setColumnWidth(1, 50);
    m_historyView->setColumnWidth(2, 160);
    m_historyView->setColumnWidth(3, 130);
    m_historyView->setColumnWidth(4, 95);
    m_historyView->setColumnWidth(5, 70);
    historySplit->addWidget(m_historyView);
    auto *historical = new QWidget(historySplit);
    auto *historicalLayout = new QVBoxLayout(historical);
    historicalLayout->setContentsMargins(0, 6, 0, 0);
    m_historyDetail = label(QStringLiteral("选择一条记录查看绑定配方和各步骤实测值。"));
    m_historyDetail->setObjectName("historyDetail");
    m_historyDetail->setWordWrap(true);
    m_historyDetail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    historicalLayout->addWidget(m_historyDetail);
    m_historyStepView = table(historical, "historyStepView");
    m_historyStepView->setModel(m_historySteps);
    m_historyStepView->setItemDelegateForColumn(5, new QualityDelegate(m_historyStepView));
    m_historyStepView->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_historyStepView->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    historicalLayout->addWidget(m_historyStepView);
    historySplit->addWidget(historical);
    historySplit->setSizes({250, 180});
    historyLayout->addWidget(historySplit, 1);
    tabs->addTab(historyPage, QStringLiteral("历史记录"));
    auto *logPage = new QWidget(tabs);
    auto *logLayout = new QVBoxLayout(logPage);
    auto *logHint = label(QStringLiteral("最近 1000 条运行记录"));
    logHint->setToolTip(
        QStringLiteral("完整文件日志保存在数据目录的 logs 文件夹，由独立线程写入。"));
    logHint->setObjectName("hint");
    logHint->setWordWrap(true);
    logLayout->addWidget(logHint);
    m_logView = new QListView(logPage);
    m_logView->setObjectName("logView");
    m_logView->setModel(m_logs);
    m_logView->setUniformItemSizes(false);
    m_logView->setWordWrap(true);
    m_logView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    logLayout->addWidget(m_logView, 1);
    tabs->addTab(logPage, QStringLiteral("运行日志"));
    content->addWidget(tabs);
    content->setStretchFactor(0, 2);
    content->setStretchFactor(1, 3);
    content->setSizes({470, 750});
    root->addWidget(content, 1);
    connect(m_historyView->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this] { showHistorySelection(); });
    connect(m_query, &QPushButton::clicked, this, [this] { queryHistory(false); });
    connect(m_older, &QPushButton::clicked, this, [this] { queryHistory(true); });
    connect(m_export, &QPushButton::clicked, this, &MainWindow::exportSelectedReport);
    connect(m_historyFilter, &QLineEdit::returnPressed, this,
            [this]
            {
                if (m_query->isEnabled())
                    queryHistory(false);
            });
}

int MainWindow::selectedStationId() const
{
    return m_stations->stationAt(m_stationView->currentIndex().row())["stationId"].toInt();
}
void MainWindow::showSelectedStation()
{
    const auto row = m_stations->stationAt(m_stationView->currentIndex().row());
    if (row.isEmpty())
    {
        updateCommandAvailability();
        return;
    }
    m_stationTitle->setText(
        QStringLiteral("工位 %1").arg(row["stationId"].toInt(), 2, 10, QChar('0')));
    m_detail->setText(row["execution"].toString() + "  ·  " + row["verdict"].toString("PENDING") +
                      "\n" + row["detail"].toString());
    m_runIdentity->setText(
        row["runId"].toString().isEmpty()
            ? QStringLiteral("配方预览（尚未测试）")
            : QStringLiteral("产品 SN：%1\n配方：%2  ·  记录：%3\n测试编号：%4")
                  .arg(row["sn"].toString(), row["recipeVersion"].toString(),
                       row["stored"].toBool() ? QStringLiteral("已保存") : QStringLiteral("未保存"),
                       row["runId"].toString()));
    m_runIdentity->setToolTip(
        row["runId"].toString().isEmpty()
            ? QStringLiteral("下表仅显示配方限值，尚无测试测量结果。")
            : QStringLiteral("本次测试编号：%1").arg(row["runId"].toString()));
    auto steps = row["stepResults"].toArray();
    if (row["runId"].toString().isEmpty())
    {
        for (const auto &v : m_recipes)
            if (v.toObject()["version"].toString() == m_recipe->currentData().toString())
            {
                for (const auto &s : v.toObject()["steps"].toArray())
                {
                    auto rule = s.toObject();
                    rule["stepId"] = rule.take("id");
                    steps.append(rule);
                }
                break;
            }
    }
    m_steps->setSteps(steps);
    updateCommandAvailability();
}
void MainWindow::syncRecipePreview()
{
    if (m_backendReady && !m_recipe->currentData().toString().isEmpty())
        m_controller->previewRecipe(selectedStationId(), m_recipe->currentData().toString());
}

void MainWindow::updateCommandAvailability()
{
    const auto row = m_stations->stationAt(m_stationView->currentIndex().row());
    m_start->setEnabled(m_backendReady && row["canStart"].toBool() &&
                        !m_snInput->text().trimmed().isEmpty() &&
                        !m_recipe->currentData().toString().isEmpty());
    m_cancel->setEnabled(m_backendReady && row["canCancel"].toBool());
}
void MainWindow::showHistorySelection()
{
    const auto row =
        m_history->recordAt(m_historyProxy->mapToSource(m_historyView->currentIndex()).row());
    m_export->setEnabled(m_backendReady && !m_reportBusy && !row["runId"].toString().isEmpty());
    if (row.isEmpty())
    {
        m_historySteps->setSteps({});
        return;
    }
    m_historySteps->setSteps(row["steps"].toArray());
    QString historyDetail = QStringLiteral("SN：%1  ·  配方：%2  ·  %3\n测试编号：%4")
                                .arg(row["sn"].toString(), row["recipeVersion"].toString(),
                                     row["verdict"].toString(), row["runId"].toString());
    if (!row["reason"].toString().isEmpty())
        historyDetail += "\n" + row["reason"].toString();
    m_historyDetail->setText(historyDetail);
    m_historyDetail->setToolTip(QStringLiteral("本次测试编号：%1").arg(row["runId"].toString()));
}
void MainWindow::queryHistory(bool older)
{
    if (!m_backendReady || m_historyBusy)
        return;
    int cursor = 0;
    if (older)
    {
        if (!m_hasOlder)
            return;
        cursor = m_history->lastId();
        if (cursor <= 0)
            return;
    }
    else
        m_queryFilter = m_historyFilter->text().trimmed();
    m_historyBusy = true;
    m_query->setEnabled(false);
    m_older->setEnabled(false);
    m_controller->refreshHistory(m_queryFilter, cursor);
}
void MainWindow::importRecipe()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("发布一个新的只读配方版本"), {}, QStringLiteral("JSON 配方 (*.json)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024)
    {
        QMessageBox::warning(this, QStringLiteral("读取失败"),
                             QStringLiteral("无法读取配方，或文件超过 256 KiB。"));
        return;
    }
    QJsonParseError parse;
    const auto json = QJsonDocument::fromJson(file.readAll(), &parse);
    QString error;
    if (parse.error != QJsonParseError::NoError || !json.isObject())
        error = QStringLiteral("必须是有效的 JSON 对象：") + parse.errorString();
    else if (!Domain::validateRecipe(json.object(), &error))
    {
    }
    if (!error.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("配方未发布"), error);
        return;
    }
    m_controller->publishRecipe(json.object());
}

void MainWindow::exportSelectedReport()
{
    if (!m_export->isEnabled() || m_reportBusy)
        return;
    // Capture the selected identity before the path dialog; incoming rows may refresh meanwhile.
    const auto row =
        m_history->recordAt(m_historyProxy->mapToSource(m_historyView->currentIndex()).row());
    const auto runId = row["runId"].toString();
    if (runId.isEmpty())
        return;
    QFileDialog dialog(this, QStringLiteral("导出所选测试报告"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilter(QStringLiteral("HTML 测试报告 (*.html *.htm)"));
    dialog.setDefaultSuffix("html");
    dialog.selectFile(QStringLiteral("测试报告-%1.html").arg(runId));
    // Let the dialog resolve the suffix before its normal overwrite confirmation.
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
        return;
    const auto path = dialog.selectedFiles().first();
    m_reportBusy = true;
    m_export->setEnabled(false);
    m_controller->exportReport(runId, path);
}
