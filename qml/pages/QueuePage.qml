import QtQuick
import QtQuick.Controls
import ".."

// The queue of changes (specs/002-manage-server-files): what the user asked for and the server has not confirmed yet, one row per change in the
// order they were made, page by page. Send runs the queue (with Auto Sync on it also runs a short while after the user's own action); Retry sends
// one failed item again; Remove drops an item (a file on the reader is never touched); an item whose outcome is unknown is checked on the server
// first and the user decides. Nothing here starts by itself.
Item {
    id: page
    property int selectedId: 0
    property var selectedRow: null
    property int checkedId: 0
    property string pendingChoice: "replace_anyway"

    function rowFor(id) {
        var items = queue.items
        for (var i = 0; i < items.length; ++i) if (items[i].id === id) return items[i]
        return null
    }
    function select(row) {
        if (selectedId === row.id) { selectedId = 0; selectedRow = null; return }
        selectedId = row.id
        selectedRow = row
    }
    // the selection follows the rows when the queue changes under it
    Connections {
        target: queue
        function onChanged() { page.selectedRow = page.rowFor(page.selectedId); if (page.selectedRow === null) page.selectedId = 0 }
    }
    function kindWord(kind) {
        switch (kind) {
        case "upload": return qsTr("Upload")
        case "mkdir": return qsTr("New folder")
        case "rename": return qsTr("Rename")
        case "move": return qsTr("Move")
        case "copy": return qsTr("Copy")
        case "delete": return qsTr("Delete")
        default: return ""
        }
    }
    function iconFor(kind) {
        switch (kind) {
        case "mkdir": return "folder"
        case "rename": return "pencil"
        case "move": return "cut"
        case "copy": return "copy"
        case "delete": return "trash"
        default: return "send"
        }
    }
    function stateWord(row) {
        switch (row.state) {
        case "sending": return row.progressText.length > 0 ? qsTr("Sending") + " " + row.progressText : qsTr("Sending")
        case "done": return qsTr("Sent")
        case "failed": return qsTr("Failed")
        case "conflict": return qsTr("Conflict")
        case "unknown": return qsTr("Outcome unknown")
        case "waiting": return qsTr("Waiting for %1").arg(row.waitingFor)
        case "other_account": return qsTr("Another account")
        default: return qsTr("Not synced")
        }
    }
    function reasonText(row) {
        if (row.reason.length === 0 || row.state === "queued" || row.state === "sending" || row.state === "done") return ""
        return Problems.title(row.reason)
    }
    // kind, size and state on one line; why an item is held follows the state ("Failed: ...") and is cut at the end of the line
    function caption(row) {
        var why = reasonText(row)
        var state = stateWord(row) + (why.length > 0 ? ": " + why : "")
        return [kindWord(row.kind), row.sizeText, state, row.sourceChanged ? qsTr("File changed since queued") : "", row.syncedBy && row.syncedBy.length > 0 ? qsTr("Sync: %1").arg(row.syncedBy) : ""].filter(function (t) { return t.length > 0 }).join("  ·  ")
    }
    function noticeText() {
        switch (queue.waitingNotice) {
        case "offline": return qsTr("No connection. The queue waits until you tap Send with a connection.")
        case "rate_limited": return qsTr("The server asks for a pause. Tap Send later.")
        case "maintenance": return qsTr("The server is in maintenance mode. Tap Send later.")
        case "signin": return qsTr("Please sign in again in Settings. Nothing was lost.")
        default: return ""
        }
    }
    function lastText() {
        if (queue.lastRunText.length === 0) return ""
        var c = queue.lastRunCounts
        var held = (c.failed || 0) + (c.conflict || 0) + (c.unknown || 0)
        return held > 0 ? qsTr("Last sent: %1 (%n sent, %2 need you)", "", c.done || 0).arg(queue.lastRunText).arg(held)
                        : qsTr("Last sent: %1 (%n sent)", "", c.done || 0).arg(queue.lastRunText)
    }
    function checkText(result) {
        switch (result) {
        case "arrived": return qsTr("It is on the server. Mark the item as done?")
        case "not_there": return qsTr("It is not on the server. Send it again, or remove the item.")
        case "something_else": return qsTr("A different file with this name is on the server. Send it again to be asked what to do, or remove the item.")
        default: return qsTr("The check did not work. Try again when there is a connection.")
        }
    }

    // the choices for the selected item that waits for the user
    function openResolve() {
        var row = selectedRow
        if (row.state === "unknown") {
            resolveChoice.title = qsTr("Outcome unknown")
            resolveChoice.choices = [{ label: qsTr("Check the server"), value: "check" }, { label: qsTr("Send again"), value: "again" }, { label: qsTr("Remove"), value: "remove" }]
        } else if (row.state === "failed" && row.reason === "target_gone" && row.kind !== "rename" && row.kind !== "delete") {   // the target folder is gone
            resolveChoice.title = qsTr("Folder not found")
            resolveChoice.choices = [{ label: qsTr("Choose another folder"), value: "retarget" }, { label: qsTr("Remove"), value: "remove" }]
        } else if (row.state === "failed") {   // the server refused the name
            resolveChoice.title = qsTr("Name not accepted")
            resolveChoice.choices = [{ label: qsTr("Edit name"), value: "edit" }, { label: qsTr("Remove"), value: "remove" }]
        } else if (row.kind === "upload" || (row.reason === "name_exists" && row.kind !== "mkdir")) {
            resolveChoice.title = row.reason === "name_exists" ? qsTr("Name already taken") : qsTr("Changed on the server")
            resolveChoice.choices = row.entryKind === "file"
                ? [{ label: qsTr("Keep both"), value: "keep_both" }, { label: qsTr("Replace anyway"), value: "replace" }, { label: qsTr("Skip"), value: "skip" }]
                : [{ label: qsTr("Keep both"), value: "keep_both" }, { label: qsTr("Skip"), value: "skip" }]
        } else if (row.reason === "changed_on_server") {   // a rename, move, copy or delete of an entry that changed on the server
            resolveChoice.title = qsTr("Changed on the server")
            resolveChoice.choices = row.kind === "delete"
                ? [{ label: qsTr("Delete anyway"), value: "apply" }, { label: qsTr("Keep the entry"), value: "skip" }]
                : [{ label: qsTr("Apply anyway"), value: "apply" }, { label: qsTr("Discard"), value: "skip" }]
        } else {
            resolveChoice.title = qsTr("Name already taken")
            resolveChoice.choices = [{ label: qsTr("Discard"), value: "skip" }]
        }
        resolveChoice.open()
    }
    function resolveError(key) {
        if (key.length === 0) return
        failDialog.text = Problems.title(key) + " " + Problems.action(key)
        failDialog.open()
    }

    Component.onCompleted: queue.refresh()

    TopBar {
        id: topBar
        title: qsTr("Queue")
        leftItems: [ IconButton { objectName: "queueBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: { queue.leaveScreen(); if (page.StackView.view) page.StackView.view.pop() } } ]
    }

    Column {
        id: header
        anchors { left: parent.left; right: parent.right; top: topBar.bottom }
        Text {
            objectName: "queueOffline"
            x: 30
            width: parent.width - 60
            topPadding: 12
            wrapMode: Text.Wrap
            visible: queue.offline
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: qsTr("Offline. Your changes are kept and nothing is lost.")
        }
        Text {
            objectName: "queueNotice"
            x: 30
            width: parent.width - 60
            topPadding: 12
            wrapMode: Text.Wrap
            visible: text.length > 0 && !queue.running
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: page.noticeText()
        }
        Text {
            objectName: "queueRun"
            x: 30
            width: parent.width - 60
            topPadding: 12
            wrapMode: Text.Wrap
            visible: queue.running
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: (queue.runStage === "preparing" ? qsTr("Preparing...") : qsTr("Sending...")) + (queue.runProgressText.length > 0 ? "  " + queue.runProgressText : "")
        }
        Text {
            objectName: "queueLast"
            x: 30
            width: parent.width - 60
            topPadding: 8
            bottomPadding: 10
            wrapMode: Text.Wrap
            visible: text.length > 0
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: page.lastText()
        }
        Rectangle { width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
    }

    Item {
        id: listArea
        anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: pager.top }
        clip: true
        onHeightChanged: queue.pageSize = Math.max(3, Math.floor(height / Style.rowHeight))

        Column {
            width: parent.width
            Repeater {
                model: queue.items
                delegate: Item {
                    objectName: "queueRow"
                    width: parent.width
                    height: Style.rowHeight
                    Rectangle { anchors.fill: parent; color: Style.pressed; visible: tap.pressed || modelData.id === page.selectedId }
                    Icon {
                        id: kindIcon
                        x: 30
                        anchors.verticalCenter: parent.verticalCenter
                        size: 48
                        source: page.iconFor(modelData.kind)
                    }
                    Column {
                        anchors { left: kindIcon.right; leftMargin: 24; right: parent.right; rightMargin: 30; verticalCenter: parent.verticalCenter }
                        spacing: 4
                        Text {
                            width: parent.width
                            elide: Text.ElideMiddle
                            font.pixelSize: Style.bodySize
                            font.bold: modelData.id === page.selectedId
                            color: Style.ink
                            text: modelData.name
                        }
                        Text {
                            objectName: "queueRowCaption"
                            width: parent.width
                            elide: Text.ElideRight
                            font.pixelSize: Style.captionSize
                            color: Style.ink
                            text: page.caption(modelData)
                        }
                    }
                    RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
                    MouseArea { id: tap; anchors.fill: parent; onClicked: page.select(modelData) }
                }
            }
        }
        Text {
            objectName: "queueEmpty"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 60 }
            wrapMode: Text.Wrap
            visible: queue.total === 0
            font.pixelSize: Style.bodySize
            color: Style.ink
            text: qsTr("Nothing is waiting to be sent.")
        }
    }

    ModelPager {
        id: pager
        anchors { horizontalCenter: parent.horizontalCenter; bottom: panel.top }
        page: queue.page
        pageCount: queue.pageCount
        visible: queue.pageCount > 1
        height: visible ? Style.touchMin : 0
        onPrevious: queue.previousPage()
        onNext: queue.nextPage()
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton {
                objectName: "queueSend"
                text: qsTr("Send")
                icon: "send"
                visible: !queue.running
                enabled: connection.hasAccount && queue.openCount > 0
                onClicked: queue.send()
            },
            ToolButton {
                objectName: "queueCancel"
                text: qsTr("Cancel")
                icon: "close"
                visible: queue.running
                onClicked: queue.cancelSend()
            },
            ToolButton {
                objectName: "queueRetry"
                text: qsTr("Retry")
                icon: "redo"
                enabled: page.selectedRow !== null && page.selectedRow.canRetry && !queue.running
                onClicked: queue.retry(page.selectedId)
            },
            ToolButton {
                objectName: "queueResolve"
                text: qsTr("Resolve")
                icon: "check"
                enabled: page.selectedRow !== null && page.selectedRow.canResolve && !queue.running
                onClicked: page.openResolve()
            },
            ToolButton {
                objectName: "queueSettings"
                text: qsTr("Settings")
                icon: "gear"
                visible: queue.waitingNotice === "signin" && !queue.running
                onClicked: if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("SettingsPage.qml"))
            },
            ToolButton {
                objectName: "queueRemove"
                text: qsTr("Remove")
                icon: "trash"
                enabled: page.selectedRow !== null && page.selectedRow.state !== "sending"
                onClicked: removeDialog.open()
            }
        ]
    }

    ChoiceDialog {
        id: resolveChoice
        objectName: "queueResolveChoice"
        title: qsTr("Outcome unknown")
        anchor: panel
        onChosen: function (value) {
            if (value === "check") { page.checkedId = page.selectedId; queue.checkUnknown(page.selectedId) }
            else if (value === "again") queue.sendAgain(page.selectedId)
            else if (value === "keep_both") queue.resolve(page.selectedId, "keep_both", "")
            else if (value === "replace") { page.checkedId = page.selectedId; page.pendingChoice = "replace_anyway"; queue.prepareResolve(page.selectedId) }
            else if (value === "apply") { page.checkedId = page.selectedId; page.pendingChoice = "apply_anyway"; queue.prepareResolve(page.selectedId) }
            else if (value === "skip") queue.resolve(page.selectedId, "skip", "")
            else if (value === "retarget") {
                var id = page.selectedId
                var chooserPage = page.StackView.view.push(Qt.resolvedUrl("FolderChooserPage.qml"), { startPath: "/" })
                chooserPage.chosen.connect(function (path) { queue.resolve(id, "retarget", path) })
            }
            else if (value === "edit") { page.checkedId = page.selectedId; editName.open(page.selectedRow.name) }
            else removeDialog.open()
        }
    }

    Connections {
        target: queue
        function onResolvePrepared(id, info) {
            if (!info.ok) { page.resolveError(info.errorKey); return }
            var kind = page.selectedRow ? page.selectedRow.kind : "upload"
            if (kind !== "upload" && page.pendingChoice === "replace_anyway")
                replaceAnyway.text = info.exists
                    ? qsTr("\"%1\" is on the server (now %2, %3). Replace it with this entry anyway? The old version goes to the server's trash.").arg(info.name).arg(info.sizeText).arg(info.dateText)
                    : qsTr("\"%1\" is not on the server any more. The change will be applied without replacing anything.").arg(info.name)
            else if (kind === "upload")
                replaceAnyway.text = info.exists
                    ? qsTr("\"%1\" on the server has changed since you last saw it (now %2, %3). Replace it anyway?").arg(info.name).arg(info.sizeText).arg(info.dateText)
                    : qsTr("\"%1\" is not on the server any more. It will be uploaded as a new file.").arg(info.name)
            else if (!info.exists)
                replaceAnyway.text = qsTr("\"%1\" is not on the server any more. Nothing can be applied to it.").arg(info.name)
            else if (kind === "delete")
                replaceAnyway.text = qsTr("\"%1\" has changed on the server since you last saw it (now %2, %3). Delete it anyway? It goes to the server's trash.").arg(info.name).arg(info.sizeText).arg(info.dateText)
            else
                replaceAnyway.text = qsTr("\"%1\" has changed on the server since you last saw it (now %2, %3). Apply the change anyway?").arg(info.name).arg(info.sizeText).arg(info.dateText)
            replaceAnyway.open()
        }
        function onResolveFinished(id, key) { page.resolveError(key) }
        function onCheckFinished(id, result) {
            checkDialog.text = page.checkText(result)
            checkDialog.arrived = result === "arrived"
            checkDialog.open()
        }
    }

    ConfirmDialog {
        id: checkDialog
        objectName: "queueCheckDialog"
        property bool arrived: false
        title: qsTr("Check the server")
        acceptText: arrived ? qsTr("Mark as done") : qsTr("OK")
        showCancel: arrived
        onAccepted: if (arrived) queue.markDone(page.checkedId)
    }

    TextEntryDialog {
        id: editName
        objectName: "queueEditName"
        title: qsTr("Edit name")
        acceptText: qsTr("Queue again")
        property int editingId: 0
        onAccepted: function (text) {
            // checked here as the core checks it: a name the server refuses is refused again before anything is queued
            editingId = page.checkedId
            queue.resolve(page.checkedId, "edit", text)
            close()
        }
    }

    ConfirmDialog {
        id: replaceAnyway
        objectName: "queueReplaceAnyway"
        title: page.pendingChoice === "replace_anyway" ? qsTr("Replace anyway") : qsTr("Apply anyway")
        acceptText: page.pendingChoice === "replace_anyway" ? qsTr("Replace") : qsTr("Apply")
        onAccepted: queue.resolve(page.checkedId, page.pendingChoice, "")
    }

    ConfirmDialog {
        id: failDialog
        objectName: "queueFailDialog"
        title: qsTr("Not done")
        showCancel: false
    }

    ConfirmDialog {
        id: removeDialog
        objectName: "queueRemoveDialog"
        title: qsTr("Remove from the queue")
        text: qsTr("The change is dropped and will not be sent. Files on the reader and on the server are not touched.")
        acceptText: qsTr("Remove")
        onAccepted: queue.remove(page.selectedId)
    }
}
