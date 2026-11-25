#include "adminaccess.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QPasswordDigestor>
#include <QRandomGenerator>
#include <QSaveFile>
#include <utility>

namespace
{
// 固定凭据格式使用的 PBKDF2 迭代次数。
constexpr int PasswordIterations = 200000;
// 限定一次管理员会话的有效毫秒数。
constexpr qint64 SessionDurationMs = 5 * 60 * 1000;

struct Credential
{
    // 保存与密码摘要绑定的管理员名。
    QString user;
    // 保存生成摘要时使用的随机盐。
    QByteArray salt;
    // 保存固定长度的 PBKDF2 密码摘要。
    QByteArray verifier;
};

// 把 message 写入可选 error 输出，并返回操作失败。
bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

// 检查管理员名是否为长度有限且无控制字符的完整文本。
bool validUser(const QString &user)
{
    if (user.isEmpty() || user.size() > 40 || user.trimmed() != user)
        return false;
    for (const auto character : user)
        if (!character.isPrint())
            return false;
    return true;
}

// 解析 canonical Base64 字段，拒绝非法字符和非预期长度。
bool decodeField(const QJsonObject &object, const QString &name, QByteArray *value)
{
    // 保存凭据字段的原始 Base64 文本。
    const auto encoded = object.value(name).toString().toLatin1();
    *value = QByteArray::fromBase64(encoded);
    return value->size() == 32 && value->toBase64() == encoded;
}

// 从 path 读取严格的本机凭据格式，不把损坏文件当成首次启动。
bool readCredential(const QString &path, Credential *credential, QString *error)
{
    // 检查凭据必须是可读取的普通文件。
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() > 4096)
        return fail(error, QStringLiteral("管理员配置无效，请检查本机凭据文件。"));
    // 读取凭据内容，明文密码不参与文件存储。
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("无法读取管理员配置。"));
    // 保存 JSON 格式解析结果。
    QJsonParseError parseError;
    // 保存本次读取的管理员配置对象。
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, QStringLiteral("管理员配置损坏，请检查本机凭据文件。"));
    // 校验格式版本、管理员名和固定加密参数。
    const auto object = document.object();
    credential->user = object.value("user").toString();
    if (object.size() != 5 || object.value("version").toInt(-1) != 1 ||
        object.value("iterations").toInt(-1) != PasswordIterations ||
        !validUser(credential->user) || !decodeField(object, "salt", &credential->salt) ||
        !decodeField(object, "verifier", &credential->verifier))
        return fail(error, QStringLiteral("管理员配置格式或版本无效，不能重新设置覆盖。"));
    return true;
}

// 用固定的迭代次数和盐计算密码摘要，随后清理临时密码字节。
QByteArray derive(const QString &password, const QByteArray &salt)
{
    // 保存仅在推导期间使用的 UTF-8 密码字节。
    auto bytes = password.toUtf8();
    // 保存固定长度的 PBKDF2-SHA256 摘要。
    const auto verifier = QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, bytes,
                                                             salt, PasswordIterations, 32);
    bytes.fill('\0');
    return verifier;
}

// 对等长摘要逐字节比较，不在首个差异处提前返回。
bool sameVerifier(const QByteArray &left, const QByteArray &right)
{
    if (left.size() != right.size())
        return false;
    // 汇总全部字节的差异，避免比较结果取决于相同前缀长度。
    volatile unsigned char difference = 0;
    for (qsizetype index = 0; index < left.size(); ++index)
        difference = difference | (static_cast<unsigned char>(left[index]) ^
                                   static_cast<unsigned char>(right[index]));
    return difference == 0;
}
} // namespace

AdminAccess::AdminAccess(QString credentialPath, std::function<qint64()> clock)
    : m_credentialPath(std::move(credentialPath)), m_clock(std::move(clock))
{
    m_elapsed.start();
}

qint64 AdminAccess::now() const
{
    return m_clock ? m_clock() : m_elapsed.elapsed();
}

bool AdminAccess::isConfigured() const
{
    // 损坏的符号链接和目录同样阻止首次设置覆盖。
    const QFileInfo info(m_credentialPath);
    return info.exists() || info.isSymLink();
}

