# Research: Trash and File Versions

Decisions for [plan.md](plan.md). Each entry: **Decision**, **Rationale**, **Alternatives**. Everything about server behavior is
**unverified** until a live run (docs/live-server-checklist.md, LV-T1..LV-T10, added by the task list; constitution IX). The server
facts below come from the public documentation of the trashbin and versions DAV endpoints and from general knowledge of how they behave;
none of it was seen in this project.

## R-1 The requests (server contract)

**Decision.** Eight requests, details in [contracts/trash-requests.md](contracts/trash-requests.md):

| ID | Request | Purpose |
|----|---------|---------|
| T1 | `GET` OCS capabilities (the call 004 L1 and 001 use) | is the trash app on, is the versions app on |
| T2 | `PROPFIND` Depth 1 on `<base>/remote.php/dav/trashbin/<uid>/trash/` | the trash list |
| T3 | `PROPFIND` Depth 0 on a files path (the existing `fetch_entry`) | does the original place exist, does its folder exist; the file's current ETag before a version restore |
| T4 | `MOVE` of a trash item to `<base>/remote.php/dav/trashbin/<uid>/restore/<trash-name>` | restore to the original place |
| T5 | `MOVE` of a trash item to a files path with `Overwrite: F` | restore to a chosen place (R-6) |
| T6 | `PROPFIND` Depth 1 on `<base>/remote.php/dav/versions/<uid>/versions/<fileid>/` | the version list of a file |
| T7 | `MOVE` of a version to `<base>/remote.php/dav/versions/<uid>/restore/target` | restore a version |
| T8 | `DELETE` of one trash item `.../trashbin/<uid>/trash/<trash-name>` | delete for good, and one step of Empty trash |

`<uid>` is the user id the account check already holds (the same value `files_url` uses). `<fileid>` comes from the listing's `oc:fileid`
of the file (`RemoteEntry::fileid`); a file whose id is unknown (entry from an old stored listing) is refreshed first or the Versions tool
says "refresh first". Every URL is built on the session's origin, with each path segment percent-encoded as `files_url` does; no address
from an answer is ever requested (the unique trash name and version id are taken from the answer as *names*, validated as single path
segments, and the URL is built by the app).

**Rationale.** The smallest set that covers the spec; reads reuse PROPFIND and `xml_min`; writes are two verbs in one audited module.

**Alternatives.** The OCS "files_trashbin" API: it does not exist for listing; DAV is the documented route. A `DELETE` of the whole
trash collection for Empty trash: rejected (R-8).

## R-2 Capability states (spec FR-027 to FR-030)

**Decision.** `TrashAvailability` holds two `FeatureState` values, **On / Off / Unknown**, one for the trash and one for versions, read
by T1 when the user opens the trash or the Versions list (not at start; the answer is not stored). Rules, from the capabilities JSON
(`ocs.data.capabilities`):

| Situation | Trash state | Versions state |
|-----------|-------------|----------------|
| `files.undelete` is `true` | On | |
| `files.versioning` is `true` | | On |
| the `files` object is present and the key is missing or `false` | Off | Off |
| the request failed, answered a non-success status, or the JSON is unusable (no `files` object) | Unknown | Unknown |

The two states are judged independently. **Off** shows the plain explanation of spec FR-028/FR-029; **Unknown** shows the generic message
with a short code and never claims an app is disabled. A real answer from the trashbin endpoint that contradicts the capability (for
example 404 while the capability said On) is **Unknown with a code**, not Off. This extends `ServerCaps` read-only: the existing
`ServerCaps::trash` (used by Delete in feature 002) keeps its meaning; a new pure function `availability_from_capabilities(const
JsonValue&)` in `trash_rules` makes the tri-state, and `TrashApi::fetch_availability()` makes the request.

**Rationale.** Spec FR-027 forbids guessing from an empty list or an error. A good capabilities answer without the key is the server's
own statement that the app is off (it announces the key only when the app is enabled - unverified, LV-T1); a failure says nothing.

**Alternatives.** Probing the trashbin endpoint and reading 404 as Off: a 404 can also come from a mistyped base path or a proxy. Reusing
`ServerCaps::trash` (nullopt = unknown): it cannot tell "answered, key missing" from "no answer", which is exactly the distinction FR-030
needs.

## R-3 Parsing the trash and version answers (hostile input)

