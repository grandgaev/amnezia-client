import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes

import PageEnum 1.0
import Style 1.0

import "./"
import "../Controls2"
import "../Controls2/TextTypes"
import "../Config"
import "../Components"

PageType {
    id: root

    property bool isControlsDisabled: false
    property bool isTabBarDisabled: false

    Connections {
        objectName: "pageControllerConnection"

        target: PageController

        function onGoToPageHome() {
            if (PageController.isStartPageVisible()) {
                tabBar.visible = false
                tabBarStackView.goToTabBarPage(PageEnum.PageSetupWizardStart)
            } else {
                tabBar.visible = true
                tabBar.setCurrentIndex(0)
                tabBarStackView.goToTabBarPage(PageEnum.PageHome)
            }
        }

        function onGoToPageSettings() {
            tabBar.setCurrentIndex(2)
            tabBarStackView.goToTabBarPage(PageEnum.PageSettings)
        }

        function onGoToPageViewConfig() {
            var pagePath = PageController.getPagePath(PageEnum.PageSetupWizardViewConfig)
            tabBarStackView.push(pagePath, { "objectName" : pagePath }, StackView.PushTransition)
        }

        function onGoToShareConnectionPage(headerText, configContentHeaderText, configCaption, configExtension, configFileName) {
            var pagePath = PageController.getPagePath(PageEnum.PageShareConnection)
            tabBarStackView.push(pagePath,
                                 { "objectName" : pagePath,
                                     "headerText" : headerText,
                                     "configContentHeaderText" : configContentHeaderText,
                                     "configCaption" : configCaption,
                                     "configExtension" : configExtension,
                                     "configFileName" : configFileName
                                 },
                                 StackView.PushTransition)
        }

        function onDisableControls(disabled) {
            isControlsDisabled = disabled
        }

        function onDisableTabBar(disabled) {
            isTabBarDisabled = disabled
        }

        function onClosePage() {
            if (tabBarStackView.depth <= 1) {
                PageController.hideWindow()
                return
            }
            tabBarStackView.pop()
        }

        function onGoToPage(page, slide) {
            var pagePath = PageController.getPagePath(page)

            if (slide) {
                tabBarStackView.push(pagePath, { "objectName" : pagePath }, StackView.PushTransition)
            } else {
                tabBarStackView.push(pagePath, { "objectName" : pagePath }, StackView.Immediate)
            }
        }

        function onGoToStartPage() {
            while (tabBarStackView.depth > 1) {
                tabBarStackView.pop()
            }
        }

        function onEscapePressed() {
            if (root.isControlsDisabled || root.isTabBarDisabled) {
                return
            }

            var pageName = tabBarStackView.currentItem.objectName
            if ((pageName === PageController.getPagePath(PageEnum.PageShare)) ||
                    (pageName === PageController.getPagePath(PageEnum.PageSettings)) ||
                    (pageName === PageController.getPagePath(PageEnum.PageSetupWizardConfigSource))) {
                PageController.goToPageHome()
            } else {
                PageController.closePage()
            }
        }
    }

    Connections {
        objectName: "connectionControllerConnections"

        target: ConnectionController

        function onNoInstalledContainers() {
            PageController.setTriggeredByConnectButton(true)

            ServersUiController.setProcessedServerId(ServersUiController.defaultServerId)
            PageController.goToPage(PageEnum.PageSetupWizardEasy)
        }
    }

    Connections {
        objectName: "installControllerConnections"

        target: InstallController

        function onInstallationErrorOccurred(error) {
            PageController.showBusyIndicator(false)

            PageController.showErrorMessage(error)

            var needCloseCurrentPage = false
            var currentPageName = tabBarStackView.currentItem.objectName

            if (currentPageName === PageController.getPagePath(PageEnum.PageSetupWizardInstalling)) {
                needCloseCurrentPage = true
            } else if (currentPageName === PageController.getPagePath(PageEnum.PageDeinstalling)) {
                needCloseCurrentPage = true
            }
            if (needCloseCurrentPage) {
                PageController.closePage()
            }
        }

        function onWrongInstallationUser(message) {
            onInstallationErrorOccurred(message)
        }

        function onUpdateContainerFinished(message, closePage) {
            PageController.showNotificationMessage(message)
            if (closePage) {
                PageController.closePage()
            }
        }

        function onCachedProfileCleared(message) {
            PageController.showNotificationMessage(message)
        }

        function onRemoveServerFinished(finishedMessage) {
            if (!ServersUiController.getServersCount()) {
                PageController.goToPageHome()
            } else {
                PageController.goToStartPage()
                PageController.goToPage(PageEnum.PageSettingsServersList)
            }
            PageController.showNotificationMessage(finishedMessage)
        }

        function onRemoveAllContainersFinished(finishedMessage) {
            if (tabBarStackView.currentItem.objectName === PageController.getPagePath(PageEnum.PageDeinstalling)) {
                PageController.closePage()
            }
            PageController.showNotificationMessage(finishedMessage)
        }

        function onRemoveContainerFinished(finishedMessage) {
            if (tabBarStackView.currentItem.objectName === PageController.getPagePath(PageEnum.PageDeinstalling)) {
                PageController.closePage()
            }
            PageController.closePage()
            PageController.showNotificationMessage(finishedMessage)
        }
    }

    Connections {
        objectName: "importControllerConnections"

        target: ImportController

        function onImportErrorOccurred(error, goToPageHome) {
            PageController.showErrorMessage(error)
        }

        function onRestoreAppConfig(data) {
            PageController.showBusyIndicator(true)
            SettingsController.restoreAppConfigFromData(data)
            PageController.showBusyIndicator(false)
        }
    }

    Connections {
        objectName: "settingsControllerConnections"

        target: SettingsController

        function onLoggingDisableByWatcher() {
            PageController.showNotificationMessage(qsTr("Logging was disabled after 14 days, log files were deleted"))
        }

        function onRestoreBackupFinished() {
            PageController.showNotificationMessage(qsTr("Settings restored from backup file"))
            PageController.goToPageHome()
        }

        function onLoggingStateChanged() {
            if (SettingsController.isLoggingEnabled) {
                var message = qsTr("Logging is enabled. Note that logs will be automatically" +
                                   "disabled after 14 days, and all log files will be deleted.")
                PageController.showNotificationMessage(message)
            }
        }
    }

    Connections {
        target: SubscriptionUiController

        function onErrorOccurred(error) {
            PageController.showErrorMessage(error)
        }
    }

    Connections {
        target: SubscriptionUiController

        function onApiConfigRemoved(message) {
            PageController.showNotificationMessage(message)
        }

        function onApiServerRemoved(message) {
            if (!ServersUiController.getServersCount()) {
                PageController.goToPageHome()
            } else {
                PageController.goToStartPage()
                PageController.goToPage(PageEnum.PageSettingsServersList)
            }
            PageController.showNotificationMessage(message)
        }

        function onInstallServerFromApiFinished(message, preferredDefaultIndex) {
            PageController.goToPageHome()
            PageController.showNotificationMessage(message)
        }

        function onBackgroundPurchaseCompleted(message) {
            PageController.showNotificationMessage(message)
        }

        function onChangeApiCountryFinished(message) {
            PageController.goToPageHome()
            PageController.showNotificationMessage(message)
        }

        function onReloadServerFromApiFinished(message) {
            PageController.goToPageHome()
            PageController.showNotificationMessage(message)
        }
    }

    StackViewType {
        id: tabBarStackView
        objectName: "tabBarStackView"

        anchors.top: parent.top
        anchors.right: parent.right
        anchors.left: parent.left
        anchors.bottom: tabBar.top

        enabled: !root.isControlsDisabled

        function goToTabBarPage(page) {
            var pagePath = PageController.getPagePath(page)
            tabBarStackView.clear(StackView.Immediate)
            tabBarStackView.replace(pagePath, { "objectName" : pagePath }, StackView.Immediate)
        }

        Component.onCompleted: {
            var pagePath
            if (PageController.isStartPageVisible()) {
                tabBar.visible = false
                pagePath = PageController.getPagePath(PageEnum.PageSetupWizardStart)
            } else {
                tabBar.visible = true
                pagePath = PageController.getPagePath(PageEnum.PageHome)
                ServersUiController.setProcessedServerId(ServersUiController.defaultServerId)
            }

            tabBarStackView.push(pagePath, { "objectName" : pagePath })
        }

        Keys.onPressed: function(event) {
            switch (event.key) {
            case Qt.Key_Tab:
            case Qt.Key_Down:
            case Qt.Key_Right:
                FocusController.nextKeyTabItem()
                break
            case Qt.Key_Backtab:
            case Qt.Key_Up:
            case Qt.Key_Left:
                FocusController.previousKeyTabItem()
                break
            default:
                PageController.keyPressEvent(event.key)
                event.accepted = true
            }
        }
    }

    TabBar {
        id: tabBar
        objectName: "tabBar"

        anchors.right: parent.right
        anchors.left: parent.left
        anchors.bottom: parent.bottom

        // Also adjust TabBar position when keyboard appears (Android 14+ workaround)
        anchors.bottomMargin: PageController.imeHeight

        topPadding: 8
        bottomPadding: 8 + PageController.safeAreaBottomMargin
        leftPadding: 96
        rightPadding: 96

        height: visible ? homeTabButton.implicitHeight + tabBar.topPadding + tabBar.bottomPadding : 0

        enabled: !root.isControlsDisabled && !root.isTabBarDisabled

        background: Shape {
            objectName: "backgroundShape"

            width: parent.width
            height: parent.height

            ShapePath {
                startX: 0
                startY: 0

                PathLine { x: width; y: 0 }
                PathLine { x: width; y: tabBar.height - 1 }
                PathLine { x: 0; y: tabBar.height - 1 }
                PathLine { x: 0; y: 0 }

                strokeWidth: 1
                strokeColor: AmneziaStyle.color.slateGray
                fillColor: AmneziaStyle.color.onyxBlack
            }
        }

        TabImageButtonType {
            id: homeTabButton
            objectName: "homeTabButton"

            isSelected: tabBar.currentIndex === 0
            image: "qrc:/images/controls/home.svg"
            clickedFunc: function () {
                tabBarStackView.goToTabBarPage(PageEnum.PageHome)
                ServersUiController.setProcessedServerId(ServersUiController.defaultServerId)
                tabBar.currentIndex = 0
            }
        }

        TabImageButtonType {
            id: shareTabButton
            objectName: "shareTabButton"

            Connections {
                target: ServersModel

                function onModelReset() {
                    if (!SettingsController.isOnTv()) {
                        var hasServerWithWriteAccess = ServersUiController.hasServerWithWriteAccess()
                        shareTabButton.visible = hasServerWithWriteAccess
                        shareTabButton.width = hasServerWithWriteAccess ? undefined : 0
                    }
                }
            }

            visible: !SettingsController.isOnTv() && ServersUiController.hasServerWithWriteAccess()
            width: !SettingsController.isOnTv() && ServersUiController.hasServerWithWriteAccess() ? undefined : 0

            isSelected: tabBar.currentIndex === 1
            image: "qrc:/images/controls/share-2.svg"
            clickedFunc: function () {
                tabBarStackView.goToTabBarPage(PageEnum.PageShare)
                tabBar.currentIndex = 1
            }
        }

        TabImageButtonType {
            id: settingsTabButton
            objectName: "settingsTabButton"

            isSelected: tabBar.currentIndex === 2
            image: (ServersUiController.hasServersFromGatewayApi && NewsModel.hasUnread && SettingsController.isNewsNotificationsEnabled()) ? "qrc:/images/controls/settings-news.svg" : "qrc:/images/controls/settings.svg"
            Binding {
                target: settingsTabButton
                property: "defaultColor"
                value: "transparent"
                when: (ServersUiController.hasServersFromGatewayApi && NewsModel.hasUnread)
            }
            clickedFunc: function () {
                tabBarStackView.goToTabBarPage(PageEnum.PageSettings)
                tabBar.currentIndex = 2
            }
        }

        TabImageButtonType {
            id: plusTabButton
            objectName: "plusTabButton"

            isSelected: tabBar.currentIndex === 3
            image: "qrc:/images/controls/plus.svg"
            clickedFunc: function () {
                tabBarStackView.goToTabBarPage(PageEnum.PageSetupWizardConfigSource)
                tabBar.currentIndex = 3
            }
        }
    }

    // ROUTING_TEST_HOOK (temporary, not to be committed)
    property int routingTestStep: 0
    property var routingTestDrawers: []
    function routingTestShot(name) {
        var w = root.Window.window
        w.contentItem.grabToImage(function(result) {
            result.saveToFile("/tmp/claude-0/-home-user-amnezia-client/d4ae3bc0-3596-59a8-a62b-4ede9efa17e4/scratchpad/shot_" + name + ".png")
        })
        console.log("ROUTINGTEST shot", name, "current page:", tabBarStackView.currentItem ? tabBarStackView.currentItem.objectName : "")
    }
    function routingTestFind(item, pred, out) {
        if (!item) return out
        if (pred(item)) out.push(item)
        var kids = item.children || []
        for (var i = 0; i < kids.length; ++i) routingTestFind(kids[i], pred, out)
        if (item.contentItem && item.contentItem !== item && kids.indexOf(item.contentItem) < 0) routingTestFind(item.contentItem, pred, out)
        return out
    }
    function routingTestScrollToEnd() {
        var page = tabBarStackView.currentItem
        var fl = routingTestFind(page, function(i) { return i instanceof Flickable }, [])
        for (var i = 0; i < fl.length; ++i) {
            if (fl[i].visible && fl[i].contentHeight > fl[i].height) {
                fl[i].contentY = fl[i].contentHeight - fl[i].height + (fl[i].originY || 0)
                return
            }
        }
    }
    function routingTestPageDrawers() {
        var page = tabBarStackView.currentItem
        return routingTestFind(page, function(i) { return i.drawerExpandedStateName !== undefined }, [])
    }
    property var routingTestActions: [
        function() {
            var wg = "[Interface]\nPrivateKey = 8H5sZ0R2E0ZtN1aTnC8m0w2fW3yq1H1Zc3bJ9rL0x1s=\nAddress = 10.8.1.2/32\nDNS = 1.1.1.1\n\n[Peer]\nPublicKey = u2Gq2t7m0p8sV9bZcXq3a1Jk4Lz5Ww6Ee7Rr8Tt9Yy0=\nEndpoint = 203.0.113.1:51820\nAllowedIPs = 0.0.0.0/0\n"
            console.log("ROUTINGTEST extract", ImportController.extractConfigFromData(wg))
            ImportController.importConfig()
        },
        function() { PageController.goToPageHome() },
        function() { root.routingTestShot("home") },
        function() { PageController.goToPage(PageEnum.PageSettingsRouting) },
        function() { root.routingTestShot("routing_empty") },
        function() {
            root.routingTestScrollToEnd()
        },
        function() { root.routingTestShot("routing_empty_end") },
        function() {
            console.log("ROUTINGTEST create", RoutingController.createProfile("Test profile"))
            RoutingController.importFromText('{"Name": "Imported", "GlobalProxy": "false", "ProxySites": ["geosite:google", "domain:example.com"], "DirectIp": ["10.0.0.0/8"], "BlockSites": ["geosite:category-ads-all"]}')
        },
        function() { root.routingTestShot("routing_list") },
        function() {
            RoutingController.importFromText('{"Name": "Imported", "ProxySites": ["domain:changed.com"]}')
        },
        function() { root.routingTestShot("import_conflict") },
        function() {
            questionDrawer.noButtonFunction()
        },
        function() {
            console.log("ROUTINGTEST profiles after keep both", RoutingController.profilesCount, RoutingController.profileIds().map(function(id) { return RoutingController.profileName(id) }))
            root.routingTestDrawers = root.routingTestPageDrawers()
            console.log("ROUTINGTEST routing page drawers", root.routingTestDrawers.length)
            root.routingTestDrawers[0].openTriggered()
        },
        function() { root.routingTestShot("drawer0") },
        function() { root.routingTestDrawers[0].closeTriggered() },
        function() { root.routingTestDrawers[1].openTriggered() },
        function() { root.routingTestShot("drawer1") },
        function() { root.routingTestDrawers[1].closeTriggered() },
        function() { root.routingTestDrawers[3].openTriggered() },
        function() { root.routingTestShot("drawer3") },
        function() { root.routingTestDrawers[3].closeTriggered() },
        function() { root.routingTestDrawers[4].openTriggered() },
        function() { root.routingTestShot("drawer4") },
        function() { root.routingTestDrawers[4].closeTriggered() },
        function() { root.routingTestDrawers[5].openTriggered() },
        function() { root.routingTestShot("drawer5") },
        function() { root.routingTestDrawers[5].closeTriggered() },
        function() { root.routingTestDrawers[6].openTriggered() },
        function() { root.routingTestShot("drawer6") },
        function() { root.routingTestDrawers[6].closeTriggered() },
        function() { root.routingTestScrollToEnd() },
        function() { root.routingTestShot("routing_list_end") },
        function() {
            var ids = RoutingController.profileIds()
            RoutingController.selectProfile(ids[1])
            RoutingController.currentProfileId = ids[1]
            PageController.goToPage(PageEnum.PageSettingsRoutingProfile)
        },
        function() { root.routingTestShot("profile") },
        function() {
            RoutingController.setConnectionActive(true)
            RoutingController.setGlobalProxy(true)
            console.log("ROUTINGTEST reconnectRequired after change while connected", RoutingController.reconnectRequired)
            RoutingController.setConnectionActive(false)
            console.log("ROUTINGTEST reconnectRequired after disconnect", RoutingController.reconnectRequired)
            root.routingTestScrollToEnd()
        },
        function() { root.routingTestShot("profile_end") },
        function() {
            RoutingController.currentRuleAction = 0
            PageController.goToPage(PageEnum.PageSettingsRoutingRules)
        },
        function() { root.routingTestShot("rules_proxy") },
        function() {
            root.routingTestScrollToEnd()
        },
        function() { root.routingTestShot("rules_proxy_end") },
        function() {
            PageController.showBusyIndicator(true)
            RoutingController.loadGeoTags(false, RoutingController.rulesText(0))
        },
        function() { root.routingTestShot("geotags") },
        function() {
            console.log("ROUTINGTEST geotags", GeoTagsModel.rowCount(), "selected", GeoTagsModel.selectedCount)
            GeoTagsModel.setSelected("geosite:youtube", true)
            GeoTagsModel.setSelected("geosite:google", false)
            console.log("ROUTINGTEST applyGeoTags:", JSON.stringify(RoutingController.applyGeoTags("# comment\ngeosite:google, domain:example.com\n")))
            root.routingTestDrawers = root.routingTestPageDrawers()
            console.log("ROUTINGTEST rules page drawers", root.routingTestDrawers.length)
            root.routingTestDrawers[1].closeTriggered()
        },
        function() { root.routingTestDrawers[0].openTriggered() },
        function() { root.routingTestShot("examples") },
        function() {
            root.routingTestDrawers[0].closeTriggered()
            PageController.closePage()
        },
        function() {
            RoutingController.currentRuleAction = 1
            PageController.goToPage(PageEnum.PageSettingsRoutingRules)
        },
        function() {
            root.routingTestScrollToEnd()
            console.log("ROUTINGTEST setRulesText warning:", RoutingController.setRulesText(1, "10.0.0.0/8\nfoo bar\nbad:thing\ngeoip:private"))
        },
        function() { root.routingTestShot("rules_direct_end") },
        function() {
            PageController.closePage()
        },
        function() { PageController.goToPage(PageEnum.PageSettingsRoutingDns) },
        function() { root.routingTestShot("dns") },
        function() { root.routingTestScrollToEnd() },
        function() { root.routingTestShot("dns_end") },
        function() { PageController.closePage() },
        function() { PageController.closePage() },
        function() { PageController.goToPage(PageEnum.PageSettingsAppSplitTunneling) },
        function() { root.routingTestShot("apps") },
        function() { PageController.closePage() },
        function() { PageController.closePage() },
        function() { PageController.goToPage(PageEnum.PageSettingsConnection) },
        function() { root.routingTestShot("connection") },
        function() {
            PageController.closePage()
        },
        function() {
            ServersUiController.setProcessedServerId(ServersUiController.defaultServerId)
            PageController.goToPage(PageEnum.PageSettingsServerInfo)
        },
        function() {
            var bars = root.routingTestFind(tabBarStackView.currentItem, function(i) { return i instanceof TabBar }, [])
            console.log("ROUTINGTEST tab bars", bars.length)
            bars[0].currentIndex = 2
        },
        function() { root.routingTestShot("server_management") },
        function() {
            root.routingTestDrawers = root.routingTestFind(tabBarStackView.currentItem, function(i) { return i.currentOptionName !== undefined }, [])
            console.log("ROUTINGTEST server routing drawers", root.routingTestDrawers.length)
            root.routingTestDrawers[0].openTriggered()
        },
        function() { root.routingTestShot("server_routing_drawer") },
        function() {
            console.log("ROUTINGTEST server routing:", RoutingController.serverRoutingDescription(ServersUiController.defaultServerId))
            RoutingController.setServerRoutingValue(ServersUiController.defaultServerId, "off")
            console.log("ROUTINGTEST server routing after off:", RoutingController.serverRoutingDescription(ServersUiController.defaultServerId))
            root.routingTestDrawers[0].closeTriggered()
        },
        function() { root.routingTestShot("server_management_off") },
        function() { RoutingController.setServerRoutingValue(ServersUiController.defaultServerId, "") },
        function() { PageController.goToPageHome() },
        function() {
            var home = tabBarStackView.currentItem
            var drawers = root.routingTestFind(home, function(i) { return i.objectName === "homeSplitTunnelingDrawer" }, [])
            console.log("ROUTINGTEST home drawers", drawers.length)
            drawers[0].openTriggered()
        },
        function() { root.routingTestShot("home_drawer") },
        function() {
            console.log("ROUTINGTEST sanity check of warning output follows")
            var x = routingTestUndefinedThing.foo
        },
        function() { console.log("ROUTINGTEST done") }
    ]
    Timer {
        interval: 1500
        running: true
        repeat: true
        onTriggered: {
            var step = root.routingTestStep++
            if (step >= root.routingTestActions.length) {
                stop()
                return
            }
            console.log("ROUTINGTEST step", step)
            root.routingTestActions[step]()
        }
    }
}
