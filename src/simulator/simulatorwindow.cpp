#include "simulatorwindow.h"
#include "simulatorcontroller.h"
#include "core/domain.h"
#include "ui/toolstyle.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace
{
QLabel *plainLabel(const QString &text, QWidget *parent = nullptr)
{
    auto *value = new QLabel(text, parent);
    value->setTextFormat(Qt::PlainText);
    return value;
}
QString stateName(const QJsonObject &snapshot)
{
    const auto state = snapshot["state"].toString();
    if (!snapshot["listening"].toBool())
        return QObject::tr("已停止");
    if (state == "executing")
        return QObject::tr("正在测试");
    if (state == "terminal")
        return QObject::tr("等待归档确认");
    if (state == "acked")
        return QObject::tr("本次测试已确认");
    if (state == "interrupted")
        return QObject::tr("中断 / 待对账");
    return QObject::tr("等待测试");
}
QString number(double value)
{
    return QString::number(value, 'g', 8);
}
void prepareTable(QTableWidget *table, const QStringList &headers)
{
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->setShowGrid(true);
    table->setWordWrap(false);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(28);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setMinimumSectionSize(42);
}
void cell(QTableWidget *table, int row, int column, const QString &text)
{
    auto *item = table->item(row, column);
    if (!item)
    {
        item = new QTableWidgetItem;
        table->setItem(row, column, item);
    }
    item->setText(text);
    item->setToolTip(text);
}
} // namespace

SimulatorWindow::SimulatorWindow(const QList<StationOptions> &options, bool autoStart,
                                 QWidget *parent)
    : QMainWindow(parent), m_controller(new SimulatorController(this))
{
    for (const auto &option : options)
    {
        if (option.stationId >= 1 && option.stationId <= 4 && !m_options.contains(option.stationId))
            m_options.insert(option.stationId, option);
    }
    setWindowTitle(tr("工位设备模拟器"));
    resize(1280, 800);
    setMinimumSize(1080, 720);
    buildUi();
    connect(m_controller, &SimulatorController::stationChanged, this,
            &SimulatorWindow::receiveSnapshot);
    connect(m_controller, &SimulatorController::diagnostic, this,
            [this](int id, const QString &message) { appendLog(id, message); });
    connect(m_controller, &SimulatorController::operationFinished, this,
            &SimulatorWindow::finishOperation);
    refreshTable();
    if (m_stationTable->rowCount())
        m_stationTable->setCurrentCell(0, 0);
    selectStation();
    appendLog(0, tr("准备就绪。先启动模拟工位，再在主程序选择工位、输入产品 SN 并开始测试。"));
    if (autoStart)
        QTimer::singleShot(0, this, &SimulatorWindow::startAll);
}

SimulatorController *SimulatorWindow::controller() const
{
    return m_controller;
}
QJsonObject SimulatorWindow::stationSnapshot(int stationId) const
{
    return m_snapshots.value(stationId);
}

