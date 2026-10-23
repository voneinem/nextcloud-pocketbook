import QtQuick
import QtQuick.Controls
import ".."

// The start page: the name, the version and the state of the connection to the user's Nextcloud.
Item {
    id: page

    StackView.onActivated: { connection.refresh(); queue.refresh() }   // a listing, a download or an Update may have confirmed the login meanwhile

    function push(name, props) { if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl(name), props || {}) }

    TopBar {
        id: topBar
        title: qsTr("PB Cloud")
        leftItems: [ IconButton { objectName: "homeQuit"; text: qsTr("Home"); icon: "home"; onClicked: device.quit() } ]
        rightItems: [ IconButton { objectName: "homeSettings"; text: qsTr("Settings"); icon: "gear"; onClicked: page.push("SettingsPage.qml") } ]
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton {
                objectName: "homeFiles"
                text: qsTr("Files")
                icon: "folder"
                enabled: connection.hasAccount
                onClicked: page.push("FolderPage.qml")
            },
            ToolButton {
                objectName: "homeFavorites"
                text: qsTr("Favorites")
                icon: "star"
                enabled: connection.hasAccount
                onClicked: page.push("FavoritesPage.qml")
            },
            ToolButton {
                objectName: "homeSearch"
                text: qsTr("Search")
                icon: "search"
                onClicked: page.push("SearchPage.qml")
            },
            ToolButton {
                objectName: "homeQueue"
                text: qsTr("Queue")
                icon: "list"
                onClicked: page.push("QueuePage.qml")
            },
            ToolButton {
                objectName: "homeSync"
                text: sync.readyCount > 0 ? qsTr("Sync (%1)").arg(sync.readyCount) : qsTr("Sync")
                icon: "arrows-both"
                enabled: connection.hasAccount
                onClicked: page.push("SyncPage.qml")
            },
            ToolButton {
                objectName: "homeStorage"
                text: qsTr("Storage")
                icon: "save"
                onClicked: page.push("SpacePage.qml")
            },
            ToolButton {
                objectName: "homeUpdate"
                text: qsTr("Update")
                icon: "sync"
                enabled: connection.hasAccount && !transfer.jobBusy
                onClicked: page.push("UpdatePage.qml", { autoStart: true })
            }
        ]
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
            text: {
                switch (connection.state) {
                case "ok": return qsTr("Connected to %1").arg(connection.serverName)
                case "offline": return qsTr("Offline, not checked")
                case "needs_signin": return qsTr("Please sign in again")
                case "problem": return Problems.title(connection.problemKey)
                case "untested": return qsTr("Not tested yet")
                default: return qsTr("Not connected")
                }
            }
        }
        Text {
            objectName: "homeChecked"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            visible: connection.checkedAtText.length > 0
            font.pixelSize: Style.labelSize
            color: Style.ink
            text: qsTr("Last checked: %1").arg(connection.checkedAtText)
        }
        Text {
            objectName: "homeUpdated"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            visible: transfer.lastUpdateText.length > 0
            font.pixelSize: Style.labelSize
            color: Style.ink
            text: transfer.lastUpdateOutcome === "success" ? qsTr("Last update: %1").arg(transfer.lastUpdateText)
                : qsTr("Last update: %1 (not complete)").arg(transfer.lastUpdateText)
        }
        Text {
            objectName: "homeQueueLine"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            visible: queue.openCount > 0
            font.pixelSize: Style.labelSize
            color: Style.ink
            text: qsTr("Queue: %n not synced", "", queue.openCount)
        }
        Text {
            objectName: "homeSyncLine"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            visible: sync.readyCount > 0
            font.pixelSize: Style.labelSize
            color: Style.ink
            text: qsTr("Sync: %n plan(s) ready", "", sync.readyCount)
        }
        Text {
            objectName: "homeAction"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            visible: text.length > 0
            font.pixelSize: Style.labelSize
            color: Style.ink
            text: {
                switch (connection.state) {
                case "needs_signin": return qsTr("The server refused the login twice. Open Settings and test the connection with the right app password.")
                case "problem": return Problems.action(connection.problemKey)
                case "untested": return qsTr("Open Settings and test the connection.")
                case "none": return qsTr("Open Settings to connect to your Nextcloud.")
                default: return ""
                }
            }
        }
    }
}
