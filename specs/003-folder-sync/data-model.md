# Data Model: Folder Synchronization

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **Research**: [research.md](research.md) | **Builds on**: [001 data model](../001-nextcloud-files-client/data-model.md), [002 data model](../002-manage-server-files/data-model.md)

Storage: the same SQLite file `pbcloud.db`, schema **5** after this feature (schema 4 is feature 002 as built). One-way migration
4 -> 5 in `core/src/store_migrations.cpp` (one transaction, backup copy, integrity check, as for 3 -> 4). A build that finds
schema 6 or higher refuses the file; an older build (schema 4) refuses a schema-5 file (`schema_too_new`, already implemented).
The user's files are never copied into the database; the sync keeps only identities (versions, sizes, times, hashes).

All times are epoch seconds. Remote paths are as in 001 (decoded, start with `/`, no trailing `/` except the root, byte-exact,
case-sensitive). Reader paths are absolute and checked by `check_source_path`. A `rel` is a path relative to a pair's server
folder (no leading `/`, components separated by `/`, never `..`); a `local_rel` is relative to a pair's reader folder.

## Tables added (schema 5)

### sync_pairs (durable user configuration; never evicted)

| Column | Type | Notes |
|--------|------|-------|
| id | INTEGER PK AUTOINCREMENT | |
| server | TEXT NOT NULL | base address the pair was created for |
| user_id | TEXT NOT NULL | the account user id (a pair is never used for another account, FR-050) |
| remote_folder | TEXT NOT NULL | server folder (remote path) |
| local_folder | TEXT NOT NULL | reader folder (canonical absolute path) |
| direction | TEXT NOT NULL CHECK(direction IN ('down','up','both')) DEFAULT 'down' | |
| max_files | INTEGER NOT NULL DEFAULT 500 | 1 .. 5,000 |
| max_bytes | INTEGER NOT NULL DEFAULT 2147483648 | 1 MiB .. 64 GiB |
| startup_sync | INTEGER NOT NULL DEFAULT 0 | 1: compare at app start (never approves) |
| auto_sync | INTEGER NOT NULL DEFAULT 0 | 1: compare after the user's own change |
| state | TEXT NOT NULL DEFAULT 'idle' CHECK(state IN ('idle','comparing','plan_ready','running','interrupted')) | the lock of FR-034 |
| problem_key | TEXT NOT NULL DEFAULT '' | `server_folder_missing`, `reader_folder_missing`, `auth_failed`, ... or `''` |
| last_compared_at | INTEGER | |
| last_sync_at | INTEGER | end of the last run |
| last_outcome | TEXT NOT NULL DEFAULT 'never' | `never`, `in_sync`, `done`, `partial`, `failed`, `interrupted` |
| last_result_json | TEXT NOT NULL DEFAULT '{}' | counts of the last run (R-12), a few hundred bytes |
| held_files, held_bytes | INTEGER | what the reader scan found at the last comparison (shown against the limits) |
| created_at | INTEGER NOT NULL | |

Indexes: `(server, user_id)`. Bound: 8 rows (`pair_limit`). Validation (`sync_rules`): see research R-2; `direction` or limits
changes are validated again; the overlap and folder rules are checked in the same transaction as the insert/update.

### sync_files (the records, R-3; durable; never evicted)

One row per name a pair has brought into agreement or deliberately left as it is.

| Column | Type | Notes |
|--------|------|-------|
| pair_id | INTEGER NOT NULL | PK part; rows are removed with their pair |
| rel | TEXT NOT NULL | PK part; the server-relative path (for a one-sided reader copy: the name it would have) |
| local_rel | TEXT NOT NULL | the reader-relative path (sanitized, collision-free) |
| kind | TEXT NOT NULL CHECK(kind IN ('file','folder')) | folders only as proposal anchors; most rows are files |
| origin | TEXT NOT NULL CHECK(origin IN ('download','upload','identical','conflict_copy')) | |
| server_etag | TEXT NOT NULL DEFAULT '' | the version agreed on (`''` for a reader-only conflict copy) |
| server_size | INTEGER | |
| server_checksum | TEXT NOT NULL DEFAULT '' | the strongest checksum the listing offered, as text |
| local_size | INTEGER | the reader file when agreed |
| local_mtime | INTEGER | seconds, 2-second resolution |
| local_sha256 | TEXT NOT NULL DEFAULT '' | the SHA-256 of the reader file as agreed, written only when the transfer already had it (a verified SHA-256 server checksum on download, the checksum an upload computed for its conditional request); never an extra pass over the file; empty otherwise, which makes an mtime-only difference count as changed (R-3) |
| diverged | INTEGER NOT NULL DEFAULT 0 | 1: kept both (R-9); never transferred |
| one_sided | TEXT NOT NULL DEFAULT '' CHECK(one_sided IN ('','reader','server')) | a conflict copy that lives on one side by design |
| quiet | INTEGER NOT NULL DEFAULT 0 | 1: "leave both sides as they are" (R-10) |
| synced_at | INTEGER NOT NULL | |

