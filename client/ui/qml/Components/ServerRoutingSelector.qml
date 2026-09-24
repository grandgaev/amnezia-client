import QtQuick
import QtQuick.Layouts

import "../Controls2"

// "Routing profile" item of the server settings: opens ServerRoutingDrawer.
ColumnLayout {
    id: root

    property string serverId
    // Full page item covered by the drawer.
    property Item drawerParent

    property string routingDescription

    function updateDescription() {
        root.routingDescription = RoutingController.serverRoutingDescription(root.serverId)
    }

    spacing: 0

    onServerIdChanged: updateDescription()
    Component.onCompleted: updateDescription()

    Connections {
        target: RoutingController

        function onProfilesChanged() {
            root.updateDescription()
        }

        function onSelectedProfileChanged() {
            root.updateDescription()
        }

        function onRoutingEnabledChanged() {
            root.updateDescription()
        }
    }

    LabelWithButtonType {
        Layout.fillWidth: true

        text: qsTr("Routing profile")
        descriptionText: root.routingDescription
        rightImageSource: "qrc:/images/controls/chevron-right.svg"

        clickedFunction: function() {
            serverRoutingDrawer.openTriggered()
        }
    }

    DividerType {}

    ServerRoutingDrawer {
        id: serverRoutingDrawer

        parent: root.drawerParent
        serverId: root.serverId
    }
}