bool AdminAccess::configure(const QString &user, const QString &password,
                            const QString &confirmation, QString *error)
{
    if (error)
        error->clear();
    logout();
    if (isConfigured())
        return fail(error, QStringLiteral("管理员已设置，不能通过首次设置覆盖。"));
    if (!validUser(user))
        return fail(error, QStringLiteral("管理员名需为1至40个可见字符，且不能有首尾空白。"));
    if (password.size() < 10 || password.size() > 128)
        return fail(error, QStringLiteral("管理员密码需为10至128个字符。"));
    for (const auto character : password)
        if (!character.isPrint())
            return fail(error, QStringLiteral("管理员密码不能包含控制字符。"));
    if (password != confirmation)
        return fail(error, QStringLiteral("两次输入的密码不一致。"));
    if (!QDir().mkpath(QFileInfo(m_credentialPath).absolutePath()))
        return fail(error, QStringLiteral("无法创建管理员配置目录。"));
    // 防止两个首次设置同时创建或覆盖管理员身份。
    QLockFile lock(m_credentialPath + ".lock");
    if (!lock.tryLock(0))
        return fail(error, QStringLiteral("管理员配置正在更新，请稍后重试。"));
    if (isConfigured())
        return fail(error, QStringLiteral("管理员已由另一操作设置，请重新登录。"));
    // 生成只用于当前管理员的随机盐。
    QByteArray salt;
    salt.reserve(32);
    for (int index = 0; index < 8; ++index)
    {
        // 从系统随机源取得四字节随机值。
        const auto random = QRandomGenerator::system()->generate();
        salt.append(reinterpret_cast<const char *>(&random), sizeof(random));
    }
    // 仅保存用户标识、盐和不可直接还原密码的推导摘要。
    const QJsonObject record{{"version", 1},
                             {"user", user},
                             {"iterations", PasswordIterations},
                             {"salt", QString::fromLatin1(salt.toBase64())},
                             {"verifier", QString::fromLatin1(derive(password, salt).toBase64())}};
    // 原子发布凭据文件，失败时不会留下半份可用配置。
    QSaveFile file(m_credentialPath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return fail(error, QStringLiteral("无法安全写入管理员配置。"));
    // 保存需要完整写入的凭据 JSON 字节。
    const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit())
        return fail(error, QStringLiteral("管理员配置保存失败。"));
    m_actor = user;
    m_sessionStarted = now();
    m_sessionDeadline = m_sessionStarted + SessionDurationMs;
    m_failures = 0;
    m_retryAt = 0;
    return true;
}

void AdminAccess::rejectLogin()
{
    m_failures = qMin(m_failures + 1, 6);
    m_retryAt = now() + qMin<qint64>(30000, qint64(1000) << (m_failures - 1));
}

bool AdminAccess::login(const QString &user, const QString &password, QString *error)
{
    if (error)
        error->clear();
    logout();
    if (now() < m_retryAt)
        return fail(error, QStringLiteral("登录尝试过于频繁，请稍后重试。"));
    // 读取本次登录需要核对的管理员和加密参数。
    Credential credential;
    if (!readCredential(m_credentialPath, &credential, error))
    {
        rejectLogin();
        return false;
    }
    if (password.size() > 128)
    {
        rejectLogin();
        return fail(error, QStringLiteral("管理员名或密码错误。"));
    }
    // 计算完整摘要后再比较，不按密码前缀提前返回。
    auto verifier = derive(password, credential.salt);
    // 同时核对管理员身份和密码摘要。
    const bool accepted = sameVerifier(verifier, credential.verifier) && user == credential.user;
    verifier.fill('\0');
    if (!accepted)
    {
        rejectLogin();
        return fail(error, QStringLiteral("管理员名或密码错误。"));
    }
    m_actor = credential.user;
    m_sessionStarted = now();
    m_sessionDeadline = m_sessionStarted + SessionDurationMs;
    m_failures = 0;
    m_retryAt = 0;
    return true;
}

void AdminAccess::logout()
{
    m_actor.clear();
    m_sessionStarted = 0;
    m_sessionDeadline = 0;
}

bool AdminAccess::isAuthorized() const
{
    // 保存同一次权限检查采用的时刻，避免跨越边界。
    const auto current = now();
    return !m_actor.isEmpty() && current >= m_sessionStarted && current < m_sessionDeadline;
}

QString AdminAccess::actor() const
{
    return isAuthorized() ? m_actor : QString();
}
