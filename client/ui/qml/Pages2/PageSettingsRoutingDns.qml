import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../Controls2"
import "../Controls2/TextTypes"
import "../Components"

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

    FlickableType {
        id: fl

        anchors.top: parent.top
        anchors.bottomMargin: PageController.imeHeight

        contentHeight: content.implicitHeight + 32 + PageController.safeAreaBottomMargin

        ColumnLayout {
            id: content

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right

            spacing: 0

            NavigationBarType {
                Layout.fillWidth: true

                title: qsTr("DNS")
                description: qsTr("Remote DNS resolves sites that go through the VPN, domestic DNS resolves sites that go directly")
            }

            RoutingReconnectNotice {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: 16
            }

            SectionHeaderType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                level: SectionHeaderType.Section
                headerText: qsTr("Remote DNS")
            }

            StringListDropDownType {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                descriptionText: qsTr("Type")
                headerText: qsTr("Remote DNS type")

                stringList: root.dnsTypeNames
                currentIndex: root.remoteTypeIndex

                onActivated: function(index) {
                    root.remoteTypeIndex = index
                }
            }

            TextFieldType {
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

            TextFieldType {
                id: remoteIp

                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                headerText: qsTr("DNS server IP address")
                textField.placeholderText: qsTr("Empty: DNS of the VPN connection")
                textField.inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            }

            SectionHeaderType {
                Layout.fillWidth: true
                Layout.topMargin: 32
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                level: SectionHeaderType.Section
                headerText: qsTr("Domestic DNS")
            }

            StringListDropDownType {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                descriptionText: qsTr("Type")
                headerText: qsTr("Domestic DNS type")

                stringList: root.dnsTypeNames
                currentIndex: root.domesticTypeIndex

                onActivated: function(index) {
                    root.domesticTypeIndex = index
                }
            }

            TextFieldType {
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

            TextFieldType {
                id: domesticIp

                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                headerText: qsTr("DNS server IP address")
                textField.placeholderText: "8.8.8.8"
                textField.inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            }

            ButtonType {
                Layout.fillWidth: true
                Layout.topMargin: 32
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Save")

                onClicked: {
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
