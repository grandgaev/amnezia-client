import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import SortFilterProxyModel 0.2

import PageEnum 1.0
import ContainerProps 1.0
import Style 1.0

import "./"
import "../Controls2"
import "../Controls2/TextTypes"
import "../Config"
import "../Components"

PageType {
    id: root

    property bool isUnsupportedContainer: ContainerProps.isUnsupportedContainer(ServersUiController.processedContainerIndex)
    property bool isClearCacheVisible: !isUnsupportedContainer && ServersUiController.isProcessedServerHasWriteAccess() && !ContainersModel.isServiceContainer(ServersUiController.processedContainerIndex)
    property bool isOutdatedAwgContainer: ServersUiController.isProcessedContainerOutdatedAwg()
    property bool isContainerUpgradeAvailable: ServersUiController.isProcessedContainerUpgradeAvailable()

    function runContainerUpgrade(mode) {
        InstallController.upgradeContainer(ServersUiController.processedServerId, ServersUiController.processedContainerIndex, mode)
        PageController.goToPage(PageEnum.PageContainerUpgrading)
    }

    function startContainerUpgrade() {
        var headerText = qsTr("Upgrade %1").arg(ContainersModel.getProcessedContainerName())

        if (root.isOutdatedAwgContainer) {
            var descriptionText = qsTr("The old container keeps serving your users until the new one passes verification, and everything rolls back automatically if it doesn't.")
            var protocolUpgradeText = qsTr("Upgrade to AmneziaWG 3.1 — users are kept, send them updated configs")
            var softwareOnlyText = qsTr("Update server software only — all configs keep working")

            showQuestionDrawer(headerText, descriptionText, protocolUpgradeText, softwareOnlyText,
                              function() { root.runContainerUpgrade(1 /* ContainerUpgradeMode::UpgradeProtocol */) },
                              function() { root.runContainerUpgrade(0 /* ContainerUpgradeMode::RefreshSoftware */) })
        } else {
            var refreshDescriptionText = qsTr("This rebuilds the container from a fresh base image. Update server software only — all configs keep working.")
            var continueText = qsTr("Continue")
            var cancelText = qsTr("Cancel")

            showQuestionDrawer(headerText, refreshDescriptionText, continueText, cancelText,
                              function() { root.runContainerUpgrade(0 /* ContainerUpgradeMode::RefreshSoftware */) },
                              function() {})
        }
    }

    BackButtonType {
        id: backButton

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: 20 + PageController.safeAreaTopMargin
        
        onFocusChanged: {
            if (this.activeFocus) {
                listView.positionViewAtBeginning()
            }
        }
    }

    ListViewType {
        id: listView

        anchors.top: backButton.bottom
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.left: parent.left

        header: ColumnLayout {
            width: listView.width

            BaseHeaderType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: root.isOutdatedAwgContainer ? 16 : 32

                headerText: ContainersModel.getProcessedContainerName() + qsTr(" settings")
                descriptionText: root.isUnsupportedContainer ? qsTr("This protocol is no longer supported.") : ""
            }

            WarningType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: 16

                visible: root.isOutdatedAwgContainer

                iconPath: "qrc:/images/controls/alert-circle.svg"
                imageColor: AmneziaStyle.color.goldenApricot
                textColor: AmneziaStyle.color.goldenApricot
                textString: qsTr("AmneziaWG 2.0 is outdated and does not include the latest security improvements, but it will continue to work. Moving to AmneziaWG 3.1 by deploying a new container on the server is recommended for stronger protocol security")
            }
        }

        model: root.isUnsupportedContainer ? null : ProtocolsModel

        delegate: ColumnLayout {
            id: delegateContent

            width: listView.width

            property bool isClientSettingsVisible: isWireGuard || isAwg
            property bool isServerSettingsVisible: ServersUiController.isProcessedServerHasWriteAccess()

            LabelWithButtonType {
                id: clientSettings

                Layout.fillWidth: true

                text: protocolName + qsTr(" connection settings")
                rightImageSource: "qrc:/images/controls/chevron-right.svg"
                visible: delegateContent.isClientSettingsVisible

                clickedFunction: function() {
                    if (isClientProtocolExists) {
                        InstallController.openClientSettings(ServersUiController.processedServerId, ServersUiController.processedContainerIndex, protocolIndex)
                        PageController.goToPage(clientProtocolPage);
                    } else {
                        PageController.showNotificationMessage(qsTr("Click the \"connect\" button to create a connection configuration"))
                    }
                }

                MouseArea {
                    anchors.fill: clientSettings
                    cursorShape: Qt.PointingHandCursor
                    enabled: false
                }
            }

            DividerType {
                visible: delegateContent.isClientSettingsVisible
            }

            LabelWithButtonType {
                id: serverSettings

                Layout.fillWidth: true

                text: protocolName + qsTr(" server settings")
                rightImageSource: "qrc:/images/controls/chevron-right.svg"
                visible: delegateContent.isServerSettingsVisible

                clickedFunction: function() {
                    InstallController.openServerSettings(ServersUiController.processedServerId, ServersUiController.processedContainerIndex, protocolIndex)
                    PageController.goToPage(serverProtocolPage);
                }

                MouseArea {
                    anchors.fill: serverSettings
                    cursorShape: Qt.PointingHandCursor
                    enabled: false
                }
            }

            DividerType {
                visible: delegateContent.isServerSettingsVisible
            }
        }

        footer: ColumnLayout {

            width: listView.width

            LabelWithButtonType {
                id: clearCacheButton

                Layout.fillWidth: true

                visible: root.isClearCacheVisible

                text: qsTr("Clear profile")

                clickedFunction: function() {
                    var headerText = qsTr("Clear %1 profile?").arg(ContainersModel.getProcessedContainerName())
                    var descriptionText = qsTr("The connection configuration will be deleted for this device only")
                    var yesButtonText = qsTr("Continue")
                    var noButtonText = qsTr("Cancel")

                    var yesButtonFunction = function() {
                        // An active connection that uses this profile reconnects with a new one.
                        PageController.showBusyIndicator(true)
                        InstallController.clearCachedProfile(ServersUiController.processedServerId, ServersUiController.processedContainerIndex)
                        PageController.showBusyIndicator(false)
                    }

                    var noButtonFunction = function() {
                    }

                    showQuestionDrawer(headerText, descriptionText, yesButtonText, noButtonText, yesButtonFunction, noButtonFunction)
                }

                MouseArea {
                    anchors.fill: clearCacheButton
                    cursorShape: Qt.PointingHandCursor
                    enabled: false
                }
            }

            DividerType {
                visible: root.isClearCacheVisible
            }

            LabelWithButtonType {
                id: upgradeContainerButton

                Layout.fillWidth: true

                visible: root.isContainerUpgradeAvailable

                text: qsTr("Upgrade")

                clickedFunction: function() {
                    root.startContainerUpgrade()
                }

                MouseArea {
                    anchors.fill: upgradeContainerButton
                    cursorShape: Qt.PointingHandCursor
                    enabled: false
                }
            }

            DividerType {
                visible: root.isContainerUpgradeAvailable
            }

            LabelWithButtonType {
                id: removeButton

                Layout.fillWidth: true

                visible: ServersUiController.isProcessedServerHasWriteAccess()

                text: qsTr("Remove ")
                textColor: AmneziaStyle.color.vibrantRed

                clickedFunction: function() {
                    var headerText = qsTr("Remove %1 from server?").arg(ContainersModel.getProcessedContainerName())
                    var descriptionText = qsTr("All users with whom you shared a connection will no longer be able to connect to it.")
                    var yesButtonText = qsTr("Continue")
                    var noButtonText = qsTr("Cancel")

                    var yesButtonFunction = function() {
                        // An active connection over this protocol switches to another protocol of the server.
                        PageController.goToPage(PageEnum.PageDeinstalling)
                        InstallController.removeContainer(ServersUiController.processedServerId, ServersUiController.processedContainerIndex)
                    }
                    var noButtonFunction = function() {

                    }

                    showQuestionDrawer(headerText, descriptionText, yesButtonText, noButtonText, yesButtonFunction, noButtonFunction)
                }

                MouseArea {
                    anchors.fill: removeButton
                    cursorShape: Qt.PointingHandCursor
                    enabled: false
                }
            }

            DividerType {
                visible: ServersUiController.isProcessedServerHasWriteAccess()
            }
        }
    }
}
