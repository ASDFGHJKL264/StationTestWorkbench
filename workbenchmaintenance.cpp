#include "workbenchcontroller.h"
#include "adminaccess.h"
#include "databasemaintenance.h"
#include "testengine.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#include <QTimer>
#include <QUuid>

namespace
{
// 复制经过认证的本地维护配置，原目录和原文件保持完整。
bool copyMaintenanceSettings(const QString &source, const QString &target, QString *error)
{
    if (!QDir().mkpath(QDir(target).filePath("maintenance")))
    {
        *error = QStringLiteral("无法创建恢复目录的维护设置");
        return false;
    }
    for (const auto &name : {"admin-auth.json", "runtime.json", "station-connections.json"})
    {
        // 打开已有管理员、运行期限或工位连接配置。
        QFile input(QDir(source).filePath(QStringLiteral("maintenance/") + name));
        if (QString::fromLatin1(name) == "admin-auth.json" &&
            (!QFileInfo(input).isFile() || QFileInfo(input).isSymLink()))
        {
            *error = QStringLiteral("管理员凭据已缺失或路径异常，未切换数据目录");
            return false;
        }
        if (!input.exists())
            continue;
        if (!input.open(QIODevice::ReadOnly) || input.size() > 65536)
        {
            *error = QStringLiteral("无法读取本地维护配置");
            return false;
        }
        // 原子写入新目录，防止遗留半份认证配置。
        QSaveFile output(QDir(target).filePath(QStringLiteral("maintenance/") + name));
        output.setDirectWriteFallback(false);
        // 保存需要迁移的维护配置内容，绝不写入日志。
        const auto bytes = input.readAll();
        if (!output.open(QIODevice::WriteOnly) ||
            !output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
            output.write(bytes) != bytes.size() || !output.commit())
        {
            *error = QStringLiteral("恢复库已生成，但维护配置复制失败：") + output.errorString();
            return false;
        }
    }
    return true;
}
// 独占目标目录，先保留恢复来源再原子切换启动指向。
bool activateRecoveredData(const QString &source, const QString &target, const QString &actor,
                           QString *error)
{
    // 防止切换到正在被其他工作台使用的目标库。
    QLockFile lock(QDir(target).filePath(".workbench.lock"));
    if (!lock.tryLock(0))
    {
        *error = QStringLiteral("恢复目录正在被其他工作台使用");
        return false;
    }
    if (!copyMaintenanceSettings(source, target, error))
        return false;
    // 即使原库损坏，新库启动后也能追溯恢复操作者。
    QSaveFile marker(QDir(target).filePath("maintenance/restored-from.json"));
    marker.setDirectWriteFallback(false);
    // 记录账号、来源和切换时间，不包含任何认证材料。
    const auto metadata =
        QJsonDocument(QJsonObject{{"version", 1},
                                  {"actor", actor},
                                  {"sourceDirectory", source},
                                  {"activatedAt",
                                   QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}})
            .toJson(QJsonDocument::Compact);
    if (!marker.open(QIODevice::WriteOnly) || marker.write(metadata) != metadata.size() ||
        !marker.commit())
    {
        *error = QStringLiteral("无法保存恢复来源，未切换当前数据：") + marker.errorString();
        return false;
    }
    return RuntimeConfiguration::activateDataDirectory(source, target, error);
}
} // namespace

