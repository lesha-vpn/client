#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QPointer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSysInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <functional>

#include "core/controllers/connectionController.h"
#include "core/controllers/coordinator/coordinatorApi.h"
#include "core/controllers/coordinator/coordinatorController.h"
#include "core/controllers/coordinator/deviceName.h"
#include "core/controllers/selfhosted/importController.h"
#include "core/models/protocols/awgProtocolConfig.h"
#include "core/utils/constants/configKeys.h"
#include "core/utils/constants/protocolConstants.h"
#include "secureQSettings.h"
#include "utils/testCoreController.h"
#include "vpnConnection.h"

using namespace amnezia;

namespace
{

// Minimal in-process HTTP/1.1 server: one request per connection, "Connection: close".
class MockHttpServer : public QObject
{
    Q_OBJECT

public:
    struct Request
    {
        QByteArray method;
        QString path;              // without query
        QUrlQuery query;
        QHash<QByteArray, QByteArray> headers; // lower-cased names
        QByteArray body;

        QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
    };
    struct Response
    {
        int status = 200;
        QByteArray body; // JSON text, empty for 204
    };
    using Handler = std::function<Response(const Request &)>;

    explicit MockHttpServer(QObject *parent = nullptr) : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, &MockHttpServer::onNewConnection);
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    QString baseUrl() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

    Handler handler;
    QList<Request> requests;

    QList<Request> requestsTo(const QByteArray &method, const QString &path) const
    {
        QList<Request> out;
        for (const Request &r : requests) {
            if (r.method == method && r.path == path) {
                out.append(r);
            }
        }
        return out;
    }

private:
    void onNewConnection()
    {
        while (QTcpSocket *socket = m_server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    }

    void onReadyRead(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();

        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }
        const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
        Request request;
        const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
        request.method = requestLine.value(0);
        const QUrl url(QString::fromLatin1(requestLine.value(1)));
        request.path = url.path();
        request.query = QUrlQuery(url);
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const qsizetype colon = line.indexOf(':');
            if (colon > 0) {
                request.headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
            }
        }
        const int contentLength = request.headers.value("content-length", "0").toInt();
        if (buffer.size() - (headerEnd + 4) < contentLength) {
            return; // wait for the rest of the body
        }
        request.body = buffer.mid(headerEnd + 4, contentLength);
        m_buffers.remove(socket);
        requests.append(request);

        const Response response = handler ? handler(request) : Response { 404, R"({"error":{"code":"not_found","message":"x"}})" };
        QByteArray out = "HTTP/1.1 " + QByteArray::number(response.status) + " " + reason(response.status) + "\r\n";
        out += "Connection: close\r\n";
        if (!response.body.isEmpty()) {
            out += "Content-Type: application/json\r\n";
        }
        out += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n\r\n";
        out += response.body;
        socket->write(out);
        socket->disconnectFromHost();
    }

    static QByteArray reason(int status)
    {
        switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 409: return "Conflict";
        default: return "Status";
        }
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

