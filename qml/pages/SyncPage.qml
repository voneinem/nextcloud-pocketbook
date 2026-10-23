import QtQuick
import QtQuick.Controls
import ".."

// Folder synchronization (specs/003-folder-sync): the pairs the user made between a server folder and a folder on the reader. A pair is only a
// setting; nothing is compared or transferred by looking at this page or by saving a pair. Each row says which way files go, the folders, the
// state and how much of the pair's limits the reader folder uses.
Item {
    id: page
    property int selectedId: 0

    function push(name, props) { if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl(name), props || {}) }
    function rowFor(id) {
        var items = sync.pairs
        for (var i = 0; i < items.length; ++i) if (items[i].id === id) return items[i]
        return null
    }
    function iconFor(direction) {
        switch (direction) {
        case "up": return "arrow-up"
        case "both": return "arrows-both"
        default: return "arrow-down"
        }
    }
    function title(row) { return row.remoteFolder + "  ->  " + row.readerFolder }
    function caption(row) {
        return [row.usageText, row.lastText].filter(function (t) { return t.length > 0 }).join("  ·  ")
    }
    function openChoices(row) {
        selectedId = row.id
        var choices = []
        if (row.usable && !sync.offline) choices.push({ label: qsTr("Sync"), value: "sync" })
        if (row.usable && sync.offline) choices.push({ label: qsTr("Preview offline"), value: "preview" })
        if (row.planReady) choices.push({ label: qsTr("Open plan"), value: "plan" })
        choices.push({ label: qsTr("Settings"), value: "settings" })
        if (row.lastText.length > 0) choices.push({ label: qsTr("Last result"), value: "result" })
        rowChoice.choices = choices
        rowChoice.open()
    }
    readonly property bool onTop: StackView.status === StackView.Active

    StackView.onActivated: sync.refresh()

    Connections {
        target: sync
        function onPlanReady(pairId, planId) { if (page.onTop) page.push("PlanPage.qml", { planId: planId, pairId: pairId }) }
        function onPreviewReady(pairId) { if (page.onTop) page.push("PlanPage.qml", { planId: 0, pairId: pairId }) }
        function onComparisonFailed(pairId, key) {
            if (!page.onTop) return
            notice.text = Problems.title(key) + " " + Problems.action(key)
            notice.open()
        }
    }

    TopBar {
        id: topBar
        title: qsTr("Sync")
        leftItems: [ IconButton { objectName: "syncBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
    }

    Column {
        id: header
        anchors { left: parent.left; right: parent.right; top: topBar.bottom }
        Text {
            objectName: "syncOffline"
            x: 30
            width: parent.width - 60
            topPadding: 12
            bottomPadding: 10
            wrapMode: Text.Wrap
            visible: sync.offline
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: qsTr("Offline. Pairs can be set up now; comparing needs the network.")
        }
        Text {
            objectName: "syncBusy"
            x: 30
            width: parent.width - 60
            topPadding: 12
            bottomPadding: 10
            wrapMode: Text.Wrap
            visible: sync.busy
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: sync.stage === "comparing" ? qsTr("Comparing...") : qsTr("Syncing...") + (sync.progressText.length > 0 ? "  " + sync.progressText : "")
        }
        Text {
            objectName: "syncReady"
            x: 30
            width: parent.width - 60
            topPadding: 12
            bottomPadding: 10
            wrapMode: Text.Wrap
            visible: sync.readyCount > 0
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: qsTr("%n plan(s) ready", "", sync.readyCount)
        }
        Rectangle { width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
    }

    Item {
        id: listArea
        anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: panel.top }
        clip: true

        Column {
            width: parent.width
            Repeater {
                model: sync.pairs
                delegate: Item {
                    objectName: "syncPairRow"
                    width: parent.width
                    height: Style.rowHeight + 36
                    opacity: modelData.usable ? 1.0 : 0.6
                    Rectangle { anchors.fill: parent; color: Style.pressed; visible: tap.pressed || modelData.id === page.selectedId }
                    Icon {
                        id: dirIcon
                        x: 30
                        anchors.verticalCenter: parent.verticalCenter
                        size: 48
                        source: page.iconFor(modelData.direction)
                    }
                    Column {
                        anchors { left: dirIcon.right; leftMargin: 24; right: parent.right; rightMargin: 30; verticalCenter: parent.verticalCenter }
                        spacing: 2
                        Text {
                            objectName: "syncPairTitle"
                            width: parent.width
                            elide: Text.ElideMiddle
                            font.pixelSize: Style.bodySize
                            font.bold: true
                            color: Style.ink
                            text: page.title(modelData)
                        }
                        Text {
                            objectName: "syncPairState"
                            width: parent.width
                            elide: Text.ElideRight
                            font.pixelSize: Style.captionSize
                            font.bold: !modelData.usable || modelData.problemKey.length > 0
                            color: Style.ink
                            text: modelData.directionText + "  ·  " + modelData.stateText
                        }
                        Text {
                            objectName: "syncPairUsage"
                            width: parent.width
                            elide: Text.ElideRight
                            font.pixelSize: Style.captionSize
                            color: Style.ink
                            text: page.caption(modelData)
                        }
                    }
                    RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
                    MouseArea { id: tap; anchors.fill: parent; onClicked: page.openChoices(modelData) }
                }
            }
        }
        Text {
            objectName: "syncEmpty"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 30; topMargin: 60 }
            wrapMode: Text.Wrap
            visible: sync.pairCount === 0
            font.pixelSize: Style.bodySize
            color: Style.ink
            text: qsTr("No pairs yet. A pair links a folder on the server with a folder on this reader. Nothing is copied until you look at a plan and approve it.")
        }
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton {
                objectName: "syncAdd"
                text: qsTr("Add pair")
                icon: "plus-circle"
                enabled: connection.hasAccount && sync.pairCount < sync.pairLimit
                onClicked: page.push("PairPage.qml", { pairId: 0 })
            },
            ToolButton {
                objectName: "syncAll"
                text: qsTr("Sync all")
                icon: "sync"
                enabled: sync.pairCount > 0 && !sync.busy && !sync.offline
                onClicked: sync.compareAll()
            }

        ]
    }

    ChoiceDialog {
        id: rowChoice
        objectName: "syncRowChoice"
        title: qsTr("Pair")
        anchor: panel
        onChosen: function (value) {
            var id = page.selectedId, row = page.rowFor(id)
            if (value === "settings") page.push("PairPage.qml", { pairId: id })
            else if (value === "sync") sync.compare(id)
            else if (value === "preview") sync.previewOffline(id)
            else if (value === "plan" && row) page.push("PlanPage.qml", { planId: row.planId, pairId: id })
            else if (value === "result") page.push("ResultPage.qml", { result: sync.resultFor(id), pairId: id })
            page.selectedId = 0
        }
    }

    ConfirmDialog {
        id: notice
        objectName: "syncNotice"
        title: qsTr("Not done")
        showCancel: false
    }
}