bool WorkbenchController::adminConfigured() const
{
    return m_adminAccess && m_adminAccess->isConfigured();
}
bool WorkbenchController::adminAuthorized() const
{
    return m_adminAccess && m_adminAccess->isAuthorized();
}
QString WorkbenchController::adminActor() const
{
    return m_adminAccess ? m_adminAccess->actor() : QString();
}
bool WorkbenchController::configureAdministrator(const QString &user, const QString &password,
                                                 const QString &confirmation, QString *error)
{
    if (!m_adminAccess || m_stopping)
    {
        if (error)
            *error = QStringLiteral("维护模块尚未就绪");
        return false;
    }
    // 接收认证失败原因，避免将密码拼入任何日志。
    QString failure;
    // 保存首次管理员设置是否成功。
    const bool ok = m_adminAccess->configure(user, password, confirmation, &failure);
    if (ok)
        audit(adminActor(), "ADMIN_CONFIGURED", QStringLiteral("首次设置本机管理员"));
    if (error)
        *error = failure;
    return ok;
}
bool WorkbenchController::loginAdministrator(const QString &user, const QString &password,
                                             QString *error)
{
    if (!m_adminAccess || m_stopping)
    {
        if (error)
            *error = QStringLiteral("维护模块尚未就绪");
        return false;
    }
    // 接收账号校验结果，不记录任何认证材料。
    QString failure;
    // 保存本次管理员认证是否成功。
    const bool ok = m_adminAccess->login(user, password, &failure);
    audit(ok ? adminActor() : QStringLiteral("未认证"), ok ? "LOGIN" : "LOGIN_FAILED",
          ok ? QStringLiteral("管理员登录") : QStringLiteral("管理员认证未通过"));
    if (error)
        *error = failure;
    return ok;
}
void WorkbenchController::logoutAdministrator()
{
    if (m_adminAccess)
    {
        if (adminAuthorized())
            audit(adminActor(), "LOGOUT", QStringLiteral("管理员退出登录"));
        m_adminAccess->logout();
    }
}
bool WorkbenchController::requireAdministrator(QString *error, bool requireDatabase) const
{
    if (!m_started || (requireDatabase && !m_ready) || m_stopping)
        *error = QStringLiteral("后台尚未就绪");
    else if (!adminAuthorized())
        *error = QStringLiteral("请先登录管理员；登录有效期为5分钟");
    else
        return true;
    return false;
}
void WorkbenchController::maintenanceNotice(const QString &message, bool error)
{
    emit maintenanceMessage(message, error);
    log(message, error);
}
void WorkbenchController::audit(const QString &actor, const QString &action, const QString &reason)
{
    if (m_ready && !m_stopping)
        QMetaObject::invokeMethod(m_repository, [r = m_repository, actor, action, reason]
                                  { r->recordAudit(actor, action, {}, 0, reason); });
}

