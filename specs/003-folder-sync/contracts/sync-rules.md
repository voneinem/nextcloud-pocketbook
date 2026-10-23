# Contract: Sync rules (the decision table and the invariants)

**Feature**: [../spec.md](../spec.md) | **Research**: [../research.md](../research.md) (R-3, R-5, R-9, R-10) | **Data model**: [../data-model.md](../data-model.md)

This feature adds **no new server request**. It uses `PROPFIND Depth: 1` (listings), `GET` (downloads, with `Range`/`If-Range`
and `If-Match` for the planned version) and, through the queue of feature 002, `PUT`, `MKCOL` and `DELETE` with the conditions
of that feature's [server-requests.md](../../002-manage-server-files/contracts/server-requests.md). Everything that differs
between 002 and a sync is *which* operations are issued and *when* - that is this document. It is the specification of the pure
function `SyncPlanner::plan` and the test table of `test_sync_planner`. Behavior against a real server is **unverified**.

## Vocabulary

For one name `n` below the pair's folders:

| Symbol | Meaning |
|--------|---------|
| `S` / `L` | `n` is present on the server / on the reader in the (complete) scan |
| `R` | a record exists for `n` |
| `Sc` | the server version changed: `S`, `R`, and the ETag differs from `server_etag` (or the listing has no ETag) |
| `Lc` | the reader file changed: `L`, `R`, and not "unchanged" by R-3 |
| `D` | the record is `diverged` |
| `Q` | a record is `quiet` |
| `P` | an open queue item (any user) names `n` as source or target |
| `dir` | `down`, `up` or `both` |
| `to-reader` allowed | `dir` is `down` or `both`; `to-server` allowed | `dir` is `up` or `both` |
| `incomplete(side)` | the scan of that side did not complete (bounds, failed folder, partial listing) |

A **complete scan of both sides** is required for any `missing` conclusion; with an incomplete side, names that are absent
from that side are treated as *unknown*, never as missing (FR-029).

## Decision table (first matching row wins)

| # | Situation | Action | Reason / note |
|---|-----------|--------|---------------|
| 1 | `P` | `skip` | `change_pending` (nothing is planned for a name that has an open queue item) |
| 2 | server entry unavailable (no permission, encrypted, unsupported) | `skip` | `unavailable` |
| 3 | reader entry not a regular file (link, special, unreadable) | `skip` | `not_regular` / `source_unreadable` |
| 4 | name cannot be made safe/valid for the other side | `skip` | `path_unsafe`, `path_too_long`, `name_forbidden`, `name_collision` |
| 5 | size >= 4 GiB on the server (needs the reader), or 0 bytes / >= 4 GiB on the reader (needs the server) | `skip` | `too_large_for_storage` / `source_unreadable` |
| 6 | `S`, not `L`, not `R` | `download` if to-reader allowed, else not listed | new on the server |
| 7 | `L`, not `S`, not `R` | `upload` if to-server allowed, else not listed | new on the reader |
| 8 | `S`, `L`, not `R`, identical (R-3: name, size, checksum) | `skip` | `in_sync`; the record is written when the plan is approved |
| 9 | `S`, `L`, not `R`, not identical | `keep_both` | first-sync clash (Story 6.4) |
| 10 | `S`, `L`, `R`, `D` | `skip` | `kept_both` |
| 11 | `S`, `L`, `R`, not `Sc`, not `Lc` | `skip` | `in_sync` |
| 12 | `S`, `L`, `R`, `Sc`, not `Lc` | `download` if to-reader allowed, else `skip` | `direction` |
| 13 | `S`, `L`, `R`, not `Sc`, `Lc` | `upload` if to-server allowed, else `skip` | `direction`; a replace of the server file needs a strong `server_etag`, else `skip` `server_version_unknown` |
| 14 | `S`, `L`, `R`, `Sc`, `Lc` | `keep_both` | changed on both sides |
| 15 | `R`, `L`, not `S`, complete server scan, not `Lc`, not `Q` | `propose_delete_reader` | deleted on the server; allowed = `leave`, `delete`, plus `transfer_again` (upload) if to-server allowed |
| 16 | `R`, `L`, not `S`, complete server scan, `Lc`, not `Q` | `propose_delete_reader` with `delete` **not** allowed | the changed reader file is kept (Story 6.5, 7.6); allowed = `leave`, plus `transfer_again` if to-server allowed |
| 17 | `R`, `S`, not `L`, complete reader scan, not `Sc`, not `Q` | `propose_delete_server` | deleted on the reader; allowed = `leave`, plus `transfer_again` (download) if to-reader allowed, plus `delete` if to-server allowed and the server has a trash |
| 18 | `R`, `S`, not `L`, complete reader scan, `Sc`, not `Q` | `propose_delete_server` with `delete` **not** allowed | the changed server file is kept; allowed = `leave`, plus `transfer_again` if to-reader allowed |
| 19 | `R`, not `S`, not `L`, both scans complete | record dropped on approval; not listed | |
| 20 | `R`, `Q`, one side missing, nothing changed since | `skip` | `in_sync` (quiet) - proposed again only when the present file changes |
| 21 | record `one_sided`, the other side lacks it | `skip` | `kept_both`; never transferred, never proposed |
| 22 | anything else | `skip` | `in_sync` |

