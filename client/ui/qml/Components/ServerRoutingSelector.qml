import QtQuick
import QtQuick.Layouts

import "../Controls2"

ColumnLayout {
    id: root

    property string serverId
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

    ListItemType {
        Layout.fillWidth: true

        text: qsTr("Routing profile")
        subtitle: root.routingDescription
        trailingIconSource: "qrc:/images/controls/chevron-right.svg"

        onClicked: {
            serverRoutingDrawer.open()
        }
    }

    ServerRoutingDrawer {
        id: serverRoutingDrawer

        serverId: root.serverId
    }
}
