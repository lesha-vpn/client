#include "coordinatorApi.h"

#include <QHostAddress>
#include <QHostInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslError>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <memory>

#include "core/utils/constants/apiConstants.h"
#include "version.h"

namespace
{
    QString encodeSegment(const QString &segment)
    {
        return QString::fromLatin1(QUrl::toPercentEncoding(segment));
    }
}

CoordinatorApi::CoordinatorApi(QNetworkAccessManager *networkManager, QObject *parent)
    : QObject(parent), m_networkManager(networkManager)
{
}

void CoordinatorApi::setBaseUrl(const QString &baseUrl)
{
    QString url = baseUrl.trimmed();
    while (url.endsWith(QLatin1Char('/'))) {
        url.chop(1);
    }
    if (QUrl(url).host() != QUrl(m_baseUrl).host()) {
        m_fallbackAddress.clear(); // belongs to the old host
    }
    m_baseUrl = url;
}

QString CoordinatorApi::baseUrl() const
{
    return m_baseUrl;
}

void CoordinatorApi::setToken(const QString &token)
{
    m_token = token;
}

void CoordinatorApi::login(const QString &code, const QString &deviceLabel, const Callback &callback)
{
    QJsonObject body;
    body[QStringLiteral("code")] = code;
    if (!deviceLabel.isEmpty()) {
        body[QStringLiteral("deviceLabel")] = deviceLabel;
    }
    send("POST", QStringLiteral("/auth/login"), &body, m_baseUrl, QString(), callback);
}

void CoordinatorApi::logout(const Callback &callback)
{
    logout(m_baseUrl, m_token, callback);
}

void CoordinatorApi::logout(const QString &baseUrl, const QString &token, const Callback &callback)
{
    const QJsonObject body;
    send("POST", QStringLiteral("/auth/logout"), &body, baseUrl, token, callback);
}

void CoordinatorApi::me(const Callback &callback)
{
    send("GET", QStringLiteral("/me"), nullptr, m_baseUrl, m_token, callback);
}

void CoordinatorApi::createConfig(const QString &name, const Callback &callback)
{
    QJsonObject body;
    body[QStringLiteral("name")] = name;
    send("POST", QStringLiteral("/configs"), &body, m_baseUrl, m_token, callback);
}

void CoordinatorApi::getConfig(const QString &configId, const Callback &callback)
{
    send("GET", QStringLiteral("/configs/") + encodeSegment(configId), nullptr, m_baseUrl, m_token, callback);
}

void CoordinatorApi::configStatus(const QString &configId, const QString &revision, const Callback &callback)
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("revision"), revision);
    send("GET", QStringLiteral("/configs/%1/status?%2").arg(encodeSegment(configId), query.toString(QUrl::FullyEncoded)), nullptr,
         m_baseUrl, m_token, callback);
}

void CoordinatorApi::deleteConfig(const QString &configId, const Callback &callback)
{
    send("DELETE", QStringLiteral("/configs/") + encodeSegment(configId), nullptr, m_baseUrl, m_token, callback);
}

void CoordinatorApi::renameConfig(const QString &configId, const QString &name, const Callback &callback)
{
    QJsonObject body;
    body[QStringLiteral("name")] = name;
    send("PATCH", QStringLiteral("/configs/") + encodeSegment(configId), &body, m_baseUrl, m_token, callback);
}

QString CoordinatorApi::host() const
{
    return QUrl(m_baseUrl).host();
}

void CoordinatorApi::setFallbackAddress(const QString &address)
{
    m_fallbackAddress = address;
}

QString CoordinatorApi::fallbackAddress() const
{
    return m_fallbackAddress;
}

void CoordinatorApi::learnAddress()
{
    const QString hostName = host();
    if (hostName.isEmpty() || !QHostAddress(hostName).isNull()) {
        return;
    }
    QHostInfo::lookupHost(hostName, this, [this, hostName](const QHostInfo &info) {
        if (hostName != host()) {
            return; // base URL changed meanwhile
        }
        for (const QHostAddress &address : info.addresses()) {
            if (address.protocol() == QAbstractSocket::IPv4Protocol) {
                emit addressLearned(address.toString());
                return;
            }
        }
    });
}

