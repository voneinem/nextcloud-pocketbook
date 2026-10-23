import QtQuick
import QtQuick.Controls
import ".."

// Create a public link (specs/004-share-links US1): the permission, an optional password and an optional expiry date, then a confirmation that says in
// plain words what the link will allow. Nothing is created by opening this screen; the confirmation defaults to Cancel. The typed password is handed to the
// core once and is then gone from here: it is the one entry that is not kept when the server refuses (the user types it again).
Item {
    id: page
    objectName: "shareFormPage"
    property string permission: "read"     // read | upload
    property string password: ""
    property string expiry: ""             // YYYY-MM-DD or ""

    function required(bit) { return (share.message && share.message.fieldMask & bit) !== 0 }
    function passwordRequired() { return permission === "upload" ? share.rules.passwordRequiredUpload === true : share.rules.passwordRequired === true }
    function allowsText() {
        var t = !share.isFolder ? qsTr("Anyone who has this link can read this file.")
              : permission === "upload" ? qsTr("Anyone who has this link can read this folder and add files to it.")
              : qsTr("Anyone who has this link can read this folder.")
        t += " " + (password.length > 0 ? qsTr("A password is set.") : qsTr("No password is set."))
        t += " " + (expiry.length > 0 ? qsTr("The link stops working after %1.").arg(share.dateText(expiry)) : qsTr("The link does not expire."))
        return t
    }
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
    function options(confirmed) { return { permission: page.permission, password: page.password, expiry: page.expiry, confirmed: confirmed } }
    function ask() {
        var r = share.create(options(false))
        if (r.needsConfirm) { confirm.text = r.summary; confirm.open() }
    }
    function leave() {
        page.password = ""
        if (page.StackView.view) page.StackView.view.pop()
    }
    Component.onCompleted: share.clearMessage()
    Component.onDestruction: page.password = ""

    TopBar {
        id: topBar
        title: qsTr("Create link")
        leftItems: [ IconButton { objectName: "shareFormBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: page.leave() } ]
    }

    Flickable {
        anchors { left: parent.left; right: parent.right; top: topBar.bottom; bottom: panel.top }
        contentHeight: body.height
        clip: true
        interactive: false

        Column {
            id: body
            width: parent.width
            Text {
                objectName: "shareFormNotice"
                x: 30
                width: parent.width - 60
                topPadding: 14
                wrapMode: Text.Wrap
                visible: share.hasExistingLink
                font.pixelSize: Style.captionSize
                color: Style.ink
                text: qsTr("This adds another link; the existing link keeps working.")
            }
            Text {
                objectName: "shareFormMessage"
                x: 30
                width: parent.width - 60
                topPadding: 10
                wrapMode: Text.Wrap
                visible: text.length > 0
                font.pixelSize: Style.captionSize
                font.bold: true
                color: Style.ink
                text: page.messageText().length > 0 ? "✗ " + page.messageText() : ""
            }
            Text {
                x: 30
                width: parent.width - 60
                topPadding: 18
                bottomPadding: 6
                font.pixelSize: Style.labelSize
                font.bold: true
                color: Style.ink
                text: qsTr("Permission")
            }
            InkRow {
                objectName: "shareFormPermissionRow"
                text: qsTr("Read only")
                value: page.permission === "read" ? "✓" : ""
                onClicked: page.permission = "read"
            }
            InkRow {
                objectName: "shareFormPermissionRow"
                visible: share.isFolder
                height: visible ? implicitHeight : 0
                enabled: share.rules.uploadOffered === true
                text: qsTr("Read and upload")
                value: page.permission === "upload" ? "✓" : (share.rules.uploadOffered === true ? "" : qsTr("Not allowed"))
                onClicked: page.permission = "upload"
            }
            InkRow {
                objectName: "shareFormPassword"
                text: qsTr("Password") + (page.passwordRequired() ? " *" : "")
                value: page.password.length > 0 ? qsTr("Set") : (page.required(1) || page.passwordRequired() ? "✗ " + qsTr("Required") : qsTr("None"))
                chevron: true
                onClicked: passwordDialog.open("")
            }
            InkRow {
                objectName: "shareFormExpiry"
                text: qsTr("Expires") + (share.rules.expiryRequired === true ? " *" : "")
                value: page.expiry.length > 0 ? share.dateText(page.expiry) : (page.required(2) || share.rules.expiryRequired === true ? "✗ " + qsTr("Required") : qsTr("Never"))
                chevron: true
                onClicked: expiryDialog.open(page.expiry)
            }
            Text {
                objectName: "shareFormLimit"
                x: 30
                width: parent.width - 60
                topPadding: 6
                wrapMode: Text.Wrap
                visible: share.rules.expiryMaxDays > 0
                font.pixelSize: Style.captionSize
                color: Style.ink
                text: qsTr("At most %n days from today.", "", share.rules.expiryMaxDays || 0)
            }
            Text {
                objectName: "shareFormAllows"
                x: 30
                width: parent.width - 60
                topPadding: 26
                wrapMode: Text.Wrap
                font.pixelSize: Style.bodySize
                color: Style.ink
                text: page.allowsText()
            }
        }
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton {
                objectName: "shareFormCreate"
                text: qsTr("Create link")
                icon: "check"
                enabled: !share.busy && share.itemShareable
                onClicked: page.ask()
            },
            ToolButton {
                objectName: "shareFormCancel"
                text: qsTr("Cancel")
                icon: "close"
                onClicked: page.leave()
            }
        ]
    }

    // the password is shown in plain letters (the user must be able to read what they type) but is a secret: no Copy, no suggestions, emptied on close
    TextEntryDialog {
        id: passwordDialog
        objectName: "sharePasswordDialog"
        secret: true
        title: qsTr("Password")
        placeholder: qsTr("1 to 256 characters")
        clearText: page.password.length > 0 ? qsTr("Remove password") : ""
        onAccepted: function (text) { page.password = text; close() }
        onCleared: page.password = ""
    }

    TextEntryDialog {
        id: expiryDialog
        objectName: "shareExpiryDialog"
        title: qsTr("Expiry date")
        placeholder: qsTr("YYYY-MM-DD")
        clearText: page.expiry.length > 0 ? qsTr("Remove date") : ""
        onAccepted: function (text) { page.expiry = text.trim(); close() }
        onCleared: page.expiry = ""
    }

    ConfirmDialog {
        id: confirm
        objectName: "shareConfirmDialog"
        title: qsTr("Create link")
        acceptText: qsTr("Create link")
        onAccepted: {
            share.create(page.options(true))   // overwrites the password string in place; the page forgets it too
            page.password = ""
        }
    }

    Connections {
        target: share
        function onCreateFinished(ok, errorKey) {
            if (ok) { page.password = ""; if (page.StackView.view) page.StackView.view.pop() }
        }
    }
}