void WorkbenchController::initializeMaintenance()
{
    m_adminAccess = std::make_unique<AdminAccess>(
        QDir(m_dataDirectory).filePath("maintenance/admin-auth.json"));
    m_maintenance = new DatabaseMaintenance;
    m_maintenanceThread.setObjectName("WorkbenchMaintenanceThread");
    m_maintenance->moveToThread(&m_maintenanceThread);
    connect(&m_maintenanceThread, &QThread::finished, m_maintenance, &QObject::deleteLater);
    connect(m_repository, &Repository::safetyStateReady, this,
            [this](const QJsonArray &closed, const QJsonArray &suspended)
            {
                m_closedRuns = closed;
                m_suspendedStations = suspended;
            });
    connect(m_engine, &TestEngine::runTimedOut, m_repository, &Repository::recordTimeout);
    connect(m_engine, &TestEngine::stationSuspensionRequired, m_repository,
            &Repository::recordStationSuspension);
    connect(m_repository, &Repository::maintenanceReady, this,
            [this](QJsonObject overview)
            {
                if (!adminAuthorized() || m_stopping)
                    return;
                overview["runTimeoutSeconds"] = m_executionLimits.runSeconds;
                overview["stepTimeoutSeconds"] = m_executionLimits.stepSeconds;
                overview["backupDirectory"] = m_backupDirectory;
                overview["dataDirectory"] = m_dataDirectory;
                overview["restoredDirectory"] = m_restoredDirectory;
                emit maintenanceReady(overview);
            });
    connect(m_engine, &TestEngine::manualDispositionPrepared, this,
            [this](int stationId, const QString &runId, bool ok, const QString &error)
            {
                if (m_stopping || !m_pendingDispositions.contains(runId))
                    return;
                if (!ok)
                {
                    m_pendingDispositions.remove(runId);
                    maintenanceNotice(error, true);
                    return;
                }
                // 保留发起动作时已经通过认证的处置身份和原因。
                const auto context = m_pendingDispositions.value(runId);
                QMetaObject::invokeMethod(
                    m_repository, [r = m_repository, stationId, runId, context]
                    { r->resolveManually(stationId, runId, context.actor, context.reason, true); });
            });
    connect(m_repository, &Repository::manualDispositionSaved, this,
            [this](int stationId, const QString &runId, bool ok, const QString &error)
            {
                if (m_stopping)
                    return;
                m_pendingDispositions.remove(runId);
                QMetaObject::invokeMethod(
                    m_engine, [e = m_engine, stationId, runId, ok, error]
                    { e->manualDispositionRecorded(stationId, runId, ok, error); });
                maintenanceNotice(
                    ok ? QStringLiteral("人工结算已记录，工位保持停用；请复位设备后再启用")
                       : QStringLiteral("人工结算失败，原任务仍保留：") + error,
                    !ok);
                refreshMaintenance();
                refreshFilteredHistory(m_historyFilter);
            });
    connect(m_repository, &Repository::stationResumed, this,
            [this](int stationId, bool ok, const QString &error)
            {
                if (m_stopping)
                    return;
                if (ok)
                    QMetaObject::invokeMethod(m_engine, [e = m_engine, stationId]
                                              { e->resumeStation(stationId); });
                maintenanceNotice(
                    ok ? QStringLiteral("工位%1已允许重新连接，旧运行仍受隔离保护").arg(stationId)
                       : QStringLiteral("启用工位失败：") + error,
                    !ok);
                refreshMaintenance();
            });
    connect(m_maintenance, &DatabaseMaintenance::backupFinished, this,
            [this](const QString &path, bool ok, const QString &error)
            {
                if (m_stopping)
                    return;
                m_backupInFlight = false;
                audit(m_backupActor, ok ? "BACKUP" : "BACKUP_FAILED", ok ? path : error);
                if (ok && m_automaticBackup)
                {
                    // 仅清理本程序生成的旧自动备份，不接触其他文件。
                    const QRegularExpression owned(
                        "^workbench-[0-9]{8}T[0-9]{9}Z-[0-9a-f]{8}\\.sqlite$");
                    // 按时间排序且属于本程序的备份文件。
                    QFileInfoList backups;
                    for (const auto &file :
                         QDir(m_backupDirectory).entryInfoList(QDir::Files, QDir::Time))
                        if (!file.isSymLink() && owned.match(file.fileName()).hasMatch())
                            backups.append(file);
                    for (int index = 7; index < backups.size(); ++index)
                        QFile::remove(backups[index].absoluteFilePath());
                }
                m_automaticBackup = false;
                maintenanceNotice(ok ? QStringLiteral("数据库一致性备份已完成：") + path
                                     : QStringLiteral("数据库备份失败：") + error,
                                  !ok);
                emit backupFinished(path, ok, error);
            });
    connect(m_maintenance, &DatabaseMaintenance::validationFinished, this,
            [this](const QString &path, bool ok, const QString &error)
            {
                if (m_stopping)
                    return;
                m_validationInFlight = false;
                emit validationFinished(path, ok, error);
            });
    connect(m_engine, &TestEngine::restorePrepared, this,
            [this](bool ok, const QString &error)
            {
                if (m_stopping || !m_restoreInFlight || m_switchingDatabase)
                    return;
                if (!ok)
                {
                    m_restoreInFlight = false;
                    emit restoreFinished(m_restoreTarget, false, error);
                    return;
                }
                QMetaObject::invokeMethod(
                    m_maintenance,
                    [worker = m_maintenance, source = m_restoreSource, target = m_restoreTarget]
                    { worker->restoreToDirectory(source, target); });
            });
    connect(
        m_maintenance, &DatabaseMaintenance::restoreFinished, this,
        [this](const QString &directory, bool ok, QString error)
        {
            if (m_stopping)
                return;
            if (ok)
            {
                // 复制设置期间保持恢复目录独占，避免另一客户端提前打开。
                QLockFile lock(QDir(directory).filePath(".workbench.lock"));
                ok = lock.tryLock(0) && copyMaintenanceSettings(m_dataDirectory, directory, &error);
                if (!ok && error.isEmpty())
                    error = QStringLiteral("恢复目录正在被另一程序使用");
            }
            m_restoreInFlight = false;
            if (ok)
                m_restoredDirectory = QFileInfo(directory).canonicalFilePath();
            QMetaObject::invokeMethod(m_engine, &TestEngine::finishRestorePause);
            audit(m_restoreActor, ok ? "RESTORE_STAGED" : "RESTORE_FAILED", ok ? directory : error);
            emit restoreFinished(directory, ok, error);
        });
    m_maintenanceThread.start();
    m_maintenanceTimer = new QTimer(this);
    m_maintenanceTimer->setInterval(60000);
    connect(m_maintenanceTimer, &QTimer::timeout, this,
            &WorkbenchController::checkAutomaticMaintenance);
    m_maintenanceTimer->start();
}

