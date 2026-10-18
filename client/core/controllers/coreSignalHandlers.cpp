#include "coreSignalHandlers.h"

#include <QTimer>
#include <QtConcurrent>

#include "core/utils/errorCodes.h"
#include "core/utils/routeModes.h"
#include "core/controllers/coreController.h"
#include "core/repositories/secureServersRepository.h"
#include "core/utils/serverConfigUtils.h"
#include "core/repositories/secureAppSettingsRepository.h"
#include "vpnConnection.h"
#include "ui/controllers/qml/pageController.h"
#include "ui/controllers/connectionUiController.h"
#include "ui/controllers/settingsUiController.h"
#include "ui/controllers/serversUiController.h"
#include "ui/controllers/ipSplitTunnelingUiController.h"
#include "ui/controllers/allowedDnsUiController.h"
#include "ui/controllers/appSplitTunnelingUiController.h"
#include "ui/controllers/languageUiController.h"
#include "ui/controllers/importUiController.h"
#include "ui/controllers/protocolSettingsUiController.h"
#include "ui/controllers/coordinator/coordinatorUiController.h"
#include "ui/models/serversModel.h"
#include "core/controllers/serversController.h"
#include "core/controllers/ipSplitTunnelingController.h"
#include "core/controllers/appSplitTunnelingController.h"
#include "core/controllers/settingsController.h"
#include "core/controllers/connectionController.h"
#include "ui/models/containersModel.h"
#include "core/utils/containerEnum.h"

#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
    #include "ui/utils/notificationHandler.h"
    #include "ui/utils/systemTrayNotificationHandler.h"
#endif

#ifdef Q_OS_ANDROID
    #include "platforms/android/android_controller.h"
#endif

#ifdef Q_OS_IOS
    #include "platforms/ios/ios_controller.h"
    #include "core/utils/swiftBridge.h"
#endif

CoreSignalHandlers::CoreSignalHandlers(CoreController* coreController, QObject* parent)
    : QObject(parent),
      m_coreController(coreController)
{
}

void CoreSignalHandlers::initAllHandlers()
{
    initErrorMessagesHandler();
    initSettingsSplitTunnelingHandler();
    initImportControllerHandler();
    initTranslationsUpdatedHandler();
    initLanguageHandler();
    initAutoConnectHandler();
    initAmneziaDnsToggledHandler();
    initServersModelUpdateHandler();
    initSitesModelUpdateHandler();
    initAllowedDnsModelUpdateHandler();
    initAppSplitTunnelingModelUpdateHandler();
    initPrepareConfigHandler();
    initStrictKillSwitchHandler();
    initAndroidSettingsHandler();
    initAndroidConnectionHandler();
    initIosImportHandler();
    initIosSettingsHandler();
    initNotificationHandler();
}

void CoreSignalHandlers::initErrorMessagesHandler()
{
    connect(m_coreController->m_connectionUiController, &ConnectionUiController::connectionErrorOccurred, this, [this](ErrorCode errorCode) {
        emit m_coreController->m_pageController->showErrorMessage(errorCode);
        m_coreController->m_connectionController->setConnectionState(Vpn::ConnectionState::Disconnected);
    });

    connect(m_coreController->m_settingsUiController, &SettingsUiController::errorOccurred, m_coreController->m_pageController,
            qOverload<ErrorCode>(&PageController::showErrorMessage));

    connect(m_coreController->m_coordinatorUiController, &CoordinatorUiController::errorOccurred, m_coreController->m_pageController,
            qOverload<const QString &>(&PageController::showErrorMessage));

    connect(m_coreController->m_protocolSettingsUiController, &ProtocolSettingsUiController::errorOccurred,
            m_coreController->m_pageController, qOverload<ErrorCode>(&PageController::showErrorMessage));
}

