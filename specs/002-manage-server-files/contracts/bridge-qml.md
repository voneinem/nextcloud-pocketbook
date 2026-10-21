# Contract: Bridge and QML (`bridge/`, `qml/`)

**Feature**: [../spec.md](../spec.md) | **Core contract**: [core-interfaces.md](core-interfaces.md) | **001**: [bridge-qml](../../001-nextcloud-files-client/contracts/bridge-qml.md)

Rules of 001 stay: the bridge is the only Qt layer; all network and file work runs on the one `Worker` thread and reports back
with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`; properties are read by the UI thread only; names shown pass
`displaySafe()` first; no bridge starts a request on its own. New in this feature: **a request may also start from the Auto
Sync timer** (one single-shot `QTimer` in `QueueBridge`, research R-14) and from nothing else besides taps.

## QueueBridge (context property `queue`)

| Member | Type | Meaning |
|--------|------|---------|
| `openCount` | int | items not `done` (shown on the Queue tool) |
| `items` (paged, `pageSize` like the other lists) | list of maps | `{id, kind, entryKind, name, targetText, sizeText, state, reason, reasonText, progressText, waitingFor, canRetry, canResolve, groupId}`; `state` is the key QML translates: `queued` ("Not synced"), `sending`, `done`, `failed`, `conflict`, `unknown`, `waiting` (a dependency is not done), `other_account` |
| `page`, `pageCount`, `pageSize`, `total` | int | local only |
| `autoSync` | bool | the setting (read/write; writing persists and takes effect at once) |
| `running`, `runStage`, `runProgressText`, `runPercent` | | a send is running; coarse text updated at most every 2 s or 5 %; `runStage`: `idle`, `preparing`, `sending`, `done` |
| `offline` | bool | no Wi-Fi now |
| `lastRunText`, `lastRunCounts` | | from `last_send_at` / `last_send_counts` |
| `waitingNotice` | string | `""` or `offline` / `rate_limited` / `maintenance` / `signin`: why the queue is waiting (FR-013) |
| `addUploads(targetFolder, paths)` | Q_INVOKABLE -> map | queues file picks; returns `{queued, asks: [...], refused: [{path, key}]}` where `asks` are collisions the QML must answer first |
| `addFolderPick(targetFolder, folderPaths)` | Q_INVOKABLE -> map | counts a folder pick (bounded walk) and returns `{folders, files, bytes, overLimit, refused}`; nothing queued yet |
| `confirmFolderPick(token, answers)` | Q_INVOKABLE | queues the group (all or none) after the counts were confirmed; `answers` = per-name collision answers or `apply_all` |
| `answerCollision(askId, choice, newName)` | Q_INVOKABLE | `keep_both`, `replace`, `skip`; the QML has shown the replace confirmation before it passes `replace` |
| `newFolder(parent, name)` | Q_INVOKABLE -> map | validates (`ok` or `{errorKey}`), asks nothing if the name exists (returns `exists`), else queues |
| `rename(remotePath, newName)` | Q_INVOKABLE -> map | validates; collision -> `asks` |
| `moveOrCopy(kind, remotePaths, targetFolder)` | Q_INVOKABLE -> map | `kind` `move` or `copy`; refuses a target that is the source, inside it or the same place; collisions -> `asks`; entries without a strong ETag -> `refreshFirst` |
| `deleteEntries(remotePaths, confirmed)` | Q_INVOKABLE -> map | without `confirmed` returns the confirmation data `{names, folders, itemsInside, deleteOffered}`; with it queues; `deleteOffered` false -> the QML shows the no-trash text and nothing is queued |
| `send()` | Q_INVOKABLE | the Send tap: ensures the network (as 001), then runs the queue |
| `cancelSend()` | Q_INVOKABLE | stops after the piece in flight |
| `retry(id)`, `remove(id)`, `sendAgain(id)` | Q_INVOKABLE | per item; `remove` of a running item is refused |
| `resolve(id, choice, newName)` | Q_INVOKABLE | held items: `keep_both`, `replace_anyway`, `apply_anyway`, `discard`, `keep_entry`; the QML asks the second confirmation before `replace_anyway`/`apply_anyway`/delete-anyway |
| `checkUnknown(id)` | Q_INVOKABLE | the read-only check; result in `checkResult` (`arrived`, `not_there`, `something_else`, `unclear`) |
| `markDone(id)` | Q_INVOKABLE | after a check says `arrived` (or the user decides) |
| `leaveScreen()` | Q_INVOKABLE | `queue_clear_done()` |
| signals | `changed`, `sendFinished(report)`, `checkFinished(id, result)`, `ask(askId, kind, name, existingInfo)` | |

The bridge calls `scheduler.note_action(now)` after `addUploads`, `confirmFolderPick`, `answerCollision` (queued),
`newFolder`, `rename`, `moveOrCopy`, `deleteEntries` (confirmed), `remove`, `resolve`; never after anything else. When the
scheduler says `Wait(ms)` it arms the one single-shot timer; on `Start` it posts `send()` **without** ensuring the network
(Auto Sync never switches Wi-Fi on). At app start it arms nothing.

`displaySafe()` is applied to every `name`, `targetText`, `waitingFor`.

## PickerBridge (context property `picker`)

| Member | Type | Meaning |
|--------|------|---------|
| `rootDirectory` | string | the storage root (`/mnt/ext1`; host: `PBC_PICKER_ROOT`) |
| `topFolders` | stringlist | `Audio Books, Downloads, Music, My pictures, Notes, Photo, screens, PBCloud` (research R-13; case-insensitive whole-name match; the last is the download folder) |
| `view` | string | `list` / `tile`, read/write, persisted as `picker_view`; invalid value ignored |
| `listDirectory(path)` | Q_INVOKABLE -> list | `{name, dir, size, selected, error}`; at the root only the existing top folders; elsewhere folders first, case-insensitive order; hidden, links and non-regular entries skipped; empty for an unreadable path |
| `toggle(path)` | Q_INVOKABLE | selects/unselects a file; refuses (message key) 0-byte, 4 GiB or more, or an unacceptable name |
| `addFolder(path)` | Q_INVOKABLE -> map | the bounded walk; selects the folder with its counts; `overLimit` -> refused |
| `selection`, `selectionText` | list, string | "<n> files, <m> folders, <size>" |
| `clear()` | Q_INVOKABLE | |
| `maxItems` | int | the queue headroom (2,000 - open items) |

The page is `FilePickerPage.qml`. It returns the selection to the caller (`FolderPage`), which calls `queue.addUploads` /
`queue.addFolderPick`. The picker itself reads no file content and sends nothing.

## BrowseBridge (changes)

| Member | Change |
|--------|--------|
| `rows` | each row gains `pending`, `syncState`, `itemId` (data-model "Listing overlay") and `selected` (select mode); built from `entries_page_overlay` |
| `selecting`, `selectedCount` | select mode (local only) |
| `toggleSelect(remotePath)`, `selectAllOnPage()`, `clearSelection()`, `selectedPaths` | Q_INVOKABLE / property |
| `open(remotePath)` | a pending row opens the queue item (`showQueueItem(itemId)` signal), not details or a listing |
| `deleteOffered` | bool: the last account check reported a trash |
| `folderChooser(path)` | Q_INVOKABLE: the folder-only rows for `FolderChooserPage` from stored listings; fetches a never-fetched folder only when online and the user opened it |

## TransferBridge / SpaceBridge

Unchanged, except that `DetailsPage` shows the "not synced" mark for an entry with an open item and `SpaceBridge` ignores the queue.

## SettingsBridge (changes)

None for Auto Sync: the switch is `queue.autoSync` (the setting lives with the queue). `SettingsBridge` is unchanged.

## DeviceBridge (existing)

`wifiConnected()` and `connectNetwork()` are used as they are; the Auto Sync path calls only `wifiConnected()`.

## Pages (all in `qml/pages/`, built from the existing components)

| Page | Contents |
|------|----------|
| `FilePickerPage` | `TopBar` (Cancel, title "Upload", Up, layout toggle: grid/list icon), path line, rows or a tile grid (glyph + extension + name, two lines elided, a check mark when selected), `PageButtons`, bottom panel: **Upload** (n selected; disabled at 0), **Add this folder** (not at the top level), **Clear**; messages for refused picks; the sibling's 400 ms tap guard and empty-state text ("Nothing to browse here. The folders ... were not found.") |
| `FolderChooserPage` | `TopBar` (Cancel, title "Choose a folder", Up), path line with the listing age, folder rows only, `PageButtons`, bottom panel: **Choose this folder**, **New folder**; shows "Offline: stored listings only" when offline |
| `QueuePage` | `TopBar` (Back, title "Queue"), state lines ("Offline", "Last sent 14:32: 3 sent, 1 failed", "Waiting: ..."), the paged item list (`InkRow`: icon, name, target, state word, progress text), bottom panel: **Send**, **Cancel**, **Retry**, **Remove**, **Resolve** (the last three act on the selected row; enabled by the row's state) |
| `FolderPage` (changed) | tools: **Refresh**, **Keep offline**, **Add** (choice: Upload files / New folder), **Select**, **Sort**, **Queue** (n); Update, Favorites, Search move to Home only (research R-16, owner point 3). Select mode: rows with check marks; tools **Rename** (one entry), **Move**, **Copy**, **Delete**, **Done**. Rows with `pending`/`syncState` show the mark and the word. |
| `HomePage` (changed) | state line adds "Queue: n not synced" when n > 0; a **Queue** tool |
| `SettingsPage` (changed) | an **Auto Sync** switch row (same wording, position and `InkSwitch` as PB Notes), default on, with one explanatory line |
| `DetailsPage` (changed) | the "not synced" mark when the entry has an open item |
| `AboutPage` (changed) | the sentence that the app writes to the server only on the user's action |

The choices for a held item (conflict, collision, unknown outcome) open as a `ChoiceDialog` with footer buttons from `QueuePage`; there is no extra page.

UI rules (constitution VI, XI; checked by `check-ui-style.sh` and `app_self_test`): no bordered text button, no animation,
icon plus label tools in the bottom panel, hairline separators, page buttons instead of scrolling, progress as text updated
coarsely, every dialog with footer buttons, every text through `qsTr` with a German entry in `qml/strings_de.tsv`
(`scripts/extract-strings.py --check`). Icons used exist in `qml/icons/` (`attach`, `send`, `plus-circle`, `select-all`,
`check`, `copy`, `cut`, `paste`, `pencil`, `trash`, `folder`, `grid`, `list`, `sync`); a missing glyph is added in the same
style by `scripts/make-icons.sh`, not a text button.

## Texts that must exist (English source; German in `strings_de.tsv`)

- State words: "Not synced", "Sending", "Sent", "Failed", "Conflict", "Outcome unknown", "Waiting for another item",
  "Moving", "Delete pending", "Other account", "Offline", "Waiting: the reader is offline / the server is busy / sign in again".
- Collision question: "A file named "<name>" is already in this folder." with **Keep both** / **Replace** / **Skip** (and, for a
  folder pick, "Apply to all remaining"); replace confirmation: "Replace "<name>" on the server? The file on the server will be
  replaced." (files only); conflict confirmation: "The file on the server has changed since you last saw it. Replace it
  anyway?"; for move/copy/delete: "This entry has changed on the server. Apply anyway?".
- Delete: "Delete "<name>"? It goes to the server's trash." / "Delete "<name>" and everything inside it? It goes to the server's
  trash." / "<n> queued items inside this folder will fail." / no trash: "This server has no trash. Deleting here would be
  permanent, so PB Cloud does not offer it."
- Unknown outcome: "The reader could not tell whether this reached the server." with **Check the server** / **Send again** /
  **Remove**; results: "It most likely arrived." / "It is not there." / "Something else with this name is there." / "Could not
  tell."
- The folder-pick confirmation: "<n> folders and <m> files, <size>. Add them to the queue?"
- One message plus one "what to do" line per failure key of server-requests.md, all without names or addresses.
- Auto Sync: label "Auto Sync", line "Send changes shortly after you make them. Off: send with the Send button."


## Changes made during implementation (2026-10-21)

- `QueueBridge`: `answerCollision(askId, choice, newName)`, `newFolder(parent, name)`, `rename(path, name)` (a taken name returns `asks`), `moveOrCopy(kind, paths, target)`,
  `deleteEntries(paths, confirmed)` (without `confirmed`: `{names, entries, folders, itemsInside, deleteOffered, trashKnown}`), `prepareResolve(id)` with `resolvePrepared`,
  `resolve(id, choice, newName)` with `resolveFinished` (choices `keep_both`, `replace_anyway`, `apply_anyway`, `edit`, `discard`/`skip`/`keep_entry`), `checkUnknown`, `markDone`,
  `sendAgain`, `retry`, `cancelSend`, `autoSync`, `waitingNotice`, `lastRunText`, `lastRunCounts`, `checkResult`.
- `FolderChooserBridge` (context property `chooser`): `open(path)`, `up()`, `refresh()`, paging, `rows` (folders only, queued folders included), `hasListing`, `busy`, `offline`.
- Pages and dialogs: `QueuePage`, `FolderChooserPage`, `FilePickerPage` (with the target-folder tool), `CollisionDialog`, the select mode and dialogs of `FolderPage`.
