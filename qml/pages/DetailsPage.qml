import QtQuick
import QtQuick.Controls
import ".."

// One file of the user's Nextcloud: what it is, whether a copy is on the reader and whether the server has a newer version, and the
// download with progress and cancel. A download starts only with a tap; a copy the user changed is never overwritten without asking.
Item {
    id: page
    property string remotePath
    property var info: ({})

    function reload() { info = transfer.details(remotePath) }
    function mine() { return transfer.jobPath === remotePath }
    function jobActive() { return mine() && transfer.jobBusy }
    function failedText() {
        var key = transfer.jobError
        var text = Problems.title(key)
        var act = Problems.action(key)
        if (key === "not_enough_space") {
            var pf = transfer.preflight(remotePath)
            text += " " + qsTr("%1 needed, %2 free.").arg(pf.neededText).arg(pf.freeText)
        }
        return (text + " " + act).trim()
    }
    function startDownload() {
        var pf = transfer.preflight(remotePath)
        if (pf.verdict === "confirm") { largeDialog.sizeText = pf.sizeText; largeDialog.open(); return }
        transfer.download(remotePath, false)
    }

    Component.onCompleted: reload()
    Connections {
        target: transfer
        function onChanged() { page.reload() }
        function onJobFinished(state) {
            page.reload()
            if (state === "blocked" && page.mine()) page.StackView.view.push(Qt.resolvedUrl("ConflictPage.qml"), { remotePath: page.remotePath })
        }
    }

    TopBar {
        id: topBar
        title: info.name !== undefined ? info.name : ""
        leftItems: [ IconButton { objectName: "detailsBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
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

            InkRow { objectName: "detailsSize"; text: qsTr("Size"); value: info.sizeText || qsTr("unknown") }
            InkRow { objectName: "detailsType"; text: qsTr("Type"); value: info.mime || qsTr("unknown") }
            InkRow { objectName: "detailsDate"; text: qsTr("Modified"); value: info.dateText || qsTr("unknown") }
            InkRow { objectName: "detailsOnReader"; text: qsTr("On the reader"); value: info.onReader ? qsTr("Yes") : qsTr("No") }
            InkRow { objectName: "detailsState"; visible: info.kind === "file"; text: qsTr("State"); value: EntryWords.stateWord(info.state) }
            InkRow { objectName: "detailsKept"; visible: info.kind === "file"; text: qsTr("Kept offline"); value: info.kept ? qsTr("Yes") : qsTr("No") }
            InkRow {
                objectName: "detailsServer"
                visible: info.onReader === true
                text: qsTr("Server version")
                value: info.serverMissing ? qsTr("Removed on the server") : (info.serverNewer ? qsTr("Newer") : qsTr("Same"))
            }
            Text {
                objectName: "detailsUnavailable"
                x: 30
                width: parent.width - 60
                topPadding: 20
                wrapMode: Text.Wrap
                visible: info.available === false
                font.pixelSize: Style.bodySize
                font.bold: true
                color: Style.ink
                text: qsTr("This entry is not available.")
            }
            Text {
                objectName: "detailsJob"
                x: 30
                width: parent.width - 60
                topPadding: 24
                wrapMode: Text.Wrap
                visible: text.length > 0
                font.pixelSize: Style.bodySize
                font.bold: true
                color: Style.ink
                text: {
                    if (!page.mine()) return ""
                    switch (transfer.jobState) {
                    case "running": return transfer.jobTotalText.length > 0
                        ? qsTr("Downloading: %1 of %2 (%3 %)").arg(transfer.jobDoneText).arg(transfer.jobTotalText).arg(transfer.jobPercent)
                        : qsTr("Downloading: %1").arg(transfer.jobDoneText)
                    case "waiting": return qsTr("Waiting for the connection. It continues by itself, or tap Cancel.")
                    case "paused": return qsTr("The download is paused at %1. Tap Resume to continue.").arg(transfer.jobDoneText)
                    case "done": return qsTr("Downloaded. It is on the reader now.")
                    case "cancelled": return qsTr("The download was cancelled. Nothing was kept.")
                    case "blocked": return Problems.title(transfer.jobError) + " " + Problems.action(transfer.jobError)
                    case "failed": return page.failedText()
                    default: return ""
                    }
                }
            }
        }
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton {
                objectName: "detailsDownload"
                text: info.onReader ? qsTr("Download again") : qsTr("Download")
                icon: "download"
                visible: info.kind === "file" && info.available !== false && !page.jobActive()
                enabled: !transfer.jobBusy && (!info.onReader || info.serverNewer === true || info.localChanged === true)
                onClicked: page.startDownload()
            },
            ToolButton {
                objectName: "detailsKeep"
                text: info.kept ? qsTr("Kept offline") : qsTr("Keep offline")
                icon: info.kept ? "check" : "download"
                visible: info.kind === "file" && info.available !== false && !page.jobActive()
                enabled: !info.kept || info.keptDirect === true
                onClicked: { transfer.setKept(page.remotePath, "file", !info.keptDirect); page.reload() }
            },
            ToolButton {
                objectName: "detailsUpdate"
                text: qsTr("Update")
                icon: "sync"
                visible: info.kind === "file" && info.kept === true && !page.jobActive()
                enabled: !transfer.jobBusy && !transfer.updateRunning
                onClicked: page.StackView.view.push(Qt.resolvedUrl("UpdatePage.qml"), { autoStart: true })
            },
            ToolButton {
                objectName: "detailsOpen"
                text: qsTr("Open")
                icon: "eye"
                visible: info.canOpen === true && !page.jobActive()
                onClicked: transfer.open(page.remotePath)
            },
            ToolButton {
                objectName: "detailsResume"
                text: qsTr("Resume")
                icon: "sync"
                visible: page.mine() && transfer.jobState === "paused"
                onClicked: {
                    var list = transfer.pausedDownloads
                    for (var i = 0; i < list.length; ++i) if (list[i].remotePath === page.remotePath) transfer.resume(list[i].id)
                }
            },
            ToolButton {
                objectName: "detailsCancel"
                text: qsTr("Cancel")
                icon: "close"
                visible: page.jobActive() || (page.mine() && transfer.jobState === "paused")
                onClicked: transfer.cancel()
            }
        ]
    }

    ConfirmDialog {
        id: largeDialog
        objectName: "detailsLargeDialog"
        property string sizeText
        title: qsTr("Large file")
        text: qsTr("This file is %1. Downloading it can take a long time and uses the battery. Download it?").arg(sizeText)
        acceptText: qsTr("Download")
        onAccepted: transfer.download(page.remotePath, true)
    }
}