PK `(pair_id, rel)`. Indexes: `(pair_id, local_rel)` unique case-folded (`local_rel_ci`, so two records cannot claim names the storage
cannot tell apart), `(pair_id, one_sided)`. Bound: 20,000 rows in total (`records_full`). A record is written or advanced only by the
transactions of R-3 (`sync_commit_download`, `queue_confirm`, `sync_record_identical`, `sync_record_conflict`, proposal outcomes).

### sync_plans (R-12)

| Column | Type | Notes |
|--------|------|-------|
| id | INTEGER PK AUTOINCREMENT | |
| pair_id | INTEGER NOT NULL | |
| trigger | TEXT NOT NULL CHECK(trigger IN ('tap','startup','auto')) | who started the comparison (informational) |
| state | TEXT NOT NULL CHECK(state IN ('ready','approved','running','done','interrupted','cancelled','superseded')) | |
| created_at | INTEGER NOT NULL | the comparison time; approval needs age <= 1 h |
| approved_at, finished_at | INTEGER | |
| incomplete | INTEGER NOT NULL DEFAULT 0 | a scan was incomplete (R-4); no proposals |
| incomplete_note | TEXT NOT NULL DEFAULT '' | count of folders not compared (a number, never names) |
| over_limit | TEXT NOT NULL DEFAULT '' | `''`, `files`, `bytes`, `both` |
| free_bytes, reserve_bytes, quota_free | INTEGER | what the plan was checked against (NULL = unknown) |
| n_download, n_upload, n_skip, n_keep_both, n_prop_server, n_prop_reader, n_failed | INTEGER NOT NULL DEFAULT 0 | counts, updated as entries finish; `n_skip` does not count the hidden `gone_both` entries |
| n_in_sync | INTEGER NOT NULL DEFAULT 0 | names that are in sync: counted, not listed (a folder with nothing to do has an empty plan however many files it holds) |
| b_download, b_upload, b_keep_both | INTEGER NOT NULL DEFAULT 0 | sizes in bytes |
| folders_new | INTEGER NOT NULL DEFAULT 0 | folders that would be created (count only) |
| truncated | INTEGER NOT NULL DEFAULT 0 | more than 5,000 entries: `plan_too_large`, not approvable |

At most one row per pair with a state other than `done`, `cancelled`, `superseded`, `interrupted` (`ready` is replaced in one
transaction when a new plan is stored). Rows of final states: the last `done` plan per pair is kept (for the result); older ones
are removed when a newer final plan is stored (bounded: <= 2 final rows per pair).

### sync_plan_entries

| Column | Type | Notes |
|--------|------|-------|
| plan_id | INTEGER NOT NULL | PK part |
| seq | INTEGER NOT NULL | PK part; plan order (also the order of execution and of the space check) |
| rel, local_rel | TEXT NOT NULL | |
| kind | TEXT NOT NULL CHECK(kind IN ('file','folder')) | |
| action | TEXT NOT NULL CHECK(action IN ('download','upload','skip','keep_both','propose_delete_server','propose_delete_reader')) | |
| reason | TEXT NOT NULL DEFAULT '' | for `skip`/unapprovable entries and the case of a proposal: a key of the table below |
| size | INTEGER | the size of what would be transferred (or of the file for a proposal) |
| plan_etag | TEXT NOT NULL DEFAULT '' | server version planned against (downloads, replaces) |
| plan_checksum | TEXT NOT NULL DEFAULT '' | the checksum text of the listing a download is verified against |
| plan_lsize, plan_lmtime | INTEGER | reader identity planned against (uploads, local deletes) |
| conflict_name | TEXT NOT NULL DEFAULT '' | for `keep_both`: the new name chosen on the reader (`down` and `both`) |
| conflict_name_server | TEXT NOT NULL DEFAULT '' | for `keep_both`: the new name chosen on the server (`up` and `both`) |
| records_below | INTEGER NOT NULL DEFAULT 0 | folder proposals: synced files below |
| allowed | TEXT NOT NULL DEFAULT '' | proposals: the comma-separated choices that are possible (`transfer_again`, `leave`, `delete`) |
| choice | TEXT NOT NULL DEFAULT '' | proposals: the user's choice, `''` = not chosen |
| removed | INTEGER NOT NULL DEFAULT 0 | removed from the plan by the user (R-12) |
| unapprovable | TEXT NOT NULL DEFAULT '' | `no_space`, `over_reserve`, ... : cannot be approved as it is |
| state | TEXT NOT NULL DEFAULT 'planned' CHECK(state IN ('planned','done','failed','skipped')) | |
| result_key | TEXT NOT NULL DEFAULT '' | why it failed or was skipped during the run |

