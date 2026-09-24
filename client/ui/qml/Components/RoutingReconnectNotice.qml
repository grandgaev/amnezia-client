import QtQuick
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects

import Style 1.0

import "../Controls2"
import "../Controls2/TextTypes"

// Shown when routing / split tunneling settings were changed during an active
// connection: they are applied by the next connection.
Rectangle {
    id: root

    visible: RoutingController.reconnectRequired && ConnectionController.isConnected

    implicitHeight: content.implicitHeight + content.anchors.topMargin + content.anchors.bottomMargin

    radius: 8
    color: AmneziaStyle.color.translucentRichBrown

    RowLayout {
        id: content

        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 8
        anchors.topMargin: 8
        anchors.bottomMargin: 8

        spacing: 8

        Image {
            Layout.alignment: Qt.AlignVCenter

            sourceSize.width: 16
            sourceSize.height: 16

            source: "qrc:/images/controls/alert-circle.svg"

            layer {
                enabled: true
                effect: ColorOverlay {
                    color: AmneziaStyle.color.goldenApricot
                }
            }
        }

        CaptionTextType {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter

            text: qsTr("Changes will be applied after reconnect")
            color: AmneziaStyle.color.paleGray
        }

        BasicButtonType {
            Layout.alignment: Qt.AlignVCenter

            implicitHeight: 32
            leftPadding: 12
            rightPadding: 12

            defaultColor: AmneziaStyle.color.transparent
            hoveredColor: AmneziaStyle.color.translucentWhite
            pressedColor: AmneziaStyle.color.sheerWhite
            disabledColor: AmneziaStyle.color.mutedGray
            textColor: AmneziaStyle.color.goldenApricot
            borderWidth: 0

            buttonTextLabel.font.pixelSize: 14
            buttonTextLabel.lineHeight: 20

            text: qsTr("Reconnect")

            clickedFunc: function() {
                ConnectionController.reconnectIfActive()
            }
        }
    }
}