Folders: a folder with at least one record below it that is missing as a whole (all its records have the same missing side)
is **one** proposal (rows 15/17 applied to the folder, `records_below = n`), and its files are not listed separately. A folder
that exists on only one side without records produces nothing by itself; its files do (rows 6/7), and the receiving side
creates the folders on the way.

### Limits, reserve and quota (after the table)

1. Added to the reader = sum of `size` of `download` entries and of the reader-side copies of `keep_both` entries (row 9/14 when
   to-reader allowed, or `dir = down`).
2. `projected_files = held_files + count`, `projected_bytes = held_bytes + sum`. If either exceeds the pair's limit and
   `count > 0`: `plan.over_limit` is set (`files`, `bytes` or `both`) with the excess; the plan is not approvable.
3. Free-space walk in plan order: an entry whose size would take `free - added_so_far` below the reserve gets
   `unapprovable = no_space`.
4. An upload larger than the known free quota gets `skip` `quota_exceeded`.

### Conflict naming (row 9, 14; research R-9)

| Direction | Reader side | Server side |
|-----------|-------------|-------------|
| `down` | `keep_both` adds `<stem> (server copy YYYY-MM-DD)<ext>` with the server version | nothing |
| `up` | nothing | `keep_both` adds `<stem> (reader copy YYYY-MM-DD)<ext>` with the reader version |
| `both` | the server copy | the reader copy |

The original name keeps its side's version on both sides. Both new names are chosen by `conflict_name` so that they are unused
on the receiving side (listing, records, open queue items) and are stored in the plan entry (`conflict_name`, and for `both` a
second name) so that the same plan, executed twice, would name them identically.

## Invariants (each is tested)

1. **Pure and deterministic.** For equal inputs `plan` returns an equal plan, in the same order (sorted by `rel`, byte-wise).
2. **No change before approval.** `plan` and its storage write plan rows only; no file, record, queue item or request is created
   by it (SC-002).
3. **Direction is a hard wall.** With `dir = down` no entry has action `upload` and no `keep_both` writes to the server; with
   `dir = up` no entry has action `download` and no `keep_both` writes to the reader; no proposal offers a choice that would
   cross the wall (FR-013, FR-014).
4. **Deletions are only proposals.** No row of the table produces an action that removes a file; the only code path that removes
   a file or queues a deletion is the *Apply choices* of the user (R-10), and it re-checks the conditions.
5. **Records advance after completion.** A record changes only in the transactions listed in R-3.
6. **Repeat gives the same result.** Applying `plan` to the state after a complete run yields only `in_sync`/`kept_both`/`quiet`
   skips (SC-003, SC-004).
7. **Conflict copies do not bounce.** After a `keep_both` has run, the next plan lists the original as `kept_both` and the
   copies as `kept_both` (row 21); no entry transfers them (SC-005).
8. **Unknown is not missing.** With an incomplete side, no proposal and no record drop is produced for names absent from that
   side (FR-029).
9. **Safe names only.** Every `local_rel` is the output of `choose_local_path` under the pair's reader folder and every server
   `rel` the output of `check_new_name`; hostile names become `skip` rows (SC-012).
10. **Bounded.** More than 5,000 entries sets `truncated`; the plan is not approvable (`plan_too_large`).
