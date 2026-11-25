#pragma once
#include "stationendpoint.h"
#include <QDialog>
#include <QList>

class QTableWidget;
class QLabel;

class StationConfigurationDialog final : public QDialog
{
    Q_OBJECT
  public:
    // 按工位显示连接配置，保存到 dataDirectory，关闭后复用窗口。
    StationConfigurationDialog(const QString &dataDirectory,
                               const QList<StationEndpoint> &endpoints, QWidget *parent = nullptr);
    // 显示读取或保存失败原因，不显示配置文件内容。
    void showError(const QString &message);

  private:
    // 校验表格输入并原子保存，当前测试连接保持原配置。
    void save();
    // 保存当前业务数据目录，配置与对应数据库一起管理。
    QString m_dataDirectory;
    // 按工位列出IP或主机名和TCP端口。
    QTableWidget *m_table;
    // 显示实际保存结果或输入错误。
    QLabel *m_status;
};
