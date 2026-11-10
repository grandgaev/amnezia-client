import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QtCore

import PageEnum 1.0
import Style 1.0

import "../Controls2"
import "../Controls2/TextTypes"
import "../Config"
import "../Components"

PageType {
    id: root

    property string actionsProfileId
    property string actionsProfileName

    function openProfile(profileId) {
        RoutingController.currentProfileId = profileId
        PageController.goToPage(PageEnum.PageSettingsRoutingProfile)
    }

    function exportProfileToFile(profileId, profileName) {
        var baseName = profileName.replace(/[\\\/:*?"<>|]/g, "_")
        if (baseName === "") {
            baseName = "routing_profile"
        }
        var fileName = ""
        if (GC.isMobile()) {
            fileName = baseName + ".json"
        } else {
            fileName = SystemController.getFileName(qsTr("Save routing profile"),
                                                    qsTr("Routing profiles (*.json)"),
                                                    StandardPaths.standardLocations(StandardPaths.DocumentsLocation) + "/" + baseName,
                                                    true,
                                                    ".json")
        }
        if (fileName !== "") {
            PageController.showBusyIndicator(true)
            RoutingController.exportToFile(profileId, fileName)
            PageController.showBusyIndicator(false)
        }
    }

    ListViewType {
        id: listView

        anchors.fill: parent

        header: ColumnLayout {
            width: listView.width

            z: 2

            spacing: 0

            NavigationBarType {
                Layout.fillWidth: true

                title: qsTr("Routing")
                description: qsTr("Split tunneling: sites, IPs, geosite, geoip")
            }

            ListItemType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Routing")

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

            NotificationType {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                visible: text !== ""

                variant: NotificationType.Warning
                text: RoutingController.protocolSupportText
            }

            RoutingReconnectNotice {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16
            }

            SectionHeaderType {
                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: 8

                level: SectionHeaderType.Section
                headerText: qsTr("Profiles")
                descriptionText: RoutingController.profilesCount > 0
                                 ? qsTr("The selected profile is used for connections when routing is on")
                                 : qsTr("No profiles yet. Add a new profile or import one: JSON, base64 or a routing link")
            }
        }

        model: RoutingProfilesModel

        delegate: Item {
            id: profileDelegate

            required property string profileId
            required property string name
            required property string description
            required property bool isSelected
            required property bool hasGeoError
            required property bool isDownloading

            width: listView.width
            implicitHeight: profileItem.implicitHeight

            RadioItemType {
                id: profileItem

                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16

                text: profileDelegate.name
                subtitle: profileDelegate.isDownloading
                          ? profileDelegate.description + "\n" + qsTr("Downloading geo files…")
                          : profileDelegate.description
                subtitleColor: profileDelegate.hasGeoError ? AmneziaStyle.color.statusDestructive : AmneziaStyle.color.textTertiary
                maximumLineCount: 2

                checked: profileDelegate.isSelected

                onClicked: {
                    RoutingController.selectProfile(profileDelegate.profileId)
                }

                IconButtonType {
                    icon.source: "qrc:/images/controls/settings.svg"
                    Accessible.name: qsTr("Edit profile")
                    Accessible.description: profileDelegate.name

                    onClicked: {
                        root.openProfile(profileDelegate.profileId)
                    }
                }

                IconButtonType {
                    icon.source: "qrc:/images/controls/more-vertical.svg"
                    Accessible.name: qsTr("More profile actions")
                    Accessible.description: profileDelegate.name

                    onClicked: {
                        root.actionsProfileId = profileDelegate.profileId
                        root.actionsProfileName = profileDelegate.name
                        profileActionsDrawer.open()
                    }
                }
            }
        }

        footer: ColumnLayout {
            width: listView.width

            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                spacing: 8

                ButtonType {
                    Layout.fillWidth: true

                    text: qsTr("Add profile")
                    leftImageSource: "qrc:/images/controls/plus.svg"

                    onClicked: {
                        addProfileDrawer.open()
                    }
                }

                ButtonType {
                    Layout.fillWidth: true

                    variant: ButtonType.Secondary

                    text: qsTr("Import")
                    leftImageSource: "qrc:/images/controls/download.svg"

                    onClicked: {
                        importDrawer.open()
                    }
                }
            }

            SectionHeaderType {
                Layout.fillWidth: true
                Layout.topMargin: 32
                Layout.bottomMargin: 8
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                level: SectionHeaderType.Section
                headerText: qsTr("Settings")
            }

            ListItemType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Excluded addresses")
                subtitle: qsTr("Addresses and subnets that always bypass the VPN when routing is on, e.g. local networks")
                trailingIconSource: "qrc:/images/controls/chevron-right.svg"

                onClicked: {
                    excludedRoutesDrawer.open()
                }
            }

            StringListDropDownType {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.bottomMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                descriptionText: qsTr("Browser identity used to download geo files")
                headerText: qsTr("Browser profile")

                stringList: RoutingController.geoUserAgents
                currentIndex: RoutingController.geoUserAgentIndex

                onActivated: function(index) {
                    RoutingController.setGeoUserAgentIndex(index)
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
                    PageController.goToPage(PageEnum.PageSettingsAppSplitTunneling)
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 16 + PageController.safeAreaBottomMargin
            }
        }
    }

    SheetType {
        id: profileActionsDrawer

        expandedHeight: profileActionsContent.implicitHeight + 16 + PageController.safeAreaBottomMargin

        ColumnLayout {
            id: profileActionsContent

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 16

            spacing: 0

            SectionHeaderType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: 16

                headerText: root.actionsProfileName
            }

            ListItemType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Edit")
                trailingIconSource: "qrc:/images/controls/chevron-right.svg"

                onClicked: {
                    profileActionsDrawer.close()
                    root.openProfile(root.actionsProfileId)
                }
            }

            ListItemType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Rename")

                onClicked: {
                    profileActionsDrawer.close()
                    renameProfileDrawer.open()
                }
            }

            ListItemType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Duplicate")

                onClicked: {
                    profileActionsDrawer.close()
                    var id = RoutingController.duplicateProfile(root.actionsProfileId)
                    if (id !== "") {
                        PageController.showNotificationMessage(qsTr("Profile \"%1\" is created").arg(RoutingController.profileName(id)))
                    }
                }
            }

            ListItemType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Copy to clipboard")

                onClicked: {
                    profileActionsDrawer.close()
                    RoutingController.exportToClipboard(root.actionsProfileId)
                }
            }

            ListItemType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Save to file")

                onClicked: {
                    profileActionsDrawer.close()
                    root.exportProfileToFile(root.actionsProfileId, root.actionsProfileName)
                }
            }

            ListItemType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                showSeparator: false
                destructive: true
                text: qsTr("Delete")

                onClicked: {
                    profileActionsDrawer.close()

                    var profileId = root.actionsProfileId
                    var headerText = qsTr("Delete profile \"%1\"?").arg(root.actionsProfileName)
                    var descriptionText = qsTr("The profile and its downloaded geo files will be removed.")
                    var yesButtonText = qsTr("Delete")
                    var noButtonText = qsTr("Cancel")

                    var yesButtonFunction = function() {
                        RoutingController.removeProfile(profileId)
                    }
                    var noButtonFunction = function() {
                    }

                    Dialogs.askQuestion(headerText, descriptionText, yesButtonText, noButtonText, yesButtonFunction, noButtonFunction)
                }
            }
        }
    }

    SheetType {
        id: addProfileDrawer

        expandedHeight: addProfileContent.implicitHeight + 32 + PageController.safeAreaBottomMargin + PageController.imeHeight

        onOpened: {
            newProfileName.textField.text = ""
            newProfileName.errorText = ""
            newProfileName.textField.forceActiveFocus()
        }

        ColumnLayout {
            id: addProfileContent

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 16
            anchors.leftMargin: 16
            anchors.rightMargin: 16

            spacing: 24

            TextFieldType {
                id: newProfileName

                Layout.fillWidth: true

                headerText: qsTr("Profile name")
                textField.maximumLength: 64
                rightButtonClickedOnEnter: true
                clickedFunc: function() {
                    createButton.clicked()
                }
            }

            ButtonType {
                id: createButton

                Layout.fillWidth: true

                text: qsTr("Create")

                onClicked: {
                    var name = newProfileName.textField.text.trim()
                    if (name === "") {
                        newProfileName.errorText = qsTr("The field can't be empty")
                        return
                    }
                    var id = RoutingController.createProfile(name)
                    if (id !== "") {
                        addProfileDrawer.close()
                        root.openProfile(id)
                    }
                }
            }
        }
    }

    SheetType {
        id: renameProfileDrawer

        expandedHeight: renameProfileContent.implicitHeight + 32 + PageController.safeAreaBottomMargin + PageController.imeHeight

        onOpened: {
            renameField.textField.text = root.actionsProfileName
            renameField.errorText = ""
            renameField.textField.forceActiveFocus()
        }

        ColumnLayout {
            id: renameProfileContent

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 16
            anchors.leftMargin: 16
            anchors.rightMargin: 16

            spacing: 24

            TextFieldType {
                id: renameField

                Layout.fillWidth: true

                headerText: qsTr("Profile name")
                textField.maximumLength: 64
                rightButtonClickedOnEnter: true
                clickedFunc: function() {
                    renameButton.clicked()
                }
            }

            ButtonType {
                id: renameButton

                Layout.fillWidth: true

                text: qsTr("Save")

                onClicked: {
                    var name = renameField.textField.text.trim()
                    if (name === "") {
                        renameField.errorText = qsTr("The field can't be empty")
                        return
                    }
                    if (name === root.actionsProfileName || RoutingController.renameProfile(root.actionsProfileId, name)) {
                        renameProfileDrawer.close()
                    }
                }
            }
        }
    }

    SheetType {
        id: importDrawer

        expandedHeight: importContent.implicitHeight + 16 + PageController.safeAreaBottomMargin

        ColumnLayout {
            id: importContent

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 16

            spacing: 0

            SectionHeaderType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: 16

                headerText: qsTr("Import routing profile")
                descriptionText: qsTr("JSON, base64 or a routing link (happ://routing/...), also exported by Happ")
            }

            ListItemType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("From clipboard")
                icon.source: "qrc:/images/controls/copy.svg"

                onClicked: {
                    importDrawer.close()
                    RoutingController.importFromClipboard()
                }
            }

            ListItemType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("From file")
                icon.source: "qrc:/images/controls/folder-search-2.svg"

                onClicked: {
                    importDrawer.close()
                    var fileName = SystemController.getFileName(qsTr("Open routing profile"),
                                                                qsTr("Routing profiles (*.json *.txt);;All files (*.*)"))
                    if (fileName !== "") {
                        PageController.showBusyIndicator(true)
                        RoutingController.importFromFile(fileName)
                        PageController.showBusyIndicator(false)
                    }
                }
            }

            ListItemType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("From link")
                subtitle: qsTr("Download the profile from an http(s) address")
                icon.source: "qrc:/images/controls/globe-2.svg"

                onClicked: {
                    importDrawer.close()
                    importUrlDrawer.open()
                }
            }

            ListItemType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                showSeparator: false
                text: qsTr("Paste text")
                icon.source: "qrc:/images/controls/text-cursor.svg"

                onClicked: {
                    importDrawer.close()
                    importTextDrawer.open()
                }
            }
        }
    }

    SheetType {
        id: importUrlDrawer

        expandedHeight: importUrlContent.implicitHeight + 32 + PageController.safeAreaBottomMargin + PageController.imeHeight

        onOpened: {
            urlField.textField.text = ""
            urlField.errorText = ""
            urlField.textField.forceActiveFocus()
        }

        ColumnLayout {
            id: importUrlContent

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 16
            anchors.leftMargin: 16
            anchors.rightMargin: 16

            spacing: 24

            SectionHeaderType {
                Layout.fillWidth: true

                headerText: qsTr("Import from link")
            }

            TextFieldType {
                id: urlField

                Layout.fillWidth: true

                headerText: qsTr("Link")
                textField.placeholderText: "https://"
                textField.inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                rightButtonClickedOnEnter: true
                clickedFunc: function() {
                    importUrlButton.clicked()
                }
            }

            ButtonType {
                id: importUrlButton

                Layout.fillWidth: true

                text: qsTr("Import")

                onClicked: {
                    var url = urlField.textField.text.trim()
                    if (url === "") {
                        urlField.errorText = qsTr("The field can't be empty")
                        return
                    }
                    importUrlDrawer.close()
                    RoutingController.importFromUrl(url)
                }
            }
        }
    }

    SheetType {
        id: importTextDrawer

        expandedHeight: importTextContent.implicitHeight + 32 + PageController.safeAreaBottomMargin + PageController.imeHeight

        onOpened: {
            importTextArea.textAreaText = ""
            importTextArea.textArea.forceActiveFocus()
        }

        ColumnLayout {
            id: importTextContent

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 16
            anchors.leftMargin: 16
            anchors.rightMargin: 16

            spacing: 24

            SectionHeaderType {
                Layout.fillWidth: true

                headerText: qsTr("Paste text")
                descriptionText: qsTr("A profile in JSON or base64, a routing link, or a plain list of sites and IPs")
            }

            TextAreaType {
                id: importTextArea

                Layout.fillWidth: true
                Layout.preferredHeight: root.height * 0.3

                placeholderText: qsTr("Paste here")
            }

            ButtonType {
                Layout.fillWidth: true

                text: qsTr("Import")

                onClicked: {
                    var text = importTextArea.textAreaText
                    if (text.trim() === "") {
                        PageController.showErrorMessage(qsTr("Nothing to import"))
                        return
                    }
                    importTextDrawer.close()
                    RoutingController.importFromText(text)
                }
            }
        }
    }

    SheetType {
        id: excludedRoutesDrawer

        expandedHeight: Math.min(excludedRoutesContent.implicitHeight + 32 + PageController.safeAreaBottomMargin + PageController.imeHeight,
                                 windowHeight * 0.9)

        onOpened: {
            excludedRoutesArea.textAreaText = RoutingController.excludedRoutesText
            excludedRoutesWarning.text = ""
        }

        ColumnLayout {
            id: excludedRoutesContent

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 16
            anchors.leftMargin: 16
            anchors.rightMargin: 16

            spacing: 16

            SectionHeaderType {
                Layout.fillWidth: true

                headerText: qsTr("Excluded addresses")
                descriptionText: qsTr("IP addresses and subnets (CIDR), one per line. They always bypass the VPN when routing is on.")
            }

            TextAreaType {
                id: excludedRoutesArea

                Layout.fillWidth: true
                Layout.preferredHeight: root.height * 0.35

                placeholderText: "192.168.0.0/16"
            }

            NotificationType {
                id: excludedRoutesWarning

                Layout.fillWidth: true

                visible: text !== ""

                variant: NotificationType.Error
            }

            ButtonType {
                Layout.fillWidth: true

                text: qsTr("Save")

                onClicked: {
                    var warning = RoutingController.setExcludedRoutesText(excludedRoutesArea.textAreaText)
                    excludedRoutesArea.textAreaText = RoutingController.excludedRoutesText
                    if (warning !== "") {
                        excludedRoutesWarning.text = warning
                        return
                    }
                    excludedRoutesDrawer.close()
                    PageController.showNotificationMessage(qsTr("Settings saved"))
                }
            }

            ButtonType {
                Layout.fillWidth: true

                variant: ButtonType.Secondary

                text: qsTr("Cancel")

                onClicked: {
                    excludedRoutesDrawer.close()
                }
            }
        }
    }
}
