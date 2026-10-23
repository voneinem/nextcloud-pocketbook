# Research: Folder Synchronization

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **Builds on**: [001 research](../001-nextcloud-files-client/research.md), [002 research](../002-manage-server-files/research.md)

All decisions below are for the planning phase. Server and device behavior that has not been exercised on a real Nextcloud
server or on the InkPad One is **unverified** (constitution IX); R-20 lists the items that feed the checklists. There is no
`NEEDS CLARIFICATION` left in the plan: the two decisions the spec left open were answered in its clarification session
(unattended syncs only prepare a plan; a plan over a limit is refused as a whole), and everything else below is a planning
decision with a stated working value.

## R-1 Starting point: what features 001 and 002 give, what changes

| Existing piece | Use in this feature | Change |
|----------------|---------------------|--------|
| `Updater` (`update.cpp`) | Pattern only: a transient, bounded `Depth: 1` walk that creates no cache rows for folders the user never opened | None. Sync has its own walk (R-4) because it needs a tree with sizes, versions and checksums, not a list of downloads |
| `Downloader` (`downloader.cpp`) | Every download of a sync: temporary file in the target folder, size and checksum check, atomic rename, resume, free-space check | Gets a per-request target root and a strict version (R-7); its journal and `recover()` are reused unchanged |
| `QueueActions`, `QueueSender`, `queue_items` (002) | Every upload and every confirmed server deletion: persistence, chunking, conditional writes, conflict, "outcome unknown", refusal messages | `queue_items` gets three columns (R-8); `QueueSender::run` can be limited to one group; `queue_confirm` also advances the sync record in the same transaction |
| `AutoSyncScheduler` (`auto_sync.cpp`) | The 10 s quiet period, reused per pair | One instance per armed pair inside `SyncScheduler` (R-13) |
| `local_files`, `entry_state`, `local_files.cpp` | The 001 folder listings and Details page show a synced file as downloaded | A read-only view of `sync_files` is consulted after `local_files` (R-15) |
| `source_walk`, `upload_source`, `new_name`, `local_name`, `check_new_name`, `hash` | Name rules, source checks, keep-both names, checksums | Reused; two small functions added (R-6, R-9) |
| `FolderChooserPage`, `FilePickerPage`, `TopBar`, `BottomPanel`, `InkRow`, `ModelPager`, dialogs | The UI of the new pages | `FilePickerPage` gets a folder-only mode (R-16) |
| `Store` schema 4 | Settings, account, server caps, queue | Schema **5** (R-21) |
| `scripts/check-constitution.sh` | Mechanical rules | N3 and N5 allow-lists get exactly the new entries (R-18) |

No new dependency (libcurl, OpenSSL, SQLite, Qt UI only).

## R-2 The pair: model, validation, bounds (FR-001 to FR-006)

A **pair** is: account (server, user id), server folder (a remote path), reader folder (an absolute path), direction
(`down`, `up`, `both`; default `down`), limits (file count, total bytes), two switches (`startup_sync`, `auto_sync`), and its
last-sync facts. At most **8 pairs** (`kMaxSyncPairs`; `pair_limit` beyond) - a bound, not a feature need.

Validation (pure, in `sync_rules`; every refusal has an error key and a message):

1. The reader folder lies under the picker root (`SourceRules.root`, `/mnt/ext1`), after `realpath`, with no link on the way
   (`check_source_path` is reused) - `source_not_allowed`.
2. It is not the picker root itself, not the app's data folder, not the download root of feature 001 (`PBCloud`), and it contains
   none of the latter two (a parent of the data folder or of the download folder would sync the app's own files) -
   `pair_folder_forbidden`.
3. It does not equal, contain or lie inside another pair's reader folder (comparison of canonical paths, component-wise,
   case-folded because the storage is case-insensitive) - `pair_overlap`. The same rule for the server folders (component-wise,
   byte-exact as everywhere on the server side).
4. The server folder is a valid remote path of the current account (the folder chooser only offers visited folders; the pair
   stores the path, not a file id). The server root `/` is allowed: the overlap rule then excludes every other pair.
5. Limits: file count 1 to 5,000, size 1 MiB to 64 GiB (`pair_limit_invalid`), defaults in R-11.
6. A pair created or switched to `up` or `both` needs the explicit confirmation of FR-005; the bridge refuses the call without
   the `confirmed` flag, so the UI cannot skip it.

Saving, changing or removing a pair writes only `sync_pairs`/`sync_files`/`sync_plans` rows (FR-003). Removing a pair deletes
its rows (records, plans) and nothing else: queue items that came from it keep their `pair_id` as a dead reference and finish as
ordinary 002 items (no record advance when the pair is gone); they stay visible in the queue until the user removes them.

