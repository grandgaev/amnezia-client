import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import PageEnum 1.0
import Style 1.0

import "../Controls2"
import "../Controls2/TextTypes"

SheetType {
    id: root

    expandedHeight: Math.min(content.implicitHeight + 24 + PageController.safeAreaBottomMargin, windowHeight * 0.9)

    ColumnLayout {
        id: content

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 0

        SectionHeaderType {
            Layout.fillWidth: true
            Layout.topMargin: 16
            Layout.rightMargin: 16
            Layout.leftMargin: 16
            Layout.bottomMargin: 32

            headerText: qsTr("Split tunneling")
            descriptionText: qsTr("Allows you to connect to some sites or applications through a VPN connection and bypass others")
        }

        ListItemType {
            id: splitTunnelingOnServer

            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16

            visible: ServersUiController.isDefaultServerDefaultContainerHasSplitTunneling

            text: qsTr("Split tunneling on the server")
            subtitle: qsTr("Enabled \nCan't be disabled for current server")
            trailingIconSource: "qrc:/images/controls/chevron-right.svg"

            onClicked: {
                root.close()
                PageController.goToPage(PageEnum.PageSettingsRouting)
            }
        }

        ListItemType {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16

            text: qsTr("Routing")
            subtitle: RoutingController.routingEnabled
                      ? qsTr("Active profile: %1").arg(RoutingController.selectedProfileName)
                      : qsTr("Disabled")

            onClicked: {
                routingSwitch.toggle()
                routingSwitch.toggled()
            }

            SwitchType {
                id: routingSwitch

                focusPolicy: Qt.NoFocus

                checked: RoutingController.routingEnabled

                onToggled: {
                    if (checked !== RoutingController.routingEnabled) {
                        RoutingController.setRoutingEnabled(checked)
                    }
                }
            }
        }

        BodyTextType {
            Layout.fillWidth: true
            Layout.topMargin: 16
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 4

            visible: RoutingController.profilesCount > 0

            text: qsTr("Profiles")
            color: AmneziaStyle.color.textTertiary
        }

        Repeater {
            model: RoutingProfilesModel

            delegate: RadioItemType {
                id: profileDelegate

                required property string profileId
                required property string name
                required property bool isSelected

                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: profileDelegate.name
                checked: profileDelegate.isSelected && RoutingController.routingEnabled

                onClicked: {
                    RoutingController.selectProfile(profileDelegate.profileId)
                    if (!RoutingController.routingEnabled) {
                        RoutingController.setRoutingEnabled(true)
                    }
                }
            }
        }

        RoutingReconnectNotice {
            Layout.fillWidth: true
            Layout.topMargin: 16
            Layout.leftMargin: 16
            Layout.rightMargin: 16
        }

        ListItemType {
            Layout.fillWidth: true
            Layout.topMargin: 16
            Layout.leftMargin: 16
            Layout.rightMargin: 16

            text: qsTr("Routing settings")
            subtitle: qsTr("Profiles with sites, IPs, geosite and geoip rules")
            trailingIconSource: "qrc:/images/controls/chevron-right.svg"

            onClicked: {
                root.close()
                PageController.goToPage(PageEnum.PageSettingsRouting)
            }
        }

        ListItemType {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16

            visible: RoutingController.isAppRoutingSupported

            text: qsTr("App-based split tunneling")
            subtitle: AppSplitTunnelingController.isSplitTunnelingEnabled ? qsTr("Enabled") : qsTr("Disabled")
            trailingIconSource: "qrc:/images/controls/chevron-right.svg"

            onClicked: {
                root.close()
                PageController.goToPage(PageEnum.PageSettingsAppSplitTunneling)
            }
        }
    }
}
