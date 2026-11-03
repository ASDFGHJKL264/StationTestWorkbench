#include "models.h"
#include <QApplication>
#include <QPainter>
#include <QStyle>
#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <algorithm>

namespace
{
bool qualityColor(const QString &text, QColor *foreground)
{
    if (text == "PASS")
        *foreground = QColor("#287044");
    else if (text == "FAIL")
        *foreground = QColor("#a83232");
    else if (text == "CANCELLED" || text == "UNKNOWN")
        *foreground = QColor("#805d24");
    else if (text == "PENDING")
        *foreground = QColor("#666666");
    else
        return false;
    return true;
}
} // namespace

QualityDelegate::QualityDelegate(QObject *parent) : QStyledItemDelegate(parent) {}
void QualityDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                            const QModelIndex &index) const
{
    QStyleOptionViewItem item(option);
    initStyleOption(&item, index);
    QColor foreground;
    if (qualityColor(item.text, &foreground) && (item.state & QStyle::State_Enabled) &&
        !(item.state & QStyle::State_Selected))
        item.palette.setColor(QPalette::Text, foreground);
    // Draw ordinary text, preserving Qt's selection, keyboard focus and accessibility.
    QStyledItemDelegate::paint(painter, item, index);
}
QSize QualityDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    return QStyledItemDelegate::sizeHint(option, index);
}

namespace
{
QString number(const QJsonValue &v)
{
    return v.isDouble() ? QString::number(v.toDouble(), 'g', 8) : QStringLiteral("—");
}
QVariant verdictBrush(const QString &verdict)
{
    if (verdict == "PASS")
        return QBrush(QColor("#087f65"));
    if (verdict == "FAIL")
        return QBrush(QColor("#c43b47"));
    if (verdict == "CANCELLED" || verdict == "UNKNOWN")
        return QBrush(QColor("#a8650a"));
    return QBrush(QColor("#64748b"));
}
QString executionText(const QString &value)
{
    if (value == "RESERVED")
        return QStringLiteral("等待终态");
    if (value == "COMPLETED")
        return QStringLiteral("已完成");
    if (value == "CANCELLED")
        return QStringLiteral("已取消");
    if (value == "INTERRUPTED")
        return QStringLiteral("中断 / 未知");
    return value;
}
QString localTime(const QString &text)
{
    auto dt = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!dt.isValid())
        dt = QDateTime::fromString(text, Qt::ISODate);
    return dt.isValid() ? dt.toLocalTime().toString("MM-dd HH:mm:ss.zzz") : text;
}
} // namespace

StationModel::StationModel(QObject *parent) : QAbstractTableModel(parent) {}
int StationModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}
int StationModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : 8;
}
QJsonObject StationModel::stationAt(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows[row] : QJsonObject{};
}
int StationModel::rowForStation(int id) const
{
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows[i]["stationId"].toInt() == id)
            return i;
    return -1;
}
void StationModel::updateStation(const QJsonObject &station)
{
    const int id = station["stationId"].toInt();
    if (id < 1 || id > 4)
        return;
    const int old = rowForStation(id);
    if (old >= 0)
    {
        m_rows[old] = station;
        emit dataChanged(index(old, 0), index(old, columnCount() - 1));
        return;
    }
    int row = 0;
    while (row < m_rows.size() && m_rows[row]["stationId"].toInt() < id)
        ++row;
    beginInsertRows({}, row, row);
    m_rows.insert(row, station);
    endInsertRows();
}
QVariant StationModel::data(const QModelIndex &i, int role) const
{
    if (!i.isValid() || i.row() < 0 || i.row() >= rowCount() || i.column() < 0 ||
        i.column() >= columnCount())
        return {};
    const auto row = m_rows[i.row()];
    if (role == StationIdRole)
        return row["stationId"].toInt();
    if (role == ObjectRole)
        return QVariant::fromValue(row);
    if (role == Qt::ToolTipRole)
        return row["detail"].toString() + "\nRun ID: " + row["runId"].toString();
    if (role == Qt::TextAlignmentRole)
        return int(Qt::AlignVCenter |
                   (i.column() < 3 || i.column() == 5 || i.column() == 6 ? Qt::AlignHCenter
                                                                         : Qt::AlignLeft));
    if (role == Qt::ForegroundRole && i.column() == 5)
        return verdictBrush(row["verdict"].toString());
    if (role == Qt::ForegroundRole && i.column() == 1)
        return QBrush(QColor(row["online"].toBool() ? "#087f65" : "#64748b"));
    if (role != Qt::DisplayRole)
        return {};
    switch (i.column())
    {
    case 0:
        return QStringLiteral("工位 %1").arg(row["stationId"].toInt(), 2, 10, QChar('0'));
    case 1:
        return row["online"].toBool() ? QStringLiteral("在线") : QStringLiteral("离线");
    case 2:
        return row["execution"].toString();
    case 3:
        return row["sn"].toString(QStringLiteral("—"));
    case 4:
        return row["recipeVersion"].toString(QStringLiteral("—"));
    case 5:
        return row["verdict"].toString("PENDING");
    case 6:
        return row["stored"].toBool() ? QStringLiteral("已提交") : QStringLiteral("未归档");
    case 7:
        return row["runId"].toString(QStringLiteral("—"));
    }
    return {};
}
QVariant StationModel::headerData(int s, Qt::Orientation o, int role) const
{
    static const QStringList names{QStringLiteral("工位"),     QStringLiteral("连接"),
                                   QStringLiteral("执行阶段"), QStringLiteral("产品 SN"),
                                   QStringLiteral("配方版本"), QStringLiteral("测试判定"),
                                   QStringLiteral("本地归档"), QStringLiteral("测试编号 Run ID")};
    if (role == Qt::DisplayRole && o == Qt::Horizontal && s >= 0 && s < names.size())
        return names[s];
    return QAbstractTableModel::headerData(s, o, role);
}

