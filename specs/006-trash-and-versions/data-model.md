# Data Model: Trash and File Versions

In-memory entities, the one stored table (schema 6), validation, state machines. Names in `code font` are the Qt-free types of
[contracts/core-interfaces.md](contracts/core-interfaces.md). Server behavior behind the fields is unverified (research R-1).

## Entities

### TrashItem (in memory; stored as a `trash_items` row)

| Field | Type | Rule |
|-------|------|------|
| `trash_name` | string | the unique name of the item in the trash (last href segment, decoded once); one path segment, 1 to 255 bytes, valid UTF-8, no `/`, `\`, control character or `..`; the identity of the item (research R-7) |
| `name` | string | display name; control characters removed; <= 255 bytes cut on a character boundary; never used as a local path |
| `original_dir` | string | the folder it was deleted from, "/"-rooted, "" when unknown; each segment checked; <= 1,024 bytes |
| `original_name` | string | its name in that folder, "" when unknown |
| `original_known` | bool | false when the location was missing, unsafe or too long (the item is then restorable only to a chosen place) |
| `deleted_at` | optional int64 | epoch seconds in [0, now + 1 day]; nullopt = "unknown date" (sorts last) |
| `kind` | `Kind` | File or Folder (never merged: a file and a folder of the same name are two items) |
| `size` | optional int64 | files only, >= 0 |
| `fileid` | optional int64 | numeric `oc:fileid`, display only |
| `unreadable` | bool | the trash name or the display data is unusable: listed, not restorable, not deletable |
| `availability` | `Availability` | `Ok`, or `Encrypted` / `NoPermission` / `Unsupported` for entries the user cannot act on: no Restore, no Delete for good (spec FR-005); derived from `nc:is-encrypted`, `oc:permissions` and `d:resourcetype` by the rule of research R-3, never from a missing property |

Ordering: `deleted_at` descending (unknown last), then `trash_name` ascending. At most `kMaxTrashRows = 1000` rows are kept.

### TrashList (in memory)

`items` (<= 1,000), `total_seen` (rows parsed before the cut), `truncated` (bool), `skipped_hostile` (count), `partial` (bool, a member
failed), `fetched_at` (epoch seconds; nullopt = never fetched), `from_store` (bool: shown from the stored copy, so offline).

### VersionRow / VersionList (in memory only)

| Field | Type | Rule |
|-------|------|------|
| `version_id` | string | 1 to 20 digits (the last href segment); anything else is skipped and counted |
| `mtime` | optional int64 | from `d:getlastmodified`; nullopt = "unknown date"; a future value is kept with `future = true` |
| `size` | optional int64 | >= 0 |

`VersionList`: `rows` (<= 200, newest first by `mtime`, ties by `version_id` descending), `truncated`, `skipped` (count), plus the
**file stamp** `file_etag` (the current ETag of the file read by T3 when the list was read) and `file_id`, used for the stale check.
The current content is not a row.

### FeatureState and TrashAvailability

`FeatureState` = `On` | `Off` | `Unknown`. `TrashAvailability { FeatureState trash, versions; }`, in memory for the open screen only.
The table that produces it is research R-2.

### RestoreProbe and RestorePlan

`RestoreProbe { bool original_known; optional<RemoteEntry> at_original; optional<bool> parent_exists; }` is what the T3 probes found.
`RestorePlan` = `OriginalFree` | `NameTaken` | `ParentMissing` | `ChoosePlace` (original unknown) | `Unavailable` (item unreadable / not
actionable). Produced by the pure `plan_restore(item, probe)` (research R-5).

### RestoreTarget

`{ string folder; string name; bool is_original; }` - the place the item will come back to. It starts as the original place and may be
changed by the user; invariant (checked before sending): `name` passes `check_new_name`, the target did not exist at the last probe, and
for a folder item the target is not equal to or inside the item's own original path (`restore_into_self`).

### ConfirmContent (pure value)

`{ ConfirmKind kind; vector<string> shown_names (<= 5, each <= 40 bytes); int items; int folders; bool truncated_notice; int stage;
bool destructive; bool default_cancel; }`. `kind` = `RestoreProposal` | `DeleteFile` | `DeleteFolder` | `EmptyTrash` | `RestoreVersion`.
`destructive` is true for the three delete kinds; `default_cancel` is always true (spec FR-016, FR-022).

### PendingDestruction (bridge state, in memory)

`{ kind (DeleteFile | DeleteFolder | EmptyTrash); vector<TrashItem> named (the exact set); int stage (1 or 2); uint64 token }`. Created by
`beginDelete` / `beginEmpty`, advanced by `confirm`, discarded by Cancel, leaving the screen, Refresh, an account change and an app switch.
`named` is a copy: later list changes do not alter it (spec FR-023).

### OperationResult

`{ Error error; bool outcome_unknown; int removed, not_removed, not_tried; string code; }` - the outcome of one action; `code` is
`T<op><status>` built from the operation and the HTTP status only (research R-11); no name or location.

## Stored table (schema 6)

```sql
CREATE TABLE trash_items (
    position      INTEGER PRIMARY KEY,   -- display order, 0..999
    trash_name    TEXT NOT NULL,
    name          TEXT NOT NULL,
    original_dir  TEXT,
    original_name TEXT,
    deleted_at    INTEGER,
    kind          INTEGER NOT NULL,      -- 0 file, 1 folder
    size          INTEGER,
    fileid        INTEGER,
    flags         INTEGER NOT NULL       -- bit 0 unreadable, bit 1 original unknown, bits 2-4 availability
);
```

Settings keys added: `trash_fetched_at` (epoch seconds), `trash_truncated` ("0"/"1"), `trash_account` (the account row id the list
belongs to). `Store::kSchemaVersion` becomes 6. Methods (new `store_trash.cpp`): `replace_trash(const TrashList&)` (one transaction),
`load_trash()`, `remove_trash_item(trash_name)` (after a confirmed restore, delete or 404), `clear_trash()`. `clear_trash()` also runs when
the account changes or is removed. **Migration 5 -> 6** creates the table and leaves every other table untouched; it is one-way; a build
that knows only schema 5 refuses a database at 6 (existing behavior, tested again by `test_store_migration_6`).

Bounds: 1,000 rows; strings as above; the table never holds a list that was not fully replaced (the transaction).

## State machines

### Trash screen

```
Closed --open--> Checking (T1) --On--> Fetching (T2) --ok--> Listed
                      |                    |                    |-- choose item --> Chosen
                      |Off                 |-- error --> Error  |-- Refresh --> Fetching
                      v                    v                    |-- Restore --> Restoring (see below)
                  ExplainOff            ExplainUnknown(code)    |-- Delete for good / Empty --> Confirming (PendingDestruction)