**Decision.** `trash_parse` turns a 207 multistatus into `TrashList` and `VersionList` with the same tolerance as `parse_listing`:
`xml_min`, a 4 MiB (trash) or 1 MiB (versions) body cap, 1,000 / 200 rows kept after sorting, `listing_too_large` when the cap is hit.
Per item:

- **Trash name** (the last segment of the `d:href`, percent-decoded once): must be one path segment, 1 to 255 bytes, valid UTF-8, no `/`,
  `\`, control characters or `..`. Otherwise the item is **unreadable**: listed (`unreadable = true`, name shown as "Unreadable item"),
  not restorable, not deletable (it cannot be addressed safely), counted.
- **Display name** (`nc:trashbin-filename`): sanitized for display only (control characters removed, at most 255 bytes cut on a character
  boundary); if missing, the trash name without its `.d<digits>` suffix.
- **Original location** (`nc:trashbin-original-location`): a relative path such as `Books/story.epub`; normalized (leading `/` removed),
  split into segments, each checked like a name; any `..`, empty inner segment, control character or more than 1,024 bytes makes the
  **original place unknown** (the item stays restorable, but only to a chosen place). Directory part = original folder; last segment =
  original name.
- **Deletion time** (`nc:trashbin-deletion-time`, epoch seconds): a number in [0, now + 1 day]; a missing or impossible value is shown as
  "unknown date" and sorts last.
- **Kind and size**: `d:resourcetype` collection = folder; `d:getcontentlength` for files only, a non-negative integer, otherwise
  unknown.
- **File id** (`oc:fileid`) kept when numeric, for display ordering only.
- **Availability** (spec FR-005): `Encrypted` when `nc:is-encrypted` is `1`; `NoPermission` when `oc:permissions` is present and
  contains no `D` (the user may not delete or restore it); `Unsupported` when `d:resourcetype` holds an element that is neither empty
  (a file) nor `d:collection`; otherwise `Ok`. A missing property never makes an item unavailable (unknown stays `Ok`; the server's
  refusal then has the last word). All three property shapes are unverified (LV-T2).
- A member with a non-2xx propstat is counted as `partial` and skipped (spec FR-004); the other members are unaffected.
- Sort: deletion time descending, then trash name ascending; the sort is made in the core, not trusted from the server.

Version rows: the last href segment is the version id (single segment, digits only, 1 to 20 characters, else skipped and counted);
`d:getlastmodified` (RFC 1123) -> `mtime`, `d:getcontentlength` -> size; a missing date is "unknown date" (spec edge case: never hidden); a future
date is kept and flagged; order is newest first by `mtime`, ties by id descending.

**Rationale.** Constitution I and the Security constraints: every parser of untrusted input has hostile tests, a malformed item is shown as
unreadable, never crashes. **Alternatives.** Trusting the server's order or names: rejected (spec FR-004).

## R-4 The stored trash list: schema 6

**Decision.** Schema 6 adds `trash_items(position INTEGER PRIMARY KEY, trash_name TEXT NOT NULL, name TEXT NOT NULL, original_dir TEXT,
original_name TEXT, deleted_at INTEGER, kind INTEGER NOT NULL, size INTEGER, fileid INTEGER, flags INTEGER NOT NULL)` and the settings
`trash_fetched_at` (epoch seconds), `trash_truncated` (0/1), `trash_account` (the account row the list belongs to). `replace_trash(list,
fetched_at)` deletes all rows and inserts the new ones in one transaction (so a kill leaves the old list or the new one, never half). The
table holds at most 1,000 rows (the parser's bound) and is cleared when the account changes or is removed (`trash_account` mismatch) and
by "Clear stored data" if the app has one. **Only the last list**; the versions list is never stored. The migration is one-way, tested
with the existing migration harness, and an older build refuses the newer database as already required.

**Rationale.** Spec FR-003 needs a list that survives restarts for the offline case; 1,000 rows of names is well under 1 MB. Replace in
one transaction keeps the store consistent without a journal.

**Alternatives.** Keep the list only in memory: no offline view after a restart (spec Story 1 scenario 4). Store versions too: they are
needed only while online and the server holds them; storing adds a second table for no requirement.

## R-5 Restore to the original place

**Decision.** `plan_restore(item, probe)` (pure, in `trash_rules`) decides before any write, from one or two T3 probes made when the user
taps Restore:

| Probe result | Plan |
|--------------|------|
| original place unknown (R-3) | `ChoosePlace` (no original to propose) |
| the original **folder** is missing (404 on its Depth 0 PROPFIND) | `ParentMissing` -> choose another folder (spec FR-008; the app never creates folders) |
| the original **name exists** in it | `NameTaken` -> choose another folder |
| the folder exists and the name is free | `OriginalFree` -> a confirmation proposes the place; on confirm T4 is sent |

The order of probes is: the entry itself (one `fetch_entry` on `<original dir>/<original name>`); if absent, the folder (`fetch_entry` on
`<original dir>`), so the common "free" case costs two cheap requests and the "taken" case one. The restore request T4 is sent only after
the user confirms the proposed place. Because T4 names no target path (the server decides), the result is **checked, not assumed**: after
a 2xx the app probes the original place again; if the entry is there, the restore is done and the item leaves the stored list. If the
server answered 2xx but the original name is still absent (it restored under another name - a server may rename on a collision that
appeared after the pre-check), the app says so plainly ("Restored, but under a different name; open the folder to see it") and does not
try to rename anything (spec FR-008), and refreshes the cached folder on request.

**Rationale.** Spec FR-007/FR-008/FR-010/FR-011: never overwrite, never create folders, treat a late-appearing name as a conflict that
changes nothing on the existing entry. The pre-check plus the post-check is the strongest condition the endpoint allows: T4 names no
target path, so neither `If-None-Match: *` nor a meaningful `Overwrite` can be expressed for it (the `Overwrite: F` header it carries
is harmless there and kept only as a guard, like the one of feature 002's moves). This is the check that constitution 1.3.0 asks the
plan to record for a write that cannot be made conditional.

**Alternatives.** Trust the pre-check alone: a name created in the seconds between check and restore would be the server's problem and
the user would not know. Create missing parents: rejected by the spec.

## R-6 Restore to a chosen place

**Decision.** For a chosen folder and name, T5 is a `MOVE` of the trash item with `Destination: <base>/remote.php/dav/files/<uid><chosen
path>`, `Overwrite: F` (a `412` means the name exists: `NameTaken`, nothing changed). Before it, the target name is checked with
`check_new_name` (the validation of feature 002 renames) and a T3 probe of the chosen folder/name. A refusal that says the server does
not support moving out of the trash (any of 403, 405, 501, or a 502 of a proxy on this request, **and** nothing changed on a re-probe)
becomes the key `restore_place_unsupported`: "This server can restore only to the original place. Rename or move the file that is there,
then restore again." No attempt is made to emulate the move by restore-then-move, because that requires the original name to be free,
which is exactly the case that sent the user here.

The chosen-place flow reuses `FolderChooserPage` with one additive option `serverOnly` (R-12). A taken name in the chosen folder asks for
a new name with the rename validation and **no replace option** (spec FR-008). A restore into a folder inside the item itself or inside
another trash item cannot happen with real folders (trash content is not in the file tree); the app still refuses a chosen path that
equals or lies inside the item's own original path when the item is a folder, and reports `restore_into_self` (spec FR-010).

**Rationale.** It is the one documented-by-behavior route to a chosen place; if the server does not allow it, honesty beats a
workaround. This is the least certain server fact of the feature: LV-T5.

**Alternatives.** Restore to the original place with a server-side rename on collision (the server's own unique-name rule): the user
would not choose the name and an existing entry's neighbor would change; rejected. Offering the chosen place only after the original
place was freed by the user: that is the fallback message.

## R-7 Identity instead of `If-Match` for trash items

**Decision.** A trash item is addressed by its **unique trash name** (the last segment of its href, which contains the deletion time and
is unique per deletion). Every T4, T5 and T8 request names exactly that segment. A request can therefore only ever act on the item the
user saw: if it was restored or removed elsewhere meanwhile, the server answers 404, which the app treats as "already gone from the trash"
(`trash_item_gone`, the item leaves the stored list, nothing else changes). No `If-Match` is sent because trash nodes carry no meaningful
ETag.

**Rationale.** The condition of Principle III exists so a write cannot hit a different version than the user saw; the unique name gives
the same guarantee for items that cannot change. Recorded in Complexity Tracking.

**Alternatives.** Address by position or by original name: rejected (two items can share a name).

## R-8 Delete for good and Empty trash

**Decision.** `delete_item` sends T8 for one item. **Empty trash is a loop of T8 over exactly the items the confirmation named** (the
rows of the list at the moment the first confirmation opened, at most 1,000), in list order, one request at a time:

- 404 = already gone = counted as removed; 2xx = removed; any other answer is counted as **not removed** with its key (`forbidden`,
  `locked`, `server_error`, `rate_limited`, ...), and the loop **continues** with the next item except on `auth_failed` (stops at once,
  spec FR-033) and on `rate_limited` (stops, "wait and try again").
- A transport failure whose outcome is unknown (the request may have been executed) marks that item **unknown**, stops the loop, and
  tells the user how many were removed, that one is unknown and how many were not tried; nothing is repeated by itself (R-10).
- The user can **Cancel** between two requests; the summary reports removed / not removed / not tried.
- Progress: the screen updates at most every 10% or 25 items, whichever is fewer updates; each result is a counter, no list is redrawn per item.
- Items that arrived in the trash after the list was read are never in the named set and stay (spec FR-023). A truncated list (R-3)
  says "N items shown; more exist and are not affected" in both confirmations.
- A **single** delete of a folder removes it with its content in one T8; a delete of a file is one T8.
- **Where the call may exist:** `delete_item` is called only from `TrashBridge` (rule N12), after the last confirmation the spec requires;
  `TrashBridge` rejects a call made without a recorded confirmation token (below).

**Confirmation chain.** `TrashBridge` holds a `PendingDestruction { kind, item(s) named, stage, token }`. `begin_delete(item)` /
`begin_empty()` returns the content of the first confirmation; `confirm(stage)` advances to the second stage (folder and Empty trash) or
executes (file, or second stage). Cancel, leaving the screen, a refresh of the list and a change of account all discard the
`PendingDestruction`. A confirmation whose named set no longer matches the held set (after a refresh) is not executed.

**Rationale.** Spec FR-021 to FR-026, SC-003. Single deletes are the only way to honor FR-023.

**Alternatives.** `DELETE` of the trash collection: removes unseen items; rejected. Parallel deletes: faster but a bigger partial-failure
surface and harder to cancel.

## R-9 Versions: list and restore

**Decision.** T6 lists `.../versions/<uid>/versions/<fileid>/`; the first member (the collection itself) is not a row. The list is the
**older** versions only (spec: the current content is not counted); an empty list shows `no_older_versions`. A **folder** has no Versions
tool. The restore (T7) is guarded in this order:

1. The user taps a version, then Restore: a confirmation names the file and the version (date, size) and says that the current content is
   kept as one more version (spec FR-016). Cancel is the default.
2. **Stale check:** T3 reads the file's current ETag; if it differs from the ETag the list was read against (held by the page), the app
   shows `version_changed_meanwhile` ("The file has changed since the list was read."), offers to reload, and sends nothing (spec FR-018).
3. T7 is sent. 2xx = done; the page then re-reads the list (T6) and the file's ETag (T3); a re-read that shows **no new version** is
   reported plainly ("Restored, but the server did not keep the previous content as a version") because that is the server's rule that the
   spec relies on (LV-T7); nothing is deleted either way.
4. A 404 on the version = `version_gone`: reload the list.

After a confirmed restore the cached folder versions above the file are cleared (R-14) and the local copy, if any, shows "outdated" by the
ordinary rule of feature 001 (the ETag changed) without any file being touched (spec FR-019).

**Rationale.** Spec Story 3 and FR-015 to FR-020. **Alternatives.** Downloading the old version first and re-uploading: transfers data,
breaks the "server keeps the current as a version" rule, and is out of scope.

## R-10 Unknown outcome (writes: T4, T5, T7, T8)

**Decision.** The same rule as feature 002 (R-7 there, FR-019/FR-020 here): when the request may have been executed and no answer
arrived (`Delivery` after the body was sent, a timeout), the result is `outcome_unknown`: the screen says so, **nothing is repeated by
the app**, the item stays in the list marked "outcome unknown", and the user checks by tapping Refresh (the trash) or Reload (the
versions) and, for a restore, opening the folder. The mark is in memory only; leaving the screen drops it (the next list shows the truth).
For Empty trash the loop stops at the first unknown outcome (R-8). `interpret_write_answer` and `Delivery` of `dav_write.h` are reused;
`trash_api` adds `interpret_trash_answer` (pure) for the trash-specific keys.

**Rationale.** Constitution III: an upload or write of unknown outcome is kept as uncertain and not repeated silently. **Alternatives.**
Retry once: forbidden (a repeated restore could create "(2)" copies on a server that renames on collisions).

## R-11 Messages derive from keys, not from server text

**Decision.** Every failure becomes a stable key (`trash_unavailable`, `versions_unavailable`, `restore_name_taken`,
`restore_parent_missing`, `restore_place_unsupported`, `restore_into_self`, `trash_item_gone`, `version_changed_meanwhile`,
`version_gone`, `no_older_versions`, `outcome_unknown`, plus the existing `auth_failed`, `forbidden`, `locked`, `server_storage_full`,
`name_forbidden`, `rate_limited`, `server_error`, `no_network`, `timeout`, and a generic `protocol_error` with a short code). The texts
live in `qml/Problems.qml` (`title`/`action` per key) and `strings_de.tsv`; server text is never shown (spec FR-032). The short code is
`T<operation digit><status>` (for example `T8-403`), built from the operation and the HTTP status only.

**Rationale.** Same approach as 004 R-4: wording from rules and status can neither leak names nor mislead. **Alternatives.** Showing the
server's message: breaks FR-032 and localization.

## R-12 Reuse of the folder chooser

**Decision.** `FolderChooserPage` gets one additive property `serverOnly` (default false). When true, the "New folder" tool is hidden and
queued (not yet existing) folders are not listed, so a restore cannot target a folder the server does not have (spec FR-010). The chooser
still works from stored listings and fetches an unvisited folder when the user opens it online. The result goes back to `TrashPage` as the
chosen path; the name check and the probe happen there.

**Rationale.** Spec FR-008 names this chooser; one option avoids a second chooser. **Alternatives.** A new chooser: duplicates a component
(constitution XI).

## R-13 Where the entries are

**Decision.** Trash: a row/tool "Trash" on `HomePage` and a tool in the normal-mode bottom panel of `FolderPage` (SC-001: folder listing ->
Trash -> item -> Restore -> confirm = 4 taps). Versions: a tool on `DetailsPage` of a file (Download, Keep offline, Update, Open, Share,
Versions). `TrashPage` shows the list with page buttons; tapping a row **marks it as the chosen item** (a visible check and the path line
"Chosen: <name>", spec FR-006), one at a time; tools: Restore, Delete for good (hidden until Story 4 is enabled in the build), Empty trash,
Refresh. `VersionsPage` shows the list; tapping a row marks one; tools: Restore, Reload. Offline: the stored trash list with its age;
tools that need the server are shown disabled with the reason line.

**Rationale.** Existing component set, page-based lists, no new look (constitution XI). **Alternatives.** A details sub-page per trash
item: more taps, more code.

## R-14 Local copies and cached listings after a change

**Decision.** After a confirmed restore (T4/T5) or version restore (T7), the cached folder versions (ETags) of the folders above the
affected path are cleared, as feature 002 does after a confirmed change ("Versions above a change"), so the next open of that folder
fetches a fresh listing instead of a stale one. Nothing else is written: the state of a local copy ("removed on the server", "outdated")
is **derived** from the stored listing by the existing `entry_state` rule, so it returns to normal when the listing is fetched again; no
file on the reader, no offline mark and no queue item is touched (spec FR-014, FR-019, FR-026). Delete for good changes no listing (the item
was not in the file tree).

**Rationale.** One invalidation helper already exists; deriving state from listings keeps the guarantee testable. **Alternatives.** Patch
the cached listing in place: fragile when the server renamed on a collision (R-5).

## R-15 Confirmation content (pure, tested)

**Decision.** `trash_rules` builds a `ConfirmContent` value (kind, item names shown, counts, flags) that the bridge turns into the dialog
text key plus arguments, so the *content* of every confirmation is unit-tested without Qt:

- Restore (proposed place): item name, the place (folder path shown), "It goes back to its original place" or "You chose this place".
- Delete for good, file: name; "cannot be undone"; Cancel default.
- Delete for good, folder, stage 1: name; "its content is included"; "cannot be undone"; stage 2: the name again and "This is the last step".
- Empty trash, stage 1: counts "N items, of which F folders with their content"; up to 5 names (each at most 40 characters, cut on a character
  boundary); "cannot be undone"; stage 2: the counts again and "This is the last step"; the truncation notice when the list was cut.
- Version restore: file name, version date and size, "the current content is kept as one more version".
- Every irreversible dialog has Cancel as `defaultButton`; `scripts/check-ui-style.sh` checks that dialogs with the destructive key set
  `destructive: true` also set `defaultCancel: true`, with a self-test.

**Rationale.** Spec FR-016, FR-022, SC-003. **Alternatives.** Free-text dialogs per screen: untestable.

## R-16 Network activity, offline and the one-request rule

**Decision.** No request at start, none by timer, none on resume. The first request of a screen is T1 (capabilities), made when the user
opens the trash or the versions; the answer is kept in memory for the screen only. A tap while the reader is offline asks the system to
bring Wi-Fi up (`ensure_network`, as the other bridges do); if that fails the screen says `no_network`. One request at a time per bridge
(a second tap while one runs is ignored). `test_no_background_network` is extended: opening the app, the Home page and the folder page
make no trash or versions request, and leaving the screen cancels nothing that was not started by a tap.

**Rationale.** Constitution VI; spec FR-031. **Alternatives.** Prefetching the trash: forbidden.

## R-17 Mechanical rules

**Decision.** (a) **N2**: `core/src/trash_api.cpp` becomes a fifth audited module that may name `MOVE` and `DELETE` (not `PUT`, `MKCOL`,
`COPY`, `PROPPATCH`, `LOCK`, `UNLOCK`). (b) **N11**: the strings `trashbin` and `/dav/versions/` appear as literals only in
`core/src/trash_parse.cpp` and `core/src/trash_api.cpp` (and tests, fixtures, docs). (c) **N12**: the identifier `delete_item` (the
trash delete) is called only from `bridge/src/trash_bridge.cpp`; no file under `core/src/` outside `trash_api.cpp`, no
`queue_*`, `sync_*`, `update*`, `auto_sync*` file and no QML file mentions it; QML reaches it only through the bridge's
`confirmDelete`/`confirmEmpty` slots. Each rule gets self-tests in `scripts/check-constitution.sh` (a planted violation per rule is
detected; the allowed files pass), as 004's N8/N9 and 005's N10 did. The rule numbers N11 and N12 are free (the script ends at N10).

**Rationale.** Principle VIII. **Alternatives.** Review only: has missed such sites before.

## R-18 Test strategy

**Decision.** Failing-first, in this order, each story recorded red before its implementation:

- Unit: `test_trash_parse` (fixtures: a normal answer; huge; truncated; wrong namespaces; duplicate hrefs; names with `..`, `/`, control
  characters, 300 bytes; a member with a 403 propstat; original locations with `..`, empty segments and 2 KB; deletion times negative, huge,
  missing); `test_version_parse` (same shape); `test_trash_availability` (the table of R-2, every row); `test_trash_rules` (restore
  planner all rows of R-5; confirmation content of R-15; empty-set selection; truncation); `test_store_trash`, `test_store_migration_6`.
- Integration against the extended fake: list; restore free/taken/parent missing/chosen/refused/lost answer/late collision; version
  restore with the stale check and the "no new version" report; delete for good and Empty trash with items arriving mid-way, a locked item, an
  auth failure part-way, a cancel; the disabled apps; `test_trash_privacy` (search everything the app writes for names and locations other
  than the stored list); `test_trash_curl` (real libcurl, TLS server, the `MOVE`/`DELETE` verbs and the `Destination`/`Overwrite` headers).
- Bridge: the confirmation chain (a call without the recorded confirmation does nothing; Cancel at stage 2 deletes nothing; leaving the
  screen discards it), one request at a time, offline behavior.
- App self-test: both pages instantiate; walks 21 and 22; the scripts' self-tests.
- Budgets (plan, Performance Goals and Constraints): `test_budget_trash` for 1,000 rows and 200 versions (host times and memory; device columns stay unverified).

**Rationale.** Constitution I. **Alternatives.** none.

## R-19 Honest verification, checklists and release

**Decision.** `docs/live-server-checklist.md` gets LV-T1..LV-T10 (capabilities keys for both apps; trash list properties and the shape of
the original location; restore to the original place incl. a late collision; restore to a chosen place; the `Overwrite: F` answer;
Empty-by-single-deletes on a trash of 50 items; version list properties; version restore keeps the current content as a version; stale file
during a version restore; a server with each app disabled). `docs/device-test-checklist.md` gets DT-T1..DT-T4 (panels fit at touch size;
the two-dialog flow readable and Cancel default; paging a 1,000-item trash; Empty trash progress and screen-off). All start **U**
(unverified). Fixtures carry `.unverified`. Version 0.5.0 -> 0.6.0 with a CHANGELOG entry are the last phase of `tasks.md`
(constitution, Development Workflow), after the pre-merge gates pass.
