import QtQuick
import QtQuick.Controls

// The rows of the favorites and of a search, page by page (no scrolling): name, the folder it lives in, size, the state of its copy and,
// for a search, where the result came from. A tap on a file shows its details; a tap on a folder opens that folder.
Item {
    id: root
    property alias emptyText: empty.text
    signal openFolder(string remotePath)

    function caption(row) {
        if (row.availability !== "ok") return qsTr("Not available")
        var parts = [row.kind === "folder" ? qsTr("Folder") : row.sizeText, row.dateText]
        if (row.kind === "file") parts.push(EntryWords.stateWord(row.state))
        if (row.kept) parts.push(qsTr("Kept offline"))
        return parts.filter(function (t) { return t.length > 0 }).join("  ·  ")
    }
    function where(row) {
        var folder = row.folderPath === "/" ? qsTr("Files") : row.folderPath
        var src = browse.mode === "search" ? EntryWords.sourceWord(row.source) : ""
        return src.length > 0 ? folder + "  ·  " + src : folder
    }

    Item {
        id: listArea
        anchors { left: parent.left; right: parent.right; top: parent.top; bottom: pager.top }
        clip: true
        onHeightChanged: browse.pageSize = Math.max(3, Math.floor(height / (Style.rowHeight + 30)))

        Column {
            width: parent.width
            Repeater {
                model: browse.rows
                delegate: Item {
                    objectName: "flatRow"
                    width: parent.width
                    height: Style.rowHeight + 30
                    opacity: modelData.availability === "ok" ? 1.0 : 0.55
                    Rectangle { anchors.fill: parent; color: Style.pressed; visible: tap.pressed }
                    Icon {
                        id: kindIcon
                        x: 30
                        anchors.verticalCenter: parent.verticalCenter
                        size: 48
                        source: modelData.kind === "folder" ? "folder" : "list"
                    }
                    Column {
                        anchors { left: kindIcon.right; leftMargin: 24; right: parent.right; rightMargin: 30; verticalCenter: parent.verticalCenter }
                        spacing: 2
                        Text {
                            width: parent.width
                            elide: Text.ElideMiddle
                            font.pixelSize: Style.bodySize
                            font.bold: modelData.kind === "folder"
                            color: Style.ink
                            text: modelData.name
                        }
                        Text {
                            objectName: "flatWhere"
                            width: parent.width
                            elide: Text.ElideMiddle
                            font.pixelSize: Style.captionSize
                            color: Style.ink
                            text: root.where(modelData)
                        }
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            font.pixelSize: Style.captionSize
                            color: Style.ink
                            text: root.caption(modelData)
                        }
                    }
                    RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
                    MouseArea {
                        id: tap
                        anchors.fill: parent
                        onClicked: {
                            if (modelData.kind === "folder" && modelData.availability === "ok") root.openFolder(modelData.remotePath)
                            else browse.open(modelData.remotePath)
                        }
                    }
                }
            }
        }
        Text {
            id: empty
            objectName: "flatEmpty"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 40 }
            wrapMode: Text.Wrap
            visible: !browse.busy && browse.rows.length === 0
            font.pixelSize: Style.bodySize
            color: Style.ink
        }
        Text {
            objectName: "flatBusy"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 40 }
            visible: browse.busy
            font.pixelSize: Style.bodySize
            font.bold: true
            color: Style.ink
            text: qsTr("Loading...")
        }
    }

    ModelPager {
        id: pager
        anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom }
        page: browse.page
        pageCount: browse.pageCount
        visible: browse.pageCount > 1
        height: visible ? Style.touchMin : 0
        onPrevious: browse.previousPage()
        onNext: browse.nextPage()
    }

    Connections {
        target: transfer
        function onJobFinished() { browse.refreshRows() }
        function onUpdateFinished() { browse.refreshRows() }
    }
}
