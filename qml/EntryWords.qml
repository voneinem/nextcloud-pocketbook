pragma Singleton
import QtQuick

// The words for what the lists say about an entry: the state of its copy, where a search result came from, and the age of a list.
// One place, so that the folder, the favorites, the search and the details say the same thing.
QtObject {
    function stateWord(state) {
        switch (state) {
        case "not_downloaded": return qsTr("Not downloaded")
        case "downloaded": return qsTr("Downloaded")
        case "outdated": return qsTr("Outdated")
        case "changed_on_reader": return qsTr("Changed on the reader")
        case "conflict": return qsTr("Conflict")
        case "removed_on_server": return qsTr("Removed on the server")
        case "missing_locally": return qsTr("Missing on the reader")
        default: return ""
        }
    }

    function sourceWord(source) {
        switch (source) {
        case "server": return qsTr("From the server")
        case "favorites": return qsTr("From your favorites")
        case "download": return qsTr("From a downloaded file")
        case "reader": return qsTr("From this reader")
        default: return ""
        }
    }

    function ageText(kind, count) {
        switch (kind) {
        case "just_now": return qsTr("Updated just now")
        case "minutes": return qsTr("Updated %n minute(s) ago", "", count)
        case "hours": return qsTr("Updated %n hour(s) ago", "", count)
        case "days": return qsTr("Updated %n day(s) ago", "", count)
        case "gone": return qsTr("Removed on the server. This is the last list the reader knows.")
        default: return qsTr("Not synced")
        }
    }
}
