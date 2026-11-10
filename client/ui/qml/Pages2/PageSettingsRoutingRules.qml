import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import SortFilterProxyModel 0.2

import Style 1.0

import "../Controls2"
import "../Controls2/TextTypes"
import "../Components"

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
        warning.text = ""
    }

    function save() {
        var text = rulesArea.textAreaText
        var warningText = RoutingController.setRulesText(root.action, text)
        root.savedText = text
        warning.text = warningText
        return warningText
    }

    function insertText(value, onNewLine) {
        var area = rulesArea.textArea
        var position = area.cursorPosition
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
                geoTagsDrawer.open()
            } else {
                PageController.showErrorMessage(message)
            }
        }
    }

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

                title: root.titles[root.action]
                description: root.descriptions[root.action]
            }

            RoutingReconnectNotice {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: 16
            }

            SmallTextType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

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

            NotificationType {
                id: warning

                Layout.fillWidth: true
                Layout.topMargin: 8
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                visible: text !== ""

                variant: NotificationType.Warning
            }

            Flow {
                Layout.fillWidth: true
                Layout.topMargin: 12
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                spacing: 8

                Repeater {
                    model: root.prefixes

                    delegate: ChipType {
                        required property string modelData

                        text: modelData

                        onClicked: {
                            root.insertText(modelData, true)
                        }
                    }
                }
            }

            ListItemType {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Examples")
                subtitle: qsTr("Supported rule formats")
                trailingIconSource: "qrc:/images/controls/chevron-right.svg"

                onClicked: {
                    examplesDrawer.open()
                }
            }

            ListItemType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Pick geosite categories")
                subtitle: qsTr("Site categories from the geosite file")
                trailingIconSource: "qrc:/images/controls/chevron-right.svg"

                onClicked: {
                    geoTagsDrawer.isGeoIp = false
                    PageController.showBusyIndicator(true)
                    RoutingController.loadGeoTags(false, rulesArea.textAreaText)
                }
            }

            ListItemType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Pick geoip categories")
                subtitle: qsTr("Countries and address categories from the geoip file")
                trailingIconSource: "qrc:/images/controls/chevron-right.svg"

                onClicked: {
                    geoTagsDrawer.isGeoIp = true
                    PageController.showBusyIndicator(true)
                    RoutingController.loadGeoTags(true, rulesArea.textAreaText)
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                visible: root.action === root.actionDirect

                spacing: 8

                SmallTextType {
                    Layout.fillWidth: true

                    text: qsTr("To keep devices of your local network reachable, add the local network addresses:") + "\n"
                          + RoutingController.lanAddressesText().split("\n").join(", ")
                }

                ButtonType {
                    Layout.fillWidth: true

                    variant: ButtonType.Secondary

                    text: qsTr("Add local network addresses")

                    onClicked: {
                        root.insertText(RoutingController.lanAddressesText(), true)
                    }
                }
            }

            ButtonType {
                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Save")

                onClicked: {
                    if (root.save() === "") {
                        PageController.showNotificationMessage(qsTr("Rules saved"))
                    }
                }
            }
        }
    }

    SheetType {
        id: examplesDrawer

        expandedHeight: windowHeight * 0.9

        Item {
            anchors.fill: parent

            ListViewType {
                id: examplesListView

                anchors.fill: parent
                anchors.bottomMargin: PageController.safeAreaBottomMargin

                header: SectionHeaderType {
                    width: examplesListView.width - 32
                    x: 16
                    height: implicitHeight + 32

                    headerText: qsTr("Rule examples")
                    descriptionText: qsTr("Tap an example to add it to the list")
                }

                model: root.examplesModel

                delegate: Item {
                    id: exampleDelegate

                    required property string example
                    required property string explanation

                    width: examplesListView.width
                    implicitHeight: exampleItem.implicitHeight

                    ListItemType {
                        id: exampleItem

                        anchors.fill: parent
                        anchors.leftMargin: 16
                        anchors.rightMargin: 16

                        text: exampleDelegate.example
                        subtitle: exampleDelegate.explanation
                        trailingIconSource: "qrc:/images/controls/plus.svg"

                        onClicked: {
                            examplesDrawer.close()
                            root.insertText(exampleDelegate.example, true)
                        }
                    }
                }
            }
        }
    }

    SheetType {
        id: geoTagsDrawer

        property bool isGeoIp: false

        expandedHeight: windowHeight * 0.9

        onOpened: {
            geoTagsSearchField.text = ""
            geoTagsListView.positionViewAtBeginning()
        }

        Item {
            anchors.fill: parent

            ColumnLayout {
                id: geoTagsHeader

                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.topMargin: 12
                anchors.leftMargin: 16
                anchors.rightMargin: 16

                spacing: 16

                SectionHeaderType {
                    Layout.fillWidth: true

                    headerText: geoTagsDrawer.isGeoIp ? qsTr("GeoIP categories") : qsTr("GeoSite categories")
                }

                SearchFieldType {
                    id: geoTagsSearchField

                    Layout.fillWidth: true
                    Layout.bottomMargin: 8
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
                        pattern: ".*" + root.escapeRegExp(geoTagsSearchField.text) + ".*"
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

                    text: section
                }

                delegate: Item {
                    id: geoTagDelegate

                    required property string value
                    required property bool isSelected

                    width: geoTagsListView.width
                    implicitHeight: geoTagCheckBox.implicitHeight + 16

                    CheckBoxType {
                        id: geoTagCheckBox

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 16
                        anchors.rightMargin: 16

                        text: geoTagDelegate.value.substring(geoTagDelegate.value.indexOf(":") + 1)
                        checked: geoTagDelegate.isSelected

                        onCheckedChanged: {
                            if (checked !== geoTagDelegate.isSelected) {
                                GeoTagsModel.setSelected(geoTagDelegate.value, checked)
                            }
                        }
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.leftMargin: 16
                        anchors.rightMargin: 16

                        height: 1
                        color: AmneziaStyle.color.borderSoft
                    }
                }
            }

            ButtonType {
                id: applyGeoTagsButton

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                anchors.bottomMargin: Math.max(16, PageController.safeAreaBottomMargin)

                text: qsTr("Apply (%1 selected)").arg(GeoTagsModel.selectedCount)

                onClicked: {
                    rulesArea.textAreaText = RoutingController.applyGeoTags(rulesArea.textAreaText)
                    geoTagsDrawer.close()
                }
            }
        }
    }
}