StepModel::StepModel(QObject *parent) : QAbstractTableModel(parent) {}
int StepModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}
int StepModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : 6;
}
void StepModel::setSteps(const QJsonArray &steps)
{
    bool same = steps.size() == m_rows.size();
    for (int i = 0; same && i < steps.size(); ++i)
        same = steps[i].toObject()["stepId"] == m_rows[i].toObject()["stepId"];
    if (same)
    {
        m_rows = steps;
        if (!m_rows.isEmpty())
            emit dataChanged(index(0, 0), index(m_rows.size() - 1, columnCount() - 1));
    }
    else
    {
        beginResetModel();
        m_rows = steps;
        endResetModel();
    }
}
QVariant StepModel::data(const QModelIndex &i, int role) const
{
    if (!i.isValid() || i.row() < 0 || i.row() >= rowCount() || i.column() < 0 ||
        i.column() >= columnCount())
        return {};
    const auto row = m_rows[i.row()].toObject();
    const bool measured = row["value"].isDouble();
    const QString verdict =
        !measured ? QStringLiteral("待测")
                  : (row["passed"].isBool() ? (row["passed"].toBool() ? "PASS" : "FAIL")
                                            : QStringLiteral("待判定"));
    if (role == Qt::ToolTipRole)
        return QStringLiteral("步骤标识：%1\n限值按当前测试绑定的配方版本判定。")
            .arg(row["stepId"].toString());
    if (role == Qt::TextAlignmentRole)
        return int(Qt::AlignVCenter | (i.column() == 1 ? Qt::AlignLeft : Qt::AlignHCenter));
    if (role == Qt::ForegroundRole && i.column() == 5)
        return verdictBrush(verdict);
    if (role != Qt::DisplayRole)
        return {};
    switch (i.column())
    {
    case 0:
        return i.row() + 1;
    case 1:
        return row["label"].toString();
    case 2:
        return number(row["min"]) + " ~ " + number(row["max"]);
    case 3:
        return number(row["value"]);
    case 4:
        return row["unit"].toString();
    case 5:
        return verdict;
    }
    return {};
}
QVariant StepModel::headerData(int s, Qt::Orientation o, int role) const
{
    static const QStringList names{QStringLiteral("序号"),     QStringLiteral("测试项目"),
                                   QStringLiteral("允许范围"), QStringLiteral("实测值"),
                                   QStringLiteral("单位"),     QStringLiteral("判定")};
    if (role == Qt::DisplayRole && o == Qt::Horizontal && s >= 0 && s < names.size())
        return names[s];
    return QAbstractTableModel::headerData(s, o, role);
}