Index `(plan_id, action)`. Bound: 5,000 rows per plan. Removed with their plan. The paged plan screen reads this table in `seq`
order with `LIMIT/OFFSET` (SQL paging, like the folder listing).

## Columns added to downloads (001 journal) - schema 5

| Column | Type | Notes |
|--------|------|-------|
| pair_id | INTEGER | the pair that records the result of this download (NULL for every download of feature 001) |
| target_root | TEXT NOT NULL DEFAULT '' | the absolute folder `target_path` is relative to (the pair's reader folder); `''` = the download root of 001 |
| checksum | TEXT NOT NULL DEFAULT '' | the `oc:checksums` text the finished file is verified against (a resumed job has no cached listing to ask) |
| strict | INTEGER NOT NULL DEFAULT 0 | 1: only the version in `expected_etag` is wanted (`If-Match`; a 412 or another ETag ends the job `changed_during_sync`) |

They let a paused pair download resume and let `Downloader::recover()` find its temporary file in the pair's folder after a kill.

## Columns added to queue_items (002) - schema 5

| Column | Type | Notes |
|--------|------|-------|
| pair_id | INTEGER | the pair whose plan created the item (NULL for every item of the user's own actions) |
| sync_rel | TEXT NOT NULL DEFAULT '' | the record the confirmation advances |
| sync_role | TEXT NOT NULL DEFAULT '' CHECK(sync_role IN ('','file','conflict_copy','delete')) | `delete`: a confirmed server deletion from a proposal (the record is dropped on confirmation) |

No foreign key: a removed pair leaves the item as an ordinary one; `queue_confirm` looks the pair up and does nothing for the sync
when it is gone. Index `(pair_id, state)` for the group send and the "change pending" lookup (`queue_sync_open_rels(pair_id)`).

## Reason and key vocabulary (plan entries, results)

`skip` reasons / `result_key` values (each has an English message and a German translation): `in_sync`, `direction`,
`kept_both`, `name_forbidden`, `name_exists`, `path_unsafe`, `path_too_long`, `source_unreadable`, `not_regular`,
`too_large_for_storage`, `unavailable` (no permission / encrypted / unsupported), `change_pending`, `by_user`,
`changed_during_sync`, `server_version_unknown`, `quota_exceeded`, `no_space`, `over_reserve`, `name_collision`, `hidden_name` (a name below the pair's server folder starts with a dot: the reader scan
skips hidden names, so such a file would look as if it vanished from the reader), `identical` (the same on both sides at the first sync: the
record is written on approval) and the hidden `gone_both` (a record whose file is gone on both sides: dropped on approval, not shown).
Pair `problem_key`: `server_folder_missing`, `reader_folder_missing`, `auth_failed`, `account_changed`, `no_network` (the pair is waiting for the network).
Run/general keys reuse 001/002: `storage_full`, `storage_readonly`, `trash_unavailable`, `queue_full`, `refresh_first`, `local_changed`.
New refusal keys: `pair_limit`, `pair_overlap`, `pair_folder_forbidden`, `pair_limit_invalid`, `sync_busy`, `plan_stale`,
`plan_not_approvable`, `plan_too_large`, `records_full`.

## State machines

```text
pair.state
  idle ──compare (tap/startup/auto)──▶ comparing ──plan stored──▶ plan_ready ──approve──▶ running ──all entries final──▶ idle (last_outcome done|partial|failed)
  comparing ──kill / error before the plan──▶ interrupted (at start) | idle with problem_key (error)
  running ──kill / cancel──▶ interrupted
  plan_ready ──cancel──▶ idle      plan_ready ──compare again──▶ comparing (the ready plan becomes superseded)
  interrupted ──compare (a new plan from the records)──▶ comparing
  any ──folder missing at compare──▶ idle (problem_key set; no plan, no proposals)

plan.state
  (none) ─▶ ready ──approve (checks of R-12)──▶ approved ──runner starts──▶ running ──▶ done | interrupted
  ready ──cancel──▶ cancelled      ready ──new plan for the pair──▶ superseded      interrupted ──new plan──▶ superseded

entry.state (approved plans only)
  planned ──transfer complete and record advanced (one transaction)──▶ done
  planned ──refused / failed during the run──▶ failed (result_key)
  planned ──changed during the sync / removed by the user / not approvable──▶ skipped (result_key)
```

## Effects of a sync on 001/002 records

| Event | Effect |
|-------|--------|
| Download committed | `sync_files` row written/advanced; the Downloader's journal row ended in the same transaction; `local_files` unchanged |
| Upload confirmed (`queue_confirm`, `pair_id` set, pair exists) | item `done`; `sync_files` row written/advanced; 002's cache patch (R-10 of 002) as before |
| Server deletion confirmed (`sync_role = delete`) | 002's effects; the `sync_files` row is dropped |
| Reader deletion confirmed by the user | file removed by `remove_pair_copy`; the row dropped; nothing on the server |
| Identical match / both gone / quiet choice | row written / dropped / `quiet = 1`, no transfer |
| Pair removed | `sync_pairs`, `sync_files`, `sync_plans`, `sync_plan_entries` rows removed; files on both sides and queue items untouched |
| Account changed (`save_account` for another server or user) | pairs and their rows of the old account stay but are marked `problem_key = account_changed` by `sync_pairs_mark_account` in the same transaction, and `SyncRunner`/`SyncBridge` refuse to compare, run or apply choices for them (no request, no file change); the user removes or re-creates them (FR-050) |
| Cache eviction (001) | removes only `folders`/`entries`; sync tables are not cache and are never evicted |

## Entities of the spec and where they live

| Spec entity | Representation |
|-------------|----------------|
| Sync pair | `sync_pairs` row |
| Sync record | `sync_files` row |
| Sync plan, plan entry | `sync_plans`, `sync_plan_entries` rows (paged) |
| Deletion proposal | a plan entry with action `propose_delete_*`, its `allowed` and `choice`; `sync_files.quiet` remembers "leave" |
| Conflict | a `keep_both` plan entry; the outcome is `sync_files.diverged` plus the one-sided copy records |
| Sync result | `sync_pairs.last_result_json` plus the last `done` plan's counts |
| Pair limits | `max_files`, `max_bytes` of the pair; the reserve is a constant of the build (R-11) |
| Queued item (002) | `queue_items` with `pair_id`/`sync_rel`/`sync_role` |
| Local copy (001) | not duplicated: the read-only view `synced_copy` over `sync_files` (R-15) |

## Size and count budgets (Principle V)

| Item | Bound |
|------|-------|
| Pairs | 8 |
| Records | 20,000 |
| Plan entries per plan | 5,000; at most 2 final plans per pair kept |
| Scan per side | 500 folders, depth 20, 5,000 files; held in memory as one sorted vector of at most 5,000 small entries |
| Database growth at the bounds | <= 10 MB (unverified) |

## As built

- `sync_files.copy_of TEXT NOT NULL DEFAULT ''` (schema 5, never released): for a conflict copy the rel of the original it was made for. The next plan makes only the copies that do not exist yet, so an interrupted keep-both run is finished, never repeated. A reader copy has `one_sided = reader` and the name of the copy as `rel` and `local_rel`; a server copy has `one_sided = server`, `rel = local_rel` = the copy's name.
- A conflict whose copies all exist but whose original is not yet `diverged` is a `skip` `kept_both` entry that carries the versions seen (`plan_etag`, `plan_lsize`, `plan_lmtime`); approval marks the original. An entry already diverged has no `plan_etag`.
- A record is `quiet` after `leave`; a folder whose records are all quiet is not proposed as a folder again (its files are judged one by one).
- Folder proposals offer `leave`, `delete` and `transfer_again` according to the direction; `delete` of a folder on the reader removes only unchanged synced files, on the server it is one delete item of the folder.
