#ifndef COORDINATORAPI_H
#define COORDINATORAPI_H

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

class QNetworkAccessManager;

// Thin async client for the VPN coordinator HTTP API (/v1, JSON, Bearer device token).
// Every call returns immediately and invokes the callback on the caller's thread once the reply finishes.
class CoordinatorApi : public QObject
{
    Q_OBJECT

public:
    struct Reply
    {
        int httpStatus = 0;     // 0 when the request never got an HTTP response
        bool networkError = false; // DNS/TLS/timeout/connection failure (coordinator unreachable)
        QString errorCode;      // error.code from the body, e.g. "invalid_code", "not_member", "name_taken"
        QString errorMessage;   // error.message from the body, or the Qt network error text
        QJsonObject body;       // parsed JSON body (empty for 204 or non-JSON replies)
        QString token;          // device token the request was sent with (empty if none)

        bool ok() const { return !networkError && httpStatus >= 200 && httpStatus < 300; }
        bool unauthorized() const { return httpStatus == 401; }
        bool notMember() const { return httpStatus == 403 && errorCode == QLatin1String("not_member"); }
        bool notFound() const { return httpStatus == 404; }
    };
    using Callback = std::function<void(const Reply &)>;

    explicit CoordinatorApi(QNetworkAccessManager *networkManager, QObject *parent = nullptr);

    // Base URL without the /v1 suffix, e.g. "https://api.example.org". Trailing slashes are ignored.
    void setBaseUrl(const QString &baseUrl);
    QString baseUrl() const;
    void setToken(const QString &token);

    // POST /v1/auth/login { code, deviceLabel } -> 200 { token, user: { tgId, username } }
    void login(const QString &code, const QString &deviceLabel, const Callback &callback);
    // POST /v1/auth/logout -> 204
    void logout(const Callback &callback);
    // Same, for an account captured earlier: the current base URL and token may already be reset by then
    void logout(const QString &baseUrl, const QString &token, const Callback &callback);
    // GET /v1/me -> { user, configs: [ConfigSummary] }
    void me(const Callback &callback);
    // POST /v1/configs { name } -> 201 Config; 409 name_taken
    void createConfig(const QString &name, const Callback &callback);
    // GET /v1/configs/{id} -> Config
    void getConfig(const QString &configId, const Callback &callback);
    // GET /v1/configs/{id}/status?revision=... -> ConfigStatus
    void configStatus(const QString &configId, const QString &revision, const Callback &callback);
    // DELETE /v1/configs/{id} -> 204
    void deleteConfig(const QString &configId, const Callback &callback);
    // PATCH /v1/configs/{id} { name } -> ConfigSummary
    void renameConfig(const QString &configId, const QString &name, const Callback &callback);

    // Hostname of the base URL, used to allow the coordinator through the strict kill switch.
    QString host() const;

    // Known IPv4 of the host. When a request can't reach the host by name (DNS goes into a dead tunnel),
    // it is retried once against this address, keeping the host name for TLS (SNI, certificate) and Host.
    void setFallbackAddress(const QString &address);
    QString fallbackAddress() const;

signals:
    // The host answered and currently resolves to this IPv4 address.
    void addressLearned(const QString &address);

private:
    // token: Bearer token to send, empty for none
    void send(const QByteArray &verb, const QString &path, const QJsonObject *body, const QString &baseUrl, const QString &token,
              const Callback &callback, bool viaFallback = false);
    void learnAddress();

    QNetworkAccessManager *m_networkManager;
    QString m_baseUrl;
    QString m_token;
    QString m_fallbackAddress;
};

#endif // COORDINATORAPI_H
