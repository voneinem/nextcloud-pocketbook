# Data Model: Upload and Manage Files on the Server

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **Research**: [research.md](research.md) | **Builds on**: [feature 001 data model](../001-nextcloud-files-client/data-model.md)

Storage: the same SQLite file `pbcloud.db` (schema **4** after this feature; schema 3 is feature 001 as built). One-way
migration 3 -> 4 in `core/src/store_migrations.cpp`; a build that finds schema 5 or higher refuses the file, and an older
build (schema 3) refuses a schema-4 file (`schema_too_new`, already implemented). Nothing in this feature is stored outside
the database except the user's source files (never copied, never modified) and the upload sessions on the server.

All times are epoch seconds. Remote paths are as in 001: decoded, start with `/`, no trailing `/` except the root, compared
byte-exactly and case-sensitively. Local paths of the reader are absolute and are checked by the picker rules (research R-13).

## Tables added (schema 4)

### queue_items (durable user data; never evicted)

One row per atomic change. A folder pick is a group of rows.

| Column | Type | Notes |
|--------|------|-------|
| id | INTEGER PK AUTOINCREMENT | creation order = send order |
| group_id | INTEGER | rows created by one pick or one multi-entry action share it; NULL for a single item |
| kind | TEXT NOT NULL CHECK(kind IN ('upload','mkdir','rename','move','copy','delete')) | |
| entry_kind | TEXT NOT NULL CHECK(entry_kind IN ('file','folder')) | what is uploaded/created/renamed/moved/copied/deleted |
| state | TEXT NOT NULL CHECK(state IN ('queued','sending','done','failed','conflict','unknown')) DEFAULT 'queued' | spec states: not synced = `queued`; sent = `done` |
| step | TEXT NOT NULL DEFAULT '' | while `sending`: `prepare` \| `chunks` \| `assemble` \| `single` \| `request`; decides recovery (R-6.5) |
| server | TEXT NOT NULL | base address the item was queued for |
| user_id | TEXT NOT NULL | user id it was queued for (items are never sent to another account) |
| src_local | TEXT NOT NULL DEFAULT '' | upload: absolute path of the source on the reader (checked by the picker and again at send) |
| src_size | INTEGER | upload: size recorded when the item started (NULL before the first start; the pick-time size is shown) |
| src_mtime | INTEGER | upload: modification time in seconds, same moments |
| src_remote | TEXT NOT NULL DEFAULT '' | rename/move/copy/delete: remote path of the entry |
| src_etag | TEXT NOT NULL DEFAULT '' | the server version of that entry the reader last saw; `''` = none (then rename/move/copy/delete cannot be queued) |
| src_fileid | INTEGER | file id of that entry (used by the outcome check of a move) |
| dst_parent | TEXT NOT NULL DEFAULT '' | upload/mkdir/move/copy: remote path of the target folder; rename: the entry's own folder |
| dst_name | TEXT NOT NULL DEFAULT '' | the name on the server after the change; validated by `is_valid_name` and the server lists |
| replace | INTEGER NOT NULL DEFAULT 0 | 1: the user confirmed replacing an existing file (upload, move, copy) |
| dst_etag | TEXT NOT NULL DEFAULT '' | replace: the ETag of the file to be replaced as confirmed (strong, non-empty) |
| depends_on | INTEGER | id of an item that must be `done` first (a `mkdir`); NULL = none |
| upload_id | TEXT NOT NULL DEFAULT '' | chunked upload: the id of the upload folder |
| chunk_size | INTEGER | chunked upload |
| checksum | TEXT NOT NULL DEFAULT '' | upload: SHA-256 hex of the source when the item started |
| result_etag | TEXT NOT NULL DEFAULT '' | the server version after the change, once known |
| result_fileid | INTEGER | the file id after the change, once known |
| reason | TEXT NOT NULL DEFAULT '' | `failed`/`conflict`/`unknown`: an error key (research R-12) or a hold reason (`name_exists`, `changed_on_server`, `source_gone`, `target_gone`, `no_answer`) |
| created_at, updated_at | INTEGER NOT NULL | |

Indexes: `(state, id)`, `(dst_parent)`, `(src_remote)`, `(group_id)`. `depends_on` has no foreign key: a `done` row is
removed only when the user leaves the queue screen, and the dependants are `done` or waiting on a non-`done` row that is
never removed by the app (a held parent keeps its dependants waiting, research R-6.3).

Bound: 2,000 rows (`queue_full` beyond; checked in the same transaction as the insert; a group is inserted whole or not at all).

Validation: `server`/`user_id` non-empty; `dst_name` valid; `src_local` for an upload under the picker root and outside the
app's data folder; `replace = 1` only with a non-empty strong `dst_etag` and entry_kind `file`; a rename/move/copy/delete
with an empty or weak `src_etag` is not insertable (`name_exists`-free refusal `refresh_first`); `dst_parent` is a valid remote path.

### server_caps (cache of what the last account check learned; one row)

| Column | Type | Notes |
|--------|------|-------|
| id | INTEGER PK CHECK(id = 1) | |
| trash | INTEGER | NULL unknown, 0 no, 1 yes (`files.undelete`); NULL counts as no (research R-9) |
| chunking | INTEGER | NULL unknown, 0 no, 1 yes (`dav.chunking`) |
| max_chunk | INTEGER | `files.chunked_upload.max_size` in bytes, NULL = not stated |
| forbidden_chars | TEXT NOT NULL DEFAULT '' | the characters, up to 64 |
| forbidden_names | TEXT NOT NULL DEFAULT '' | newline-separated, up to 64 names of 32 bytes |
| forbidden_basenames | TEXT NOT NULL DEFAULT '' | same bounds |
| forbidden_extensions | TEXT NOT NULL DEFAULT '' | same bounds |
| checked_at | INTEGER | |

