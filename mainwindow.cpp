#include "mainwindow.h"
#include "historydialog.h"
#include "logdialog.h"
#include "maintenancedialog.h"
#include "stationconfigurationdialog.h"
#include "runtimeconfiguration.h"
#include "workbenchcontroller.h"
#include "models.h"
#include "toolstyle.h"
#include "domain.h"
#include <QAbstractItemView>
#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QStatusBar>
#include <QTableView>
#include <QToolBar>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
// 创建统一样式的只读表格；parent 为所属控件，name 为控件名称。
QTableView *table(QWidget *parent, const char *name)
{
    // view 为刚创建并待设置显示方式的表格。
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
// 创建纯文本标签；text 为显示文字，parent 为所属控件。
QLabel *label(const QString &text, QWidget *parent = nullptr)
{
    // value 为待显示纯文本的标签。
    auto *value = new QLabel(text, parent);
    value->setTextFormat(Qt::PlainText);
    return value;
}
// 打开隐藏复用的业务窗口，不改变共享后台的运行状态。
void openWindow(QDialog *window)
{
    window->show();
    window->raise();
    window->activateWindow();
}
} // namespace

MainWindow::MainWindow(const QString &dataDirectory, const QList<StationEndpoint> &endpoints,
                       QWidget *parent)
    : QMainWindow(parent), m_controller(new WorkbenchController(this)),
      m_stations(new StationModel(this)), m_steps(new StepModel(this)),
      m_historyDialog(new HistoryDialog(m_controller, this)),
      m_logDialog(new LogDialog(m_controller, this))
{
    // 保存连接配置读取失败的原因，损坏时暂停接入并打开修复窗口。
    QString connectionError;
    // 已保存的连接优先于启动脚本提供的初始地址。
    const auto configuredEndpoints =
        RuntimeConfiguration::loadStationEndpoints(dataDirectory, endpoints, &connectionError);
    m_stationConfigurationDialog = new StationConfigurationDialog(
        dataDirectory, connectionError.isEmpty() ? configuredEndpoints : endpoints, this);
    setWindowTitle(QStringLiteral("多工位功能测试与追溯"));
    resize(1080, 740);
    setMinimumSize(860, 600);
    // 按当前屏幕限制初始尺寸，子窗口不再挤占测试区。
    if (auto *screen = QGuiApplication::primaryScreen())
    {
        setMinimumSize(qMin(860, screen->availableGeometry().width() - 40),
                       qMin(600, screen->availableGeometry().height() - 60));
        resize(qMin(1080, screen->availableGeometry().width() - 40),
               qMin(740, screen->availableGeometry().height() - 60));
    }
    buildUi();
    buildNavigation();
    for (const auto &ep : configuredEndpoints)
        m_stations->updateStation({{"stationId", ep.stationId},
                                   {"online", false},
                                   {"execution", QStringLiteral("初始化中")},
                                   {"verdict", "PENDING"},
                                   {"stored", false}});
    m_stationView->setFixedHeight(m_stationView->horizontalHeader()->sizeHint().height() +
                                  qBound(1, m_stations->rowCount(), 4) *
                                      m_stationView->verticalHeader()->defaultSectionSize() +
                                  4);
    if (m_stations->rowCount())
        m_stationView->selectRow(0);
    connect(m_controller, &WorkbenchController::stationUpdated, this,
            // 收到工位状态后刷新界面；snapshot 为该工位的最新状态快照。
            [this](const QJsonObject &snapshot)
            {
                m_stations->updateStation(snapshot);
                if (!m_stationView->currentIndex().isValid() && m_stations->rowCount())
                    m_stationView->selectRow(0);
                if (selectedStationId() == snapshot["stationId"].toInt())
                    showSelectedStation();
                // online 累计当前在线的工位数量。
                int online = 0;
                for (int i = 0; i < m_stations->rowCount(); ++i)
                    if (m_stations->stationAt(i)["online"].toBool())
                        ++online;
                m_connectionSummary->setText(
                    QStringLiteral("%1 / %2 工位在线").arg(online).arg(m_stations->rowCount()));
            });
    connect(m_controller, &WorkbenchController::recipesReady, this,
            // 刷新配方下拉框；recipes 为后台返回的已发布配方列表。
            [this](const QJsonArray &recipes)
            {
                // selected 保存刷新列表前选中的配方版本。
                const QString selected = m_recipe->currentData().toString();
                m_recipes = recipes;
                m_recipe->blockSignals(true);
                m_recipe->clear();
                for (const auto &value : recipes)
                {
                    // r 保存当前配方的名称、版本及步骤内容。
                    const auto r = value.toObject();
                    m_recipe->addItem(r["name"].toString() + " · " + r["version"].toString(),
                                      r["version"].toString());
                }
                // old 为原先选中版本在新列表中的位置。
                const int old = m_recipe->findData(selected);
                if (old >= 0)
                    m_recipe->setCurrentIndex(old);
                m_recipe->blockSignals(false);
                showSelectedStation();
                syncRecipePreview();
            });
    connect(m_controller, &WorkbenchController::logMessage, this,
            [this](const QString &text, bool warning)
            {
                if (warning)
                    statusBar()->showMessage(text.section('\n', 0, 0).left(120), 12000);
            });
    connect(m_controller, &WorkbenchController::ready, this,
            [this](bool ok)
            {
                m_backendReady = ok;
                m_import->setEnabled(ok);
                updateCommandAvailability();
                syncRecipePreview();
                statusBar()->showMessage(ok ? QStringLiteral("就绪")
                                            : QStringLiteral("后台不可用"));
                if (!ok)
                    m_detail->setText(QStringLiteral("后台不可用"));
            });
    m_controller->initialize(dataDirectory, configuredEndpoints);
    if (!connectionError.isEmpty())
    {
        m_stationConfigurationDialog->showError(connectionError);
        QTimer::singleShot(0, this, [this] { openWindow(m_stationConfigurationDialog); });
    }
}

