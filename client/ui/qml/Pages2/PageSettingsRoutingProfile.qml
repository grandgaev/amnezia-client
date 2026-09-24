import QtQuick
import QtQuick.Layouts

import PageEnum 1.0
import Style 1.0

import "../Controls2"
import "../Controls2/TextTypes"
import "../Components"

// Editor of the profile RoutingController.currentProfileId.
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
        contentHeight: content.implicitHeight + 32 + PageController.safeAreaBottomMargin

        ColumnLayout {
            id: content

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right

            spacing: 0

            HeaderTypeWithButton {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                headerText: RoutingController.currentName
                descriptionText: root.isActiveProfile ? qsTr("Active profile") : qsTr("This profile is not active")

                actionButtonImage: "qrc:/images/controls/edit-3.svg"
                actionButtonFunction: function() {
                    renameDrawer.openTriggered()
                }
            }

            BasicButtonType {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                visible: !root.isActiveProfile

                defaultColor: AmneziaStyle.color.transparent
                hoveredColor: AmneziaStyle.color.translucentWhite
                pressedColor: AmneziaStyle.color.sheerWhite
                disabledColor: AmneziaStyle.color.mutedGray
                textColor: AmneziaStyle.color.paleGray
                borderWidth: 1

                text: qsTr("Use this profile")

                clickedFunc: function() {
                    RoutingController.selectProfile(RoutingController.currentProfileId)
                    if (!RoutingController.routingEnabled) {
                        RoutingController.setRoutingEnabled(true)
                    }
                }
            }

            RoutingReconnectNotice {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16
            }

            SwitcherType {
                Layout.fillWidth: true
                Layout.margins: 16

                text: qsTr("Unmatched traffic goes via VPN")
                descriptionText: checked ? qsTr("Traffic that matches no rule goes through the VPN")
                                         : qsTr("Traffic that matches no rule goes directly, bypassing the VPN")

                checked: RoutingController.currentGlobalProxy
                onToggled: {
                    if (checked !== RoutingController.currentGlobalProxy) {
                        RoutingController.setGlobalProxy(checked)
                    }
                }
            }

            DividerType {}

            Header2TextType {
                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Rules")
            }

            ParagraphTextType {
                Layout.fillWidth: true
                Layout.topMargin: 8
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                color: AmneziaStyle.color.mutedGray
                text: qsTr("Sites (domain:, full:, keyword:, regexp:, geosite:) and IP addresses (addresses, subnets, geoip:)")
            }

            LabelWithButtonType {
                Layout.fillWidth: true
                Layout.topMargin: 8

                text: qsTr("Via VPN")
                descriptionText: root.rulesCountText(root.proxyRulesCount)
                rightImageSource: "qrc:/images/controls/chevron-right.svg"

                clickedFunction: function() {
                    root.openRules(0)
                }
            }

            DividerType {}

            LabelWithButtonType {
                Layout.fillWidth: true

                text: qsTr("Direct")
                descriptionText: root.rulesCountText(root.directRulesCount)
                rightImageSource: "qrc:/images/controls/chevron-right.svg"

                clickedFunction: function() {
                    root.openRules(1)
                }
            }

            DividerType {}

            LabelWithButtonType {
                Layout.fillWidth: true

                text: qsTr("Block")
                descriptionText: root.rulesCountText(root.blockRulesCount)
                rightImageSource: "qrc:/images/controls/chevron-right.svg"

                clickedFunction: function() {
                    root.openRules(2)
                }
            }

            DividerType {}

            StringListDropDownType {
                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                drawerParent: root

                descriptionText: qsTr("Rule order")
                headerText: qsTr("Rule order")

                stringList: RoutingController.routeOrderNames
                currentIndex: RoutingController.currentRouteOrderIndex

                onActivated: function(index) {
                    RoutingController.setRouteOrderIndex(index)
                }
            }

            CaptionTextType {
                Layout.fillWidth: true
                Layout.topMargin: 8
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                color: AmneziaStyle.color.mutedGray
                text: qsTr("Which rule group wins when a site or an address matches several groups")
            }

            StringListDropDownType {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                drawerParent: root

                descriptionText: qsTr("Domain strategy")
                headerText: qsTr("Domain strategy")

                stringList: RoutingController.domainStrategies
                currentIndex: RoutingController.currentDomainStrategyIndex

                onActivated: function(index) {
                    RoutingController.setDomainStrategyIndex(index)
                }
            }

            CaptionTextType {
                Layout.fillWidth: true
                Layout.topMargin: 8
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                color: AmneziaStyle.color.mutedGray
                text: qsTr("AsIs: domain rules only. IPIfNonMatch: resolve the domain for IP rules when no domain rule matches. IPOnDemand: resolve the domain for IP rules right away.")
            }

            LabelWithButtonType {
                Layout.fillWidth: true
                Layout.topMargin: 16

                text: qsTr("DNS")
                descriptionText: qsTr("Remote: %1").arg(root.dnsText(RoutingController.currentRemoteDnsType,
                                                                      RoutingController.currentRemoteDnsDomain,
                                                                      RoutingController.currentRemoteDnsIp,
                                                                      qsTr("DNS of the VPN connection")))
                                 + "\n"
                                 + qsTr("Domestic: %1").arg(root.dnsText(RoutingController.currentDomesticDnsType,
                                                                          RoutingController.currentDomesticDnsDomain,
                                                                          RoutingController.currentDomesticDnsIp,
                                                                          "8.8.8.8"))
                rightImageSource: "qrc:/images/controls/chevron-right.svg"

                clickedFunction: function() {
                    PageController.goToPage(PageEnum.PageSettingsRoutingDns)
                }
            }

            DividerType {}

            Header2TextType {
                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Geo files")
            }

            ParagraphTextType {
                Layout.fillWidth: true
                Layout.topMargin: 8
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                color: AmneziaStyle.color.mutedGray
                text: qsTr("Category lists for geosite: and geoip: rules")
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                spacing: 4

                ListItemTitleType {
                    Layout.fillWidth: true
                    text: "geosite.dat"
                }

                CaptionTextType {
                    Layout.fillWidth: true
                    color: AmneziaStyle.color.mutedGray
                    text: RoutingController.currentGeoSiteStatus
                }

                ListItemTitleType {
                    Layout.fillWidth: true
                    Layout.topMargin: 12
                    text: "geoip.dat"
                }

                CaptionTextType {
                    Layout.fillWidth: true
                    color: AmneziaStyle.color.mutedGray
                    text: RoutingController.currentGeoIpStatus
                }
            }

            WarningType {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                visible: textString !== ""

                textString: RoutingController.currentGeoError
                textColor: AmneziaStyle.color.vibrantRed
                imageColor: AmneziaStyle.color.vibrantRed
                iconPath: "qrc:/images/controls/alert-circle.svg"
            }

            TextFieldWithHeaderType {
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

            TextFieldWithHeaderType {
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

            CaptionTextType {
                Layout.fillWidth: true
                Layout.topMargin: 8
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                color: AmneziaStyle.color.mutedGray
                text: qsTr("Clear a link to restore the default one. The file is downloaded again after the link is changed.")
            }

            BasicButtonType {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                enabled: !RoutingController.currentDownloading

                defaultColor: AmneziaStyle.color.transparent
                hoveredColor: AmneziaStyle.color.translucentWhite
                pressedColor: AmneziaStyle.color.sheerWhite
                disabledColor: AmneziaStyle.color.transparent
                textColor: enabled ? AmneziaStyle.color.paleGray : AmneziaStyle.color.mutedGray
                borderWidth: 1
                borderColor: enabled ? AmneziaStyle.color.paleGray : AmneziaStyle.color.charcoalGray

                text: RoutingController.currentDownloading ? qsTr("Downloading…") : qsTr("Update geo files")
                leftImageSource: "qrc:/images/controls/refresh-cw.svg"

                clickedFunc: function() {
                    RoutingController.updateGeoFiles()
                }
            }
        }
    }

    DrawerType2 {
        id: renameDrawer

        anchors.fill: parent
        expandedHeight: root.height * 0.35 + PageController.safeAreaBottomMargin + PageController.imeHeight

        expandedStateContent: ColumnLayout {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 32
            anchors.leftMargin: 16
            anchors.rightMargin: 16

            spacing: 16

            Connections {
                target: renameDrawer

                function onOpened() {
                    profileNameField.textField.text = RoutingController.currentName
                    profileNameField.errorText = ""
                    profileNameField.textField.forceActiveFocus()
                }
            }

            TextFieldWithHeaderType {
                id: profileNameField

                Layout.fillWidth: true

                headerText: qsTr("Profile name")
                textField.maximumLength: 64
                rightButtonClickedOnEnter: true
                clickedFunc: function() {
                    saveNameButton.clicked()
                }
            }

            BasicButtonType {
                id: saveNameButton

                Layout.fillWidth: true

                text: qsTr("Save")

                clickedFunc: function() {
                    var name = profileNameField.textField.text.trim()
                    if (name === "") {
                        profileNameField.errorText = qsTr("The field can't be empty")
                        return
                    }
                    if (name === RoutingController.currentName
                            || RoutingController.renameProfile(RoutingController.currentProfileId, name)) {
                        renameDrawer.closeTriggered()
                    }
                }
            }
        }
    }
}
