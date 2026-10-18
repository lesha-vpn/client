#include "coordinatorController.h"

#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QHostInfo>
#include <QRegularExpression>
#include <QDebug>
#include <QSysInfo>
#include <QUrl>

#include <memory>

#include "core/controllers/connectionController.h"
#include "core/controllers/coordinator/deviceName.h"
#include "core/controllers/selfhosted/importController.h"
#include "core/repositories/secureAppSettingsRepository.h"
#include "core/repositories/secureServersRepository.h"
#include "core/utils/constants/configKeys.h"
#include "core/utils/errorStrings.h"
#include "core/utils/networkUtilities.h"
#include "core/utils/serverConfigUtils.h"

#ifdef AMNEZIA_DESKTOP
    #include "core/utils/ipcClient.h"
#endif

using namespace amnezia;

namespace
{
namespace entryKey
{
constexpr QLatin1String configId("configId");
constexpr QLatin1String name("name");
constexpr QLatin1String revision("revision");
constexpr QLatin1String state("state");
constexpr QLatin1String displayName("displayName"); // server name the app last set from the coordinator
} // namespace entryKey

constexpr qint64 errorRefreshThrottleMsecs = 60 * 1000;
} // namespace

CoordinatorController::CoordinatorController(SecureServersRepository *serversRepository,
                                             SecureAppSettingsRepository *appSettingsRepository, ImportController *importController,
                                             ConnectionController *connectionController, CoordinatorApi *api, QObject *parent)
    : QObject(parent),
      m_serversRepository(serversRepository),
      m_appSettingsRepository(appSettingsRepository),
      m_importController(importController),
      m_connectionController(connectionController),
      m_api(api)
{
    m_api->setBaseUrl(m_appSettingsRepository->coordinatorBaseUrl());
    m_api->setFallbackAddress(m_appSettingsRepository->coordinatorApiAddress());
    m_api->setToken(m_appSettingsRepository->coordinatorToken());

    // Remember where the coordinator lives: the tunnel bypasses it, and requests fall back to it when DNS fails
    connect(m_api, &CoordinatorApi::addressLearned, this, [this](const QString &address) {
        m_api->setFallbackAddress(address);
        if (m_appSettingsRepository->coordinatorApiAddress() != address) {
            m_appSettingsRepository->setCoordinatorApiAddress(address);
            emit apiAddressChanged();
        }
    });

    // A fixed tick: setInterval() on a running timer restarts it, so frequent state flips would starve the check
    m_refreshTimer.setInterval(connectedRefreshIntervalMsecs);
    connect(&m_refreshTimer, &QTimer::timeout, this, &CoordinatorController::onRefreshTimer);

    connect(m_serversRepository, &SecureServersRepository::serverRemoved, this,
            [this](const QString &serverId, int) { onServerRemoved(serverId); });
    connect(m_connectionController, &ConnectionController::connectionStateChanged, this,
            &CoordinatorController::onConnectionStateChanged);

    // "Reset all data" wiped the account and the managed map behind our back
    connect(m_appSettingsRepository, &SecureAppSettingsRepository::settingsCleared, this, [this]() {
        m_api->setToken(QString());
        m_api->setBaseUrl(QString());
        m_api->setFallbackAddress(QString());
        m_refreshTimer.stop();
        m_pendingReconnectServerId.clear();
        emit loggedInChanged();
    });
}

bool CoordinatorController::isLoggedIn() const
{
    return !m_appSettingsRepository->coordinatorToken().isEmpty() && !m_appSettingsRepository->coordinatorBaseUrl().isEmpty();
}

QString CoordinatorController::baseUrl() const
{
    return m_appSettingsRepository->coordinatorBaseUrl();
}

QString CoordinatorController::username() const
{
    // username may be null for Telegram users without one; tgId is the identity then
    const QJsonObject user = m_appSettingsRepository->coordinatorUser();
    const QString name = user.value("username").toString();
    if (!name.isEmpty()) {
        return QStringLiteral("@") + name;
    }
    const QJsonValue tgId = user.value("tgId");
    const QString id = tgId.isString() ? tgId.toString() : QString::number(tgId.toInteger());
    return tgId.isUndefined() || tgId.isNull() ? QString() : QStringLiteral("Telegram ID %1").arg(id);
}

void CoordinatorController::start()
{
    if (!isLoggedIn()) {
        return;
    }
    m_refreshTimer.start();
    QTimer::singleShot(0, this, &CoordinatorController::refreshAll);
}

