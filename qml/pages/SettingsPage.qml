import QtQuick
import QtQuick.Controls
import ".."

// The options of the app: the account (address, user name, app password, the Test and the browser login), the language, the note on the
// certificate authority, and the way to About. Language changes are saved at once; a value the app does not accept says so.
Item {
    id: page
    property bool waitingForLogin: false
    property bool tested: false   // a Test was started on this page: its result is shown below the fields

    // how the app password is kept, in plain words and measured on this storage (FR-007)
    function storageText() {
        switch (connection.credentialsProtection) {
        case "enforced": return qsTr("The app password is kept as plain text in a file that only this app can read. Anyone who gets the reader's storage could still use it until you remove the app password in Nextcloud (Settings > Security). Your account password is never stored.")
        case "not_enforced": return qsTr("The app password is kept as plain text in a file, and this reader's storage cannot make that file private: anyone with USB access to the reader can read it and use the app password until you remove it in Nextcloud (Settings > Security). Your account password is never stored.")
        default: return qsTr("The app password is kept as plain text in a file on the reader; whether the storage keeps that file private is checked when it is saved. Anyone who gets the reader's storage could use the app password until you remove it in Nextcloud (Settings > Security). Your account password is never stored.")
        }
    }
    function quotaText() {
        if (connection.quotaUnlimited) return qsTr("%1 used, no limit").arg(connection.quotaUsedText)
        if (connection.quotaTotalText.length > 0) return qsTr("%1 of %2 used").arg(connection.quotaUsedText).arg(connection.quotaTotalText)
        if (connection.quotaUsedText.length > 0) return qsTr("%1 used").arg(connection.quotaUsedText)
        return qsTr("unknown")
    }
    // the page is longer than the screen: scrolling is by page, as everywhere (no dragging, no kinetic movement on e-ink)
    readonly property bool scrollable: flick.contentHeight > flick.height + 1
    function pageBy(direction) {
        var maxY = Math.max(0, flick.contentHeight - flick.height)
        flick.contentY = Math.min(maxY, Math.max(0, flick.contentY + direction * Math.max(100, flick.height - 80)))
    }

    function scrollTo(item) {
        var y = item.mapToItem(column, 0, 0).y
        flick.contentY = Math.max(0, y - 160)
    }

    function push(name) { if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl(name)) }
    function openChoice() {
        var c = [], list = settings.languageChoices
        for (var m = 0; m < list.length; ++m) c.push({ label: list[m].label, value: list[m].value })
        choice.choices = c
        choice.current = settings.language
        choice.open()
    }
    function errorText() {
        switch (settings.errorKey) {
        case "": return ""
        default: return qsTr("The setting could not be saved.")
        }
    }

    TopBar {
        id: topBar
        title: qsTr("Settings")
        leftItems: [ IconButton { objectName: "settingsBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
    }

    Flickable {
        id: flick
        anchors { left: parent.left; right: parent.right; top: topBar.bottom; bottom: panel.top }
        contentWidth: width
        contentHeight: column.implicitHeight + 40
        boundsBehavior: Flickable.StopAtBounds
        interactive: false
        clip: true

        Column {
            id: column
            width: parent.width

            component Head: Text {
                x: 30
                width: parent.width - 60
                topPadding: 24
                bottomPadding: 6
                font.pixelSize: Style.labelSize
                font.bold: true
                color: Style.ink
            }
            Head { text: qsTr("Account") }
            Item {
                width: parent.width
                height: fields.implicitHeight + 20
                Column {
                    id: fields
                    x: 30
                    width: parent.width - 60
                    spacing: 18
                    InkField {
                        id: addressField
                        objectName: "settingsAddress"
                        width: parent.width
                        label: qsTr("Server address")
                        placeholderText: "cloud.example.org"
                        onFieldFocus: function (item, gained) { if (gained) page.scrollTo(addressField) }
                    }
                    InkField {
                        id: userField
                        objectName: "settingsUser"
                        width: parent.width
                        label: qsTr("User name")
                        onFieldFocus: function (item, gained) { if (gained) page.scrollTo(userField) }
                    }
                    InkField {
                        id: passwordField
                        objectName: "settingsPassword"
                        width: parent.width
                        label: qsTr("App password")
                        onFieldFocus: function (item, gained) { if (gained) page.scrollTo(passwordField) }
                        Component.onCompleted: field.echoMode = TextInput.Password
                    }
                }
            }
            Text {
                objectName: "settingsStorageText"
                x: 30
                width: parent.width - 60
                wrapMode: Text.Wrap
                font.pixelSize: Style.captionSize
                color: Style.ink
                text: page.storageText()
            }
            Text {
                objectName: "settingsResult"
                x: 30
                width: parent.width - 60
                topPadding: 14
                wrapMode: Text.Wrap
                visible: text.length > 0
                font.pixelSize: Style.bodySize
                font.bold: true
                color: Style.ink
                text: connection.busy ? qsTr("Checking...")
                    : connection.problemKey.length > 0 ? Problems.title(connection.problemKey) + " " + Problems.action(connection.problemKey)
                    : (page.tested && connection.state === "ok") ? qsTr("The connection works.") : ""
            }
            Column {
                objectName: "settingsServer"
                width: parent.width
                visible: connection.hasAccount
                Head { text: qsTr("Server") }
                InkRow { text: qsTr("Name"); value: connection.serverName }
                InkRow { text: qsTr("Version"); value: connection.serverVersion }
                InkRow { text: qsTr("Storage space"); value: page.quotaText(); enabled: true }
                Text {
                    objectName: "settingsClockWarning"
                    x: 30
                    width: parent.width - 60
                    topPadding: 10
                    wrapMode: Text.Wrap
                    visible: connection.clockWarningMinutes > 0
                    font.pixelSize: Style.bodySize
                    font.bold: true
                    color: Style.ink
                    text: qsTr("The reader's clock differs from the server's by about %n minute(s). Check the date and time in the reader's settings.", "", connection.clockWarningMinutes)
                }
            }

            Head { text: qsTr("Language") }
            InkRow {
                objectName: "settingsLanguage"
                text: qsTr("Language of the app")
                value: settings.language === "de" ? "Deutsch" : (settings.language === "en" ? "English" : qsTr("Automatic"))
                chevron: true
                onClicked: page.openChoice()
            }
            Text {
                objectName: "settingsError"
                x: 30
                width: parent.width - 60
                visible: text.length > 0
                wrapMode: Text.Wrap
                topPadding: 12
                font.pixelSize: Style.bodySize
                font.bold: true
                color: Style.ink
                text: page.errorText()
            }

            Head { text: qsTr("Synchronization") }
            Item {
                width: parent.width
                height: 110
                Text {
                    x: 30
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Auto Sync")
                    font.pixelSize: Style.bodySize
                    color: Style.ink
                }
                InkSwitch {
                    objectName: "settingsAutoSync"
                    anchors { right: parent.right; rightMargin: 30; verticalCenter: parent.verticalCenter }
                    checked: queue.autoSync
                    onToggled: function (on) { queue.autoSync = on }
                }
                RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
            }
            Text {
                objectName: "settingsAutoSyncText"
                x: 30
                width: parent.width - 60
                wrapMode: Text.Wrap
                font.pixelSize: Style.captionSize
                color: Style.ink
                text: queue.autoSync ? qsTr("Changes in the queue are sent a few seconds after your last action, when the reader is online. Nothing is sent at any other time.")
                                     : qsTr("Changes stay in the queue until you tap Send on the Queue screen.")
            }

            Text {
                objectName: "settingsAutoSyncPairs"
                x: 30
                width: parent.width - 60
                topPadding: 6
                wrapMode: Text.Wrap
                font.pixelSize: Style.captionSize
                color: Style.ink
                text: qsTr("Also applies to pair syncs (they only compare).")
            }

            Head { text: qsTr("Security") }
            Text {
                objectName: "settingsCustomCa"
                x: 30
                width: parent.width - 60
                wrapMode: Text.Wrap
                font.pixelSize: Style.captionSize
                color: Style.ink
                text: settings.customCaStatus === "loaded" ? qsTr("Your own certificate authority is loaded (%n certificate(s)). Every connection is still checked; a certificate that cannot be verified is refused.", "", settings.customCaCount)
                    : settings.customCaStatus === "invalid" ? qsTr("The file custom-ca.pem could not be used and is ignored. Every connection is checked against the usual authorities; there is no way to skip the check.")
                    : qsTr("Every connection is checked against the usual authorities. For a server with your own authority, put its certificate as custom-ca.pem in the app's data folder (see the README).")
            }
            Head { text: qsTr("About this app") }
            InkRow { objectName: "settingsAbout"; text: qsTr("About, licenses"); chevron: true; onClicked: page.push("AboutPage.qml") }
        }
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton {
                objectName: "settingsPageUp"
                text: qsTr("Page up")
                icon: "chevron-up"
                visible: page.scrollable
                enabled: flick.contentY > 1
                onClicked: page.pageBy(-1)
            },
            ToolButton {
                objectName: "settingsPageDown"
                text: qsTr("Page down")
                icon: "chevron-down"
                visible: page.scrollable
                enabled: flick.contentY < flick.contentHeight - flick.height - 1
                onClicked: page.pageBy(1)
            },
            ToolButton {
                objectName: "settingsTest"
                text: qsTr("Test")
                icon: "check"
                enabled: !connection.busy
                onClicked: {
                    page.tested = true
                    Qt.inputMethod.hide()
                    connection.testConnection(addressField.text, userField.text, passwordField.text)
                    passwordField.text = ""   // the typed password does not stay on the screen
                }
            },
            ToolButton {
                objectName: "settingsBrowserLogin"
                text: qsTr("Browser login")
                icon: "sync"
                enabled: !connection.busy
                onClicked: {
                    Qt.inputMethod.hide()
                    page.waitingForLogin = true
                    connection.startLogin(addressField.text)
                }
            },
            ToolButton {
                objectName: "settingsSignOut"
                text: qsTr("Sign out")
                icon: "close"
                visible: connection.hasAccount
                enabled: !connection.busy
                onClicked: signOutDialog.open()
            }
        ]
    }

    Connections {
        target: connection
        function onChanged() {
            if (!page.waitingForLogin) return
            if (connection.loginActive) {
                page.waitingForLogin = false
                page.push("LoginPage.qml")
            } else if (!connection.busy) {
                page.waitingForLogin = false   // the start failed: the reason is shown below the fields
            }
        }
    }

    ConfirmDialog {
        id: signOutDialog
        objectName: "settingsSignOutDialog"
        title: qsTr("Sign out?")
        text: qsTr("The app password is removed from the reader. The files you downloaded stay on the reader. To make the app password useless on the server too, remove it in Nextcloud under Settings > Security.")
        acceptText: qsTr("Sign out")
        onAccepted: {
            connection.signOut()
            addressField.text = ""
            userField.text = ""
            page.tested = false
        }
    }

    Component.onCompleted: {
        settings.refresh()
        connection.refresh()
        addressField.text = connection.address
        userField.text = connection.loginName
    }

    ChoiceDialog {
        id: choice
        objectName: "settingsChoice"
        title: qsTr("Choose a value")
        onChosen: function (value) { settings.setLanguage(value) }
    }
}
