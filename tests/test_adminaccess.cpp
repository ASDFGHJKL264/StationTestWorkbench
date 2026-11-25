#include "adminaccess.h"
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace
{
// 提供仅用于自动验证的公开示例密码。
QString examplePassword()
{
    return QStringLiteral("Only-For-Admin-Tests-439!");
}

// 读取临时凭据文件，用于检查没有保存明文或被失败操作覆盖。
QByteArray readFile(const QString &path)
{
    // 保存本次验证需要读取的文件。
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// 写入损坏配置样本，验证认证失败时不会进行首次设置覆盖。
bool writeFile(const QString &path, const QByteArray &bytes)
{
    // 保存本次需要注入的配置样本。
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
} // namespace

class AdminAccessTests final : public QObject
{
    Q_OBJECT
  private slots:
    // 验证首次设置校验、加盐保存及重新加载后的登录。
    void configurationPersistsOnlySaltedVerifierAndRequiresExplicitLoginAfterReload()
    {
        // 隔离管理员配置，避免触及用户的实际维护凭据。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 保存本次首次设置使用的独立凭据路径。
        const auto path = directory.filePath("maintenance/admin-auth.json");
        // 保存可控制的单调时钟，避免等待真实会话期限。
        qint64 clock = 0;
        // 保存本次首次设置及会话验证对象。
        AdminAccess access(path, [&] { return clock; });
        // 收集用户可见的校验失败原因。
        QString error;
        QVERIFY(!access.isConfigured());
        QVERIFY(!access.isAuthorized());
        QVERIFY(access.actor().isEmpty());
        QVERIFY(!access.configure({}, examplePassword(), examplePassword(), &error));
        QVERIFY(!access.configure(" admin", examplePassword(), examplePassword(), &error));
        QVERIFY(!access.configure("admin", "short", "short", &error));
        QVERIFY(!access.configure("admin", examplePassword(), "different-password", &error));
        QVERIFY(!access.isConfigured());
        error = "stale-error";
        QVERIFY(access.configure("line-admin", examplePassword(), examplePassword(), &error));
        QVERIFY(error.isEmpty());
        QVERIFY(access.isConfigured());
        QVERIFY(access.isAuthorized());
        QCOMPARE(access.actor(), QString("line-admin"));
        // 保存发布到磁盘的实际凭据内容。
        const auto contents = readFile(path);
        QVERIFY(!contents.isEmpty());
        QVERIFY(!contents.contains(examplePassword().toUtf8()));
        QVERIFY(!contents.contains("password"));
        // 检查格式与推导参数，防止降为无盐或低成本摘要。
        const auto record = QJsonDocument::fromJson(contents).object();
        QCOMPARE(record.value("version").toInt(), 1);
        QCOMPARE(record.value("iterations").toInt(), 200000);
        QCOMPARE(QByteArray::fromBase64(record.value("salt").toString().toLatin1()).size(), 32);
        QCOMPARE(QByteArray::fromBase64(record.value("verifier").toString().toLatin1()).size(), 32);
        // 重新加载配置不会自动继承上一次进程中的管理员会话。
        AdminAccess reloaded(path, [&] { return clock; });
        QVERIFY(reloaded.isConfigured());
        QVERIFY(!reloaded.isAuthorized());
        QVERIFY(reloaded.login("line-admin", examplePassword(), &error));
        QCOMPARE(reloaded.actor(), QString("line-admin"));
        reloaded.logout();
        QVERIFY(!reloaded.isAuthorized());
        QVERIFY(reloaded.actor().isEmpty());
        QVERIFY(!reloaded.configure("replacement", examplePassword(), examplePassword(), &error));
        QCOMPARE(readFile(path), contents);
    }

    // 验证错误密码和用户名不会保留权限，连续尝试受到冷却限制。
    void failedLoginClearsSessionAndThrottlesRepeatedAttempts()
    {
        // 隔离本组登录失败验证使用的凭据。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 保存用于推进失败冷却的单调毫秒数。
        qint64 clock = 0;
        // 保存需要核验会话与失败冷却的认证对象。
        AdminAccess access(directory.filePath("admin.json"), [&] { return clock; });
        // 收集对外显示的失败信息。
        QString error;
        QVERIFY(access.configure("engineer", examplePassword(), examplePassword(), &error));
        QVERIFY(!access.login("engineer", "wrong-password", &error));
        QVERIFY(!access.isAuthorized());
        QVERIFY(access.actor().isEmpty());
        QVERIFY(!error.contains(examplePassword()));
        QVERIFY(!access.login("engineer", examplePassword(), &error));
        QVERIFY(error.contains(QStringLiteral("频繁")));
        clock = 1000;
        QVERIFY(!access.login("other-admin", examplePassword(), &error));
        QVERIFY(!access.login("engineer", examplePassword(), &error));
        clock = 2999;
        QVERIFY(!access.login("engineer", examplePassword(), &error));
        clock = 3000;
        QVERIFY(access.login("engineer", examplePassword(), &error));
        QVERIFY(access.isAuthorized());
        QVERIFY(error.isEmpty());
        QVERIFY(!access.login("engineer", QString(129, 'x'), &error));
        QVERIFY(!access.isAuthorized());
        clock += 1000;
        QVERIFY(access.login("engineer", examplePassword(), &error));
    }

    // 验证会话到五分钟精确失效，退出和时钟倒退均不能继续授权。
    void sessionExpiresAtFiveMinutesAndLogoutRevokesImmediately()
    {
        // 隔离本组会话期限验证的数据。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 控制会话起点与结束边界。
        qint64 clock = 6000;
        // 保存当前会话授权对象。
        AdminAccess access(directory.filePath("admin.json"), [&] { return clock; });
        QVERIFY(access.configure("admin", examplePassword(), examplePassword()));
        clock += 299999;
        QVERIFY(access.isAuthorized());
        QCOMPARE(access.actor(), QString("admin"));
        ++clock;
        QVERIFY(!access.isAuthorized());
        QVERIFY(access.actor().isEmpty());
        QVERIFY(access.login("admin", examplePassword()));
        --clock;
        QVERIFY(!access.isAuthorized());
        ++clock;
        QVERIFY(access.isAuthorized());
        access.logout();
        QVERIFY(!access.isAuthorized());
        clock += 1000000;
        QVERIFY(!access.isAuthorized());
    }

    // 验证损坏、未来版本及不合法的加密参数均不能被首次设置绕过。
    void invalidCredentialFilesFailClosedAndCannotBeOverwritten()
    {
        // 隔离原始有效凭据和所有损坏样本。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 保存生成有效配置时使用的文件路径。
        const auto originalPath = directory.filePath("valid.json");
        // 生成一个拥有正确盐和推导参数的格式基准。
        AdminAccess original(originalPath);
        QVERIFY(original.configure("admin", examplePassword(), examplePassword()));
        // 保存用于修改格式字段的有效 JSON 对象。
        const auto good = QJsonDocument::fromJson(readFile(originalPath)).object();
        // 收集损坏或不支持的凭据文件样本。
        QList<QByteArray> cases{QByteArray("not-json"), QByteArray("{}"), QByteArray(5000, 'x')};
        // 保存本次只修改版本或推导字段的配置副本。
        auto changed = good;
        changed["version"] = 2;
        cases.append(QJsonDocument(changed).toJson());
        changed = good;
        changed["iterations"] = 1;
        cases.append(QJsonDocument(changed).toJson());
        changed = good;
        changed["salt"] = QStringLiteral("bad-base64");
        cases.append(QJsonDocument(changed).toJson());
        changed = good;
        changed["verifier"] = good.value("verifier").toString() + "!";
        cases.append(QJsonDocument(changed).toJson());
        for (const auto &contents : cases)
        {
            // 保存本次损坏样本路径，始终与基准凭据分离。
            const auto path = directory.filePath("invalid.json");
            QVERIFY(writeFile(path, contents));
            // 验证损坏文件存在时只能拒绝，不能作为首次启动处理。
            AdminAccess access(path);
            // 收集格式失败原因，不能含明文密码。
            QString error;
            QVERIFY(access.isConfigured());
            QVERIFY(!access.login("admin", examplePassword(), &error));
            QVERIFY(!error.isEmpty());
            QVERIFY(!access.isAuthorized());
            QVERIFY(!access.configure("replacement", examplePassword(), examplePassword(), &error));
            QCOMPARE(readFile(path), contents);
            QVERIFY(!error.contains(examplePassword()));
        }
    }

    // 验证相同密码独立设置时使用不同随机盐和摘要。
    void independentConfigurationsUseDifferentSalts()
    {
        // 隔离两份独立首次设置的凭据。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // firstPath、secondPath 分别保存两个独立管理员配置。
        const auto firstPath = directory.filePath("first.json"),
                   secondPath = directory.filePath("second.json");
        // first、second 使用相同输入，验证随机盐不会复用。
        AdminAccess first(firstPath), second(secondPath);
        QVERIFY(first.configure("admin", examplePassword(), examplePassword()));
        QVERIFY(second.configure("admin", examplePassword(), examplePassword()));
        // firstRecord、secondRecord 保存两份实际写入的加密字段。
        const auto firstRecord = QJsonDocument::fromJson(readFile(firstPath)).object(),
                   secondRecord = QJsonDocument::fromJson(readFile(secondPath)).object();
        QVERIFY(firstRecord.value("salt") != secondRecord.value("salt"));
        QVERIFY(firstRecord.value("verifier") != secondRecord.value("verifier"));
    }

    // 验证凭据路径被目录占用时不删除或重新建立配置。
    void occupiedCredentialDirectoryIsNotReset()
    {
        // 保存与凭据文件冲突的已有目录。
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 将已有目录传为凭据路径，验证不能被覆盖。
        AdminAccess access(directory.path());
        QVERIFY(access.isConfigured());
        QVERIFY(!access.configure("admin", examplePassword(), examplePassword()));
        QVERIFY(!access.login("admin", examplePassword()));
        QVERIFY(QDir(directory.path()).exists());
        QVERIFY(!access.isAuthorized());
    }
};

QTEST_GUILESS_MAIN(AdminAccessTests)
#include "test_adminaccess.moc"
