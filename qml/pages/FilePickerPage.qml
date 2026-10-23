import QtQuick
import QtQuick.Controls
import ".."

// The file browser of the Upload action (specs/002-manage-server-files US1): the reader's own storage as a list or as tiles (the choice is
// remembered), the path, folders and files as big rows, whole rows per page, no kinetic scrolling. A tap on a file marks it, a tap on a
// folder opens it, "Add this folder" marks the folder one is in with everything inside. "Upload" puts the marked files and folders into the
// queue for the server folder `targetFolder`; nothing is sent. Adapted from the attach browser of notes-pocketbook (MIT, same author).
Item {
    id: page
    property string targetFolder: "/"
    property string path: picker.rootDirectory
    property var rows: picker.listDirectory(path)
    property string message: ""
    // "list" or "tile": the reader's last choice (kept by the bridge); a tap on the toggle changes and stores it
    property string view: !folderMode && picker.view === "tile" ? "tile" : "list"
    property var pickData: null          // the counted folder pick that waits for the user's confirmation
    property var pickedFiles: []
    property var leftOut: []             // [{name, key}] of picks that could not be queued
    property int queuedCount: 0
    signal queued(int count, var asks)   // the questions are names that are taken: the caller asks them (nothing is queued for them yet)
    property var pendingAsks: []
    // Folder mode (specs/003-folder-sync): choose ONE folder of the reader for a pair. Folders only, nothing is selected, nothing is queued;
    // "Choose this folder" hands the path back. "New folder" makes one below the folder one is in.
    property bool folderMode: false
    signal chosen(string path)

    function trimSlash(p) { return p.length > 1 && p.charAt(p.length - 1) === "/" ? p.substring(0, p.length - 1) : p }
    // the top level: "Up" is not available here and the reader never sees the root of its storage
    readonly property bool atTop: trimSlash(path) === trimSlash(picker.rootDirectory)
    function join(base, name) { return base.charAt(base.length - 1) === "/" ? base + name : base + "/" + name }
    function parentOf(p) {
        var cut = trimSlash(p)
        var i = cut.lastIndexOf("/")
        return i <= 0 ? "/" : cut.substring(0, i)
    }
    function reload() { rows = picker.listDirectory(path) }
    // Taps on entries are ignored for 400 ms after the folder changed: a second tap meant for the old folder would otherwise land on whatever
    // is now under the finger. Up, Cancel, the toggle and paging are not delayed.
    property double lastNavigation: 0
    function changedFolder() { lastNavigation = Date.now(); reload(); showFirstPage() }
    function showFirstPage() { list.contentY = 0; grid.contentY = 0 }
    function setView(v) {
        picker.view = v
        showFirstPage()
    }
    function extensionOf(name) {
        var i = name.lastIndexOf(".")
        return i <= 0 || i === name.length - 1 ? "" : name.substring(i + 1, i + 5).toUpperCase()
    }
    function refusalText(key) { return Problems.title(key) + " " + Problems.action(key) }
    function open(entry) {
        if (Date.now() - lastNavigation < 400) return
        message = ""
        if (entry.disabled) { message = refusalText(entry.reason); return }
        if (entry.dir) { path = entry.path; changedFolder(); return }
        message = refusalText(picker.toggle(entry.path))
        if (message.trim().length === 0) message = ""
        reload()
    }
    function folderSelected() {
        var depends = picker.folderCount + picker.fileCount   // read so that a binding on this function follows the selection
        var f = picker.selectedFolders()
        for (var i = 0; i < f.length; ++i) if (f[i] === trimSlash(path)) return true
        return false
    }
    function toggleThisFolder() {
        message = ""
        var r = picker.addFolder(trimSlash(path))
        if (!r.ok) message = r.overLimit ? qsTr("This folder holds too many files to add at once. Pick a smaller folder.") : refusalText(r.errorKey)
        reload()
    }
    function selectionText() {
        var parts = []
        if (picker.fileCount > 0) parts.push(qsTr("%n file(s)", "", picker.fileCount))
        if (picker.folderCount > 0) parts.push(qsTr("%n folder(s)", "", picker.folderCount))
        return parts.length === 0 ? qsTr("Nothing selected") : parts.join(", ") + ", " + picker.byteText
    }

    // ---- queuing ----
    function finish(results) {
        var count = 0
        var out = []
        var asks = []
        for (var i = 0; i < results.length; ++i) {
            count += results[i].queued
            for (var j = 0; j < results[i].refused.length; ++j) out.push(results[i].refused[j])
            for (var a = 0; a < results[i].asks.length; ++a) asks.push(results[i].asks[a])
            if (results[i].errorKey.length > 0) {
                message = refusalText(results[i].errorKey)
                if (count === 0 && out.length === 0 && asks.length === 0) return
            }
        }
        queuedCount = count
        pendingAsks = asks
        leftOut = out
        picker.clear()
        if (out.length > 0) {
            var lines = []
            for (var k = 0; k < Math.min(out.length, 6); ++k) lines.push(out[k].name + "  -  " + Problems.title(out[k].key))
            if (out.length > 6) lines.push(qsTr("and %n more", "", out.length - 6))
            leftOutDialog.text = lines.join("\n")
            leftOutDialog.open()
        } else {
            done()
        }
    }
    function done() {
        page.queued(queuedCount, pendingAsks)
        if (page.StackView.view) page.StackView.view.pop()
    }
    function upload() {
        message = ""
        var files = picker.selectedFiles(), folders = picker.selectedFolders()
        pickedFiles = files
        if (folders.length === 0) {
            finish([queue.addUploads(targetFolder, files)])
            return
        }
        pickData = queue.addFolderPick(targetFolder, folders)
        if (pickData.errorKey.length > 0) { message = refusalText(pickData.errorKey); return }
        if (pickData.overLimit || pickData.items + files.length > pickData.headroom) { message = refusalText("queue_full"); return }
        var what = qsTr("%1 and %2").arg(qsTr("%n folder(s)", "", pickData.folders)).arg(qsTr("%n file(s)", "", pickData.files + files.length))
        confirm.text = qsTr("%1, %2. Add them to the queue?").arg(what).arg(pickData.bytesText)
        confirm.open()
    }

    function chooseThisFolder() {
        message = ""
        var r = picker.chooseFolder(trimSlash(path))
        if (!r.ok) { message = refusalText(r.errorKey); return }
        page.chosen(r.path)
        if (page.StackView.view) page.StackView.view.pop()
    }

    Component.onCompleted: { if (folderMode) { picker.folderMode = true; reload() } }
    Component.onDestruction: { picker.clear(); picker.folderMode = false }

    TopBar {
        id: topBar
        title: page.folderMode ? qsTr("Reader folder")
            : qsTr("Upload to %1").arg(page.targetFolder === "/" ? qsTr("Files") : page.targetFolder.substring(page.targetFolder.lastIndexOf("/") + 1))
        leftItems: [ IconButton { objectName: "pickerCancel"; text: qsTr("Cancel"); icon: "chevron-left"; onClicked: { picker.clear(); page.StackView.view.pop() } } ]
        rightItems: [
            IconButton {
                objectName: "pickerUp"
                text: qsTr("Up"); icon: "chevron-up"
                enabled: !page.atTop
                onClicked: { page.message = ""; page.path = page.parentOf(page.path); page.changedFolder() }
            },
            // the layout toggle: shows the layout a tap switches to (a grid of squares while the list is shown, bars while tiles are shown)
            IconButton {
                objectName: "pickerLayout"
                visible: !page.folderMode
                property string glyph: page.view === "list" ? "grid" : "list"
                text: qsTr("Layout"); icon: glyph
                onClicked: page.setView(page.view === "list" ? "tile" : "list")
            }
        ]
    }
    Text {
        id: pathLine
        objectName: "pickerPath"
        anchors { top: topBar.bottom; topMargin: 12; left: parent.left; right: parent.right; leftMargin: 30; rightMargin: 30 }
        elide: Text.ElideMiddle
        font.pixelSize: Style.captionSize
        color: Style.ink
        text: page.path
    }
    Text {
        id: selection
        objectName: "pickerSelection"
        anchors { top: pathLine.bottom; left: parent.left; right: parent.right; leftMargin: 30; rightMargin: 30 }
        elide: Text.ElideRight
        font.pixelSize: Style.captionSize
        font.bold: true
        color: Style.ink
        text: page.folderMode ? qsTr("Choose the folder for the pair") : page.selectionText()
    }
    Text {
        id: notice
        objectName: "pickerMessage"
        anchors { top: selection.bottom; left: parent.left; right: parent.right; leftMargin: 30; rightMargin: 30 }
        visible: page.message.length > 0
        wrapMode: Text.Wrap
        font.pixelSize: Style.captionSize
        font.bold: true
        color: Style.ink
        text: page.message
    }
    Rectangle { id: rule; anchors.top: notice.visible ? notice.bottom : selection.bottom; anchors.topMargin: 8; width: parent.width; height: Style.ruleWidth; color: Style.ink }

    // the room between the rule and the page buttons; a view takes whole rows of it, so no row is cut off at a page's end
    readonly property real room: footer.y - (rule.y + rule.height)

    ListView {
        id: list
        anchors { left: parent.left; right: parent.right; top: rule.bottom }
        height: Math.max(Style.rowHeight, Math.floor(page.room / Style.rowHeight) * Style.rowHeight)
        clip: true
        visible: page.view === "list"
        model: page.view === "list" ? page.rows : []
        interactive: false
        boundsBehavior: Flickable.StopAtBounds
        reuseItems: false

        delegate: Item {
            objectName: "pickerRow"
            width: list.width
            height: Style.rowHeight
            readonly property var entry: modelData
            property string text: entry.name   // what a walk reads
            opacity: entry.disabled ? 0.4 : 1.0
            Rectangle { anchors.fill: parent; color: Style.pressed; visible: tap.pressed }
            Icon {
                id: mark
                x: 30
                anchors.verticalCenter: parent.verticalCenter
                size: 48
                source: entry.selected ? "check" : (entry.dir ? "folder" : "list")
            }
            Column {
                anchors { left: mark.right; leftMargin: 24; right: parent.right; rightMargin: 30; verticalCenter: parent.verticalCenter }
                spacing: 4
                Text {
                    width: parent.width
                    elide: Text.ElideMiddle
                    maximumLineCount: 1
                    font.pixelSize: Style.bodySize
                    font.bold: entry.dir || entry.selected
                    color: Style.ink
                    text: entry.name
                }
                Text {
                    width: parent.width
                    elide: Text.ElideRight
                    font.pixelSize: Style.captionSize
                    color: Style.ink
                    text: entry.disabled ? qsTr("Not available for a pair") : entry.dir ? (entry.selected ? qsTr("Folder, selected with everything inside") : qsTr("Folder")) : (entry.selected ? qsTr("Selected") + "  ·  " : "") + entry.sizeText
                }
            }
            RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
            MouseArea { id: tap; anchors.fill: parent; onClicked: page.open(entry) }
        }
    }
    // Tile view: whole rows of tiles per page like the list's whole rows; only the tiles on the page exist
    GridView {
        id: grid
        anchors { left: parent.left; right: parent.right; top: rule.bottom }
        height: Math.max(cellHeight, Math.floor(page.room / cellHeight) * cellHeight)
        clip: true
        visible: page.view === "tile"
        model: page.view === "tile" ? page.rows : []
        interactive: false
        boundsBehavior: Flickable.StopAtBounds
        cacheBuffer: 0
        reuseItems: false
        readonly property int columns: Math.max(2, Math.floor(width / 270))
        cellWidth: width / columns
        cellHeight: 250

        delegate: Item {
            objectName: "pickerTile"
            width: grid.cellWidth
            height: grid.cellHeight
            readonly property var entry: modelData
            Item {   // the symbol area, 170 px high
                x: 10; y: 6
                width: parent.width - 20
                height: 170
                Item {   // folder: a tab on a body
                    objectName: "folderGlyph"
                    visible: entry.dir
                    anchors.centerIn: parent
                    width: 120; height: 92
                    Rectangle { x: 0; y: 0; width: 50; height: 20; color: Style.paper; border.width: 3; border.color: Style.ink }
                    Rectangle { x: 0; y: 16; width: 120; height: 76; color: Style.paper; border.width: 3; border.color: Style.ink }
                }
                Rectangle {   // file: a page with its extension
                    objectName: "fileGlyph"
                    visible: !entry.dir
                    anchors.centerIn: parent
                    width: 100; height: 130
                    color: Style.paper; border.width: 3; border.color: Style.ink
                    Text {
                        anchors.centerIn: parent
                        font.pixelSize: Style.captionSize - 2
                        font.bold: true
                        color: Style.ink
                        text: page.extensionOf(entry.name)
                    }
                }
                Icon {   // a check mark in the corner of what is selected
                    visible: entry.selected
                    anchors { right: parent.right; top: parent.top }
                    size: 56
                    source: "check"
                }
            }
            Text {
                x: 10; y: 180
                width: parent.width - 20
                height: 62
                elide: Text.ElideRight
                maximumLineCount: 2
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: Style.captionSize - 4
                font.bold: entry.dir || entry.selected
                color: Style.ink
                text: entry.name
            }
            Rectangle { anchors.fill: parent; anchors.margins: 3; color: "transparent"; border.width: entry.selected ? 3 : Style.hairlineWidth; border.color: entry.selected ? Style.ink : Style.hairline }
            Rectangle { anchors.fill: parent; color: Style.pressed; visible: tileTap.pressed; z: -1 }
            MouseArea { id: tileTap; anchors.fill: parent; onClicked: page.open(entry) }
        }
    }
    Text {
        objectName: "pickerEmpty"
        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 40 }
        visible: page.rows.length === 0
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: Style.bodySize
        color: Style.ink
        text: page.folderMode ? qsTr("There are no folders in this one. Use New folder to make one.")
            : page.atTop ? qsTr("Nothing to browse here. None of the folders for uploads were found.")
                         : qsTr("This folder is empty or cannot be read.")
    }

    Item {
        id: footer
        anchors { left: parent.left; right: parent.right; bottom: panel.top }
        height: Style.touchMin
        PageButtons { anchors.centerIn: parent; target: page.view === "list" ? list : grid; snap: page.view === "list" ? Style.rowHeight : grid.cellHeight }
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton {
                objectName: "pickerChooseFolder"
                text: qsTr("Choose this folder")
                icon: "check"
                visible: page.folderMode
                onClicked: page.chooseThisFolder()
            },
            ToolButton {
                objectName: "pickerNewFolder"
                text: qsTr("New folder")
                icon: "plus-circle"
                visible: page.folderMode
                onClicked: newFolder.open("")
            },
            ToolButton {
                objectName: "pickerUpload"
                text: qsTr("Upload")
                icon: "send"
                visible: !page.folderMode
                enabled: picker.hasSelection
                onClicked: page.upload()
            },
            ToolButton {
                objectName: "pickerAddFolder"
                text: page.folderSelected() ? qsTr("Remove this folder") : qsTr("Add this folder")
                icon: page.folderSelected() ? "minus-circle" : "plus-circle"
                visible: !page.atTop && !page.folderMode
                onClicked: page.toggleThisFolder()
            },
            ToolButton {
                objectName: "pickerTarget"
                text: qsTr("Target folder")
                icon: "folder"
                visible: !page.folderMode
                onClicked: {
                    var chooserPage = page.StackView.view.push(Qt.resolvedUrl("FolderChooserPage.qml"), { startPath: page.targetFolder })
                    chooserPage.chosen.connect(function (path) { page.targetFolder = path })
                }
            },
            ToolButton {
                objectName: "pickerClear"
                text: qsTr("Clear")
                icon: "close"
                visible: !page.folderMode
                enabled: picker.hasSelection
                onClicked: { picker.clear(); page.message = ""; page.reload() }
            }
        ]
    }

    ConfirmDialog {
        id: confirm
        objectName: "pickerConfirm"
        title: qsTr("Add to the queue?")
        acceptText: qsTr("Add")
        cancelText: qsTr("Cancel")
        onAccepted: {
            var results = [queue.confirmFolderPick(page.pickData.token, "")]
            if (page.pickedFiles.length > 0) results.push(queue.addUploads(page.targetFolder, page.pickedFiles))
            page.finish(results)
        }
    }

    ConfirmDialog {
        id: leftOutDialog
        objectName: "pickerLeftOut"
        title: qsTr("Some files were left out")
        showCancel: false
        onAccepted: page.done()
        onRejected: page.done()
    }

    TextEntryDialog {
        id: newFolder
        objectName: "pickerNewFolderEntry"
        title: qsTr("New folder")
        acceptText: qsTr("Add")
        onAccepted: function (text) {
            var r = picker.createFolder(page.trimSlash(page.path), text)
            if (!r.ok) { errorText = Problems.title(r.errorKey) + " " + Problems.action(r.errorKey); return }
            close()
            page.reload()
        }
    }
}
