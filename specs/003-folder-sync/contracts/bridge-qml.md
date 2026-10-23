# Contract: Bridge and QML (`bridge/`, `qml/`)

**Feature**: [../spec.md](../spec.md) | **Core contract**: [core-interfaces.md](core-interfaces.md) | **002**: [bridge-qml](../../002-manage-server-files/contracts/bridge-qml.md) | **001**: [bridge-qml](../../001-nextcloud-files-client/contracts/bridge-qml.md)

Rules of 001 and 002 stay: the bridge is the only Qt layer; all network and file work runs on the one `Worker` thread and reports
back with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`; properties are read by the UI thread only; names shown pass
`displaySafe()` first; no bridge starts a request on its own. New in this feature: **a comparison (never a transfer) may also
start from the start-up hook and from the Auto Sync timer of `SyncBridge`**, and from nothing else besides taps.

## SyncBridge (context property `sync`)

| Member | Type | Meaning |
|--------|------|---------|
| `pairs` | list of maps | the pair list: `{id, remoteFolder, readerFolder, direction, directionText, state, stateText, problemKey, lastText, heldFiles, heldBytes, maxFiles, maxBytes, usageText, planReady, planOverLimit, planCounts, startupSync, autoSync}`; names pass `displaySafe()` |
| `pairCount`, `pairLimit` | int | 8 |
| `busy`, `busyPairId`, `stage`, `progressText`, `percent` | | a comparison or run is going: `stage` is `idle`, `comparing`, `downloading`, `uploading`, `applying`, `done`; coarse text, at most one update per 2 s or 5 % |
| `offline` | bool | no Wi-Fi now |
| `lastResult` | map | `{pairId, downloaded, uploaded, skipped, keptBoth, proposalsOpen, failed, seconds, failures: [{key, count}], stoppedBy}` of the run just finished or of the pair asked for |
| `readyCount` | int | pairs with a plan ready (the mark on the Home tool) |
| `addPair(map)` | Q_INVOKABLE -> map | `{remoteFolder, readerFolder, direction, maxFiles, maxBytes, startupSync, autoSync, confirmed}`; returns `{ok, id}` or `{errorKey}` (`pair_overlap`, `pair_folder_forbidden`, `source_not_allowed`, `pair_limit_invalid`, `pair_limit`, `confirmation_needed` for a sending direction without `confirmed`). Writes the pair only; no request, no file change |
| `updatePair(id, map)` | Q_INVOKABLE -> map | same keys; a change to a sending direction needs `confirmed`; takes effect at the next comparison; calls `noteLocalAction` |
| `removePair(id)` | Q_INVOKABLE -> map | removes rows only; `{ok, queueItemsKept}` |
| `describeDirection(direction)` | Q_INVOKABLE -> string | the plain-language summary shown before saving (FR-002) |
| `compare(id)` / `compareAll()` | Q_INVOKABLE | the Sync tap (`account_changed` for a pair of another account: nothing is started): ensures the network (as 001), then `SyncRunner::compare` on the worker; `sync_busy` if one is running for the pair; opens the plan when it finishes (signal `planReady(pairId, planId)`) |
| `previewOffline(id)` | Q_INVOKABLE | the plan from stored listings, in memory, flagged stale; not approvable |
| `plan(planId)` | Q_INVOKABLE -> map | header: `{id, pairId, createdText, ageText, trigger, state, totals{...}, sizes{...}, freeText, quotaText, reserveText, overLimit, overLimitText, incomplete, incompleteNote, truncated, approvable, approveErrorKey, stale}` |
| `planPage(planId, section, page)` / `planPageCount` | Q_INVOKABLE | `section` `transfers`, `skips`, `proposals`; rows `{seq, nameText, action, actionText, sizeText, reason, reasonText, unapprovable, removed, allowed, choice}`; page size like the other lists |
| `removeEntry(planId, seq, removed)` | Q_INVOKABLE | take a transfer entry out of the plan / put it back (only while `ready`) |
| `approve(planId)` | Q_INVOKABLE | the Approve tap: `can_approve`, then `SyncRunner::run` on the worker; progress via properties; **the only way a transfer starts** |
| `cancelRun()` | Q_INVOKABLE | stops after the item in flight |
| `cancelPlan(planId)` | Q_INVOKABLE | marks it `cancelled`; touches nothing else |
| `setChoice(planId, seq, choice)` | Q_INVOKABLE | `transfer_again`, `leave`, `delete`, `` (clear); only allowed choices |
| `applyChoices(planId, confirmed)` | Q_INVOKABLE -> map | without `confirmed`: `{deletes: [{nameText, side, trash}], transfers, leaves}` for the confirmation dialog; with it: `SyncRunner::apply_choices` on the worker. **A `delete` is never applied without `confirmed`** |
| `noteAction(remotePath)` / `noteLocalAction(localPath)` | Q_INVOKABLE | called by `QueueBridge` after the user's own queuing action (`noteAction`) and by the pair dialogs; the matching pairs with `autoSync` on get `note_action`; ignored when the global Auto Sync (`queue.autoSync`) is off |
| `startupCompare()` | called once by `main.cpp` after the account check succeeded and `wifiConnected()` is true | compares, one after the other, the pairs with `startupSync`; does nothing otherwise; **never** approves |
| signals | `changed`, `planReady(pairId, planId)`, `runFinished(result)`, `comparisonFailed(pairId, errorKey)` | |

The bridge arms **one** single-shot `QTimer` (rule N3 allow-list, research R-18) from `SyncScheduler::next`. It fires `compare` for
the due pairs without ensuring the network (Auto Sync never switches Wi-Fi on); offline: the pair shows "waiting" and nothing is
retried. The timer is never armed at start, on Wi-Fi coming up, after a failure or after a run.

## QueueBridge (change)

After every action that already calls `scheduler.note_action(now)` (002), the bridge also calls
`sync.noteAction(<remote folder of the action>)`. Nothing else changes; sync uploads appear in the queue list like any other
items (`groupId` is shown), can be retried/resolved there, and are labelled with the pair's name.

## BrowseBridge / DetailsPage (change; R-15)

`rows` gain `syncedBy` (`""` or the pair id) when the entry lies inside a pair's server folder; the state of such a file is
computed from `synced_copy` when there is no 001 copy. "Remove local copy" for a synced file is replaced by a line "Managed by Sync"
and a link to the Sync screen. Opening a synced file uses `resolve_local_file(root, local_rel)`.

## FolderChooserBridge / PickerBridge (change)

`PickerBridge` gets `folderMode` (bool): the browser lists folders only and offers **Choose this folder** and **New folder** (name
checked by `sanitize_component`; created below the picker root by `create_picker_folder` of `core/src/local_files.cpp` - the bridge does no filesystem mutation itself; never over an existing entry); the app's data folder and the
download folder `PBCloud` are shown disabled with the reason. `FolderChooserBridge` is used unchanged for the server folder.

## Pages (all in `qml/pages/`, built from the existing components)

| Page | Contents |
|------|----------|
| `SyncPage` (new) | `TopBar` (Back, title "Sync"), a line with the offline state and "N plans ready", the paged pair list (`InkRow`: direction icon, "Books -> Reader/Books", state line, usage line "34 of 500 files, 410 MB of 2 GB", last result), bottom panel: **Add pair**, **Sync all**, **Preview offline** (when offline); a pair row tap opens a `ChoiceDialog`: **Sync**, **Open plan** (when ready), **Settings**, **Last result** |
| `PairPage` (new) | `TopBar` (Cancel, title "Pair" / "New pair", Save), rows: *Server folder* (opens `FolderChooserPage`), *Reader folder* (opens `FilePickerPage` in folder mode), *Direction* (three rows with a mark and the plain summary below), *Limits* (two fields: files, size with a unit), *Sync at start* and *Auto Sync* (`InkSwitch` rows with one explanatory line each: "Only compares; you decide what happens."), bottom panel: **Remove pair** (confirm dialog: "Files on both sides stay where they are."). Saving a sending direction shows the confirmation naming both folders (FR-005) |
| `PlanPage` (new) | `TopBar` (Back, title "Review"), totals block (counts and sizes per action, free space, quota, reserve), a banner for *incomplete* ("N folders were not compared"), *over limit* ("Over the file limit by N. Raise the limit or remove entries."), *stale* ("Compared 3 hours ago. Compare again."), tab line **Transfers / Skipped / Proposals**, the paged rows (`InkRow`: action icon, name, size, reason), entry tap in *Transfers*: remove/put back; in *Proposals*: a `ChoiceDialog` with the allowed choices; bottom panel: **Approve** (disabled with the reason when `approvable` is false), **Cancel**, **Compare again**, **Apply choices** (Proposals tab) |
| `ResultPage` (new) | the counts of `lastResult`, failures with reason texts and "N uploads are in the queue and need a decision" with a link to `QueuePage`, duration; bottom panel: **Done**, **Sync again** |
| `HomePage` (change) | a **Sync** tool with a mark when `readyCount > 0`; state line adds "Sync: N plans ready" |
| `SettingsPage` (change) | the Auto Sync line gets "Also applies to pair syncs (they only compare)." |
| `DetailsPage` (change) | "Managed by Sync" line for a synced file (above) |
| `FilePickerPage` (change) | the folder mode (above) |

The confirmation for deletions (`applyChoices` without `confirmed`) is one `ConfirmDialog` per file naming the file and the side
(FR-026), with the texts below; there is no "delete all" shortcut.

UI rules (constitution VI, XI; checked by `check-ui-style.sh` and `app_self_test`): no bordered text button, no animation, icon
plus label tools in the bottom panel, hairline separators, page buttons instead of scrolling, progress as text updated coarsely,
every dialog with footer buttons, every text through `qsTr` with a German entry in `qml/strings_de.tsv`
(`scripts/extract-strings.py --check`). Icons used exist in `qml/icons/` (`sync`, `folder`, `check`, `trash`, `plus-circle`,
`arrow-down`, `arrow-up`, `arrows-both`, `warning`); a missing glyph is added in the same style by `scripts/make-icons.sh`.

## Texts that must exist (English source; German in `strings_de.tsv`)

- Directions: "Download only: files from the server are copied to the reader. Nothing on the reader is sent." / "Upload only:
  new files from the reader are sent to the server. Nothing from the server is copied." / "Two-way: new and changed files are
  copied in both directions. Nothing is ever deleted automatically."
- Sending-direction confirmation: "Files in "<reader folder>" can be sent to "<server folder>". Continue?"
- States: "Never synced", "Plan ready - <n> downloads, <m> uploads", "In sync", "Waiting: the reader is offline", "Interrupted",
  "Server folder not found", "Reader folder not available", "Plan ready, over the limit", "Other account".
- Plan: "Review before anything changes", "Nothing has been changed yet.", "Over the file limit by <n>. Raise the limit or remove entries.",
  "<n> folders were not compared; deletions are not proposed.", "Compared <age> ago. Compare again before approving.",
  "Not enough free space (a reserve of <size> is kept)."
- Skip reasons: one short line each for every key of data-model.md ("Already in sync", "Kept both versions", "Name not allowed on the
  server", "Changed while syncing - next time", "Waiting in the queue", ...), all without names or addresses.
- Conflict: "Changed on both sides. Both versions are kept: the other version is added as "<name>"."
- Proposals: "Deleted on the server: "<name>"", "Deleted on the reader: "<name>"", choices **Copy it back**, **Leave as it is**,
  **Delete on the server (goes to the trash)**, **Delete on the reader**; confirmation: "Delete "<name>" on the server? It goes
  to the server's trash." / "Delete "<name>" from the reader? It cannot be recovered with this app." / "The file on the reader has
  changed. It will not be deleted."; folder proposal: ""<folder>" and <n> synced files".
- Result: "<n> downloaded, <m> uploaded, <k> skipped, <j> kept both, <p> proposals open, <f> failed", "<n> uploads are in the queue
  and need a decision.", "A sync is already running for this pair."
- One message plus one "what to do" line per new refusal key of data-model.md (`pair_overlap`, `pair_folder_forbidden`, ...).

## As built

- `SyncBridge::setChoice(planId, seq, choice)` and `applyChoices(planId, confirmed)` (without `confirmed`: `{ok, deletes: [{nameText, side, trash, folder, recordsBelow}], transfers, leaves}`, nothing applied; with it the run starts and `runFinished` follows).
- Plan header: `overLimit`, `overLimitText` ("Over the file limit by N."); plan rows: `copyReader`, `copyServer` (the names a keep-both adds), `folder`, `recordsBelow`, `allowed`, `choice`.
- Result map: `deleted`. Pair row text "Interrupted: n of m done" from the interrupted plan. Queue rows: `syncedBy` (the pair's server folder).
- `stage` also takes `uploading` and `applying`.
- `PairPage` asks for a confirmation that names the new numbers when a limit is raised.
