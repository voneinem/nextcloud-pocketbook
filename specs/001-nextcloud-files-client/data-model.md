# Data Model: Read-only Nextcloud Files Client

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **Research**: [research.md](research.md)

Storage: SQLite file `pbcloud.db` in the data folder (`/mnt/ext1/system/pbcloud/`), schema **2** (schema 1 is the
baseline: `settings`, `sync_state`). One-way migration 1 → 2 in `core/src/store_migrations.cpp`; a build that finds
schema 3 or higher refuses to open the file (Principle III, already implemented in `Store::open`). The app password
is **not** in the database; it is the one value of the `credentials` file (research R-6).

All times are epoch seconds. All text from the server is stored as received (bytes, no normalization) and treated as
untrusted when it is displayed or used to build a local name.

## Paths

- **Remote path**: application-relative, decoded, always starts with `/`, never ends with `/` except the root `/`.
  Example: `/Books/Novels/story.epub`. Built from `d:href` by removing `/remote.php/dav/files/<user-id>`. Segment
  comparison is byte-exact and case-sensitive (the server is case-sensitive).
- **Local path**: relative to the download root (`/mnt/ext1/PBCloud/`), made only by `local_name.h` (research R-9).
  Stored as bytes. The download root itself is configuration, not data.

## Tables (schema 2, added by the migration)

### account (one row)

| Column | Type | Notes |
|--------|------|-------|
| id | INTEGER PK CHECK(id = 1) | exactly one account in this feature |
| base_url | TEXT NOT NULL | canonical `https://host[:port][/subpath]`, no trailing slash |
| login_name | TEXT NOT NULL | used for Basic authentication |
| user_id | TEXT NOT NULL | used in DAV paths; may differ from `login_name` |
| server_name | TEXT NOT NULL DEFAULT '' | shown after a Test; bounded to 64 characters |
| server_version | TEXT NOT NULL DEFAULT '' | e.g. `31.0.4` |
| quota_used | INTEGER | bytes, NULL = unknown |
| quota_total | INTEGER | bytes, NULL = unknown, -1 = unlimited |
| checked_at | INTEGER | last successful account check or Test |
| state | TEXT NOT NULL DEFAULT 'untested' CHECK(state IN ('untested','ok','needs_signin','problem')) | `needs_signin` after two consecutive 401 (research R-12) |
| problem_key | TEXT NOT NULL DEFAULT '' | stable error key of the last failed check, `''` = none |

Validation: `base_url` must pass the https-only check; `user_id` must be non-empty and contain no `/`, control
characters or `..`. The account row and the `credentials` file are written together: first the file (atomic), then
the row; sign-out deletes the file first (Principle IV).

### folders (cached listings, bounded)

| Column | Type | Notes |
|--------|------|-------|
| path | TEXT PK | remote path of the folder |
| etag | TEXT NOT NULL DEFAULT '' | as received, quotes included, `W/` kept |
| fileid | INTEGER | `oc:fileid`, NULL when absent |
| fetched_at | INTEGER | NULL = never confirmed by the server ("not synced") |
| last_access_at | INTEGER NOT NULL | for eviction |
| entry_count | INTEGER NOT NULL DEFAULT 0 | |
| gone | INTEGER NOT NULL DEFAULT 0 | 1 after a 404 on refresh ("removed on the server") |

### entries (rows of a cached listing)

| Column | Type | Notes |
|--------|------|-------|
| parent | TEXT NOT NULL | FK `folders(path)` ON DELETE CASCADE |
| name | TEXT NOT NULL | one segment as sent |
| kind | TEXT NOT NULL CHECK(kind IN ('file','folder')) | |
| size | INTEGER | bytes, NULL = unknown (`oc:size` / `getcontentlength`) |
| mtime | INTEGER | NULL = unknown |
| etag | TEXT NOT NULL DEFAULT '' | |
| fileid | INTEGER | |
| mime | TEXT NOT NULL DEFAULT '' | bounded to 100 characters |
| permissions | TEXT NOT NULL DEFAULT '' | `oc:permissions` letters, used only to describe availability |
| favorite | INTEGER NOT NULL DEFAULT 0 | |
| checksum | TEXT NOT NULL DEFAULT '' | `oc:checksums` value as received, bounded to 256 bytes |
| availability | TEXT NOT NULL DEFAULT 'ok' CHECK(availability IN ('ok','no_permission','encrypted','unsupported','unreadable')) | research R-7 |
| reason | TEXT NOT NULL DEFAULT '' | short key, e.g. `no_read_permission`, `end_to_end_encrypted`, `status_403` |

