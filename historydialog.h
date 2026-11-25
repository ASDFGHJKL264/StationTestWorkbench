#pragma once
#include "repository.h"
#include <QDialog>

class WorkbenchController;
class HistoryModel;
class StepModel;
class QSortFilterProxyModel;
class QTableView;
class QLineEdit;
class QCheckBox;
class QDateEdit;
class QComboBox;
class QPushButton;
class QLabel;

class HistoryDialog final : public QDialog
{
    Q_OBJECT
  public:
    // 创建独立历史窗口，复用主窗口的后台控制器。
    explicit HistoryDialog(WorkbenchController *controller, QWidget *parent = nullptr);

  private:
    // 创建历史筛选、列表、步骤和导出控件。
    void buildUi();
    // 显示当前历史行的身份、步骤和判定。
    void showHistorySelection();
    // 查询首页或沿同一条件读取更早记录。
    void queryHistory(bool older);
    // 读取当前填写的历史筛选条件。
    HistoryFilter historyFilter() const;
    // 条件变化后使旧分页游标失效。
    void historyFilterChanged();
    // 以固定测试编号导出所选报告。
    void exportSelectedReport();
    // 导出全部历史失败产品的去重序列号。
    void exportFailedSns();
    // 共享主窗口的后台服务，不拥有其生命周期。
    WorkbenchController *m_controller;
    // 保存当前历史页及所选运行的步骤。
    HistoryModel *m_history;
    // 保存所选历史测试的步骤明细。
    StepModel *m_historySteps;
    // 提供当前历史页内的排序。
    QSortFilterProxyModel *m_historyProxy;
    // 显示历史记录及对应步骤。
    QTableView *m_historyView, *m_historyStepView;
    // 输入完整产品序列号。
    QLineEdit *m_historyFilter;
    // 选择工位和质量判定条件。
    QComboBox *m_historyStation, *m_historyVerdict;
    // 控制起止日期是否参与查询。
    QCheckBox *m_historyFromEnabled, *m_historyToEnabled;
    // 保存按本地日历选择的日期边界。
    QDateEdit *m_historyFromDate, *m_historyToDate;
    // 查询、翻页及两种导出入口。
    QPushButton *m_query, *m_older, *m_export, *m_exportFailedSns;
    // 显示当前页数量、所选身份和操作结果。
    QLabel *m_historySummary, *m_historyDetail, *m_status;
    // 保存后台可用性及当前查询、导出任务状态。
    bool m_backendReady = false, m_historyBusy = false, m_hasOlder = false;
    // 标记报告和异常序列号导出是否在途。
    bool m_reportBusy = false, m_failedSnsBusy = false;
    // 固定本页查询条件，供分页及迟到回报核对。
    HistoryFilter m_queryFilter;
};