void CoreSignalHandlers::initSettingsSplitTunnelingHandler()
{
    connect(m_coreController->m_settingsController, &SettingsController::siteSplitTunnelingRouteModeChanged, this, [this](RouteMode mode) {
        m_coreController->m_ipSplitTunnelingController->setRouteMode(mode);
    });
    connect(m_coreController->m_settingsController, &SettingsController::siteSplitTunnelingToggled, this, [this](bool enabled) {
        m_coreController->m_ipSplitTunnelingController->toggleSplitTunneling(enabled);
    });
    connect(m_coreController->m_settingsController, &SettingsController::appSplitTunnelingRouteModeChanged, this, [this](AppsRouteMode mode) {
        m_coreController->m_appSplitTunnelingController->setRouteMode(mode);
    });
    connect(m_coreController->m_settingsController, &SettingsController::appSplitTunnelingToggled, this, [this](bool enabled) {
        m_coreController->m_appSplitTunnelingController->toggleSplitTunneling(enabled);
    });
}

void CoreSignalHandlers::initImportControllerHandler()
{
    connect(m_coreController->m_importCoreController, &ImportController::importFinished, this, [this]() {
        if (m_coreController->m_connectionUiController->isConnected()) {
            return;
        }

        const int newServerIndex = m_coreController->m_serversController->getServersCount() - 1;
        const QString serverId = m_coreController->m_serversController->getServerId(newServerIndex);
        if (!serverId.isEmpty()) {
            m_coreController->m_serversController->setDefaultServer(serverId);
        }
        if (m_coreController->m_serversUiController) {
            m_coreController->m_serversUiController->setProcessedServerId(serverId);
        }
    });
}

void CoreSignalHandlers::initTranslationsUpdatedHandler()
{
    connect(m_coreController->m_languageUiController, &LanguageUiController::updateTranslations, m_coreController, &CoreController::updateTranslator);
    connect(m_coreController, &CoreController::translationsUpdated, m_coreController->m_languageUiController, &LanguageUiController::translationsUpdated);
    connect(m_coreController, &CoreController::translationsUpdated, m_coreController->m_connectionUiController, &ConnectionUiController::onTranslationsUpdated);
    // Fork: server descriptions ("AmneziaWG (version 3.1)") are built before the first translator is installed
    connect(m_coreController, &CoreController::translationsUpdated, m_coreController->m_serversUiController, &ServersUiController::updateModel);
}

void CoreSignalHandlers::initLanguageHandler()
{
    connect(m_coreController->m_appSettingsRepository, &SecureAppSettingsRepository::appLanguageChanged, m_coreController->m_languageUiController, &LanguageUiController::onAppLanguageChanged);
    connect(m_coreController->m_settingsUiController, &SettingsUiController::resetLanguageToSystem, m_coreController->m_languageUiController, [this]() {
        m_coreController->m_languageUiController->changeLanguage(m_coreController->m_languageUiController->getSystemLanguageEnum());
    });
}

void CoreSignalHandlers::initAutoConnectHandler()
{
    if (m_coreController->m_settingsUiController->isAutoConnectEnabled()
        && !m_coreController->m_serversController->getDefaultServerId().isEmpty()) {
        QTimer::singleShot(1000, this, [this]() { m_coreController->m_connectionUiController->toggleConnection(); });
    }
}

void CoreSignalHandlers::initAmneziaDnsToggledHandler()
{
    connect(m_coreController->m_appSettingsRepository, &SecureAppSettingsRepository::useAmneziaDnsChanged, m_coreController->m_serversUiController, &ServersUiController::updateModel);
}

void CoreSignalHandlers::initServersModelUpdateHandler()
{
    connect(m_coreController->m_serversRepository, &SecureServersRepository::serverAdded,
            m_coreController->m_serversUiController, &ServersUiController::updateModel);
    connect(m_coreController->m_serversRepository, &SecureServersRepository::serverEdited,
            m_coreController->m_serversUiController, &ServersUiController::updateModel);
    connect(m_coreController->m_serversRepository, &SecureServersRepository::serverRemoved,
            m_coreController->m_serversUiController, &ServersUiController::updateModel);
    connect(m_coreController->m_serversRepository, &SecureServersRepository::defaultServerChanged,
            m_coreController->m_serversUiController, &ServersUiController::onDefaultServerChanged);
}