MainWindow::~MainWindow()
{
    disconnect(m_controller, nullptr, this, nullptr);
    disconnect(m_controller, nullptr, m_historyDialog, nullptr);
    disconnect(m_controller, nullptr, m_logDialog, nullptr);
    if (m_maintenanceDialog)
        disconnect(m_controller, nullptr, m_maintenanceDialog, nullptr);
    m_controller->shutdown();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_historyDialog->close();
    m_logDialog->close();
    m_stationConfigurationDialog->close();
    if (m_maintenanceDialog)
        m_maintenanceDialog->close();
    QMainWindow::closeEvent(event);
    if (event->isAccepted())
        m_controller->shutdown();
}

void MainWindow::buildNavigation()
{
    // 将业务窗口入口放到独立工具栏，主窗口只显示当前测试。
    auto *navigation = addToolBar(QStringLiteral("窗口"));
    navigation->setObjectName("windowNavigation");
    navigation->setMovable(false);
    navigation->setFloatable(false);
    // 复用同一个历史窗口，保留当前查询与分页。
    auto *history = new QAction(QStringLiteral("历史追溯"), this);
    history->setObjectName("openHistoryAction");
    // 复用日志窗口及其持续更新的缓存。
    auto *logs = new QAction(QStringLiteral("运行日志"), this);
    logs->setObjectName("openLogAction");
    // 单独配置各工位的IP和端口，不占用当前测试区域。
    auto *connections = new QAction(QStringLiteral("工位配置"), this);
    connections->setObjectName("openStationConfigurationAction");
    // 管理员维护与正常测试操作各自独立。
    auto *maintenance = new QAction(QStringLiteral("管理员维护"), this);
    maintenance->setObjectName("openMaintenanceAction");
    navigation->addAction(history);
    navigation->addAction(logs);
    navigation->addSeparator();
    navigation->addAction(connections);
    navigation->addAction(maintenance);
    connect(history, &QAction::triggered, this, [this] { openWindow(m_historyDialog); });
    connect(logs, &QAction::triggered, this, [this] { openWindow(m_logDialog); });
    connect(connections, &QAction::triggered, this,
            [this] { openWindow(m_stationConfigurationDialog); });
    connect(maintenance, &QAction::triggered, this,
            [this]
            {
                if (!m_maintenanceDialog)
                    m_maintenanceDialog = new MaintenanceDialog(m_controller, this);
                openWindow(m_maintenanceDialog);
            });
}

