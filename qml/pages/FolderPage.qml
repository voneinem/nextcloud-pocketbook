import QtQuick
import QtQuick.Controls
import ".."

// One folder of the user's Nextcloud, page by page (no scrolling): the path line, how old the list is, the entries with name, size and
// date, page buttons, and the tools Refresh and Sort. Opening a folder uses the stored list when there is one; Refresh is explicit.
Item {
    id: page

    function pathLine() {
        var segs = browse.pathSegments, parts = []
        for (var i = 0; i < segs.length; ++i) parts.push(i === 0 ? qsTr("Files") : segs[i].label)
        return parts.join("  /  ")
    }
    function folderTitle() {
        var segs = browse.pathSegments
        return segs.length > 1 ? segs[segs.length - 1].label : qsTr("Files")
    }
    function ageText() { return EntryWords.ageText(browse.listingAgeKind, browse.listingAgeCount) }
    function reasonText(key) {
        if (key === "end_to_end_encrypted") return qsTr("This folder is end-to-end encrypted. The app cannot open it.")
        if (key === "no_read_permission") return qsTr("You are not allowed to read this.")
        if (key === "unsupported_state") return qsTr("The server shows this entry in a state the app does not support.")
        return qsTr("The server did not let the app read this entry.")
    }
    function rowCaption(row) {
        if (row.availability !== "ok") return qsTr("Not available")
        var parts = [row.kind === "folder" ? qsTr("Folder") : row.sizeText, row.dateText]
        if (row.kind === "file") parts.push(EntryWords.stateWord(row.state))
        if (row.kept) parts.push(qsTr("Kept offline"))
        return parts.filter(function (t) { return t.length > 0 }).join("  ·  ")
    }
    function sortLabel() {
        var k = browse.sortKey, a = browse.sortAscending
        if (k === "date") return a ? qsTr("Date, oldest first") : qsTr("Date, newest first")
        if (k === "size") return a ? qsTr("Size, smallest first") : qsTr("Size, largest first")
        return a ? qsTr("Name, A to Z") : qsTr("Name, Z to A")
    }
    function openSortChoice() {
        sortChoice.choices = [
            { label: qsTr("Name, A to Z"), value: "name:asc" }, { label: qsTr("Name, Z to A"), value: "name:desc" },
            { label: qsTr("Date, newest first"), value: "date:desc" }, { label: qsTr("Date, oldest first"), value: "date:asc" },
            { label: qsTr("Size, largest first"), value: "size:desc" }, { label: qsTr("Size, smallest first"), value: "size:asc" }]
        sortChoice.current = browse.sortKey + ":" + (browse.sortAscending ? "asc" : "desc")
        sortChoice.open()
    }

    Component.onCompleted: browse.open(browse.path)

    TopBar {
        id: topBar
        title: page.folderTitle()
        leftItems: [ IconButton { objectName: "folderBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
        rightItems: [ IconButton { objectName: "folderUp"; text: qsTr("Up"); icon: "chevron-up"; enabled: browse.canGoUp && !browse.busy; onClicked: browse.up() } ]
    }

    Column {
        id: header
        anchors { left: parent.left; right: parent.right; top: topBar.bottom }
        Text {
            objectName: "folderPath"
            x: 30
            width: parent.width - 60
            topPadding: 12
            elide: Text.ElideMiddle
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: page.pathLine()
        }
        Text {
            objectName: "folderAge"
            x: 30
            width: parent.width - 60
            bottomPadding: 10
            wrapMode: Text.Wrap
            visible: browse.hasListing
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: (browse.offline ? qsTr("Offline. ") : "") + page.ageText() + (browse.clockSuspect ? " " + qsTr("(the reader's clock may be wrong)") : "")
        }
        Text {
            objectName: "folderError"
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
        Rectangle { width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
    }

    Item {
        id: listArea
        anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: pager.top }
        clip: true
        onHeightChanged: browse.pageSize = Math.max(3, Math.floor(height / Style.rowHeight))

        Column {
            width: parent.width
            Repeater {
                model: browse.rows
                delegate: Item {
                    objectName: "folderRow"
                    width: parent.width
                    height: Style.rowHeight
                    opacity: modelData.availability === "ok" ? 1.0 : 0.55
                    Rectangle { anchors.fill: parent; color: Style.pressed; visible: tap.pressed }
                    Icon {
                        id: kindIcon
                        x: 30
                        anchors.verticalCenter: parent.verticalCenter
                        size: 48
                        source: modelData.kind === "folder" ? "folder" : "list"
                    }
                    Column {
                        anchors { left: kindIcon.right; leftMargin: 24; right: chev.left; rightMargin: 12; verticalCenter: parent.verticalCenter }
                        spacing: 4
                        Text {
                            width: parent.width
                            elide: Text.ElideMiddle
                            font.pixelSize: Style.bodySize
                            font.bold: modelData.kind === "folder"
                            color: Style.ink
                            text: modelData.name
                        }
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            font.pixelSize: Style.captionSize
                            color: Style.ink
                            text: page.rowCaption(modelData)
                        }
                    }
                    Icon {
                        id: chev
                        anchors { right: parent.right; rightMargin: 24; verticalCenter: parent.verticalCenter }
                        size: 40
                        visible: modelData.kind === "folder" && modelData.availability === "ok"
                        width: visible ? 40 : 0
                        source: visible ? "chevron-right" : ""
                    }
                    RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
                    MouseArea { id: tap; anchors.fill: parent; onClicked: browse.open(modelData.remotePath) }
                }
            }
        }
        Text {
            objectName: "folderEmpty"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 60 }
            wrapMode: Text.Wrap
            visible: !browse.busy && browse.rows.length === 0
            font.pixelSize: Style.bodySize
            color: Style.ink
            text: browse.hasListing ? qsTr("This folder is empty.")
                : (browse.errorKey === "no_network" || browse.errorKey === "server_unreachable" || browse.errorKey === "timeout")
                    ? qsTr("This folder is not available offline. Connect to the network and open it again.")
                : browse.errorKey.length > 0 ? Problems.title(browse.errorKey) + " " + Problems.action(browse.errorKey)
                : ""
        }
        Text {
            objectName: "folderBusy"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 60 }
            visible: browse.busy
            font.pixelSize: Style.bodySize
            font.bold: true
            color: Style.ink
            text: qsTr("Loading...")
        }
    }

    ModelPager {
        id: pager
        anchors { horizontalCenter: parent.horizontalCenter; bottom: panel.top }
        page: browse.page
        pageCount: browse.pageCount
        visible: browse.pageCount > 1
        height: visible ? Style.touchMin : 0
        onPrevious: browse.previousPage()
        onNext: browse.nextPage()
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton { objectName: "folderRefresh"; text: browse.hasListing ? qsTr("Refresh") : qsTr("Retry"); icon: "sync"; enabled: !browse.busy; onClicked: browse.refresh() },
            ToolButton {
                objectName: "folderKeep"
                text: browse.folderKept ? qsTr("Kept offline") : qsTr("Keep offline")
                icon: browse.folderKept ? "check" : "download"
                visible: browse.canGoUp && browse.hasListing
                enabled: !browse.folderKept || browse.folderKeptDirect
                onClicked: { transfer.setKept(browse.path, "folder", !browse.folderKeptDirect); browse.refreshRows() }
            },
            ToolButton {
                objectName: "folderUpdate"
                text: qsTr("Update")
                icon: "sync"
                enabled: !transfer.jobBusy && !transfer.updateRunning && connection.hasAccount
                onClicked: page.StackView.view.push(Qt.resolvedUrl("UpdatePage.qml"), { autoStart: true })
            },
            ToolButton { objectName: "folderSort"; text: qsTr("Sort"); icon: "filter"; enabled: browse.hasListing; onClicked: page.openSortChoice() },
            ToolButton { objectName: "folderFavorites"; text: qsTr("Favorites"); icon: "star"; enabled: connection.hasAccount; onClicked: page.StackView.view.push(Qt.resolvedUrl("FavoritesPage.qml")) },
            ToolButton { objectName: "folderSearch"; text: qsTr("Search"); icon: "search"; onClicked: page.StackView.view.push(Qt.resolvedUrl("SearchPage.qml")) },
            ToolButton { objectName: "folderSettings"; text: qsTr("Settings"); icon: "gear"; visible: !browse.hasListing && !browse.busy; onClicked: page.StackView.view.push(Qt.resolvedUrl("SettingsPage.qml")) }
        ]
    }

    Connections {
        target: transfer
        function onJobFinished() { browse.refreshRows() }
        function onUpdateFinished() { browse.refreshRows() }
    }

    Connections {
        target: browse
        enabled: page.StackView.status === StackView.Active   // a page above this one (favorites, search) handles its own taps
        function onShowReason(key) { reasonDialog.text = page.reasonText(key); reasonDialog.open() }
        function onShowDetails(remotePath) { page.StackView.view.push(Qt.resolvedUrl("DetailsPage.qml"), { remotePath: remotePath }) }
    }

    ConfirmDialog {
        id: reasonDialog
        objectName: "folderReason"
        title: qsTr("Not available")
        showCancel: false
    }

    ChoiceDialog {
        id: sortChoice
        objectName: "folderSortChoice"
        title: qsTr("Sort by")
        anchor: panel
        onChosen: function (value) {
            var p = value.split(":")
            browse.setSort(p[0], p[1] === "asc")
        }
    }
}
