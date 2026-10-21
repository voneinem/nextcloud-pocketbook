import QtQuick

// The question for a name that is taken in the server folder (specs/002-manage-server-files US3): keep both under a new name, replace the file on the
// server (the caller asks a second confirmation first), or skip. Nothing is queued before the answer. `others` is the number of questions that follow;
// with it the user can give the same answer to all of them. The close icon is a Skip: it queues nothing.
// Usage: CollisionDialog { ask: map of the bridge; others: n; onAnswered: function (choice, all) { ... } }, open().
DialogFrame {
    id: root
    property var ask: ({})
    property int others: 0
    property bool sameForOthers: false
    signal answered(string choice, bool all)

    title: qsTr("Name already taken")
    footer: ask.replacePossible ? [{ text: qsTr("Skip"), role: "skip" }, { text: qsTr("Keep both"), role: "keep_both" }, { text: qsTr("Replace"), role: "replace" }]
                                : [{ text: qsTr("Skip"), role: "skip" }, { text: qsTr("Keep both"), role: "keep_both" }]
    onCloseRequested: { close(); answered("skip", false) }
    onFooterClicked: function (role) {
        close()
        answered(role, sameForOthers && others > 0)
    }

    Text {
        objectName: "collisionText"
        width: parent.width
        wrapMode: Text.Wrap
        font.pixelSize: Style.bodySize
        color: Style.ink
        text: root.ask.existingIsFolder ? qsTr("A folder named \"%1\" is already in %2. A file cannot replace it.").arg(root.ask.name).arg(root.ask.targetFolder)
            : root.ask.inQueue && root.ask.existingSizeText === "" ? qsTr("\"%1\" is already waiting in the queue for %2.").arg(root.ask.name).arg(root.ask.targetFolder)
            : qsTr("\"%1\" is already in %2.").arg(root.ask.name).arg(root.ask.targetFolder)
    }
    Text {
        objectName: "collisionInfo"
        width: parent.width
        wrapMode: Text.Wrap
        visible: text.length > 0
        font.pixelSize: Style.captionSize
        color: Style.ink
        text: [root.ask.existingSizeText, root.ask.existingDateText].filter(function (t) { return t && t.length > 0 }).join(", ")
              ? qsTr("On the server: %1").arg([root.ask.existingSizeText, root.ask.existingDateText].filter(function (t) { return t && t.length > 0 }).join(", ")) : ""
    }
    Text {
        objectName: "collisionRefresh"
        width: parent.width
        wrapMode: Text.Wrap
        visible: root.ask.refreshNeeded === true
        font.pixelSize: Style.captionSize
        color: Style.ink
        text: qsTr("To replace it, refresh this folder first.")
    }
    Item {
        width: parent.width
        height: visible ? Style.touchMin : 0
        visible: root.others > 0
        Text {
            anchors { left: parent.left; right: sw.left; rightMargin: 16; verticalCenter: parent.verticalCenter }
            wrapMode: Text.Wrap
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: qsTr("Do the same for the other %n name(s)", "", root.others)
        }
        InkSwitch {
            id: sw
            objectName: "collisionAll"
            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
            checked: root.sameForOthers
            onToggled: function (on) { root.sameForOthers = on }
        }
    }
}
