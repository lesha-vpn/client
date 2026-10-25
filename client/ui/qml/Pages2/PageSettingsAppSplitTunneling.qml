import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import QtCore

import SortFilterProxyModel 0.2

import InstalledAppsModel 1.0

import PageEnum 1.0
import ContainerProps 1.0
import Style 1.0

import "./"
import "../Controls2"
import "../Controls2/TextTypes"
import "../Config"
import "../Components"

PageType {
    id: root

    property bool pageEnabled

    // Lesha VPN: on Android the page itself is the checkbox list of installed apps
    readonly property bool isAndroid: Qt.platform.os === "android"

    Component.onCompleted: {
        if (ConnectionController.isConnected) {
            PageController.showNotificationMessage(qsTr("Cannot change split tunneling settings during active connection"))
            root.pageEnabled = false
        } else {
            root.pageEnabled = true
        }

        if (root.isAndroid) {
            root.reloadInstalledApps()
        }
    }

    // The order (checked, user, system) is fixed here, so toggling doesn't move rows until the page is reopened
    function reloadInstalledApps() {
        PageController.showBusyIndicator(true)
        installedAppsModel.updateModel(AppSplitTunnelingController.getPackagesInList())
        PageController.showBusyIndicator(false)
    }

    function escapeRegExp(text) {
        return text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
    }

    InstalledAppsModel {
        id: installedAppsModel
    }

    QtObject {
        id: routeMode
        property int allApps: 0
        property int onlyForwardApps: 1
        property int allExceptApps: 2
    }

    property list<QtObject> routeModesModel: [
        onlyForwardApps,
        allExceptApps
    ]

    QtObject {
        id: onlyForwardApps

        readonly property string name: qsTr("Only the apps from the list should have access via VPN")
        readonly property int type: routeMode.onlyForwardApps
    }

    QtObject {
        id: allExceptApps
        
        readonly property string name: qsTr("Apps from the list should not have access via VPN")
        readonly property int type: routeMode.allExceptApps
    }

    function getRouteModesModelIndex() {
        var currentRouteMode = AppSplitTunnelingController.routeMode
        if ((routeMode.onlyForwardApps === currentRouteMode) || (routeMode.allApps === currentRouteMode)) {
            return 0
        } else if (routeMode.allExceptApps === currentRouteMode) {
            return 1
        }
    }

    ColumnLayout {
        id: header

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right

        anchors.topMargin: 20 + PageController.safeAreaTopMargin

        BackButtonType {
            id: backButton
        }

        HeaderTypeWithSwitcher {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16

            headerText: qsTr("App split tunneling")

            enabled: root.pageEnabled
            showSwitcher: true
            switcher {
                checked: AppSplitTunnelingController.isSplitTunnelingEnabled
                enabled: root.pageEnabled
            }
            switcherFunction: function(checked) {
                AppSplitTunnelingController.toggleSplitTunneling(checked)
                selector.text = root.routeModesModel[getRouteModesModelIndex()].name
            }
        }

        DropDownType {
            id: selector

            Layout.fillWidth: true
            Layout.topMargin: 32
            Layout.leftMargin: 16
            Layout.rightMargin: 16

            drawerHeight: 0.4375
            drawerParent: root

            headerText: qsTr("Mode")

            enabled: (Qt.platform.os === "android") && root.pageEnabled

            listView: ListViewWithRadioButtonType {
                rootWidth: root.width

                model: root.routeModesModel

                selectedIndex: getRouteModesModelIndex()

                clickedFunction: function() {
                    selector.text = selectedText
                    selector.closeTriggered()
                    if (AppSplitTunnelingController.routeMode !== root.routeModesModel[selectedIndex].type) {
                        AppSplitTunnelingController.routeMode = root.routeModesModel[selectedIndex].type
                    }
                }

                Component.onCompleted: {
                    if (root.routeModesModel[selectedIndex].type === AppSplitTunnelingController.routeMode) {
                        selector.text = selectedText
                    } else {
                        selector.text = root.routeModesModel[0].name
                    }
                }

                Connections {
                    target: AppSplitTunnelingController
                    function onRouteModeChanged() {
                        selectedIndex = getRouteModesModelIndex()
                        // each mode has its own list of apps
                        if (root.isAndroid) {
                            root.reloadInstalledApps()
                        }
                    }
                }
            }
        }

        WarningType {
            Layout.fillWidth: true
            Layout.topMargin: 8
            Layout.leftMargin: 16
            Layout.rightMargin: 16

            textString: qsTr("Only \"Apps from the list should not have access via VPN\" mode is available on Windows")
            iconPath: "qrc:/images/controls/alert-circle.svg"

            visible: (Qt.platform.os === "windows") && root.pageEnabled
        }
    }

    ListViewType {
        id: listView

        visible: !root.isAndroid

        ScrollBar.vertical: ScrollBarType { policy: ScrollBar.AlwaysOn }

        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.bottomMargin: addAppButton.implicitHeight + 48 + PageController.safeAreaBottomMargin + (searchField.textField.activeFocus ? 0 : PageController.imeHeight)
        anchors.left: parent.left
        anchors.right: parent.right
        clip: true

        model: SortFilterProxyModel {
            id: proxyAppSplitTunnelingModel
            sourceModel: AppSplitTunnelingModel
            filters: RegExpFilter {
                roleName: "appPath"
                pattern: ".*" + searchField.textField.text + ".*"
                caseSensitivity: Qt.CaseInsensitive
            }
            sorters: [
                RoleSorter { roleName: "appPath"; sortOrder: Qt.AscendingOrder }
            ]
        }

        delegate: ColumnLayout {
            width: listView.width

            LabelWithButtonType {
                Layout.fillWidth: true

                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: appPath
                rightImageSource: "qrc:/images/controls/trash.svg"
                rightImageColor: AmneziaStyle.color.paleGray

                clickedFunction: function() {
                    var headerText = qsTr("Remove ") + appPath + "?"
                    var yesButtonText = qsTr("Continue")
                    var noButtonText = qsTr("Cancel")

                    var yesButtonFunction = function() {
                        AppSplitTunnelingController.removeApp(proxyAppSplitTunnelingModel.mapToSource(index))
                    }
                    var noButtonFunction = function() {
                    }

                    showQuestionDrawer(headerText, "", yesButtonText, noButtonText, yesButtonFunction, noButtonFunction)
                }
            }

            DividerType {}
        }
    }

    ListViewType {
        id: installedAppsListView

        visible: root.isAndroid

        ScrollBar.vertical: ScrollBarType { policy: ScrollBar.AlwaysOn }

        anchors.top: header.bottom
        anchors.topMargin: 16
        anchors.bottom: parent.bottom
        anchors.bottomMargin: addAppButton.implicitHeight + 48 + PageController.safeAreaBottomMargin + (searchField.textField.activeFocus ? 0 : PageController.imeHeight)
        anchors.left: parent.left
        anchors.right: parent.right
        clip: true

        model: SortFilterProxyModel {
            id: proxyInstalledAppsModel
            sourceModel: installedAppsModel
            filters: RegExpFilter {
                roleName: "appName"
                pattern: ".*" + root.escapeRegExp(searchField.textField.text) + ".*"
                caseSensitivity: Qt.CaseInsensitive
            }
        }

        section.property: "group"
        section.criteria: ViewSection.FullString
        section.delegate: CaptionTextType {
            required property string section

            width: installedAppsListView.width
            leftPadding: 16
            rightPadding: 16
            topPadding: 16
            bottomPadding: 8

            color: AmneziaStyle.color.mutedGray
            text: section === "0" ? qsTr("Selected")
                                  : section === "1" ? qsTr("Apps")
                                                    : qsTr("System apps")
        }

        delegate: ColumnLayout {
            width: installedAppsListView.width

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                CheckBoxType {
                    Layout.fillWidth: true

                    enabled: root.pageEnabled
                    text: appName
                    descriptionText: group === InstalledAppsModel.SystemGroup ? packageName : ""
                    checked: isAppSelected
                    onToggled: {
                        installedAppsModel.setAppSelected(proxyInstalledAppsModel.mapToSource(index), checked)
                        AppSplitTunnelingController.setPackageInList(appName, packageName, checked)
                    }
                }

                Image {
                    source: "image://installedAppImage/" + appIcon

                    sourceSize.width: 24
                    sourceSize.height: 24

                    Layout.rightMargin: 16
                }
            }

            DividerType {}
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        
        height: addAppButton.implicitHeight + 48 + PageController.safeAreaBottomMargin
        
        color: AmneziaStyle.color.midnightBlack
        
        RowLayout {
            id: addAppButton

            enabled: root.pageEnabled

            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 24
            anchors.rightMargin: 16
            anchors.leftMargin: 16
            anchors.bottomMargin: 24 + PageController.safeAreaBottomMargin

            TextFieldWithHeaderType {
                id: searchField

                Layout.fillWidth: true

                textField.placeholderText: qsTr("application name")
                buttonImageSource: root.isAndroid ? "" : "qrc:/images/controls/plus.svg"

                rightButtonClickedOnEnter: true

                clickedFunc: function() {
                    searchField.focus = false
                    PageController.showBusyIndicator(true)

                    if (Qt.platform.os === "windows") {
                        var fileName = SystemController.getFileName(qsTr("Open executable file"),
                                                                    qsTr("Executable files (*.*)"))
                        if (fileName !== "") {
                            AppSplitTunnelingController.addApp(fileName)
                        }
                    }

                    PageController.showBusyIndicator(false)
                }
            }
        }
    }
}
