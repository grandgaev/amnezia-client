import QtQuick

DropDownType {
    id: root

    property var stringList: []
    property int currentIndex: -1

    signal activated(int index)

    text: (currentIndex >= 0 && currentIndex < stringList.length) ? stringList[currentIndex] : ""

    drawerHeight: 0.5
    fitContent: true

    ListModel {
        id: stringListModel
    }

    function rebuildModel() {
        stringListModel.clear()
        for (var i = 0; i < root.stringList.length; ++i) {
            stringListModel.append({ "name": root.stringList[i] })
        }
    }

    onStringListChanged: rebuildModel()
    Component.onCompleted: rebuildModel()

    listView: RadioListType {
        model: stringListModel
        currentValue: root.text

        onActivated: function(index) {
            root.close()
            if (index !== root.currentIndex) {
                root.activated(index)
            }
        }
    }
}