void CoordinatorApi::send(const QByteArray &verb, const QString &path, const QJsonObject *body, const QString &baseUrl,
                          const QString &token, const Callback &callback, bool viaFallback)
{
    QUrl url(baseUrl + QStringLiteral("/v1") + path, QUrl::StrictMode);
    const QString hostName = url.host();
    QNetworkRequest request;
    if (viaFallback) {
        // Connect to the IP, but keep the name for TLS (SNI and certificate check) and for the Host header
        request.setPeerVerifyName(hostName);
        const int port = url.port();
        request.setRawHeader("Host", (port > 0 ? QStringLiteral("%1:%2").arg(hostName).arg(port) : hostName).toUtf8());
        url.setHost(m_fallbackAddress);
        // HTTP/2 takes :authority from the URL (the IP), not from the Host header; a reverse proxy routing by name needs HTTP/1.1
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    }
    request.setUrl(url);
    request.setTransferTimeout(apiDefs::requestTimeoutMsecs);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("X-Client-Request-ID", QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8());
    request.setRawHeader("X-App-Version", QByteArray(APP_FULL_VERSION));
    if (!token.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    }

    const QByteArray data = body ? QJsonDocument(*body).toJson(QJsonDocument::Compact) : QByteArray();

    QNetworkReply *reply = nullptr;
    if (verb == "GET") {
        reply = m_networkManager->get(request);
    } else if (verb == "POST") {
        reply = m_networkManager->post(request, data);
    } else if (verb == "DELETE") {
        reply = m_networkManager->deleteResource(request);
    } else {
        reply = m_networkManager->sendCustomRequest(request, verb, data);
    }

    auto sslErrors = std::make_shared<QStringList>();
    connect(reply, &QNetworkReply::sslErrors, this, [sslErrors](const QList<QSslError> &errors) {
        for (const QSslError &error : errors) {
            sslErrors->append(error.errorString());
        }
    });

    const std::shared_ptr<QJsonObject> bodyCopy = body ? std::make_shared<QJsonObject>(*body) : nullptr;
    connect(reply, &QNetworkReply::finished, this, [this, reply, sslErrors, callback, verb, path, bodyCopy, baseUrl,
                                                    viaFallback, hostName, token]() {
        Reply result;
        result.token = token;
        result.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        result.networkError = result.httpStatus == 0 || !sslErrors->isEmpty()
                || reply->error() == QNetworkReply::OperationCanceledError || reply->error() == QNetworkReply::TimeoutError;

        const QByteArray responseBody = reply->readAll();
        if (!responseBody.isEmpty()) {
            const QJsonDocument document = QJsonDocument::fromJson(responseBody);
            if (document.isObject()) {
                result.body = document.object();
            }
        }

        const QJsonObject error = result.body.value(QStringLiteral("error")).toObject();
        result.errorCode = error.value(QStringLiteral("code")).toString();
        result.errorMessage = error.value(QStringLiteral("message")).toString();

        if (!sslErrors->isEmpty()) {
            result.errorMessage = QStringLiteral("SSL error: ") + sslErrors->join(QStringLiteral("; "));
        } else if (result.errorMessage.isEmpty() && !result.ok()) {
            result.errorMessage = reply->errorString();
        }

        reply->deleteLater();

        // Unreachable by name (no HTTP answer, not a TLS problem): retry once via the known address.
        // A POST may already have reached the server before a timeout or reset (a created config, a used login code),
        // so it is only retried when the connection itself failed.
        const QNetworkReply::NetworkError networkError = reply->error();
        const bool notSent = networkError == QNetworkReply::HostNotFoundError || networkError == QNetworkReply::ConnectionRefusedError
                || networkError == QNetworkReply::TemporaryNetworkFailureError || networkError == QNetworkReply::NetworkSessionFailedError
                || networkError == QNetworkReply::UnknownNetworkError;
        const bool canRetry = !viaFallback && result.httpStatus == 0 && sslErrors->isEmpty() && !m_fallbackAddress.isEmpty()
                && m_fallbackAddress != hostName && hostName == host() && (verb != "POST" || notSent);
        if (canRetry) {
            send(verb, path, bodyCopy.get(), baseUrl, token, callback, true);
            return;
        }
        if (!viaFallback && result.httpStatus != 0) {
            learnAddress();
        }
        if (callback) {
            callback(result);
        }
    });
}