Replaced as a whole at every successful account check; cleared by `save_account` for another server or user and by
`clear_account`. A missing list is stored as `''` and means "unknown", never "nothing forbidden".

### settings (existing table) - keys added

| Key | Values | Default |
|-----|--------|---------|
| `auto_sync` | `1`, `0` | `1` (on; constitution VI: the default is stated here and in the plan) |
| `picker_view` | `list`, `tile` | `list` |
| `last_send_at` | epoch seconds of the end of the last send run | unset |
| `last_send_counts` | JSON `{"done":n,"failed":n,"conflict":n,"unknown":n,"left":n}` | unset |

`sync_state` (001: the last Update) is not reused: the queue has its own last-run values above (an Update and a send are
different things and may both be shown).

## State machine of a queue item

```text
                       pick / action (row committed, nothing sent)
(none) ─────────────────────────────────────────────▶ queued ("not synced")
queued ──a run takes it, step written──▶ sending
sending ──server confirmed (effects of R-10 in the same transaction)──▶ done ──leaving the queue screen──▶ (row removed)
sending ──a refusal that did not execute (R-12)──▶ failed      ──user: Retry / Edit / Remove──▶ queued | (removed)
sending ──412, 405, 409 held by R-3/R-8──▶ conflict            ──user: choice──▶ queued (new name / replace / apply) | (removed)
sending ──no answer after the request may have arrived (R-7)──▶ unknown ──user: Check / Send again / Remove──▶ queued | done | (removed)
sending ──no connection, nothing sent (delivery = not_sent)──▶ queued   (reason: no_connection; the run stops)
sending ──cancel between chunks──▶ queued   (upload folder kept for resume)
sending ──app killed / power lost──▶ (at start, no network) queued (step prepare|chunks) | unknown (step single|assemble|request)
queued ──user: Remove──▶ (removed; an upload folder is deleted first, best effort)
```

`unknown`, `conflict` and `failed` are never taken by a run. Only the user's tap (or the choice buttons) moves them.
`sending` exists for one item at a time. An item whose `depends_on` row is not `done` is `queued` but not taken.

## Listing overlay (FR-008, research R-6.8)

The folder page shows the **cached entries of the folder merged with the open queue rows that touch it**. Rows that are
`done` are no longer an overlay (their effects were written into `entries` by R-10). The merge is done by one SQL statement over
`entries` and `queue_items` (a temporary view per call), so sorting and paging stay in SQL.

| Open item | In the **source/own** folder | In the **target** folder |
|-----------|------------------------------|--------------------------|
| upload (new) | - | a pending file row: the name, the picked size, no date, mark "not synced" |
| upload (replace) | the existing row is shown with the mark | - |
| mkdir | - | a pending folder row with the mark |
| rename | the entry shown under its **new** name with the mark; its identity (the old path) is kept for taps | - |
| move | the entry stays visible with "moving", mark | a pending row (name, kind) with the mark |
| copy | - | a pending row with the mark |
| delete | the entry stays visible with "delete pending", mark | - |

Row fields added to the browse row map: `pending` (`""` or `new`, `renamed`, `moving`, `copying`, `deleting`, `replacing`),
`syncState` (`""`, `queued`, `sending`, `failed`, `conflict`, `unknown`), `itemId`. A pending row is not a real entry:
a tap opens the queue item (not the details page, not a folder listing); a pending folder can be chosen as an upload
target (its `dst_parent` is its future path; items for it depend on its `mkdir`, research R-6).

## Entities of the spec and where they live

| Spec entity | Representation |
|-------------|----------------|
| Queued item | `queue_items` row |
| Upload progress | `upload_id`, `chunk_size`, `src_size`, `src_mtime`, `checksum`, `step` of the row; the server's upload folder is the authority for which chunks exist |
| Collision / conflict | the row in state `conflict` with `reason` and the open choice; the choices (keep both = new `dst_name`, replace = `replace`+`dst_etag`, skip = removal) are written back into the row |
| Remote entry (extended) | 001's `entries` row plus the overlay fields above |
| Local copy / offline mark | 001's `local_files` and `offline_marks`, rewritten by R-10 on rename/move |
| Server refusal | `reason` (an error key) |
| Auto Sync setting | `settings.auto_sync` |
| Browser layout setting | `settings.picker_view` |

## Cache, eviction and the queue

Cache eviction (001) removes only `folders`/`entries` rows. The queue, `server_caps` and the settings are not cache: they
are never evicted, and eviction never changes what the overlay shows for an open item (the item carries its own names and
paths; the overlay only needs `entries` for the rows it decorates). Clearing the cache on the Storage screen does not touch
the queue.

## Effects on 001 records (single transaction per item, research R-10)

```text
done(upload new)      entries += file row          local_files, marks: unchanged
done(upload replace)  entries  = file row          local_files: linked only if src_local is the recorded copy of the same remote path
                                                    and (size, mtime) unchanged since the item started
done(mkdir)           entries += folder row
done(rename|move)     entries -= old, += new       local_files.remote_path, offline_marks.remote_path rewritten (folder: by prefix)
done(copy)            entries += copy row
done(delete)          entries -= row (and below)   local_files, marks: unchanged (the 001 states show "removed on the server")
```

## Size and count budgets (Principle V)

| Item | Bound |
|------|-------|
| Queue rows | 2,000 |
| Folder pick walk | 2,000 items, depth 12 |
| Forbidden-name lists | 64 entries of 32 bytes each, per list |
| Database growth at the queue limit | <= 2 MB (about 400 bytes per row plus indexes) |
| Chunks per upload | 10,000 (never reached below 97 GiB) |
