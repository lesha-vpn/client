#include "coordinatorController.h"

#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QHostInfo>
#include <QRegularExpression>
#include <QDebug>
#include <QSysInfo>
#include <QUrl>

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

// Windows has no flag emoji: a flag (two regional indicator symbols) renders as its letter code ("NL").
// Drop the leading flag of the coordinator's "🇳🇱 Нидерланды" there.
QString platformServerName(const QString &serverName)
{
#ifdef Q_OS_WIN
    const QList<uint> codepoints = serverName.toUcs4();
    qsizetype i = 0;
    while (i < codepoints.size() && codepoints.at(i) >= 0x1F1E6 && codepoints.at(i) <= 0x1F1FF) {
        ++i;
    }
    if (i > 0) {
        const QString rest = QString::fromUcs4(reinterpret_cast<const char32_t *>(codepoints.constData() + i), codepoints.size() - i)
                                     .trimmed();
        if (!rest.isEmpty()) {
            return rest;
        }
    }
#endif
    return serverName;
}
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
        }
    });

    m_refreshTimer.setInterval(refreshIntervalMsecs);
    connect(&m_refreshTimer, &QTimer::timeout, this, &CoordinatorController::refreshAll);

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
        emit managedConfigsChanged();
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
    while (url.endsWith('/')) {
        url.chop(1);
    }
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
    // Requests capture the token when sent, so the reset may wipe it right after this returns
    const QJsonObject managed = managedConfigs();
    for (auto it = managed.constBegin(); it != managed.constEnd(); ++it) {
        const QJsonObject entry = it.value().toObject();
        if (entry.value(entryKey::state).toString() == QLatin1String("revoked")) {
            continue;
        }
        m_api->deleteConfig(entry.value(entryKey::configId).toString(), [](const CoordinatorApi::Reply &reply) {
            if (!reply.ok() && !reply.notFound()) {
                qWarning() << "CoordinatorController::forgetDevice(): config revoke failed:" << reply.httpStatus << reply.errorMessage;
            }
        });
    }
    m_api->logout([](const CoordinatorApi::Reply &) {});
}

