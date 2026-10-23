import QtQuick
import QtQuick.Controls
import ".."

// One public link (specs/004-share-links US3, US4): the complete link in large plain letters, wrapped at any character so that no part is cut off, and its
// state. The link is shown here and nowhere else; it is not tappable and the app never opens it. Copy is offered only where the reader has a clipboard that
// other apps can use. The password, the expiry date and (for a folder) the permission change one at a time: each change states its effect in a confirmation
// and the screen then shows the state the server reports. Leaving the screen drops the link (and clears the clipboard if it still holds exactly the link).
Item {
    id: page
    objectName: "shareLinkPage"
    property bool copied: false
    property string pendingAction: ""      // the change the user asked for and has to confirm
    property string pendingPassword: ""    // a typed password until it is confirmed (then handed over and forgotten) or cancelled
    property string pendingExpiry: ""
    property string pendingPermission: ""

    function stateWarning() {
        switch (share.detail.linkState) {
        case "different_address": return qsTr("This address is not the one of your server. Check it before you pass it on.")
        case "not_secure": return qsTr("This address does not use https. Anyone on the way can read what is sent.")
        case "unreadable": return qsTr("The link is not readable and is not shown.")
        default: return ""
        }
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
    function changeOptions(confirmed) {
        return { action: pendingAction, password: pendingPassword, expiry: pendingExpiry, permission: pendingPermission, confirmed: confirmed }
    }
    // asks the bridge what the change would do; a refusal shows in the banner, an acceptable change goes to the confirmation
    function ask(action, password, expiry, permission) {
        page.copied = false
        pendingAction = action
        pendingPassword = password || ""
        pendingExpiry = expiry || ""
        pendingPermission = permission || ""
        var r = share.change(share.detail.id, changeOptions(false))
        if (r.needsConfirm) { changeConfirm.text = r.effectText; changeConfirm.open() }
        else pendingPassword = ""
    }
    function leave() {
        pendingPassword = ""
        share.leaveLink()
        if (page.StackView.view) page.StackView.view.pop()
    }
    Component.onDestruction: { pendingPassword = ""; share.leaveLink() }

    TopBar {
        id: topBar
        title: qsTr("Link")
        leftItems: [ IconButton { objectName: "shareLinkBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: page.leave() } ]
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
                objectName: "shareLinkItem"
                x: 30
                width: parent.width - 60
                topPadding: 14
                elide: Text.ElideMiddle
                font.pixelSize: Style.bodySize
                font.bold: true
                color: Style.ink
                text: share.itemName
            }
            // the link: plain text, wrapped at any character, no handler of any kind (it cannot be tapped or opened)
            Text {
                objectName: "shareLinkText"
                x: 30
                width: parent.width - 60
                topPadding: 26
                bottomPadding: 12
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                font.pixelSize: 40
                font.bold: true
                color: Style.ink
                text: share.detail.linkText || ""
                visible: text.length > 0
            }
            Text {
                objectName: "shareLinkLength"
                x: 30
                width: parent.width - 60
                bottomPadding: 10
                visible: share.detail.linkLength > 0
                font.pixelSize: Style.captionSize
                color: Style.ink
                text: qsTr("%n characters", "", share.detail.linkLength || 0)
            }
            Text {
                objectName: "shareLinkWarning"
                x: 30
                width: parent.width - 60
                bottomPadding: 14
                wrapMode: Text.Wrap
                visible: text.length > 0
                font.pixelSize: Style.captionSize
                font.bold: true
                color: Style.ink
                text: page.stateWarning().length > 0 ? "! " + page.stateWarning() : ""
            }
            Text {
                objectName: "shareLinkMessage"
                x: 30
                width: parent.width - 60
                bottomPadding: 14
                wrapMode: Text.Wrap
                visible: text.length > 0
                font.pixelSize: Style.captionSize
                font.bold: true
                color: Style.ink
                text: page.messageText().length > 0 ? "✗ " + page.messageText() : ""
            }
            Text {
                objectName: "shareLinkCopied"
                x: 30
                width: parent.width - 60
                bottomPadding: 14
                wrapMode: Text.Wrap
                visible: page.copied
                font.pixelSize: Style.captionSize
                font.bold: true
                color: Style.ink
                text: qsTr("Copied. Other apps can read it until you copy something else.")
            }
            Rectangle { width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
            // the three settings: a tap changes one of them (only for a link of the user)
            InkRow {
                objectName: "shareLinkPermission"
                text: qsTr("Permission")
                value: share.detail.permissionText || ""
                chevron: share.detail.canChange === true && share.isFolder
                enabled: share.detail.canChange === true && share.isFolder && !share.busy
                opacity: 1.0
                onClicked: {
                    permissionChoice.choices = share.rules.uploadOffered === true ? [{ label: qsTr("Read only"), value: "read" }, { label: qsTr("Read and upload"), value: "upload" }]
                                                                                  : [{ label: qsTr("Read only"), value: "read" }]
                    permissionChoice.current = share.detail.permission || ""
                    permissionChoice.open()
                }
            }
            InkRow {
                objectName: "shareLinkPassword"
                text: qsTr("Password")
                value: share.detail.passwordValue || ""
                chevron: share.detail.canChange === true
                enabled: share.detail.canChange === true && !share.busy
                opacity: 1.0
                onClicked: passwordDialog.open("")
            }
            InkRow {
                objectName: "shareLinkExpiry"
                text: qsTr("Expires")
                value: share.detail.expiryValue || ""
                chevron: share.detail.canChange === true
                enabled: share.detail.canChange === true && !share.busy
                opacity: 1.0
                onClicked: expiryDialog.open(share.detail.expires || "")
            }
            Text {
                objectName: "shareLinkNotYours"
                x: 30
                width: parent.width - 60
                topPadding: 14
                wrapMode: Text.Wrap
                visible: share.detail.canChange === false
                font.pixelSize: Style.captionSize
                color: Style.ink
                text: qsTr("This link belongs to someone else. You can see it but not change it.")
            }
        }
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton {
                objectName: "shareLinkCopy"
                text: qsTr("Copy")
                icon: "copy"
                visible: share.clipboardAvailable && share.detail.linkState !== "unreadable"
                onClicked: page.copied = share.copyLink()
            },
            ToolButton {
                objectName: "shareLinkRemove"
                text: qsTr("Remove link")
                icon: "trash"
                visible: share.detail.canChange === true
                enabled: !share.busy
                onClicked: {
                    page.copied = false
                    var r = share.remove(share.detail.id, false)
                    if (r.needsConfirm) { removeConfirm.text = r.effectText; removeConfirm.open() }
                }
            }
        ]
    }

    // the password is shown in plain letters but is a secret: no Copy, no suggestions, emptied on close; the page holds it only until the change is confirmed
    TextEntryDialog {
        id: passwordDialog
        objectName: "sharePasswordDialog"
        secret: true
        title: qsTr("Password")
        placeholder: qsTr("1 to 256 characters")
        clearText: share.detail.hasPassword === true ? qsTr("Remove password") : ""
        onAccepted: function (text) { close(); page.ask("set_password", text, "", "") }
        onCleared: page.ask("clear_password", "", "", "")
    }

    TextEntryDialog {
        id: expiryDialog
        objectName: "shareExpiryDialog"
        title: qsTr("Expiry date")
        placeholder: qsTr("YYYY-MM-DD")
        clearText: (share.detail.expires || "").length > 0 ? qsTr("Remove date") : ""
        onAccepted: function (text) { close(); page.ask("set_expiry", "", text.trim(), "") }
        onCleared: page.ask("clear_expiry", "", "", "")
    }

    ChoiceDialog {
        id: permissionChoice
        objectName: "sharePermissionChoice"
        title: qsTr("Permission")
        anchor: panel
        onChosen: function (value) { if (value !== (share.detail.permission || "")) page.ask("set_permission", "", "", value) }
    }

    ConfirmDialog {
        id: changeConfirm
        objectName: "shareChangeConfirm"
        title: qsTr("Change link")
        acceptText: qsTr("Change")
        onAccepted: {
            share.change(share.detail.id, page.changeOptions(true))   // overwrites the password string in place; the page forgets it too
            page.pendingPassword = ""
        }
        onRejected: page.pendingPassword = ""
    }

    // Remove: the confirmation names the item and says the link stops working at once and the item is not deleted; the first button (Cancel) is the default
    ConfirmDialog {
        id: removeConfirm
        objectName: "shareRemoveConfirm"
        title: qsTr("Remove link")
        acceptText: qsTr("Remove link")
        onAccepted: share.remove(share.detail.id, true)
    }

    // the link is gone (removed now, or already removed): back to the list, which is read again
    Connections {
        target: share
        function onRemoveFinished(ok, errorKey) {
            if (ok && page.StackView.view) page.StackView.view.pop()
        }
    }
}