void CoordinatorController::login(const QString &baseUrl, const QString &code)
{
    QString url = baseUrl.trimmed();
    if (!url.isEmpty() && !url.contains(QLatin1String("://"))) {
        url.prepend(QLatin1String("https://"));
    }
    const QUrl parsedUrl(url);
    const bool loopback = parsedUrl.host() == QLatin1String("localhost") || parsedUrl.host() == QLatin1String("127.0.0.1");
    if (!parsedUrl.isValid() || parsedUrl.host().isEmpty()
        || (parsedUrl.scheme() != QLatin1String("https") && !(parsedUrl.scheme() == QLatin1String("http") && loopback))) {
        // The device token must never travel in clear text
        emit loginFinished(false, tr("Enter a server address starting with https://"));
        return;
    }
    QString normalizedCode = code;
    normalizedCode.remove(QRegularExpression(QStringLiteral("\\s")));
    normalizedCode = normalizedCode.toUpper();

    m_api->setBaseUrl(url);
    url = m_api->baseUrl(); // without trailing slashes
    allowThroughKillSwitch();
    beginBusy();
    m_api->login(normalizedCode, deviceLabel(), [this, url](const CoordinatorApi::Reply &reply) {
        endBusy();
        const QString token = reply.body.value("token").toString();
        if (!reply.ok() || token.isEmpty()) {
            m_api->setBaseUrl(m_appSettingsRepository->coordinatorBaseUrl());
            m_api->setFallbackAddress(m_appSettingsRepository->coordinatorApiAddress());
            emit loginFinished(false, describeError(reply));
            return;
        }

        // The known address belongs to the previous host (setBaseUrl() already dropped the API's copy)
        const bool hostChanged = QUrl(url).host() != QUrl(m_appSettingsRepository->coordinatorBaseUrl()).host();
        if (hostChanged && !m_appSettingsRepository->coordinatorApiAddress().isEmpty()) {
            m_appSettingsRepository->setCoordinatorApiAddress(QString());
            emit apiAddressChanged();
        }
        m_appSettingsRepository->setCoordinatorBaseUrl(url);
        m_appSettingsRepository->setCoordinatorToken(token);
        m_appSettingsRepository->setCoordinatorUser(reply.body.value("user").toObject());
        m_api->setToken(token);
        m_notMemberReported = false;

        emit loggedInChanged();
        emit loginFinished(true, QString());
        m_refreshTimer.start();
        // The login page follows up with setupThisDevice(), which also refreshes the remote list
    });
}

void CoordinatorController::logout()
{
    if (isLoggedIn()) {
        allowThroughKillSwitch();
        m_api->logout([](const CoordinatorApi::Reply &reply) {
            if (!reply.ok()) {
                qWarning() << "CoordinatorController::logout(): server-side logout failed:" << reply.httpStatus << reply.errorMessage;
            }
        });
    }
    // The request above already captured the token; it is safe to forget it now.
    m_appSettingsRepository->clearCoordinatorAccount();
    m_api->setToken(QString());
    m_refreshTimer.stop();
    emit loggedInChanged();
}

void CoordinatorController::forgetDevice()
{
    if (!isLoggedIn()) {
        return;
    }
    allowThroughKillSwitch();
    QStringList configIds;
    const QJsonObject managed = managedConfigs();
    for (auto it = managed.constBegin(); it != managed.constEnd(); ++it) {
        const QJsonObject entry = it.value().toObject();
        if (entry.value(entryKey::state).toString() != QLatin1String("revoked")) {
            configIds.append(entry.value(entryKey::configId).toString());
        }
    }

    // The reset wipes the account right after this returns: requests capture the token when sent, and the logout,
    // which must wait for the deletes (a revoked token would fail them with 401), carries its own copy.
    const QString baseUrl = m_api->baseUrl();
    const QString token = m_appSettingsRepository->coordinatorToken();
    CoordinatorApi *api = m_api;
    const auto logout = [api, baseUrl, token]() {
        api->logout(baseUrl, token, [](const CoordinatorApi::Reply &reply) {
            if (!reply.ok()) {
                qWarning() << "CoordinatorController::forgetDevice(): server-side logout failed:" << reply.httpStatus << reply.errorMessage;
            }
        });
    };
    if (configIds.isEmpty()) {
        logout();
        return;
    }
    const auto pending = std::make_shared<int>(configIds.size());
    for (const QString &configId : std::as_const(configIds)) {
        m_api->deleteConfig(configId, [pending, logout](const CoordinatorApi::Reply &reply) {
            if (!reply.ok() && !reply.notFound()) {
                qWarning() << "CoordinatorController::forgetDevice(): config revoke failed:" << reply.httpStatus << reply.errorMessage;
            }
            if (--*pending == 0) {
                logout();
            }
        });
    }
}

