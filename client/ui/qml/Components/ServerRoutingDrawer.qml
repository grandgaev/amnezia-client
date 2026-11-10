import QtQuick
import QtQuick.Layouts

import "../Controls2"

SheetType {
    id: root

    property string serverId
    property string currentOptionName

    expandedHeight: windowHeight * 0.7

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

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 16
        anchors.bottomMargin: PageController.safeAreaBottomMargin

        spacing: 0

        BackButtonType {
            backButtonFunction: function() {
                root.close()
            }
        }

        SectionHeaderType {
            Layout.fillWidth: true
            Layout.topMargin: 8
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 32

            headerText: qsTr("Routing profile")
            descriptionText: qsTr("Routing profile used for connections to this server")
        }

        RadioListType {
            Layout.fillWidth: true
            Layout.fillHeight: true

            model: optionsModel
            currentValue: root.currentOptionName

            onActivated: function(index) {
                RoutingController.setServerRoutingValue(root.serverId, optionsModel.get(index).value)
                root.close()
            }
        }
    }
}