void CoreSignalHandlers::initSitesModelUpdateHandler()
{
    connect(m_coreController->m_appSettingsRepository, &SecureAppSettingsRepository::sitesChanged, m_coreController->m_ipSplitTunnelingUiController, &IpSplitTunnelingUiController::updateModel);
    connect(m_coreController->m_appSettingsRepository, &SecureAppSettingsRepository::sitesSplitTunnelingEnabledChanged, m_coreController->m_ipSplitTunnelingUiController, &IpSplitTunnelingUiController::updateModel);
    connect(m_coreController->m_appSettingsRepository, &SecureAppSettingsRepository::routeModeChanged, m_coreController->m_ipSplitTunnelingUiController, &IpSplitTunnelingUiController::updateModel);
}

void CoreSignalHandlers::initAllowedDnsModelUpdateHandler()
{
    connect(m_coreController->m_appSettingsRepository, &SecureAppSettingsRepository::allowedDnsServersChanged, m_coreController->m_allowedDnsUiController, &AllowedDnsUiController::updateModel);
}

void CoreSignalHandlers::initAppSplitTunnelingModelUpdateHandler()
{
    connect(m_coreController->m_appSettingsRepository, &SecureAppSettingsRepository::appsChanged, m_coreController->m_appSplitTunnelingUiController, &AppSplitTunnelingUiController::updateModel);
    connect(m_coreController->m_appSettingsRepository, &SecureAppSettingsRepository::appsSplitTunnelingEnabledChanged, m_coreController->m_appSplitTunnelingUiController, &AppSplitTunnelingUiController::updateModel);
    connect(m_coreController->m_appSettingsRepository, &SecureAppSettingsRepository::appsRouteModeChanged, m_coreController->m_appSplitTunnelingUiController, &AppSplitTunnelingUiController::updateModel);
}

void CoreSignalHandlers::initPrepareConfigHandler()
{
    connect(m_coreController->m_connectionUiController, &ConnectionUiController::prepareConfig, this, [this]() {
        m_coreController->m_connectionController->setConnectionState(Vpn::ConnectionState::Preparing);

        const QString serverId = m_coreController->m_serversController->getDefaultServerId();
        if (serverId.isEmpty()) {
            m_coreController->m_connectionController->setConnectionState(Vpn::ConnectionState::Disconnected);
            return;
        }

        // Fork: no SSH provisioning, the stored client config is used as is
        const ErrorCode errorCode = m_coreController->m_serversController->validateClientConfig(serverId);
        if (errorCode != ErrorCode::NoError) {
            emit m_coreController->m_pageController->showErrorMessage(errorCode);
            m_coreController->m_connectionController->setConnectionState(Vpn::ConnectionState::Disconnected);
            return;
        }
        m_coreController->m_connectionUiController->openConnection();
    });
}

void CoreSignalHandlers::initStrictKillSwitchHandler()
{
    connect(m_coreController->m_settingsUiController, &SettingsUiController::strictKillSwitchEnabledChanged, m_coreController->m_connectionController,
            &ConnectionController::onKillSwitchModeChanged);
}

