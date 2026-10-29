#ifndef COORDINATORCONTROLLER_H
#define COORDINATORCONTROLLER_H

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>
#include <optional>

#include "core/controllers/coordinator/coordinatorApi.h"
#include "core/protocols/vpnProtocol.h"

class SecureServersRepository;
class SecureAppSettingsRepository;
class ImportController;
class ConnectionController;

// Keeps servers that came from the VPN coordinator in sync with it (spec section 9.3).
//
// Persistent state lives in SecureAppSettingsRepository (coordinator* accessors):
//   - base URL, device token (encrypted), user { tgId, username };
//   - managed configs: JSON object keyed by storageServerId -> { configId, name, revision, state }.
// Servers themselves are ordinary Native AWG servers in SecureServersRepository; the managed map is the only link.
class CoordinatorController : public QObject
{
    Q_OBJECT

public:
    // Config states reported by the coordinator plus client-side ones.
    // "ok" | "outdated" | "suspended" | "revoked"; "unknown" before the first check.
    explicit CoordinatorController(SecureServersRepository *serversRepository, SecureAppSettingsRepository *appSettingsRepository,
                                   ImportController *importController, ConnectionController *connectionController,
                                   CoordinatorApi *api, QObject *parent = nullptr);

    bool isLoggedIn() const;
    QString baseUrl() const;
    QString username() const; // "@name", or "Telegram ID <id>" when the user has no username

    // Account
    void login(const QString &baseUrl, const QString &code);
    void logout(); // revokes the token on the server (best effort) and forgets it locally; managed servers stay as static configs
    // Before "reset all data": revoke this app's configs and the token on the server (best effort) so the reset leaves
    // no orphaned configs behind. Local state is cleared by the settings reset itself (settingsCleared).
    void forgetDevice();

    // Remote configs of this account (GET /v1/me); result in remoteConfigsUpdated().
    void fetchRemoteConfigs();
    // Create a config on the coordinator and add it to the app.
    void createConfig(const QString &name);
    // Download an existing remote config and add it to the app (no-op re-download if already added).
    void addConfigToApp(const QString &configId);
    // Delete the config on the coordinator and remove its server from the app.
    void deleteConfig(const QString &configId);
    // Rename the config on the coordinator ([a-z0-9-]{1,32}). Result in configRenamed() or errorOccurred().
    void renameConfig(const QString &configId, const QString &name);
    // After login: make sure this device has a config in the app, creating one named after the device if
    // none is added yet. Result in deviceSetupFinished().
    void setupThisDevice();
    // Config name for this device ([a-z0-9-]{1,32}) made from its name (deviceName::platformName()) that is not in
    // takenNames; fallback (the OS) when the name gives nothing usable.
    static QString deviceConfigName(const QString &deviceName, const QString &fallback, const QStringList &takenNames);

    // Check every managed config that isn't revoked (status -> fetch if outdated -> replace in place).
    // Silent on network errors.
    void refreshAll();

    // storageServerId of the managed server for configId, or empty.
    QString serverIdForConfig(const QString &configId) const;
    // configId of a managed server, or empty.
    QString configIdForServer(const QString &serverId) const;
    bool isManagedServer(const QString &serverId) const;
    QJsonObject managedConfigs() const;

    // Hook for the app: start the periodic timer and do the startup check.
    void start();

    static constexpr int refreshIntervalMsecs = 15 * 60 * 1000;
    // While connected to a managed server: a replaced entry node looks like a silent tunnel, not an error.
    // The timer always ticks at this interval; when not connected a tick only refreshes once refreshIntervalMsecs passed.
    static constexpr int connectedRefreshIntervalMsecs = 2 * 60 * 1000;
    // A reconnect requested after a config update only applies to the disconnect it caused
    static constexpr qint64 pendingReconnectTimeoutMsecs = 30 * 1000;

signals:
    void loggedInChanged();
    void loginFinished(bool success, const QString &errorText);
    void remoteConfigsUpdated(const QJsonArray &configs); // [{ id, name, revision, createdAt, inApp: bool, state }]
    void configAdded(const QString &serverId);
    void configReplaced(const QString &serverId);
    void configRenamed(const QString &configId, const QString &name);
    void configDeleted(const QString &configId); // deleted on the coordinator and its server removed from the app
    void deviceSetupFinished(bool success);
    // The stored coordinator address (coordinatorApiAddress, excluded from the tunnel) changed
    void apiAddressChanged();
    // Human-readable problem for the user (not member, suspended, revoked, re-login needed, request failed).
    void errorOccurred(const QString &text);
    void busyChanged(bool busy);
    void networkStateChanged(const QString &network); // "ok" | "degraded" | "down"

private:
    void onConnectionStateChanged(Vpn::ConnectionState state);
    void onServerRemoved(const QString &serverId);

    void onRefreshTimer();
    void checkConfig(const QString &serverId);
    // GET /v1/me: stores the user, reconciles the managed entries and emits remoteConfigsUpdated().
    // done gets the summaries, or nullopt on failure (already reported).
    void requestMe(bool notify, const std::function<void(const std::optional<QJsonArray> &)> &done);
    // Sync managed entries with GET /v1/me: entries missing there are revoked, suspended ones are marked.
    // Returns the summaries for remoteConfigsUpdated().
    QJsonArray reconcile(const QJsonArray &remoteConfigs, bool notify);
    // A managed server whose config is gone (revoked), preferring the default one; the new device config replaces it.
    QString revokedServerToReplace() const;
    void beginBusy();
    void endBusy();
    bool isConnectionActive() const; // connected or on the way there
    // Import Config JSON (from GET/POST) as a new server or replace serverId's config if non-empty.
    bool applyConfig(const QJsonObject &config, const QString &existingServerId);
    void handleAuthError(const CoordinatorApi::Reply &reply); // 401 -> forget token; 403 not_member -> message
    // Failed request: auth errors go to handleAuthError(), the rest to errorOccurred(). True for an auth error.
    bool reportError(const CoordinatorApi::Reply &reply);
    QString describeError(const CoordinatorApi::Reply &reply) const;
    void setManagedEntry(const QString &serverId, const QJsonObject &entry); // empty entry removes it
    bool setManagedState(const QString &serverId, const QString &state); // true if the stored state changed
    void reconnectIfActive(const QString &serverId);
    void allowThroughKillSwitch();
    QString deviceLabel() const;

    SecureServersRepository *m_serversRepository;
    SecureAppSettingsRepository *m_appSettingsRepository;
    ImportController *m_importController;
    ConnectionController *m_connectionController;
    CoordinatorApi *m_api;

    QTimer m_refreshTimer;
    qint64 m_lastRefreshMsecs = 0;
    QSet<QString> m_checksInFlight; // serverIds with a status check or download running
    QSet<QString> m_killSwitchAllowed; // addresses already allowed through the strict kill switch in this connection state
    QString m_lastNetworkState;
    QString m_pendingReconnectServerId;
    qint64 m_pendingReconnectMsecs = 0;
    Vpn::ConnectionState m_connectionState = Vpn::ConnectionState::Disconnected;
    int m_busyCount = 0;
    qint64 m_lastErrorRefreshMsecs = 0;
    bool m_notMemberReported = false;
};

#endif // COORDINATORCONTROLLER_H