void CoordinatorController::fetchRemoteConfigs()
{
    if (!isLoggedIn()) {
        return;
    }
    beginBusy();
    requestMe(true, [this](const std::optional<QJsonArray> &) { endBusy(); });
}

void CoordinatorController::requestMe(bool notify, const std::function<void(const std::optional<QJsonArray> &)> &done)
{
    allowThroughKillSwitch();
    m_api->me([this, notify, done](const CoordinatorApi::Reply &reply) {
        if (!reply.ok()) {
            reportError(reply);
            done(std::nullopt);
            return;
        }
        m_notMemberReported = false;

        const QJsonObject user = reply.body.value("user").toObject();
        if (!user.isEmpty()) {
            m_appSettingsRepository->setCoordinatorUser(user);
        }

        const QJsonArray configs = reconcile(reply.body.value("configs").toArray(), notify);
        emit remoteConfigsUpdated(configs);
        done(configs);
    });
}

QJsonArray CoordinatorController::reconcile(const QJsonArray &remoteConfigs, bool notify)
{
    QHash<QString, QString> remoteStates; // configId -> state
    for (const QJsonValue &value : remoteConfigs) {
        const QJsonObject summary = value.toObject();
        remoteStates.insert(summary.value("id").toString(), summary.value("state").toString());
    }

    // /me lists every config of the account that isn't revoked: a managed config missing there is gone for good
    // (revoked in the bot, or left over from another account or coordinator)
    const QJsonObject managed = managedConfigs();
    for (auto it = managed.constBegin(); it != managed.constEnd(); ++it) {
        const QJsonObject entry = it.value().toObject();
        const QString configId = entry.value(entryKey::configId).toString();
        const QString name = entry.value(entryKey::name).toString();
        if (!remoteStates.contains(configId)) {
            if (setManagedState(it.key(), QStringLiteral("revoked")) && notify) {
                emit errorOccurred(tr("Config \"%1\" was deleted").arg(name));
            }
        } else if (remoteStates.value(configId) == QLatin1String("suspended")) {
            setManagedState(it.key(), QStringLiteral("suspended"));
        } else if (entry.value(entryKey::state).toString() != QLatin1String("ok")
                   && entry.value(entryKey::state).toString() != QLatin1String("outdated")) {
            setManagedState(it.key(), QStringLiteral("unknown")); // active again; the next status check settles it
        }
    }

    const QJsonObject updated = managedConfigs();
    QJsonArray result;
    for (const QJsonValue &value : remoteConfigs) {
        QJsonObject summary = value.toObject();
        const QString configId = summary.value("id").toString();
        const QString serverId = serverIdForConfig(configId);
        summary["inApp"] = !serverId.isEmpty();
        // Follow renames made elsewhere (the bot); a rename doesn't change the revision, so applyConfig won't see it
        const QString name = summary.value("name").toString();
        if (!serverId.isEmpty() && !name.isEmpty()) {
            QJsonObject entry = updated.value(serverId).toObject();
            if (entry.value(entryKey::name).toString() != name) {
                entry[entryKey::name] = name;
                setManagedEntry(serverId, entry);
            }
        }
        result.append(summary);
    }
    return result;
}

QString CoordinatorController::revokedServerToReplace() const
{
    const QJsonObject managed = managedConfigs();
    const QString defaultServerId = m_serversRepository->defaultServerId();
    QString candidate;
    for (auto it = managed.constBegin(); it != managed.constEnd(); ++it) {
        if (it.value().toObject().value(entryKey::state).toString() != QLatin1String("revoked")
            || m_serversRepository->indexOfServerId(it.key()) < 0) {
            continue;
        }
        if (it.key() == defaultServerId) {
            return it.key();
        }
        if (candidate.isEmpty()) {
            candidate = it.key();
        }
    }
    return candidate;
}

