import QtQuick
import QtQuick.Controls
import ".."

// Choose a folder of the server for Move or Copy (specs/002-manage-server-files US5): folders only, page by page, from the stored listings (a folder
// that was never fetched is fetched when it is opened and the reader is online). "Choose this folder" hands the path back; nothing is sent from here
// except that one listing. A folder that is still queued is listed too. `blocked` are paths that cannot be chosen (the folders being moved).
Item {
    id: page
    property string startPath: "/"
    property var blocked: []
    property string verb: qsTr("Move")
    signal chosen(string path)

    function pathLine() {
        var segs = chooser.pathSegments, parts = []
        for (var i = 0; i < segs.length; ++i) parts.push(i === 0 ? qsTr("Files") : segs[i].label)
        return parts.join("  /  ")
    }
    function isBlocked(p) {
        for (var i = 0; i < blocked.length; ++i) if (p === blocked[i] || p.indexOf(blocked[i] + "/") === 0) return true
        return false
    }

    Component.onCompleted: chooser.open(startPath)

    TopBar {
        id: topBar
        title: qsTr("Choose a folder")
        leftItems: [ IconButton { objectName: "chooserCancel"; text: qsTr("Cancel"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
        rightItems: [ IconButton { objectName: "chooserUp"; text: qsTr("Up"); icon: "chevron-up"; enabled: chooser.canGoUp && !chooser.busy; onClicked: chooser.up() } ]
    }

    Column {
        id: header
        anchors { left: parent.left; right: parent.right; top: topBar.bottom }
        Text {
            objectName: "chooserPath"
            x: 30
            width: parent.width - 60
            topPadding: 12
            bottomPadding: 10
            elide: Text.ElideMiddle
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: page.pathLine()
        }
        Text {
            objectName: "chooserOffline"
            x: 30
            width: parent.width - 60
            bottomPadding: 10
            wrapMode: Text.Wrap
            visible: chooser.offline
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: qsTr("Offline: only folders the reader has stored are shown.")
        }
        Rectangle { width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
    }

    Item {
        id: listArea
        anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: pager.top }
        clip: true
        onHeightChanged: chooser.pageSize = Math.max(3, Math.floor(height / Style.rowHeight))

        Column {
            width: parent.width
            Repeater {
                model: chooser.rows
                delegate: Item {
                    objectName: "chooserRow"
                    width: parent.width
                    height: Style.rowHeight
                    opacity: page.isBlocked(modelData.remotePath) ? 0.4 : 1.0
                    Rectangle { anchors.fill: parent; color: Style.pressed; visible: tap.pressed }
                    Icon { id: icon; x: 30; anchors.verticalCenter: parent.verticalCenter; size: 48; source: "folder" }
                    Column {
                        anchors { left: icon.right; leftMargin: 24; right: chev.left; rightMargin: 12; verticalCenter: parent.verticalCenter }
                        Text { width: parent.width; elide: Text.ElideMiddle; font.pixelSize: Style.bodySize; font.bold: true; color: Style.ink; text: modelData.name }
                        Text { width: parent.width; visible: modelData.pending; font.pixelSize: Style.captionSize; color: Style.ink; text: qsTr("Not synced") }
                    }
                    Icon { id: chev; anchors { right: parent.right; rightMargin: 24; verticalCenter: parent.verticalCenter }
                        size: 40
                        source: "chevron-right"
                    }
                    RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
                    MouseArea { id: tap; anchors.fill: parent; enabled: !page.isBlocked(modelData.remotePath); onClicked: chooser.open(modelData.remotePath) }
                }
            }
        }
        Text {
            objectName: "chooserEmpty"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 60 }
            wrapMode: Text.Wrap
            visible: !chooser.busy && chooser.rows.length === 0
            font.pixelSize: Style.bodySize
            color: Style.ink
            text: !chooser.hasListing && chooser.offline ? qsTr("This folder is not stored on the reader. Connect to the network and open it again.")
                : chooser.errorKey.length > 0 ? Problems.title(chooser.errorKey) + " " + Problems.action(chooser.errorKey)
                : qsTr("There are no folders in this one.")
        }
        Text {
            objectName: "chooserBusy"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 60 }
            visible: chooser.busy
            font.pixelSize: Style.bodySize
            font.bold: true
            color: Style.ink
            text: qsTr("Loading...")
        }
    }

    ModelPager {
        id: pager
        anchors { horizontalCenter: parent.horizontalCenter; bottom: panel.top }
        page: chooser.page
        pageCount: chooser.pageCount
        visible: chooser.pageCount > 1
        height: visible ? Style.touchMin : 0
        onPrevious: chooser.previousPage()
        onNext: chooser.nextPage()
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton {
                objectName: "chooserChoose"
                text: qsTr("Choose this folder")
                icon: "check"
                enabled: !chooser.busy && !page.isBlocked(chooser.path) && (chooser.hasListing || chooser.path === "/")
                onClicked: { page.chosen(chooser.path); if (page.StackView.view) page.StackView.view.pop() }
            },
            ToolButton {
                objectName: "chooserNewFolder"
                text: qsTr("New folder")
                icon: "plus-circle"
                enabled: !chooser.busy && (chooser.hasListing || chooser.path === "/")
                onClicked: newFolder.open("")
            }
        ]
    }

    TextEntryDialog {
        id: newFolder
        objectName: "chooserNewFolderEntry"
        title: qsTr("New folder")
        acceptText: qsTr("Add")
        onAccepted: function (text) {
            var r = queue.newFolder(chooser.path, text)
            if (r.errorKey.length > 0) {
                errorText = r.exists ? qsTr("A file or folder with this name is already there.") : Problems.title(r.errorKey) + " " + Problems.action(r.errorKey)
                return
            }
            close()
            chooser.refresh()
        }
    }
}
