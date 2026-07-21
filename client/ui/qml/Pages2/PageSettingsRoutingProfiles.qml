import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import PageEnum 1.0
import Style 1.0

import "./"
import "../Controls2"
import "../Controls2/TextTypes"
import "../Config"
import "../Components"

PageType {
    id: root

    property bool pageEnabled: true

    Component.onCompleted: {
        if (ConnectionController.isConnected) {
            PageController.showNotificationMessage(qsTr("Cannot change routing settings during active connection"))
            root.pageEnabled = false
        } else {
            root.pageEnabled = true
        }
        RoutingProfilesController.updateModel()
    }

    Connections {
        target: RoutingProfilesController

        function onFinished(message) {
            PageController.showNotificationMessage(message)
        }
        function onErrorOccurred(errorMessage) {
            PageController.showErrorMessage(errorMessage)
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

            headerText: qsTr("Routing")
            descriptionText: qsTr("Route traffic by domains, IP, geosite and geoip through the VPN, directly or blocked, like in a routing profile.")

            enabled: root.pageEnabled
            showSwitcher: true
            switcher {
                checked: RoutingProfilesController.isRoutingEnabled
                enabled: root.pageEnabled
            }
            switcherFunction: function(checked) {
                RoutingProfilesController.isRoutingEnabled = checked
            }
        }
    }

    ListViewType {
        id: listView

        anchors.top: header.bottom
        anchors.topMargin: 16
        anchors.bottom: parent.bottom
        anchors.bottomMargin: bottomBar.implicitHeight + 48

        width: parent.width
        enabled: root.pageEnabled
        clip: true

        model: RoutingProfilesModel

        delegate: ColumnLayout {
            width: listView.width

            LabelWithButtonType {
                Layout.fillWidth: true

                text: name
                descriptionText: {
                    var parts = []
                    parts.push(qsTr("%n rule(s)", "", ruleCount))
                    parts.push(globalProxy ? qsTr("Proxy by default") : qsTr("Direct by default"))
                    if (isActive) {
                        parts.push(qsTr("Active"))
                    }
                    return parts.join(" · ")
                }
                rightImageSource: "qrc:/images/controls/chevron-right.svg"
                rightImageColor: isActive ? AmneziaStyle.color.goldenApricot : AmneziaStyle.color.paleGray

                clickedFunction: function() {
                    RoutingProfilesController.loadProfile(index)
                    PageController.goToPage(PageEnum.PageSettingsRoutingProfile)
                }
            }

            DividerType {}
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        height: bottomBar.implicitHeight + 48
        color: AmneziaStyle.color.midnightBlack

        ColumnLayout {
            id: bottomBar
            enabled: root.pageEnabled

            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            anchors.bottomMargin: 24

            BasicButtonType {
                Layout.fillWidth: true

                text: qsTr("Add routing profile")
                rightImageSource: "qrc:/images/controls/plus.svg"

                clickedFunc: function() {
                    RoutingProfilesController.beginNewProfile()
                    PageController.goToPage(PageEnum.PageSettingsRoutingProfile)
                }
            }

            BasicButtonType {
                Layout.fillWidth: true
                Layout.topMargin: 8

                defaultColor: AmneziaStyle.color.transparent
                hoveredColor: AmneziaStyle.color.translucentWhite
                pressedColor: AmneziaStyle.color.sheerWhite
                textColor: AmneziaStyle.color.paleGray
                borderWidth: 1

                text: qsTr("Import from link")

                clickedFunc: function() {
                    importDrawer.openTriggered()
                }
            }
        }
    }

    DrawerType2 {
        id: importDrawer

        anchors.fill: parent
        expandedHeight: parent.height * 0.35

        expandedStateContent: ColumnLayout {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right

            Header2Type {
                Layout.fillWidth: true
                Layout.margins: 16

                headerText: qsTr("Import routing profile")
                descriptionText: qsTr("Paste a happ://routing/add/... link")
            }

            TextFieldWithHeaderType {
                id: importField
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                headerText: qsTr("Link")
                textField.placeholderText: "happ://routing/add/..."
            }

            BasicButtonType {
                Layout.fillWidth: true
                Layout.margins: 16

                text: qsTr("Import")

                clickedFunc: function() {
                    RoutingProfilesController.importDeeplink(importField.textField.text)
                    importField.textField.text = ""
                    importDrawer.closeTriggered()
                }
            }
        }
    }
}
