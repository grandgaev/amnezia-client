import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import PageEnum 1.0
import Style 1.0

import "./"
import "../Controls2"
import "../Config"

PageType {
    id: root

    FlickableType {
        id: flickable

        anchors.top: parent.top

        contentHeight: content.implicitHeight

        ColumnLayout {
            id: content

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right

            spacing: 0

            NavigationBarType {
                Layout.fillWidth: true

                title: qsTr("Connection")
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: 24 + PageController.safeAreaBottomMargin

                spacing: 0

                ListItemType {
                    id: amneziaDnsRow

                    Layout.fillWidth: true

                    text: qsTr("Use AmneziaDNS")
                    subtitle: qsTr("If AmneziaDNS is installed on the server")

                    onClicked: {
                        amneziaDnsSwitch.toggle()
                        amneziaDnsSwitch.toggled()
                    }

                    SwitchType {
                        id: amneziaDnsSwitch

                        focusPolicy: Qt.NoFocus

                        checked: SettingsController.isAmneziaDnsEnabled()
                        onToggled: function() {
                            if (checked !== SettingsController.isAmneziaDnsEnabled()) {
                                SettingsController.toggleAmneziaDns(checked)
                            }
                        }
                    }
                }

                ListItemType {
                    id: dnsServersButton

                    Layout.fillWidth: true

                    text: qsTr("DNS servers")
                    subtitle: qsTr("When AmneziaDNS is not used or installed")
                    trailingIconSource: "qrc:/images/controls/chevron-right.svg"

                    onClicked: {
                        PageController.goToPage(PageEnum.PageSettingsDns)
                    }
                }

                ListItemType {
                    id: routingButton

                    Layout.fillWidth: true

                    text: qsTr("Routing")
                    subtitle: RoutingController.isAppRoutingSupported
                              ? qsTr("Split tunneling: sites, IPs, geosite, geoip, apps")
                              : qsTr("Split tunneling: sites, IPs, geosite, geoip")
                    trailingIconSource: "qrc:/images/controls/chevron-right.svg"

                    onClicked: {
                        PageController.goToPage(PageEnum.PageSettingsRouting)
                    }
                }

                ListItemType {
                    id: killSwitchButton

                    Layout.fillWidth: true

                    visible: !GC.isMobile()

                    text: qsTr("KillSwitch")
                    subtitle: qsTr("Blocks network connections without VPN")
                    trailingIconSource: "qrc:/images/controls/chevron-right.svg"

                    onClicked: {
                        PageController.goToPage(PageEnum.PageSettingsKillSwitch)
                    }
                }
            }
        }
    }
}
