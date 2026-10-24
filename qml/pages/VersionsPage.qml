import QtQuick
import QtQuick.Controls
import ".."

// The older versions of one file (specs/006-trash-and-versions): date and size, newest first. Opening the page is the user's tap on Versions and the only
// thing that reads the server; nothing here starts by itself. One version at a time is chosen; Restore asks for a confirmation that names the file and the
// version and says that the current content is kept as one more version (Cancel is first). When the versions app is off, or its answer is unclear, the page
// says so in plain words and offers no tool.
Item {
    id: page
    objectName: "versionsPage"
    property string remotePath

    function messageText() {
        var m = versions.message
        if (!m || !m.key) return ""
        var t = Problems.title(m.key) + " " + Problems.action(m.key)
        if (m.code && m.code.length > 0) t += " (" + qsTr("Code %1").arg(m.code) + ")"
        return t
    }
    function explainText() {
        if (versions.state === "explainOff")
            return Problems.title("versions_unavailable") + " " + qsTr("Older versions cannot be listed or restored from here.") + " " + Problems.action("versions_unavailable")
        if (versions.state === "explainUnknown") return qsTr("The server's answer was not clear.") + " " + page.messageText()
        return ""
    }
    function listShown() { return versions.state === "listed" || versions.state === "empty" }
    function resultText() {
        var r = versions.result
        if (!r || r.kind !== "restore_version") return ""
        if (r.ok && r.previousNotKept) return qsTr("Restored. The server did not keep the previous content as a version.")
        if (r.ok) return qsTr("Restored. The previous content is kept as a version.")
        if (r.outcomeUnknown) return Problems.title("outcome_unknown") + " " + qsTr("Tap Reload to see what happened.")
        var t = Problems.title(r.key) + " " + Problems.action(r.key)
        if (r.code && r.code.length > 0) t += " (" + qsTr("Code %1").arg(r.code) + ")"
        return t
    }
    function leave() { if (page.StackView.view) page.StackView.view.pop() }
    Component.onCompleted: versions.open(remotePath)
    Component.onDestruction: versions.cancelStep()

    TopBar {
        id: topBar
        title: qsTr("Versions")
        leftItems: [ IconButton { objectName: "versionsBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: page.leave() } ]
    }

    Column {
        id: header
        anchors { left: parent.left; right: parent.right; top: topBar.bottom }
        Text {
            objectName: "versionsFile"
            x: 30
            width: parent.width - 60
            topPadding: 14
            elide: Text.ElideMiddle
            visible: versions.fileName.length > 0
            font.pixelSize: Style.bodySize
            font.bold: true
            color: Style.ink
            text: versions.fileName
        }
        Text {
            objectName: "versionsOffline"
            x: 30
            width: parent.width - 60
            topPadding: 10
            wrapMode: Text.Wrap
            visible: versions.offline
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: qsTr("Versions need a connection.")
        }
        Text {
            objectName: "versionsResult"
            x: 30
            width: parent.width - 60
            topPadding: 10
            wrapMode: Text.Wrap
            visible: text.length > 0
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: page.resultText()
        }
        Text {
            objectName: "versionsMessage"
            x: 30
            width: parent.width - 60
            topPadding: 10
            bottomPadding: 10
            wrapMode: Text.Wrap
            visible: text.length > 0 && versions.state !== "explainOff" && versions.state !== "explainUnknown" && versions.state !== "empty"
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: page.messageText().length > 0 ? "✗ " + page.messageText() : ""
        }
        Text {
            objectName: "versionsNote"
            x: 30
            width: parent.width - 60
            topPadding: 8
            bottomPadding: 10
            wrapMode: Text.Wrap
            visible: page.listShown() && versions.truncated
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: qsTr("Only the newest %1 versions are shown. More exist.").arg(versions.total)
        }
        Rectangle { width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
    }

    Item {
        id: listArea
        anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: pager.top }
        clip: true
        onHeightChanged: versions.pageSize = Math.max(3, Math.floor(height / Style.rowHeight))

        Column {
            width: parent.width
            Repeater {
                model: page.listShown() ? versions.rows : []
                delegate: Item {
                    objectName: "versionsRow"
                    width: parent.width
                    height: Style.rowHeight
                    Rectangle { anchors.fill: parent; color: Style.pressed; visible: tap.pressed || modelData.chosen }
                    Column {
                        anchors { left: parent.left; leftMargin: 30; right: mark.left; rightMargin: 12; verticalCenter: parent.verticalCenter }
                        spacing: 4
                        Text {
                            objectName: "versionsRowDate"
                            width: parent.width
                            elide: Text.ElideRight
                            font.pixelSize: Style.bodySize
                            color: Style.ink
                            text: modelData.dateText + (modelData.future ? " (" + qsTr("in the future") + ")" : "")
                        }
                        Text {
                            objectName: "versionsRowSize"
                            width: parent.width
                            elide: Text.ElideRight
                            font.pixelSize: Style.captionSize
                            color: Style.ink
                            text: modelData.sizeText
                        }
                    }
                    Rectangle {
                        id: mark
                        objectName: "versionsCheck"
                        anchors { right: parent.right; rightMargin: 24; verticalCenter: parent.verticalCenter }
                        width: 44
                        height: 44
                        color: Style.paper
                        border.width: 3
                        border.color: Style.ink
                        Icon { anchors.centerIn: parent; size: 36; source: "check"; visible: modelData.chosen === true }
                    }
                    RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
                    MouseArea { id: tap; anchors.fill: parent; onClicked: versions.choose(modelData.index) }
                }
            }
        }
        Text {
            objectName: "versionsEmpty"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 60 }
            wrapMode: Text.Wrap
            visible: versions.state === "empty"
            font.pixelSize: Style.bodySize
            color: Style.ink
            text: qsTr("No older versions.")
        }
        Text {
            objectName: "versionsBusy"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 60 }
            wrapMode: Text.Wrap
            visible: versions.state === "checking" || versions.state === "loading"
            font.pixelSize: Style.bodySize
            font.bold: true
            color: Style.ink
            text: qsTr("Loading...")
        }
        Text {
            objectName: "versionsExplain"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 60 }
            wrapMode: Text.Wrap
            visible: versions.state === "explainOff" || versions.state === "explainUnknown"
            font.pixelSize: Style.bodySize
            color: Style.ink
            text: page.explainText()
        }
        Text {
            objectName: "versionsFailed"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 60 }
            wrapMode: Text.Wrap
            visible: versions.state === "failed"
            font.pixelSize: Style.bodySize
            font.bold: true
            color: Style.ink
            text: page.messageText()
        }
    }

    ModelPager {
        id: pager
        anchors { horizontalCenter: parent.horizontalCenter; bottom: panel.top }
        page: versions.page
        pageCount: versions.pageCount
        visible: page.listShown() && versions.pageCount > 1
        height: visible ? Style.touchMin : 0
        onPrevious: versions.previousPage()
        onNext: versions.nextPage()
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton {
                objectName: "versionsRestore"
                text: qsTr("Restore")
                icon: "undo"
                visible: versions.state === "listed"
                enabled: versions.canAct && !versions.busy
                onClicked: {
                    var r = versions.restore()
                    if (r.needsConfirm) {
                        confirm.text = qsTr("Restore %1 to the version of %2, %3? The current content is kept as one more version.")
                            .arg(r.summary.names[0]).arg(r.summary.dateText).arg(r.summary.sizeText)
                        confirm.open()
                    }
                }
            },
            ToolButton {
                objectName: "versionsReload"
                text: qsTr("Reload")
                icon: "sync"
                enabled: !versions.busy
                onClicked: versions.reload()
            }
        ]
    }

    ConfirmDialog {
        id: confirm
        objectName: "versionsRestoreConfirm"
        title: qsTr("Restore")
        acceptText: qsTr("Restore")
        onAccepted: versions.restoreConfirmed()
        onRejected: versions.cancelStep()
    }
}