void MainWindow::buildUi()
{
    // Resolve all roles locally so a later system palette change cannot recolor text alone.
    setPalette(ToolStyle::lightPalette());
    setStyleSheet(ToolStyle::stylesheet() +
                  QStringLiteral("QDateEdit { min-height:23px; color:#222222; background:white; }"
                                 "QDateEdit:disabled { color:#666666; background:#eeeeee; }"));
    // central 为容纳全部界面区域的中心控件。
    auto *central = new QWidget(this);
    setCentralWidget(central);
    // root 为主窗口各区域的纵向布局。
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(10, 8, 10, 6);
    root->setSpacing(8);
    // header 为标题、在线工位数和模拟标记所在区域。
    auto *header = new QFrame(central);
    header->setObjectName("masthead");
    // headerLayout 横向排列窗口顶部信息。
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 2, 0, 2);
    // title 为窗口顶部的工作台名称标签。
    auto *title = label(QStringLiteral("多工位功能测试与追溯"));
    title->setObjectName("appTitle");
    headerLayout->addWidget(title);
    headerLayout->addStretch();
    m_connectionSummary = label(QStringLiteral("等待后台就绪"));
    m_connectionSummary->setObjectName("connections");
    headerLayout->addWidget(m_connectionSummary);
    // badge 提醒操作员当前使用模拟数据。
    auto *badge = label(QStringLiteral("模拟数据"));
    badge->setObjectName("simBadge");
    headerLayout->addWidget(badge);
    root->addWidget(header);

    // board 为工位状态表所在的分组框。
    auto *board = new QGroupBox(QStringLiteral("工位状态"), central);
    // boardLayout 安排工位状态表的位置。
    auto *boardLayout = new QVBoxLayout(board);
    boardLayout->setContentsMargins(8, 8, 8, 6);
    m_stationView = table(board, "stationView");
    m_stationView->setModel(m_stations);
    m_stationView->setColumnHidden(7, true);
    m_stationView->setItemDelegateForColumn(5, new QualityDelegate(m_stationView));
    // widths 保存工位状态表各列的初始宽度。
    const QList<int> widths{64, 60, 150, 170, 140, 80, 80, 250};
    for (int c = 0; c < widths.size(); ++c)
        m_stationView->setColumnWidth(c, widths[c]);
    boardLayout->addWidget(m_stationView);
    root->addWidget(board);
    connect(m_stationView->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            // 切换所选工位时刷新测试详情和配方预览。
            [this]
            {
                showSelectedStation();
                syncRecipePreview();
            });

    // 分行显示产品、配方及测试动作，避免横向拥挤。
    auto *commands = new QFrame(central);
    commands->setObjectName("commandBar");
    // 对齐每行输入框及操作按钮。
    auto *grid = new QGridLayout(commands);
    grid->setContentsMargins(0, 2, 0, 2);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(8);
    m_stationTitle = label(QStringLiteral("工位 —"), commands);
    m_stationTitle->setObjectName("stationTitle");
    m_stationTitle->setMinimumWidth(70);
    grid->addWidget(m_stationTitle, 0, 0);
    grid->addWidget(label(QStringLiteral("产品 SN"), commands), 0, 1);
    m_snInput = new QLineEdit(commands);
    m_snInput->setObjectName("snInput");
    m_snInput->setMaxLength(64);
    m_snInput->setPlaceholderText(QStringLiteral("输入或扫码产品序列号"));
    m_snInput->setClearButtonEnabled(true);
    grid->addWidget(m_snInput, 0, 2, 1, 3);
    grid->addWidget(label(QStringLiteral("配方"), commands), 1, 1);
    m_recipe = new QComboBox(commands);
    m_recipe->setObjectName("recipeCombo");
    m_recipe->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_recipe->setMinimumContentsLength(18);
    grid->addWidget(m_recipe, 1, 2, 1, 2);
    m_import = new QPushButton(QStringLiteral("发布配方"), commands);
    m_import->setEnabled(false);
    grid->addWidget(m_import, 1, 4);
    // 将三个测试动作作为同一操作组。
    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    m_start = new QPushButton(QStringLiteral("开始测试"), commands);
    m_start->setObjectName("startButton");
    m_start->setEnabled(false);
    actions->addWidget(m_start);
    m_cancel = new QPushButton(QStringLiteral("请求取消"), commands);
    m_cancel->setObjectName("cancelButton");
    m_cancel->setEnabled(false);
    actions->addWidget(m_cancel);
    m_reconcile = new QPushButton(QStringLiteral("重新核对"), commands);
    m_reconcile->setObjectName("reconcileButton");
    m_reconcile->setEnabled(false);
    actions->addWidget(m_reconcile);
    actions->addStretch();
    grid->addLayout(actions, 2, 2, 1, 3);
    grid->setColumnStretch(2, 1);
    grid->setColumnStretch(3, 1);
    root->addWidget(commands);
    connect(m_snInput, &QLineEdit::textChanged, this, &MainWindow::updateCommandAvailability);
    connect(m_snInput, &QLineEdit::returnPressed, this,
            // 扫码回车时，在允许开始的情况下触发测试。
            [this]
            {
                if (m_start->isEnabled())
                    m_start->click();
            });
    connect(m_recipe, &QComboBox::currentIndexChanged, this,
            // 切换配方时刷新步骤预览并通知所选工位。
            [this]
            {
                showSelectedStation();
                syncRecipePreview();
            });
    connect(m_start, &QPushButton::clicked, this,
            // 点击开始后提交当前工位、SN 和配方，并等待后台准入。
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
            // 点击取消后向当前工位提交取消请求。
            [this]
            {
                if (m_cancel->isEnabled())
                    m_controller->cancelTest(selectedStationId());
            });
    connect(m_import, &QPushButton::clicked, this, &MainWindow::importRecipe);
    connect(m_reconcile, &QPushButton::clicked, this,
            // 点击重新核对后按原测试编号查询当前工位状态。
            [this]
            {
                if (m_reconcile->isEnabled())
                {
                    m_reconcile->setEnabled(false);
                    m_controller->retryReconciliation(selectedStationId());
                }
            });

    // execution 为左侧当前测试的分组框。
    auto *execution = new QGroupBox(QStringLiteral("当前测试"), central);
    // exec 纵向排列当前测试的身份、步骤和提示。
    auto *exec = new QVBoxLayout(execution);
    exec->setContentsMargins(8, 8, 8, 6);
    exec->setSpacing(6);
    m_detail = label(QStringLiteral("待连接"));
    m_detail->setObjectName("detail");
    m_detail->setWordWrap(true);
    exec->addWidget(m_detail);
    m_runIdentity = label(QStringLiteral("配方预览"));
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
    root->addWidget(execution, 1);
}

