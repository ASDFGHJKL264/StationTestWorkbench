#pragma once
#include <QElapsedTimer>
#include <QString>
#include <functional>

// 本机维护入口的管理员认证；凭据文件与业务数据库分开保存。
class AdminAccess final
{
  public:
    // 使用 credentialPath 保存凭据，可传入单调 clock 以验证会话期限。
    explicit AdminAccess(QString credentialPath, std::function<qint64()> clock = {});
    // 判断凭据路径是否已占用，损坏的配置也不能重新设置。
    bool isConfigured() const;
    // 首次创建 user 管理员，校验两次密码并保存加盐摘要。
    bool configure(const QString &user, const QString &password, const QString &confirmation,
                   QString *error = nullptr);
    // 校验 user 和 password，成功后建立五分钟维护会话。
    bool login(const QString &user, const QString &password, QString *error = nullptr);
    // 立即结束当前管理员会话。
    void logout();
    // 判断管理员会话是否仍在有效期限内。
    bool isAuthorized() const;
    // 返回当前已认证的操作人，会话失效时返回空。
    QString actor() const;

  private:
    // 获取真实或测试时钟的单调毫秒数。
    qint64 now() const;
    // 记录失败次数并推进下次允许登录的时刻。
    void rejectLogin();
    // 保存仅供本机维护使用的凭据文件路径。
    QString m_credentialPath;
    // 可选的测试时钟，默认使用 m_elapsed 的单调计时。
    std::function<qint64()> m_clock;
    // 为会话和失败冷却提供不受系统时间调整影响的时钟。
    QElapsedTimer m_elapsed;
    // 保存本次成功登录的管理员名。
    QString m_actor;
    // m_sessionStarted 记录登录时刻，m_sessionDeadline 限定会话结束时刻。
    qint64 m_sessionStarted = 0, m_sessionDeadline = 0;
    // 限定下一次允许尝试登录的时刻。
    qint64 m_retryAt = 0;
    // 累计连续登录失败次数，用于逐步延长冷却。
    int m_failures = 0;
};
