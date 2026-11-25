#pragma once
#include "stationendpoint.h"
#include <QJsonArray>
#include <QList>
#include <QMainWindow>

class WorkbenchController;
class StationModel;
class StepModel;
class QTableView;
class QLineEdit;
class QComboBox;
class QPushButton;
class QLabel;
class QCloseEvent;
class HistoryDialog;
class LogDialog;
class MaintenanceDialog;
class StationConfigurationDialog;

class MainWindow final : public QMainWindow
{
    Q_OBJECT
  public:
    // 创建测试主窗口，共用 dataDirectory 数据和 endpoints 工位配置。
    MainWindow(const QString &dataDirectory, const QList<StationEndpoint> &endpoints,
               QWidget *parent = nullptr);
    // 停止唯一后台控制器并回收各业务窗口。
    ~MainWindow() override;
    // 返回所有业务窗口共享的后台控制器。
    WorkbenchController *controller() const
    {
        return m_controller;
    }

  protected:
    // 关闭主窗口时一并关闭子窗口并停止后台。
    void closeEvent(QCloseEvent *event) override;

  private:
    // 创建工位表、分行输入和当前测试区域。
    void buildUi();
    // 创建历史、日志、工位配置及管理员维护入口。
    void buildNavigation();
    // 将所选工位的状态和步骤显示到当前测试区。
    void showSelectedStation();
    // 将当前配方作为所选工位的只读预览发送。
    void syncRecipePreview();
    // 按后台状态更新开始、取消和核对按钮。
    void updateCommandAvailability();
    // 读取并校验 JSON 文件后发布新配方版本。
    void importRecipe();
    // 返回当前选中工位的业务编号。
    int selectedStationId() const;
    // 主窗口持有所有业务窗口共用的后台服务。
    WorkbenchController *m_controller;
    // 保存工位状态及当前测试步骤。
    StationModel *m_stations;
    // 保存当前测试的步骤数据。
    StepModel *m_steps;
    // 显示工位和所选工位的测试步骤。
    QTableView *m_stationView, *m_stepView;
    // 输入待测产品序列号。
    QLineEdit *m_snInput;
    // 选择新测试使用的配方版本。
    QComboBox *m_recipe;
    // 提交测试、取消、核对及配方发布操作。
    QPushButton *m_start, *m_cancel, *m_reconcile, *m_import;
    // 显示当前工位、测试状态、身份及在线数量。
    QLabel *m_stationTitle, *m_detail, *m_runIdentity, *m_connectionSummary;
    // 缓存已发布配方，供当前测试步骤预览。
    QJsonArray m_recipes;
    // 标记后台是否已初始化成功。
    bool m_backendReady = false;
    // 隐藏后复用历史及日志窗口，缓存不随关窗丢失。
    HistoryDialog *m_historyDialog;
    // 复用独立日志窗口并保留隐藏期间的记录。
    LogDialog *m_logDialog;
    // 首次打开时创建管理员维护窗口。
    MaintenanceDialog *m_maintenanceDialog = nullptr;
    // 复用独立工位连接配置窗口，保存结果不改变当前连接。
    StationConfigurationDialog *m_stationConfigurationDialog;
};
