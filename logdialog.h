#pragma once
#include <QDialog>

class WorkbenchController;
class BoundedLogModel;
class QListView;
class QLabel;

class LogDialog final : public QDialog
{
    Q_OBJECT
  public:
    // 在共享控制器初始化前建立日志缓存，隐藏窗口也继续收集。
    explicit LogDialog(WorkbenchController *controller, QWidget *parent = nullptr);

  private:
    // 缓存最近一千条运行日志。
    BoundedLogModel *m_logs;
    // 显示缓存日志并按用户当前位置决定是否跟随滚动。
    QListView *m_logView;
    // 显示当前已缓存的日志条数。
    QLabel *m_count;
};