void SimulatorWindow::buildUi()
{
    setStyleSheet(ToolStyle::stylesheet());
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(10, 8, 10, 6);
    root->setSpacing(6);
    auto *header = new QFrame(central);
    header->setObjectName("masthead");
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 2);
    auto *title = plainLabel(tr("工位设备模拟器"));
    title->setObjectName("appTitle");
    headerLayout->addWidget(title);
    headerLayout->addStretch();
    m_connections = plainLabel({});
    m_connections->setObjectName("connectionSummaryLabel");
    headerLayout->addWidget(m_connections);
    auto *badge = plainLabel(tr("模拟数据"));
    badge->setObjectName("simBadge");
    badge->setToolTip(tr("所有数值由程序生成，未连接真实检测仪器。"));
    headerLayout->addWidget(badge);
    root->addWidget(header);

    auto *guide = new QFrame(central);
    guide->setObjectName("guide");
    auto *guideLayout = new QHBoxLayout(guide);
    guideLayout->setContentsMargins(0, 2, 0, 2);
    auto *instruction = plainLabel(tr("启动工位后，在主程序输入产品 SN 并开始测试。"));
    instruction->setObjectName("guideLabel");
    instruction->setWordWrap(true);
    guideLayout->addWidget(instruction, 1);
    m_startAll = new QPushButton(tr("启动全部"), guide);
    m_startAll->setObjectName("startAllButton");
    m_stopAll = new QPushButton(tr("停止全部"), guide);
    m_stopAll->setObjectName("stopAllButton");
    guideLayout->addWidget(m_startAll);
    guideLayout->addWidget(m_stopAll);
    connect(m_startAll, &QPushButton::clicked, this, &SimulatorWindow::startAll);
    connect(m_stopAll, &QPushButton::clicked, this, &SimulatorWindow::stopAll);
    root->addWidget(guide);

    auto *body = new QSplitter(Qt::Horizontal, central);
    body->setChildrenCollapsible(false);
    auto *leftScroll = new QScrollArea(body);
    leftScroll->setWidgetResizable(true);
    leftScroll->setFrameShape(QFrame::NoFrame);
    leftScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *left = new QWidget;
    left->setMinimumWidth(350);
    leftScroll->setMinimumWidth(368);
    leftScroll->setWidget(left);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(6);
    auto *stations = new QGroupBox(tr("模拟工位"), left);
    auto *stationsLayout = new QVBoxLayout(stations);
    stationsLayout->setContentsMargins(8, 10, 8, 8);
    m_stationTable = new QTableWidget(stations);
    m_stationTable->setObjectName("stationTable");
    prepareTable(m_stationTable, {tr("工位"), tr("端口"), tr("监听"), tr("客户端"), tr("任务")});
    m_stationTable->setRowCount(m_options.size());
    m_stationTable->setColumnWidth(0, 44);
    m_stationTable->setColumnWidth(1, 60);
    m_stationTable->setColumnWidth(2, 54);
    m_stationTable->setColumnWidth(3, 66);
    m_stationTable->verticalHeader()->setDefaultSectionSize(28);
    m_stationTable->setFixedHeight(32 + 28 * m_options.size());
    stationsLayout->addWidget(m_stationTable);
    m_stationTable->setToolTip(tr("选择工位，查看任务并修改该工位的模拟设置。"));
    leftLayout->addWidget(stations);
    connect(m_stationTable, &QTableWidget::currentCellChanged, this, [this] { selectStation(); });

    auto *settings = new QGroupBox(tr("工位设置"), left);
    auto *settingsLayout = new QVBoxLayout(settings);
    settingsLayout->setContentsMargins(8, 10, 8, 8);
    settingsLayout->setSpacing(6);
    m_settingsTitle = plainLabel({});
    m_settingsTitle->setObjectName("settingsTitle");
    settingsLayout->addWidget(m_settingsTitle);
    auto *form = new QFormLayout;
    form->setSpacing(6);
    m_port = new QSpinBox(settings);
    m_port->setObjectName("portSpin");
    m_port->setRange(0, 65535);
    m_port->setSpecialValueText(tr("自动分配"));
    m_port->setToolTip(tr("只监听本机 127.0.0.1。修改端口需先停止工位；0 用于自动分配空闲端口。"));
    form->addRow(tr("监听端口"), m_port);
    auto *directoryRow = new QWidget(settings);
    auto *directoryLayout = new QHBoxLayout(directoryRow);
    directoryLayout->setContentsMargins(0, 0, 0, 0);
    directoryLayout->setSpacing(5);
    m_directory = new QLineEdit(directoryRow);
    m_directory->setObjectName("dataDirectoryEdit");
    m_directory->setToolTip(tr("保存该工位的任务和待确认结果；每个工位应使用独立目录。"));
    directoryLayout->addWidget(m_directory);
    auto *browse = new QToolButton(directoryRow);
    browse->setText("…");
    browse->setObjectName("browseDirectoryButton");
    directoryLayout->addWidget(browse);
    form->addRow(tr("状态目录"), directoryRow);
    connect(browse, &QToolButton::clicked, this,
            [this]
            {
                const auto path = QFileDialog::getExistingDirectory(this, tr("选择工位状态目录"),
                                                                    m_directory->text());
                if (!path.isEmpty())
                    m_directory->setText(path);
            });
    m_stepMs = new QSpinBox(settings);
    m_stepMs->setObjectName("stepMsSpin");
    m_stepMs->setRange(1, 60000);
    m_stepMs->setSuffix(" ms");
    m_stepMs->setToolTip(tr("模拟每个测试步骤的等待时间。例如 1000 ms 表示每一步耗时约 1 秒。"));
    form->addRow(tr("每步耗时"), m_stepMs);
    m_failStep = new QComboBox(settings);
    m_failStep->setObjectName("failStepCombo");
    m_failStep->addItem(tr("正常"), "");
    m_failStep->setToolTip(tr("使选定步骤返回大于配方上限的值，用于验证主程序显示 FAIL。"));
    form->addRow(tr("异常数值"), m_failStep);
    m_faultRecipeHint = plainLabel({});
    m_faultRecipeHint->setObjectName("faultRecipeHint");
    m_faultRecipeHint->setWordWrap(true);
    form->addRow(m_faultRecipeHint);
    settingsLayout->addLayout(form);
    m_dropAck = new QCheckBox(tr("丢弃首次归档确认"), settings);
    m_dropAck->setObjectName("dropAckCheck");
    m_dropAck->setToolTip(tr("测试结果仍会发送；故意忽略第一次归档 ACK，验证主程序的重试与去重。"));
    settingsLayout->addWidget(m_dropAck);
    auto *settingButtons = new QHBoxLayout;
    m_start = new QPushButton(tr("启动工位"), settings);
    m_start->setObjectName("startStationButton");
    m_stop = new QPushButton(tr("停止工位"), settings);
    m_stop->setObjectName("stopStationButton");
    m_apply = new QPushButton(tr("应用设置"), settings);
    m_apply->setObjectName("applyFaultButton");
    settingButtons->addWidget(m_start);
    settingButtons->addWidget(m_stop);
    settingButtons->addWidget(m_apply);
    settingsLayout->addLayout(settingButtons);
    m_disconnect = new QPushButton(tr("模拟断线"), settings);
    m_disconnect->setObjectName("disconnectButton");
    m_disconnect->setToolTip(
        tr("断开当前 TCP 连接，模拟任务继续执行；主程序重连后可查询任务结果。"));
    settingsLayout->addWidget(m_disconnect);
    auto *note = plainLabel(tr("断线时继续测试；停止工位后保留状态。"));
    note->setToolTip(tr("请在下一次测试前应用设置。断线仅关闭TCP连接；停止工位将停止执行，"
                        "执行中重新启动会报告中断并等待主程序对账。"));
    note->setObjectName("note");
    note->setWordWrap(true);
    settingsLayout->addWidget(note);
    settingsLayout->addStretch();
    leftLayout->addWidget(settings, 1);
    connect(m_start, &QPushButton::clicked, this, &SimulatorWindow::startSelected);
    connect(m_stop, &QPushButton::clicked, this, &SimulatorWindow::stopSelected);
    connect(m_apply, &QPushButton::clicked, this, &SimulatorWindow::applySelected);
    connect(m_disconnect, &QPushButton::clicked, this,
            [this]
            {
                const int id = selectedStationId();
                if (id < 0 || m_pending.contains(id))
                    return;
                m_pending.insert(id);
                refreshAvailability();
                m_controller->disconnectStation(id);
            });

    auto *right = new QSplitter(Qt::Vertical, body);
    right->setChildrenCollapsible(false);
    auto *task = new QGroupBox(tr("当前任务"), right);
    auto *taskLayout = new QVBoxLayout(task);
    taskLayout->setContentsMargins(8, 10, 8, 8);
    taskLayout->setSpacing(6);
    m_state = plainLabel({});
    m_state->setObjectName("stateLabel");
    m_state->setWordWrap(true);
    taskLayout->addWidget(m_state);
    m_runSummary = plainLabel({});
    m_runSummary->setObjectName("runSummaryLabel");
    m_runSummary->setWordWrap(true);
    m_runSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    taskLayout->addWidget(m_runSummary);
    m_stepTable = new QTableWidget(task);
    m_stepTable->setObjectName("stepTable");
    prepareTable(m_stepTable, {tr("序号"), tr("测试项目"), tr("允许范围"), tr("模拟返回值"),
                               tr("单位"), tr("范围检查")});
    m_stepTable->setColumnWidth(0, 42);
    m_stepTable->setColumnWidth(1, 112);
    m_stepTable->setColumnWidth(2, 112);
    m_stepTable->setColumnWidth(3, 98);
    m_stepTable->setColumnWidth(4, 48);
    taskLayout->addWidget(m_stepTable, 1);
    auto *taskNote = plainLabel(tr("最终判定与归档状态以主程序为准。"));
    taskNote->setToolTip(tr("这里仅检查模拟返回值是否在配方范围内。产品的最终PASS/FAIL、"
                            "结果入库和归档确认由主程序处理。"));
    taskNote->setObjectName("note");
    taskNote->setWordWrap(true);
    taskLayout->addWidget(taskNote);
    right->addWidget(task);
    auto *logGroup = new QGroupBox(tr("通信日志"), right);
    auto *logLayout = new QVBoxLayout(logGroup);
    logLayout->setContentsMargins(8, 10, 8, 8);
    logLayout->setSpacing(6);
    auto *logControls = new QHBoxLayout;
    auto *logNote = plainLabel(tr("全部工位，最近 500 条"));
    logNote->setObjectName("note");
    logControls->addWidget(logNote, 1);
    auto *clearLog = new QPushButton(tr("清空显示"), logGroup);
    clearLog->setObjectName("clearLogButton");
    logControls->addWidget(clearLog);
    logLayout->addLayout(logControls);
    m_logs = new QPlainTextEdit(logGroup);
    m_logs->setObjectName("simulatorLog");
    m_logs->setReadOnly(true);
    m_logs->document()->setMaximumBlockCount(500);
    logLayout->addWidget(m_logs);
    connect(clearLog, &QPushButton::clicked, m_logs, &QPlainTextEdit::clear);
    right->addWidget(logGroup);
    right->setSizes({340, 260});
    body->addWidget(leftScroll);
    body->addWidget(right);
    body->setSizes({378, 882});
    root->addWidget(body, 1);
    setCentralWidget(central);
    statusBar()->showMessage(tr("模拟器等待主程序下发任务。关闭窗口会停止本窗口启动的工位。"));
}

