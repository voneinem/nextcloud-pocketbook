import QtQuick
import QtQuick.Controls
import ".."

// One folder of the user's Nextcloud, page by page (no scrolling): the path line, how old the list is, the entries with name, size and
// date, page buttons, and the tools Refresh and Sort. Opening a folder uses the stored list when there is one; Refresh is explicit.
Item {
    id: page

    function pathLine() {
        var segs = browse.pathSegments, parts = []
        for (var i = 0; i < segs.length; ++i) parts.push(i === 0 ? qsTr("Files") : segs[i].label)
        return parts.join("  /  ")
    }
    function folderTitle() {
        var segs = browse.pathSegments
        return segs.length > 1 ? segs[segs.length - 1].label : qsTr("Files")
    }
    function ageText() { return EntryWords.ageText(browse.listingAgeKind, browse.listingAgeCount) }
    function reasonText(key) {
        if (key === "end_to_end_encrypted") return qsTr("This folder is end-to-end encrypted. The app cannot open it.")
        if (key === "no_read_permission") return qsTr("You are not allowed to read this.")
        if (key === "unsupported_state") return qsTr("The server shows this entry in a state the app does not support.")
        return qsTr("The server did not let the app read this entry.")
    }
    function rowCaption(row) {
        if (row.availability !== "ok") return qsTr("Not available")
        var parts = [row.kind === "folder" ? qsTr("Folder") : row.sizeText, row.dateText]
        if (row.kind === "file") parts.push(EntryWords.stateWord(row.state))
        if (row.kept) parts.push(qsTr("Kept offline"))
        parts.push(EntryWords.pendingWord(row.pending, row.syncState))
        return parts.filter(function (t) { return t.length > 0 }).join("  ·  ")
    }
    function sortLabel() {
        var k = browse.sortKey, a = browse.sortAscending
        if (k === "date") return a ? qsTr("Date, oldest first") : qsTr("Date, newest first")
        if (k === "size") return a ? qsTr("Size, smallest first") : qsTr("Size, largest first")
        return a ? qsTr("Name, A to Z") : qsTr("Name, Z to A")
    }
    function openSortChoice() {
        sortChoice.choices = [
            { label: qsTr("Name, A to Z"), value: "name:asc" }, { label: qsTr("Name, Z to A"), value: "name:desc" },
            { label: qsTr("Date, newest first"), value: "date:desc" }, { label: qsTr("Date, oldest first"), value: "date:asc" },
            { label: qsTr("Size, largest first"), value: "size:desc" }, { label: qsTr("Size, smallest first"), value: "size:asc" }]
        sortChoice.current = browse.sortKey + ":" + (browse.sortAscending ? "asc" : "desc")
        sortChoice.open()
    }

    Component.onCompleted: browse.open(browse.path)

    TopBar {
        id: topBar
        title: page.folderTitle()
        leftItems: [ IconButton { objectName: "folderBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
        rightItems: [ IconButton { objectName: "folderUp"; text: qsTr("Up"); icon: "chevron-up"; enabled: browse.canGoUp && !browse.busy; onClicked: browse.up() } ]
    }

    Column {
        id: header
        anchors { left: parent.left; right: parent.right; top: topBar.bottom }
        Text {
            objectName: "folderPath"
            x: 30
            width: parent.width - 60
            topPadding: 12
            elide: Text.ElideMiddle
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: page.pathLine()
        }
        Text {
            objectName: "folderAge"
            x: 30
            width: parent.width - 60
            bottomPadding: 10
            wrapMode: Text.Wrap
            visible: browse.hasListing
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: (browse.offline ? qsTr("Offline. ") : "") + page.ageText() + (browse.clockSuspect ? " " + qsTr("(the reader's clock may be wrong)") : "")
        }
        Text {
            objectName: "folderError"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            visible: browse.errorKey.length > 0 && browse.hasListing
            bottomPadding: 10
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: Problems.title(browse.errorKey) + " " + Problems.action(browse.errorKey)
        }
        Rectangle { width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
    }

    Item {
        id: listArea
        anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: pager.top }
        clip: true
        onHeightChanged: browse.pageSize = Math.max(3, Math.floor(height / Style.rowHeight))

        Column {
            width: parent.width
            Repeater {
                model: browse.rows
                delegate: Item {
                    objectName: "folderRow"
                    width: parent.width
                    height: Style.rowHeight
                    opacity: modelData.availability === "ok" ? 1.0 : 0.55
                    Rectangle { anchors.fill: parent; color: Style.pressed; visible: tap.pressed || (browse.selecting && modelData.selected) }
                    Icon {
                        id: kindIcon
                        x: 30
                        anchors.verticalCenter: parent.verticalCenter
                        size: 48
                        source: modelData.kind === "folder" ? "folder" : "list"
                    }
                    Column {
                        anchors { left: kindIcon.right; leftMargin: 24; right: chev.left; rightMargin: 12; verticalCenter: parent.verticalCenter }
                        spacing: 4
                        Text {
                            width: parent.width
                            elide: Text.ElideMiddle
                            font.pixelSize: Style.bodySize
                            font.bold: modelData.kind === "folder"
                            color: Style.ink
                            text: modelData.name
                        }
                        Text {
                            objectName: "folderRowCaption"
                            width: parent.width
                            elide: Text.ElideRight
                            font.pixelSize: Style.captionSize
                            color: Style.ink
                            text: page.rowCaption(modelData)
                        }
                    }
                    Icon {
                        id: chev
                        anchors { right: parent.right; rightMargin: 24; verticalCenter: parent.verticalCenter }
                        size: 40
                        visible: !browse.selecting && modelData.kind === "folder" && modelData.availability === "ok"
                        width: visible || browse.selecting ? 40 : 0
                        source: visible ? "chevron-right" : ""
                    }
                    // select mode: a check box at the right, filled with a check mark when the entry is selected
                    Rectangle {
                        objectName: "folderCheck"
                        visible: browse.selecting
                        anchors { right: parent.right; rightMargin: 24; verticalCenter: parent.verticalCenter }
                        width: 44
                        height: 44
                        color: Style.paper
                        border.width: 3
                        border.color: Style.ink
                        Icon { anchors.centerIn: parent; size: 36; source: "check"; visible: modelData.selected === true }
                    }
                    RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
                    MouseArea {
                        id: tap
                        anchors.fill: parent
                        onClicked: browse.selecting ? browse.toggleSelect(modelData.remotePath) : browse.open(modelData.remotePath)
                    }
                }
            }
        }
        Text {
            objectName: "folderEmpty"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 60 }
            wrapMode: Text.Wrap
            visible: !browse.busy && browse.rows.length === 0
            font.pixelSize: Style.bodySize
            color: Style.ink
            text: browse.hasListing ? qsTr("This folder is empty.")
                : (browse.errorKey === "no_network" || browse.errorKey === "server_unreachable" || browse.errorKey === "timeout")
                    ? qsTr("This folder is not available offline. Connect to the network and open it again.")
                : browse.errorKey.length > 0 ? Problems.title(browse.errorKey) + " " + Problems.action(browse.errorKey)
                : ""
        }
        Text {
            objectName: "folderBusy"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 60 }
            visible: browse.busy
            font.pixelSize: Style.bodySize
            font.bold: true
            color: Style.ink
            text: qsTr("Loading...")
        }
    }

    ModelPager {
        id: pager
        anchors { horizontalCenter: parent.horizontalCenter; bottom: panel.top }
        page: browse.page
        pageCount: browse.pageCount
        visible: browse.pageCount > 1
        height: visible ? Style.touchMin : 0
        onPrevious: browse.previousPage()
        onNext: browse.nextPage()
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton { objectName: "folderRefresh"; text: browse.hasListing ? qsTr("Refresh") : qsTr("Retry"); icon: "sync"; visible: !browse.selecting; enabled: !browse.busy; onClicked: browse.refresh() },
            ToolButton {
                objectName: "folderKeep"
                text: browse.folderKept ? qsTr("Kept offline") : qsTr("Keep offline")
                icon: browse.folderKept ? "check" : "download"
                visible: !browse.selecting && browse.canGoUp && browse.hasListing
                enabled: !browse.folderKept || browse.folderKeptDirect
                onClicked: { transfer.setKept(browse.path, "folder", !browse.folderKeptDirect); browse.refreshRows() }
            },
            ToolButton {
                objectName: "folderAdd"
                text: qsTr("Add")
                icon: "plus-circle"
                visible: !browse.selecting
                enabled: connection.hasAccount && !browse.busy && browse.hasListing
                onClicked: { addChoice.choices = [{ label: qsTr("Upload files"), value: "upload" }, { label: qsTr("New folder"), value: "folder" }]; addChoice.open() }
            },
            ToolButton {
                objectName: "folderSelect"
                text: qsTr("Select")
                icon: "select-all"
                visible: !browse.selecting
                enabled: connection.hasAccount && !browse.busy && browse.hasListing && browse.rows.length > 0
                onClicked: browse.setSelecting(true)
            },
            ToolButton {
                objectName: "folderRename"
                text: qsTr("Rename")
                icon: "pencil"
                visible: browse.selecting
                enabled: browse.selectedCount === 1
                onClicked: { var r = page.selectedRow(); if (r) renameEntry.open(r.name) }
            },
            ToolButton {
                objectName: "folderShare"
                text: qsTr("Share")
                icon: "share"
                visible: browse.selecting
                enabled: browse.selectedCount === 1
                onClicked: { var r = page.selectedRow(); if (r) page.openShare(r.remotePath) }
            },
            ToolButton {
                objectName: "folderMove"
                text: qsTr("Move")
                icon: "cut"
                visible: browse.selecting
                enabled: browse.selectedCount > 0
                onClicked: page.chooseTarget("move")
            },
            ToolButton {
                objectName: "folderCopy"
                text: qsTr("Copy")
                icon: "copy"
                visible: browse.selecting
                enabled: browse.selectedCount > 0
                onClicked: page.chooseTarget("copy")
            },
            ToolButton {
                objectName: "folderDelete"
                text: qsTr("Delete")
                icon: "trash"
                visible: browse.selecting
                enabled: browse.selectedCount > 0
                onClicked: page.askDelete()
            },
            ToolButton {
                objectName: "folderSelectDone"
                text: qsTr("Done")
                icon: "check"
                visible: browse.selecting
                onClicked: browse.setSelecting(false)
            },
            ToolButton {
                objectName: "folderQueue"
                visible: !browse.selecting
                text: queue.openCount > 0 ? qsTr("Queue (%1)").arg(queue.openCount) : qsTr("Queue")
                icon: "list"
                onClicked: page.StackView.view.push(Qt.resolvedUrl("QueuePage.qml"))
            },
            ToolButton {
                objectName: "folderTrash"
                text: qsTr("Trash")
                icon: "trash"
                visible: !browse.selecting
                enabled: connection.hasAccount
                onClicked: page.StackView.view.push(Qt.resolvedUrl("TrashPage.qml"))   // DT-T1 checks that the panel still fits at the usual touch size
            },
            ToolButton { objectName: "folderSort"; text: qsTr("Sort"); icon: "filter"; visible: !browse.selecting; enabled: browse.hasListing; onClicked: page.openSortChoice() },
            ToolButton { objectName: "folderSettings"; text: qsTr("Settings"); icon: "gear"; visible: !browse.hasListing && !browse.busy; onClicked: page.StackView.view.push(Qt.resolvedUrl("SettingsPage.qml")) }
        ]
    }

    Connections {
        target: transfer
        function onJobFinished() { browse.refreshRows() }
        function onUpdateFinished() { browse.refreshRows() }
    }

    Connections {
        target: queue
        function onSendFinished() { browse.refreshRows() }   // what the server confirmed is no longer "Not synced"
    }

    Connections {
        target: browse
        enabled: page.StackView.status === StackView.Active   // a page above this one (favorites, search) handles its own taps
        function onShowReason(key) { reasonDialog.text = page.reasonText(key); reasonDialog.open() }
        function onShowDetails(remotePath) { page.StackView.view.push(Qt.resolvedUrl("DetailsPage.qml"), { remotePath: remotePath }) }
    }

    ConfirmDialog {
        id: reasonDialog
        objectName: "folderReason"
        title: qsTr("Not available")
        showCancel: false
    }

    // the Add tool: what the user can put into this folder
    ChoiceDialog {
        id: addChoice
        objectName: "folderAddChoice"
        title: qsTr("Add")
        anchor: panel
        onChosen: function (value) {
            if (value === "folder") { newFolderEntry.open(""); return }
            if (value === "upload") {
                var picker = page.StackView.view.push(Qt.resolvedUrl("FilePickerPage.qml"), { targetFolder: browse.path })
                picker.queued.connect(function (count, list) {
                    page.queuedCount = count
                    if (list.length > 0) page.startAsks(list)
                    else if (count > 0) queuedNotice.open()
                })
            }
        }
    }

    property int queuedCount: 0

    // ---- names that are taken (specs/002-manage-server-files US3): one question at a time; nothing is queued for a name until it is answered ----
    property var asks: []
    function startAsks(list) { asks = list.slice(); askNext() }
    function askNext() {
        if (asks.length === 0) { browse.refreshRows(); if (queuedCount > 0) queuedNotice.open(); return }
        collision.ask = asks[0]
        collision.others = asks.length - 1
        collision.sameForOthers = false
        collision.open()
    }
    function dropFirst() { var rest = asks.slice(1); asks = rest }
    // The same answer for the question and all that follow. A name that cannot be replaced, or a new name that is taken as well, is asked again.
    function answerAll(choice) {
        var rest = asks.slice()
        var again = []
        for (var i = 0; i < rest.length; ++i) {
            if (choice === "replace" && !rest[i].replacePossible) { again.push(rest[i]); continue }
            var r = queue.answerCollision(rest[i].askId, choice, "")
            queuedCount += r.queued
            for (var j = 0; j < r.asks.length; ++j) again.push(r.asks[j])
        }
        asks = again
        askNext()
    }
    function answerOne(choice, name) {
        var r = queue.answerCollision(asks[0].askId, choice, name)
        queuedCount += r.queued
        if (r.errorKey.length > 0) return r
        if (r.queued === 0 && r.asks.length > 0 && choice === "keep_both") { var copy = asks.slice(); copy[0] = r.asks[0]; asks = copy; return r }
        dropFirst()
        return r
    }
    ConfirmDialog {
        id: queuedNotice
        objectName: "folderQueuedNotice"
        title: qsTr("Added to the queue")
        text: qsTr("%n item(s) are in the queue, marked Not synced. Nothing has been sent yet.", "", page.queuedCount)
        showCancel: false
        onVisibleChanged: if (!visible) browse.refreshRows()
    }

    // ---- Move and Copy: choose the destination, confirm, queue ----
    property string moveKind: "move"
    property string moveTarget: ""
    function chooseTarget(kind) {
        moveKind = kind
        var blocked = []
        var paths = browse.selectedPaths
        for (var i = 0; i < browse.rows.length; ++i) if (browse.rows[i].selected && browse.rows[i].kind === "folder") blocked.push(browse.rows[i].remotePath)
        var chooserPage = page.StackView.view.push(Qt.resolvedUrl("FolderChooserPage.qml"), { startPath: browse.path, blocked: blocked })
        chooserPage.chosen.connect(function (target) {
            moveTarget = target
            var folders = blocked.length > 0
            var what = qsTr("%n entry(ies)", "", paths.length)
            moveConfirm.text = (moveKind === "copy" ? qsTr("Copy %1 to \"%2\"?") : qsTr("Move %1 to \"%2\"?")).arg(what).arg(target)
                + (folders ? " " + qsTr("Folders go with everything inside them.") : "")
            moveConfirm.open()
        })
    }
    function afterMoveCopy(r) {
        queuedCount = r.queued
        browse.setSelecting(false)
        if (r.errorKey.length > 0) { askFail.text = Problems.title(r.errorKey) + " " + Problems.action(r.errorKey); askFail.open(); return }
        if (r.refused.length > 0) {
            var lines = []
            for (var k = 0; k < Math.min(r.refused.length, 6); ++k) lines.push(r.refused[k].name + "  -  " + Problems.title(r.refused[k].key))
            if (r.refused.length > 6) lines.push(qsTr("and %n more", "", r.refused.length - 6))
            refusedMoves.text = lines.join("\n")
            refusedMoves.pendingAsks = r.asks
            refusedMoves.open()
            return
        }
        continueAfterMoveCopy(r.asks)
    }
    function continueAfterMoveCopy(list) {
        if (list.length > 0) startAsks(list)
        else { browse.refreshRows(); if (queuedCount > 0) queuedNotice.open() }
    }

    // ---- Delete: the confirmation names the trash; without a trash nothing is offered (research R-9) ----
    function askDelete() {
        var info = queue.deleteEntries(browse.selectedPaths, false)
        if (!info.deleteOffered) { (info.trashKnown ? noTrash : trashUnknown).open(); return }
        var what
        if (info.entries === 1) {
            what = info.folders === 1 ? qsTr("Delete \"%1\" and everything inside it? It goes to the server's trash.").arg(info.names[0])
                                      : qsTr("Delete \"%1\"? It goes to the server's trash.").arg(info.names[0])
        } else {
            what = qsTr("Delete %n entries? They go to the server's trash.", "", info.entries)
            if (info.folders > 0) what += " " + qsTr("Folders go with everything inside them.")
        }
        if (info.itemsInside > 0) what += " " + qsTr("%n queued item(s) inside will fail.", "", info.itemsInside)
        deleteConfirm.text = what
        deleteConfirm.open()
    }
    function afterDelete(r) {
        browse.setSelecting(false)
        if (r.errorKey.length > 0) { askFail.text = Problems.title(r.errorKey) + " " + Problems.action(r.errorKey); askFail.open(); return }
        queuedCount = r.queued
        if (r.refused.length > 0) {
            var lines = []
            for (var k = 0; k < Math.min(r.refused.length, 6); ++k) lines.push(r.refused[k].name + "  -  " + Problems.title(r.refused[k].key))
            refusedMoves.text = lines.join("\n")
            refusedMoves.pendingAsks = []
            refusedMoves.open()
            return
        }
        browse.refreshRows()
        if (queuedCount > 0) queuedNotice.open()
    }

    // Share (specs/004-share-links): reads the rules and the shares of the selected item and shows them; an unavailable item and a missing connection are
    // shown by the share page itself, anything else that stops the read is told here
    function openShare(path) {
        var r = share.open(path)
        if (r.errorKey === "busy" || r.errorKey === "share_item_gone") {
            shareFail.text = Problems.title(r.errorKey) + " " + Problems.action(r.errorKey)
            shareFail.open()
            return
        }
        page.StackView.view.push(Qt.resolvedUrl("SharePage.qml"))
    }
    function selectedRow() {
        for (var i = 0; i < browse.rows.length; ++i) if (browse.rows[i].selected) return browse.rows[i]
        return null
    }
    function nameProblem(r) {
        if (r.exists) return qsTr("A file or folder with this name is already there.")
        return Problems.title(r.errorKey) + " " + Problems.action(r.errorKey)
    }

    TextEntryDialog {
        id: newFolderEntry
        objectName: "folderNewFolder"
        title: qsTr("New folder")
        acceptText: qsTr("Add")
        onAccepted: function (text) {
            var r = queue.newFolder(browse.path, text)
            if (r.errorKey.length > 0) { errorText = page.nameProblem(r); return }
            close()
            browse.refreshRows()
        }
    }

    TextEntryDialog {
        id: renameEntry
        objectName: "folderRenameEntry"
        title: qsTr("Rename")
        acceptText: qsTr("Rename")
        onAccepted: function (text) {
            var sel = page.selectedRow()
            if (!sel) { close(); return }
            var r = queue.rename(sel.remotePath, text)
            if (r.errorKey.length > 0) { errorText = page.nameProblem(r); return }
            close()
            browse.setSelecting(false)
            if (r.asks.length > 0) { page.queuedCount = 0; page.startAsks(r.asks); return }   // the name is taken: the user decides
            browse.refreshRows()
        }
    }

    ConfirmDialog {
        id: deleteConfirm
        objectName: "folderDeleteConfirm"
        title: qsTr("Delete")
        acceptText: qsTr("Delete")
        onAccepted: page.afterDelete(queue.deleteEntries(browse.selectedPaths, true))
    }

    ConfirmDialog {
        id: trashUnknown
        objectName: "folderTrashUnknown"
        title: qsTr("Delete")
        text: qsTr("The reader does not know yet whether the server has a trash. Tap Test in Settings with a connection, then try again.")
        showCancel: false
    }

    ConfirmDialog {
        id: noTrash
        objectName: "folderNoTrash"
        title: qsTr("Delete")
        text: qsTr("This server has no trash. Deleting here would be permanent, so PB Cloud does not offer it. Delete in the web interface if you are sure.")
        showCancel: false
    }

    ConfirmDialog {
        id: moveConfirm
        objectName: "folderMoveConfirm"
        title: page.moveKind === "copy" ? qsTr("Copy") : qsTr("Move")
        acceptText: page.moveKind === "copy" ? qsTr("Copy") : qsTr("Move")
        onAccepted: page.afterMoveCopy(queue.moveOrCopy(page.moveKind, browse.selectedPaths, page.moveTarget))
    }

    ConfirmDialog {
        id: refusedMoves
        objectName: "folderRefusedMoves"
        property var pendingAsks: []
        title: qsTr("Some entries were left out")
        showCancel: false
        onAccepted: page.continueAfterMoveCopy(pendingAsks)
        onRejected: page.continueAfterMoveCopy(pendingAsks)
    }

    CollisionDialog {
        id: collision
        objectName: "folderCollision"
        onAnswered: function (choice, all) {
            if (choice === "replace") { replaceConfirm.all = all; replaceConfirm.open(); return }
            if (choice === "keep_both" && !all) { nameEntry.open(page.asks[0].suggested); return }
            if (all) { page.answerAll(choice); return }
            page.answerOne("skip", "")
            page.askNext()
        }
    }

    ConfirmDialog {
        id: replaceConfirm
        objectName: "folderReplaceConfirm"
        property bool all: false
        title: qsTr("Replace on the server")
        text: all ? qsTr("The files on the server with these names will be replaced by yours.")
                  : qsTr("Replace \"%1\" on the server? The file on the server will be replaced by yours.").arg(page.asks.length > 0 ? page.asks[0].name : "")
        acceptText: qsTr("Replace")
        onAccepted: {
            if (all) { page.answerAll("replace"); return }
            var r = page.answerOne("replace", "")
            if (r.errorKey.length > 0) { askFail.text = Problems.title(r.errorKey) + " " + Problems.action(r.errorKey); askFail.open(); return }
            page.askNext()
        }
        onRejected: collision.open()
    }

    TextEntryDialog {
        id: nameEntry
        objectName: "folderKeepBothName"
        title: qsTr("Keep both")
        acceptText: qsTr("Add")
        onAccepted: function (text) {
            var r = page.answerOne("keep_both", text)
            if (r.errorKey.length > 0) { errorText = Problems.title(r.errorKey); return }
            if (r.queued === 0 && r.asks.length > 0) { errorText = qsTr("That name is taken as well."); return }
            close()
            page.askNext()
        }
        onRejected: collision.open()
    }

    ConfirmDialog {
        id: askFail
        objectName: "folderAskFail"
        title: qsTr("Not added")
        showCancel: false
        onAccepted: { page.dropFirst(); page.askNext() }
        onRejected: { page.dropFirst(); page.askNext() }
    }

    ChoiceDialog {
        id: sortChoice
        objectName: "folderSortChoice"
        title: qsTr("Sort by")
        anchor: panel
        onChosen: function (value) {
            var p = value.split(":")
            browse.setSort(p[0], p[1] === "asc")
        }
    }

    ConfirmDialog {
        id: shareFail
        objectName: "shareFailDialog"
        title: qsTr("Share")
        showCancel: false
    }
}
