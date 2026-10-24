import QtQuick
import QtQuick.Controls
import ".."

// The server's trash (specs/006-trash-and-versions): what was deleted, newest first, with the folder it came from and the day it was deleted. Opening
// the page is the user's tap on Trash and the only thing that reads the server; nothing here starts by itself. One item at a time is chosen (a check mark
// and the line "Chosen: ..."). When the trash app of the server is off, or its answer is unclear, the page says so in plain words and offers no tool.
Item {
    id: page
    objectName: "trashPage"

    // the plain-language text of the last problem (qml/Problems.qml); the short code of an unclear answer is added here
    function messageText() {
        var m = trash.message
        if (!m || !m.key) return ""
        var t = Problems.title(m.key) + " " + Problems.action(m.key)
        if (m.code && m.code.length > 0) t += " (" + qsTr("Code %1").arg(m.code) + ")"
        return t
    }
    function explainText() {
        if (trash.state === "explainOff")
            return Problems.title("trash_unavailable") + " " + qsTr("Deleted files cannot be listed or restored from here, and deleting on this server is permanent.") + " " + qsTr("Ask the server's administrator.")
        if (trash.state === "explainUnknown") return qsTr("The server's answer was not clear.") + " " + page.messageText()
        return ""
    }
    function listShown() { return trash.state === "listed" }
    function ageLine() {
        return (trash.offline ? qsTr("Offline. ") : "") + EntryWords.ageText(trash.ageKind, trash.ageCount)
            + (trash.clockSuspect ? " " + qsTr("(the reader's clock may be wrong)") : "")
    }
    function leave() { if (page.StackView.view) page.StackView.view.pop() }
    Component.onCompleted: trash.open()
    Component.onDestruction: { trash.cancelStep(); trash.cancelRun() }   // a proposal does not outlive the screen, and a run of Empty trash stops when the screen is left

    // ---- restore (spec Story 2) ----
    property string restoreName: ""       // the chosen item, for the texts of the dialogs
    property string restoreMode: "original"   // original | chosen: what the confirmation is for
    property string restorePlace: ""
    function restoreText() {
        return restoreMode === "original"
            ? qsTr("Restore %1 to %2? It goes back to its original place.").arg(restoreName).arg(restorePlace)
            : qsTr("Restore %1 to %2? You chose this place.").arg(restoreName).arg(restorePlace)
    }
    function chooseFolder() {
        var chooser = page.StackView.view.push(Qt.resolvedUrl("FolderChooserPage.qml"), { serverOnly: true, startPath: "/", verb: qsTr("Restore") })
        chooser.chosen.connect(function (path) { trash.chooseTarget(path) })
    }
    // ---- delete for good and Empty trash (spec Story 4): a chain of one or two confirmations that name what will be lost; Cancel is the default ----
    function deleteText(c) {
        var last = c.stage === 2
        if (c.kind === "empty_trash") {
            var what = qsTr("%n item(s)", "", c.items)
            var t = last ? qsTr("This is the last step. Delete %1 for good? This cannot be undone.").arg(what)
                         : qsTr("Delete %1 for good (%2 of them folders with their content), among them %3? This cannot be undone.").arg(what).arg(c.folders).arg(c.names.join(", "))
            if (c.truncatedNotice) t += " " + qsTr("Only the newest items are listed here; older ones are not affected.")
            return t
        }
        if (c.kind === "delete_folder")
            return last ? qsTr("This is the last step. Delete the folder %1 for good? This cannot be undone.").arg(c.names[0])
                        : qsTr("Delete the folder %1 and everything in it for good? This cannot be undone.").arg(c.names[0])
        return qsTr("Delete %1 for good? This cannot be undone.").arg(c.names[0])
    }
    function showStep(c) {
        if (c.started) return   // the last confirmation was given: the deletion runs, nothing more to ask
        if (c.errorKey) { problemDialog.text = Problems.title(c.errorKey) + " " + Problems.action(c.errorKey); problemDialog.open(); return }
        deleteConfirm.text = page.deleteText(c)
        deleteConfirm.open()
    }
    function resultText() {
        var r = trash.result
        if (r && r.kind === "delete") {
            if (r.ok) return qsTr("Deleted for good.")
            if (r.outcomeUnknown) return Problems.title("outcome_unknown") + " " + qsTr("Refresh to see what happened.")
            var d = Problems.title(r.key) + " " + Problems.action(r.key)
            if (r.code && r.code.length > 0) d += " (" + qsTr("Code %1").arg(r.code) + ")"
            return d
        }
        if (r && r.kind === "empty") {
            var e = qsTr("%n item(s) deleted for good.", "", r.removed)
            if (r.notRemoved > 0) e += " " + qsTr("%n could not be deleted.", "", r.notRemoved)
            if (r.notTried > 0) e += " " + qsTr("%n were not tried.", "", r.notTried)
            if (r.outcomeUnknown) e += " " + Problems.title("outcome_unknown") + " " + qsTr("Refresh to see what happened.")
            else if (r.cancelled) e += " " + qsTr("Stopped.")
            else if (r.key && r.key.length > 0) e += " " + Problems.title(r.key) + " " + Problems.action(r.key)
            return e
        }
        if (!r || r.kind !== "restore") return ""
        if (r.ok && r.landedElsewhere) return qsTr("Restored, but the server put it under another name. Open the folder to see it.")
        if (r.ok) return qsTr("Restored.")
        if (r.outcomeUnknown) return Problems.title("outcome_unknown") + " " + qsTr("Refresh to see what happened.")
        var t = Problems.title(r.key) + " " + Problems.action(r.key)
        if (r.code && r.code.length > 0) t += " (" + qsTr("Code %1").arg(r.code) + ")"
        return t
    }

    TopBar {
        id: topBar
        title: qsTr("Trash")
        leftItems: [ IconButton { objectName: "trashBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: page.leave() } ]
    }

    Column {
        id: header
        anchors { left: parent.left; right: parent.right; top: topBar.bottom }
        Text {
            objectName: "trashChosen"
            x: 30
            width: parent.width - 60
            topPadding: 14
            elide: Text.ElideMiddle
            visible: page.listShown() && trash.chosenName.length > 0
            font.pixelSize: Style.bodySize
            font.bold: true
            color: Style.ink
            text: qsTr("Chosen: %1").arg(trash.chosenName)
        }
        Text {
            objectName: "trashAge"
            x: 30
            width: parent.width - 60
            topPadding: 10
            wrapMode: Text.Wrap
            visible: page.listShown()
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: page.ageLine()
        }
        Text {
            objectName: "trashMessage"
            x: 30
            width: parent.width - 60
            topPadding: 10
            wrapMode: Text.Wrap
            visible: text.length > 0 && trash.state !== "explainOff" && trash.state !== "explainUnknown"
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: page.messageText().length > 0 ? "✗ " + page.messageText() : ""
        }
        Text {
            objectName: "trashResult"
            x: 30
            width: parent.width - 60
            topPadding: 10
            wrapMode: Text.Wrap
            visible: page.listShown() && text.length > 0
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: page.resultText()
        }
        Text {
            objectName: "trashProgress"
            x: 30
            width: parent.width - 60
            topPadding: 10
            wrapMode: Text.Wrap
            visible: trash.busy && trash.progress.total > 1
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: qsTr("Deleting %1 of %2").arg(trash.progress.done).arg(trash.progress.total)
        }
        Text {
            objectName: "trashReason"
            x: 30
            width: parent.width - 60
            topPadding: 10
            wrapMode: Text.Wrap
            visible: page.listShown() && trash.chosenName.length > 0 && !trash.canAct
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: qsTr("Restoring needs a connection.")
        }
        Text {
            objectName: "trashNote"
            x: 30
            width: parent.width - 60
            topPadding: 8
            bottomPadding: 10
            wrapMode: Text.Wrap
            visible: page.listShown() && trash.truncated
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: qsTr("Only the newest %1 items are shown. More exist.").arg(trash.total)
        }
        Rectangle { width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
    }

    Item {
        id: listArea
        anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: pager.top }
        clip: true
        onHeightChanged: trash.pageSize = Math.max(3, Math.floor(height / Style.rowHeight))

        Column {
            width: parent.width
            Repeater {
                model: page.listShown() ? trash.rows : []
                delegate: Item {
                    objectName: "trashRow"
                    width: parent.width
                    height: Style.rowHeight
                    opacity: modelData.actionable ? 1.0 : 0.55
                    Rectangle { anchors.fill: parent; color: Style.pressed; visible: tap.pressed || modelData.chosen }
                    Icon {
                        id: kindIcon
                        x: 30
                        anchors.verticalCenter: parent.verticalCenter
                        size: 48
                        source: modelData.isFolder ? "folder" : "list"
                    }
                    Column {
                        anchors { left: kindIcon.right; leftMargin: 24; right: mark.left; rightMargin: 12; verticalCenter: parent.verticalCenter }
                        spacing: 4
                        Text {
                            objectName: "trashRowName"
                            width: parent.width
                            elide: Text.ElideMiddle
                            font.pixelSize: Style.bodySize
                            font.bold: modelData.isFolder
                            color: Style.ink
                            text: modelData.name
                        }
                        Text {
                            objectName: "trashRowCaption"
                            width: parent.width
                            elide: Text.ElideRight
                            font.pixelSize: Style.captionSize
                            color: Style.ink
                            text: [modelData.originalText, modelData.deletedText, modelData.sizeText].filter(function (p) { return p.length > 0 }).join(" · ")
                        }
                    }
                    // the one chosen item: a box at the right, filled with a check mark
                    Rectangle {
                        id: mark
                        objectName: "trashCheck"
                        anchors { right: parent.right; rightMargin: 24; verticalCenter: parent.verticalCenter }
                        width: 44
                        height: 44
                        color: Style.paper
                        border.width: 3
                        border.color: Style.ink
                        Icon { anchors.centerIn: parent; size: 36; source: "check"; visible: modelData.chosen === true }
                    }
                    RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
                    MouseArea {
                        id: tap
                        anchors.fill: parent
                        onClicked: trash.choose(modelData.index)
                    }
                }
            }
        }
        Text {
            objectName: "trashEmpty"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 60 }
            wrapMode: Text.Wrap
            visible: page.listShown() && trash.total === 0
            font.pixelSize: Style.bodySize
            color: Style.ink
            text: qsTr("The trash is empty.")
        }
        Text {
            objectName: "trashBusy"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 60 }
            wrapMode: Text.Wrap
            visible: trash.state === "checking" || trash.state === "loading"
            font.pixelSize: Style.bodySize
            font.bold: true
            color: Style.ink
            text: qsTr("Loading...")
        }
        // the trash app is off or its answer is unclear: the explanation takes the place of the list, and no tool of the trash is offered
        Text {
            objectName: "trashExplain"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 60 }
            wrapMode: Text.Wrap
            visible: trash.state === "explainOff" || trash.state === "explainUnknown"
            font.pixelSize: Style.bodySize
            color: Style.ink
            text: page.explainText()
        }
        Text {
            objectName: "trashFailed"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 60 }
            wrapMode: Text.Wrap
            visible: trash.state === "failed"
            font.pixelSize: Style.bodySize
            font.bold: true
            color: Style.ink
            text: page.messageText()
        }
    }

    ModelPager {
        id: pager
        anchors { horizontalCenter: parent.horizontalCenter; bottom: panel.top }
        page: trash.page
        pageCount: trash.pageCount
        visible: page.listShown() && trash.pageCount > 1
        height: visible ? Style.touchMin : 0
        onPrevious: trash.previousPage()
        onNext: trash.nextPage()
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton {
                objectName: "trashRestore"
                text: qsTr("Restore")
                icon: "undo"
                visible: trash.state === "listed"
                enabled: trash.canAct && trash.chosenName.length > 0 && !trash.busy
                onClicked: trash.restore()
            },
            ToolButton {
                objectName: "trashDelete"
                text: qsTr("Delete for good")
                icon: "trash"
                visible: trash.deleteEnabled && trash.state === "listed"
                enabled: trash.canAct && trash.chosenName.length > 0 && !trash.busy
                onClicked: page.showStep(trash.beginDelete())
            },
            ToolButton {
                objectName: "trashEmptyAll"
                text: qsTr("Empty trash")
                icon: "minus-circle"
                visible: trash.deleteEnabled && trash.state === "listed"
                enabled: trash.canAct && trash.total > 0 && !trash.busy
                onClicked: page.showStep(trash.beginEmpty())
            },
            ToolButton {
                objectName: "trashCancelRun"
                text: qsTr("Cancel")
                icon: "close"
                visible: trash.busy && trash.progress.total > 1
                onClicked: trash.cancelRun()
            },
            ToolButton {
                objectName: "trashRefresh"
                text: qsTr("Refresh")
                icon: "sync"
                enabled: !trash.busy
                onClicked: trash.refresh()
            }
        ]
    }

    Connections {
        target: trash
        // the original place was looked at: free = a confirmation, anything else = the user chooses a folder
        function onRestorePlan(plan, proposal) {
            page.restoreName = proposal.names.length > 0 ? proposal.names[0] : ""
            if (plan === "original_free") {
                page.restoreMode = "original"
                page.restorePlace = proposal.place
                restoreConfirm.open()
            } else {
                takenDialog.text = plan === "choose_place"
                    ? qsTr("%1 has no known original place. Choose a folder.").arg(page.restoreName)
                    : qsTr("%1 cannot go back to its original place: the place is taken or the folder is gone. Choose another folder.").arg(page.restoreName)
                takenDialog.open()
            }
        }
        // the chosen place was looked at
        function onTargetChecked(answer) {
            if (answer.errorKey) { problemDialog.text = Problems.title(answer.errorKey) + " " + Problems.action(answer.errorKey); problemDialog.open(); return }
            if (answer.needsName) {
                nameEntry.open(answer.name)
                nameEntry.errorText = qsTr("This name is taken there. Choose another name.")
                return
            }
            if (answer.needsConfirm) {
                page.restoreMode = "chosen"
                page.restorePlace = answer.summary.place
                restoreConfirm.open()
            }
        }
    }

    ConfirmDialog {
        id: restoreConfirm
        objectName: "trashRestoreConfirm"
        title: qsTr("Restore")
        acceptText: qsTr("Restore")
        text: page.restoreText()
        onAccepted: { if (page.restoreMode === "original") trash.restoreConfirmed(); else trash.restoreChosenConfirmed() }
        onRejected: trash.cancelStep()
    }

    // the original place cannot be used: choose another folder (no replace exists)
    ConfirmDialog {
        id: takenDialog
        objectName: "trashTakenDialog"
        title: qsTr("Restore")
        acceptText: qsTr("Choose folder")
        onAccepted: page.chooseFolder()
        onRejected: trash.cancelStep()
    }

    TextEntryDialog {
        id: nameEntry
        objectName: "trashNameEntry"
        title: qsTr("New name")
        acceptText: qsTr("OK")
        onAccepted: function (text) {
            var r = trash.setTargetName(text)
            if (r.errorKey && r.errorKey.length > 0) { errorText = Problems.title(r.errorKey) + " " + Problems.action(r.errorKey); return }
            close()
        }
        onRejected: trash.cancelStep()
    }

    // the irreversible steps: one confirmation for a file, two for a folder and for Empty trash; every one says what is lost and defaults to Cancel
    ConfirmDialog {
        id: deleteConfirm
        objectName: "trashDeleteConfirm"
        title: qsTr("Delete for good")
        acceptText: qsTr("Delete for good")
        destructive: true
        defaultCancel: true
        onAccepted: page.showStep(trash.confirmStep())   // the next confirmation, or the start of the deletion ({started: true} shows nothing more)
        onRejected: trash.cancelStep()
    }

    ConfirmDialog {
        id: problemDialog
        objectName: "trashProblemDialog"
        title: qsTr("Restore")
        showCancel: false
        onAccepted: trash.cancelStep()
    }
}
