#pragma once
#include <QObject>
#include <QString>

// 使用独立数据库连接执行维护，不占用实时结果归档队列。
class DatabaseMaintenance final : public QObject
{
    Q_OBJECT
  public:
    // 创建维护工作对象，由 parent 管理生命周期。
    explicit DatabaseMaintenance(QObject *parent = nullptr) : QObject(parent) {}
  public slots:
    // 从 sourcePath 生成一致快照，验证后发布到尚不存在的 destinationPath。
    void backupDatabase(const QString &sourcePath, const QString &destinationPath);
    // 只读检查 path 的结构、关联和测试数据，不修改被验证数据库。
    void validateBackup(const QString &path);
    // 将备份还原到新空目录的 data/workbench.db，保留原数据库。
    void restoreToDirectory(const QString &backupPath, const QString &newDataDirectory);
  signals:
    // 所有连接关闭后回报备份发布结果。
    void backupFinished(const QString &path, bool ok, const QString &error);
    // 所有连接关闭后回报备份验证结果。
    void validationFinished(const QString &path, bool ok, const QString &error);
    // 目标目录锁和连接释放后回报新目录恢复结果。
    void restoreFinished(const QString &newDataDirectory, bool ok, const QString &error);
};