int SimulatorWindow::selectedStationId() const
{
    const auto *item = m_stationTable->item(m_stationTable->currentRow(), 0);
    return item ? item->data(Qt::UserRole).toInt() : -1;
}

StationOptions SimulatorWindow::editedOptions() const
{
    auto options = m_options.value(selectedStationId());
    options.port = quint16(m_port->value());
    options.dataDirectory = m_directory->text().trimmed();
    options.stepMs = m_stepMs->value();
    options.failStep = m_failStep->currentData().toString();
    options.dropFirstResultAck = m_dropAck->isChecked();
    return options;
}

void SimulatorWindow::selectStation()
{
    loadSettings();
    refreshDetails();
    refreshAvailability();
}

void SimulatorWindow::loadSettings()
{
    const int id = selectedStationId();
    if (id < 0)
        return;
    const auto options = m_options.value(id);
    m_settingsTitle->setText(tr("工位 %1").arg(id, 2, 10, QChar('0')));
    m_port->setValue(options.port);
    m_directory->setText(options.dataDirectory);
    m_directory->setToolTip(options.dataDirectory);
    m_stepMs->setValue(options.stepMs);
    refreshFaultChoices(options.failStep);
    m_dropAck->setChecked(options.dropFirstResultAck);
}

void SimulatorWindow::refreshFaultChoices(const QString &selectedStep)
{
    const auto snapshot = m_snapshots.value(selectedStationId());
    auto recipe = snapshot["recipe"].toObject();
    if (snapshot["faultsEditable"].toBool() && !snapshot["recipePreview"].toObject().isEmpty())
        recipe = snapshot["recipePreview"].toObject();
    if (recipe.isEmpty())
        recipe = Domain::defaultRecipe();

    QStringList labels{tr("正常")};
    QStringList ids{QString()};
    QStringList tips{tr("所有步骤返回配方中的正常参考值。")};
    for (const auto &value : recipe["steps"].toArray())
    {
        const auto step = value.toObject();
        labels.append(tr("%1超限").arg(step["label"].toString()));
        ids.append(step["id"].toString());
        tips.append(tr("步骤 %1，返回值大于本配方上限 %2 %3。")
                        .arg(step["id"].toString(), number(step["max"].toDouble()),
                             step["unit"].toString()));
    }
    const bool missing = !selectedStep.isEmpty() && !ids.contains(selectedStep);
    if (missing)
    {
        // Preserve a configured/custom fault explicitly rather than silently changing the server.
        labels.append(tr("步骤 %1（当前配方未包含）").arg(selectedStep));
        ids.append(selectedStep);
        tips.append(tr("该步骤未出现在此配方中，不会产生超限；请选择新项目后应用设置。"));
    }
    bool changed = m_failStep->count() != ids.size();
    for (int i = 0; !changed && i < ids.size(); ++i)
        changed =
            m_failStep->itemData(i).toString() != ids[i] || m_failStep->itemText(i) != labels[i];
    const QSignalBlocker blocker(m_failStep);
    if (changed)
    {
        m_failStep->clear();
        for (int i = 0; i < ids.size(); ++i)
            m_failStep->addItem(labels[i], ids[i]);
    }
    for (int i = 0; i < ids.size(); ++i)
        m_failStep->setItemData(i, tips[i], Qt::ToolTipRole);
    m_failStep->setCurrentIndex(qMax(0, int(ids.indexOf(selectedStep))));
    m_faultRecipeHint->setText(
        missing
            ? tr("配方：%1。原异常项目不在此配方中，请重新选择。").arg(recipe["version"].toString())
            : tr("异常选项配方：%1").arg(recipe["version"].toString()));
}

