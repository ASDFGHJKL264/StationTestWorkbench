#include "workbenchcontroller.h"
#include "runtimeconfiguration.h"
#include "databasemaintenance.h"
#include "adminaccess.h"
#include "repository.h"
#include "domain.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>

namespace
{
// 读取本次临时验证文件，供核对配置和原库是否完整保留。
QByteArray readBytes(const QString &path)
{
    // 打开需要比较的本地测试文件。
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// 创建仅用于故障注入或指向校验的临时文件。
bool writeBytes(const QString &path, const QByteArray &bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    // 写入本用例提供的固定内容。
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

// 查找指定目标的最新异步操作回报，忽略同时到达的自动备份结果。
QList<QVariant> resultForPath(const QSignalSpy &spy, const QString &path)
{
    // 保留最近一次匹配目标的完整信号参数。
    QList<QVariant> result;
    for (const auto &record : spy)
        if (!record.isEmpty() && record[0].toString() == path)
            result = record;
    return result;
}

// 判断控制器启动后的自动备份是否已完成，避免手工维护与其争用门禁。
bool automaticBackupCompleted(const QSignalSpy &spy, const QString &directory)
{
    for (const auto &record : spy)
        if (record.size() >= 3 && record[1].toBool() &&
            QFileInfo(record[0].toString()).absolutePath() == QDir(directory).absolutePath())
            return true;
    return false;
}

// 只读取得指定数据库中的一个文本字段，所有连接在返回前关闭。
QString databaseText(const QString &path, const QString &sql)
{
    // 使用独立名称，避免读取验证干扰控制器的数据库线程。
    const auto name = "maintenance-workflow-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    // 接收本次只读查询的首个字段。
    QString result;
    {
        // 为验证创建独立且只读的 SQLite 连接。
        auto database = QSqlDatabase::addDatabase("QSQLITE", name);
        database.setDatabaseName(path);
        database.setConnectOptions("QSQLITE_OPEN_READONLY");
        if (database.open())
        {
            {
                // 在连接关闭前释放查询对象。
                QSqlQuery query(database);
                if (query.exec(sql) && query.next())
                    result = query.value(0).toString();
            }
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(name);
    return result;
}

// 用真实仓储流程生成带配方快照和测量步骤的备份测试数据。
bool seedHistory(const QString &directory)
{
    // 从正式内置规则生成一次有效完成结果。
    const auto recipe = Domain::defaultRecipe();
    // 固定运行身份便于恢复后核对，内容不依赖数据库模式版本。
    const QJsonObject run{{"runId", "maintenance-source-run"},
                          {"stationId", 1},
                          {"sn", "MAINTENANCE-SN-001"},
                          {"recipeVersion", recipe["version"]},
                          {"recipe", recipe}};
    // 按配方顺序收集有效测量值。
    QJsonArray values;
    for (const auto &value : recipe["steps"].toArray())
    {
        // 取出该步骤的稳定身份和标称测量值。
        const auto step = value.toObject();
        values.append(QJsonObject{{"stepId", step["id"]}, {"value", step["nominal"]}});
    }
    // 使用独立仓储创建模式并提交测试身份及终态。
    Repository repository;
    // 观察真实初始化、预约和结果提交是否全部成功。
    QSignalSpy initialized(&repository, &Repository::initialized),
        reserved(&repository, &Repository::runReserved),
        saved(&repository, &Repository::resultSaved);
    repository.initialize(QDir(directory).filePath("data/workbench.db"));
    if (initialized.isEmpty() || !initialized.last()[0].toBool())
        return false;
    repository.reserveRun(run);
    if (reserved.isEmpty() || !reserved.last()[1].toBool())
        return false;
    repository.saveResult(QJsonObject{{"runId", run["runId"]},
                                      {"stationId", 1},
                                      {"sn", run["sn"]},
                                      {"recipeVersion", run["recipeVersion"]},
                                      {"outcome", "completed"},
                                      {"steps", values}});
    return !saved.isEmpty() && saved.last()[1].toBool();
}
} // namespace

class MaintenanceWorkflowTests final : public QObject
{
    Q_OBJECT
  private slots:
    // 验证所有维护入口均在未登录时拒绝操作，并保留运行期限和数据。
    void unauthenticatedMaintenanceRequestsAreRejected()
    {
        // 隔离本轮控制器、备份和恢复输出。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 使用无工位配置，只验证维护权限及线程协调。
        WorkbenchController controller;
        // 收集每种操作的权限拒绝回报。
        QSignalSpy ready(&controller, &WorkbenchController::ready),
            backup(&controller, &WorkbenchController::backupFinished),
            validation(&controller, &WorkbenchController::validationFinished),
            restore(&controller, &WorkbenchController::restoreFinished),
            messages(&controller, &WorkbenchController::maintenanceMessage),
            restarted(&controller, &WorkbenchController::restartRequested);
        controller.initialize(directory.path(), {});
        QTRY_VERIFY(!ready.isEmpty());
        QVERIFY(ready.last()[0].toBool());
        QVERIFY(!controller.adminAuthorized());
        // 各自使用不存在的目标，以确认拒绝发生在任何写文件前。
        const auto backupPath = directory.filePath("manual-backup.sqlite");
        // 保存恢复到的新目录位置，未认证时不得创建它。
        const auto restoredPath = directory.filePath("restored");
        // 记录期限配置原文，拒绝设置后应逐字节不变。
        const auto settingsPath = directory.filePath("maintenance/runtime.json");
        // 保存修改请求前的配置文件内容。
        const auto originalSettings = readBytes(settingsPath);
        controller.backupDatabase(backupPath);
        QVERIFY(!resultForPath(backup, backupPath).isEmpty());
        QVERIFY(!resultForPath(backup, backupPath)[1].toBool());
        controller.validateDatabaseBackup(backupPath);
        QVERIFY(!resultForPath(validation, backupPath).isEmpty());
        QVERIFY(!resultForPath(validation, backupPath)[1].toBool());
        controller.restoreDatabase(backupPath, restoredPath);
        QVERIFY(!resultForPath(restore, restoredPath).isEmpty());
        QVERIFY(!resultForPath(restore, restoredPath)[1].toBool());
        // 逐个检查没有单独完成信号的维护请求也明确拒绝。
        int messageCount = messages.size();
        controller.resolveException(1, "unavailable-run",
                                    QStringLiteral("设备已断电，申请人工处置"), true);
        QCOMPARE(messages.size(), ++messageCount);
        QVERIFY(messages.last()[1].toBool());
        controller.resumeStation(1, QStringLiteral("已确认设备完成复位"), true);
        QCOMPARE(messages.size(), ++messageCount);
        QVERIFY(messages.last()[1].toBool());
        controller.setExecutionTimeouts(900, 90);
        QCOMPARE(messages.size(), ++messageCount);
        QVERIFY(messages.last()[1].toBool());
        controller.activateRestoredDatabase(restoredPath);
        QCOMPARE(messages.size(), ++messageCount);
        QVERIFY(messages.last()[1].toBool());
        QCOMPARE(readBytes(settingsPath), originalSettings);
        QVERIFY(!QFileInfo::exists(backupPath));
        QVERIFY(!QFileInfo::exists(restoredPath));
        QCOMPARE(restarted.size(), 0);
        controller.shutdown();
    }

    // 验证认证后的异步备份、恢复、设置迁移和启动指向均连通。
    void authenticatedBackupRestoreAndActivationPreserveOriginalData()
    {
        // 用独立临时根目录保留原库和恢复库，便于同时核对。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 保存本次原始工作台的数据根目录。
        const auto original = directory.filePath("original");
        QVERIFY(seedHistory(original));
        // 操作备份源工作台的实际维护控制器。
        WorkbenchController controller;
        // 观察启动及各项异步维护结果。
        QSignalSpy ready(&controller, &WorkbenchController::ready),
            backup(&controller, &WorkbenchController::backupFinished),
            validation(&controller, &WorkbenchController::validationFinished),
            restore(&controller, &WorkbenchController::restoreFinished),
            restarted(&controller, &WorkbenchController::restartRequested),
            messages(&controller, &WorkbenchController::maintenanceMessage);
        controller.initialize(original, {});
        QTRY_VERIFY(!ready.isEmpty());
        QVERIFY(ready.last()[0].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(
            automaticBackupCompleted(backup, QDir(original).filePath("backups")), 15000);
        // 接收维护调用或配置检查的错误原因。
        QString error;
        // 固定演示凭据仅供临时测试，不能作为产品默认密码。
        const QString password = QStringLiteral("Only-for-tests-2026");
        QVERIFY2(controller.configureAdministrator("maintenance-admin", password, password, &error),
                 qPrintable(error));
        QVERIFY(controller.adminAuthorized());
        QCOMPARE(controller.adminActor(), QString("maintenance-admin"));
        controller.setExecutionTimeouts(321, 37);
        // 核对期限已保存，后续恢复必须复制同一配置。
        const auto limits = RuntimeConfiguration::loadLimits(original, &error);
        QCOMPARE(limits.runSeconds, 321);
        QCOMPARE(limits.stepSeconds, 37);
        // 核对工位连接配置随恢复的数据目录一起迁移。
        const QList<StationEndpoint> connections{{1, "192.0.2.10", 46501}};
        QVERIFY2(RuntimeConfiguration::saveStationEndpoints(original, connections, &error),
                 qPrintable(error));
        // 保留完整凭据摘要，以确认迁移没有重新生成管理员身份。
        const auto credentials = readBytes(QDir(original).filePath("maintenance/admin-auth.json"));
        QVERIFY(!credentials.isEmpty());
        QVERIFY(!credentials.contains(password.toUtf8()));
        // 手工备份与自动备份分开命名，便于关联信号及核对快照。
        const auto backupPath = directory.filePath("manual-snapshot.sqlite");
        controller.backupDatabase(backupPath);
        QTRY_VERIFY_WITH_TIMEOUT(!resultForPath(backup, backupPath).isEmpty(), 15000);
        QVERIFY2(resultForPath(backup, backupPath)[1].toBool(),
                 qPrintable(resultForPath(backup, backupPath)[2].toString()));
        controller.validateDatabaseBackup(backupPath);
        QTRY_VERIFY_WITH_TIMEOUT(!resultForPath(validation, backupPath).isEmpty(), 15000);
        QVERIFY2(resultForPath(validation, backupPath)[1].toBool(),
                 qPrintable(resultForPath(validation, backupPath)[2].toString()));
        // 恢复到尚不存在的新目录，原库不参与覆盖。
        const auto restored = directory.filePath("restored");
        controller.restoreDatabase(backupPath, restored);
        QTRY_VERIFY_WITH_TIMEOUT(!resultForPath(restore, restored).isEmpty(), 15000);
        QVERIFY2(resultForPath(restore, restored)[1].toBool(),
                 qPrintable(resultForPath(restore, restored)[2].toString()));
        QCOMPARE(readBytes(QDir(restored).filePath("maintenance/admin-auth.json")), credentials);
        QCOMPARE(readBytes(QDir(restored).filePath("maintenance/runtime.json")),
                 readBytes(QDir(original).filePath("maintenance/runtime.json")));
        QCOMPARE(readBytes(QDir(restored).filePath("maintenance/station-connections.json")),
                 readBytes(QDir(original).filePath("maintenance/station-connections.json")));
        // 用恢复目录中的凭据真实登录，而非仅比较文件存在性。
        AdminAccess restoredAccess(QDir(restored).filePath("maintenance/admin-auth.json"));
        QVERIFY2(restoredAccess.login("maintenance-admin", password, &error), qPrintable(error));
        QCOMPARE(restoredAccess.actor(), QString("maintenance-admin"));
        QCOMPARE(databaseText(QDir(restored).filePath("data/workbench.db"),
                              "SELECT sn FROM runs WHERE run_id='maintenance-source-run'"),
                 QString("MAINTENANCE-SN-001"));
        QCOMPARE(databaseText(QDir(restored).filePath("data/workbench.db"),
                              "SELECT COUNT(*) FROM steps WHERE run_id='maintenance-source-run'"),
                 QString::number(Domain::defaultRecipe()["steps"].toArray().size()));
        controller.activateRestoredDatabase(restored);
        QTRY_COMPARE_WITH_TIMEOUT(restarted.size(), 1, 5000);
        QCOMPARE(QFileInfo(restarted.first()[0].toString()).canonicalFilePath(),
                 QFileInfo(restored).canonicalFilePath());
        error.clear();
        QCOMPARE(QFileInfo(RuntimeConfiguration::resolveDataDirectory(original, &error))
                     .canonicalFilePath(),
                 QFileInfo(restored).canonicalFilePath());
        QVERIFY2(error.isEmpty(), qPrintable(error));
        controller.shutdown();
        QVERIFY(QFileInfo::exists(QDir(original).filePath("data/workbench.db")));
        QCOMPARE(databaseText(QDir(original).filePath("data/workbench.db"),
                              "SELECT sn FROM runs WHERE run_id='maintenance-source-run'"),
                 QString("MAINTENANCE-SN-001"));
        for (const auto &record : messages)
            QVERIFY(!record[0].toString().contains(password));
    }

    // 验证原库损坏导致初始化失败时，独立管理员和恢复入口仍可使用。
    void damagedCurrentDatabaseCanStillAuthenticateValidateAndRecover()
    {
        // 分别保存健康快照、损坏原库和新恢复目录。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 使用实际仓储生成可验证的备份来源。
        const auto healthy = directory.filePath("healthy");
        QVERIFY(seedHistory(healthy));
        // 通过正式维护类生成独立快照，不直接复制在线数据库文件。
        DatabaseMaintenance worker;
        // 收集同步夹具备份的实际完成结果。
        QSignalSpy snapshot(&worker, &DatabaseMaintenance::backupFinished);
        // 保存后续校验和恢复所使用的健康快照路径。
        const auto backupPath = directory.filePath("known-good.sqlite");
        worker.backupDatabase(QDir(healthy).filePath("data/workbench.db"), backupPath);
        QCOMPARE(snapshot.size(), 1);
        QVERIFY2(snapshot.first()[1].toBool(), qPrintable(snapshot.first()[2].toString()));
        // 原数据目录的管理员配置独立于损坏的业务数据库。
        const auto original = directory.filePath("damaged");
        // 固定测试密码只写入内存，凭据文件必须保存推导摘要。
        const QString password = QStringLiteral("Recovery-test-only-2026");
        // 在数据库损坏前模拟已设置的本机管理员。
        AdminAccess originalAccess(QDir(original).filePath("maintenance/admin-auth.json"));
        // 接收认证及运行配置保存失败原因。
        QString error;
        QVERIFY2(originalAccess.configure("recovery-admin", password, password, &error),
                 qPrintable(error));
        originalAccess.logout();
        QVERIFY(RuntimeConfiguration::saveLimits(original, {600, 45}, &error));
        // 用固定无效文件模拟无法被 SQLite 打开的原库。
        const QByteArray damagedBytes("Not a SQLite database: recovery workflow fixture\n");
        QVERIFY(writeBytes(QDir(original).filePath("data/workbench.db"), damagedBytes));
        // 即使数据库初始化失败，维护线程和本机认证对象也必须存在。
        WorkbenchController controller;
        // 分别收集原库失败、备份校验、恢复和切换请求。
        QSignalSpy ready(&controller, &WorkbenchController::ready),
            validation(&controller, &WorkbenchController::validationFinished),
            restore(&controller, &WorkbenchController::restoreFinished),
            restarted(&controller, &WorkbenchController::restartRequested);
        controller.initialize(original, {});
        QTRY_VERIFY(!ready.isEmpty());
        QVERIFY(!ready.last()[0].toBool());
        QVERIFY(controller.adminConfigured());
        QVERIFY(!controller.adminAuthorized());
        QVERIFY2(controller.loginAdministrator("recovery-admin", password, &error),
                 qPrintable(error));
        controller.validateDatabaseBackup(backupPath);
        QTRY_VERIFY_WITH_TIMEOUT(!resultForPath(validation, backupPath).isEmpty(), 15000);
        QVERIFY2(resultForPath(validation, backupPath)[1].toBool(),
                 qPrintable(resultForPath(validation, backupPath)[2].toString()));
        // 新目录必须从健康快照生成，损坏文件保留供回退或排查。
        const auto restored = directory.filePath("recovered-from-damage");
        controller.restoreDatabase(backupPath, restored);
        QTRY_VERIFY_WITH_TIMEOUT(!resultForPath(restore, restored).isEmpty(), 15000);
        QVERIFY2(resultForPath(restore, restored)[1].toBool(),
                 qPrintable(resultForPath(restore, restored)[2].toString()));
        // 校验恢复库沿用了原管理员密码。
        AdminAccess restoredAccess(QDir(restored).filePath("maintenance/admin-auth.json"));
        QVERIFY2(restoredAccess.login("recovery-admin", password, &error), qPrintable(error));
        // 读取恢复后的运行配置，确认维护设置未因业务库损坏而丢失。
        const auto limits = RuntimeConfiguration::loadLimits(restored, &error);
        QCOMPARE(limits.runSeconds, 600);
        QCOMPARE(limits.stepSeconds, 45);
        controller.activateRestoredDatabase(restored);
        QTRY_COMPARE_WITH_TIMEOUT(restarted.size(), 1, 5000);
        // 原库损坏时，恢复来源仍须由新目录保存操作者和来源身份。
        const auto provenance =
            QJsonDocument::fromJson(
                readBytes(QDir(restored).filePath("maintenance/restored-from.json")))
                .object();
        QCOMPARE(provenance["actor"].toString(), QString("recovery-admin"));
        QCOMPARE(QFileInfo(provenance["sourceDirectory"].toString()).canonicalFilePath(),
                 QFileInfo(original).canonicalFilePath());
        QVERIFY(!QJsonDocument(provenance).toJson().contains(password.toUtf8()));
        error.clear();
        QCOMPARE(QFileInfo(RuntimeConfiguration::resolveDataDirectory(original, &error))
                     .canonicalFilePath(),
                 QFileInfo(restored).canonicalFilePath());
        QVERIFY2(error.isEmpty(), qPrintable(error));
        controller.shutdown();
        QCOMPARE(readBytes(QDir(original).filePath("data/workbench.db")), damagedBytes);
        // 再启动恢复后的目录，确认它确实可作为工作台主库使用。
        WorkbenchController recovered;
        // 检查新库的真实初始化结果。
        QSignalSpy recoveredReady(&recovered, &WorkbenchController::ready);
        recovered.initialize(restored, {});
        QTRY_VERIFY(!recoveredReady.isEmpty());
        QVERIFY2(recoveredReady.last()[0].toBool(), "Restored database failed to initialize");
        QVERIFY2(recovered.loginAdministrator("recovery-admin", password, &error),
                 qPrintable(error));
        recovered.shutdown();
        QVERIFY(databaseText(QDir(restored).filePath("data/workbench.db"),
                             "SELECT reason FROM operational_events WHERE "
                             "action='RESTORED_DATABASE_OPENED' ORDER BY id DESC LIMIT 1")
                    .contains("recovery-admin"));
    }

    // 验证连接配置重读、地址规范化及非法保存不破坏原配置。
    void stationConnectionsPersistAndRejectInvalidEdits()
    {
        // 隔离当前连接配置及无效输入产生的文件。
        QTemporaryDir directory;
        // 模拟桌面启动参数提供的初始地址。
        const QList<StationEndpoint> fallback{{1, "127.0.0.1", 46501}};
        // 接收连接配置验证错误。
        QString error;
        QCOMPARE(RuntimeConfiguration::loadStationEndpoints(directory.path(), fallback, &error)
                     .first()
                     .port,
                 quint16(46501));
        QVERIFY(error.isEmpty());
        // 使用三个不同目标验证IPv4、IPv6和主机名。
        const QList<StationEndpoint> requested{
            {1, " 192.0.2.10 ", 1}, {2, "2001:db8::2", 65535}, {3, "STATION-3.Example.", 46503}};
        QVERIFY2(RuntimeConfiguration::saveStationEndpoints(directory.path(), requested, &error),
                 qPrintable(error));
        // 验证已保存的配置优先于启动参数。
        const auto loaded =
            RuntimeConfiguration::loadStationEndpoints(directory.path(), fallback, &error);
        QCOMPARE(loaded.size(), 3);
        QCOMPARE(loaded[0].host, QString("192.0.2.10"));
        QCOMPARE(loaded[0].port, quint16(1));
        QCOMPARE(loaded[1].host, QString("2001:db8::2"));
        QCOMPARE(loaded[1].port, quint16(65535));
        QCOMPARE(loaded[2].host, QString("station-3.example"));
        // 记录有效文件，所有失败保存都必须保留这些内容。
        const auto path = directory.filePath("maintenance/station-connections.json");
        // 保留可用配置的完整字节以核对原子写入边界。
        const auto original = readBytes(path);
        for (const auto &invalid : QList<QList<StationEndpoint>>{
                 {},
                 {{1, "", 46501}},
                 {{1, "999.1.1.1", 46501}},
                 {{1, "http://127.0.0.1", 46501}},
                 {{1, "0.0.0.0", 46501}},
                 {{1, "127.0.0.1", 0}},
                 {{2, "127.0.0.1", 46501}},
                 {{1, "127.0.0.1", 46501}, {2, "::ffff:127.0.0.1", 46501}},
                 {{1, "Station.Example.", 46501}, {2, "station.example", 46501}}})
        {
            QVERIFY(!RuntimeConfiguration::saveStationEndpoints(directory.path(), invalid, &error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(readBytes(path), original);
        }
        // 模拟数据目录路径被普通文件占用，确认保存失败可以反馈。
        const auto blocked = directory.filePath("blocked");
        QVERIFY(writeBytes(blocked, "existing file"));
        QVERIFY(!RuntimeConfiguration::saveStationEndpoints(blocked, requested, &error));
        QVERIFY(!error.isEmpty());
    }

    // 验证损坏、未来版本和端口类型错误不会悄悄连接启动参数中的其他设备。
    void brokenStationConnectionsNeverFallBackToAnotherTarget()
    {
        // 保存本次故障注入产生的设置文件。
        QTemporaryDir directory;
        // 提供不应在配置损坏时自动启用的连接。
        const QList<StationEndpoint> fallback{{1, "127.0.0.1", 46501}};
        // 定位需要注入无效内容的连接配置。
        const auto path = directory.filePath("maintenance/station-connections.json");
        // 接收每种坏配置的拒绝原因。
        QString error;
        for (const auto &bytes :
             {QByteArray("not json"), QByteArray("{\"version\":2,\"stations\":[]}"),
              QByteArray("{\"version\":1,\"stations\":[]}"), QByteArray(65537, 'x')})
        {
            QVERIFY(writeBytes(path, bytes));
            QVERIFY(RuntimeConfiguration::loadStationEndpoints(directory.path(), fallback, &error)
                        .isEmpty());
            QVERIFY(!error.isEmpty());
        }
        for (const auto &port : QJsonArray{0, -1, 65536, 1.5, "46501"})
        {
            // 构造格式合法但端口取值无效的文件。
            const QJsonObject object{
                {"version", 1},
                {"stations",
                 QJsonArray{QJsonObject{{"stationId", 1}, {"host", "127.0.0.1"}, {"port", port}}}}};
            QVERIFY(writeBytes(path, QJsonDocument(object).toJson()));
            QVERIFY(RuntimeConfiguration::loadStationEndpoints(directory.path(), fallback, &error)
                        .isEmpty());
            QVERIFY(!error.isEmpty());
        }
    }

    // 验证期限范围、损坏配置默认值，以及恢复目录的存在性和循环保护。
    void runtimeConfigurationBoundsAndRestorePointersRejectInvalidState()
    {
        // 隔离配置文件和目录切换记录。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 接收运行配置或指向校验的失败原因。
        QString error;
        // 不存在设置时使用安全的默认期限。
        const auto defaults = RuntimeConfiguration::loadLimits(directory.path(), &error);
        QCOMPARE(defaults.runSeconds, 120);
        QCOMPARE(defaults.stepSeconds, 30);
        QVERIFY(error.isEmpty());
        QVERIFY(RuntimeConfiguration::saveLimits(directory.path(), {1, 1}, &error));
        QVERIFY(RuntimeConfiguration::saveLimits(directory.path(), {86400, 86400}, &error));
        QVERIFY(RuntimeConfiguration::saveLimits(directory.path(), {300, 30}, &error));
        // 保存有效配置内容，拒绝非法设置时不能覆盖它。
        const auto settingsPath = directory.filePath("maintenance/runtime.json");
        // 保留有效配置的字节快照。
        const auto validSettings = readBytes(settingsPath);
        for (const auto &limits :
             QList<RuntimeConfiguration::ExecutionLimits>{{0, 1}, {86401, 1}, {20, 21}, {20, 0}})
        {
            error.clear();
            QVERIFY(!RuntimeConfiguration::saveLimits(directory.path(), limits, &error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(readBytes(settingsPath), validSettings);
        }
        QVERIFY(writeBytes(settingsPath, "{broken runtime settings"));
        error.clear();
        // 损坏设置只回退内存默认值，不能悄悄覆盖原文件。
        const auto corrupted = RuntimeConfiguration::loadLimits(directory.path(), &error);
        QCOMPARE(corrupted.runSeconds, 120);
        QCOMPARE(corrupted.stepSeconds, 30);
        QVERIFY(!error.isEmpty());
        QCOMPARE(readBytes(settingsPath), QByteArray("{broken runtime settings"));
        QVERIFY(writeBytes(settingsPath, QJsonDocument(QJsonObject{{"version", 1},
                                                                   {"runTimeoutSeconds", 30.5},
                                                                   {"stepTimeoutSeconds", 5}})
                                             .toJson()));
        error.clear();
        QCOMPARE(RuntimeConfiguration::loadLimits(directory.path(), &error).runSeconds, 120);
        QVERIFY(!error.isEmpty());
        // 目录指向校验只需要真实存在的目标库路径，不替代恢复前的数据库验证。
        const auto first = directory.filePath("first");
        // 保存第二个目录，便于构造双向循环。
        const auto second = directory.filePath("second");
        // 保存存在但尚无数据库的无效目标目录。
        const auto empty = directory.filePath("empty");
        QVERIFY(writeBytes(QDir(first).filePath("data/workbench.db"), "placeholder"));
        QVERIFY(writeBytes(QDir(second).filePath("data/workbench.db"), "placeholder"));
        QVERIFY(QDir().mkpath(empty));
        QVERIFY(!RuntimeConfiguration::activateDataDirectory(first, first, &error));
        QVERIFY(!RuntimeConfiguration::activateDataDirectory(first, empty, &error));
        QVERIFY(!RuntimeConfiguration::activateDataDirectory(first, directory.filePath("missing"),
                                                             &error));
        error.clear();
        QVERIFY2(RuntimeConfiguration::activateDataDirectory(first, second, &error),
                 qPrintable(error));
        QCOMPARE(QFileInfo(RuntimeConfiguration::resolveDataDirectory(first, &error))
                     .canonicalFilePath(),
                 QFileInfo(second).canonicalFilePath());
        // 手写第二条指向形成环，验证启动时不会无限跟随。
        const QJsonObject loop{{"version", 1},
                               {"activeDataDirectory", QFileInfo(first).canonicalFilePath()}};
        QVERIFY(writeBytes(QDir(second).filePath("maintenance/active-data-dir.json"),
                           QJsonDocument(loop).toJson()));
        error.clear();
        QVERIFY(RuntimeConfiguration::resolveDataDirectory(first, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(writeBytes(QDir(first).filePath("maintenance/active-data-dir.json"), "not json"));
        error.clear();
        QVERIFY(RuntimeConfiguration::resolveDataDirectory(first, &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }
};

QTEST_GUILESS_MAIN(MaintenanceWorkflowTests)
#include "test_maintenance_workflow.moc"
