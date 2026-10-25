#ifndef INSTALLEDAPPSMODEL_H
#define INSTALLEDAPPSMODEL_H

#include <QAbstractListModel>
#include <QJsonArray>
#include <QVariantList>
#include <QVector>

// Lesha VPN: all installed apps for the app split tunneling screen. The order is fixed when the
// list is loaded (apps from the split tunneling list, then user apps, then system apps, each
// alphabetically), so toggling a checkbox does not move a row until the screen is reopened.
class InstalledAppsModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        AppNameRole = Qt::UserRole + 1,
        PackageNameRole,
        AppIconRole,
        IsAppSelectedRole,
        GroupRole
    };

    enum Group {
        SelectedGroup = 0,
        UserGroup,
        SystemGroup
    };
    Q_ENUM(Group)

    explicit InstalledAppsModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    // installedApps: JSON objects from AppListProvider (package, name, system);
    // savedApps: maps with appName/packageName of the apps in the split tunneling list
    void setApps(const QJsonArray &installedApps, const QVariantList &savedApps);

public slots:
    void updateModel(const QVariantList &savedApps);
    void setAppSelected(const int index, const bool selected);

protected:
    QHash<int, QByteArray> roleNames() const override;

private:
    struct App {
        QString name;
        QString packageName;
        bool isSelected = false;
        Group group = UserGroup;
    };

    QVector<App> m_apps;
};

#endif // INSTALLEDAPPSMODEL_H
