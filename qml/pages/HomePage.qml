import QtQuick
import QtQuick.Controls
import ".."

// The start page of the baseline: the name, the version and the state of the connection. The app is not connected to a server yet.
Item {
    id: page

    function push(name) { if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl(name)) }

    TopBar {
        id: topBar
        title: qsTr("PB Cloud")
        leftItems: [ IconButton { objectName: "homeQuit"; text: qsTr("Home"); icon: "home"; onClicked: device.quit() } ]
        rightItems: [ IconButton { objectName: "homeSettings"; text: qsTr("Settings"); icon: "gear"; onClicked: page.push("SettingsPage.qml") } ]
    }

    Column {
        anchors { top: topBar.bottom; topMargin: 80; left: parent.left; right: parent.right }
        spacing: 24
        Text {
            objectName: "homeVersion"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: qsTr("Version %1").arg(about.version)
        }
        Text {
            objectName: "homeState"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            font.pixelSize: Style.bodySize
            font.bold: true
            color: Style.ink
            text: qsTr("Not connected")
        }
    }
}