Reader folder created for the purpose: the folder browser offers "New folder" (`mkdir` below the picker root, name checked by
`sanitize_component`, never over an existing entry). The `mkdir` is done by `create_picker_folder` in the audited `core/src/local_files.cpp`
(rule N5's module), not in `bridge/src/picker_bridge.cpp`, which stays free of filesystem mutation (rule N6).

**Account (FR-050).** Every pair carries the `server` and `user_id` it was created for. `save_account` for another server or user, and
`clear_account`, mark all pairs `account_changed` (`sync_pairs_mark_account`, same transaction); in addition `SyncRunner::compare`,
`run` and `apply_choices` and `SyncBridge` compare the pair with the current account row before any lock or request, so a pair, or a plan stored
earlier, is never used for another account even if the mark was missed.

## R-3 Sync records and change detection (FR-020 to FR-024, FR-037, FR-038, FR-041)

For every file a pair has once brought into agreement, `sync_files` stores one **record** (data-model.md):

| Field | Meaning |
|-------|---------|
| `rel` | server-relative path below the pair's server folder (what the server called it), the key together with `pair_id` |
| `local_rel` | the relative path below the pair's reader folder (after the safe-name rules, R-6); equals `rel` unless a name had to be changed |
| `server_etag`, `server_size`, `server_checksum` | the server version the pair last agreed on |
| `local_size`, `local_mtime`, `local_sha256` | the reader file as it was when agreed; the hash only when the transfer already had it (a verified SHA-256 from the server on download, the checksum an upload computed for its conditional request); never an extra pass over the file |
| `origin` | `download`, `upload`, `identical`, `conflict_copy` |
| `diverged` | 1: a conflict was kept as two versions (R-9); the file is never transferred again by a sync |
| `one_sided` | `''`, `reader` or `server`: a conflict copy that exists on one side by design (R-9) |
| `quiet` | 1: the user chose "leave both sides as they are" for a deletion proposal (R-10) |

**Unchanged/changed rules** (FR-023: no decision by clock, size or name alone):

- *Server side changed* means: the current `etag` differs byte-wise from `server_etag`. A listing entry without any ETag
  cannot be compared: it is treated as changed, which at worst produces a conflict that keeps both versions, never a silent
  overwrite. Size and date of the server never decide.
- *Reader side changed* means: the file is not a regular file any more, or `size` or `mtime` differ from the record, **unless**
  only `mtime` differs, the record has a `local_sha256` and the file's hash equals it (then it is unchanged, and the record's
  `mtime` is refreshed when the next sync completes). Without a stored hash, a differing `mtime` counts as changed. FAT stores
  `mtime` with 2-second resolution, so the comparison uses that resolution. This is the same reader-side rule as feature 001's
  `local_copy_changed`, extended by the hash tie-break; the false-positive direction (reporting a change that did not happen)
  only ever leads to a conflict that keeps both versions.
- A *missing* file is "not in the listing of a complete scan" (R-4); an incomplete scan never makes a file missing (FR-029).

**Advancing a record** happens only after the file is complete on both sides: a download advances it in the same transaction
that commits the `sync_files` copy (R-7), an upload in `queue_confirm` (R-8), an *identical* first-sync match when the plan is
approved (no transfer needed), a conflict copy when its transfer is done. Nothing else writes a record. A crash between
"transfer complete" and "record advanced" is closed by the transaction; the only window is "file complete, nothing committed",
and the next plan then finds the file as new on both sides: for a download the file is identical by checksum (or size and
etag-equal download journal row), otherwise a conflict that keeps both - never a loss, at worst a duplicate that the user
sees. The kill tests (R-19) assert exactly this.

**First sync with both sides populated (FR-020):** same relative name, same size and, when the server lists a checksum, the
same checksum computed over the reader file with `hash_file` (streamed, 64 KiB; cancellable; only for candidates with equal
names and sizes) - recorded as `identical`, no transfer. Without a server checksum, equal size is not enough: conflict.

## R-4 Scanning both sides (FR-011, FR-012, FR-029)

**Server side.** `SyncScan::server()` walks the pair's server folder with `fetch_listing` (`Depth: 1`, one folder at a time,
breadth-first) through `DavSession`, reading each listing **transiently**: no `folders`/`entries` rows are created or patched
(constitution V: cached only for visited folders; the same decision as the Updater in 001 R-10). Bounds: 500 folders, depth 20,
5,000 files (`kSyncMaxFolders`, `kSyncMaxDepth`, `kSyncMaxFiles`); a listing the parser flags `partial`, a folder that fails or
times out, or a bound that is hit sets the scan's `incomplete` flag and records which folders were not listed. The scan returns
a flat, sorted vector of `ScanEntry {rel, kind, size, etag, checksum, mtime, availability}` - held in memory only for the run
(5,000 entries is well below 2 MB). Entries marked unavailable by the listing (no permission, encrypted) keep their reason and
become `skip` entries (Story 3 scenario 7). A 404 on the pair's folder itself is the pair state `server_folder_missing`: no plan,
no proposals. A login failure ends the scan with the general key (reconnect message), as in 001/002.

**Reader side.** `SyncScan::reader()` walks the pair's reader folder with `opendir`/`fstatat` without following links
(`AT_SYMLINK_NOFOLLOW`), bounded by the same numbers, skipping hidden entries (a name starting with `.`, which also covers the
app's own `.pbc-<id>.part` temporary files), links, special files and unreadable entries (counted and listed as `skip` with
their reason). The reader folder missing, unreadable or on absent removable storage is the pair state `reader_folder_missing`:
no plan, no proposals (edge case in the spec). Names on the reader are compared case-folded (the storage ignores case); two
reader names that differ only by case cannot exist on FAT, but a scan of an other file system (the host demo) reports the second
as `name_collision` skip.

Neither walk keeps more than the flat vector; the planner receives two sorted vectors and merges them in one pass (no tree in
memory), which carries SC-010.

## R-5 The planner (FR-007 to FR-015, FR-020 to FR-029)

`SyncPlanner::plan(PlannerInput)` is a **pure function** over: the pair (direction, limits), the records, the two scan
vectors, free space and reserve, the quota (when known), and the set of paths with open queue items. It returns a `SyncPlan`.
The complete decision table is in [contracts/sync-rules.md](contracts/sync-rules.md); its shape:

```text
per name (merged from records, server scan, reader scan):
  new on one side only        -> transfer to the other side if the direction allows it, otherwise ignored (not listed)
  on both sides, no record    -> identical (R-3)  or  conflict
  record, nothing changed     -> skip "in sync"
  record, one side changed    -> transfer to the other side if the direction allows it, otherwise skip "direction"
  record, both changed        -> conflict (keep both)
  record, diverged            -> skip "kept both"
  record, one side missing    -> deletion proposal (or "changed on the other side": keep, R-10)
  record, both missing        -> record dropped when the plan is approved
  open queue item for the name -> skip "change pending"
```

Plan entries get an action (`download`, `upload`, `skip`, `keep_both`, `propose_delete_server`, `propose_delete_reader`), a
reason for skips, sizes, and - for transfers - the identity they were planned against (server ETag, reader size and mtime),
which the runner re-checks (R-14). The plan then gets its totals, the **limit check** (R-11), the **space check** (the sum of
downloads against free space minus reserve, entry by entry in plan order; entries that do not fit are marked `no_space` and are
not approvable) and the `incomplete` flag from the scans. Creation of the plan changes no file and queues nothing (FR-007); it
writes only the plan rows.

Folders are not plan entries except for deletion proposals (R-10) and as a *count* ("12 new folders") in the totals: an empty
folder is created on the receiving side together with the first file in it (assumption in the spec), by `ensure_directories`
on the reader and by a `mkdir` queue item on the server (002 already orders folder before content).

## R-6 Names across the boundary (FR-019, FR-048, SC-012)

- **Server name -> reader name.** `sanitize_component` and `choose_local_path` of feature 001, applied per component with a
  `TakenMap` built from the pair's records plus the reader scan (so a name taken on the card, recorded or not, gets " (2)"),
  rooted at the **pair's reader folder** instead of the download root. A name for which `choose_local_path` returns an error
  (`path_unsafe`, `path_too_long`) is a `skip` entry with that reason. The result is stored as `local_rel`.
- **Reader name -> server name.** `check_new_name` with the saved `ServerCaps`; a name the server forbids, an overlong name,
  or one that fails `is_valid_name` is a `skip` entry (`name_forbidden`). A server name that differs only by case from an existing
  one is treated by the existing name-collision rules of 002 (`name_use`), and a collision in the plan is a conflict question
  that 002 resolves as keep both; the sync never answers a name question for the user: it skips the entry with `name_exists`.
- A hostile name (separator, `..`, control characters, overlong, normalization variants) never reaches a path function
  unsanitised; `sync_runner.cpp` has no `open`/`rename` on a path that did not come from `choose_local_path` or from the
  reader scan (rule N7, R-18).

## R-7 Downloads (FR-016, FR-040, SC-011)

The Downloader serves a sync with two small, additive changes:

