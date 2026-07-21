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

    property int currentTab: 0

    Connections {
        target: RoutingProfilesController
        function onFinished(message) {
            PageController.showNotificationMessage(message)
            PageController.closePage()
        }
        function onErrorOccurred(errorMessage) {
            PageController.showErrorMessage(errorMessage)
        }
    }

    property list<QtObject> domainStrategyModel: [
        QtObject { property string name: qsTr("Prefer IP if no domain rule matches"); property int type: 0 },
        QtObject { property string name: qsTr("Resolve IP on demand"); property int type: 1 },
        QtObject { property string name: qsTr("Match domains as-is"); property int type: 2 }
    ]

    property list<QtObject> ruleOrderModel: [
        QtObject { property string name: qsTr("Block, Direct, Proxy"); property int type: 0 },
        QtObject { property string name: qsTr("Block, Proxy, Direct"); property int type: 1 },
        QtObject { property string name: qsTr("Proxy, Direct, Block"); property int type: 2 },
        QtObject { property string name: qsTr("Proxy, Block, Direct"); property int type: 3 },
        QtObject { property string name: qsTr("Direct, Proxy, Block"); property int type: 4 },
        QtObject { property string name: qsTr("Direct, Block, Proxy"); property int type: 5 }
    ]

    BackButtonType {
        id: backButton
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: 20 + PageController.safeAreaTopMargin
    }

    FlickableType {
        id: fl
        anchors.top: backButton.bottom
        anchors.bottom: saveBar.top
        contentHeight: content.height

        ColumnLayout {
            id: content
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 0

            Header2Type {
                Layout.fillWidth: true
                Layout.margins: 16

                headerText: qsTr("Routing profile")
            }

            TextFieldWithHeaderType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                headerText: qsTr("Profile name")
                textField.text: RoutingProfilesController.editName
                textField.onEditingFinished: RoutingProfilesController.editName = textField.text
            }

            SwitcherType {
                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Send all traffic through the VPN by default")
                descriptionText: qsTr("On: everything is proxied except the rules below. Off: everything is direct except the rules below.")
                checked: RoutingProfilesController.editGlobalProxy
                onCheckedChanged: RoutingProfilesController.editGlobalProxy = checked
            }

            // --- proxy / direct / block tabs ---
            TabBar {
                id: tabBar
                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                currentIndex: root.currentTab
                onCurrentIndexChanged: root.currentTab = currentIndex

                background: Rectangle { color: AmneziaStyle.color.transparent }

                TabButtonType { text: qsTr("Proxy") }
                TabButtonType { text: qsTr("Direct") }
                TabButtonType { text: qsTr("Block") }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 8
                currentIndex: root.currentTab

                // Proxy
                ColumnLayout {
                    TextAreaWithFooterType {
                        Layout.fillWidth: true
                        headerText: qsTr("Domains routed through the VPN")
                        placeholderText: "geosite:google\ndomain:example.com\nfull:site.org"
                        textAreaText: RoutingProfilesController.editProxySites
                        textArea.onEditingFinished: RoutingProfilesController.editProxySites = textAreaText
                    }
                    TextAreaWithFooterType {
                        Layout.fillWidth: true
                        Layout.topMargin: 16
                        headerText: qsTr("IP / CIDR / geoip routed through the VPN")
                        placeholderText: "geoip:netflix\n1.2.3.0/24\n2001:db8::/32"
                        textAreaText: RoutingProfilesController.editProxyIp
                        textArea.onEditingFinished: RoutingProfilesController.editProxyIp = textAreaText
                    }
                }

                // Direct
                ColumnLayout {
                    TextAreaWithFooterType {
                        Layout.fillWidth: true
                        headerText: qsTr("Domains that bypass the VPN")
                        placeholderText: "geosite:ru\ngeosite:category-gov-ru"
                        textAreaText: RoutingProfilesController.editDirectSites
                        textArea.onEditingFinished: RoutingProfilesController.editDirectSites = textAreaText
                    }
                    TextAreaWithFooterType {
                        Layout.fillWidth: true
                        Layout.topMargin: 16
                        headerText: qsTr("IP / CIDR / geoip that bypass the VPN")
                        placeholderText: "geoip:ru\n10.0.0.0/8\n192.168.0.0/16"
                        textAreaText: RoutingProfilesController.editDirectIp
                        textArea.onEditingFinished: RoutingProfilesController.editDirectIp = textAreaText
                    }
                }

                // Block
                ColumnLayout {
                    TextAreaWithFooterType {
                        Layout.fillWidth: true
                        headerText: qsTr("Domains to block")
                        placeholderText: "geosite:category-ads-all"
                        textAreaText: RoutingProfilesController.editBlockSites
                        textArea.onEditingFinished: RoutingProfilesController.editBlockSites = textAreaText
                    }
                    TextAreaWithFooterType {
                        Layout.fillWidth: true
                        Layout.topMargin: 16
                        headerText: qsTr("IP / CIDR / geoip to block")
                        placeholderText: "geoip:ads"
                        textAreaText: RoutingProfilesController.editBlockIp
                        textArea.onEditingFinished: RoutingProfilesController.editBlockIp = textAreaText
                    }
                }
            }

            CaptionTextType {
                Layout.fillWidth: true
                Layout.topMargin: 8
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                color: AmneziaStyle.color.mutedGray
                text: qsTr("One rule per line. Domains accept domain:, full:, keyword:, regexp:, geosite:. IP accepts addresses, CIDR and geoip:.")
            }

            // --- advanced ---
            Header2Type {
                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                headerText: qsTr("Advanced")
            }

            DropDownType {
                id: strategyDropDown
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                drawerHeight: 0.4
                drawerParent: root
                headerText: qsTr("Domain strategy")
                text: root.domainStrategyModel[RoutingProfilesController.editDomainStrategy].name

                listView: ListViewWithRadioButtonType {
                    rootWidth: root.width
                    model: root.domainStrategyModel
                    selectedIndex: RoutingProfilesController.editDomainStrategy
                    clickedFunction: function() {
                        RoutingProfilesController.editDomainStrategy = root.domainStrategyModel[selectedIndex].type
                        strategyDropDown.text = selectedText
                        strategyDropDown.closeTriggered()
                    }
                }
            }

            DropDownType {
                id: orderDropDown
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                drawerHeight: 0.5
                drawerParent: root
                headerText: qsTr("Rule priority order")
                text: root.ruleOrderModel[RoutingProfilesController.editOrder].name

                listView: ListViewWithRadioButtonType {
                    rootWidth: root.width
                    model: root.ruleOrderModel
                    selectedIndex: RoutingProfilesController.editOrder
                    clickedFunction: function() {
                        RoutingProfilesController.editOrder = root.ruleOrderModel[selectedIndex].type
                        orderDropDown.text = selectedText
                        orderDropDown.closeTriggered()
                    }
                }
            }

            SwitcherType {
                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("FakeDNS")
                descriptionText: qsTr("Improves domain matching. Recommended when using DNS over UDP.")
                checked: RoutingProfilesController.editFakeDns
                onCheckedChanged: RoutingProfilesController.editFakeDns = checked
            }

            TextFieldWithHeaderType {
                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                headerText: qsTr("geosite.dat URL (optional)")
                textField.text: RoutingProfilesController.editGeositeUrl
                textField.onEditingFinished: RoutingProfilesController.editGeositeUrl = textField.text
            }

            TextFieldWithHeaderType {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                headerText: qsTr("geoip.dat URL (optional)")
                textField.text: RoutingProfilesController.editGeoipUrl
                textField.onEditingFinished: RoutingProfilesController.editGeoipUrl = textField.text
            }

            Item { Layout.preferredHeight: 24 }
        }
    }

    Rectangle {
        id: saveBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        height: saveButton.implicitHeight + 48
        color: AmneziaStyle.color.midnightBlack

        BasicButtonType {
            id: saveButton
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            anchors.bottomMargin: 24

            text: qsTr("Save profile")

            clickedFunc: function() {
                RoutingProfilesController.saveDraft()
            }
        }
    }
}
