import QtQuick
import QtQuick.Layouts

import SortFilterProxyModel 0.2

import Style 1.0

import "../Controls2"
import "../Controls2/TextTypes"
import "../Components"

// Plain text editor of the sites and IPs of one rule group
// (RoutingController.currentRuleAction) of the current profile.
PageType {
    id: root

    readonly property int action: RoutingController.currentRuleAction
    readonly property int actionDirect: 1

    property string savedText
    readonly property bool isModified: rulesArea.textAreaText !== root.savedText

    readonly property var titles: [qsTr("Via VPN"), qsTr("Direct"), qsTr("Block")]
    readonly property var descriptions: [
        qsTr("Sites and addresses that always go through the VPN"),
        qsTr("Sites and addresses that always go directly, bypassing the VPN"),
        qsTr("Sites and addresses that are blocked")
    ]
    readonly property var prefixes: ["domain:", "full:", "keyword:", "regexp:", "geosite:", "geoip:"]

    property list<QtObject> examplesModel: [
        QtObject {
            readonly property string example: "domain:example.com"
            readonly property string explanation: qsTr("The domain and all its subdomains")
        },
        QtObject {
            readonly property string example: "full:www.example.com"
            readonly property string explanation: qsTr("Exactly this domain")
        },
        QtObject {
            readonly property string example: "keyword:example"
            readonly property string explanation: qsTr("Any domain that contains the text")
        },
        QtObject {
            readonly property string example: "regexp:\\.example\\.(com|net)$"
            readonly property string explanation: qsTr("Domains that match a regular expression")
        },
        QtObject {
            readonly property string example: "example.com"
            readonly property string explanation: qsTr("An entry without a prefix matches any domain that contains it")
        },
        QtObject {
            readonly property string example: "geosite:youtube"
            readonly property string explanation: qsTr("A category of the geosite file")
        },
        QtObject {
            readonly property string example: "geosite:category-ads-all"
            readonly property string explanation: qsTr("Advertising domains, useful for blocking")
        },
        QtObject {
            readonly property string example: "203.0.113.10, 10.0.0.0/8"
            readonly property string explanation: qsTr("IP addresses and subnets")
        },
        QtObject {
            readonly property string example: "geoip:us"
            readonly property string explanation: qsTr("IP addresses of a country from the geoip file")
        },
        QtObject {
            readonly property string example: "geoip:private"
            readonly property string explanation: qsTr("Private and local network addresses")
        }
    ]

    function loadText() {
        root.savedText = RoutingController.rulesText(root.action)
        rulesArea.textAreaText = root.savedText
        warning.textString = ""
    }

    function save() {
        var text = rulesArea.textAreaText
        var warningText = RoutingController.setRulesText(root.action, text)
        root.savedText = text
        warning.textString = warningText
        return warningText
    }

    // Inserts text at the cursor of the editor, or appends it on a new line.
    function insertText(value, onNewLine) {
        var area = rulesArea.textArea
        var position = area.activeFocus ? area.cursorPosition : area.length
        var before = area.getText(0, position)
        var insertion = value
        if (onNewLine && before.length > 0 && before.charAt(before.length - 1) !== "\n") {
            insertion = "\n" + insertion
        }
        area.insert(position, insertion)
        area.cursorPosition = position + insertion.length
        area.forceActiveFocus()
    }

    function escapeRegExp(text) {
        return text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
    }

    Component.onCompleted: loadText()

    // Leaving the page keeps the edited rules.
    Component.onDestruction: {
        if (root.isModified) {
            var warningText = root.save()
            if (warningText !== "") {
                PageController.showNotificationMessage(warningText)
            }
        }
    }

    Connections {
        target: RoutingController

        function onGeoTagsLoaded(success, message) {
            PageController.showBusyIndicator(false)
            if (success) {
                geoTagsDrawer.openTriggered()
            } else {
                PageController.showErrorMessage(message)
            }
        }
    }

    BackButtonType {
        id: backButton

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: 20 + PageController.safeAreaTopMargin

        backButtonFunction: function() {
            if (root.isModified) {
                var warningText = root.save()
                if (warningText !== "") {
                    PageController.showNotificationMessage(warningText)
                }
            }
            PageController.closePage()
        }

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

                headerText: root.titles[root.action]
                descriptionText: root.descriptions[root.action]
            }

            RoutingReconnectNotice {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16
            }

            CaptionTextType {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                color: AmneziaStyle.color.mutedGray
                text: qsTr("One rule per line, commas also separate rules. Lines starting with # are comments.")
            }

            TextAreaType {
                id: rulesArea

                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(200, root.height * 0.4)
                Layout.topMargin: 8
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                placeholderText: "domain:example.com\ngeosite:youtube\n10.0.0.0/8\ngeoip:us"

                textArea.inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            }

            WarningType {
                id: warning

                Layout.fillWidth: true
                Layout.topMargin: 8
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                visible: textString !== ""

                textColor: AmneziaStyle.color.goldenApricot
                imageColor: AmneziaStyle.color.goldenApricot
                iconPath: "qrc:/images/controls/alert-circle.svg"
            }

            Flow {
                Layout.fillWidth: true
                Layout.topMargin: 12
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                spacing: 8

                Repeater {
                    model: root.prefixes

                    delegate: BasicButtonType {
                        required property string modelData

                        implicitHeight: 36
                        leftPadding: 12
                        rightPadding: 12

                        // Keeps the focus (and the cursor) in the editor.
                        focusPolicy: Qt.NoFocus

                        defaultColor: AmneziaStyle.color.transparent
                        hoveredColor: AmneziaStyle.color.translucentWhite
                        pressedColor: AmneziaStyle.color.sheerWhite
                        disabledColor: AmneziaStyle.color.mutedGray
                        textColor: AmneziaStyle.color.paleGray
                        borderWidth: 1
                        borderColor: AmneziaStyle.color.charcoalGray

                        buttonTextLabel.font.pixelSize: 14
                        buttonTextLabel.lineHeight: 20

                        text: modelData

                        clickedFunc: function() {
                            root.insertText(modelData, true)
                        }
                    }
                }
            }

            LabelWithButtonType {
                Layout.fillWidth: true
                Layout.topMargin: 16

                text: qsTr("Examples")
                descriptionText: qsTr("Supported rule formats")
                rightImageSource: "qrc:/images/controls/chevron-right.svg"

                clickedFunction: function() {
                    examplesDrawer.openTriggered()
                }
            }

            DividerType {}

            LabelWithButtonType {
                Layout.fillWidth: true

                text: qsTr("Pick geosite categories")
                descriptionText: qsTr("Site categories from the geosite file")
                rightImageSource: "qrc:/images/controls/chevron-right.svg"

                clickedFunction: function() {
                    geoTagsDrawer.isGeoIp = false
                    PageController.showBusyIndicator(true)
                    RoutingController.loadGeoTags(false, rulesArea.textAreaText)
                }
            }

            DividerType {}

            LabelWithButtonType {
                Layout.fillWidth: true

                text: qsTr("Pick geoip categories")
                descriptionText: qsTr("Countries and address categories from the geoip file")
                rightImageSource: "qrc:/images/controls/chevron-right.svg"

                clickedFunction: function() {
                    geoTagsDrawer.isGeoIp = true
                    PageController.showBusyIndicator(true)
                    RoutingController.loadGeoTags(true, rulesArea.textAreaText)
                }
            }

            DividerType {}

            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                visible: root.action === root.actionDirect

                spacing: 8

                CaptionTextType {
                    Layout.fillWidth: true

                    color: AmneziaStyle.color.mutedGray
                    text: qsTr("To keep devices of your local network reachable, add the local network addresses:") + "\n"
                          + RoutingController.lanAddressesText().split("\n").join(", ")
                }

                BasicButtonType {
                    Layout.fillWidth: true

                    defaultColor: AmneziaStyle.color.transparent
                    hoveredColor: AmneziaStyle.color.translucentWhite
                    pressedColor: AmneziaStyle.color.sheerWhite
                    disabledColor: AmneziaStyle.color.mutedGray
                    textColor: AmneziaStyle.color.paleGray
                    borderWidth: 1

                    text: qsTr("Add local network addresses")

                    clickedFunc: function() {
                        root.insertText(RoutingController.lanAddressesText(), true)
                    }
                }
            }

            BasicButtonType {
                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Save")

                clickedFunc: function() {
                    if (root.save() === "") {
                        PageController.showNotificationMessage(qsTr("Rules saved"))
                    }
                }
            }
        }
    }

    DrawerType2 {
        id: examplesDrawer

        anchors.fill: parent
        expandedHeight: root.height * 0.9

        expandedStateContent: Item {
            implicitHeight: examplesDrawer.expandedHeight

            BackButtonType {
                id: examplesBackButton

                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.topMargin: 16

                backButtonImage: "qrc:/images/controls/arrow-left.svg"
                backButtonFunction: function() {
                    examplesDrawer.closeTriggered()
                }
            }

            ListViewType {
                id: examplesListView

                anchors.top: examplesBackButton.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.bottomMargin: PageController.safeAreaBottomMargin

                header: ColumnLayout {
                    width: examplesListView.width

                    Header2Type {
                        Layout.fillWidth: true
                        Layout.margins: 16

                        headerText: qsTr("Rule examples")
                        descriptionText: qsTr("Tap an example to add it to the list")
                    }
                }

                model: root.examplesModel

                delegate: ColumnLayout {
                    id: exampleDelegate

                    required property string example
                    required property string explanation

                    width: examplesListView.width
                    spacing: 0

                    LabelWithButtonType {
                        Layout.fillWidth: true

                        text: exampleDelegate.example
                        descriptionText: exampleDelegate.explanation
                        rightImageSource: "qrc:/images/controls/plus.svg"

                        clickedFunction: function() {
                            examplesDrawer.closeTriggered()
                            root.insertText(exampleDelegate.example, true)
                        }
                    }

                    DividerType {}
                }
            }
        }
    }

    DrawerType2 {
        id: geoTagsDrawer

        property bool isGeoIp: false

        anchors.fill: parent
        expandedHeight: root.height * 0.9

        expandedStateContent: Item {
            implicitHeight: geoTagsDrawer.expandedHeight

            Connections {
                target: geoTagsDrawer

                function onOpened() {
                    geoTagsSearchField.textField.text = ""
                    geoTagsListView.positionViewAtBeginning()
                }
            }

            ColumnLayout {
                id: geoTagsHeader

                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.topMargin: 16

                spacing: 0

                BackButtonType {
                    Layout.fillWidth: true

                    backButtonImage: "qrc:/images/controls/arrow-left.svg"
                    backButtonFunction: function() {
                        geoTagsDrawer.closeTriggered()
                    }
                }

                Header2Type {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16

                    headerText: geoTagsDrawer.isGeoIp ? qsTr("GeoIP categories") : qsTr("GeoSite categories")
                }

                TextFieldWithHeaderType {
                    id: geoTagsSearchField

                    Layout.fillWidth: true
                    Layout.topMargin: 16
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.bottomMargin: 8

                    backgroundColor: AmneziaStyle.color.slateGray

                    textField.placeholderText: qsTr("Search")
                }
            }

            ListViewType {
                id: geoTagsListView

                anchors.top: geoTagsHeader.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: applyGeoTagsButton.top
                anchors.bottomMargin: 8

                model: SortFilterProxyModel {
                    id: geoTagsProxyModel

                    sourceModel: GeoTagsModel
                    filters: RegExpFilter {
                        roleName: "value"
                        pattern: ".*" + root.escapeRegExp(geoTagsSearchField.textField.text) + ".*"
                        caseSensitivity: Qt.CaseInsensitive
                    }
                }

                section.property: "header"
                section.delegate: CaptionTextType {
                    required property string section

                    width: geoTagsListView.width
                    leftPadding: 16
                    rightPadding: 16
                    topPadding: 8

                    color: AmneziaStyle.color.mutedGray
                    text: section
                }

                delegate: ColumnLayout {
                    id: geoTagDelegate

                    required property string value
                    required property bool isSelected

                    width: geoTagsListView.width
                    spacing: 0

                    CheckBoxType {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16

                        text: geoTagDelegate.value.substring(geoTagDelegate.value.indexOf(":") + 1)
                        checked: geoTagDelegate.isSelected

                        onCheckedChanged: {
                            if (checked !== geoTagDelegate.isSelected) {
                                GeoTagsModel.setSelected(geoTagDelegate.value, checked)
                            }
                        }
                    }

                    DividerType {}
                }
            }

            BasicButtonType {
                id: applyGeoTagsButton

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                anchors.bottomMargin: 16 + PageController.safeAreaBottomMargin

                text: qsTr("Apply (%1 selected)").arg(GeoTagsModel.selectedCount)

                clickedFunc: function() {
                    rulesArea.textAreaText = RoutingController.applyGeoTags(rulesArea.textAreaText)
                    geoTagsDrawer.closeTriggered()
                }
            }
        }
    }
}