void WorkbenchController::refreshMaintenance()
{
    // 接收后台或管理员会话检查失败原因。
    QString error;
    if (!requireAdministrator(&error, false))
    {
        emit maintenanceMessage(error, true);
        return;
    }
    if (!m_ready)
    {
        // 原库损坏时仍显示本机备份位置，供管理员恢复到新目录。
        emit maintenanceReady(QJsonObject{{"exceptions", QJsonArray{}},
                                          {"audit", QJsonArray{}},
                                          {"suspendedStations", QJsonArray{}},
                                          {"runTimeoutSeconds", m_executionLimits.runSeconds},
                                          {"stepTimeoutSeconds", m_executionLimits.stepSeconds},
                                          {"backupDirectory", m_backupDirectory},
                                          {"dataDirectory", m_dataDirectory},
                                          {"restoredDirectory", m_restoredDirectory}});
        return;
    }
    QMetaObject::invokeMethod(m_repository, &Repository::queryMaintenance);
}
void WorkbenchController::backupDatabase(const QString &path)
{
    // 接收备份权限及忙状态错误。
    QString error;
    if (!requireAdministrator(&error) || m_backupInFlight || m_validationInFlight ||
        m_restoreInFlight)
    {
        emit backupFinished(path, false,
                            error.isEmpty() ? QStringLiteral("另一个维护任务正在进行") : error);
        return;
    }
    m_backupInFlight = true;
    m_backupActor = adminActor();
    m_automaticBackup = false;
    QMetaObject::invokeMethod(m_maintenance, [worker = m_maintenance, source = m_databasePath, path]
                              { worker->backupDatabase(source, path); });
}
void WorkbenchController::validateDatabaseBackup(const QString &path)
{
    // 接收备份验证权限或忙状态错误。
    QString error;
    if (!requireAdministrator(&error, false) || m_validationInFlight || m_backupInFlight ||
        m_restoreInFlight)
    {
        emit validationFinished(path, false,
                                error.isEmpty() ? QStringLiteral("另一个维护任务正在进行") : error);
        return;
    }
    m_validationInFlight = true;
    QMetaObject::invokeMethod(m_maintenance,
                              [worker = m_maintenance, path] { worker->validateBackup(path); });
}
void WorkbenchController::restoreDatabase(const QString &path, const QString &directory)
{
    // 接收恢复权限或在途维护任务的错误。
    QString error;
    if (!requireAdministrator(&error, false) || m_restoreInFlight || m_backupInFlight ||
        m_validationInFlight)
    {
        emit restoreFinished(directory, false,
                             error.isEmpty() ? QStringLiteral("请等待当前维护任务结束") : error);
        return;
    }
    m_restoredDirectory.clear();
    m_restoreInFlight = true;
    m_restoreSource = path;
    m_restoreTarget = directory;
    m_restoreActor = adminActor();
    QMetaObject::invokeMethod(m_engine, &TestEngine::prepareRestore);
}
void WorkbenchController::activateRestoredDatabase(const QString &directory)
{
    // 接收切换权限与目标校验错误。
    QString error;
    if (!requireAdministrator(&error, false) || m_restoreInFlight || m_backupInFlight ||
        m_restoredDirectory.isEmpty() ||
        QFileInfo(directory).canonicalFilePath() != m_restoredDirectory)
    {
        maintenanceNotice(
            error.isEmpty() ? QStringLiteral("请先完成本次备份恢复，再切换数据目录") : error, true);
        return;
    }
    // 此次切换仍须经过网络线程的原子闲置检查。
    m_restoreInFlight = true;
    m_switchingDatabase = true;
    // 保留提交动作时的已认证身份，会话之后到期也不会丢失追溯账号。
    const auto actor = adminActor();
    // 一次性等待门禁结果，随后立即断开临时连接。
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection =
        connect(m_engine, &TestEngine::restorePrepared, this,
                [this, directory, actor, connection](bool ok, QString failure)
                {
                    disconnect(*connection);
                    if (!ok)
                    {
                        m_restoreInFlight = false;
                        m_switchingDatabase = false;
                        maintenanceNotice(failure, true);
                        return;
                    }
                    if (!activateRecoveredData(m_dataDirectory, directory, actor, &failure))
                    {
                        m_restoreInFlight = false;
                        m_switchingDatabase = false;
                        QMetaObject::invokeMethod(m_engine, &TestEngine::finishRestorePause);
                        maintenanceNotice(failure, true);
                        return;
                    }
                    audit(actor, "RESTORE_ACTIVATED", directory);
                    emit restartRequested(directory);
                });
    QMetaObject::invokeMethod(m_engine, &TestEngine::prepareRestore);
}
void WorkbenchController::resolveException(int stationId, const QString &runId,
                                           const QString &reason, bool confirmedStopped)
{
    // 接收处置权限或输入条件的错误。
    QString error;
    if (!requireAdministrator(&error) || !confirmedStopped || reason.trimmed().size() < 5 ||
        reason.size() > 1000 || runId.isEmpty() || m_restoreInFlight ||
        m_pendingDispositions.contains(runId))
    {
        maintenanceNotice(error.isEmpty()
                              ? QStringLiteral("请确认设备已经停止，并填写5至1000字的处置原因")
                              : error,
                          true);
        return;
    }
    m_pendingDispositions.insert(runId, {adminActor(), reason.trimmed()});
    QMetaObject::invokeMethod(m_engine, [e = m_engine, stationId, runId]
                              { e->prepareManualDisposition(stationId, runId); });
}
void WorkbenchController::resumeStation(int stationId, const QString &reason, bool confirmedReset)
{
    // 接收启用工位的权限或复位确认错误。
    QString error;
    if (!requireAdministrator(&error) || !confirmedReset || reason.trimmed().size() < 5 ||
        reason.size() > 1000 || m_restoreInFlight ||
        !m_stationViews.value(stationId)["safetySuspended"].toBool())
    {
        maintenanceNotice(error.isEmpty()
                              ? QStringLiteral("请选择停用工位，确认旧设备已复位并填写启用原因")
                              : error,
                          true);
        return;
    }
    QMetaObject::invokeMethod(m_repository,
                              [r = m_repository, stationId, actor = adminActor(), reason]
                              { r->resumeStation(stationId, actor, reason.trimmed()); });
}
void WorkbenchController::setExecutionTimeouts(int runSeconds, int stepSeconds)
{
    // 接收期限设置的权限或写入错误。
    QString error;
    if (!requireAdministrator(&error))
    {
        maintenanceNotice(error, true);
        return;
    }
    // 保存校验通过后的新测试期限。
    const RuntimeConfiguration::ExecutionLimits limits{runSeconds, stepSeconds};
    if (!RuntimeConfiguration::saveLimits(m_dataDirectory, limits, &error))
    {
        maintenanceNotice(error, true);
        return;
    }
    m_executionLimits = limits;
    QMetaObject::invokeMethod(
        m_engine, [e = m_engine, limits]
        { e->configureTimeouts(limits.runSeconds * 1000LL, limits.stepSeconds * 1000LL); });
    audit(adminActor(), "TIMEOUT_SETTINGS",
          QStringLiteral("整轮%1秒；步骤%2秒；仅对新测试生效").arg(runSeconds).arg(stepSeconds));
    maintenanceNotice(QStringLiteral("测试时限已保存，将从下一次新测试开始生效"), false);
    refreshMaintenance();
}
void WorkbenchController::checkAutomaticMaintenance()
{
    if (!m_ready || m_stopping || m_restoreInFlight)
        return;
    // 获取当前数据盘的剩余空间，空间不足时限频提示。
    const QStorageInfo storage(m_dataDirectory);
    // 使用 UTC 毫秒为低空间警告限频。
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (storage.isValid() && storage.isReady() && storage.bytesAvailable() >= 0 &&
        storage.bytesAvailable() <
            qMax<qint64>(512LL * 1024 * 1024, QFileInfo(m_databasePath).size() * 2) &&
        now - m_lastSpaceWarning > 600000)
    {
        m_lastSpaceWarning = now;
        maintenanceNotice(QStringLiteral("数据盘剩余空间不足，请备份并检查磁盘容量"), true);
    }
    if (m_backupInFlight || m_validationInFlight)
        return;
    if (!QDir().mkpath(m_backupDirectory))
    {
        maintenanceNotice(QStringLiteral("无法创建自动备份目录"), true);
        return;
    }
    // 只识别本程序生成的自动备份名称。
    const QRegularExpression owned("^workbench-[0-9]{8}T[0-9]{9}Z-[0-9a-f]{8}\\.sqlite$");
    for (const auto &file : QDir(m_backupDirectory).entryInfoList(QDir::Files, QDir::Time))
        if (!file.isSymLink() && owned.match(file.fileName()).hasMatch() &&
            file.lastModified().date() == QDate::currentDate())
            return;
    // 为当天首次成功备份创建不可覆盖的唯一名称。
    const auto path =
        QDir(m_backupDirectory)
            .filePath("workbench-" +
                      QDateTime::currentDateTimeUtc().toString("yyyyMMdd'T'HHmmsszzz'Z'") + "-" +
                      QUuid::createUuid().toString(QUuid::Id128).left(8) + ".sqlite");
    m_backupInFlight = true;
    m_automaticBackup = true;
    m_backupActor = QStringLiteral("系统");
    QMetaObject::invokeMethod(m_maintenance, [worker = m_maintenance, source = m_databasePath, path]
                              { worker->backupDatabase(source, path); });
}
