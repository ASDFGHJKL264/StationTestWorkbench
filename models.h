#pragma once
#include <QAbstractTableModel>
#include <QAbstractListModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QStyledItemDelegate>

// Text stays in the model for copying and accessibility; the delegate only changes painting.
class QualityDelegate final : public QStyledItemDelegate
{
  public:
    // 创建判定文字绘制器；parent 为所属对象。
    explicit QualityDelegate(QObject *parent = nullptr);
    // 绘制判定单元格；painter 为绘图工具，option 为显示状态，index 为目标单元格。
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    // 返回单元格建议大小；option 为显示状态，index 为目标单元格。
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

class StationModel final : public QAbstractTableModel
{
    Q_OBJECT
  public:
    enum Role
    {
        // StationIdRole 用于读取当前行的工位编号。
        StationIdRole = Qt::UserRole + 1,
        // ObjectRole 用于读取当前行完整的工位状态。
        ObjectRole
    };
    // 创建工位表模型；parent 为所属对象。
    explicit StationModel(QObject *parent = nullptr);
    // 返回顶层工位行数；parent 为父级索引。
    int rowCount(const QModelIndex &parent = {}) const override;
    // 返回工位表列数；parent 为父级索引。
    int columnCount(const QModelIndex &parent = {}) const override;
    // 读取工位单元格；index 为单元格位置，role 为文字、颜色等数据用途。
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    // 读取工位表头；section 为行列序号，orientation 为方向，role 为数据用途。
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    // 新增或更新工位行；station 为包含工位编号的状态快照。
    void updateStation(const QJsonObject &station);
    // 读取指定工位行；row 为从零开始的行号。
    QJsonObject stationAt(int row) const;
    // 按编号查找行号；stationId 为目标工位编号。
    int rowForStation(int stationId) const;

  private:
    // m_rows 按工位编号保存各工位状态。
    QList<QJsonObject> m_rows;
};

class StepModel final : public QAbstractTableModel
{
    Q_OBJECT
  public:
    // 创建测试步骤表模型；parent 为所属对象。
    explicit StepModel(QObject *parent = nullptr);
    // 返回顶层测试步骤数；parent 为父级索引。
    int rowCount(const QModelIndex &parent = {}) const override;
    // 返回步骤表列数；parent 为父级索引。
    int columnCount(const QModelIndex &parent = {}) const override;
    // 读取步骤单元格；index 为单元格位置，role 为文字、颜色等数据用途。
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    // 读取步骤表头；section 为行列序号，orientation 为方向，role 为数据用途。
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    // 更新测试步骤；steps 为配方限值及已收到的测量结果。
    void setSteps(const QJsonArray &steps);

  private:
    // m_rows 保存按配方顺序排列的步骤。
    QJsonArray m_rows;
};

class HistoryModel final : public QAbstractTableModel
{
    Q_OBJECT
  public:
    enum Role
    {
        // SortRole 用真实时间和数值提供历史表排序依据。
        SortRole = Qt::UserRole + 1
    };
    // 创建历史记录表模型；parent 为所属对象。
    explicit HistoryModel(QObject *parent = nullptr);
    // 返回当前页历史记录数；parent 为父级索引。
    int rowCount(const QModelIndex &parent = {}) const override;
    // 返回历史记录表列数；parent 为父级索引。
    int columnCount(const QModelIndex &parent = {}) const override;
    // 读取历史单元格；index 为单元格位置，role 为显示或排序等数据用途。
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    // 读取历史表头；section 为行列序号，orientation 为方向，role 为数据用途。
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    // 替换当前历史页；rows 为后台查询返回的记录。
    void setRows(const QJsonArray &rows);
    // 读取一条历史记录；row 为当前页内从零开始的行号。
    QJsonObject recordAt(int row) const;
    // 返回当前页最小记录编号，作为查询更早记录的游标。
    int lastId() const;

  private:
    // m_rows 保存当前页历史记录及其步骤详情。
    QJsonArray m_rows;
};

class BoundedLogModel final : public QAbstractListModel
{
    Q_OBJECT
  public:
    // 创建限量日志列表；capacity 为最多保留条数，parent 为所属对象。
    explicit BoundedLogModel(int capacity = 1000, QObject *parent = nullptr);
    // 返回界面缓存的日志条数；parent 为父级索引。
    int rowCount(const QModelIndex &parent = {}) const override;
    // 读取日志行；index 为行位置，role 为显示文字或颜色等数据用途。
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    // 追加日志；message 为日志内容，warning 标记是否为警告。
    void append(const QString &message, bool warning = false);

  private:
    struct Entry
    {
        // time 保存显示时间，text 保存日志正文。
        QString time, text;
        // warning 决定该条日志是否显示为警告。
        bool warning;
    };
    // m_rows 保存最近的日志条目。
    QList<Entry> m_rows;
    // m_capacity 限制界面缓存的最大日志条数。
    int m_capacity;
};
