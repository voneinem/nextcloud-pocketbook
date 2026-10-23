import QtQuick
import QtQuick.Controls
import ".."

// Review before anything changes (specs/003-folder-sync US2): what a sync of one pair would do, from the comparison just made. Nothing here has
// changed a file; Approve is the only thing that starts a transfer. The user can take entries out of the plan (they come back at the next sync), cancel
// it, or compare again. A plan of 0 is the offline preview: what the stored listings say, not current, never approvable.
Item {
    id: review
    property int planId: 0
    property int pairId: 0
    property var head: ({})
    property string section: "transfers"
    property int pageIndex: 0
    property var rows: []
    property int pages: 1
    property bool started: false       // Approve was tapped on this page: the result replaces it

    function reload() {
        head = sync.plan(planId)
        pages = sync.planPageCount(planId, section)
        if (pageIndex >= pages) pageIndex = Math.max(0, pages - 1)
        rows = sync.planPage(planId, section, pageIndex)
    }
    function setSection(key) { section = key; pageIndex = 0; reload() }
    function count(key) {
        if (!head.totals) return 0
        var t = head.totals
        switch (key) {
        case "transfers": return t.downloads + t.uploads + t.keptBoth
        case "skips": return t.skipped
        default: return t.proposalsServer + t.proposalsReader
        }
    }
    function iconFor(row) {
        switch (row.action) {
        case "download": return "arrow-down"
        case "upload": return "arrow-up"
        case "keep_both": return "arrows-both"
        case "propose_delete_server":
        case "propose_delete_reader": return "trash"
        default: return "minus-circle"
        }
    }
    function actionWord(row) {
        switch (row.action) {
        case "download": return qsTr("Download")
        case "upload": return qsTr("Upload")
        case "keep_both": return qsTr("Keep both")
        case "propose_delete_server":
        case "propose_delete_reader": return qsTr("Deletion proposal")
        default: return qsTr("Skipped")
        }
    }
    function proposalWord(row) {
        // propose_delete_server: the server still has it, the reader lost it
        var gone = row.action === "propose_delete_server" ? qsTr("Deleted on the reader") : qsTr("Deleted on the server")
        return row.folder ? gone + ": " + qsTr("a folder with %n synced file(s)", "", row.recordsBelow) : gone
    }
    function choiceWord(row, choice) {
        switch (choice) {
        case "transfer_again": return row.action === "propose_delete_server" ? qsTr("Copy it back") : qsTr("Send it to the server again")
        case "leave": return qsTr("Leave as it is")
        case "delete": return row.action === "propose_delete_server" ? qsTr("Delete on the server (goes to the trash)") : qsTr("Delete on the reader")
        default: return qsTr("No choice yet")
        }
    }
    function caption(row) {
        if (row.action === "propose_delete_server" || row.action === "propose_delete_reader") {
            var pw = [proposalWord(row)]
            if (row.sizeText.length > 0) pw.push(row.sizeText)
            pw.push(row.choice.length > 0 ? choiceWord(row, row.choice) : qsTr("No choice yet"))
            return pw.join("  ·  ")
        }
        var parts = [actionWord(row)]
        if (row.sizeText.length > 0) parts.push(row.sizeText)
        if (row.removed) parts.push(qsTr("Taken out of the plan"))
        else if (row.unapprovable.length > 0) parts.push(Problems.title(row.unapprovable))
        else if (row.reason.length > 0) parts.push(Problems.title(row.reason))
        if (row.localNameText.length > 0) parts.push(qsTr("on the reader as \"%1\"").arg(row.localNameText))
        if (row.action === "keep_both") {
            if (row.copyReader.length > 0 && row.copyServer.length > 0) parts.push(qsTr("adds \"%1\" on the reader and \"%2\" on the server").arg(row.copyReader).arg(row.copyServer))
            else if (row.copyReader.length > 0) parts.push(qsTr("adds \"%1\" on the reader").arg(row.copyReader))
            else if (row.copyServer.length > 0) parts.push(qsTr("adds \"%1\" on the server").arg(row.copyServer))
        }
        return parts.join("  ·  ")
    }
    function ageLine() {
        if (!head.ageKind) return ""
        switch (head.ageKind) {
        case "just_now": return qsTr("Compared just now.")
        case "minutes": return qsTr("Compared %n minute(s) ago.", "", head.ageCount)
        case "hours": return qsTr("Compared %n hour(s) ago.", "", head.ageCount)
        case "days": return qsTr("Compared %n day(s) ago.", "", head.ageCount)
        default: return ""
        }
    }
    function totalsLine() {
        if (!head.totals) return ""
        var t = head.totals, parts = []
        if (t.downloads > 0) parts.push(qsTr("%n download(s)", "", t.downloads) + " (" + head.sizes.downloadText + ")")
        if (t.uploads > 0) parts.push(qsTr("%n upload(s)", "", t.uploads) + " (" + head.sizes.uploadText + ")")
        if (t.keptBoth > 0) parts.push(qsTr("%n kept both", "", t.keptBoth))
        if (t.skipped > 0) parts.push(qsTr("%n skipped", "", t.skipped))
        if (parts.length === 0) return qsTr("Nothing to do: everything is in sync.")
        if (t.inSync > 0) parts.push(qsTr("%n in sync", "", t.inSync))
        return parts.join(", ")
    }
    function spaceLine() {
        if (!head.freeText) return ""
        return qsTr("Free on the reader: %1 (a reserve of %2 is kept).").arg(head.freeText).arg(head.reserveText)
    }
    function reasonLine() {
        if (head.approvable !== false) return ""
        var key = head.approveErrorKey
        if (key === "over_limit" && head.overLimitText && head.overLimitText.length > 0) return head.overLimitText + " " + Problems.action(key)
        if (key === "plan_stale") return qsTr("Compared %1 ago. Compare again before approving.").arg(ageShort())
        return Problems.title(key) + " " + Problems.action(key)
    }
    function ageShort() {
        switch (head.ageKind) {
        case "minutes": return qsTr("%n minute(s)", "", head.ageCount)
        case "hours": return qsTr("%n hour(s)", "", head.ageCount)
        case "days": return qsTr("%n day(s)", "", head.ageCount)
        default: return qsTr("a moment")
        }
    }
    property var choiceRow: null
    property var deleteQueue: []
    function applyAsk() {
        var ask = sync.applyChoices(planId, false)
        if (!ask.ok) { notice.text = Problems.title(ask.errorKey) + " " + Problems.action(ask.errorKey); notice.open(); return }
        if (ask.deletes.length === 0 && ask.transfers === 0 && ask.leaves === 0) { notice.text = qsTr("Choose what to do with at least one proposal first."); notice.open(); return }
        deleteQueue = ask.deletes
        nextDelete()
    }
    // one confirmation for every file that would be deleted, naming the file and the side; there is no "delete all"
    function nextDelete() {
        if (deleteQueue.length === 0) { applyNow(); return }
        var d = deleteQueue[0]
        confirmDelete.text = d.folder ? qsTr("Delete the folder \"%1\" and its synced files on the %2? Files that changed or are new stay.").arg(d.nameText).arg(d.side === "server" ? qsTr("server") : qsTr("reader"))
            : d.side === "server" ? qsTr("Delete \"%1\" on the server? It goes to the server's trash.").arg(d.nameText)
            : qsTr("Delete \"%1\" from the reader? It cannot be recovered with this app.").arg(d.nameText)
        confirmDelete.open()
    }
    function applyNow() {
        var r = sync.applyChoices(planId, true)
        if (!r.ok) { notice.text = Problems.title(r.errorKey) + " " + Problems.action(r.errorKey); notice.open(); return }
        started = true
    }
    function tapRow(row) {
        if (section === "proposals" && planId !== 0 && (head.state === "ready" || head.state === "done") && row.state === "planned") {
            choiceRow = row
            var list = []
            for (var i = 0; i < row.allowed.length; ++i) list.push({ label: choiceWord(row, row.allowed[i]), value: row.allowed[i] })
            list.push({ label: choiceWord(row, ""), value: "" })
            proposalChoice.choices = list
            proposalChoice.current = row.choice
            proposalChoice.open()
            return
        }
        if (section !== "transfers" || planId === 0 || head.state !== "ready") return
        var r = sync.removeEntry(planId, row.seq, !row.removed)
        if (!r.ok) { notice.text = Problems.title(r.errorKey) + " " + Problems.action(r.errorKey); notice.open(); return }
        reload()
    }

    Component.onCompleted: reload()
    Connections {
        target: sync
        function onChanged() { if (!review.started) review.reload() }
        function onRunFinished(result) {
            if (!review.started) return
            review.StackView.view.replace(Qt.resolvedUrl("ResultPage.qml"), { result: result, pairId: review.pairId })
        }
        function onPlanReady(pairId, newPlanId) {   // "Compare again" came back with a new plan
            if (review.started || pairId !== review.pairId) return
            review.planId = newPlanId
            review.section = "transfers"
            review.pageIndex = 0
            review.reload()
        }
        function onComparisonFailed(pairId, key) {
            if (pairId !== review.pairId) return
            notice.text = Problems.title(key) + " " + Problems.action(key)
            notice.open()
        }
    }

    TopBar {
        id: topBar
        title: qsTr("Review")
        leftItems: [ IconButton { objectName: "planBack"; text: qsTr("Back"); icon: "chevron-left"; enabled: !sync.busy; onClicked: if (review.StackView.view) review.StackView.view.pop() } ]
    }

    Column {
        id: header
        anchors { left: parent.left; right: parent.right; top: topBar.bottom }
        Text {
            objectName: "planBusy"
            x: 30
            width: parent.width - 60
            topPadding: 12
            bottomPadding: 6
            wrapMode: Text.Wrap
            visible: sync.busy
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: (sync.stage === "comparing" ? qsTr("Comparing...") : qsTr("Syncing...")) + (sync.progressText.length > 0 ? "  " + sync.progressText : "")
        }
        Text {
            objectName: "planNothingChanged"
            x: 30
            width: parent.width - 60
            topPadding: 12
            wrapMode: Text.Wrap
            visible: !sync.busy
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: review.head.state === "ready" ? qsTr("Review before anything changes. Nothing has been changed yet.")
                : review.head.state === "preview" ? qsTr("From stored listings, not current. Nothing can be approved from here.") : ""
        }
        Text {
            objectName: "planTotals"
            x: 30
            width: parent.width - 60
            topPadding: 6
            wrapMode: Text.Wrap
            font.pixelSize: Style.bodySize
            font.bold: true
            color: Style.ink
            text: review.totalsLine()
        }
        Text {
            objectName: "planSpace"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            visible: text.length > 0
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: review.spaceLine()
        }
        Text {
            objectName: "planAge"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: review.ageLine()
        }
        Text {
            objectName: "planIncomplete"
            x: 30
            width: parent.width - 60
            topPadding: 6
            wrapMode: Text.Wrap
            visible: review.head.incomplete === true
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: qsTr("%n folder(s) were not compared; deletions are not proposed.", "", review.head.incompleteFolders || 0)
        }
        Text {
            objectName: "planKeepBothNote"
            x: 30
            width: parent.width - 60
            topPadding: 6
            wrapMode: Text.Wrap
            visible: review.head.totals !== undefined && review.head.totals.keptBoth > 0
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: qsTr("Changed on both sides. Both versions are kept: the other version is added next to the file under a new name.")
        }
        Text {
            objectName: "planNotApprovable"
            x: 30
            width: parent.width - 60
            topPadding: 6
            wrapMode: Text.Wrap
            visible: text.length > 0 && review.head.state !== "preview"
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: review.reasonLine()
        }
        // the three sections as a line of flat words: the open one is bold and underlined
        Row {
            x: 30
            topPadding: 10
            spacing: 36
            Repeater {
                model: [{ key: "transfers", label: qsTr("Transfers") }, { key: "skips", label: qsTr("Skipped") }, { key: "proposals", label: qsTr("Proposals") }]
                delegate: Item {
                    objectName: "planTab"
                    visible: modelData.key !== "proposals" || review.count("proposals") > 0
                    property string text: modelData.label + " (" + review.count(modelData.key) + ")" + (review.section === modelData.key ? " *" : "")
                    width: visible ? tabLabel.implicitWidth : 0
                    height: Style.touchMin - 20
                    Text {
                        id: tabLabel
                        anchors.verticalCenter: parent.verticalCenter
                        font.pixelSize: Style.bodySize
                        font.bold: review.section === modelData.key
                        font.underline: review.section === modelData.key
                        color: Style.ink
                        text: modelData.label + " (" + review.count(modelData.key) + ")"
                    }
                    MouseArea { anchors.fill: parent; anchors.margins: -12; onClicked: review.setSection(modelData.key) }
                }
            }
        }
        Rectangle { width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
    }

    Item {
        id: listArea
        anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: pager.top }
        clip: true
        onHeightChanged: sync.planPageSize = Math.max(3, Math.floor(height / Style.rowHeight))

        Column {
            width: parent.width
            Repeater {
                model: review.rows
                delegate: Item {
                    objectName: "planRow"
                    width: parent.width
                    height: Style.rowHeight
                    property string text: modelData.nameText + " " + review.caption(modelData)
                    opacity: modelData.removed ? 0.5 : 1.0
                    Rectangle { anchors.fill: parent; color: Style.pressed; visible: tap.pressed }
                    Icon {
                        id: rowIcon
                        x: 30
                        anchors.verticalCenter: parent.verticalCenter
                        size: 48
                        source: review.iconFor(modelData)
                    }
                    Column {
                        anchors { left: rowIcon.right; leftMargin: 24; right: parent.right; rightMargin: 30; verticalCenter: parent.verticalCenter }
                        spacing: 4
                        Text {
                            width: parent.width
                            elide: Text.ElideMiddle
                            font.pixelSize: Style.bodySize
                            font.strikeout: modelData.removed
                            color: Style.ink
                            text: modelData.nameText
                        }
                        Text {
                            objectName: "planRowCaption"
                            width: parent.width
                            elide: Text.ElideRight
                            font.pixelSize: Style.captionSize
                            color: Style.ink
                            text: review.caption(modelData)
                        }
                    }
                    RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
                    MouseArea { id: tap; anchors.fill: parent; onClicked: review.tapRow(modelData) }
                }
            }
        }
        Text {
            objectName: "planEmpty"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 50 }
            wrapMode: Text.Wrap
            visible: review.rows.length === 0
            font.pixelSize: Style.bodySize
            color: Style.ink
            text: review.section === "transfers" ? qsTr("Nothing would be transferred.") : (review.section === "skips" ? qsTr("Nothing is skipped.") : qsTr("There are no proposals."))
        }
    }

    ModelPager {
        id: pager
        anchors { horizontalCenter: parent.horizontalCenter; bottom: panel.top }
        page: review.pageIndex
        pageCount: review.pages
        visible: review.pages > 1
        height: visible ? Style.touchMin : 0
        onPrevious: { if (review.pageIndex > 0) { review.pageIndex--; review.reload() } }
        onNext: { if (review.pageIndex + 1 < review.pages) { review.pageIndex++; review.reload() } }
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton {
                objectName: "planApprove"
                text: qsTr("Approve")
                icon: "check"
                visible: !sync.busy
                enabled: review.planId !== 0 && review.head.approvable === true
                onClicked: {
                    var r = sync.approve(review.planId)
                    if (!r.ok) { notice.text = Problems.title(r.errorKey) + " " + Problems.action(r.errorKey); notice.open(); return }
                    review.started = true
                }
            },
            ToolButton {
                objectName: "planStop"
                text: qsTr("Stop")
                icon: "close"
                visible: sync.busy && sync.stage !== "comparing"
                onClicked: sync.cancelRun()
            },
            ToolButton {
                objectName: "planCancel"
                text: qsTr("Cancel")
                icon: "close"
                visible: !sync.busy
                onClicked: {
                    sync.cancelPlan(review.planId)
                    if (review.StackView.view) review.StackView.view.pop()
                }
            },
            ToolButton {
                objectName: "planApply"
                text: qsTr("Apply choices")
                icon: "check"
                visible: !sync.busy && review.section === "proposals" && (review.head.state === "ready" || review.head.state === "done")
                onClicked: review.applyAsk()
            },
            ToolButton {
                objectName: "planCompare"
                text: qsTr("Compare again")
                icon: "sync"
                visible: !sync.busy && review.pairId !== 0
                enabled: !sync.offline
                onClicked: sync.compare(review.pairId)
            }
        ]
    }

    ChoiceDialog {
        id: proposalChoice
        objectName: "planProposalChoice"
        title: qsTr("Proposal")
        anchor: panel
        onChosen: function (value) {
            if (!review.choiceRow) return
            var r = sync.setChoice(review.planId, review.choiceRow.seq, value)
            if (!r.ok) { notice.text = Problems.title(r.errorKey) + " " + Problems.action(r.errorKey); notice.open() }
            review.choiceRow = null
            review.reload()
        }
    }

    ConfirmDialog {
        id: confirmDelete
        objectName: "planConfirmDelete"
        title: qsTr("Delete")
        acceptText: qsTr("Delete")
        onAccepted: { review.deleteQueue = review.deleteQueue.slice(1); review.nextDelete() }
        onRejected: review.deleteQueue = []
    }

    ConfirmDialog {
        id: notice
        objectName: "planNotice"
        title: qsTr("Not done")
        showCancel: false
    }
}