void SimulatorWindow::startSelected()
{
    const int id = selectedStationId();
    if (id < 0 || m_pending.contains(id) || m_snapshots.value(id)["listening"].toBool())
        return;
    m_options[id] = editedOptions();
    m_errors.remove(id);
    m_pending.insert(id);
    refreshAvailability();
    m_controller->startStation(m_options.value(id));
}
void SimulatorWindow::stopSelected()
{
    const int id = selectedStationId();
    if (id < 0 || m_pending.contains(id))
        return;
    m_pending.insert(id);
    refreshAvailability();
    m_controller->stopStation(id);
}
void SimulatorWindow::applySelected()
{
    const int id = selectedStationId();
    if (id < 0 || m_pending.contains(id))
        return;
    const auto options = editedOptions();
    if (!m_snapshots.value(id)["listening"].toBool())
    {
        m_options[id] = options;
        appendLog(id, tr("已保存模拟设置，启动工位时生效。"));
        refreshTable();
        refreshDetails();
        return;
    }
    m_requestedOptions[id] = options;
    m_pending.insert(id);
    refreshAvailability();
    m_controller->configureFaults(id, options.failStep, options.stepMs, options.dropFirstResultAck);
}

void SimulatorWindow::startAll()
{
    const int selected = selectedStationId();
    if (selected >= 0 && !m_snapshots.value(selected)["listening"].toBool() &&
        !m_pending.contains(selected))
        m_options[selected] = editedOptions();
    for (auto it = m_options.cbegin(); it != m_options.cend(); ++it)
    {
        if (m_pending.contains(it.key()) || m_snapshots.value(it.key())["listening"].toBool())
            continue;
        m_pending.insert(it.key());
        m_errors.remove(it.key());
        m_controller->startStation(it.value());
    }
    refreshAvailability();
}