QByteArray errorBody(const QString &code)
{
    QJsonObject error;
    error["code"] = code;
    error["message"] = code;
    QJsonObject body;
    body["error"] = error;
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QByteArray jsonBytes(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QString awgConf(const QString &endpoint)
{
    return QStringLiteral("[Interface]\n"
                          "PrivateKey = yAnz5TF+lXXJte14tji3zlMNq+hd2rYUIgJBgB3fBmk=\n"
                          "Address = 10.8.1.2/32\n"
                          "DNS = 1.1.1.1, 8.8.8.8\n"
                          "MTU = 1280\n"
                          "Jc = 4\n"
                          "Jmin = 10\n"
                          "Jmax = 50\n"
                          "S1 = 37\n"
                          "S2 = 91\n"
                          "H1 = 1234567891\n"
                          "H2 = 1234567892\n"
                          "H3 = 1234567893\n"
                          "H4 = 1234567894\n"
                          "\n"
                          "[Peer]\n"
                          "PublicKey = xTIBA5rboUvnH4htodjb6e697QjLERt1NAB4mZqp8Dg=\n"
                          "PresharedKey = mT1Yt5DtYDGY3PZRXbTyqWfDp5W5C8gGq1Ww/2lNC3Y=\n"
                          "AllowedIPs = 0.0.0.0/0\n"
                          "Endpoint = %1\n"
                          "PersistentKeepalive = 25\n")
            .arg(endpoint);
}

// AWG 3.1 params as the coordinator's awg3 profile renders them (mirrors upstream AwgInstaller)
QString awg3Conf(const QString &endpoint)
{
    QString conf = awgConf(endpoint);
    QString params = QStringLiteral("Jc = 4\nJmin = 10\nJmax = 50\n"
                                    "S1 = 12\nS2 = 12\nS3 = 12\nS4 = 12\n"
                                    "H1 = 1\nH2 = 2\nH3 = 3\nH4 = 4\n"
                                    "I1 = <r 2><b 0x858000010001000000000669636c6f756403636f6d0000010001c00c000100010000105a00044d583737>\n"
                                    "HeaderProtectionKey = q5YQqvq3ZsdHo1ag8Grk1zE1gKXlo6VgSc8uzQ6QY5E=\n"
                                    "RekeyAfterTime = 110\nRekeyTimeout = 5\nRejectAfterTime = 170\n"
                                    "KeepaliveTimeout = 10\nMaxHandshakeAttempts = 18\n"
                                    "RandomTrailers = on\nDisableCookies = on\n");
    const int start = conf.indexOf("Jc = ");
    const int end = conf.indexOf("\n[Peer]");
    conf.replace(start, end - start, params);
    return conf;
}

QJsonObject configJson(const QString &id, const QString &name, const QString &revision, const QString &endpoint)
{
    QJsonObject config;
    config["id"] = id;
    config["name"] = name;
    config["revision"] = revision;
    config["createdAt"] = "2026-10-12T10:00:00Z";
    config["awg"] = awgConf(endpoint);
    config["vpnKey"] = "vpn://unused";
    config["endpoint"] = endpoint;
    return config;
}

// The AWG container's parsed last_config of a stored server (empty if not found).
QJsonObject awgLastConfig(const QJsonObject &serverJson, QString *containerName = nullptr)
{
    for (const QJsonValue &value : serverJson.value(configKey::containers).toArray()) {
        const QJsonObject container = value.toObject();
        if (container.contains(configKey::awg)) {
            if (containerName) {
                *containerName = container.value(configKey::container).toString();
            }
            const QString lastConfig = container.value(configKey::awg).toObject().value(configKey::lastConfig).toString();
            return QJsonDocument::fromJson(lastConfig.toUtf8()).object();
        }
    }
    return {};
}

} // namespace

class TestCoordinatorController : public QObject
{
    Q_OBJECT

private:
    SecureQSettings *m_settings = nullptr;
    TestCoreController *m_core = nullptr;
    QSharedPointer<VpnConnection> m_vpnConnection;
    ConnectionController *m_connectionController = nullptr;
    QNetworkAccessManager *m_networkManager = nullptr;
    CoordinatorApi *m_api = nullptr;
    CoordinatorController *m_controller = nullptr;
    MockHttpServer *m_server = nullptr;

    // Mock server state
    const QString m_token = QStringLiteral("device-token-123");
    QString m_statusState;   // "ok" | "outdated" | "suspended" | "revoked"
    int m_statusHttp = 200;  // non-200 -> error reply
    QString m_statusErrorCode;
    QJsonObject m_currentConfig; // returned by GET /v1/configs/{id}
    bool m_issueAwg3 = false;    // POST /v1/configs renders an AWG 3.1 config
    QString m_nextConfigId = QStringLiteral("cfg-1"); // id of the next config POST /v1/configs creates

    MockHttpServer::Response handle(const MockHttpServer::Request &request)
    {
        const bool authorized = request.headers.value("authorization") == "Bearer " + m_token.toUtf8();
        if (request.method == "POST" && request.path == "/v1/auth/login") {
            if (request.json().value("code").toString() != "AB12CD") {
                return { 400, errorBody("invalid_code") };
            }
            QJsonObject user;
            user["tgId"] = 42;
            user["username"] = "alice";
            QJsonObject body;
            body["token"] = m_token;
            body["user"] = user;
            return { 200, jsonBytes(body) };
        }
        if (!authorized) {
            return { 401, errorBody("invalid_token") };
        }
        if (request.method == "POST" && request.path == "/v1/auth/logout") {
            return { 204, {} };
        }
        if (request.method == "GET" && request.path == "/v1/me") {
            QJsonObject user;
            user["tgId"] = 42;
            user["username"] = "alice";
            QJsonArray configs;
            if (!m_currentConfig.isEmpty()) {
                QJsonObject summary;
                summary["id"] = m_currentConfig.value("id");
                summary["name"] = m_currentConfig.value("name");
                summary["revision"] = m_currentConfig.value("revision");
                summary["createdAt"] = m_currentConfig.value("createdAt");
                configs.append(summary);
            }
            QJsonObject body;
            body["user"] = user;
            body["configs"] = configs;
            return { 200, jsonBytes(body) };
        }
        if (request.method == "POST" && request.path == "/v1/configs") {
            const QString name = request.json().value("name").toString();
            m_currentConfig = configJson(m_nextConfigId, name, "rev1", "203.0.113.10:51820");
            if (m_issueAwg3) {
                m_currentConfig["awg"] = awg3Conf("203.0.113.10:51820");
            }
            return { 201, jsonBytes(m_currentConfig) };
        }
        const QString configPath = "/v1/configs/" + m_currentConfig.value("id").toString();
        if (request.method == "DELETE" && !m_currentConfig.isEmpty() && request.path == configPath) {
            m_currentConfig = {};
            return { 204, {} };
        }
        if (request.method == "GET" && !m_currentConfig.isEmpty() && request.path == configPath + "/status") {
            if (m_statusHttp != 200) {
                return { m_statusHttp, errorBody(m_statusErrorCode) };
            }
            QJsonObject body;
            body["state"] = m_statusState;
            body["revision"] = m_currentConfig.value("revision");
            body["network"] = "ok";
            body["checkedAt"] = "2026-10-12T10:05:00Z";
            return { 200, jsonBytes(body) };
        }
        if (request.method == "PATCH" && request.path == "/v1/configs/cfg-1") {
            const QString name = request.json().value("name").toString();
            if (name == "taken") {
                return { 409, errorBody("name_taken") };
            }
            m_currentConfig["name"] = name;
            QJsonObject summary;
            summary["id"] = "cfg-1";
            summary["name"] = name;
            summary["revision"] = m_currentConfig.value("revision");
            summary["state"] = "active";
            summary["createdAt"] = m_currentConfig.value("createdAt");
            return { 200, jsonBytes(summary) };
        }
        if (request.method == "GET" && !m_currentConfig.isEmpty() && request.path == configPath) {
            return { 200, jsonBytes(m_currentConfig) };
        }
        return { 404, errorBody("not_found") };
    }

    void login()
    {
        QSignalSpy loginSpy(m_controller, &CoordinatorController::loginFinished);
        m_controller->login(m_server->baseUrl() + "/", " ab12cd ");
        QTRY_COMPARE(loginSpy.count(), 1);
        QVERIFY(loginSpy.at(0).at(0).toBool());
        QVERIFY(m_controller->isLoggedIn());
    }

    QString createPhone()
    {
        QSignalSpy addedSpy(m_controller, &CoordinatorController::configAdded);
        m_controller->createConfig("phone");
        [&]() { QTRY_COMPARE(addedSpy.count(), 1); }();
        return addedSpy.isEmpty() ? QString() : addedSpy.at(0).at(0).toString();
    }

private slots:
    void initTestCase()
    {
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);

        const QString testOrg = "AmneziaVPN-Test-" + QUuid::createUuid().toString();
        m_settings = new SecureQSettings(testOrg, "amnezia-client", nullptr, false);
        m_vpnConnection = QSharedPointer<VpnConnection>::create(nullptr, nullptr);
        m_core = new TestCoreController(m_vpnConnection, m_settings, nullptr, this);

        m_connectionController =
                new ConnectionController(m_core->m_serversRepository, m_core->m_appSettingsRepository, m_vpnConnection.get(), this);
        m_networkManager = new QNetworkAccessManager(this);
        m_api = new CoordinatorApi(m_networkManager, this);
        m_controller = new CoordinatorController(m_core->m_serversRepository, m_core->m_appSettingsRepository,
                                                 m_core->m_importCoreController, m_connectionController, m_api, this);

        m_server = new MockHttpServer(this);
        m_server->handler = [this](const MockHttpServer::Request &r) { return handle(r); };
        QVERIFY(m_server->listen());
    }

    void cleanupTestCase()
    {
        m_settings->clearSettings();
        delete m_controller;
        delete m_core;
        delete m_settings;
    }

    void init()
    {
        m_settings->clearSettings();
        m_core->m_serversRepository->invalidateCache();
        m_api->setToken(QString());
        m_server->requests.clear();
        m_statusState = "ok";
        m_statusHttp = 200;
        m_statusErrorCode.clear();
        m_currentConfig = {};
        m_issueAwg3 = false;
        m_nextConfigId = QStringLiteral("cfg-1");
    }

    void testLogin()
    {
        QSignalSpy loggedInSpy(m_controller, &CoordinatorController::loggedInChanged);
        QSignalSpy remoteSpy(m_controller, &CoordinatorController::remoteConfigsUpdated);
        login();

        const auto logins = m_server->requestsTo("POST", "/v1/auth/login");
        QCOMPARE(logins.size(), 1);
        QCOMPARE(logins.first().json().value("code").toString(), QStringLiteral("AB12CD"));
        QVERIFY(!logins.first().headers.value("x-app-version").isEmpty());
        QVERIFY(!logins.first().headers.contains("authorization"));

        QCOMPARE(m_core->m_appSettingsRepository->coordinatorToken(), m_token);
        QCOMPARE(m_controller->baseUrl(), m_server->baseUrl());
        QCOMPARE(m_controller->username(), QStringLiteral("@alice"));
        QCOMPARE(loggedInSpy.count(), 1);

        // The login page follows up with setupThisDevice(), which fetches /me itself: no second request here
        QTest::qWait(200);
        QCOMPARE(m_server->requestsTo("GET", "/v1/me").size(), 0);
        m_controller->fetchRemoteConfigs();
        QTRY_COMPARE(remoteSpy.count(), 1);
        const auto meRequests = m_server->requestsTo("GET", "/v1/me");
        QCOMPARE(meRequests.size(), 1);
        QCOMPARE(meRequests.first().headers.value("authorization"), "Bearer " + m_token.toUtf8());
        QVERIFY(!meRequests.first().headers.value("x-app-version").isEmpty());
    }

    void testLoginCleansUpCode()
    {
        QSignalSpy loginSpy(m_controller, &CoordinatorController::loginFinished);
        m_controller->login(m_server->baseUrl(), " ab 12\ncd ");
        QTRY_COMPARE(loginSpy.count(), 1);
        QVERIFY(loginSpy.at(0).at(0).toBool());
        QCOMPARE(m_server->requestsTo("POST", "/v1/auth/login").first().json().value("code").toString(), QStringLiteral("AB12CD"));
    }

    void testLoginRejectsPlainHttp()
    {
        QSignalSpy loginSpy(m_controller, &CoordinatorController::loginFinished);
        m_controller->login("http://vpn.example.com", "AB12CD");
        QCOMPARE(loginSpy.count(), 1);
        QVERIFY(!loginSpy.at(0).at(0).toBool());
        QVERIFY(m_server->requestsTo("POST", "/v1/auth/login").isEmpty());
        QVERIFY(!m_controller->isLoggedIn());
    }

    void testLoginInvalidCode()
    {
        QSignalSpy loginSpy(m_controller, &CoordinatorController::loginFinished);
        m_controller->login(m_server->baseUrl(), "zzzzzz");
        QTRY_COMPARE(loginSpy.count(), 1);
        QVERIFY(!loginSpy.at(0).at(0).toBool());
        QVERIFY(!m_controller->isLoggedIn());
    }

    void testCreateConfig()
    {
        login();
        const int serversBefore = m_core->m_serversRepository->serversCount();
        const QString serverId = createPhone();
        QVERIFY(!serverId.isEmpty());

        const auto creates = m_server->requestsTo("POST", "/v1/configs");
        QCOMPARE(creates.size(), 1);
        QCOMPARE(creates.first().headers.value("authorization"), "Bearer " + m_token.toUtf8());
        QCOMPARE(creates.first().json().value("name").toString(), QStringLiteral("phone"));

        QCOMPARE(m_core->m_serversRepository->serversCount(), serversBefore + 1);
        const auto server = m_core->m_serversRepository->nativeConfig(serverId);
        QVERIFY(server.has_value());
        QCOMPARE(server->description, QStringLiteral("phone"));
        QCOMPARE(server->hostName, QStringLiteral("203.0.113.10"));
        QVERIFY(ContainerUtils::isAwgContainer(server->defaultContainer));

        QString containerName;
        const QJsonObject lastConfig = awgLastConfig(server->toJson(), &containerName);
        QVERIFY2(!lastConfig.isEmpty(), "AWG last_config must be stored");
        QVERIFY(ContainerUtils::isAwgContainer(ContainerUtils::containerFromString(containerName)));
        QCOMPARE(lastConfig.value(configKey::mtu).toString(), QStringLiteral("1280"));
        QCOMPARE(lastConfig.value(configKey::port).toVariant().toInt(), 51820);
        QCOMPARE(lastConfig.value(configKey::hostName).toString(), QStringLiteral("203.0.113.10"));

        const QJsonObject entry = m_controller->managedConfigs().value(serverId).toObject();
        QCOMPARE(entry.value("configId").toString(), QStringLiteral("cfg-1"));
        QCOMPARE(entry.value("revision").toString(), QStringLiteral("rev1"));
        QCOMPARE(entry.value("state").toString(), QStringLiteral("ok"));
        QCOMPARE(m_controller->serverIdForConfig("cfg-1"), serverId);
    }

    void testSetupThisDevice()
    {
        login();
        QSignalSpy setupSpy(m_controller, &CoordinatorController::deviceSetupFinished);
        QSignalSpy addedSpy(m_controller, &CoordinatorController::configAdded);
        m_controller->setupThisDevice();
        QTRY_COMPARE(setupSpy.count(), 1);
        QVERIFY(setupSpy.at(0).at(0).toBool());
        QCOMPARE(addedSpy.count(), 1);

        const auto creates = m_server->requestsTo("POST", "/v1/configs");
        QCOMPARE(creates.size(), 1);
        const QString name = creates.first().json().value("name").toString();
        QCOMPARE(name, CoordinatorController::deviceConfigName(deviceName::platformName(), QSysInfo::productType(), {}));
        QCOMPARE(m_controller->serverIdForConfig("cfg-1"), addedSpy.at(0).at(0).toString());

        // A config is already in the app: nothing new is created
        m_controller->setupThisDevice();
        QTRY_COMPARE(setupSpy.count(), 2);
        QVERIFY(setupSpy.at(1).at(0).toBool());
        QCOMPARE(m_server->requestsTo("POST", "/v1/configs").size(), 1);
    }

    void testSetupReplacesRevokedConfig()
    {
        login();
        const QString serverId = createPhone();
        QVERIFY(!serverId.isEmpty());
        const int serversBefore = m_core->m_serversRepository->serversCount();

        // Revoked in the bot (or another account signed in): /me no longer lists it
        m_currentConfig = {};
        m_nextConfigId = QStringLiteral("cfg-2");
        QSignalSpy setupSpy(m_controller, &CoordinatorController::deviceSetupFinished);
        QSignalSpy replacedSpy(m_controller, &CoordinatorController::configReplaced);
        m_controller->setupThisDevice();
        QTRY_COMPARE(setupSpy.count(), 1);
        QVERIFY(setupSpy.at(0).at(0).toBool());

        // A new config takes the dead one's place instead of piling up next to it
        QCOMPARE(m_server->requestsTo("POST", "/v1/configs").size(), 2);
        QCOMPARE(replacedSpy.count(), 1);
        QCOMPARE(m_core->m_serversRepository->serversCount(), serversBefore);
        QCOMPARE(m_controller->serverIdForConfig("cfg-2"), serverId);
        QVERIFY(m_controller->serverIdForConfig("cfg-1").isEmpty());
        QCOMPARE(m_controller->managedConfigs().value(serverId).toObject().value("state").toString(), QStringLiteral("ok"));
    }

    void testFetchMarksMissingConfigRevoked()
    {
        login();
        const QString serverId = createPhone();
        QVERIFY(!serverId.isEmpty());

        m_currentConfig = {};
        QSignalSpy remoteSpy(m_controller, &CoordinatorController::remoteConfigsUpdated);
        QSignalSpy errorSpy(m_controller, &CoordinatorController::errorOccurred);
        m_controller->fetchRemoteConfigs();
        QTRY_COMPARE(remoteSpy.count(), 1);
        QCOMPARE(m_controller->managedConfigs().value(serverId).toObject().value("state").toString(), QStringLiteral("revoked"));
        QCOMPARE(errorSpy.count(), 1);
    }

    void testDeleteConfigRemovesServer()
    {
        login();
        const QString serverId = createPhone();
        QVERIFY(!serverId.isEmpty());

        QSignalSpy deletedSpy(m_controller, &CoordinatorController::configDeleted);
        m_controller->deleteConfig("cfg-1");
        QTRY_COMPARE(deletedSpy.count(), 1);
        QCOMPARE(deletedSpy.at(0).at(0).toString(), QStringLiteral("cfg-1"));
        QCOMPARE(m_server->requestsTo("DELETE", "/v1/configs/cfg-1").size(), 1);
        QVERIFY(m_core->m_serversRepository->indexOfServerId(serverId) < 0);
        QVERIFY(!m_controller->isManagedServer(serverId));
    }

    void testForgetDeviceRevokesConfigsAndToken()
    {
        login();
        QVERIFY(!createPhone().isEmpty());

        m_controller->forgetDevice();
        // "Reset all data" wipes the account right after forgetDevice() returns
        m_core->m_appSettingsRepository->clearSettings();
        QVERIFY(!m_controller->isLoggedIn());

        QTRY_COMPARE(m_server->requestsTo("POST", "/v1/auth/logout").size(), 1);
        QCOMPARE(m_server->requestsTo("DELETE", "/v1/configs/cfg-1").size(), 1);
        // The token is revoked only after the configs are gone, or the deletes would fail with 401
        int deleteIndex = -1;
        int logoutIndex = -1;
        for (int i = 0; i < m_server->requests.size(); ++i) {
            const MockHttpServer::Request &request = m_server->requests.at(i);
            if (request.method == "DELETE") {
                deleteIndex = i;
            } else if (request.method == "POST" && request.path == "/v1/auth/logout") {
                logoutIndex = i;
            }
        }
        QVERIFY(deleteIndex >= 0 && deleteIndex < logoutIndex);
        QCOMPARE(m_server->requestsTo("POST", "/v1/auth/logout").first().headers.value("authorization"),
                 "Bearer " + m_token.toUtf8());
        QVERIFY(m_currentConfig.isEmpty());
    }

    void testDeviceConfigName_data()
    {
        QTest::addColumn<QString>("hostName");
        QTest::addColumn<QStringList>("taken");
        QTest::addColumn<QString>("expected");

        QTest::newRow("mac hostname") << "Noahs-MacBook-Pro" << QStringList() << "noahs-macbook-pro";
        QTest::newRow("mac computer name") << "Noah’s MacBook Pro" << QStringList() << "noahs-macbook-pro";
        QTest::newRow("android settings") << "Galaxy S23 Ultra" << QStringList() << "galaxy-s23-ultra";
        QTest::newRow("cyrillic") << "Ноутбук Лёши PC_1" << QStringList() << "noutbuk-leshi-pc-1";
        QTest::newRow("symbols only") << "★ ✓" << QStringList() << "android";
        QTest::newRow("localhost") << "localhost" << QStringList() << "android";
        QTest::newRow("empty") << "" << QStringList() << "android";
        QTest::newRow("taken") << "laptop" << QStringList { "laptop", "laptop-2" } << "laptop-3";
        QTest::newRow("long") << QString(40, 'a') << QStringList() << QString(28, 'a');
    }

    void testDeviceConfigName()
    {
        QFETCH(QString, hostName);
        QFETCH(QStringList, taken);
        QFETCH(QString, expected);
        const QString name = CoordinatorController::deviceConfigName(hostName, QStringLiteral("android"), taken);
        QCOMPARE(name, expected);
        QVERIFY(QRegularExpression(QStringLiteral("^[a-z0-9-]{1,32}$")).match(name).hasMatch());
    }

    void testToConfigName_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("expected");

        QTest::newRow("already valid") << "pixel-8" << "pixel-8";
        QTest::newRow("upper and spaces") << "  My Phone  " << "my-phone";
        QTest::newRow("cyrillic") << "Телефон мамы" << "telefon-mamy";
        QTest::newRow("empty") << "" << "";
        QTest::newRow("long") << QString(40, 'b') << QString(32, 'b');
        QTest::newRow("cut at dash") << QString(31, 'c') + " d" << QString(31, 'c');
    }

    void testToConfigName()
    {
        QFETCH(QString, text);
        QFETCH(QString, expected);
        QCOMPARE(deviceName::toConfigName(text), expected);
    }

    void testRenameConfig()
    {
        login();
        const QString serverId = createPhone();
        QSignalSpy renamedSpy(m_controller, &CoordinatorController::configRenamed);
        QSignalSpy errorSpy(m_controller, &CoordinatorController::errorOccurred);

        m_controller->renameConfig("cfg-1", "pixel-8");
        QTRY_COMPARE(renamedSpy.count(), 1);
        QCOMPARE(renamedSpy.at(0).at(1).toString(), QStringLiteral("pixel-8"));
        const auto patches = m_server->requestsTo("PATCH", "/v1/configs/cfg-1");
        QCOMPARE(patches.size(), 1);
        QCOMPARE(patches.first().json().value("name").toString(), QStringLiteral("pixel-8"));
        QCOMPARE(m_controller->managedConfigs().value(serverId).toObject().value("name").toString(), QStringLiteral("pixel-8"));
        // The server keeps its name in the app
        QCOMPARE(m_core->m_serversRepository->nativeConfig(serverId)->description, QStringLiteral("phone"));

        m_controller->renameConfig("cfg-1", "taken");
        QTRY_COMPARE(errorSpy.count(), 1);
        QCOMPARE(renamedSpy.count(), 1);
        QCOMPARE(m_controller->managedConfigs().value(serverId).toObject().value("name").toString(), QStringLiteral("pixel-8"));
    }

    void testAwg3ConfigDetectedAs31()
    {
        m_issueAwg3 = true;
        login();
        const QString serverId = createPhone();
        QVERIFY(!serverId.isEmpty());

        const auto server = m_core->m_serversRepository->nativeConfig(serverId);
        QVERIFY(server.has_value());
        QJsonObject awgJson;
        for (const QJsonValue &value : server->toJson().value(configKey::containers).toArray()) {
            if (value.toObject().contains(configKey::awg)) {
                awgJson = value.toObject().value(configKey::awg).toObject();
            }
        }
        QVERIFY(!awgJson.isEmpty());
        const AwgProtocolConfig awg = AwgProtocolConfig::fromJson(awgJson);
        QCOMPARE(awg.clientProtocolVersion(), QString(protocols::awg::awgV3));

        const QJsonObject lastConfig = awgLastConfig(server->toJson());
        QCOMPARE(lastConfig.value(configKey::headerProtectionKey).toString(),
                 QStringLiteral("q5YQqvq3ZsdHo1ag8Grk1zE1gKXlo6VgSc8uzQ6QY5E="));
        QCOMPARE(lastConfig.value(configKey::rekeyAfterTime).toString(), QStringLiteral("110"));
        QCOMPARE(lastConfig.value(configKey::maxHandshakeAttempts).toString(), QStringLiteral("18"));
        QCOMPARE(lastConfig.value(configKey::randomTrailers).toString(), QStringLiteral("on"));
        QCOMPARE(lastConfig.value(configKey::disableCookies).toString(), QStringLiteral("on"));
        QVERIFY(lastConfig.value(configKey::specialJunk1).toString().startsWith("<r 2>"));
        QVERIFY(lastConfig.value(configKey::config).toString().contains("HeaderProtectionKey = "));
    }

    void testRefreshOutdatedReplacesInPlace()
    {
        login();
        const QString serverId = createPhone();
        QVERIFY(!serverId.isEmpty());
        const int serversBefore = m_core->m_serversRepository->serversCount();

        m_currentConfig = configJson("cfg-1", "phone", "rev2", "198.51.100.7:51821");
        m_statusState = "outdated";

        QSignalSpy replacedSpy(m_controller, &CoordinatorController::configReplaced);
        m_controller->refreshAll();
        QTRY_COMPARE(replacedSpy.count(), 1);
        QCOMPARE(replacedSpy.at(0).at(0).toString(), serverId);

        const auto statusRequests = m_server->requestsTo("GET", "/v1/configs/cfg-1/status");
        QCOMPARE(statusRequests.size(), 1);
        QCOMPARE(statusRequests.first().query.queryItemValue("revision"), QStringLiteral("rev1"));
        QCOMPARE(m_server->requestsTo("GET", "/v1/configs/cfg-1").size(), 1);

        QCOMPARE(m_core->m_serversRepository->serversCount(), serversBefore);
        QCOMPARE(m_controller->serverIdForConfig("cfg-1"), serverId);
        const auto server = m_core->m_serversRepository->nativeConfig(serverId);
        QVERIFY(server.has_value());
        QCOMPARE(server->hostName, QStringLiteral("198.51.100.7"));
        QCOMPARE(server->description, QStringLiteral("phone"));
        const QJsonObject lastConfig = awgLastConfig(server->toJson());
        QCOMPARE(lastConfig.value(configKey::hostName).toString(), QStringLiteral("198.51.100.7"));
        QCOMPARE(lastConfig.value(configKey::port).toVariant().toInt(), 51821);
        QCOMPARE(lastConfig.value(configKey::mtu).toString(), QStringLiteral("1280"));
        QVERIFY(lastConfig.value(configKey::config).toString().contains("198.51.100.7:51821"));

        const QJsonObject entry = m_controller->managedConfigs().value(serverId).toObject();
        QCOMPARE(entry.value("revision").toString(), QStringLiteral("rev2"));
        QCOMPARE(entry.value("state").toString(), QStringLiteral("ok"));
    }

    void testServerNamedAfterExitCountry()
    {
        login();
        const QString serverId = createPhone(); // old coordinator: no serverName, named after the config
        QCOMPARE(m_core->m_serversRepository->nativeConfig(serverId)->description, QStringLiteral("phone"));

        // The coordinator starts sending serverName: the server is renamed
        m_currentConfig = configJson("cfg-1", "phone", "rev2", "198.51.100.7:51821");
        m_currentConfig["serverName"] = QStringLiteral("🇳🇱 Нидерланды");
        m_statusState = "outdated";
        QSignalSpy replacedSpy(m_controller, &CoordinatorController::configReplaced);
        m_controller->refreshAll();
        QTRY_COMPARE(replacedSpy.count(), 1);
        QCOMPARE(m_core->m_serversRepository->nativeConfig(serverId)->description, QStringLiteral("🇳🇱 Нидерланды"));

        // The user renames it in the app: later updates keep the user's name
        QJsonObject serverJson = m_core->m_serversRepository->nativeConfig(serverId)->toJson();
        serverJson[configKey::description] = QStringLiteral("Мой VPN");
        m_core->m_serversRepository->editServer(serverId, serverJson, serverConfigUtils::configTypeFromJson(serverJson));
        m_currentConfig["revision"] = "rev3";
        m_currentConfig["serverName"] = QStringLiteral("🇩🇪 Германия");
        m_controller->refreshAll();
        QTRY_COMPARE(replacedSpy.count(), 2);
        QCOMPARE(m_core->m_serversRepository->nativeConfig(serverId)->description, QStringLiteral("Мой VPN"));
    }

    void testCreatedServerUsesServerName()
    {
        login();
        m_currentConfig = configJson("cfg-1", "phone", "rev1", "203.0.113.10:51820");
        m_currentConfig["serverName"] = QStringLiteral("🇳🇱 Нидерланды");
        QSignalSpy addedSpy(m_controller, &CoordinatorController::configAdded);
        m_controller->addConfigToApp("cfg-1");
        QTRY_COMPARE(addedSpy.count(), 1);
        QCOMPARE(m_core->m_serversRepository->nativeConfig(addedSpy.at(0).at(0).toString())->description,
                 QStringLiteral("🇳🇱 Нидерланды"));
    }

    void testLearnsCoordinatorAddress()
    {
        login();
        const int port = QUrl(m_server->baseUrl()).port();
        m_api->setBaseUrl(QStringLiteral("http://localhost:%1").arg(port));
        QSignalSpy learnedSpy(m_api, &CoordinatorApi::addressLearned);
        m_controller->fetchRemoteConfigs();
        QTRY_VERIFY(learnedSpy.count() >= 1);
        QCOMPARE(learnedSpy.at(0).at(0).toString(), QStringLiteral("127.0.0.1"));
        QCOMPARE(m_core->m_appSettingsRepository->coordinatorApiAddress(), QStringLiteral("127.0.0.1"));
        QCOMPARE(m_api->fallbackAddress(), QStringLiteral("127.0.0.1"));
    }

    void testFallsBackToKnownAddressWhenDnsFails()
    {
        login();
        const int port = QUrl(m_server->baseUrl()).port();
        // The host is unreachable (like DNS/connect through a dead tunnel), so the first
        // attempt gets no HTTP answer. We point it at loopback 127.0.0.2, where nothing
        // listens: the connection is refused instantly (httpStatus 0), deterministically,
        // without relying on a real resolver (a CI resolver may hijack bogus names and
        // answer 200, which would skip the fallback). The mock only binds 127.0.0.1.
        const QString unreachableHost = QStringLiteral("127.0.0.2");
        m_api->setBaseUrl(QStringLiteral("http://%1:%2").arg(unreachableHost).arg(port));
        m_api->setFallbackAddress(QStringLiteral("127.0.0.1"));

        QSignalSpy remoteSpy(m_controller, &CoordinatorController::remoteConfigsUpdated);
        QSignalSpy errorSpy(m_controller, &CoordinatorController::errorOccurred);
        m_server->requests.clear();
        m_controller->fetchRemoteConfigs();
        QTRY_COMPARE_WITH_TIMEOUT(m_server->requestsTo("GET", "/v1/me").size(), 1, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(remoteSpy.count(), 1, 15000);
        QCOMPARE(errorSpy.count(), 0);

        const auto meRequests = m_server->requestsTo("GET", "/v1/me");
        QCOMPARE(meRequests.size(), 1);
        QCOMPARE(meRequests.first().headers.value("host"), QStringLiteral("%1:%2").arg(unreachableHost).arg(port).toUtf8());
        QCOMPARE(meRequests.first().headers.value("authorization"), "Bearer " + m_token.toUtf8());

        // Without a known address the failure is reported as before
        m_api->setFallbackAddress(QString());
        m_controller->fetchRemoteConfigs();
        QTRY_COMPARE_WITH_TIMEOUT(errorSpy.count(), 1, 15000);
    }

    void testSuspendedReportedOnce()
    {
        login();
        const QString serverId = createPhone();
        QVERIFY(!serverId.isEmpty());

        m_statusState = "suspended";
        QSignalSpy errorSpy(m_controller, &CoordinatorController::errorOccurred);
        m_controller->refreshAll();
        QTRY_COMPARE(m_server->requestsTo("GET", "/v1/configs/cfg-1/status").size(), 1);
        QTRY_COMPARE(errorSpy.count(), 1);
        m_controller->refreshAll();
        QTRY_COMPARE(m_server->requestsTo("GET", "/v1/configs/cfg-1/status").size(), 2);
        QTest::qWait(200);
        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(m_controller->managedConfigs().value(serverId).toObject().value("state").toString(),
                 QStringLiteral("suspended"));
    }

    void testInvalidTokenLogsOut()
    {
        login();
        const QString serverId = createPhone();
        QVERIFY(!serverId.isEmpty());

        m_statusHttp = 401;
        m_statusErrorCode = "invalid_token";
        QSignalSpy loggedInSpy(m_controller, &CoordinatorController::loggedInChanged);
        QSignalSpy errorSpy(m_controller, &CoordinatorController::errorOccurred);
        m_controller->refreshAll();
        QTRY_VERIFY(!m_controller->isLoggedIn());
        QCOMPARE(loggedInSpy.count(), 1);
        QTRY_COMPARE(errorSpy.count(), 1);
        QVERIFY(m_core->m_appSettingsRepository->coordinatorToken().isEmpty());
        // The server stays in the app as a static config
        QVERIFY(m_core->m_serversRepository->indexOfServerId(serverId) >= 0);
    }

    void testNotFoundMarksRevoked()
    {
        login();
        const QString serverId = createPhone();
        QVERIFY(!serverId.isEmpty());

        m_statusHttp = 404;
        m_statusErrorCode = "not_found";
        QSignalSpy errorSpy(m_controller, &CoordinatorController::errorOccurred);
        m_controller->refreshAll();
        QTRY_COMPARE(m_controller->managedConfigs().value(serverId).toObject().value("state").toString(),
                     QStringLiteral("revoked"));
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(m_controller->isLoggedIn());

        // A revoked config is not polled any more
        m_controller->refreshAll();
        QTest::qWait(200);
        QCOMPARE(m_server->requestsTo("GET", "/v1/configs/cfg-1/status").size(), 1);
    }

    void testConcurrentChecksApplyOnce()
    {
        login();
        const QString serverId = createPhone();
        QVERIFY(!serverId.isEmpty());

        m_currentConfig = configJson("cfg-1", "phone", "rev2", "198.51.100.7:51821");
        m_statusState = "outdated";
        QSignalSpy replacedSpy(m_controller, &CoordinatorController::configReplaced);
        m_controller->refreshAll();
        m_controller->refreshAll(); // e.g. the timer and a connection error at once
        QTRY_COMPARE(replacedSpy.count(), 1);
        QTest::qWait(200);
        QCOMPARE(replacedSpy.count(), 1);
        QCOMPARE(m_server->requestsTo("GET", "/v1/configs/cfg-1/status").size(), 1);
        QCOMPARE(m_server->requestsTo("GET", "/v1/configs/cfg-1").size(), 1);
    }

    void testLoginToNewHostForgetsAddress()
    {
        // Signed out of another coordinator whose address is still known
        m_core->m_appSettingsRepository->setCoordinatorBaseUrl(QStringLiteral("https://old.example.org"));
        m_core->m_appSettingsRepository->setCoordinatorApiAddress(QStringLiteral("203.0.113.5"));
        QSignalSpy addressSpy(m_controller, &CoordinatorController::apiAddressChanged);
        login();
        QVERIFY(m_core->m_appSettingsRepository->coordinatorApiAddress().isEmpty());
        QCOMPARE(addressSpy.count(), 1);

        // Signing in to the same host again keeps it
        m_core->m_appSettingsRepository->setCoordinatorApiAddress(QStringLiteral("127.0.0.1"));
        login();
        QCOMPARE(m_core->m_appSettingsRepository->coordinatorApiAddress(), QStringLiteral("127.0.0.1"));
        QCOMPARE(addressSpy.count(), 1);
    }

    void testImportKeepsIssuerMtu_data()
    {
        QTest::addColumn<QString>("issuerMtu");
        QTest::addColumn<QString>("expectedMtu");
        QTest::newRow("1280 kept") << "1280" << "1280";
        QTest::newRow("1500 -> default") << "1500" << QString(protocols::awg::defaultMtu);
    }

    void testImportKeepsIssuerMtu()
    {
        QFETCH(QString, issuerMtu);
        QFETCH(QString, expectedMtu);

        ImportController *importController = m_core->m_importCoreController;
        const auto awgResult = importController->extractConfigFromData(awgConf("203.0.113.10:51820"));
        QCOMPARE(awgResult.errorCode, ErrorCode::NoError);

        // Build an Amnezia-format config (as in a vpn:// key) with the issuer's MTU in last_config
        QJsonObject amneziaConfig = awgResult.config;
        QJsonArray containers = amneziaConfig.value(configKey::containers).toArray();
        QVERIFY(!containers.isEmpty());
        QJsonObject container = containers.at(0).toObject();
        QJsonObject protocol = container.value(configKey::awg).toObject();
        QJsonObject lastConfig = QJsonDocument::fromJson(protocol.value(configKey::lastConfig).toString().toUtf8()).object();
        lastConfig[configKey::mtu] = issuerMtu;
        lastConfig[configKey::config] = lastConfig.value(configKey::config).toString().replace("MTU = 1280", "MTU = " + issuerMtu);
        protocol[configKey::lastConfig] = QString(QJsonDocument(lastConfig).toJson());
        container[configKey::awg] = protocol;
        containers.replace(0, container);
        amneziaConfig[configKey::containers] = containers;

        const QByteArray compressed = qCompress(QJsonDocument(amneziaConfig).toJson());
        const QString vpnKey = "vpn://"
                + QString::fromLatin1(compressed.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));

        const auto result = importController->extractConfigFromData(vpnKey);
        QCOMPARE(result.errorCode, ErrorCode::NoError);
        QCOMPARE(result.configType, ConfigTypes::Amnezia);
        QCOMPARE(awgLastConfig(result.config).value(configKey::mtu).toString(), expectedMtu);
    }
};

QTEST_MAIN(TestCoordinatorController)
#include "testCoordinatorController.moc"