Offline: Closed --open--> ListedFromStore (age shown; server actions disabled with the reason)
```

### Restore

```
Chosen --Restore--> Probing (T3 x1..2) --plan--> OriginalFree --> ConfirmingPlace --confirm--> Sending (T4)
                                         |-- NameTaken / ParentMissing / ChoosePlace --> ChoosingFolder (chooser, serverOnly)
                                                     --> ChoosingName (if taken) --> ConfirmingPlace --confirm--> Sending (T5)
Sending --2xx--> Verifying (T3) --present--> Done (item leaves the list, folder cache above cleared)
                                 |-- absent --> DoneRenamedByServer (plain message, nothing renamed by the app)
Sending --refused--> Refused(key) (item stays)        Sending --no answer--> OutcomeUnknown (not repeated)
Sending --404--> trash_item_gone (item leaves the stored list)
```

### Delete for good / Empty trash

```
Chosen --Delete for good--> Confirm1 --(file)--> Executing (T8) --> Done
                                      |--(folder)--> Confirm2 --> Executing (T8) --> Done
Listed --Empty trash--> Confirm1 --> Confirm2 --> Executing (T8 per named item, one at a time) --> Summary(removed, not removed, not tried, unknown)
Any Cancel / leave / Refresh / account change --> Discarded (nothing sent)
```

### Versions screen

```
Closed --open--> Checking (T1) --On--> Fetching (T3 file stamp + T6) --ok--> Listed | Empty("no older versions")
Listed --choose--> Chosen --Restore--> Confirming --confirm--> StaleCheck (T3) --same--> Sending (T7) --2xx--> Verifying (T6 + T3) --> Done
                                                                      |-- differs --> Stale (reload offered, nothing sent)
Off --> ExplainOff        Unknown --> ExplainUnknown(code)
```

## Validation rules (summary)

- A server-supplied name, location or id never reaches a local path; trash names and version ids only ever become URL path segments built by
  the app, after the single-segment checks above.
- A restore target name passes `check_new_name`; no replace option exists anywhere.
- A destructive action runs only with a `PendingDestruction` at its last stage and a matching token.
- Every list is bounded (1,000 / 200 rows, 4 MiB / 1 MiB answers); an over-bound answer is `listing_too_large` with a plain message.
- No field holds a secret; no name or location is logged (research R-11, rules R6/R8/N4/N11).
