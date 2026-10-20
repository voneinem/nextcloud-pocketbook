import QtQuick
import QtQuick.Controls
import ".."

// The files starred in Nextcloud. The stored list is shown; it is fetched when it was never fetched and on Refresh, never on its own.
Item {
    id: page

    function openSortChoice() {
        sortChoice.choices = [
            { label: qsTr("Name, A to Z"), value: "name:asc" }, { label: qsTr("Name, Z to A"), value: "name:desc" },
            { label: qsTr("Date, newest first"), value: "date:desc" }, { label: qsTr("Date, oldest first"), value: "date:asc" },
            { label: qsTr("Size, largest first"), value: "size:desc" }, { label: qsTr("Size, smallest first"), value: "size:asc" }]
        sortChoice.current = browse.sortKey + ":" + (browse.sortAscending ? "asc" : "desc")
        sortChoice.open()
    }
    function emptyText() {
        if (browse.errorKey.length > 0 && !browse.hasListing) {
            return browse.errorKey === "no_network" ? qsTr("The favorites are not available offline. Connect to the network and tap Refresh.")
                : (Problems.title(browse.errorKey) + " " + Problems.action(browse.errorKey)).trim()
        }
        return browse.hasListing ? qsTr("You have no favorites in Nextcloud.") : ""
    }

    Component.onCompleted: browse.showFavorites()
    Component.onDestruction: browse.showFolder()

    TopBar {
        id: topBar
        title: qsTr("Favorites")
        leftItems: [ IconButton { objectName: "favoritesBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
    }

    Column {
        id: header
        anchors { left: parent.left; right: parent.right; top: topBar.bottom }
        Text {
            objectName: "favoritesAge"
            x: 30
            width: parent.width - 60
            topPadding: 12
            bottomPadding: 10
            wrapMode: Text.Wrap
            visible: browse.hasListing
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: (browse.offline ? qsTr("Offline. ") : "") + EntryWords.ageText(browse.listingAgeKind, browse.listingAgeCount) + (browse.clockSuspect ? " " + qsTr("(the reader's clock may be wrong)") : "")
        }
        Text {
            objectName: "favoritesError"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            visible: browse.errorKey.length > 0 && browse.hasListing
            bottomPadding: 10
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: Problems.title(browse.errorKey) + " " + Problems.action(browse.errorKey)
        }
        Text {
            objectName: "favoritesMore"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            visible: browse.moreResults
            bottomPadding: 10
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: qsTr("Only the first 2000 favorites are kept.")
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
            ToolButton { objectName: "favoritesRefresh"; text: qsTr("Refresh"); icon: "sync"; enabled: !browse.busy; onClicked: browse.refresh() },
            ToolButton { objectName: "favoritesSort"; text: qsTr("Sort"); icon: "filter"; enabled: browse.hasListing; onClicked: page.openSortChoice() }
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
        objectName: "favoritesReason"
        title: qsTr("Not available")
        showCancel: false
    }

    ChoiceDialog {
        id: sortChoice
        objectName: "favoritesSortChoice"
        title: qsTr("Sort by")
        anchor: panel
        onChosen: function (value) {
            var p = value.split(":")
            browse.setSort(p[0], p[1] === "asc")
        }
    }
}