void CoordinatorController::createConfig(const QString &name)
{
    if (!isLoggedIn()) {
        return;
    }
    allowThroughKillSwitch();
    beginBusy();
    m_api->createConfig(name.trimmed(), [this](const CoordinatorApi::Reply &reply) {
        endBusy();
        if (reply.ok()) {
            applyConfig(reply.body, QString());
        } else if (reportError(reply)) {
            return;
        }
        fetchRemoteConfigs();
    });
}

void CoordinatorController::addConfigToApp(const QString &configId)
{
    if (!isLoggedIn()) {
        return;
    }
    const QString existingServerId = serverIdForConfig(configId);
    if (!existingServerId.isEmpty()) {
        allowThroughKillSwitch();
        checkConfig(existingServerId);
        return;
    }

    allowThroughKillSwitch();
    beginBusy();
    m_api->getConfig(configId, [this](const CoordinatorApi::Reply &reply) {
        endBusy();
        if (reply.ok()) {
            applyConfig(reply.body, QString());
        } else if (reportError(reply)) {
            return;
        }
        fetchRemoteConfigs();
    });
}

void CoordinatorController::setupThisDevice()
{
    if (!isLoggedIn()) {
        emit deviceSetupFinished(false);
        return;
    }

    beginBusy();
    // Managed entries left from a revoked config or another account don't count as this device's config
    requestMe(false, [this](const std::optional<QJsonArray> &remoteConfigs) {
        if (!remoteConfigs) {
            endBusy();
            emit deviceSetupFinished(false);
            return;
        }
        QStringList takenNames;
        bool hasLiveConfig = false;
        for (const QJsonValue &value : *remoteConfigs) {
            const QJsonObject summary = value.toObject();
            takenNames.append(summary.value("name").toString());
            hasLiveConfig = hasLiveConfig || summary.value("inApp").toBool();
        }
        if (hasLiveConfig) {
            endBusy();
            emit deviceSetupFinished(true);
            return;
        }

        const QString name = deviceConfigName(deviceName::platformName(), QSysInfo::productType(), takenNames);
        m_api->createConfig(name, [this](const CoordinatorApi::Reply &reply) {
            endBusy();
            bool added = false;
            if (reply.ok()) {
                // Take the place of a dead config instead of leaving it next to the new one
                const QString replacedServerId = revokedServerToReplace();
                added = applyConfig(reply.body, replacedServerId);
                if (added && !replacedServerId.isEmpty()) {
                    reconnectIfActive(replacedServerId);
                }
            } else {
                reportError(reply);
            }
            emit deviceSetupFinished(added);
            if (isLoggedIn()) {
                fetchRemoteConfigs();
            }
        });
    });
}

QString CoordinatorController::deviceConfigName(const QString &deviceName, const QString &fallback, const QStringList &takenNames)
{
    // 28 leaves room for a "-NN" suffix
    QString base = deviceName.trimmed().compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0
            ? QString()
            : deviceName::toConfigName(deviceName, 28);
    if (base.isEmpty()) {
        base = deviceName::toConfigName(fallback, 28);
    }
    if (base.isEmpty()) {
        base = QStringLiteral("device");
    }

    QString name = base;
    for (int i = 2; takenNames.contains(name); ++i) {
        name = QStringLiteral("%1-%2").arg(base).arg(i);
    }
    return name;
}

void CoordinatorController::renameConfig(const QString &configId, const QString &name)
{
    if (!isLoggedIn()) {
        return;
    }
    allowThroughKillSwitch();
    beginBusy();
    m_api->renameConfig(configId, name, [this, configId](const CoordinatorApi::Reply &reply) {
        endBusy();
        if (reply.ok()) {
            const QString newName = reply.body.value("name").toString();
            const QString serverId = serverIdForConfig(configId);
            if (!serverId.isEmpty()) {
                QJsonObject entry = managedConfigs().value(serverId).toObject();
                entry[entryKey::name] = newName;
                setManagedEntry(serverId, entry);
            }
            emit configRenamed(configId, newName);
        } else if (reportError(reply)) {
            return;
        }
        fetchRemoteConfigs();
    });
}

