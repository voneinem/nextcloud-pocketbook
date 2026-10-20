import QtQuick
import QtQuick.Controls
import ".."

// Update of what is kept offline (never automatic): the page starts it when it opens, because the tap that opened it was the request.
// It tells what was checked, what was fetched, what needs the user's decision (a copy changed on both sides) and what has vanished
// on the server (the copy stays on the reader).
Item {
    id: page
    property var report: transfer.updateReport
    property bool autoStart: false   // the tap that opens the page is the request; a page created for any other reason starts nothing

    function stoppedText() {
        var key = report.stoppedBy
        if (!key || key.length === 0) return ""
        if (key === "cancelled") return qsTr("The update was stopped.")
        return (Problems.title(key) + " " + Problems.action(key)).trim()
    }
    function nameOf(path) { return transfer.details(path).name }

    Component.onCompleted: if (autoStart && !transfer.updateRunning && !transfer.jobBusy) transfer.update()

    TopBar {
        id: topBar
        title: qsTr("Update")
        leftItems: [ IconButton { objectName: "updateBack"; text: qsTr("Back"); icon: "chevron-left"; enabled: !transfer.updateRunning; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
    }

    Flickable {
        anchors { left: parent.left; right: parent.right; top: topBar.bottom; bottom: panel.top }
        contentWidth: width
        contentHeight: column.implicitHeight + 40
        boundsBehavior: Flickable.StopAtBounds
        interactive: false
        clip: true

        Column {
            id: column
            width: parent.width

            Text {
                objectName: "updateStatus"
                x: 30
                width: parent.width - 60
                topPadding: 24
                bottomPadding: 10
                wrapMode: Text.Wrap
                font.pixelSize: Style.bodySize
                font.bold: true
                color: Style.ink
                text: {
                    switch (transfer.updateStage) {
                    case "checking": return qsTr("Checking what you keep offline...")
                    case "confirm": return qsTr("Waiting for your answer.")
                    case "downloading": return qsTr("Downloading: %1 of %2 files").arg(transfer.updateDone).arg(transfer.updateTotal)
                    case "done": return page.stoppedText().length > 0 ? page.stoppedText() : qsTr("The update is finished.")
                    default: return ""
                    }
                }
            }
            Text {
                objectName: "updateNothingKept"
                x: 30
                width: parent.width - 60
                wrapMode: Text.Wrap
                visible: transfer.updateStage === "done" && report.files === 0 && report.folders === 0 && !(report.stoppedBy && report.stoppedBy.length > 0)
                font.pixelSize: Style.bodySize
                color: Style.ink
                text: qsTr("Nothing is kept offline yet. Open a folder or a file and tap Keep offline.")
            }
            Column {
                width: parent.width
                visible: transfer.updateStage === "done" && report.files !== undefined
                InkRow { objectName: "updateChecked"; text: qsTr("Files checked"); value: String(report.files) }
                InkRow { objectName: "updateAdded"; text: qsTr("New"); value: String(report.added) }
                InkRow { objectName: "updateUpdated"; text: qsTr("Updated"); value: String(report.updated) }
                InkRow { objectName: "updateSkipped"; text: qsTr("Unchanged"); value: String(report.skipped) }
                InkRow { objectName: "updateFailed"; visible: report.failed > 0; text: qsTr("Failed"); value: String(report.failed) }
                InkRow { objectName: "updateRemoved"; visible: report.removed > 0; text: qsTr("Removed on the server"); value: String(report.removed) }
                InkRow { objectName: "updateUnchecked"; visible: report.uncheckedFolders > 0; text: qsTr("Folders not checked"); value: String(report.uncheckedFolders) }
            }
            Text {
                x: 30
                width: parent.width - 60
                topPadding: 16
                wrapMode: Text.Wrap
                visible: transfer.updateStage === "done" && report.needDecision > 0
                font.pixelSize: Style.bodySize
                font.bold: true
                color: Style.ink
                text: qsTr("You decide: you and the server both changed these.")
            }
            Repeater {
                model: transfer.updateStage === "done" ? report.decisionPaths : []
                delegate: InkRow {
                    objectName: "updateDecision"
                    text: page.nameOf(modelData)
                    chevron: true
                    onClicked: page.StackView.view.push(Qt.resolvedUrl("ConflictPage.qml"), { remotePath: modelData })
                }
            }
            Text {
                x: 30
                width: parent.width - 60
                topPadding: 16
                wrapMode: Text.Wrap
                visible: transfer.updateStage === "done" && report.removed > 0
                font.pixelSize: Style.bodySize
                font.bold: true
                color: Style.ink
                text: qsTr("These are no longer on the server. The copy stays on the reader.")
            }
            Repeater {
                model: transfer.updateStage === "done" ? report.removedPaths : []
                delegate: InkRow {
                    objectName: "updateRemovedItem"
                    text: page.nameOf(modelData)
                    chevron: true
                    onClicked: page.StackView.view.push(Qt.resolvedUrl("DetailsPage.qml"), { remotePath: modelData })
                }
            }
        }
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton {
                objectName: "updateCancel"
                text: qsTr("Cancel")
                icon: "close"
                visible: transfer.updateRunning
                onClicked: transfer.cancelUpdate()
            },
            ToolButton {
                objectName: "updateAgain"
                text: qsTr("Update again")
                icon: "sync"
                visible: !transfer.updateRunning
                enabled: !transfer.jobBusy
                onClicked: transfer.update()
            }
        ]
    }

    Connections {
        target: transfer
        function onUpdateNeedsConfirm(bytesText, freeText) {
            confirm.text = qsTr("The update will download %1. %2 are free on the reader. Continue?").arg(bytesText).arg(freeText)
            confirm.open()
        }
    }

    ConfirmDialog {
        id: confirm
        objectName: "updateConfirm"
        title: qsTr("Large update")
        acceptText: qsTr("Download")
        onAccepted: transfer.confirmUpdate(true)
        onRejected: transfer.confirmUpdate(false)
    }
}
