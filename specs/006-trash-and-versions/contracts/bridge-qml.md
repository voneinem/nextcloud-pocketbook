# Contract: Bridge and QML (`bridge/`, `qml/`)

**Feature**: [../spec.md](../spec.md) | **Core contract**: [core-interfaces.md](core-interfaces.md) | **004**: [bridge-qml](../../004-share-links/contracts/bridge-qml.md) | **002**: [bridge-qml](../../002-manage-server-files/contracts/bridge-qml.md) | **001**: [bridge-qml](../../001-nextcloud-files-client/contracts/bridge-qml.md)

Rules of 001-005 stay: the bridge is the only Qt layer; all network work runs on the one `Worker` and reports back with
`QMetaObject::invokeMethod(..., Qt::QueuedConnection)`; properties are read by the UI thread only; names shown pass `display_safe()`;
no bridge starts a request on its own. **No timer is added** (rule N3 unchanged). There is no secret in this feature.

## TrashBridge (context property `trash`; `bridge/include/trash_bridge.h`)

| Member | Type | Meaning |
|--------|------|---------|
| `open()` | Q_INVOKABLE | the Trash tap: T1 then T2 on the worker; offline: shows the stored list (`fromStore`), sends nothing; `busy` if a request runs. Discards any `PendingDestruction` |
| `refresh()` | Q_INVOKABLE | T1 + T2 again (the Refresh tool); discards a pending confirmation |
| `state` | string | `closed`, `checking`, `loading`, `listed`, `explainOff`, `explainUnknown`, `failed`, `working` |
| `availability` | map | `{trash, versions}` each `on` / `off` / `unknown` (read from T1) |
| `offline`, `busy`, `fromStore` | bool | no Wi-Fi now; a request is running (a second action answers `{errorKey: "busy"}`, not queued); the shown list is the stored copy |
| `fetchedAtText` | string | the age of the list ("not synced" if never) as the other lists show it |
| `rows` | list of maps | the shown page: `{index, name, originalText, deletedText, sizeText, isFolder, unreadable, actionable, chosen}`; `originalText` is the original folder or "unknown location"; no `trash_name` is exposed |
| `page`, `pageCount`, `pageSize`, `total`, `truncated` | | page buttons as the other lists; `truncated` shows "N items shown; more exist" |
| `choose(index)` | Q_INVOKABLE | marks exactly one item as chosen (a second `choose` moves the mark); the page shows "Chosen: <name>" (spec FR-006); `chosen` in `rows` |
| `chosenName` | string | the chosen item's display name or "" |
| `deleteEnabled` | bool | read-only, from `Config::delete_enabled`; the page shows Delete for good and Empty trash only when true |
| `restore()` | Q_INVOKABLE | starts the restore of the chosen item: T3 probes -> `plan`; emits `restorePlan(plan, proposal)` |
| `restoreConfirmed()` | Q_INVOKABLE | after the confirmation of the proposed place: T4 |
| `chooseTarget(folderPath)` | Q_INVOKABLE -> map | the result of the folder chooser: `{needsName, ...}` (the name is taken there) or `{needsConfirm, summary}`; checks `check_restore_target` and probes |
| `setTargetName(name)` | Q_INVOKABLE -> map | validates a new name (no replace option exists); returns `{ok}` or `{errorKey}` |
| `restoreChosenConfirmed()` | Q_INVOKABLE | after confirmation: T5 |
| `beginDelete()` | Q_INVOKABLE -> map | for the chosen item: creates a `PendingDestruction` and returns the content of the first confirmation (`ConfirmContent` as a map: `kind`, `names`, `items`, `folders`, `stage`, `truncatedNotice`, `destructive`, `defaultCancel`); sends nothing |
| `beginEmpty()` | Q_INVOKABLE -> map | creates the `PendingDestruction` for the whole held list (the named set) and returns the first confirmation; sends nothing |
| `confirmStep()` | Q_INVOKABLE -> map | advances: returns the next confirmation (stage 2) or, at the last stage, executes T8 (one item or the loop) and returns `{started: true}`; without a pending destruction it does nothing and returns `{errorKey: "nothing_pending"}` |
| `cancelStep()` | Q_INVOKABLE | discards the `PendingDestruction`; also called by the page when it closes |
| `cancelRun()` | Q_INVOKABLE | stops an Empty-trash loop between two requests |
| `progress` | map | `{done, total, removed, notRemoved}` while an Empty-trash loop runs (updated at most every 10% or 25 items) |
| `result` | map | `{kind, key, text, hint, code, removed, notRemoved, notTried, unknown}` of the last action; contains no name or location |
| signals | `changed`, `opened`, `restorePlan(plan, proposal)`, `actionFinished(result)`, `closed` | |