PRIMARY KEY (parent, name). Indexes: `(parent, kind, name)` and, for sorting, expressions are computed with the
registered function `pbc_fold(name)`; no extra index is needed for 10,000 rows per folder.

Validation: name is not empty, has no `/`, is not `.` or `..`, and is at most 1,024 bytes; entries violating this are
not stored and counted in the refresh result (`skipped_hostile`). At most 10,000 entries per folder (research R-7).

Only folders the user opened (or that were already cached) get rows here: an Update that lists a folder the user never
opened uses the answer transiently and writes no `folders`/`entries` row (research R-10).

Bound: total `entries` ≤ 50,000 and `folders` ≤ 2,000. Eviction runs in the same transaction as the insert that would
exceed it: delete `folders` rows (cascade) in order of oldest `last_access_at`, never the root, never the folder being
written. Eviction touches no other table.

### local_files (downloaded copies; independent of the cache)

| Column | Type | Notes |
|--------|------|-------|
| remote_path | TEXT PK | the entry this copy came from |
| local_path | TEXT NOT NULL UNIQUE | relative to the download root (case-insensitive uniqueness is enforced by `local_name.h`, not by SQL) |
| size | INTEGER NOT NULL | bytes at commit, from `stat` after the rename |
| mtime | INTEGER NOT NULL | `stat` mtime after the rename |
| remote_etag | TEXT NOT NULL | the server version this copy was downloaded from |
| remote_size | INTEGER | size the server announced |
| remote_mtime | INTEGER | |
| downloaded_at | INTEGER NOT NULL | |
| server_missing | INTEGER NOT NULL DEFAULT 0 | set when a successful listing of the parent lacks the entry; cleared when it reappears |
| seen_etag | TEXT NOT NULL DEFAULT '' | the ETag the user chose to keep their own copy against ("Keep my copy"); `''` = none |

A row exists only after the file is complete and renamed. It is deleted only by the Space screen (explicit user
action), never by cache eviction and never because the server no longer has the file (Principle III).

### offline_marks

| Column | Type | Notes |
|--------|------|-------|
| remote_path | TEXT PK | |
| kind | TEXT NOT NULL CHECK(kind IN ('file','folder')) | a folder mark covers all files below it, subfolders included (clarification) |
| marked_at | INTEGER NOT NULL | |

Removing a mark never removes a local copy. A path below an already marked folder may be marked too; the more specific
mark is kept (it survives removing the folder mark).

### favorites (cached list of Nextcloud favorites)

| Column | Type | Notes |
|--------|------|-------|
| remote_path | TEXT PK | |
| kind | TEXT NOT NULL | file / folder |
| size, mtime, etag, mime | as in `entries` | |
| position | INTEGER NOT NULL | order as received |

The time of the last successful fetch is the setting `favorites_fetched_at`. A new successful fetch replaces all rows
in one transaction.

### downloads (journal of transfers in progress)

| Column | Type | Notes |
|--------|------|-------|
| id | INTEGER PK AUTOINCREMENT | also the `.pbc-<id>.part` name |
| remote_path | TEXT NOT NULL | |
| target_path | TEXT NOT NULL | relative local path chosen for the commit |
| expected_etag | TEXT NOT NULL | strong ETag usable for `If-Range`, `''` if none/weak |
| expected_size | INTEGER | NULL = unknown |
| received | INTEGER NOT NULL DEFAULT 0 | bytes safely on disk at the last checkpoint (informational; the file size on disk is authoritative) |
| state | TEXT NOT NULL CHECK(state IN ('running','waiting','paused')) | |
| started_at, updated_at | INTEGER NOT NULL | |

Written **before** the first request of a job (Principle III). Deleted in the same transaction that inserts or updates
`local_files` at commit, or on cancel/failure after the partial file was removed. At start: rows without a partial
file are deleted, `.pbc-*.part` files without a row are deleted, `running` rows become `paused`.

### settings (existing table) — keys added

| Key | Values | Default |
|-----|--------|---------|
| `sort_key` | `name`, `date`, `size` | `name` |
| `sort_dir` | `asc`, `desc` | `asc` |
| `large_file_warn_mib` | integer 1–4095 | 100 |
| `favorites_fetched_at` | epoch seconds | unset |
| `last_folder` | remote path last shown (restored without a request) | `/` |
| `ui_language` | existing | `auto` |