void CoordinatorController::fetchRemoteConfigs()
{
    if (!isLoggedIn()) {
        return;
    }
    allowThroughKillSwitch();
    beginBusy();
    m_api->me([this](const CoordinatorApi::Reply &reply) {
        endBusy();
        if (!reply.ok()) {
            if (reply.unauthorized() || reply.notMember()) {
                handleAuthError(reply);
            } else {
                emit errorOccurred(describeError(reply));
            }
            return;
        }
        m_notMemberReported = false;

        const QJsonObject user = reply.body.value("user").toObject();
        if (!user.isEmpty()) {
            m_appSettingsRepository->setCoordinatorUser(user);
        }

        emit remoteConfigsUpdated(reconcile(reply.body.value("configs").toArray(), true));
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
        if (summary.value("state").toString().isEmpty() && !serverId.isEmpty()) {
            summary["state"] = updated.value(serverId).toObject().value(entryKey::state).toString();
        }
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
        } else if (reply.unauthorized() || reply.notMember()) {
            handleAuthError(reply);
            return;
        } else {
            emit errorOccurred(describeError(reply));
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
        } else if (reply.unauthorized() || reply.notMember()) {
            handleAuthError(reply);
            return;
        } else {
            emit errorOccurred(describeError(reply));
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

    allowThroughKillSwitch();
    beginBusy();
    m_api->me([this](const CoordinatorApi::Reply &reply) {
        if (!reply.ok()) {
            endBusy();
            if (reply.unauthorized() || reply.notMember()) {
                handleAuthError(reply);
            } else {
                emit errorOccurred(describeError(reply));
            }
            emit deviceSetupFinished(false);
            return;
        }
        m_notMemberReported = false;
        const QJsonObject user = reply.body.value("user").toObject();
        if (!user.isEmpty()) {
            m_appSettingsRepository->setCoordinatorUser(user);
        }

        // Managed entries left from a revoked config or another account don't count as this device's config
        const QJsonArray remoteConfigs = reconcile(reply.body.value("configs").toArray(), false);
        emit remoteConfigsUpdated(remoteConfigs);
        QStringList takenNames;
        bool hasLiveConfig = false;
        for (const QJsonValue &value : remoteConfigs) {
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
                added = applyConfig(reply.body, revokedServerToReplace());
            } else if (reply.unauthorized() || reply.notMember()) {
                handleAuthError(reply);
            } else {
                emit errorOccurred(describeError(reply));
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
        } else if (reply.unauthorized() || reply.notMember()) {
            handleAuthError(reply);
            return;
        } else {
            emit errorOccurred(describeError(reply));
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
                const bool active = m_connectionState == Vpn::ConnectionState::Connected
                        || m_connectionState == Vpn::ConnectionState::Connecting
                        || m_connectionState == Vpn::ConnectionState::Reconnecting
                        || m_connectionState == Vpn::ConnectionState::Preparing;
                if (active && m_serversRepository->defaultServerId() == serverId) {
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
        } else if (reply.unauthorized() || reply.notMember()) {
            handleAuthError(reply);
            return;
        } else {
            emit errorOccurred(describeError(reply));
        }
        fetchRemoteConfigs();
    });
}

void CoordinatorController::refreshAll()
{
    if (!isLoggedIn()) {
        return;
    }
    const QStringList serverIds = managedConfigs().keys();
    if (serverIds.isEmpty()) {
        return;
    }
    allowThroughKillSwitch();
    for (const QString &serverId : serverIds) {
        checkConfig(serverId);
    }
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
    updateRefreshInterval();

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

void CoordinatorController::updateRefreshInterval()
{
    const bool connected = m_connectionState == Vpn::ConnectionState::Connected
            && isManagedServer(m_serversRepository->defaultServerId());
    const int interval = connected ? connectedRefreshIntervalMsecs : refreshIntervalMsecs;
    if (m_refreshTimer.interval() != interval) {
        m_refreshTimer.setInterval(interval); // restarts the timer if it is running
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
    if (configId.isEmpty()) {
        return;
    }

    m_api->configStatus(configId, entry.value(entryKey::revision).toString(), [this, serverId, configId](const CoordinatorApi::Reply &reply) {
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
            m_api->getConfig(configId, [this, serverId, markRevoked](const CoordinatorApi::Reply &configReply) {
                if (!isManagedServer(serverId)) {
                    return;
                }
                if (configReply.ok()) {
                    if (applyConfig(configReply.body, serverId)) {
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
            : platformServerName(config.value("serverName").toString().trimmed());

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
        emit errorOccurred(tr("You have been signed out. Sign in again with a code from the bot"));
    } else if (reply.notMember()) {
        if (!m_notMemberReported) {
            m_notMemberReported = true;
            emit errorOccurred(tr("You are no longer a member of the chat, VPN access is suspended"));
        }
    }
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
    emit managedConfigsChanged();
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
    switch (m_connectionState) {
    case Vpn::ConnectionState::Connected:
    case Vpn::ConnectionState::Connecting:
    case Vpn::ConnectionState::Reconnecting:
    case Vpn::ConnectionState::Preparing:
        m_pendingReconnectServerId = serverId;
        m_pendingReconnectMsecs = QDateTime::currentMSecsSinceEpoch();
        m_connectionController->closeConnection();
        break;
    case Vpn::ConnectionState::Error: {
        // The old config failed (dead entry node); the new one is the fix, so use it right away
        const ErrorCode error = m_connectionController->openConnection(serverId);
        if (error != ErrorCode::NoError) {
            qWarning() << "CoordinatorController: reconnect after config update failed:" << errorString(error);
        }
        break;
    }
    default:
        break;
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
        return;
    }
    auto allow = [](const QStringList &ips) {
        if (ips.isEmpty()) {
            return;
        }
        IpcClient::withInterface([&](QSharedPointer<IpcInterfaceReplica> iface) {
            QRemoteObjectPendingReply<bool> reply = iface->addKillSwitchAllowedRange(ips);
            if (!reply.waitForFinished(1000) || !reply.returnValue())
                qWarning() << "CoordinatorController::allowThroughKillSwitch(): Failed to execute remote addKillSwitchAllowedRange call";
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