void CoordinatorController::deleteConfig(const QString &configId)
{
    if (!isLoggedIn()) {
        return;
    }
    allowThroughKillSwitch();
    beginBusy();
    m_api->deleteConfig(configId, [this, configId](const CoordinatorApi::Reply &reply) {
        endBusy();
        if (reply.ok() || reply.notFound()) {
            const QString serverId = serverIdForConfig(configId);
            if (!serverId.isEmpty()) {
                if (isConnectionActive() && m_serversRepository->defaultServerId() == serverId) {
                    // Also while connecting or reconnecting: the revoked key must not stay up
                    m_pendingReconnectServerId.clear();
                    m_connectionController->closeConnection();
                }
                if (m_serversRepository->indexOfServerId(serverId) >= 0) {
                    m_serversRepository->removeServer(serverId); // onServerRemoved() drops the managed entry
                } else {
                    setManagedEntry(serverId, QJsonObject());
                }
            }
            emit configDeleted(configId);
        } else if (reportError(reply)) {
            return;
        }
        fetchRemoteConfigs();
    });
}

void CoordinatorController::refreshAll()
{
    if (!isLoggedIn()) {
        return;
    }
    m_lastRefreshMsecs = QDateTime::currentMSecsSinceEpoch();
    // A revoked config never comes back by itself; GET /me (reconcile) resets the state if it reappears
    QStringList serverIds;
    const QJsonObject managed = managedConfigs();
    for (auto it = managed.constBegin(); it != managed.constEnd(); ++it) {
        if (it.value().toObject().value(entryKey::state).toString() != QLatin1String("revoked")) {
            serverIds.append(it.key());
        }
    }
    if (serverIds.isEmpty()) {
        return;
    }
    allowThroughKillSwitch();
    for (const QString &serverId : std::as_const(serverIds)) {
        checkConfig(serverId);
    }
}

void CoordinatorController::onRefreshTimer()
{
    const bool connected = m_connectionState == Vpn::ConnectionState::Connected
            && isManagedServer(m_serversRepository->defaultServerId());
    if (!connected && QDateTime::currentMSecsSinceEpoch() - m_lastRefreshMsecs < refreshIntervalMsecs) {
        return;
    }
    refreshAll();
}

QString CoordinatorController::serverIdForConfig(const QString &configId) const
{
    if (configId.isEmpty()) {
        return QString();
    }
    const QJsonObject managed = managedConfigs();
    for (auto it = managed.constBegin(); it != managed.constEnd(); ++it) {
        if (it.value().toObject().value(entryKey::configId).toString() == configId) {
            return it.key();
        }
    }
    return QString();
}

QString CoordinatorController::configIdForServer(const QString &serverId) const
{
    return managedConfigs().value(serverId).toObject().value(entryKey::configId).toString();
}

bool CoordinatorController::isManagedServer(const QString &serverId) const
{
    return !serverId.isEmpty() && managedConfigs().contains(serverId);
}

QJsonObject CoordinatorController::managedConfigs() const
{
    return m_appSettingsRepository->coordinatorConfigs();
}

void CoordinatorController::onConnectionStateChanged(Vpn::ConnectionState state)
{
    m_connectionState = state;
    // The service resets the kill switch's allowed ranges as the connection goes up or down
    m_killSwitchAllowed.clear();

    if (!m_pendingReconnectServerId.isEmpty()
        && (state == Vpn::ConnectionState::Disconnected || state == Vpn::ConnectionState::Error)) {
        const QString serverId = m_pendingReconnectServerId;
        const bool fresh = QDateTime::currentMSecsSinceEpoch() - m_pendingReconnectMsecs <= pendingReconnectTimeoutMsecs;
        m_pendingReconnectServerId.clear();
        if (fresh && m_serversRepository->indexOfServerId(serverId) >= 0) {
            const ErrorCode error = m_connectionController->openConnection(serverId);
            if (error != ErrorCode::NoError) {
                qWarning() << "CoordinatorController: reconnect after config update failed:" << errorString(error);
            }
            return;
        }
    }

    if (state == Vpn::ConnectionState::Error && isLoggedIn()) {
        const QString serverId = m_serversRepository->defaultServerId();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (isManagedServer(serverId) && now - m_lastErrorRefreshMsecs >= errorRefreshThrottleMsecs) {
            m_lastErrorRefreshMsecs = now;
            allowThroughKillSwitch();
            checkConfig(serverId);
        }
    }
}

bool CoordinatorController::isConnectionActive() const
{
    switch (m_connectionState) {
    case Vpn::ConnectionState::Connected:
    case Vpn::ConnectionState::Connecting:
    case Vpn::ConnectionState::Reconnecting:
    case Vpn::ConnectionState::Preparing:
        return true;
    default:
        return false;
    }
}

void CoordinatorController::onServerRemoved(const QString &serverId)
{
    if (isManagedServer(serverId)) {
        setManagedEntry(serverId, QJsonObject());
    }
    if (m_pendingReconnectServerId == serverId) {
        m_pendingReconnectServerId.clear();
    }
}

