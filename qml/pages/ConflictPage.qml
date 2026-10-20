import QtQuick
import QtQuick.Controls
import ".."

// The copy on the reader was changed, and the server has another version: nothing is chosen for the user. Keep the copy, keep both
// (the server version is saved next to it), or replace the copy after a second confirmation that says what is lost.
Item {
    id: page
    property string remotePath
    property var info: ({})

    function reload() { info = transfer.details(remotePath) }
    Component.onCompleted: reload()
    Connections {
        target: transfer
        function onJobFinished(state) {
            if (state === "done" && page.StackView.view) page.StackView.view.pop()
        }
    }

    TopBar {
        id: topBar
        title: qsTr("Your copy was changed")
        leftItems: [ IconButton { objectName: "conflictBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
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
                x: 30
                width: parent.width - 60
                topPadding: 20
                bottomPadding: 10
                wrapMode: Text.Wrap
                font.pixelSize: Style.bodySize
                color: Style.ink
                text: qsTr("You changed the copy of this file on the reader, and the server has another version. Nothing is replaced until you choose.")
            }
            InkRow { text: qsTr("Your copy"); value: [info.localSizeText, info.localDateText].filter(function (t) { return t && t.length > 0 }).join("  ·  ") }
            InkRow { text: qsTr("Server version"); value: [info.sizeText, info.dateText].filter(function (t) { return t && t.length > 0 }).join("  ·  ") }
            Text {
                objectName: "conflictAnswer"
                x: 30
                width: parent.width - 60
                topPadding: 20
                wrapMode: Text.Wrap
                visible: transfer.jobBusy || (transfer.jobState === "failed" && transfer.jobPath === page.remotePath)
                font.pixelSize: Style.bodySize
                font.bold: true
                color: Style.ink
                text: transfer.jobBusy ? qsTr("Downloading...") : Problems.title(transfer.jobError) + " " + Problems.action(transfer.jobError)
            }
        }
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton {
                objectName: "conflictKeepMine"
                text: qsTr("Keep my copy")
                icon: "check"
                enabled: !transfer.jobBusy
                onClicked: { transfer.resolveConflict(page.remotePath, "keep_mine"); if (page.StackView.view) page.StackView.view.pop() }
            },
            ToolButton {
                objectName: "conflictKeepBoth"
                text: qsTr("Keep both")
                icon: "copy"
                enabled: !transfer.jobBusy
                onClicked: transfer.resolveConflict(page.remotePath, "keep_both")
            },
            ToolButton {
                objectName: "conflictReplace"
                text: qsTr("Replace my copy")
                icon: "trash"
                enabled: !transfer.jobBusy
                onClicked: replaceDialog.open()
            }
        ]
    }

    ConfirmDialog {
        id: replaceDialog
        objectName: "conflictReplaceDialog"
        title: qsTr("Replace your copy?")
        text: qsTr("Your copy (%1, changed %2) will be lost and replaced by the version from the server.").arg(info.localSizeText || "").arg(info.localDateText || "")
        acceptText: qsTr("Replace")
        onAccepted: transfer.resolveConflict(page.remotePath, "replace")
    }
}
