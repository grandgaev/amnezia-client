import QtQuick
import QtQuick.Layouts

import PageEnum 1.0
import Style 1.0

import "../Controls2"
import "../Controls2/TextTypes"

DrawerType2 {
    id: root

    anchors.fill: parent
    expandedHeight: parent.height * 0.9

    expandedStateContent: Item {
        implicitHeight: root.expandedHeight

        FlickableType {
            id: flickable

            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.bottomMargin: PageController.safeAreaBottomMargin

            contentHeight: content.implicitHeight + 16

            ColumnLayout {
                id: content

                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 0

                Header2Type {
                    Layout.fillWidth: true
                    Layout.topMargin: 24
                    Layout.rightMargin: 16
                    Layout.leftMargin: 16
                    Layout.bottomMargin: 16

                    headerText: qsTr("Split tunneling")
                    descriptionText: qsTr("Allows you to connect to some sites or applications through a VPN connection and bypass others")
                }

                LabelWithButtonType {
                    id: splitTunnelingOnServer
                    Layout.fillWidth: true

                    visible: ServersUiController.isDefaultServerDefaultContainerHasSplitTunneling

                    text: qsTr("Split tunneling on the server")
                    descriptionText: qsTr("Enabled \nCan't be disabled for current server")
                    rightImageSource: "qrc:/images/controls/chevron-right.svg"

                    clickedFunction: function() {
                        PageController.goToPage(PageEnum.PageSettingsRouting)
                        root.closeTriggered()
                    }
                }

                DividerType {
                    visible: splitTunnelingOnServer.visible
                }

                SwitcherType {
                    id: routingSwitch

                    Layout.fillWidth: true
                    Layout.margins: 16

                    text: qsTr("Routing")
                    descriptionText: RoutingController.routingEnabled
                                     ? qsTr("Active profile: %1").arg(RoutingController.selectedProfileName)
                                     : qsTr("Disabled")

                    checked: RoutingController.routingEnabled
                    onToggled: {
                        if (checked !== RoutingController.routingEnabled) {
                            RoutingController.setRoutingEnabled(checked)
                        }
                    }
                }

                ListItemTitleType {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16

                    visible: RoutingController.profilesCount > 0

                    text: qsTr("Profiles")
                    color: AmneziaStyle.color.mutedGray
                }

                Repeater {
                    model: RoutingProfilesModel

                    delegate: ColumnLayout {
                        id: profileDelegate

                        required property string profileId
                        required property string name
                        required property bool isSelected

                        Layout.fillWidth: true
                        spacing: 0

                        VerticalRadioButton {
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

                            Keys.onEnterPressed: this.clicked()
                            Keys.onReturnPressed: this.clicked()
                        }

                        DividerType {}
                    }
                }

                RoutingReconnectNotice {
                    Layout.fillWidth: true
                    Layout.topMargin: 16
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                }

                LabelWithButtonType {
                    Layout.fillWidth: true
                    Layout.topMargin: 16

                    text: qsTr("Routing settings")
                    descriptionText: qsTr("Profiles with sites, IPs, geosite and geoip rules")
                    rightImageSource: "qrc:/images/controls/chevron-right.svg"

                    clickedFunction: function() {
                        PageController.goToPage(PageEnum.PageSettingsRouting)
                        root.closeTriggered()
                    }
                }

                DividerType {}

                LabelWithButtonType {
                    id: appSplitTunnelingButton
                    Layout.fillWidth: true

                    visible: RoutingController.isAppRoutingSupported

                    text: qsTr("App-based split tunneling")
                    descriptionText: AppSplitTunnelingController.isSplitTunnelingEnabled ? qsTr("Enabled") : qsTr("Disabled")
                    rightImageSource: "qrc:/images/controls/chevron-right.svg"

                    clickedFunction: function() {
                        PageController.goToPage(PageEnum.PageSettingsAppSplitTunneling)
                        root.closeTriggered()
                    }
                }

                DividerType {
                    visible: appSplitTunnelingButton.visible
                }
            }
        }
    }
}
