#include "installedAppsModel.h"

#include <QCollator>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QtConcurrent>

#include <algorithm>

#ifdef Q_OS_ANDROID
    #include "platforms/android/android_controller.h"
#endif

InstalledAppsModel::InstalledAppsModel(QObject *parent) : QAbstractListModel(parent)
{
}

int InstalledAppsModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return m_apps.size();
}

QVariant InstalledAppsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rowCount()))
        return QVariant();

    const App &app = m_apps.at(index.row());
    switch (role) {
    case AppNameRole: return app.name;
    case AppIconRole: return app.packageName;
    case PackageNameRole: return app.packageName;
    case IsAppSelectedRole: return app.isSelected;
    case GroupRole: return static_cast<int>(app.group);
    }

    return QVariant();
}

void InstalledAppsModel::setApps(const QJsonArray &installedApps, const QVariantList &savedApps)
{
    QHash<QString, QString> savedNames;
    for (const QVariant &saved : savedApps) {
        const QVariantMap map = saved.toMap();
        const QString packageName = map.value("packageName").toString();
        if (!packageName.isEmpty()) {
            savedNames.insert(packageName, map.value("appName").toString());
        }
    }

    QVector<App> apps;
    QSet<QString> seen;
    for (const QJsonValue &value : installedApps) {
        const QJsonObject object = value.toObject();
        App app;
        app.packageName = object.value("package").toString();
        if (app.packageName.isEmpty() || seen.contains(app.packageName)) {
            continue;
        }
        seen.insert(app.packageName);
        app.name = object.value("name").toString();
        if (app.name.isEmpty()) {
            app.name = app.packageName;
        }
        app.isSelected = savedNames.contains(app.packageName);
        app.group = app.isSelected ? SelectedGroup : (object.value("system").toBool() ? SystemGroup : UserGroup);
        apps.append(app);
    }

    // apps from the list that are no longer installed stay visible so they can be unchecked
    for (auto it = savedNames.cbegin(); it != savedNames.cend(); ++it) {
        if (seen.contains(it.key())) {
            continue;
        }
        App app;
        app.packageName = it.key();
        app.name = it.value().isEmpty() ? it.key() : it.value();
        app.isSelected = true;
        app.group = SelectedGroup;
        apps.append(app);
    }

    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);
    std::sort(apps.begin(), apps.end(), [&collator](const App &a, const App &b) {
        if (a.group != b.group) {
            return a.group < b.group;
        }
        const int byName = collator.compare(a.name, b.name);
        if (byName != 0) {
            return byName < 0;
        }
        return a.packageName < b.packageName;
    });

    beginResetModel();
    m_apps = apps;
    endResetModel();
}

void InstalledAppsModel::updateModel(const QVariantList &savedApps)
{
    QJsonArray installedApps;
#ifdef Q_OS_ANDROID
    QFuture<QJsonArray> future = QtConcurrent::run([]() {
        return AndroidController::instance()->getAppList();
    });

    QFutureWatcher<QJsonArray> watcher;
    QEventLoop wait;
    connect(&watcher, &QFutureWatcher<QJsonArray>::finished, &wait, &QEventLoop::quit);
    watcher.setFuture(future);
    wait.exec();

    installedApps = future.result();
#endif
    setApps(installedApps, savedApps);
}

void InstalledAppsModel::setAppSelected(const int index, const bool selected)
{
    if (index < 0 || index >= m_apps.size() || m_apps[index].isSelected == selected) {
        return;
    }
    m_apps[index].isSelected = selected;
    const QModelIndex modelIndex = this->index(index);
    emit dataChanged(modelIndex, modelIndex, { IsAppSelectedRole });
}

QHash<int, QByteArray> InstalledAppsModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[AppNameRole] = "appName";
    roles[AppIconRole] = "appIcon";
    roles[PackageNameRole] = "packageName";
    roles[IsAppSelectedRole] = "isAppSelected";
    roles[GroupRole] = "group";
    return roles;
}
