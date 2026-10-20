import QtQuick
import QtQuick.Controls
import ".."

// The login in a browser (Login Flow v2): the link and a QR code of it, to open on a phone or a computer; no password is typed on the
// reader. The user taps Check afterwards; nothing is polled in the background.
Item {
    id: page
    property string answer: ""   // the result of the last Check, as a key: pending | expired | failed

    function answerText() {
        switch (answer) {
        case "pending": return qsTr("Not approved yet. Approve the login on your other device, then tap Check again.")
        case "expired": return qsTr("The login ran out of time. Go back and start it again.")
        case "failed": return Problems.title(connection.problemKey) + " " + Problems.action(connection.problemKey)
        default: return ""
        }
    }

    Connections {
        target: connection
        function onLoginChecked(status) {
            page.answer = status
            if (status === "approved" && page.StackView.view) page.StackView.view.pop()
        }
    }

    TopBar {
        id: topBar
        title: qsTr("Log in with a browser")
        leftItems: [ IconButton { objectName: "loginBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: { connection.cancelLogin(); if (page.StackView.view) page.StackView.view.pop() } } ]
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
            spacing: 20

            Text {
                x: 30
                width: parent.width - 60
                topPadding: 20
                wrapMode: Text.Wrap
                font.pixelSize: Style.bodySize
                color: Style.ink
                text: qsTr("On your phone or computer, open this link, log in to your Nextcloud and allow the access. Then tap Check here.")
            }
            Image {
                id: qr
                objectName: "loginQr"
                anchors.horizontalCenter: parent.horizontalCenter
                visible: status === Image.Ready
                source: connection.qrImageUrl
                cache: false
                smooth: false
                width: 520
                height: 520
                fillMode: Image.PreserveAspectFit
            }
            Text {
                objectName: "loginLink"
                x: 30
                width: parent.width - 60
                wrapMode: Text.WrapAnywhere
                font.pixelSize: Style.labelSize
                color: Style.ink
                text: connection.loginUrl
            }
            Text {
                objectName: "loginMinutes"
                x: 30
                width: parent.width - 60
                font.pixelSize: Style.captionSize
                color: Style.ink
                text: connection.loginActive ? qsTr("About %n minute(s) left.", "", connection.loginExpiresInMinutes) : ""
            }
            Text {
                objectName: "loginAnswer"
                x: 30
                width: parent.width - 60
                wrapMode: Text.Wrap
                visible: text.length > 0
                font.pixelSize: Style.bodySize
                font.bold: true
                color: Style.ink
                text: connection.busy ? qsTr("Checking...") : page.answerText()
            }
        }
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton { objectName: "loginCheck"; text: qsTr("Check"); icon: "check"; enabled: connection.loginActive && !connection.busy; onClicked: connection.checkLogin() },
            ToolButton { objectName: "loginCancel"; text: qsTr("Cancel"); icon: "close"; onClicked: { connection.cancelLogin(); if (page.StackView.view) page.StackView.view.pop() } }
        ]
    }
}