HistoryModel::HistoryModel(QObject *parent) : QAbstractTableModel(parent) {}
int HistoryModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}
int HistoryModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : 7;
}
void HistoryModel::setRows(const QJsonArray &rows)
{
    beginResetModel();
    m_rows = rows;
    endResetModel();
}
QJsonObject HistoryModel::recordAt(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows[row].toObject() : QJsonObject{};
}
int HistoryModel::lastId() const
{
    int lowest = 0;
    for (const auto &value : m_rows)
    {
        const int id = value.toObject()["id"].toInt();
        if (id > 0 && (!lowest || id < lowest))
            lowest = id;
    }
    return lowest;
}
QVariant HistoryModel::data(const QModelIndex &i, int role) const
{
    if (!i.isValid() || i.row() < 0 || i.row() >= rowCount() || i.column() < 0 ||
        i.column() >= columnCount())
        return {};
    const auto row = recordAt(i.row());
    if (role == Qt::UserRole)
        return QVariant::fromValue(row);
    if (role == SortRole)
    {
        // Sort actual time values, not the shortened month/day string shown in the view.
        if (i.column() == 0)
        {
            auto dt = QDateTime::fromString(row["startedAt"].toString(), Qt::ISODateWithMs);
            if (!dt.isValid())
                dt = QDateTime::fromString(row["startedAt"].toString(), Qt::ISODate);
            return dt.toMSecsSinceEpoch();
        }
        if (i.column() == 1)
            return row["stationId"].toInt();
        const QStringList keys{"startedAt", "stationId", "sn",   "recipeVersion",
                               "execution", "verdict",   "runId"};
        return row[keys[i.column()]].toString();
    }
    if (role == Qt::ToolTipRole)
        return QStringLiteral("Run ID: %1\n开始：%2\n结束：%3\n%4")
            .arg(row["runId"].toString(), row["startedAt"].toString(), row["finishedAt"].toString(),
                 row["reason"].toString());
    if (role == Qt::ForegroundRole && i.column() == 5)
        return verdictBrush(row["verdict"].toString());
    if (role == Qt::TextAlignmentRole && (i.column() == 1 || i.column() == 4 || i.column() == 5))
        return int(Qt::AlignCenter);
    if (role != Qt::DisplayRole)
        return {};
    switch (i.column())
    {
    case 0:
        return localTime(row["startedAt"].toString());
    case 1:
        return row["stationId"].toInt();
    case 2:
        return row["sn"].toString();
    case 3:
        return row["recipeVersion"].toString();
    case 4:
        return executionText(row["execution"].toString());
    case 5:
        return row["verdict"].toString();
    case 6:
        return row["runId"].toString();
    }
    return {};
}
QVariant HistoryModel::headerData(int s, Qt::Orientation o, int role) const
{
    static const QStringList names{QStringLiteral("开始时间（本地）"), QStringLiteral("工位"),
                                   QStringLiteral("产品 SN"),          QStringLiteral("配方版本"),
                                   QStringLiteral("执行结果"),         QStringLiteral("判定"),
                                   QStringLiteral("测试编号")};
    if (role == Qt::DisplayRole && o == Qt::Horizontal && s >= 0 && s < names.size())
        return names[s];
    return QAbstractTableModel::headerData(s, o, role);
}

BoundedLogModel::BoundedLogModel(int capacity, QObject *parent)
    : QAbstractListModel(parent), m_capacity(qMax(1, capacity))
{
}
int BoundedLogModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}
QVariant BoundedLogModel::data(const QModelIndex &i, int role) const
{
    if (!i.isValid() || i.row() < 0 || i.row() >= rowCount())
        return {};
    const auto &row = m_rows[i.row()];
    if (role == Qt::DisplayRole)
        return row.time +
               (row.warning ? QStringLiteral("  [警告] ") : QStringLiteral("  [信息] ")) + row.text;
    if (role == Qt::ToolTipRole)
        return row.text;
    if (role == Qt::ForegroundRole)
        return QBrush(QColor(row.warning ? "#b45309" : "#475569"));
    return {};
}
void BoundedLogModel::append(const QString &text, bool warning)
{
    if (m_rows.size() == m_capacity)
    {
        beginRemoveRows({}, 0, 0);
        m_rows.removeFirst();
        endRemoveRows();
    }
    const int next = m_rows.size();
    beginInsertRows({}, next, next);
    m_rows.append(
        {QDateTime::currentDateTime().toString("HH:mm:ss.zzz"), text.left(4096), warning});
    endInsertRows();
}
