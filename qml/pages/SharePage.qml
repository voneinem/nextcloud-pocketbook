import QtQuick
import QtQuick.Controls
import ".."

// Share links of one file or folder (specs/004-share-links): what is already shared, page by page - public links, and shares with users and groups
// (read-only: this app does not create, change or remove them). Nothing here starts by itself: the list is read when the user opened Share and is not kept
// after the screen is left. A link is never shown in the list; the link screen shows it when the user taps the link.
Item {
    id: page
    objectName: "sharePage"
    property var summaryRow: null

    // the plain-language texts of the server's rules and of a refusal (qml/Problems.qml); the limit in days and the short code are added here
    function messageText() {
        var m = share.message
        if (!m || !m.key) return ""
        var t = Problems.title(m.key)
        if (m.limitDays > 0 && (m.fieldMask & 2) !== 0)
            t += " " + qsTr("At most %n days from today.", "", m.limitDays)
        t += " " + Problems.action(m.key)
        if (m.code && m.code.length > 0) t += " (" + qsTr("Code %1").arg(m.code) + ")"
        return t
    }
    // a rule the server states in its capabilities, shown as a note when no refusal was needed to learn it
    function ruleNote() {
        if (share.message && share.message.key) return ""
        if (share.state !== "listed") return ""
        if (!share.rules.sharingOffered) return Problems.title("share_not_offered")
        if (!share.rules.linkCreationOffered) return Problems.title("share_public_disabled") + " " + Problems.action("share_public_disabled")
        return ""
    }
    function noteText() {
        var notes = []
        if (!share.itemShareable) notes.push(share.unavailableReason)
        if (share.truncated) notes.push(Problems.title("share_list_too_big") + " " + Problems.action("share_list_too_big"))
        if (share.inheritedNote === "inherited_failed") notes.push(qsTr("Shares from parent folders could not be read."))
        if (share.staleNote) notes.push(qsTr("The list may be out of date. Tap Refresh."))
        if (share.hasExistingLink && share.state === "listed") notes.push(qsTr("This item already has a public link. A new link is a second one."))
        return notes.join("\n")
    }
    function iconFor(kind) {
        switch (kind) {
        case "link": return "slash-link"
        default: return "info"
        }
    }
    function canCreate() {
        return share.state === "listed" && share.itemShareable && !share.offline && share.rules.linkCreationOffered === true
    }
    function leave() {
        if (page.StackView.view) page.StackView.view.pop()
        share.clearSecrets()
    }
    Component.onDestruction: share.clearSecrets()

    TopBar {
        id: topBar
        title: qsTr("Share")
        leftItems: [ IconButton { objectName: "shareBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: page.leave() } ]
    }

    Column {
        id: header
        anchors { left: parent.left; right: parent.right; top: topBar.bottom }
        Text {
            objectName: "shareItemName"
            x: 30
            width: parent.width - 60
            topPadding: 14
            elide: Text.ElideMiddle
            font.pixelSize: Style.bodySize
            font.bold: true
            color: Style.ink
            text: share.itemName
        }
        Text {
            objectName: "shareOffline"
            x: 30
            width: parent.width - 60
            topPadding: 10
            wrapMode: Text.Wrap
            visible: share.offline
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: qsTr("Sharing needs a connection.")
        }
        Text {
            objectName: "shareMessage"
            x: 30
            width: parent.width - 60
            topPadding: 10
            wrapMode: Text.Wrap
            visible: text.length > 0
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: page.messageText().length > 0 ? "✗ " + page.messageText() : page.ruleNote()
        }
        Text {
            objectName: "shareNote"
            x: 30
            width: parent.width - 60
            topPadding: 8
            bottomPadding: 10
            wrapMode: Text.Wrap
            visible: text.length > 0
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: page.noteText()
        }
        Rectangle { width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
    }

    Item {
        id: listArea
        anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: pager.top }
        clip: true
        onHeightChanged: share.pageSize = Math.max(3, Math.floor(height / Style.rowHeight))

        Column {
            width: parent.width
            Repeater {
                model: share.rows
                delegate: Item {
                    objectName: "shareRow"
                    width: parent.width
                    height: Style.rowHeight
                    Rectangle { anchors.fill: parent; color: Style.pressed; visible: tap.pressed }
                    Icon {
                        id: kindIcon
                        x: 30
                        anchors.verticalCenter: parent.verticalCenter
                        size: 48
                        source: page.iconFor(modelData.kind)
                    }
                    Column {
                        anchors { left: kindIcon.right; leftMargin: 24; right: chev.left; rightMargin: 12; verticalCenter: parent.verticalCenter }
                        spacing: 4
                        Text {
                            objectName: "shareRowTitle"
                            width: parent.width
                            elide: Text.ElideRight
                            font.pixelSize: Style.bodySize
                            font.bold: modelData.canOpen
                            color: Style.ink
                            text: modelData.title
                        }
                        Text {
                            objectName: "shareRowDetail"
                            width: parent.width
                            elide: Text.ElideRight
                            font.pixelSize: Style.captionSize
                            color: Style.ink
                            text: modelData.detailLine
                        }
                    }
                    Icon {
                        id: chev
                        anchors { right: parent.right; rightMargin: 24; verticalCenter: parent.verticalCenter }
                        size: 40
                        visible: modelData.canOpen
                        width: visible ? 40 : 0
                        source: visible ? "chevron-right" : ""
                    }
                    RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
                    MouseArea {
                        id: tap
                        anchors.fill: parent
                        onClicked: {
                            if (modelData.canOpen) { share.showLink(modelData.id); return }   // the link screen opens when the link has arrived (linkOpened)
                            page.summaryRow = modelData
                            summary.text = modelData.title + "\n" + modelData.detailLine
                            summary.open()
                        }
                    }
                }
            }
        }
        Text {
            objectName: "shareEmpty"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 60 }
            wrapMode: Text.Wrap
            visible: share.state === "listed" && share.total === 0
            font.pixelSize: Style.bodySize
            color: Style.ink
            text: qsTr("Nothing is shared.")
        }
        Text {
            objectName: "shareBusy"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 60 }
            wrapMode: Text.Wrap
            visible: share.state === "loading"
            font.pixelSize: Style.bodySize
            color: Style.ink
            text: qsTr("Loading...")
        }
    }

    ModelPager {
        id: pager
        anchors { horizontalCenter: parent.horizontalCenter; bottom: panel.top }
        page: share.page
        pageCount: share.pageCount
        visible: share.pageCount > 1
        height: visible ? Style.touchMin : 0
        onPrevious: share.previousPage()
        onNext: share.nextPage()
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton {
                objectName: "shareCreate"
                text: qsTr("Create link")
                icon: "plus-circle"
                enabled: page.canCreate()
                onClicked: page.StackView.view.push(Qt.resolvedUrl("ShareFormPage.qml"))
            },
            ToolButton {
                objectName: "shareRefresh"
                text: qsTr("Refresh")
                icon: "sync"
                enabled: share.itemShareable && !share.busy
                onClicked: share.refresh()
            }
        ]
    }

    Connections {
        target: share
        function onLinkOpened(id) { page.StackView.view.push(Qt.resolvedUrl("ShareLinkPage.qml")) }
    }

    // a share with a user, a group or of another kind: what it is, nothing that changes it (this app only lists them)
    ConfirmDialog {
        id: summary
        objectName: "shareSummaryDialog"
        title: qsTr("Share")
        acceptText: qsTr("Close")
        showCancel: false
    }
}
