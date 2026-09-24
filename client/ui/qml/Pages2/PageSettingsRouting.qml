import QtQuick
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

    // Profile the actions drawer / rename drawer work with.
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

    BackButtonType {
        id: backButton

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: 20 + PageController.safeAreaTopMargin

        onActiveFocusChanged: {
            if (backButton.enabled && backButton.activeFocus) {
                listView.positionViewAtBeginning()
            }
        }
    }

    ListViewType {
        id: listView

        anchors.top: backButton.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right

        header: ColumnLayout {
            width: listView.width
            spacing: 0

            HeaderTypeWithSwitcher {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                headerText: qsTr("Routing")
                descriptionText: qsTr("Split tunneling: sites, IPs, geosite, geoip")

                showSwitcher: true
                switcher {
                    checked: RoutingController.routingEnabled
                }
                switcherFunction: function(checked) {
                    if (checked !== RoutingController.routingEnabled) {
                        RoutingController.setRoutingEnabled(checked)
                    }
                }
            }

            WarningType {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                visible: textString !== ""

                textString: RoutingController.protocolSupportText
                iconPath: "qrc:/images/controls/alert-circle.svg"
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

                text: qsTr("Profiles")
            }

            ParagraphTextType {
                Layout.fillWidth: true
                Layout.topMargin: 8
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                color: AmneziaStyle.color.mutedGray
                text: RoutingController.profilesCount > 0
                      ? qsTr("The selected profile is used for connections when routing is on")
                      : qsTr("No profiles yet. Add a new profile or import one: JSON, base64 or a routing link")
            }
        }

        model: RoutingProfilesModel

        delegate: ColumnLayout {
            id: profileDelegate

            required property string profileId
            required property string name
            required property string description
            required property bool isSelected
            required property bool hasGeoError
            required property bool isDownloading

            width: listView.width
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 8

                spacing: 0

                VerticalRadioButton {
                    id: profileRadioButton

                    Layout.fillWidth: true

                    text: profileDelegate.name
                    descriptionText: profileDelegate.isDownloading
                                     ? profileDelegate.description + "\n" + qsTr("Downloading geo files…")
                                     : profileDelegate.description
                    descriptionColor: profileDelegate.hasGeoError ? AmneziaStyle.color.vibrantRed : AmneziaStyle.color.mutedGray
                    textMaximumLineCount: 2

                    checked: profileDelegate.isSelected

                    onClicked: {
                        RoutingController.selectProfile(profileDelegate.profileId)
                    }

                    Keys.onEnterPressed: this.clicked()
                    Keys.onReturnPressed: this.clicked()
                }

                ImageButtonType {
                    implicitWidth: 48
                    implicitHeight: 56

                    image: "qrc:/images/controls/settings.svg"
                    imageColor: AmneziaStyle.color.paleGray

                    onClicked: {
                        root.openProfile(profileDelegate.profileId)
                    }
                }

                ImageButtonType {
                    implicitWidth: 40
                    implicitHeight: 56

                    image: "qrc:/images/controls/more-vertical.svg"
                    imageColor: AmneziaStyle.color.paleGray

                    onClicked: {
                        root.actionsProfileId = profileDelegate.profileId
                        root.actionsProfileName = profileDelegate.name
                        profileActionsDrawer.openTriggered()
                    }
                }
            }

            DividerType {}
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

                BasicButtonType {
                    Layout.fillWidth: true

                    text: qsTr("Add profile")
                    leftImageSource: "qrc:/images/controls/plus.svg"

                    clickedFunc: function() {
                        addProfileDrawer.openTriggered()
                    }
                }

                BasicButtonType {
                    Layout.fillWidth: true

                    defaultColor: AmneziaStyle.color.transparent
                    hoveredColor: AmneziaStyle.color.translucentWhite
                    pressedColor: AmneziaStyle.color.sheerWhite
                    disabledColor: AmneziaStyle.color.mutedGray
                    textColor: AmneziaStyle.color.paleGray
                    borderWidth: 1

                    text: qsTr("Import")
                    leftImageSource: "qrc:/images/controls/download.svg"

                    clickedFunc: function() {
                        importDrawer.openTriggered()
                    }
                }
            }

            Header2TextType {
                Layout.fillWidth: true
                Layout.topMargin: 32
                Layout.bottomMargin: 8
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: qsTr("Settings")
            }

            LabelWithButtonType {
                Layout.fillWidth: true

                text: qsTr("Excluded addresses")
                descriptionText: qsTr("Addresses and subnets that always bypass the VPN when routing is on, e.g. local networks")
                rightImageSource: "qrc:/images/controls/chevron-right.svg"

                clickedFunction: function() {
                    excludedRoutesDrawer.openTriggered()
                }
            }

            DividerType {}

            StringListDropDownType {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.bottomMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                drawerParent: root

                descriptionText: qsTr("Browser identity for geo files download")
                headerText: qsTr("User-Agent")

                stringList: RoutingController.geoUserAgents
                currentIndex: RoutingController.geoUserAgentIndex

                onActivated: function(index) {
                    RoutingController.setGeoUserAgentIndex(index)
                }
            }

            DividerType {
                visible: appSplitTunnelingButton.visible
            }

            LabelWithButtonType {
                id: appSplitTunnelingButton

                Layout.fillWidth: true

                visible: RoutingController.isAppRoutingSupported

                text: qsTr("App-based split tunneling")
                descriptionText: AppSplitTunnelingController.isSplitTunnelingEnabled ? qsTr("Enabled") : qsTr("Disabled")
                rightImageSource: "qrc:/images/controls/chevron-right.svg"

                clickedFunction: function() {
                    PageController.goToPage(PageEnum.PageSettingsAppSplitTunneling)
                }
            }

            DividerType {
                visible: appSplitTunnelingButton.visible
            }

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 16 + PageController.safeAreaBottomMargin
            }
        }
    }

    DrawerType2 {
        id: profileActionsDrawer

        anchors.fill: parent

        expandedStateContent: ColumnLayout {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right

            spacing: 0

            onImplicitHeightChanged: {
                profileActionsDrawer.expandedHeight = implicitHeight + 32 + PageController.safeAreaBottomMargin
            }

            Header2Type {
                Layout.fillWidth: true
                Layout.margins: 16

                headerText: root.actionsProfileName
            }

            LabelWithButtonType {
                Layout.fillWidth: true

                text: qsTr("Edit")
                rightImageSource: "qrc:/images/controls/chevron-right.svg"

                clickedFunction: function() {
                    profileActionsDrawer.closeTriggered()
                    root.openProfile(root.actionsProfileId)
                }
            }

            DividerType {}

            LabelWithButtonType {
                Layout.fillWidth: true

                text: qsTr("Rename")

                clickedFunction: function() {
                    profileActionsDrawer.closeTriggered()
                    renameProfileDrawer.openTriggered()
                }
            }

            DividerType {}

            LabelWithButtonType {
                Layout.fillWidth: true

                text: qsTr("Duplicate")

                clickedFunction: function() {
                    profileActionsDrawer.closeTriggered()
                    var id = RoutingController.duplicateProfile(root.actionsProfileId)
                    if (id !== "") {
                        PageController.showNotificationMessage(qsTr("Profile \"%1\" is created").arg(RoutingController.profileName(id)))
                    }
                }
            }

            DividerType {}

            LabelWithButtonType {
                Layout.fillWidth: true

                text: qsTr("Copy to clipboard")

                clickedFunction: function() {
                    profileActionsDrawer.closeTriggered()
                    RoutingController.exportToClipboard(root.actionsProfileId)
                }
            }

            DividerType {}

            LabelWithButtonType {
                Layout.fillWidth: true

                text: qsTr("Save to file")

                clickedFunction: function() {
                    profileActionsDrawer.closeTriggered()
                    root.exportProfileToFile(root.actionsProfileId, root.actionsProfileName)
                }
            }

            DividerType {}

            LabelWithButtonType {
                Layout.fillWidth: true

                text: qsTr("Delete")
                textColor: AmneziaStyle.color.vibrantRed

                clickedFunction: function() {
                    profileActionsDrawer.closeTriggered()

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

                    showQuestionDrawer(headerText, descriptionText, yesButtonText, noButtonText, yesButtonFunction, noButtonFunction)
                }
            }
        }
    }

    DrawerType2 {
        id: addProfileDrawer

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
                target: addProfileDrawer

                function onOpened() {
                    newProfileName.textField.text = ""
                    newProfileName.errorText = ""
                    newProfileName.textField.forceActiveFocus()
                }
            }

            TextFieldWithHeaderType {
                id: newProfileName

                Layout.fillWidth: true

                headerText: qsTr("Profile name")
                textField.maximumLength: 64
                rightButtonClickedOnEnter: true
                clickedFunc: function() {
                    createButton.clicked()
                }
            }

            BasicButtonType {
                id: createButton

                Layout.fillWidth: true

                text: qsTr("Create")

                clickedFunc: function() {
                    var name = newProfileName.textField.text.trim()
                    if (name === "") {
                        newProfileName.errorText = qsTr("The field can't be empty")
                        return
                    }
                    var id = RoutingController.createProfile(name)
                    if (id !== "") {
                        addProfileDrawer.closeTriggered()
                        root.openProfile(id)
                    }
                }
            }
        }
    }

    DrawerType2 {
        id: renameProfileDrawer

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
                target: renameProfileDrawer

                function onOpened() {
                    renameField.textField.text = root.actionsProfileName
                    renameField.errorText = ""
                    renameField.textField.forceActiveFocus()
                }
            }

            TextFieldWithHeaderType {
                id: renameField

                Layout.fillWidth: true

                headerText: qsTr("Profile name")
                textField.maximumLength: 64
                rightButtonClickedOnEnter: true
                clickedFunc: function() {
                    renameButton.clicked()
                }
            }

            BasicButtonType {
                id: renameButton

                Layout.fillWidth: true

                text: qsTr("Save")

                clickedFunc: function() {
                    var name = renameField.textField.text.trim()
                    if (name === "") {
                        renameField.errorText = qsTr("The field can't be empty")
                        return
                    }
                    if (name === root.actionsProfileName || RoutingController.renameProfile(root.actionsProfileId, name)) {
                        renameProfileDrawer.closeTriggered()
                    }
                }
            }
        }
    }

    DrawerType2 {
        id: importDrawer

        anchors.fill: parent

        expandedStateContent: ColumnLayout {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right

            spacing: 0

            onImplicitHeightChanged: {
                importDrawer.expandedHeight = implicitHeight + 32 + PageController.safeAreaBottomMargin
            }

            Header2Type {
                Layout.fillWidth: true
                Layout.margins: 16

                headerText: qsTr("Import routing profile")
                descriptionText: qsTr("JSON, base64 or a routing link (happ://routing/...), also exported by Happ")
            }

            LabelWithButtonType {
                Layout.fillWidth: true

                text: qsTr("From clipboard")
                leftImageSource: "qrc:/images/controls/copy.svg"
                isSmallLeftImage: true

                clickedFunction: function() {
                    importDrawer.closeTriggered()
                    RoutingController.importFromClipboard()
                }
            }

            DividerType {}

            LabelWithButtonType {
                Layout.fillWidth: true

                text: qsTr("From file")
                leftImageSource: "qrc:/images/controls/folder-open.svg"
                isSmallLeftImage: true

                clickedFunction: function() {
                    importDrawer.closeTriggered()
                    var fileName = SystemController.getFileName(qsTr("Open routing profile"),
                                                                qsTr("Routing profiles (*.json *.txt);;All files (*.*)"))
                    if (fileName !== "") {
                        PageController.showBusyIndicator(true)
                        RoutingController.importFromFile(fileName)
                        PageController.showBusyIndicator(false)
                    }
                }
            }

            DividerType {}

            LabelWithButtonType {
                Layout.fillWidth: true

                text: qsTr("From link")
                descriptionText: qsTr("Download the profile from an http(s) address")
                leftImageSource: "qrc:/images/controls/globe-2.svg"
                isSmallLeftImage: true

                clickedFunction: function() {
                    importDrawer.closeTriggered()
                    importUrlDrawer.openTriggered()
                }
            }

            DividerType {}

            LabelWithButtonType {
                Layout.fillWidth: true

                text: qsTr("Paste text")
                leftImageSource: "qrc:/images/controls/text-cursor.svg"
                isSmallLeftImage: true

                clickedFunction: function() {
                    importDrawer.closeTriggered()
                    importTextDrawer.openTriggered()
                }
            }
        }
    }

    DrawerType2 {
        id: importUrlDrawer

        anchors.fill: parent
        expandedHeight: root.height * 0.4 + PageController.safeAreaBottomMargin + PageController.imeHeight

        expandedStateContent: ColumnLayout {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 16
            anchors.leftMargin: 16
            anchors.rightMargin: 16

            spacing: 16

            Connections {
                target: importUrlDrawer

                function onOpened() {
                    urlField.textField.text = ""
                    urlField.errorText = ""
                    urlField.textField.forceActiveFocus()
                }
            }

            Header2Type {
                Layout.fillWidth: true

                headerText: qsTr("Import from link")
            }

            TextFieldWithHeaderType {
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

            BasicButtonType {
                id: importUrlButton

                Layout.fillWidth: true

                text: qsTr("Import")

                clickedFunc: function() {
                    var url = urlField.textField.text.trim()
                    if (url === "") {
                        urlField.errorText = qsTr("The field can't be empty")
                        return
                    }
                    importUrlDrawer.closeTriggered()
                    RoutingController.importFromUrl(url)
                }
            }
        }
    }

    DrawerType2 {
        id: importTextDrawer

        anchors.fill: parent
        expandedHeight: root.height * 0.7 + PageController.safeAreaBottomMargin

        expandedStateContent: ColumnLayout {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 16
            anchors.leftMargin: 16
            anchors.rightMargin: 16

            spacing: 16

            Connections {
                target: importTextDrawer

                function onOpened() {
                    importTextArea.textAreaText = ""
                    importTextArea.textArea.forceActiveFocus()
                }
            }

            Header2Type {
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

            BasicButtonType {
                Layout.fillWidth: true

                text: qsTr("Import")

                clickedFunc: function() {
                    var text = importTextArea.textAreaText
                    if (text.trim() === "") {
                        PageController.showErrorMessage(qsTr("Nothing to import"))
                        return
                    }
                    importTextDrawer.closeTriggered()
                    RoutingController.importFromText(text)
                }
            }
        }
    }

    DrawerType2 {
        id: excludedRoutesDrawer

        anchors.fill: parent
        expandedHeight: root.height * 0.9

        expandedStateContent: ColumnLayout {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 16
            anchors.leftMargin: 16
            anchors.rightMargin: 16

            spacing: 16

            Connections {
                target: excludedRoutesDrawer

                function onOpened() {
                    excludedRoutesArea.textAreaText = RoutingController.excludedRoutesText
                    excludedRoutesWarning.textString = ""
                }
            }

            Header2Type {
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

            WarningType {
                id: excludedRoutesWarning

                Layout.fillWidth: true

                visible: textString !== ""
                textColor: AmneziaStyle.color.vibrantRed
                imageColor: AmneziaStyle.color.vibrantRed
                iconPath: "qrc:/images/controls/alert-circle.svg"
            }

            BasicButtonType {
                Layout.fillWidth: true

                text: qsTr("Save")

                clickedFunc: function() {
                    var warning = RoutingController.setExcludedRoutesText(excludedRoutesArea.textAreaText)
                    excludedRoutesArea.textAreaText = RoutingController.excludedRoutesText
                    if (warning !== "") {
                        excludedRoutesWarning.textString = warning
                        return
                    }
                    excludedRoutesDrawer.closeTriggered()
                    PageController.showNotificationMessage(qsTr("Settings saved"))
                }
            }

            BasicButtonType {
                Layout.fillWidth: true

                defaultColor: AmneziaStyle.color.transparent
                hoveredColor: AmneziaStyle.color.translucentWhite
                pressedColor: AmneziaStyle.color.sheerWhite
                disabledColor: AmneziaStyle.color.mutedGray
                textColor: AmneziaStyle.color.paleGray
                borderWidth: 1

                text: qsTr("Cancel")

                clickedFunc: function() {
                    excludedRoutesDrawer.closeTriggered()
                }
            }
        }
    }
}