### sync_state (existing row) — use

`outcome`/`counts_json`/`problem_key` describe the last **Update**: `counts_json` holds
`{"folders":n,"files":n,"updated":n,"new":n,"skipped":n,"decision":n,"removed":n,"failed":n,"unchecked_folders":n}`;
`problem_area` is `update` or `account`; `clock_skew_s` is set after every successful server answer (existing
`clock_check.h`).

## Entity relationships

```text
account 1 ── 0..n folders ── 0..n entries           (cache; bounded; evictable)
account 1 ── 0..n local_files                       (durable; user data)
account 1 ── 0..n offline_marks                     (durable; user choice)
account 1 ── 0..1 favorites list                    (cache; replaced as a whole)
downloads 0..n ── 1 local_files (at commit)         (journal; transient)
```

`local_files`, `offline_marks` and `downloads` refer to remote **paths**, not to rows of `entries`, so evicting a
listing never breaks them. A renamed or moved file on the server appears as a removed path and a new path; the old
copy stays flagged `server_missing` and the user decides (FR-028). Using `oc:fileid` to follow renames is a later
improvement and is not needed to be correct.

## Entry state (computed)

Inputs: the cached `entries` row (or `favorites`/search result) `E`, the `local_files` row `L` (may be absent), and
the file system `stat(L.local_path)` `S`.

```text
no L                                          -> not_downloaded
L, S missing                                  -> missing_locally
L.server_missing = 1                          -> removed_on_server   (checked first among states with L)
local_changed  := S.size != L.size or S.mtime != L.mtime
server_changed := E.etag != L.remote_etag  (when E.etag == '' compare size and mtime instead)
                  and E.etag != L.seen_etag
local_changed and server_changed              -> conflict
local_changed                                 -> changed_on_reader
server_changed                                -> outdated
otherwise                                     -> downloaded
```

Folders have no state of their own; the list shows a "kept offline" mark for a folder in `offline_marks` and counts of
child states in the details line only when the child listing is cached.

A weak ETag (`W/…`) is compared for equality like any other ETag (it is only unusable for `If-Range`).
When `E` is a stale cache (never refreshed since the download), the state is computed from what is known and the screen
shows the listing age; "outdated" can therefore only appear after a refresh or an Update (spec US4: the list shows what
the reader knows).

## State transitions of a download job

```text
        start (preflight ok, journal row written)
(none) ───────────────────────────────────────────▶ running
running ──connection lost / stall──▶ waiting ──Wi-Fi back──▶ running
waiting ──10 min or 5 attempts──▶ paused ──user taps Resume──▶ running
running ──size and checksum ok, rename ok──▶ done      (journal row deleted; local_files row written)
running|waiting|paused ──Cancel──▶ cancelled           (partial file and row deleted)
running ──checksum/size mismatch, 4xx/5xx final──▶ failed (partial file and row deleted; error key shown)
running ──final file exists and local copy is changed──▶ blocked (partial file kept, conflict page; user choice continues or cancels)
```

`done`, `cancelled` and `failed` leave no `.part` file. `blocked` keeps the verified partial file until the user
chooses (so the bytes are not downloaded twice); if the app is closed it is treated as `paused`.

## Kept-offline item scope

A file is **in scope for Update** if its remote path equals a `file` mark, or lies below a `folder` mark at any depth.
Scope is evaluated from the marks at the time Update starts. A file that the user downloaded by hand and that is not in
scope is never touched by Update.

## Size and count budgets (Principle V)

| Item | Bound |
|------|-------|
| Entries per folder | 10,000 |
| Entries in the cache | 50,000 |
| Cached folders | 2,000 |
| Folders walked per Update | 500, depth 20 |
| Server search results | 100 |
| Favorites kept | 2,000 |
| `local_files` rows | not bounded by the app (bounded by the card); the Space screen pages them |
| Database file | ≤ 50 MB expected at the cache limits (about 300 bytes per entry plus indexes); checked by a budget test |

## As built: schema 3

`favorites` has two more columns, added by the step 2 -> 3 of `store_migrations.cpp`: `availability TEXT NOT NULL DEFAULT 'ok'` (same values as in `entries`) and
`reason TEXT NOT NULL DEFAULT ''`. A favorite the user may not read, or an encrypted folder, keeps its marker so that it is shown as unavailable and is never
downloaded (FR-013). Rows written by a schema 2 build read as available. `Store::kSchemaVersion` is 3; a build that knows only schema 2 refuses the file.

