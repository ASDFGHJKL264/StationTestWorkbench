#include "historydialog.h"
#include "workbenchcontroller.h"
#include "models.h"
#include "toolstyle.h"
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QTableView>
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
} // namespace

HistoryDialog::HistoryDialog(WorkbenchController *controller, QWidget *parent)
    : QDialog(parent), m_controller(controller), m_history(new HistoryModel(this)),
      m_historySteps(new StepModel(this)), m_historyProxy(new QSortFilterProxyModel(this))
{
    setObjectName("historyDialog");
    setWindowTitle(QStringLiteral("历史追溯"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setPalette(ToolStyle::lightPalette());
    setStyleSheet(ToolStyle::stylesheet() +
                  QStringLiteral("QDateEdit { min-height:23px; color:#222222; background:white; }"
                                 "QDateEdit:disabled { color:#666666; background:#eeeeee; }"));
    resize(1080, 740);
    // 适配当前屏幕，不强制放大主窗口。
    if (auto *screen = QGuiApplication::primaryScreen())
        resize(qMin(1080, screen->availableGeometry().width() - 40),
               qMin(740, screen->availableGeometry().height() - 60));
    buildUi();
    connect(m_controller, &WorkbenchController::historyReady, this,
            // 显示查询结果；rows 为当前页记录，more 表示是否还有更早记录。
            [this](const QJsonArray &rows, bool more)
            {
                m_historyBusy = false;
                m_hasOlder = more;
                m_history->setRows(rows);
                m_query->setEnabled(m_backendReady);
                // currentFilter 表示界面条件是否仍与本次结果对应。
                const bool currentFilter = historyFilter() == m_queryFilter;
                m_older->setEnabled(m_backendReady && currentFilter && more &&
                                    m_history->lastId() > 0);
                m_historySummary->setText(
                    QStringLiteral("本页 %1 条 · %2")
                        .arg(rows.size())
                        .arg(more ? QStringLiteral("可查看更早记录") : QStringLiteral("已到末页")));
                if (!currentFilter)
                    historyFilterChanged();
                m_historySteps->setSteps({});
                m_historyDetail->setText(QStringLiteral("未选择记录"));
                if (!rows.isEmpty())
                    m_historyView->selectRow(0);
                else
                    showHistorySelection();
            });
    connect(m_controller, &WorkbenchController::reportReady, this,
            // 反馈报告导出；path 为文件位置，ok 为成功标记，error 为失败原因。
            [this](const QString &path, bool ok, const QString &error)
            {
                m_reportBusy = false;
                showHistorySelection();
                // text 为本次报告导出结果的提示文字。
                const auto text = ok ? QStringLiteral("测试报告已导出：") + path
                                     : QStringLiteral("测试报告导出失败：") + error;
                m_status->setText(text);
            });
    connect(m_controller, &WorkbenchController::historyFailed, this,
            // 恢复查询操作并提示失败；error 为查询失败原因。
            [this](const QString &error)
            {
                m_historyBusy = false;
                m_query->setEnabled(m_backendReady);
                m_older->setEnabled(m_backendReady && historyFilter() == m_queryFilter &&
                                    m_hasOlder && m_history->lastId() > 0);
                m_historySummary->setText(QStringLiteral("历史查询失败：") + error);
            });
    connect(m_controller, &WorkbenchController::failedSnsExported, this,
            // 恢复导出按钮，并显示文件位置、数量或失败原因。
            [this](const QString &path, bool ok, qint64 count, const QString &error)
            {
                m_failedSnsBusy = false;
                m_exportFailedSns->setText(QStringLiteral("导出异常 SN"));
                m_exportFailedSns->setEnabled(m_backendReady);
                m_status->setText(
                    ok ? QStringLiteral("已导出 %1 个异常 SN：%2").arg(count).arg(path)
                       : QStringLiteral("导出失败：") + error);
            });
    connect(m_controller, &WorkbenchController::ready, this,
            [this](bool ok)
            {
                m_backendReady = ok;
                m_query->setEnabled(ok && !m_historyBusy);
                m_older->setEnabled(ok && !m_historyBusy && m_hasOlder &&
                                    historyFilter() == m_queryFilter);
                m_exportFailedSns->setEnabled(ok && !m_failedSnsBusy);
                showHistorySelection();
            });
}

void HistoryDialog::buildUi()
{
    // historyPage 为历史查询与导出页面。
    auto *historyPage = this;
    // historyLayout 纵向排列历史筛选、列表与步骤详情。
    auto *historyLayout = new QVBoxLayout(historyPage);
    historyLayout->setContentsMargins(8, 8, 8, 8);
    // filterLayout 横向排列历史 SN 输入框和查询分页按钮。
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
    historyLayout->addLayout(filterLayout);
    // conditions 横向排列日期、工位和判定筛选条件。
    auto *conditions = new QHBoxLayout;
    conditions->setSpacing(6);
    m_historyFromEnabled = new QCheckBox(QStringLiteral("开始日期"), historyPage);
    m_historyFromEnabled->setObjectName("historyFromEnabled");
    conditions->addWidget(m_historyFromEnabled);
    m_historyFromDate = new QDateEdit(QDate::currentDate(), historyPage);
    m_historyFromDate->setObjectName("historyFromDate");
    m_historyFromDate->setDisplayFormat("yyyy-MM-dd");
    m_historyFromDate->setCalendarPopup(true);
    m_historyFromDate->setEnabled(false);
    conditions->addWidget(m_historyFromDate);
    m_historyToEnabled = new QCheckBox(QStringLiteral("结束日期"), historyPage);
    m_historyToEnabled->setObjectName("historyToEnabled");
    conditions->addWidget(m_historyToEnabled);
    m_historyToDate = new QDateEdit(QDate::currentDate(), historyPage);
    m_historyToDate->setObjectName("historyToDate");
    m_historyToDate->setDisplayFormat("yyyy-MM-dd");
    m_historyToDate->setCalendarPopup(true);
    m_historyToDate->setEnabled(false);
    conditions->addWidget(m_historyToDate);
    m_historyStation = new QComboBox(historyPage);
    m_historyStation->setObjectName("historyStationFilter");
    m_historyStation->addItem(QStringLiteral("全部工位"), 0);
    for (int station = 1; station <= 4; ++station)
        m_historyStation->addItem(QStringLiteral("工位 %1").arg(station), station);
    conditions->addWidget(m_historyStation);
    m_historyVerdict = new QComboBox(historyPage);
    m_historyVerdict->setObjectName("historyVerdictFilter");
    m_historyVerdict->addItem(QStringLiteral("全部判定"), QString{});
    for (const auto *verdict : {"PASS", "FAIL", "UNKNOWN"})
        m_historyVerdict->addItem(QString::fromLatin1(verdict), QString::fromLatin1(verdict));
    conditions->addWidget(m_historyVerdict);
    historyLayout->addLayout(conditions);
    connect(m_historyFromEnabled, &QCheckBox::toggled, m_historyFromDate, &QWidget::setEnabled);
    connect(m_historyToEnabled, &QCheckBox::toggled, m_historyToDate, &QWidget::setEnabled);
    connect(m_historyFromEnabled, &QCheckBox::toggled, this, &HistoryDialog::historyFilterChanged);
    connect(m_historyToEnabled, &QCheckBox::toggled, this, &HistoryDialog::historyFilterChanged);
    connect(m_historyFromDate, &QDateEdit::dateChanged, this, &HistoryDialog::historyFilterChanged);
    connect(m_historyToDate, &QDateEdit::dateChanged, this, &HistoryDialog::historyFilterChanged);
    connect(m_historyStation, &QComboBox::currentIndexChanged, this,
            &HistoryDialog::historyFilterChanged);
    connect(m_historyVerdict, &QComboBox::currentIndexChanged, this,
            &HistoryDialog::historyFilterChanged);
    connect(m_historyFilter, &QLineEdit::textChanged, this, &HistoryDialog::historyFilterChanged);
    // exportLayout 横向排列两种历史导出按钮。
    auto *exportLayout = new QHBoxLayout;
    m_export = new QPushButton(QStringLiteral("导出本次报告"), historyPage);
    m_export->setObjectName("exportReportButton");
    m_export->setEnabled(false);
    exportLayout->addWidget(m_export);
    m_exportFailedSns = new QPushButton(QStringLiteral("导出异常 SN"), historyPage);
    m_exportFailedSns->setObjectName("exportFailedSnsButton");
    m_exportFailedSns->setEnabled(false);
    exportLayout->addWidget(m_exportFailedSns);
    exportLayout->addStretch();
    historyLayout->addLayout(exportLayout);
    m_historySummary = label(QStringLiteral("0 条"));
    m_historySummary->setWordWrap(true);
    m_historySummary->setObjectName("historySummary");
    historyLayout->addWidget(m_historySummary);
    // historySplit 分隔历史记录列表和所选记录的步骤详情。
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
    // historical 为所选历史记录的身份和步骤展示区。
    auto *historical = new QWidget(historySplit);
    // historicalLayout 纵向排列历史身份说明和步骤表。
    auto *historicalLayout = new QVBoxLayout(historical);
    historicalLayout->setContentsMargins(0, 6, 0, 0);
    m_historyDetail = label(QStringLiteral("未选择记录"));
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
    connect(m_historyView->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            // 选择历史行时刷新对应的配方和实测步骤。
            [this] { showHistorySelection(); });
    // 点击查询时从当前条件的最新一页开始读取。
    connect(m_query, &QPushButton::clicked, this, [this] { queryHistory(false); });
    // 点击更早记录时沿当前查询游标翻页。
    connect(m_older, &QPushButton::clicked, this, [this] { queryHistory(true); });
    connect(m_export, &QPushButton::clicked, this, &HistoryDialog::exportSelectedReport);
    connect(m_exportFailedSns, &QPushButton::clicked, this, &HistoryDialog::exportFailedSns);
    connect(m_historyFilter, &QLineEdit::returnPressed, this,
            // 历史 SN 输入框回车时重新查询最新记录。
            [this]
            {
                if (m_query->isEnabled())
                    queryHistory(false);
            });
    // 显示操作结果，并把关闭入口留在表格之外。
    auto *footer = new QHBoxLayout;
    m_status = label({}, this);
    m_status->setObjectName("historyStatus");
    m_status->setWordWrap(true);
    footer->addWidget(m_status, 1);
    // 关闭仅隐藏历史窗口，查询与导出继续由共享控制器完成。
    auto *close = new QPushButton(QStringLiteral("关闭"), this);
    close->setObjectName("closeHistoryButton");
    footer->addWidget(close);
    historyLayout->addLayout(footer);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
}
void HistoryDialog::showHistorySelection()
{
    // row 为当前选中的历史记录及其步骤详情。
    const auto row =
        m_history->recordAt(m_historyProxy->mapToSource(m_historyView->currentIndex()).row());
    m_export->setEnabled(m_backendReady && !m_reportBusy && !row["runId"].toString().isEmpty());
    if (row.isEmpty())
    {
        m_historySteps->setSteps({});
        return;
    }
    m_historySteps->setSteps(row["steps"].toArray());
    // historyDetail 保存所选历史记录的身份与判定说明。
    QString historyDetail = QStringLiteral("SN：%1  ·  配方：%2  ·  %3\n测试编号：%4")
                                .arg(row["sn"].toString(), row["recipeVersion"].toString(),
                                     row["verdict"].toString(), row["runId"].toString());
    if (!row["reason"].toString().isEmpty())
        historyDetail += "\n" + row["reason"].toString();
    m_historyDetail->setText(historyDetail);
}
void HistoryDialog::queryHistory(bool older)
{
    if (!m_backendReady || m_historyBusy)
        return;
    // cursor 为读取更早记录时使用的数据库编号界限。
    int cursor = 0;
    if (older)
    {
        if (!m_hasOlder || !(historyFilter() == m_queryFilter))
            return;
        cursor = m_history->lastId();
        if (cursor <= 0)
            return;
    }
    else
    {
        // filter 为本次最新查询准备提交的筛选条件。
        const auto filter = historyFilter();
        if (filter.startDate.isValid() && filter.endDate.isValid() &&
            filter.startDate > filter.endDate)
        {
            m_historySummary->setText(QStringLiteral("开始日期不能晚于结束日期"));
            return;
        }
        m_queryFilter = filter;
    }
    m_historyBusy = true;
    m_query->setEnabled(false);
    m_older->setEnabled(false);
    m_controller->refreshFilteredHistory(m_queryFilter, cursor);
}

HistoryFilter HistoryDialog::historyFilter() const
{
    // filter 汇总历史查询控件当前填写的有效条件。
    HistoryFilter filter;
    filter.sn = m_historyFilter->text().trimmed();
    filter.stationId = m_historyStation->currentData().toInt();
    filter.verdict = m_historyVerdict->currentData().toString();
    if (m_historyFromEnabled->isChecked())
        filter.startDate = m_historyFromDate->date();
    if (m_historyToEnabled->isChecked())
        filter.endDate = m_historyToDate->date();
    return filter;
}

void HistoryDialog::historyFilterChanged()
{
    m_hasOlder = false;
    m_older->setEnabled(false);
    m_historySummary->setText(QStringLiteral("筛选已更改"));
}
void HistoryDialog::exportSelectedReport()
{
    if (!m_export->isEnabled() || m_reportBusy)
        return;
    // row 保存打开文件对话框前选中的历史记录。
    // Capture the selected identity before the path dialog; incoming rows may refresh meanwhile.
    const auto row =
        m_history->recordAt(m_historyProxy->mapToSource(m_historyView->currentIndex()).row());
    // runId 固定本次导出对应的测试编号。
    const auto runId = row["runId"].toString();
    if (runId.isEmpty())
        return;
    // dialog 用于选择本次 HTML 报告的保存位置。
    QFileDialog dialog(this, QStringLiteral("导出所选测试报告"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilter(QStringLiteral("HTML 测试报告 (*.html *.htm)"));
    dialog.setDefaultSuffix("html");
    dialog.selectFile(QStringLiteral("测试报告-%1.html").arg(runId));
    // Let the dialog resolve the suffix before its normal overwrite confirmation.
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
        return;
    // path 为确认后报告实际保存到的文件路径。
    const auto path = dialog.selectedFiles().first();
    m_reportBusy = true;
    m_export->setEnabled(false);
    m_controller->exportReport(runId, path);
}

void HistoryDialog::exportFailedSns()
{
    if (!m_backendReady || m_failedSnsBusy)
        return;
    // dialog 用于选择全历史异常 SN 清单的 CSV 保存位置。
    QFileDialog dialog(this, QStringLiteral("导出异常 SN"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilter(QStringLiteral("CSV 清单 (*.csv)"));
    dialog.setDefaultSuffix("csv");
    dialog.selectFile(QStringLiteral("异常产品SN.csv"));
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
        return;
    m_failedSnsBusy = true;
    m_exportFailedSns->setText(QStringLiteral("导出中…"));
    m_exportFailedSns->setEnabled(false);
    m_controller->exportFailedSns(dialog.selectedFiles().first());
}