`Config` (set by `main.cpp`): `Store*`, `Worker*`, `HttpTransport*`, `Clock*`, `wifi_up`, `ensure_network`, `format_date`, `user_id`,
`account_id`, `caps` (the stored `ServerCaps` for `check_new_name`), `delete_enabled` (false until the constitution amendment is
merged; hides Delete for good and Empty trash and makes `beginDelete`/`beginEmpty` answer `{errorKey: "not_available"}`).
`PendingDestruction` is also discarded when `ConnectionBridge` reports that the account changed or was disconnected, and when the app is
switched away (as 004's `clearSecrets` is).

Bridge rules:

* the only caller of `TrashApi::delete_item` is `confirmStep()` at the last stage with a matching token (rule N12);
* no `qDebug`/`qWarning`/`qInfo` and no `log::text`/`log::path` in the trash bridges; they log only `trash.*` / `versions.*` with
  `log::status` and `log::count`;
* after a confirmed restore the bridge clears the cached folder versions above the affected path (research R-14) and removes the item from
  the stored list; it touches no file on the reader, no offline mark and no queue item.

## VersionsBridge (context property `versions`; `bridge/include/versions_bridge.h`)

| Member | Type | Meaning |
|--------|------|---------|
| `open(remotePath)` | Q_INVOKABLE | the Versions tap on `DetailsPage` of a file: T1, then T3 (file stamp) and T6; a folder or a file without a known id answers `{errorKey: "refresh_first"}` / is not offered |
| `reload()` | Q_INVOKABLE | T3 + T6 again |
| `state` | string | `closed`, `checking`, `loading`, `listed`, `empty`, `explainOff`, `explainUnknown`, `failed`, `working` |
| `fileName`, `offline`, `busy` | | the heading (`display_safe()`), connection state, request running |
| `rows` | list of maps | the shown page: `{index, dateText, sizeText, future, chosen}`; the current content is not a row |
| `page`, `pageCount`, `pageSize`, `total`, `truncated` | | page buttons |
| `choose(index)` | Q_INVOKABLE | marks one version |
| `restore()` | Q_INVOKABLE -> map | returns the confirmation (`RestoreVersion`: file name, version date and size, "the current content is kept as one more version"); sends nothing |
| `restoreConfirmed()` | Q_INVOKABLE | the stale check (T3) then T7; reports `version_changed_meanwhile` without sending when the file changed |
| `result` | map | `{key, text, hint, code}` of the last action |
| signals | `changed`, `opened`, `actionFinished(result)`, `closed` | |

## Pages and dialogs (`qml/pages/`)

| Page / change | Content |
|---------------|---------|
| `TrashPage.qml` (new) | `TopBar` ("Trash", with the path line "Chosen: <name>"); `ModelPager` list of `InkRow`s (name, "from <original folder>", date, size, a check mark on the chosen row); the age line and the offline/stored note; `BottomPanel` tools **Restore**, **Delete for good** (icon trash, only when `delete_enabled`), **Empty trash**, **Refresh**; the tools that need the server are disabled with the reason line when offline; the explanation panes for `explainOff` and `explainUnknown` replace the list and offer no action |
| `VersionsPage.qml` (new) | `TopBar` ("Versions of <file>"); `ModelPager` list (date, size); tools **Restore**, **Reload**; the explanation panes as above; "No older versions" in the empty state |
| `HomePage.qml` (changed) | a "Trash" entry (same row style as the others) |
| `FolderPage.qml` (changed) | a **Trash** tool in the normal-mode bottom panel (DT-T1 checks the fit) |
| `DetailsPage.qml` (changed) | a **Versions** tool for a file (not offered for a folder: a folder has no details page) |
| `FolderChooserPage.qml` (changed) | property `serverOnly` (default false): hides "New folder" and queued folders; the choice returns through the existing signal |
| `ConfirmDialog.qml` (reused) | the proposed restore place, the single delete of a file, a version restore; `defaultCancel` |
| two successive `ConfirmDialog`s | folder delete for good and Empty trash: the second is opened only by the first's confirm; both `destructive: true`, `defaultCancel: true`; the second text begins with "This is the last step." |
| `TextEntryDialog.qml` (reused) | a new name when the chosen name is taken (rename validation; no replace button) |
| `ChoiceDialog.qml` (reused) | "The place is taken / the folder is gone: choose another folder" with Choose folder and Cancel |
| `Problems.qml` (changed) | title/action per new key (the list in core-interfaces.md) |

Dialog wording (source text, English; German in `strings_de.tsv`), none of it from the server:

| Key | Source text |
|-----|-------------|
| Restore place | "Restore <name> to <folder>?" / "It goes back to its original place." / "You chose this place." |
| Place taken | "<name> cannot go back to its original place: the place is taken or the folder is gone. Choose another folder." |
| Delete file for good | "Delete <name> for good? This cannot be undone." |
| Delete folder, step 1 | "Delete the folder <name> and everything in it for good? This cannot be undone." |
| Empty trash, step 1 | "Delete <N> items for good (<F> folders with their content), among them <names>? This cannot be undone." |
| Last step | "This is the last step. Delete <what> for good? This cannot be undone." |
| Trash off | "This server has no trash app enabled. Deleted files cannot be listed or restored from here, and deleting on this server is permanent. Ask the server's administrator." |
| Versions off | "This server does not keep older versions, so none can be listed or restored. Ask the server's administrator." |
| Unknown | "The server's answer was not clear. Try again later. (Code <code>)" |
| Version restore | "Restore <file> to the version of <date>, <size>? The current content is kept as one more version." |
| Stale | "The file has changed since the list was read. Reload the list." |
| Unsupported place | "This server can restore only to the original place. Rename or move the file that is there, then restore again." |

## Texts and checks

* Every new string is added to `qml/strings_de.tsv` in the same task; `scripts/extract-strings.py --check` stays green.
* `scripts/check-ui-style.sh` gains one rule with a self-test: a `ConfirmDialog` that sets `destructive: true` must also set
  `defaultCancel: true`.
* The app self-test instantiates both pages and the changed ones; `qml` files are in the package.