void CoordinatorController::checkConfig(const QString &serverId)
{
    if (!isLoggedIn()) {
        return;
    }
    const QJsonObject entry = managedConfigs().value(serverId).toObject();
    const QString configId = entry.value(entryKey::configId).toString();
    if (configId.isEmpty() || m_checksInFlight.contains(serverId)) {
        return; // one at a time: two downloads of the same update would apply and reconnect twice
    }

    m_checksInFlight.insert(serverId);
    m_api->configStatus(configId, entry.value(entryKey::revision).toString(), [this, serverId, configId](const CoordinatorApi::Reply &reply) {
        m_checksInFlight.remove(serverId);
        if (reply.networkError) {
            qWarning() << "CoordinatorController: status check failed for" << configId << reply.errorMessage;
            return;
        }
        if (reply.unauthorized() || reply.notMember()) {
            handleAuthError(reply);
            return;
        }
        if (!isManagedServer(serverId)) {
            return; // removed while the request was in flight
        }

        const QString name = managedConfigs().value(serverId).toObject().value(entryKey::name).toString();
        auto markRevoked = [this, serverId, name]() {
            if (setManagedState(serverId, QStringLiteral("revoked"))) {
                emit errorOccurred(tr("Config \"%1\" was deleted").arg(name));
            }
        };

        if (reply.notFound()) {
            markRevoked();
            return;
        }
        if (!reply.ok()) {
            qWarning() << "CoordinatorController: status check failed for" << configId << reply.httpStatus << reply.errorMessage;
            return;
        }
        m_notMemberReported = false;

        const QString network = reply.body.value("network").toString();
        if (!network.isEmpty() && network != m_lastNetworkState) {
            m_lastNetworkState = network;
            emit networkStateChanged(network);
        }

        const QString state = reply.body.value("state").toString();
        if (state == QLatin1String("ok")) {
            setManagedState(serverId, state);
        } else if (state == QLatin1String("outdated")) {
            m_checksInFlight.insert(serverId);
            m_api->getConfig(configId, [this, serverId, markRevoked](const CoordinatorApi::Reply &configReply) {
                m_checksInFlight.remove(serverId);
                if (!isManagedServer(serverId)) {
                    return;
                }
                if (configReply.ok()) {
                    const QString revision = configReply.body.value("revision").toString();
                    if (!revision.isEmpty()
                        && revision == managedConfigs().value(serverId).toObject().value(entryKey::revision).toString()) {
                        setManagedState(serverId, QStringLiteral("ok")); // already applied
                    } else if (applyConfig(configReply.body, serverId)) {
                        reconnectIfActive(serverId);
                    }
                } else if (configReply.unauthorized() || configReply.notMember()) {
                    handleAuthError(configReply);
                } else if (configReply.notFound()) {
                    markRevoked();
                } else {
                    qWarning() << "CoordinatorController: config download failed:" << configReply.httpStatus << configReply.errorMessage;
                }
            });
        } else if (state == QLatin1String("suspended")) {
            if (setManagedState(serverId, state)) {
                emit errorOccurred(tr("VPN access is suspended: you are not a member of the chat"));
            }
        } else if (state == QLatin1String("revoked")) {
            markRevoked();
        } else {
            qWarning() << "CoordinatorController: unknown config state" << state;
        }
    });
}