void SimulatorWindow::stopAll()
{
    for (auto it = m_options.cbegin(); it != m_options.cend(); ++it)
    {
        if (m_pending.contains(it.key()) || !m_snapshots.value(it.key())["listening"].toBool())
            continue;
        m_pending.insert(it.key());
        m_controller->stopStation(it.key());
    }
    refreshAvailability();
}

void SimulatorWindow::receiveSnapshot(const QJsonObject &snapshot)
{
    const int id = snapshot["stationId"].toInt();
    if (!m_options.contains(id))
        return;
    m_snapshots[id] = snapshot;
    if (snapshot["listening"].toBool() && snapshot["port"].toInt() > 0)
        m_options[id].port = quint16(snapshot["port"].toInt());
    refreshTable();
    if (selectedStationId() == id)
        refreshDetails();
    refreshAvailability();
}

void SimulatorWindow::finishOperation(int id, const QString &operation, bool ok,
                                      const QString &error)
{
    m_pending.remove(id);
    if (ok)
    {
        m_errors.remove(id);
        if (operation == "configure" && m_requestedOptions.contains(id))
            m_options[id] = m_requestedOptions.take(id);
        const QMap<QString, QString> names{
            {"start", tr("工位已启动，等待主程序连接。")},
            {"stop", tr("工位已停止，状态记录保留。")},
            {"configure", tr("模拟设置已应用，供下一次测试使用。")},
            {"disconnect", tr("已模拟断线；任务继续执行，等待主程序重连。")}};
        appendLog(id, names.value(operation, tr("操作完成。")));
    }
    else
    {
        m_requestedOptions.remove(id);
        m_errors[id] = error;
        appendLog(id, tr("操作失败：") + error, true);
    }
    refreshTable();
    if (selectedStationId() == id)
    {
        loadSettings();
        refreshDetails();
    }
    refreshAvailability();
}

