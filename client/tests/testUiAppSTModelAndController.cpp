#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUuid>
#include <QTest>

#include "utils/testCoreController.h"
#include "core/models/serverDescription.h"
#include "secureQSettings.h"
#include "vpnConnection.h"
#include "ui/models/installedAppsModel.h"

using namespace amnezia;

class TestUiAppSTModelAndController : public QObject
{
    Q_OBJECT

private:
    TestCoreController *m_coreController;
    SecureQSettings *m_settings;

private slots:
    void initTestCase()
    {
        QString testOrg = "AmneziaVPN-Test-" + QUuid::createUuid().toString();
        m_settings = new SecureQSettings(testOrg, "amnezia-client", nullptr, false);

        auto vpnConnection = QSharedPointer<VpnConnection>::create(nullptr, nullptr);

        m_coreController = new TestCoreController(vpnConnection, m_settings, nullptr, this);
    }

    void cleanupTestCase()
    {
        m_settings->clearSettings();
        delete m_coreController;
        delete m_settings;
    }

    void init()
    {
        m_settings->clearSettings();
        if (m_coreController->m_serversModel) {
            m_coreController->m_serversModel->updateModel(QVector<ServerDescription>(), QString{});
        }
    }

    void testRolesAndSignals()
    {
        QSignalSpy finishedSpy(m_coreController->m_appSplitTunnelingUiController, &AppSplitTunnelingUiController::finished);
        QSignalSpy errorOccurredSpy(m_coreController->m_appSplitTunnelingUiController, &AppSplitTunnelingUiController::errorOccurred);
        QSignalSpy isSplitTunnelingChangedSpy(m_coreController->m_appSplitTunnelingUiController, &AppSplitTunnelingUiController::isSplitTunnelingEnabledChanged);

        m_coreController->m_appSplitTunnelingUiController->toggleSplitTunneling(true);
        QVERIFY2(isSplitTunnelingChangedSpy.count() == 1, "isSplitTunnelingChangedSpy signal should be emitted");
        QVERIFY2(m_coreController->m_appSplitTunnelingUiController->isSplitTunnelingEnabled() == true, "AppSplitTunneling should be enabled");

        m_coreController->m_appSplitTunnelingUiController->toggleSplitTunneling(false);
        QVERIFY2(isSplitTunnelingChangedSpy.count() == 2, "isSplitTunnelingChangedSpy signal should be emitted 2nd time");
        QVERIFY2(m_coreController->m_appSplitTunnelingUiController->isSplitTunnelingEnabled() == false, "AppSplitTunneling should be disabled");

        const QString app = QCoreApplication::applicationFilePath();

        m_coreController->m_appSplitTunnelingUiController->addApp(app);
        m_coreController->m_appSplitTunnelingUiController->updateModel();
        QVERIFY2(finishedSpy.count() == 1, "finished signal should be emitted");
        QVERIFY2(m_coreController->m_appSplitTunnelingModel->rowCount() == 1, "AppSplitTunnelingModel should have 1 row");

        QModelIndex appSTModelIndex = m_coreController->m_appSplitTunnelingModel->index(0, 0);
        QVERIFY2(appSTModelIndex.isValid(), "Site model index should be valid");

        auto appPath = m_coreController->m_appSplitTunnelingModel->data(appSTModelIndex, AppSplitTunnelingModel::AppPathRole);
        QString msg = QString("app path should be %1, got %2").arg(app, appPath.toString());
        QVERIFY2(app.contains(appPath.toString()) == true, msg.toLocal8Bit().constData());

        auto pkgAppName = m_coreController->m_appSplitTunnelingModel->data(appSTModelIndex, AppSplitTunnelingModel::PackageAppNameRole);
        QVERIFY2(pkgAppName == true, "app name should be set");

        auto pkgAppIcon = m_coreController->m_appSplitTunnelingModel->data(appSTModelIndex, AppSplitTunnelingModel::PackageAppIconRole);
        QVERIFY2(pkgAppIcon == true, "app image should be set");

        m_coreController->m_appSplitTunnelingUiController->addApp(app);
        m_coreController->m_appSplitTunnelingUiController->updateModel();
        QVERIFY2(errorOccurredSpy.count() == 1, "errorOccurred signal should be emitted");
        QVERIFY2(m_coreController->m_appSplitTunnelingModel->rowCount() == 1, "AppSplitTunnelingModel should have 3 rows (same app should not be added)");

        m_coreController->m_appSplitTunnelingUiController->removeApp(0);
        m_coreController->m_appSplitTunnelingUiController->updateModel();
        QVERIFY2(finishedSpy.count() == 2, "finished signal should be emitted");
        QVERIFY2(m_coreController->m_appSplitTunnelingModel->rowCount() == 0, "AppSplitTunnelingModel should have 0 rows");
    }
    void testInstalledAppsCheckboxList()
    {
        auto *uiController = m_coreController->m_appSplitTunnelingUiController;
        uiController->setRouteMode(static_cast<int>(AppsRouteMode::VpnAllExceptApps));

        uiController->setPackageInList("Telegram", "org.telegram", true);
        uiController->setPackageInList("Gone", "com.removed", true);
        uiController->setPackageInList("Telegram", "org.telegram", true);
        QCOMPARE(uiController->getPackagesInList().size(), 2);

        auto app = [](const QString &package, const QString &name, bool system) {
            return QJsonObject { { "package", package }, { "name", name }, { "system", system } };
        };
        const QJsonArray installed { app("com.android.settings", "Settings", true), app("ru.yandex", "яндекс", false),
                                     app("com.whatsapp", "WhatsApp", false), app("org.telegram", "Telegram", false),
                                     app("com.android.phone", "", true), app("app.b", "Браузер", false) };

        InstalledAppsModel model;
        model.setApps(installed, uiController->getPackagesInList());

        auto column = [&model](int role) {
            QStringList values;
            for (int i = 0; i < model.rowCount(); ++i) {
                values << model.data(model.index(i), role).toString();
            }
            return values;
        };
        QCOMPARE(column(InstalledAppsModel::PackageNameRole),
                 QStringList({ "com.removed", "org.telegram", "com.whatsapp", "app.b", "ru.yandex", "com.android.phone",
                               "com.android.settings" }));
        QCOMPARE(column(InstalledAppsModel::GroupRole), QStringList({ "0", "0", "1", "1", "1", "2", "2" }));
        QCOMPARE(model.data(model.index(5), InstalledAppsModel::AppNameRole).toString(), QString("com.android.phone"));

        // toggling keeps the order; the list itself changes right away
        model.setAppSelected(1, false);
        uiController->setPackageInList("Telegram", "org.telegram", false);
        model.setAppSelected(2, true);
        uiController->setPackageInList("WhatsApp", "com.whatsapp", true);
        QCOMPARE(model.data(model.index(1), InstalledAppsModel::PackageNameRole).toString(), QString("org.telegram"));
        QCOMPARE(model.data(model.index(1), InstalledAppsModel::IsAppSelectedRole).toBool(), false);
        QCOMPARE(model.data(model.index(2), InstalledAppsModel::IsAppSelectedRole).toBool(), true);

        // reopening the page re-sorts
        model.setApps(installed, uiController->getPackagesInList());
        QCOMPARE(column(InstalledAppsModel::PackageNameRole).mid(0, 3), QStringList({ "com.removed", "com.whatsapp", "org.telegram" }));
        QCOMPARE(column(InstalledAppsModel::GroupRole).mid(0, 3), QStringList({ "0", "0", "1" }));

        // the other mode has its own list
        uiController->setRouteMode(static_cast<int>(AppsRouteMode::VpnOnlyForwardApps));
        QCOMPARE(uiController->getPackagesInList().size(), 0);
    }
};

QTEST_MAIN(TestUiAppSTModelAndController)
#include "testUiAppSTModelAndController.moc"
