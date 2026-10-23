import QtQuick
import QtQuick.Controls
import ".."

// One pair (specs/003-folder-sync US1): the server folder, the reader folder, the direction, the limits and the two switches. Saving writes the pair
// and nothing else: no request, no file is touched. A direction that can send files from the reader to the server is saved only after the user
// confirmed it with both folders named (FR-005).
Item {
    id: page
    property int pairId: 0                 // 0 = a new pair
    property string remotePath: ""
    property string readerPath: ""
    property string direction: "down"
    property string startDirection: "down"
    property var maxFiles: 500
    property var maxBytes: 2147483648
    property var startMaxFiles: 500
    property var startMaxBytes: 2147483648
    property bool limitsOk: false     // the raised limits were confirmed
    property bool startupSync: false
    property bool autoSync: false
    property string message: ""
    // the directions that are ready to use; the others are shown with "Available soon" until their story is built
    readonly property bool uploadReady: true
    readonly property bool bothReady: true

    function lastName(path) {
        if (path === "" || path === "/") return qsTr("Files")
        var i = path.lastIndexOf("/")
        return i >= 0 ? path.substring(i + 1) : path
    }
    function sizeText(bytes) {
        var gib = 1024 * 1024 * 1024, mib = 1024 * 1024
        if (bytes % gib === 0) return qsTr("%1 GB").arg(bytes / gib)
        return qsTr("%1 MB").arg(Math.round(bytes / mib))
    }
    // "2", "2 GB", "500 MB", "1.5 gb": a plain number is megabytes
    function parseSize(text) {
        var m = /^\s*(\d+(?:[.,]\d+)?)\s*(mb|gb)?\s*$/i.exec(text)
        if (!m) return -1
        var n = parseFloat(m[1].replace(",", "."))
        var unit = (m[2] || "mb").toLowerCase()
        return Math.round(n * (unit === "gb" ? 1024 * 1024 * 1024 : 1024 * 1024))
    }
    function refusal(key) { return Problems.title(key) + " " + Problems.action(key) }
    function load() {
        if (pairId === 0) return
        var row = null, items = sync.pairs
        for (var i = 0; i < items.length; ++i) if (items[i].id === pairId) row = items[i]
        if (!row) return
        remotePath = row.remotePath
        readerPath = row.readerPath
        direction = startDirection = row.direction
        maxFiles = startMaxFiles = row.maxFiles
        maxBytes = startMaxBytes = row.maxBytes
        startupSync = row.startupSync
        autoSync = row.autoSync
    }
    function spec(confirmed) {
        return { remoteFolder: remotePath, readerFolder: readerPath, direction: direction, maxFiles: maxFiles, maxBytes: maxBytes,
                 startupSync: startupSync, autoSync: autoSync, confirmed: confirmed }
    }
    function sends(d) { return d === "up" || d === "both" }
    function save(confirmed) {
        message = ""
        if (remotePath.length === 0) { message = qsTr("Choose the server folder first."); return }
        if (readerPath.length === 0) { message = qsTr("Choose the reader folder first."); return }
        // raising a limit names the new numbers first: it is what lets a plan that was over the limit through
        if (pairId !== 0 && !limitsOk && (maxFiles > startMaxFiles || maxBytes > startMaxBytes)) {
            limitConfirm.text = qsTr("The pair may then hold up to %1 files and %2. Continue?").arg(maxFiles).arg(sizeText(maxBytes))
            limitConfirm.open()
            return
        }
        // the confirmation is asked here as well as in the bridge, so that the dialog can name both folders
        if (!confirmed && sends(direction) && !sends(startDirection)) {
            sendConfirm.text = qsTr("Files in \"%1\" can be sent to \"%2\". Continue?").arg(readerPath).arg(remotePath)
            sendConfirm.open()
            return
        }
        var r = pairId === 0 ? sync.addPair(spec(confirmed)) : sync.updatePair(pairId, spec(confirmed))
        if (!r.ok) { message = refusal(r.errorKey); return }
        if (page.StackView.view) page.StackView.view.pop()
    }

    Component.onCompleted: load()

    TopBar {
        id: topBar
        title: page.pairId === 0 ? qsTr("New pair") : qsTr("Pair")
        leftItems: [ IconButton { objectName: "pairCancel"; text: qsTr("Cancel"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
        rightItems: [ IconButton { objectName: "pairSave"; text: qsTr("Save"); icon: "check"; onClicked: page.save(false) } ]
    }

    Column {
        id: body
        anchors { left: parent.left; right: parent.right; top: topBar.bottom }

        Text {
            objectName: "pairMessage"
            x: 30
            width: parent.width - 60
            topPadding: 12
            bottomPadding: 8
            visible: page.message.length > 0
            wrapMode: Text.Wrap
            font.pixelSize: Style.captionSize
            font.bold: true
            color: Style.ink
            text: "✗ " + page.message
        }
        InkRow {
            objectName: "pairServerFolder"
            text: qsTr("Server folder")
            value: page.remotePath.length > 0 ? page.lastName(page.remotePath) : qsTr("Choose")
            chevron: true
            onClicked: {
                var serverPage = page.StackView.view.push(Qt.resolvedUrl("FolderChooserPage.qml"), { startPath: page.remotePath.length > 0 ? page.remotePath : "/", verb: qsTr("Sync") })
                serverPage.chosen.connect(function (path) { page.remotePath = path })
            }
        }
        InkRow {
            objectName: "pairReaderFolder"
            text: qsTr("Reader folder")
            value: page.readerPath.length > 0 ? page.lastName(page.readerPath) : qsTr("Choose")
            chevron: true
            onClicked: {
                var readerPage = page.StackView.view.push(Qt.resolvedUrl("FilePickerPage.qml"), { folderMode: true })
                readerPage.chosen.connect(function (path) { page.readerPath = path })
            }
        }

        Text {
            x: 30
            width: parent.width - 60
            topPadding: 18
            bottomPadding: 6
            font.pixelSize: Style.labelSize
            font.bold: true
            color: Style.ink
            text: qsTr("Direction")
        }
        Repeater {
            model: [
                { key: "down", label: qsTr("Download only"), ready: true },
                { key: "up", label: qsTr("Upload only"), ready: page.uploadReady },
                { key: "both", label: qsTr("Two-way"), ready: page.bothReady }
            ]
            delegate: Item {
                objectName: "pairDirection"
                width: body.width
                height: dirText.y + dirText.height + 12
                opacity: modelData.ready ? 1.0 : 0.4
                readonly property bool selected: page.direction === modelData.key
                property string text: modelData.label + (selected ? " (selected)" : "") + (modelData.ready ? "" : " " + qsTr("Available soon"))   // what a walk reads
                Rectangle { anchors.fill: parent; color: Style.pressed; visible: dirTap.pressed && modelData.ready }
                Icon { id: mark; x: 30; y: 14; size: 44; source: parent.selected ? "check" : "" }
                Text {
                    id: dirLabel
                    anchors { left: mark.right; leftMargin: 20; right: parent.right; rightMargin: 30 }
                    y: 16
                    font.pixelSize: Style.bodySize
                    font.bold: parent.selected
                    color: Style.ink
                    text: modelData.label
                }
                Text {
                    id: dirText
                    anchors { left: dirLabel.left; right: parent.right; rightMargin: 30; top: dirLabel.bottom; topMargin: 2 }
                    wrapMode: Text.Wrap
                    font.pixelSize: Style.captionSize
                    color: Style.ink
                    text: !modelData.ready ? qsTr("Available soon") : (parent.selected ? sync.describeDirection(modelData.key) : "")
                    visible: text.length > 0
                }
                RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
                MouseArea { id: dirTap; anchors.fill: parent; enabled: modelData.ready; onClicked: page.direction = modelData.key }
            }
        }

        Text {
            x: 30
            width: parent.width - 60
            topPadding: 18
            bottomPadding: 6
            font.pixelSize: Style.labelSize
            font.bold: true
            color: Style.ink
            text: qsTr("Limits")
        }
        InkRow {
            objectName: "pairMaxFiles"
            text: qsTr("Most files")
            value: String(page.maxFiles)
            chevron: true
            onClicked: { filesEntry.open(String(page.maxFiles)) }
        }
        InkRow {
            objectName: "pairMaxSize"
            text: qsTr("Most data")
            value: page.sizeText(page.maxBytes)
            chevron: true
            onClicked: { sizeEntry.open(page.sizeText(page.maxBytes)) }
        }

        Item {
            width: parent.width
            height: 110
            Text { x: 30; anchors.verticalCenter: parent.verticalCenter; text: qsTr("Sync at start"); font.pixelSize: Style.bodySize; color: Style.ink }
            InkSwitch {
                objectName: "pairStartup"
                anchors { right: parent.right; rightMargin: 30; verticalCenter: parent.verticalCenter }
                checked: page.startupSync
                onToggled: function (on) { page.startupSync = on }
            }
            RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
        }
        Text {
            x: 30
            width: parent.width - 60
            topPadding: 4
            bottomPadding: 8
            wrapMode: Text.Wrap
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: qsTr("Compares when the app starts and Wi-Fi is on. Nothing changes until you approve.")
        }
        Item {
            width: parent.width
            height: 110
            Text { x: 30; anchors.verticalCenter: parent.verticalCenter; text: qsTr("Auto Sync"); font.pixelSize: Style.bodySize; color: Style.ink }
            InkSwitch {
                objectName: "pairAuto"
                anchors { right: parent.right; rightMargin: 30; verticalCenter: parent.verticalCenter }
                checked: page.autoSync
                onToggled: function (on) { page.autoSync = on }
            }
            RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
        }
        Text {
            x: 30
            width: parent.width - 60
            topPadding: 4
            wrapMode: Text.Wrap
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: qsTr("Compares shortly after a change you make here. Nothing changes until you approve.")
        }
    }

    BottomPanel {
        id: panel
        anchors.bottom: parent.bottom
        tools: [
            ToolButton {
                objectName: "pairRemove"
                text: qsTr("Remove pair")
                icon: "trash"
                visible: page.pairId !== 0
                onClicked: removeConfirm.open()
            }
        ]
    }

    TextEntryDialog {
        id: filesEntry
        objectName: "pairFilesEntry"
        title: qsTr("Most files")
        acceptText: qsTr("OK")
        onAccepted: function (text) {
            var n = /^\s*\d+\s*$/.test(text) ? parseInt(text, 10) : -1
            if (n < 1 || n > 5000) { errorText = qsTr("Use a number from 1 to 5,000."); return }
            page.maxFiles = n
            close()
        }
    }
    TextEntryDialog {
        id: sizeEntry
        objectName: "pairSizeEntry"
        title: qsTr("Most data")
        placeholder: qsTr("for example 500 MB or 2 GB")
        acceptText: qsTr("OK")
        onAccepted: function (text) {
            var b = page.parseSize(text)
            if (b < 1024 * 1024 || b > 64 * 1024 * 1024 * 1024) { errorText = qsTr("Use 1 MB to 64 GB."); return }
            page.maxBytes = b
            close()
        }
    }
    ConfirmDialog {
        id: limitConfirm
        objectName: "pairLimitConfirm"
        title: qsTr("Raise the limits?")
        acceptText: qsTr("Continue")
        onAccepted: { page.limitsOk = true; page.save(false) }
    }
    ConfirmDialog {
        id: sendConfirm
        objectName: "pairSendConfirm"
        title: qsTr("Send files to the server?")
        acceptText: qsTr("Continue")
        onAccepted: page.save(true)
    }
    ConfirmDialog {
        id: removeConfirm
        objectName: "pairRemoveConfirm"
        title: qsTr("Remove this pair?")
        text: qsTr("Files on both sides stay where they are.")
        acceptText: qsTr("Remove")
        onAccepted: {
            var r = sync.removePair(page.pairId)
            if (!r.ok) { page.message = page.refusal(r.errorKey); return }
            if (page.StackView.view) page.StackView.view.pop()
        }
    }
}
