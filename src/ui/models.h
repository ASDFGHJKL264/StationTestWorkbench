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
    explicit QualityDelegate(QObject *parent = nullptr);
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

class StationModel final : public QAbstractTableModel
{
    Q_OBJECT
  public:
    enum Role
    {
        StationIdRole = Qt::UserRole + 1,
        ObjectRole
    };
    explicit StationModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void updateStation(const QJsonObject &station);
    QJsonObject stationAt(int row) const;
    int rowForStation(int stationId) const;

  private:
    QList<QJsonObject> m_rows;
};

class StepModel final : public QAbstractTableModel
{
    Q_OBJECT
  public:
    explicit StepModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void setSteps(const QJsonArray &steps);

  private:
    QJsonArray m_rows;
};

class HistoryModel final : public QAbstractTableModel
{
    Q_OBJECT
  public:
    enum Role
    {
        SortRole = Qt::UserRole + 1
    };
    explicit HistoryModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void setRows(const QJsonArray &rows);
    QJsonObject recordAt(int row) const;
    int lastId() const;

  private:
    QJsonArray m_rows;
};

class BoundedLogModel final : public QAbstractListModel
{
    Q_OBJECT
  public:
    explicit BoundedLogModel(int capacity = 1000, QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    void append(const QString &message, bool warning = false);

  private:
    struct Entry
    {
        QString time, text;
        bool warning;
    };
    QList<Entry> m_rows;
    int m_capacity;
};
