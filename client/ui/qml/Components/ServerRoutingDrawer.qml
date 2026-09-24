import QtQuick
import QtQuick.Layouts

import "../Controls2"

// Routing profile used for connections to one server:
// the active profile (default), no routing, or a specific profile.
DrawerType2 {
    id: root

    property string serverId
    property string currentOptionName

    anchors.fill: parent
    expandedHeight: parent ? parent.height * 0.7 : 0

    ListModel {
        id: optionsModel
    }

    function updateOptions() {
        optionsModel.clear()
        optionsModel.append({ "name": qsTr("Default (active profile)"), "value": "" })
        optionsModel.append({ "name": qsTr("Off (all traffic via VPN)"), "value": "off" })

        var ids = RoutingController.profileIds()
        for (var i = 0; i < ids.length; ++i) {
            optionsModel.append({ "name": RoutingController.profileName(ids[i]), "value": ids[i] })
        }
    }

    function selectedOptionIndex() {
        var value = RoutingController.serverRoutingValue(root.serverId)
        for (var i = 0; i < optionsModel.count; ++i) {
            if (optionsModel.get(i).value === value) {
                return i
            }
        }
        return 0
    }

    onAboutToShow: {
        root.updateOptions()
        root.currentOptionName = optionsModel.get(root.selectedOptionIndex()).name
    }

    expandedStateContent: Item {
        implicitHeight: root.expandedHeight

        ColumnLayout {
            anchors.fill: parent
            anchors.topMargin: 16
            anchors.bottomMargin: PageController.safeAreaBottomMargin

            spacing: 0

            BackButtonType {
                Layout.fillWidth: true

                backButtonImage: "qrc:/images/controls/arrow-left.svg"
                backButtonFunction: function() {
                    root.closeTriggered()
                }
            }

            Header2Type {
                Layout.fillWidth: true
                Layout.topMargin: 8
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: 16

                headerText: qsTr("Routing profile")
                descriptionText: qsTr("Routing profile used for connections to this server")
            }

            ListViewWithRadioButtonType {
                id: optionsListView

                Layout.fillWidth: true
                Layout.fillHeight: true

                rootWidth: root.width

                model: optionsModel
                currentValue: root.currentOptionName

                clickedFunction: function() {
                    RoutingController.setServerRoutingValue(root.serverId, optionsModel.get(selectedIndex).value)
                    root.closeTriggered()
                }
            }
        }
    }
}
