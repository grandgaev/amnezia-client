import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

import Style 1.0

import "../Controls2"
import "../Controls2/TextTypes"

Control {
    id: root

    property bool reconnectPending: false

    visible: RoutingController.reconnectRequired && ConnectionController.isConnected

    padding: 12
    leftPadding: 16

    onVisibleChanged: {
        if (!visible) {
            root.reconnectPending = false
        }
    }

    Timer {
        interval: 1500
        running: root.reconnectPending
        onTriggered: root.reconnectPending = false
    }

    background: Rectangle {
        radius: 16
        color: AmneziaStyle.color.bgSurface
    }

    contentItem: RowLayout {
        spacing: 12

        IconImage {
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: 24
            Layout.preferredHeight: 24

            source: "qrc:/images/controls/alert-triangle.svg"
            sourceSize: Qt.size(24, 24)
            color: AmneziaStyle.color.statusWarning
        }

        SmallTextType {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter

            text: qsTr("Changes will be applied after reconnect")
            color: AmneziaStyle.color.textPrimary
        }

        ButtonType {
            Layout.alignment: Qt.AlignVCenter

            variant: ButtonType.Text
            size: ButtonType.Small

            enabled: !root.reconnectPending && !ConnectionController.isConnectionInProgress

            text: qsTr("Reconnect")

            onClicked: {
                root.reconnectPending = true
                ConnectionController.reconnectIfActive()
            }
        }
    }
}
