import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import PageEnum 1.0
import Style 1.0

import "../Controls2"
import "../Controls2/TextTypes"
import "../Components"

PageType {
    id: root

    readonly property bool isActiveProfile: RoutingController.routingEnabled
                                            && RoutingController.selectedProfileId === RoutingController.currentProfileId

    property int proxyRulesCount: 0
    property int directRulesCount: 0
    property int blockRulesCount: 0

    function updateRulesCounts() {
        root.proxyRulesCount = RoutingController.rulesCount(0)
        root.directRulesCount = RoutingController.rulesCount(1)
        root.blockRulesCount = RoutingController.rulesCount(2)
    }

    function openRules(action) {
        RoutingController.currentRuleAction = action
        PageController.goToPage(PageEnum.PageSettingsRoutingRules)
    }

    function rulesCountText(count) {
        return count > 0 ? qsTr("%n rule(s)", "", count) : qsTr("No rules")
    }

    function dnsText(type, domain, ip, emptyIpText) {
        var server = ip !== "" ? ip : emptyIpText
        if (type === "DoH" && domain !== "") {
            server = domain
        }
        return type + " · " + server
    }

    Component.onCompleted: updateRulesCounts()

    Connections {
        target: RoutingController

        function onCurrentProfileChanged() {
            root.updateRulesCounts()
        }
    }

    FlickableType {
        id: fl

        anchors.top: parent.top

        contentHeight: content.implicitHeight + 32 + PageController.safeAreaBottomMargin

        ColumnLayout {
            id: content

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right

            spacing: 0

            NavigationBarType {
                Layout.fillWidth: true

                title: RoutingController.currentName
                description: root.isActiveProfile ? qsTr("Active profile") : qsTr("This profile is not active")

                IconButtonType {
                    icon.source: "qrc:/images/controls/edit-3.svg"
                    Accessible.name: qsTr("Rename profile")
                    Accessible.description: RoutingController.currentName

                    onClicked: renameDrawer.open()
                }
            }

            ButtonType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: 16

                visible: !root.isActiveProfile

                variant: ButtonType.Secondary

                text: qsTr("Use this profile")

                onClicked: {
                    RoutingController.selectProfile(RoutingController.currentProfileId)
                    if (!RoutingController.routingEnabled) {
                        RoutingController.setRoutingEnabled(true)
                    }
                }
            }

            RoutingReconnectNotice {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: 16
            }

            ListItemType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Unmatched traffic goes via VPN")
                subtitle: globalProxySwitch.checked ? qsTr("Traffic that matches no rule goes through the VPN")
                                                    : qsTr("Traffic that matches no rule goes directly, bypassing the VPN")

                onClicked: {
                    globalProxySwitch.toggle()
                    globalProxySwitch.toggled()
                }

                SwitchType {
                    id: globalProxySwitch

                    focusPolicy: Qt.NoFocus

                    checked: RoutingController.currentGlobalProxy

                    onToggled: {
                        if (checked !== RoutingController.currentGlobalProxy) {
                            RoutingController.setGlobalProxy(checked)
                        }
                    }
                }
            }

            SectionHeaderType {
                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: 8

                level: SectionHeaderType.Section
                headerText: qsTr("Rules")
                descriptionText: qsTr("Sites (domain:, full:, keyword:, regexp:, geosite:) and IP addresses (addresses, subnets, geoip:)")
            }

            ListItemType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Via VPN")
                subtitle: root.rulesCountText(root.proxyRulesCount)
                trailingIconSource: "qrc:/images/controls/chevron-right.svg"

                onClicked: {
                    root.openRules(0)
                }
            }

            ListItemType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Direct")
                subtitle: root.rulesCountText(root.directRulesCount)
                trailingIconSource: "qrc:/images/controls/chevron-right.svg"

                onClicked: {
                    root.openRules(1)
                }
            }

            ListItemType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Block")
                subtitle: root.rulesCountText(root.blockRulesCount)
                trailingIconSource: "qrc:/images/controls/chevron-right.svg"

                onClicked: {
                    root.openRules(2)
                }
            }

            StringListDropDownType {
                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                descriptionText: qsTr("Rule order")
                headerText: qsTr("Rule order")

                stringList: RoutingController.routeOrderNames
                currentIndex: RoutingController.currentRouteOrderIndex

                onActivated: function(index) {
                    RoutingController.setRouteOrderIndex(index)
                }
            }

            SmallTextType {
                Layout.fillWidth: true
                Layout.topMargin: 8
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Which rule group wins when a site or an address matches several groups")
            }

            StringListDropDownType {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                descriptionText: qsTr("How domain names are matched against IP rules")
                headerText: qsTr("Domain matching")

                stringList: RoutingController.domainStrategies
                currentIndex: RoutingController.currentDomainStrategyIndex

                onActivated: function(index) {
                    RoutingController.setDomainStrategyIndex(index)
                }
            }

            SmallTextType {
                Layout.fillWidth: true
                Layout.topMargin: 8
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Choose whether IP address rules should also be applied to domain names")
            }

            ListItemType {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("DNS")
                subtitle: qsTr("Remote: %1").arg(root.dnsText(RoutingController.currentRemoteDnsType,
                                                              RoutingController.currentRemoteDnsDomain,
                                                              RoutingController.currentRemoteDnsIp,
                                                              qsTr("DNS of the VPN connection")))
                          + "\n"
                          + qsTr("Domestic: %1").arg(root.dnsText(RoutingController.currentDomesticDnsType,
                                                                  RoutingController.currentDomesticDnsDomain,
                                                                  RoutingController.currentDomesticDnsIp,
                                                                  "8.8.8.8"))
                trailingIconSource: "qrc:/images/controls/chevron-right.svg"

                onClicked: {
                    PageController.goToPage(PageEnum.PageSettingsRoutingDns)
                }
            }

            SectionHeaderType {
                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                level: SectionHeaderType.Section
                headerText: qsTr("Geo files")
                descriptionText: qsTr("Category lists for geosite: and geoip: rules")
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                spacing: 4

                BodyTextType {
                    Layout.fillWidth: true
                    text: "geosite.dat"
                }

                SmallTextType {
                    Layout.fillWidth: true
                    text: RoutingController.currentGeoSiteStatus
                }

                BodyTextType {
                    Layout.fillWidth: true
                    Layout.topMargin: 12
                    text: "geoip.dat"
                }

                SmallTextType {
                    Layout.fillWidth: true
                    text: RoutingController.currentGeoIpStatus
                }
            }

            NotificationType {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                visible: text !== ""

                variant: NotificationType.Error
                text: RoutingController.currentGeoError
            }

            TextFieldType {
                id: geoSiteUrlField

                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                headerText: qsTr("Link to geosite.dat")
                textField.inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText

                Binding {
                    target: geoSiteUrlField.textField
                    property: "text"
                    value: RoutingController.currentGeoSiteUrl
                    when: !geoSiteUrlField.textField.activeFocus
                    restoreMode: Binding.RestoreNone
                }

                buttonImageSource: textField.text.trim() !== RoutingController.currentGeoSiteUrl ? "qrc:/images/controls/check.svg" : ""
                rightButtonClickedOnEnter: true
                clickedFunc: function() {
                    geoSiteUrlField.textField.focus = false
                    RoutingController.setGeoSiteUrl(geoSiteUrlField.textField.text)
                    geoSiteUrlField.textField.text = RoutingController.currentGeoSiteUrl
                }
            }

            TextFieldType {
                id: geoIpUrlField

                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                headerText: qsTr("Link to geoip.dat")
                textField.inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText

                Binding {
                    target: geoIpUrlField.textField
                    property: "text"
                    value: RoutingController.currentGeoIpUrl
                    when: !geoIpUrlField.textField.activeFocus
                    restoreMode: Binding.RestoreNone
                }

                buttonImageSource: textField.text.trim() !== RoutingController.currentGeoIpUrl ? "qrc:/images/controls/check.svg" : ""
                rightButtonClickedOnEnter: true
                clickedFunc: function() {
                    geoIpUrlField.textField.focus = false
                    RoutingController.setGeoIpUrl(geoIpUrlField.textField.text)
                    geoIpUrlField.textField.text = RoutingController.currentGeoIpUrl
                }
            }

            SmallTextType {
                Layout.fillWidth: true
                Layout.topMargin: 8
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Clear a link to restore the default one. The file is downloaded again after the link is changed.")
            }

            ButtonType {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                enabled: !RoutingController.currentDownloading

                variant: ButtonType.Secondary

                text: RoutingController.currentDownloading ? qsTr("Downloading…") : qsTr("Update geo files")
                leftImageSource: "qrc:/images/controls/refresh-cw.svg"

                onClicked: {
                    RoutingController.updateGeoFiles()
                }
            }
        }
    }

    SheetType {
        id: renameDrawer

        expandedHeight: renameContent.implicitHeight + 32 + PageController.safeAreaBottomMargin + PageController.imeHeight

        onOpened: {
            profileNameField.textField.text = RoutingController.currentName
            profileNameField.errorText = ""
            profileNameField.textField.forceActiveFocus()
        }

        ColumnLayout {
            id: renameContent

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 16
            anchors.leftMargin: 16
            anchors.rightMargin: 16

            spacing: 24

            TextFieldType {
                id: profileNameField

                Layout.fillWidth: true

                headerText: qsTr("Profile name")
                textField.maximumLength: 64
                rightButtonClickedOnEnter: true
                clickedFunc: function() {
                    saveNameButton.clicked()
                }
            }

            ButtonType {
                id: saveNameButton

                Layout.fillWidth: true

                text: qsTr("Save")

                onClicked: {
                    var name = profileNameField.textField.text.trim()
                    if (name === "") {
                        profileNameField.errorText = qsTr("The field can't be empty")
                        return
                    }
                    if (name === RoutingController.currentName
                            || RoutingController.renameProfile(RoutingController.currentProfileId, name)) {
                        renameDrawer.close()
                    }
                }
            }
        }
    }
}
