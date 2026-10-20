import QtQuick
import QtQuick.Controls
import ".."

// Search by name. What the reader knows is searched as you type, with no connection; the server is asked only when you tap
// "Also search the server".
Item {
    id: page
    property bool serverAsked: false

    function emptyText() {
        if (browse.errorKey.length > 0) return (Problems.title(browse.errorKey) + " " + Problems.action(browse.errorKey)).trim()
        if (field.text.trim().length === 0) return qsTr("Type part of a name.")
        return qsTr("Nothing found on this reader.")
    }

    Component.onCompleted: browse.searchOffline("")
    StackView.onActivated: field.forceFocus()   // the page is on the stack now: the keyboard can come up
    Component.onDestruction: browse.showFolder()

    TopBar {
        id: topBar
        title: qsTr("Search")
        leftItems: [ IconButton { objectName: "searchBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
    }

    Column {
        id: header
        anchors { left: parent.left; right: parent.right; top: topBar.bottom }
        InkField {
            id: field
            objectName: "searchField"
            x: 30
            width: parent.width - 60
            topPadding: 10
            placeholderText: qsTr("Name")
            onTextEdited: { page.serverAsked = false; browse.searchOffline(text) }
            onAccepted: { page.serverAsked = false; browse.searchOffline(text) }
        }
        Text {
            objectName: "searchMore"
            x: 30
            width: parent.width - 60
            topPadding: 8
            wrapMode: Text.Wrap
            visible: browse.moreResults
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: qsTr("More results exist. Type more of the name to narrow the search.")
        }
        Text {
            objectName: "searchError"
            x: 30
            width: parent.width - 60
            topPadding: 8
            wrapMode: Text.Wrap
            visible: browse.errorKey.length > 0 && browse.rows.length > 0
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: Problems.title(browse.errorKey) + " " + Problems.action(browse.errorKey)
        }
        Rectangle { width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
    }

    FlatList {
        anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: panel.top }
        emptyText: page.emptyText()
        onOpenFolder: function (remotePath) {
            browse.open(remotePath)
            page.StackView.view.replace(Qt.resolvedUrl("FolderPage.qml"))
        }
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton {
                objectName: "searchServer"
                text: qsTr("Also search the server")
                icon: "search"
                enabled: field.text.trim().length > 0 && !browse.busy && connection.hasAccount
                onClicked: { page.serverAsked = true; browse.searchServer(field.text) }
            }
        ]
    }

    Connections {
        target: browse
        enabled: page.StackView.status === StackView.Active
        function onShowReason(key) { reasonDialog.text = qsTr("The server did not let the app read this entry."); reasonDialog.open() }
        function onShowDetails(remotePath) { page.StackView.view.push(Qt.resolvedUrl("DetailsPage.qml"), { remotePath: remotePath }) }
    }

    ConfirmDialog {
        id: reasonDialog
        objectName: "searchReason"
        title: qsTr("Not available")
        showCancel: false
    }
}