int MainWindow::selectedStationId() const
{
    return m_stations->stationAt(m_stationView->currentIndex().row())["stationId"].toInt();
}
void MainWindow::showSelectedStation()
{
    // row 为当前选中工位的状态和测试身份。
    const auto row = m_stations->stationAt(m_stationView->currentIndex().row());
    if (row.isEmpty())
    {
        updateCommandAvailability();
        return;
    }
    m_stationTitle->setText(
        QStringLiteral("工位 %1").arg(row["stationId"].toInt(), 2, 10, QChar('0')));
    m_detail->setText(row["execution"].toString() + "  ·  " + row["verdict"].toString("PENDING") +
                      (row["timedOut"].toBool() ? QStringLiteral("  ·  超时") : QString()));
    m_runIdentity->setText(
        row["runId"].toString().isEmpty()
            ? QStringLiteral("配方预览")
            : QStringLiteral("产品 SN：%1\n配方：%2  ·  记录：%3\n测试编号：%4")
                  .arg(row["sn"].toString(), row["recipeVersion"].toString(),
                       row["stored"].toBool() ? QStringLiteral("已保存") : QStringLiteral("未保存"),
                       row["runId"].toString()));
    // steps 保存当前工位实测步骤，尚未测试时填入配方预览。
    auto steps = row["stepResults"].toArray();
    if (row["runId"].toString().isEmpty())
    {
        for (const auto &v : m_recipes)
            if (v.toObject()["version"].toString() == m_recipe->currentData().toString())
            {
                for (const auto &s : v.toObject()["steps"].toArray())
                {
                    // rule 保存当前预览步骤，供转换步骤标识字段。
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
    // row 提供所选工位允许开始、取消和核对的状态。
    const auto row = m_stations->stationAt(m_stationView->currentIndex().row());
    m_start->setEnabled(m_backendReady && row["canStart"].toBool() &&
                        !m_snInput->text().trimmed().isEmpty() &&
                        !m_recipe->currentData().toString().isEmpty());
    m_cancel->setEnabled(m_backendReady && row["canCancel"].toBool());
    m_reconcile->setEnabled(m_backendReady && row["canReconcile"].toBool());
}
void MainWindow::importRecipe()
{
    // path 为操作员选择的待发布配方 JSON 文件路径。
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("发布一个新的只读配方版本"), {}, QStringLiteral("JSON 配方 (*.json)"));
    if (path.isEmpty())
        return;
    // file 负责读取选中的配方文件。
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024)
    {
        QMessageBox::warning(this, QStringLiteral("读取失败"),
                             QStringLiteral("无法读取配方，或文件超过 256 KiB。"));
        return;
    }
    // parse 保存配方 JSON 解析时的错误位置和原因。
    QJsonParseError parse;
    // json 保存解析后的配方文档。
    const auto json = QJsonDocument::fromJson(file.readAll(), &parse);
    // error 保存解析或配方规则校验的失败原因。
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