1. `DownloadRequest` gains `target_root` (absolute; empty = the download root of 001), `strip_prefix` (the pair's server folder)
   and `pair_id`. When `target_root` is set, `choose_local_path` is called with the pair-scoped `TakenMap` and the remote path
   relative to `strip_prefix`; the file is recorded through `Store::sync_commit_download` (a transaction that writes the
   `sync_files` record and ends the journal row) instead of `commit_download`. Without `target_root` nothing changes for 001.
2. `DownloadRequest.strict_version`: the first answer of the server must carry the planned ETag, otherwise the job ends
   `changed_during_sync` with nothing written (the request carries `If-Match` with the planned strong ETag when there is one;
   a 412 or a differing ETag in the answer ends the job the same way). The sync leaves such a file for the next plan and reports it
   (FR-040).

Everything else is the existing engine: temporary file `.pbc-<job>.part` in the target folder, `Range` with `If-Range` resume,
size and checksum verification, atomic rename, `preflight_download` (free space, 4 GiB, read-only), a coarse progress callback,
`recover()` at start. A download never overwrites a reader file that is not the recorded copy: the planner turns that case into
a conflict (R-9) and the request is issued with `ConflictPolicy::KeepBoth`, whose naming is the pair's conflict name (R-9), so even
a plan that is stale cannot overwrite.

## R-8 Uploads through the queue (FR-017, FR-018, FR-039)

For every approved `upload` entry the runner calls a new `QueueActions::add_sync_uploads(pair, entries)` that writes one **group**
of `queue_items` in a single transaction: `mkdir` items for folders that do not exist on the server yet, then `upload` items with
`src_local` = the reader file, `dst_parent`/`dst_name`, and for a changed file `replace = 1` with `dst_etag` = the record's
`server_etag` (strong; a weak or missing ETag makes the entry a `skip` with `server_version_unknown` at plan time). A `replace`
that meets a different server version is a 002 conflict that keeps both versions (FR-023 of this spec = FR-022/023 of 002).
New columns (data-model.md): `pair_id`, `sync_rel`, `sync_role` (`file` | `conflict_copy`). The item is created *before* any
request, so the queue's persistence rules (002 FR-006/007) are the persistence of the sync's uploads (FR-037).

`QueueSender::run(Cancel&, only_item, only_group)` is extended with `only_group` so that a sync sends **only its own group**
and never the unrelated items of the user's queue. Chunking, resume, conditional writes, quota check, refusal keys, cancel and
`unknown` are the sender's, unchanged. `queue_confirm` gets one extra step in the same transaction: when `pair_id` is set and the
pair still exists, the matching `sync_files` row is written/advanced with the server identity from the confirmation
(`result_etag`, size) and the reader identity from the item (`src_size`, `src_mtime`, `checksum`). An item that ends `failed`,
`conflict` or `unknown` leaves the record untouched: the file is not "synced" until the user's decision (FR-039), and the next
plan lists it as `skip` with `change_pending` while the item is open.

The files of an Outbox are **never touched** (FR-018): the sender opens sources read-only without following links (002 rule
N6, unchanged), and the sync has no code path that moves, renames or deletes a source file.

A sync does not wait for the user: the runner returns the upload group id; sending is started by the runner on the worker right
after approval (the user approved the plan, which includes the uploads), and the result shows each item's state. Uploads that stay
`conflict`/`unknown`/`failed` are counted in the result under `failed` (with reason) and are resolved on the Queue screen of 002.

## R-9 Conflicts: keep both versions (FR-021, FR-022, FR-024, Story 6)

Planner outcome `keep_both` for a name with changes on both sides, a first-sync name clash with different content, a download
whose reader target exists but is not the recorded copy, or an upload whose server target changed. Execution gives **each side
the other's version under a new name, and leaves the original name alone on both sides**:

| Direction | Reader gets | Server gets | Original name afterwards |
|-----------|-------------|-------------|--------------------------|
| `down` | server version as `<stem> (server copy YYYY-MM-DD)<ext>` | nothing (nothing is sent) | reader version stays on the reader, server version stays on the server |
| `up` | nothing (nothing is copied) | reader version as `<stem> (reader copy YYYY-MM-DD)<ext>` | unchanged on both sides |
| `both` | the server copy | the reader copy | unchanged on both sides |

The copy names come from `with_suffix`-style generation: `conflict_name(name, kind, date, taken)` produces the marked name, adds
" (2)", " (3)" while the name is taken on that side (listing, records, open queue items), and keeps the extension and the 255-byte
limit. The server copy is a normal download (to the new `local_rel`) and the reader copy a normal queue upload (to the new
`rel`), so every guarantee of R-7/R-8 applies to them.

Bookkeeping that makes the result stable (FR-022, SC-005):

- The original record gets `diverged = 1` with the identities of the two versions seen. A diverged name is never transferred and
  never proposed again: it is listed as `skip` "kept both" (and counted under *kept both* in the result) in every plan until the
  user changes the situation with ordinary file management (deleting one of the two originals turns the other into the ordinary
  case "record, one side missing" and a deletion proposal as in R-10).
- Each conflict copy gets a record with `origin = conflict_copy` and `one_sided = reader` (the server copy lives only on the
  reader) or `server` (the reader copy lives only on the server). A one-sided copy is *never* transferred to the other side and
  never proposed for deletion: its only purpose is to stop the next sync from bouncing it back. If the user deletes it, the
  record is dropped silently.
- "Deleted on one side, changed on the other" (Story 6 scenario 5) is **not** a keep-both but a kept file: the planner emits a
  proposal entry whose deletion choice is *not offered* (R-10), so the changed version is kept and shown.

The conflict date is the reader's date in the reader's time zone (`format_date`), never used for any decision.

## R-10 Deletions are proposals (FR-025 to FR-029, Story 7)

A name with a record and one side missing in a **complete** scan produces a proposal entry:

- `propose_delete_server`: the file is missing on the reader (the user removed it) and the server still has it.
- `propose_delete_reader`: the file is missing on the server and the reader still has it.

Each proposal carries the choices that are possible *for that pair and that situation*:

| Choice | Meaning | Offered when |
|--------|---------|--------------|
| `transfer_again` | copy it back to the side that lacks it | the direction allows that transfer (`down`/`both` to the reader, `up`/`both` to the server); the other side's file is unchanged or changed - both are fine |
| `leave` | do nothing, set `quiet = 1` so that it is not proposed again until the file changes | always |
| `delete` | delete the file on the side that still has it | for `propose_delete_server`: the direction allows writes to the server (`up`/`both`) and the server has a trash (002 FR-033); for `propose_delete_reader`: the reader file is **unchanged** since the record (Story 7 scenario 6) |

Rules:

1. Approving a plan never confirms a proposal (FR-009). The plan page lists proposals in their own section; the user chooses per
   entry and then taps **Apply choices**, which shows a confirmation naming each file and side for every `delete` (FR-026) and
   the wording about the trash/unrecoverability (FR-027). Execution of proposals is a separate, short run (no transfer of plan
   entries) and may happen long after the plan was approved, subject to the plan age rule (R-12).
2. `delete` on the server enqueues a 002 `delete` item (conditional on the record's strong ETag, to the trash, refused without a
   trash - `trash_unavailable` makes the choice unavailable in the UI before the confirmation). `delete` on the reader is a
   single audited function `remove_pair_copy(...)` in `local_files.cpp` (the only new place that deletes a user's file): it
   re-checks that the file is a regular file, inside the pair's reader folder, with no link on the way, and unchanged against the
   record; otherwise it reports `local_changed` and deletes nothing. There is no trash on the reader; the dialog says the file
   cannot be recovered through this app.
3. A folder missing as a whole produces **one** proposal naming the folder and the number of synced files below it
   (`records_below`). A `delete` removes only files that have records and are unchanged; files added since, and changed files,
   stay and the result says how many were kept; the (then possibly non-empty) folder is removed only when empty afterwards.
4. After `transfer_again`, `leave` or a completed `delete` the record is updated (re-created by the transfer, `quiet = 1`, or
   dropped). No proposal is made from an incomplete scan, an unavailable folder or a pair in `*_folder_missing` (FR-029).
5. A name that is a download-only pair's reader-missing file (the user deleted a book to free space) is proposed with
   `transfer_again` and `leave`; after `leave` it stays quiet, so the sync does not keep re-downloading what the user removed.

## R-11 Limits, reserve and space (FR-042 to FR-047, Story 9)

- **What a limit counts.** The files of the pair's reader folder tree as the reader scan found them (regular files only): their
  number and total bytes ("held"). The Sync screen shows held against the limits (FR-047).
- **What a limit blocks.** Only plans that *add* to the reader: `download` entries and reader-side conflict copies. A plan's
  *projected* held = held + added. If projected files > `max_files` or projected bytes > `max_bytes`, the plan is `over_limit`: it
  states which limit and by how much and **cannot be approved** (FR-043). The user can raise the limit (Pair settings, with
  the confirmation of the new numbers) or remove entries from the plan until it fits; the system never picks entries to make
  a plan fit (clarification 2). A plan that adds nothing to the reader (upload only, or only skips/proposals) is never
  over-limit, even if the folder already holds more than the limit (FR-046: lowering a limit deletes nothing and only stops
  further downloads).
- **Start-up and Auto Sync plans** that are over a limit are stored as plans in the state `ready` with the flag `over_limit`;
  the pair shows "plan ready, over limit" (FR-044).
- **Reserve.** A sync keeps `reserve = max(256 MiB, 2 % of the volume)` free (`kSyncReserveMin`, `kSyncReservePercent`, working
  values). The planner marks, in plan order, every download whose size would take free space below the reserve as `no_space`;
  these entries cannot be approved (this marking is part of the planner of Story 3 / Story 2 scenario 5; Story 9 adds only the limits),
  and `preflight_download` re-checks before every download (it already refuses `not_enough_space`; the reserve is an additional check in the
  runner). A per-file refusal (`not_enough_space`, `no_space`) skips that entry and the run continues with the next one. A card that
  becomes full or read-only *during* a download (`storage_full`, `storage_readonly`) stops the run safely: the partial file is removed,
  no further download starts, the record stays untouched and the result names the reason. Storage problems are reported, never ignored.
- **Defaults** (working values, planning decisions): `max_files = 500`, `max_bytes = 2 GiB`.
- **Server side.** The quota check of 002 FR-038 applies to uploads at send time; the plan shows the quota when known and
  marks an upload larger than the free quota as `skip` with `quota_exceeded`.

## R-12 Review, approval and the plan's life (FR-007 to FR-011, FR-033)

`sync_plans` + `sync_plan_entries` persist a plan so that a plan prepared at start-up (R-13) waits across restarts and is shown
page by page without recomputation. States: `ready` -> `approved` -> `running` -> `done` | `interrupted`; `ready` -> `cancelled`
or `superseded` (a new plan for the same pair replaces a ready one in one transaction; only one non-final plan per pair).

- **Entry removal.** The user can remove `download`, `upload` and `keep_both` entries from a plan before approving
  (`removed = 1`); removed entries are skipped with `by_user` in the result and appear again in the next plan (FR-008).
  Removal is not offered for `skip` entries (nothing to remove) or for proposals (they are choices, R-10).
- **Approval** requires: plan state `ready`, not `incomplete_blocking`, not `over_limit`, no unapprovable entries left (`no_space`,
  and so on - the user removes them), plan age <= 1 hour (`kSyncPlanMaxAgeS = 3600`, working value; older: *Compare again*
  first, which keeps the user's removals out because the plan changed), and a connected reader. Removing every transfer entry
  leaves a plan that only has skips and proposals: it can be "approved" as a no-op and the result says nothing was transferred.
- **Cancel** marks the plan `cancelled`; no file, record or queue item is touched (SC-002).
- **Counts** for the review and the result: downloaded, uploaded, skipped, kept both, open proposals, failed, with sizes, are
  columns of `sync_plans` updated as entries finish; the result shown after a run is read from them plus the duration and kept
  as the pair's `last_result_json` (one result per pair, FR-010).
- **Offline.** With no connection Sync says so and compares nothing. It offers **Preview from stored listings**: the planner runs
  over the cached listings of visited folders only, flagged `stale` and `incomplete`, labelled with the age of the oldest
  listing; such a plan cannot be approved and writes no plan rows (shown from memory). It never claims to be current.
- **Incomplete comparison.** A plan with `incomplete = 1` (R-4) can be approved for its transfers only; it never has proposals
  (R-10.4) and says which folders were not compared.
- **Unattended plans** (R-13) are exactly these plans; there is no code path that approves one (FR-033).

## R-13 Triggers and the scheduler (FR-030 to FR-036, Story 5)

Three, and only three, ways to start a *comparison*; nothing else in the product starts one:

1. **Start-up:** after the one account check of feature 001 has succeeded **and** the reader's Wi-Fi is already on, the bridge
   starts a comparison for each pair with `startup_sync = 1`, one after the other, on the worker. Not on Wi-Fi coming up later,
   not after a failed account check, not with the app in the background. The app never turns Wi-Fi on.
2. **Sync button:** the tap starts a comparison for one pair or for all pairs; with all pairs the review of each plan is shown in
   turn.
3. **Auto Sync:** `SyncScheduler` holds one `AutoSyncScheduler` per pair (the 10 s quiet period of 002, a working value shared
   with it). `SyncBridge::noteAction(remotePath)` and `SyncBridge::noteLocalAction(localPath)` are called by `QueueBridge` after
   the user's own queuing action and by the pair-settings dialog after a save; they find the pairs whose server or reader
   folder contains the path and call `note_action` for those pairs only whose `auto_sync = 1` **and** the global Auto Sync
   setting of 002 is on (so the one switch in Settings still means "nothing automatic"). One single-shot `QTimer` in
   `sync_bridge.cpp` (the second allow-list entry of rule N3) fires when the earliest pair is due; it never fires at start,
   on Wi-Fi coming up, after a failed or finished sync, or while the reader is offline (then the pair shows "waiting" and nothing
   retries).

What the three do after the comparison is the same: store the plan (R-12), mark the pair `plan_ready` and **stop**. A tap
additionally opens the plan page. No trigger approves, transfers, queues or deletes (FR-033, SC-002, SC-007). Defaults: `startup_sync = 0`
and `auto_sync = 0` for a new pair (the spec's working default), switchable per pair in the pair settings (`PairPage`); the one global Auto Sync switch of 002 stays in Settings with its wording, and gains the line "Also applies to pair syncs (they only compare)." (FR-032). Because unattended
syncs now only compare, the owner may prefer `on`; that is a one-line change in `SyncRules::defaults()` and is raised as owner
point 1 in the plan.

One comparison or run per pair at a time (FR-034): `sync_pairs.state` is the lock (`comparing`, `running`), checked and set in a
transaction; a second trigger gets `sync_busy` and the UI says one is running. The `Worker` is a single thread, so comparisons of
different pairs are also serial. At app start, pairs left in `comparing`/`running` by a kill become `interrupted` (R-14).

## R-14 Running an approved plan; resumable and idempotent (FR-035 to FR-041, Story 8)

`SyncRunner::run(plan_id, Cancel&)` executes in a fixed order and checks cancel before each item:

1. `running` state written; reader folders ensured (`ensure_directories`), nothing else created.
2. **Downloads** (and server-copy downloads of conflicts) one by one through the Downloader (R-7).
3. **Uploads** (and reader-copy uploads): `add_sync_uploads` creates the group, then `QueueSender::run(..., only_group)` sends it.
4. **Identical** first-sync matches and **record drops** (both sides gone) are written (they involve no transfer).
5. Final counts, `last_result_json`, state `done` (or `interrupted` after a cancel/stop; the unfinished entries stay `planned`).

Every entry has its own state in `sync_plan_entries` (`planned`, `done`, `failed`, `skipped`), updated in the transaction that
advances its record, so a repeated run resumes from the entries that are not `done`. **Repeating an interrupted sync**
(the user taps Sync again) never resumes the old plan blindly: it computes a **new plan** from the records and the scans, and
the records are the only source of truth. Because a record advances only when a file is complete on both sides (R-3), the new plan
lists exactly what is still to do and the final state equals that of an uninterrupted run (FR-038, SC-004). Old interrupted plans
are marked `superseded`.

**Changes during the run (FR-040).** Before each transfer the runner re-reads the planned identity: for a download, the
`strict_version` check (R-7); for an upload, `inspect_source` against the planned `(size, mtime)` and again at the sender's own
start-of-item check (002 R-5). A difference ends the entry as `changed_during_sync`: not transferred, record untouched, counted
under *skipped* with the reason "changed during the sync", and listed again by the next plan.

**Kill points** (test hooks named like the Downloader's): `plan_written`, `approved`, `before_download`, `downloaded`,
`record_committed`, `group_created`, `item_confirmed`, `proposals_applied`, `result_written`. At app start (no network):
`Downloader::recover()`, `queue_recover()` (002) and `sync_recover()`: pairs and plans in `comparing`/`running` become
`interrupted`, with the counts so far shown (Story 8 scenario 4). No file is touched by recovery.

**Cancel:** stops after the item in flight (a download keeps its journal row and is resumable; an upload is a queue item that
stays resumable); completed entries stay completed.

## R-15 The 001 screens and synced copies (FR-053, Story 3 scenario 8)

Feature 001 states a file by `compute_state(cached, local, stat)`. Synced files are recorded in `sync_files`, not in `local_files`
(a remote path may be both a 001 mirror copy and a pair copy; two tables avoid a key collision and keep 001's rules for its own
copies intact). `Store::local_file(remote_path)` stays 001's. New `Store::synced_copy(remote_path)` returns a `LocalFile`-shaped
view (`SyncedCopy {root, local_rel, etag, size, mtime}`) for a path that lies inside a pair's server folder; `BrowseBridge` and
`DetailsPage` ask `local_file` first and `synced_copy` second, and `resolve_local_file` takes the root as a parameter (the pair's
reader folder instead of the download root, with the same "no link anywhere on the way, inside the root" rule). "Remove local
copy" of a synced file is refused with a plain message pointing to the Sync screen (the pair would otherwise propose to
download it again and the record would be stale); removing a pair keeps every file (SC-013). Opening a synced file goes
through the reader's viewer exactly as for a downloaded one.

## R-16 UI structure (constitution VI, XI; FR-052)

Existing components only (TopBar, BottomPanel, IconButton, InkRow, ModelPager/PageButtons, dialogs with footer buttons, Style); no
bordered text buttons, no animation, page-based lists, icon-and-label tools.

| Page | Content |
|------|---------|
| `SyncPage` (new; a tool on Home, label "Sync") | pair list (page-based): server folder -> reader folder, direction icon, state line ("never synced", "plan ready - 4 downloads", "in sync, 14:02", "interrupted", "folder not available", "over limit"), files/storage against limits, last result; tools: Add pair, Sync all, offline preview; per pair: Sync, Settings |
| `PairPage` (new) | add/edit: server folder (opens `FolderChooserPage`), reader folder (opens `FilePickerPage` in folder-only mode with "New folder"), direction (three rows, selected one marked, plain-language summary below), limits (two number fields), start-up sync and Auto Sync switches (`InkSwitch`), Remove pair |
| `PlanPage` (new) | totals line (counts and sizes per action, free space, quota), banner for incomplete/over-limit/stale, paged entries with action icon, size, reason; tools: Approve, Cancel, Compare again; entry tap: remove from plan; separate "Proposals" section with the choice buttons and **Apply choices** (R-10) |
| `ResultPage` (new, or a dialog on `SyncPage`) | counts, failures with reasons, duration, link to the Queue for uploads that stayed open |
| `SettingsPage` | the global Auto Sync switch of 002 gets a hint that pairs also follow it; nothing else |
| `HomePage` | Sync tool (with a "plan ready" mark); `AboutPage` unchanged except version and notices |

Texts: English source strings with German translations in `qml/strings_de.tsv`; every new key has one (SC-014); each error key has
a message and an action line. Page size follows the existing pagers. Progress during a run: coarse, at most one update per 2 s or
5 % (as in 002).

## R-17 Logging (FR-049)

Log codes added to `log_codes.h`: `sync_compare_start/end`, `sync_plan_ready`, `sync_run_start/end`, `sync_item_done/failed`,
`sync_recover`, `sync_limit_hit`. Fields: counts, pair id, plan id, entry sequence number, error keys. Never a server address,
user, name, path, ETag, checksum or server text (rule N4 applies to the new files; its self-tests get one violation each for
`sync_*.cpp`).

## R-18 Mechanical checks (constitution VIII)

`scripts/check-constitution.sh`, each change with a self-test that proves the rule detects a violation:

- **N2** (write verbs only in audited modules): unchanged list; new files `sync_*.cpp` are **not** added - they must never name
  a write verb. Self-test: a `PUT` in `sync_runner.cpp` is a violation.
- **N3** (no timers, no sleeping): allow-list gets `bridge/src/sync_bridge.cpp` for exactly one `QTimer` (the Auto Sync
  quiet period); `sync_*.cpp` in the core still may not use timers or sleeping. Self-tests: `QTimer` in `sync_scheduler.cpp`
  and `sleep_for` in `sync_runner.cpp` are violations.
- **N5** (file mutation only in audited modules): `core/src/local_files.cpp` (already audited) hosts `remove_pair_copy`; `sync_*.cpp`
  may not call `unlink`, `rename`, `remove`, `rmdir`, `truncate`. Self-test: `unlink` in `sync_runner.cpp` is a violation.
- **N6** (upload sources opened read-only): unchanged; `sync_scan.cpp` opens nothing for writing.
- **N7 (new)**: `sync_runner.cpp`, `sync_scan.cpp` and `sync_planner.cpp` build paths only through `choose_local_path`,
  `check_source_path` or the scan; the rule bans `std::string + "/" +` path joins in those three files outside a named helper.
  Self-test: a raw `root + "/" + name` join in `sync_runner.cpp` is a violation.
- `test_no_background_network` is extended (R-19) and acts as a rule: no request without one of the three triggers.
- Fixture check: new fixtures carry `.unverified`.

## R-19 Test strategy (constitution I, IX)

Tests are written first and observed failing. All against the in-process fake Nextcloud (no real server), with the kill
harness for interruption.

| Layer | Tests (new unless stated) |
|-------|---------------------------|
| Unit | `test_sync_rules` (pair validation, overlaps, forbidden folders, limits), `test_sync_planner` (the full decision table of sync-rules.md, every direction, table-driven), `test_sync_planner_limits` (limit, reserve, partial removal, never self-selecting), `test_sync_scan_remote` and `test_sync_scan_local` (bounds, partial listing, hostile names, links, hidden, collisions), `test_sync_records` (change detection, mtime-only with hash), `test_sync_conflict_name`, `test_store_schema5`, `test_store_sync` (pairs, records, plans, bounds), `test_sync_scheduler` (per-pair quiet period, global switch, no fire at start) |
| Integration | `test_sync_download` (first sync, incremental, identical skip, local-only untouched, unavailable entries), `test_sync_upload` (Outbox through the queue, files unchanged, re-sync sends nothing, changed file replace, unknown outcome), `test_sync_both` (two-way, 100 files), `test_sync_conflicts` (keep both, stable on repeat, per direction, first-sync clash, delete-vs-change), `test_sync_deletions` (proposals only, approve deletes nothing, per-choice effects, trash, changed reader file, folder proposal), `test_sync_incomplete` (partial listing, folder missing, no proposals), `test_sync_limits` (never exceeds limits, reserve), `test_sync_kill` (kill at every hook of R-14 for 20 downloads + 5 uploads; final state equals the uninterrupted run), `test_sync_idempotent` (second run makes 0 transfers), `test_sync_concurrent` (second trigger refused), `test_sync_hostile_names`, `test_sync_curl` (a download and an upload through the real libcurl transport and the local TLS server) |
| Bridge | `test_sync_bridge` (properties, pair CRUD with confirmations, plan paging, approve/cancel/remove-entry, proposals, triggers), `test_no_background_network` extended (idle hour with start-up/auto pairs; Wi-Fi toggles; no unreviewed run) |
| QML / walk | `tests/qml` stubs for the four pages; `tests/walks/11-folder-sync.walk` (add pair, review, approve, result, proposals) |
| Budget | `test_budget_sync` (1,000-file compare time and memory, 500 MB file memory; marked unverified until measured on the device) |
| Fixtures | `tests/fixtures/nextcloud/` listings with 5,000 entries, partial multistatus, hostile names, unavailable entries - all `.unverified` |

## R-20 Verification items (feeds the checklists, constitution IX)

Live server (`docs/live-server-checklist.md`, LV-S1..S8) and device (`docs/device-test-checklist.md`, DT-S1..S8), all
**unverified** until done: LV-S1 `oc:checksums` presence and algorithm per file on the target server; LV-S2 ETag stability of
a file across a rename and across a desktop-client sync; LV-S3 `Depth: 1` walk of a 1,000-file folder tree: time and paging;
LV-S4 a listing that fails halfway; LV-S5 conditional replace of a synced file; LV-S6 server deletion to the trash from a
proposal; LV-S7 unavailable entries (no permission, encrypted) inside a paired folder; LV-S8 external changes between the compare and the run.
DT-S1 start-up compare with Wi-Fi on and off; DT-S2 `mtime` resolution and stability of FAT files (does a copy by PC keep the
size/mtime pair stable); DT-S3 the library picks up downloaded files in the pair folder; DT-S4 an Outbox folder written by another app (PB Notes export);
DT-S5 free-space reserve with a nearly full card; DT-S6 peak memory (compare of 1,000 files; 500 MB file) and compare time; DT-S7
kill/battery pull during a sync; DT-S8 removable storage absent. Results are recorded when obtained.

## R-21 Versions, schema, documents (constitution VIII, X)

Schema **4 -> 5** in `store_migrations.cpp` (one transaction, backup copy, integrity check as for 3 -> 4): tables `sync_pairs`,
`sync_files`, `sync_plans`, `sync_plan_entries`; `queue_items` gets `pair_id`, `sync_rel`, `sync_role`. A build that finds schema 6
or higher refuses the file; an older build refuses schema 5 (`schema_too_new`, existing). Settings keys: none new (the global
`auto_sync` of 002 is reused; per-pair switches are columns). `VERSION` 0.2.0 -> **0.3.0**, `CHANGELOG.md`, `README.md`
(limits, the plain statement that deletions are never automatic and reader-side deletion is unrecoverable), `INSTALL.md`,
`NOTICE` (no new component), the About box (nothing new to attribute), `docs/live-server-checklist.md`,
`docs/device-test-checklist.md`. All English; German only in `qml/strings_de.tsv`.

## R-22 Limits and budgets (constitution V)

| Quantity | Working value | Where it comes from |
|----------|---------------|---------------------|
| Pairs | 8 | bound |
| Folders / depth / files per scan side | 500 / 20 / 5,000 | the Updater's folder and depth bounds; files bounded by R-4 |
| Plan entries stored | 5,000 (a plan with more cannot be approved: `plan_too_large`) | bound |
| Records | 20,000 total (`records_full` beyond: new pairs refused, scans of a full pair refused) | 8 pairs x 5,000 less overlap, bounded |
| Database growth at the bounds | <= 10 MB (records ~300 B, entries ~250 B, plus indexes) | estimate, unverified |
| Default pair limits | 500 files, 2 GiB | R-11 |
| Reserve | max(256 MiB, 2 % of the volume) | R-11 |
| Plan age for approval | 1 hour | R-12 |
| Auto Sync quiet period | 10 s (shared with 002) | R-13 |
| Largest file | 4 GiB - 1 on the reader (FAT); larger server files skipped (`too_large_for_storage`) | 001/002 |
| Compare | 1,000 files / 5 levels shown within 10 s on a simulated fast connection; page turn <= 1 s | SC-010 |
| Memory | peak <= 150 MB (`VmHWM`) during a 1,000-file compare and during a 500 MB transfer; 64 KiB buffer; two sorted vectors, no tree | SC-010, SC-011 |
| Cold start | <= 5 s (unchanged; `sync_recover` adds no network and <= 100 ms for 20,000 rows) | 001 |
| Progress refresh | <= 1 update per 2 s or 5 % | 002 |
| Install size | package <= 25 MB gate; expected growth < 300 KB | existing gate |

## Traceability: functional requirements to decisions

| FR | Decision | FR | Decision |
|----|----------|----|----------|
| FR-001..006 | R-2, R-16 | FR-030..032 | R-13 |
| FR-007..012 | R-4, R-5, R-12 | FR-033 | R-12, R-13 |
| FR-013..015 | R-5, contracts/sync-rules.md | FR-034..036 | R-13, R-14 |
| FR-016 | R-7 | FR-037..041 | R-3, R-14 |
| FR-017, 018 | R-8 | FR-042..047 | R-11 |
| FR-019 | R-6 | FR-048, 049 | R-6, R-17 |
| FR-020 | R-3 | FR-050 | R-13 (account check, `user_id` on every row) |
| FR-021..024 | R-9, R-3 | FR-051..052 | R-12, R-16 |
| FR-025..029 | R-10 | FR-053 | R-15, R-2 |
