import QtQuick
import QtQuick.Controls
import ".."

// What the app put on the reader: the downloaded copies with their sizes, the space they use, the free space and the size of the saved
// lists. A copy is removed from the reader only; the server is never touched. A copy that was changed on the reader needs a second,
// explicit confirmation that says its changes are lost.
Item {
    id: page
    property string pendingPath
    property string pendingName
    property bool pendingChanged: false

    Component.onCompleted: space.refresh()

    function ask(row) {
        pendingPath = row.remotePath
        pendingName = row.name
        pendingChanged = row.changedLocally
        removeDialog.open()
    }

    TopBar {
        id: topBar
        title: qsTr("Storage")
        leftItems: [ IconButton { objectName: "spaceBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
    }

    Column {
        id: header
        anchors { left: parent.left; right: parent.right; top: topBar.bottom }
        InkRow { objectName: "spaceUsed"; text: qsTr("Used by downloads"); value: space.usedText }
        InkRow { objectName: "spaceFree"; text: qsTr("Free on the reader"); value: space.freeText.length > 0 ? space.freeText : qsTr("unknown") }
        InkRow { objectName: "spaceCache"; text: qsTr("Saved lists and records"); value: space.cacheText }
    }

    Item {
        id: listArea
        anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: pager.top }
        clip: true
        onHeightChanged: space.pageSize = Math.max(3, Math.floor(height / Style.rowHeight))

        Column {
            width: parent.width
            Repeater {
                model: space.items
                delegate: Item {
                    objectName: "spaceRow"
                    width: parent.width
                    height: Style.rowHeight
                    Rectangle { anchors.fill: parent; color: Style.pressed; visible: tap.pressed }
                    Column {
                        anchors { left: parent.left; leftMargin: 30; right: removeIcon.left; rightMargin: 12; verticalCenter: parent.verticalCenter }
                        spacing: 4
                        Text {
                            width: parent.width
                            elide: Text.ElideMiddle
                            font.pixelSize: Style.bodySize
                            color: Style.ink
                            text: modelData.name
                        }
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            font.pixelSize: Style.captionSize
                            color: Style.ink
                            text: [modelData.sizeText, EntryWords.stateWord(modelData.state)].filter(function (t) { return t.length > 0 }).join("  ·  ")
                        }
                    }
                    Icon {
                        id: removeIcon
                        anchors { right: parent.right; rightMargin: 24; verticalCenter: parent.verticalCenter }
                        size: 40
                        source: "close"
                    }
                    RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
                    MouseArea { id: tap; anchors.fill: parent; onClicked: page.ask(modelData) }
                }
            }
        }
        Text {
            objectName: "spaceEmpty"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 40 }
            wrapMode: Text.Wrap
            visible: space.total === 0
            font.pixelSize: Style.bodySize
            color: Style.ink
            text: qsTr("Nothing is downloaded yet.")
        }
    }

    ModelPager {
        id: pager
        anchors { horizontalCenter: parent.horizontalCenter; bottom: panel.top }
        page: space.page
        pageCount: space.pageCount
        visible: space.pageCount > 1
        height: visible ? Style.touchMin : 0
        onPrevious: space.previousPage()
        onNext: space.nextPage()
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton {
                objectName: "spaceClearCache"
                text: qsTr("Clear lists")
                icon: "close"
                enabled: !space.busy
                onClicked: clearDialog.open()
            }
        ]
    }

    Connections {
        target: space
        function onRemoveFinished(ok, errorKey) {
            if (!ok) {
                failDialog.text = (Problems.title(errorKey) + " " + Problems.action(errorKey)).trim()
                failDialog.open()
            }
        }
    }

    ConfirmDialog {
        id: removeDialog
        objectName: "spaceRemoveDialog"
        title: qsTr("Remove from the reader")
        text: page.pendingChanged
            ? qsTr("You changed this copy on the reader. Your changes will be lost. The file stays on your server. Remove the copy?")
            : qsTr("The copy on the reader is deleted. The file stays on your server. Remove the copy?")
        acceptText: qsTr("Remove")
        onAccepted: space.remove(page.pendingPath, page.pendingChanged)
    }

    ConfirmDialog {
        id: clearDialog
        objectName: "spaceClearDialog"
        title: qsTr("Clear the saved lists")
        text: qsTr("The saved folder lists are deleted and will be loaded again when you open a folder. Downloaded files, favorites and offline marks stay.")
        acceptText: qsTr("Clear")
        onAccepted: space.clearCache()
    }

    ConfirmDialog {
        id: failDialog
        objectName: "spaceFailDialog"
        title: qsTr("Not removed")
        showCancel: false
    }
}