void SimulatorWindow::refreshTable()
{
    const QSignalBlocker blocker(m_stationTable);
    int row = 0, listening = 0, connected = 0;
    for (auto it = m_options.cbegin(); it != m_options.cend(); ++it, ++row)
    {
        const int id = it.key();
        const auto snapshot = m_snapshots.value(id);
        const bool online = snapshot["listening"].toBool();
        listening += online;
        connected += snapshot["connected"].toBool();
        cell(m_stationTable, row, 0, QString("%1").arg(id, 2, 10, QChar('0')));
        m_stationTable->item(row, 0)->setData(Qt::UserRole, id);
        cell(m_stationTable, row, 1, QString::number(it->port));
        cell(m_stationTable, row, 2, online ? tr("已启动") : tr("已停止"));
        cell(m_stationTable, row, 3,
             snapshot["handshaken"].toBool()  ? tr("已就绪")
             : snapshot["connected"].toBool() ? tr("握手中")
                                              : tr("未连接"));
        cell(m_stationTable, row, 4, m_pending.contains(id) ? tr("处理中…") : stateName(snapshot));
        m_stationTable->item(row, 2)->setForeground(QColor(online ? "#27633a" : "#6b6b6b"));
        m_stationTable->item(row, 4)->setToolTip(m_errors.contains(id) ? m_errors.value(id)
                                                                       : stateName(snapshot));
    }
    m_connections->setText(
        tr("%1 / %2 工位启动 · %3 已连接").arg(listening).arg(m_options.size()).arg(connected));
}