bool CoordinatorController::applyConfig(const QJsonObject &config, const QString &existingServerId)
{
    const QString configId = config.value("id").toString();
    const QString name = config.value("name").toString();
    const QString awgText = config.value("awg").toString();

    const ImportController::ImportResult result = m_importController->extractConfigFromData(awgText);
    if (configId.isEmpty() || awgText.isEmpty() || result.errorCode != ErrorCode::NoError || result.config.isEmpty()) {
        qWarning() << "CoordinatorController: failed to parse config" << configId << errorString(result.errorCode);
        emit errorOccurred(tr("Could not import config \"%1\"").arg(name));
        return false;
    }

    // The server is named after the exit country ("serverName"); older coordinators only send the config name
    const QString displayName = config.value("serverName").toString().trimmed().isEmpty()
            ? name
            : config.value("serverName").toString().trimmed();

    QJsonObject serverJson = result.config;
    serverJson[configKey::description] = displayName;
    const serverConfigUtils::ConfigType kind = serverConfigUtils::configTypeFromJson(serverJson);
    if (kind == serverConfigUtils::ConfigType::Invalid || !serverConfigUtils::isConfigFormatVersionSupported(serverJson)) {
        // The repository would silently keep the old config, and the new revision would never be retried
        qWarning() << "CoordinatorController: unsupported config" << configId;
        emit errorOccurred(tr("Could not import config \"%1\"").arg(name));
        return false;
    }

    const bool replace = !existingServerId.isEmpty() && m_serversRepository->indexOfServerId(existingServerId) >= 0;
    QString serverId = existingServerId;
    if (replace) {
        // Keep a name the user gave the server in the app; a name the coordinator set follows the coordinator.
        const auto current = m_serversRepository->nativeConfig(existingServerId);
        const QJsonObject previous = managedConfigs().value(existingServerId).toObject();
        const QString previousDisplayName = previous.contains(entryKey::displayName)
                ? previous.value(entryKey::displayName).toString()
                : previous.value(entryKey::name).toString();
        if (current.has_value() && !current->description.isEmpty() && current->description != previousDisplayName) {
            serverJson[configKey::description] = current->description;
        }
        m_serversRepository->editServer(existingServerId, serverJson, kind);
    } else {
        if (!existingServerId.isEmpty()) {
            setManagedEntry(existingServerId, QJsonObject()); // stale entry, server is gone
        }
        const bool wasConnected = m_connectionController->isConnected();
        serverId = m_serversRepository->addServer(QString(), serverJson, kind);
        if (serverId.isEmpty() || m_serversRepository->indexOfServerId(serverId) < 0) {
            emit errorOccurred(tr("Could not add config \"%1\"").arg(name));
            return false;
        }
        if (!wasConnected || m_serversRepository->serversCount() == 1) {
            m_serversRepository->setDefaultServer(serverId);
        }
    }

    QJsonObject entry;
    entry[entryKey::configId] = configId;
    entry[entryKey::name] = name;
    entry[entryKey::displayName] = displayName;
    entry[entryKey::revision] = config.value("revision").toString();
    entry[entryKey::state] = QStringLiteral("ok");
    setManagedEntry(serverId, entry);

    if (replace) {
        emit configReplaced(serverId);
    } else {
        emit configAdded(serverId);
    }
    return true;
}

void CoordinatorController::handleAuthError(const CoordinatorApi::Reply &reply)
{
    if (reply.unauthorized()) {
        if (!isLoggedIn() || reply.token != m_appSettingsRepository->coordinatorToken()) {
            return; // already handled by a parallel request, or sent with a token replaced by a new login since
        }
        m_appSettingsRepository->clearCoordinatorAccount();
        m_api->setToken(QString());
        m_refreshTimer.stop();
        emit loggedInChanged();
        emit errorOccurred(describeError(reply));
    } else if (reply.notMember()) {
        if (!m_notMemberReported) {
            m_notMemberReported = true;
            emit errorOccurred(describeError(reply));
        }
    }
}

bool CoordinatorController::reportError(const CoordinatorApi::Reply &reply)
{
    if (reply.unauthorized() || reply.notMember()) {
        handleAuthError(reply);
        return true;
    }
    emit errorOccurred(describeError(reply));
    return false;
}

QString CoordinatorController::describeError(const CoordinatorApi::Reply &reply) const
{
    // Server messages are English and meant for logs; the user only sees these
    if (reply.networkError) {
        qWarning() << "CoordinatorController: request failed:" << reply.errorMessage;
        return tr("No connection to the Лёша VPN server. Check your internet connection and try again");
    }
    if (reply.errorCode == QLatin1String("invalid_code")) {
        return tr("Wrong or expired code");
    }
    if (reply.errorCode == QLatin1String("rate_limited") || reply.httpStatus == 429) {
        return tr("Too many attempts. Try again in a minute");
    }
    if (reply.errorCode == QLatin1String("name_taken")) {
        return tr("This name is already taken");
    }
    if (reply.errorCode == QLatin1String("invalid_name")) {
        return tr("The name may only contain Latin letters, digits and hyphens, up to 32 characters");
    }
    if (reply.notMember()) {
        return tr("You are no longer a member of the chat, VPN access is suspended");
    }
    if (reply.unauthorized()) {
        return tr("You have been signed out. Sign in again with a code from the bot");
    }
    if (reply.notFound()) {
        return tr("Config not found");
    }
    qWarning() << "CoordinatorController: request failed:" << reply.httpStatus << reply.errorCode << reply.errorMessage;
    return tr("Лёша VPN server error (HTTP %1). Try again later").arg(reply.httpStatus);
}

