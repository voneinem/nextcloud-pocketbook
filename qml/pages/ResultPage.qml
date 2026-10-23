import QtQuick
import QtQuick.Controls
import ".."

// What a sync did (specs/003-folder-sync US2): the counts, the reasons for what failed, how long it took. The last result of every pair stays available
// from the Sync screen.
Item {
    id: page
    property var result: ({})
    property int pairId: 0

    function stoppedText() {
        var key = result.stoppedBy
        if (!key || key.length === 0) return ""
        if (key === "cancelled") return qsTr("The sync was stopped. What was done stays done; the next sync finishes the rest.")
        return Problems.title(key) + " " + Problems.action(key)
    }
    function countsLine() {
        var line = qsTr("%1 downloaded, %2 uploaded, %3 skipped, %4 kept both, %5 proposals open, %6 failed")
            .arg(result.downloaded || 0).arg(result.uploaded || 0).arg(result.skipped || 0).arg(result.keptBoth || 0).arg(result.proposalsOpen || 0).arg(result.failed || 0)
        if ((result.deleted || 0) > 0) line += ", " + qsTr("%n deleted", "", result.deleted)
        return line
    }
    function timeLine() {
        var s = result.seconds || 0
        var when = result.atText && result.atText.length > 0 ? result.atText : ""
        var took = s < 2 ? qsTr("It took a moment.") : qsTr("It took %n second(s).", "", s)
        return when.length > 0 ? when + "  ·  " + took : took
    }

    TopBar {
        id: topBar
        title: qsTr("Result")
        leftItems: [ IconButton { objectName: "resultBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
    }

    Column {
        anchors { left: parent.left; right: parent.right; top: topBar.bottom; topMargin: 30 }
        spacing: 14
        Text {
            objectName: "resultCounts"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            font.pixelSize: Style.bodySize
            font.bold: true
            color: Style.ink
            text: page.countsLine()
        }
        Text {
            objectName: "resultTime"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: page.timeLine()
        }
        Text {
            objectName: "resultStopped"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            visible: text.length > 0
            font.pixelSize: Style.bodySize
            font.bold: true
            color: Style.ink
            text: page.stoppedText()
        }
        Repeater {
            model: page.result.failures || []
            delegate: Text {
                objectName: "resultFailure"
                x: 30
                width: page.width - 60
                wrapMode: Text.Wrap
                font.pixelSize: Style.captionSize
                color: Style.ink
                text: qsTr("%n file(s): %1", "", modelData.count).arg(Problems.title(modelData.key) + " " + Problems.action(modelData.key))
            }
        }
        Text {
            objectName: "resultQueueNote"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            visible: (page.result.uploaded !== undefined || (page.result.failed || 0) > 0) && queue.openCount > 0 && (page.result.failed || 0) > 0
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: qsTr("Some files wait in the Queue. Open it to decide what happens to them.")
        }
        Text {
            objectName: "resultSkippedNote"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            visible: (page.result.skipped || 0) > 0
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: qsTr("Skipped entries come back in the next plan if they can still be synced.")
        }
    }

    BottomPanel {
        anchors.bottom: parent.bottom
        tools: [
            ToolButton {
                objectName: "resultDone"
                text: qsTr("Done")
                icon: "check"
                onClicked: if (page.StackView.view) page.StackView.view.pop()
            },
            ToolButton {
                objectName: "resultQueue"
                text: qsTr("Queue")
                icon: "list"
                visible: queue.openCount > 0 && (page.result.failed || 0) > 0
                onClicked: if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("QueuePage.qml"))
            },
            ToolButton {
                objectName: "resultAgain"
                text: qsTr("Sync again")
                icon: "sync"
                visible: page.pairId !== 0
                enabled: !sync.busy && !sync.offline
                onClicked: {
                    sync.compare(page.pairId)
                    if (page.StackView.view) page.StackView.view.pop()
                }
            }
        ]
    }
}
