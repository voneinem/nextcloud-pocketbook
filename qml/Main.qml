import QtQuick
import QtQuick.Controls
import QtQuick.Window

// The single window. The system rotates (device.orientation), Qt resizes the window; this stage only
// compensates the panel's devicePixelRatio so that sizes authored in panel pixels are 1:1.
// No page sets a font family: every text inherits the application font, which carries the emoji fallback
// (app/main.cpp).
ApplicationWindow {
    id: win
    visible: true
    visibility: Window.FullScreen
    color: "#ffffff"

    // True once a page is shown (checked by the UI self-test: a blank screen must never go unnoticed).
    readonly property bool pageLoaded: nav.depth > 0

    Item {
        id: stage
        objectName: "stage"   // where the text fields' copy-and-paste bubble is put (TextMenu)
        readonly property real dpr: Screen.devicePixelRatio
        width: win.width * dpr
        height: win.height * dpr
        anchors.centerIn: parent
        scale: 1 / dpr

        StackView {
            id: nav
            anchors.fill: parent
            // initialItem with a binding silently showed nothing (measured on the desktop and the reader):
            // push the first page explicitly: the start page, or the storage error page when the database could not be opened.
            Component.onCompleted: push(Qt.resolvedUrl(startupError.length > 0 ? "pages/StorageErrorPage.qml" : "pages/HomePage.qml"))
            // E-ink: no transitions.
            pushEnter: Transition {}
            pushExit: Transition {}
            popEnter: Transition {}
            popExit: Transition {}
            replaceEnter: Transition {}
            replaceExit: Transition {}
        }

        // clearSecrets() of the share bridge closed the sharing screens (the account changed, the application was left, ...): none stays on the stack
        Connections {
            target: share
            function onClosed() {
                while (nav.depth > 1 && nav.currentItem && nav.currentItem.objectName.indexOf("share") === 0) nav.pop()
            }
        }

        // A hand-off to the mail app that was not accepted (specs/005-mail-link): the notice is shown the next time the app is shown, when no sharing
        // screen is open (while the link screen is open it shows the notice itself). It holds a key only: no link, no name.
        ConfirmDialog {
            id: mailNoticeDialog
            objectName: "mailNoticeDialog"
            showCancel: false
            title: Problems.title("mail_not_accepted")
            text: Problems.action("mail_not_accepted")
            onAccepted: share.dismissMailNotice()
            onRejected: share.dismissMailNotice()
        }
        function showMailNotice() {
            var onShareScreen = nav.currentItem && nav.currentItem.objectName.indexOf("share") === 0
            if (share.mailNotice.length > 0 && Qt.application.state === Qt.ApplicationActive && !onShareScreen) mailNoticeDialog.open()
        }
        Connections {
            target: share
            function onMailNoticeChanged() { stage.showMailNotice() }
        }
        Connections {
            target: Qt.application
            function onStateChanged() { stage.showMailNotice() }
        }

        Connections {
            target: device
            function onBackRequested() {
                // a page with an open popup (the editor's "/" menu) takes Back for itself (spec 006 FR-009)
                var page = nav.currentItem
                if (page && page.backHandled && page.backHandled()) return
                if (nav.depth > 1) {
                    Qt.inputMethod.hide()
                    nav.pop()
                }
            }
        }
    }
}