void CoreSignalHandlers::initAndroidSettingsHandler()
{
#ifdef Q_OS_ANDROID
    connect(m_coreController->m_appSettingsRepository, &SecureAppSettingsRepository::saveLogsChanged, AndroidController::instance(), &AndroidController::setSaveLogs);
    connect(m_coreController->m_appSettingsRepository, &SecureAppSettingsRepository::screenshotsEnabledChanged, AndroidController::instance(), &AndroidController::setScreenshotsEnabled);
    connect(m_coreController->m_serversRepository, &SecureServersRepository::serverRemoved, this,
            [](const QString &/*serverId*/, int removedIndex) {
                AndroidController::instance()->resetLastServer(removedIndex);
            });
    connect(m_coreController->m_appSettingsRepository, &SecureAppSettingsRepository::settingsCleared, []() { AndroidController::instance()->resetLastServer(-1); });

    // Fork: always-on and the QS tile connect with the service's last saved config; keep it in step with the
    // default server (e.g. a coordinator config replaced after the entry node moved) without connecting.
    const auto saveDefaultServerConfig = [this](const QString &serverId) {
        SecureServersRepository *serversRepository = m_coreController->m_serversRepository;
        if (!serverId.isEmpty() && serverId == serversRepository->defaultServerId()) {
            m_coreController->m_connectionController->saveAndroidConfig(serverId);
        }
    };
    connect(m_coreController->m_serversRepository, &SecureServersRepository::serverEdited, this, saveDefaultServerConfig);
    connect(m_coreController->m_serversRepository, &SecureServersRepository::serverAdded, this, saveDefaultServerConfig);
    connect(m_coreController->m_serversRepository, &SecureServersRepository::defaultServerChanged, this, saveDefaultServerConfig);
    // The saved config also carries the coordinator bypass (the API address, only while signed in)
    const auto saveCurrentDefaultServerConfig = [this, saveDefaultServerConfig]() {
        saveDefaultServerConfig(m_coreController->m_serversRepository->defaultServerId());
    };
    connect(m_coreController->m_coordinatorController, &CoordinatorController::apiAddressChanged, this, saveCurrentDefaultServerConfig);
    connect(m_coreController->m_coordinatorController, &CoordinatorController::loggedInChanged, this, saveCurrentDefaultServerConfig);
#endif
}

void CoreSignalHandlers::initAndroidConnectionHandler()
{
#ifdef Q_OS_ANDROID
    connect(AndroidController::instance(), &AndroidController::initConnectionState, this, [this](Vpn::ConnectionState state) {
        m_coreController->m_connectionUiController->onConnectionStateChanged(state);
        m_coreController->m_connectionController->restoreConnection();
    });
#endif
}

void CoreSignalHandlers::initIosImportHandler()
{
#ifdef Q_OS_IOS
    connect(IosController::Instance(), &IosController::importConfigFromOutside, this, [this](QString data) {
        emit m_coreController->m_pageController->goToPageHome();
        m_coreController->m_importController->extractConfigFromData(data);
        emit m_coreController->m_pageController->goToPageViewConfig();
    });
#endif
}

void CoreSignalHandlers::initIosSettingsHandler()
{
#ifdef Q_OS_IOS
    connect(m_coreController->m_appSettingsRepository, &SecureAppSettingsRepository::screenshotsEnabledChanged, [](bool enabled) { SWIFT_BRIDGE_NAMESPACE::toggleScreenshots(enabled); });
#endif
}

void CoreSignalHandlers::initNotificationHandler()
{
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
    m_coreController->m_notificationHandler = NotificationHandler::create(m_coreController);

    connect(m_coreController->m_connectionController, &ConnectionController::connectionStateChanged, m_coreController->m_notificationHandler,
            &NotificationHandler::setConnectionState);

    connect(m_coreController->m_notificationHandler, &NotificationHandler::raiseRequested, m_coreController->m_pageController, &PageController::raiseMainWindow);
    connect(m_coreController->m_notificationHandler, &NotificationHandler::connectRequested, m_coreController->m_connectionUiController,
            static_cast<void (ConnectionUiController::*)()>(&ConnectionUiController::openConnection));
    connect(m_coreController->m_notificationHandler, &NotificationHandler::disconnectRequested, m_coreController->m_connectionUiController,
            &ConnectionUiController::closeConnection);
    connect(m_coreController, &CoreController::translationsUpdated, m_coreController->m_notificationHandler, &NotificationHandler::onTranslationsUpdated);

    auto* trayHandler = qobject_cast<SystemTrayNotificationHandler*>(m_coreController->m_notificationHandler);
    connect(m_coreController, &CoreController::websiteUrlChanged, trayHandler, &SystemTrayNotificationHandler::updateWebsiteUrl);
#endif    
}
