import QtQuick
import QtQuick.Controls
import ".."

// The options of the app: the language, the note on the certificate authority, and the way to About. Changes are saved at once; a value the app does not accept says so.
Item {
    id: page

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

    Component.onCompleted: settings.refresh()

    TopBar {
        id: topBar
        title: qsTr("Settings")
        leftItems: [ IconButton { objectName: "settingsBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
    }

    Flickable {
        anchors { left: parent.left; right: parent.right; top: topBar.bottom; bottom: parent.bottom }
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

    ChoiceDialog {
        id: choice
        objectName: "settingsChoice"
        title: qsTr("Choose a value")
        onChosen: function (value) { settings.setLanguage(value) }
    }
}