void SimulatorWindow::refreshDetails()
{
    const int id = selectedStationId();
    const auto snapshot = m_snapshots.value(id);
    refreshFaultChoices(m_failStep->currentData().toString());
    m_state->setText(m_errors.contains(id)
                         ? tr("请检查：") + m_errors.value(id)
                         : tr("工位 %1 · %2").arg(id, 2, 10, QChar('0')).arg(stateName(snapshot)));
    const auto sn = snapshot["sn"].toString();
    const auto runId = snapshot["runId"].toString();
    if (runId.isEmpty())
        m_runSummary->setText(tr("等待主程序下发测试任务。下表为默认配方，尚无模拟结果。"));
    else
        m_runSummary->setText(tr("产品 SN：%1     配方：%2\n本次测试编号：%3")
                                  .arg(sn, snapshot["recipeVersion"].toString(), runId));
    auto recipe = snapshot["recipe"].toObject();
    if (recipe.isEmpty())
        recipe = Domain::defaultRecipe();
    const auto rules = recipe["steps"].toArray();
    const auto measured = snapshot["steps"].toArray();
    QMap<QString, double> values;
    for (const auto &step : measured)
        values.insert(step.toObject()["stepId"].toString(), step.toObject()["value"].toDouble());
    m_stepTable->setRowCount(rules.size());
    for (int row = 0; row < rules.size(); ++row)
    {
        const auto rule = rules[row].toObject();
        const auto stepId = rule["id"].toString();
        const bool available = values.contains(stepId);
        const double value = values.value(stepId);
        const bool pass = available && std::isfinite(value) && value >= rule["min"].toDouble() &&
                          value <= rule["max"].toDouble();
        cell(m_stepTable, row, 0, QString::number(row + 1));
        cell(m_stepTable, row, 1, rule["label"].toString());
        cell(m_stepTable, row, 2,
             number(rule["min"].toDouble()) + " ~ " + number(rule["max"].toDouble()));
        cell(m_stepTable, row, 3, available ? number(value) : tr("待返回"));
        cell(m_stepTable, row, 4, rule["unit"].toString());
        cell(m_stepTable, row, 5, available ? pass ? tr("范围内") : tr("超限") : tr("待测"));
        m_stepTable->item(row, 5)->setForeground(QColor(!available ? "#6b6b6b"
                                                        : pass     ? "#27633a"
                                                                   : "#a12a2a"));
    }
}

void SimulatorWindow::refreshAvailability()
{
    const int id = selectedStationId();
    const bool valid = m_options.contains(id);
    const auto snapshot = m_snapshots.value(id);
    const bool running = snapshot["listening"].toBool();
    const bool pending = m_pending.contains(id);
    const auto state = snapshot["state"].toString();
    const bool busy = state == "executing" || state == "terminal" ||
                      (state == "interrupted" && !snapshot["reconciled"].toBool());
    m_start->setEnabled(valid && !running && !pending);
    m_stop->setEnabled(valid && running && !pending);
    m_apply->setEnabled(valid && !pending && (!running || !busy));
    m_port->setEnabled(valid && !running && !pending);
    m_directory->setEnabled(valid && !running && !pending);
    if (auto *browse = findChild<QToolButton *>("browseDirectoryButton"))
        browse->setEnabled(valid && !running && !pending);
    m_stepMs->setEnabled(valid && !pending && (!running || !busy));
    m_failStep->setEnabled(valid && !pending && (!running || !busy));
    m_dropAck->setEnabled(valid && !pending && (!running || !busy));
    m_disconnect->setEnabled(valid && running && snapshot["connected"].toBool() && !pending);
    bool canStart = false, canStop = false;
    for (auto it = m_options.cbegin(); it != m_options.cend(); ++it)
    {
        if (m_pending.contains(it.key()))
            continue;
        const bool online = m_snapshots.value(it.key())["listening"].toBool();
        canStart |= !online;
        canStop |= online;
    }
    m_startAll->setEnabled(canStart);
    m_stopAll->setEnabled(canStop);
}

void SimulatorWindow::appendLog(int stationId, const QString &text, bool error)
{
    const auto prefix = stationId > 0 ? tr("工位%1").arg(stationId, 2, 10, QChar('0')) : tr("系统");
    QString bounded = text.left(2048);
    bounded.replace('\r', ' ');
    bounded.replace('\n', ' ');
    const auto line = QDateTime::currentDateTime().toString("HH:mm:ss.zzz") + "  [" + prefix +
                      "]  " + (error ? tr("错误：") : QString()) + bounded;
    m_logs->appendPlainText(line);
    statusBar()->showMessage(prefix + " · " + bounded, error ? 15000 : 6000);
}
