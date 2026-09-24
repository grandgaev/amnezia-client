import QtQuick
import QtQuick.Layouts

import "../Controls2"
import "../Controls2/TextTypes"
import "../Components"

// DNS settings of the profile RoutingController.currentProfileId.
PageType {
    id: root

    readonly property var dnsTypes: ["DoU", "DoH"]
    readonly property var dnsTypeNames: [qsTr("DNS over UDP (DoU)"), qsTr("DNS over HTTPS (DoH)")]

    property int remoteTypeIndex: 0
    property int domesticTypeIndex: 1

    function load() {
        root.remoteTypeIndex = Math.max(0, root.dnsTypes.indexOf(RoutingController.currentRemoteDnsType))
        root.domesticTypeIndex = Math.max(0, root.dnsTypes.indexOf(RoutingController.currentDomesticDnsType))
        remoteDohUrl.textField.text = RoutingController.currentRemoteDnsDomain
        remoteIp.textField.text = RoutingController.currentRemoteDnsIp
        domesticDohUrl.textField.text = RoutingController.currentDomesticDnsDomain
        domesticIp.textField.text = RoutingController.currentDomesticDnsIp
    }

    Component.onCompleted: load()

    BackButtonType {
        id: backButton

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: 20 + PageController.safeAreaTopMargin

        onActiveFocusChanged: {
            if (backButton.enabled && backButton.activeFocus) {
                fl.contentY = 0
            }
        }
    }

    FlickableType {
        id: fl

        anchors.top: backButton.bottom
        anchors.bottom: parent.bottom
        anchors.bottomMargin: PageController.imeHeight
        contentHeight: content.implicitHeight + 32 + PageController.safeAreaBottomMargin

        ColumnLayout {
            id: content

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right

            spacing: 0

            BaseHeaderType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                headerText: qsTr("DNS")
                descriptionText: qsTr("Remote DNS resolves sites that go through the VPN, domestic DNS resolves sites that go directly")
            }

            RoutingReconnectNotice {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16
            }

            Header2TextType {
                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Remote DNS")
            }

            StringListDropDownType {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                drawerParent: root

                descriptionText: qsTr("Type")
                headerText: qsTr("Remote DNS type")

                stringList: root.dnsTypeNames
                currentIndex: root.remoteTypeIndex

                onActivated: function(index) {
                    root.remoteTypeIndex = index
                }
            }

            TextFieldWithHeaderType {
                id: remoteDohUrl

                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                visible: root.remoteTypeIndex === 1

                headerText: qsTr("DoH server URL")
                textField.placeholderText: "https://cloudflare-dns.com/dns-query"
                textField.inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            }

            TextFieldWithHeaderType {
                id: remoteIp

                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                headerText: qsTr("DNS server IP address")
                textField.placeholderText: qsTr("Empty: DNS of the VPN connection")
                textField.inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            }

            Header2TextType {
                Layout.fillWidth: true
                Layout.topMargin: 32
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Domestic DNS")
            }

            StringListDropDownType {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                drawerParent: root

                descriptionText: qsTr("Type")
                headerText: qsTr("Domestic DNS type")

                stringList: root.dnsTypeNames
                currentIndex: root.domesticTypeIndex

                onActivated: function(index) {
                    root.domesticTypeIndex = index
                }
            }

            TextFieldWithHeaderType {
                id: domesticDohUrl

                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                visible: root.domesticTypeIndex === 1

                headerText: qsTr("DoH server URL")
                textField.placeholderText: "https://dns.google/dns-query"
                textField.inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            }

            TextFieldWithHeaderType {
                id: domesticIp

                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                headerText: qsTr("DNS server IP address")
                textField.placeholderText: "8.8.8.8"
                textField.inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            }

            BasicButtonType {
                Layout.fillWidth: true
                Layout.topMargin: 32
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Save")

                clickedFunc: function() {
                    RoutingController.setRemoteDns(root.dnsTypes[root.remoteTypeIndex],
                                                   remoteDohUrl.textField.text,
                                                   remoteIp.textField.text)
                    RoutingController.setDomesticDns(root.dnsTypes[root.domesticTypeIndex],
                                                     domesticDohUrl.textField.text,
                                                     domesticIp.textField.text)
                    root.load()
                    PageController.showNotificationMessage(qsTr("Settings saved"))
                }
            }
        }
    }
}