void CoordinatorController::setManagedEntry(const QString &serverId, const QJsonObject &entry)
{
    QJsonObject managed = managedConfigs();
    if (entry.isEmpty()) {
        if (!managed.contains(serverId)) {
            return;
        }
        managed.remove(serverId);
    } else {
        if (managed.value(serverId).toObject() == entry) {
            return;
        }
        managed[serverId] = entry;
    }
    m_appSettingsRepository->setCoordinatorConfigs(managed);
}

bool CoordinatorController::setManagedState(const QString &serverId, const QString &state)
{
    QJsonObject entry = managedConfigs().value(serverId).toObject();
    if (entry.isEmpty() || entry.value(entryKey::state).toString() == state) {
        return false;
    }
    entry[entryKey::state] = state;
    setManagedEntry(serverId, entry);
    return true;
}

void CoordinatorController::reconnectIfActive(const QString &serverId)
{
    if (m_serversRepository->defaultServerId() != serverId) {
        return;
    }
    if (isConnectionActive()) {
        m_pendingReconnectServerId = serverId;
        m_pendingReconnectMsecs = QDateTime::currentMSecsSinceEpoch();
        m_connectionController->closeConnection();
    } else if (m_connectionState == Vpn::ConnectionState::Error) {
        // The old config failed (dead entry node); the new one is the fix, so use it right away
        const ErrorCode error = m_connectionController->openConnection(serverId);
        if (error != ErrorCode::NoError) {
            qWarning() << "CoordinatorController: reconnect after config update failed:" << errorString(error);
        }
    }
}

void CoordinatorController::beginBusy()
{
    if (m_busyCount++ == 0) {
        emit busyChanged(true);
    }
}

void CoordinatorController::endBusy()
{
    if (m_busyCount > 0 && --m_busyCount == 0) {
        emit busyChanged(false);
    }
}

void CoordinatorController::allowThroughKillSwitch()
{
#ifdef AMNEZIA_DESKTOP
    if (!m_appSettingsRepository->isStrictKillSwitchEnabled()) {
        m_killSwitchAllowed.clear(); // turning it on again starts from a fresh ruleset
        return;
    }
    // Every call reapplies the whole firewall ruleset and blocks for up to a second: only send new addresses
    auto allow = [this](const QStringList &ips) {
        QStringList added;
        for (const QString &ip : ips) {
            if (!m_killSwitchAllowed.contains(ip) && !added.contains(ip)) {
                added.append(ip);
            }
        }
        if (added.isEmpty()) {
            return;
        }
        IpcClient::withInterface([&](QSharedPointer<IpcInterfaceReplica> iface) {
            QRemoteObjectPendingReply<bool> reply = iface->addKillSwitchAllowedRange(added);
            if (!reply.waitForFinished(1000) || !reply.returnValue()) {
                qWarning() << "CoordinatorController::allowThroughKillSwitch(): Failed to execute remote addKillSwitchAllowedRange call";
                return;
            }
            for (const QString &ip : std::as_const(added)) {
                m_killSwitchAllowed.insert(ip);
            }
        });
    };

    // The known address goes first: DNS may be unreachable through a dead tunnel, and a blocking lookup would
    // freeze the UI. The current address is added when the lookup comes back.
    const QString fallback = m_api->fallbackAddress();
    if (!fallback.isEmpty()) {
        allow({ fallback });
    }
    const QString host = m_api->host();
    if (!host.isEmpty() && QHostAddress(host).isNull()) {
        QHostInfo::lookupHost(host, this, [allow, fallback](const QHostInfo &info) {
            QStringList ips;
            for (const QHostAddress &address : info.addresses()) {
                if (address.protocol() == QAbstractSocket::IPv4Protocol && address.toString() != fallback) {
                    ips.append(address.toString());
                }
            }
            allow(ips);
        });
    } else if (!host.isEmpty() && host != fallback) {
        allow({ host });
    }
#endif
}

QString CoordinatorController::deviceLabel() const
{
    const QString name = deviceName::platformName();
    const QString label = name.isEmpty() ? QSysInfo::prettyProductName()
                                         : QStringLiteral("%1 (%2)").arg(name, QSysInfo::prettyProductName());
    return label.left(64); // the coordinator rejects longer labels
}
