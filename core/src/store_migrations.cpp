// The built-in schema upgrades. Every step is a script that Store::open runs inside ONE transaction after a copy of the
// database file, followed by an integrity check. Steps only go forward; an older build refuses a database of a newer
// version (schema_too_new). `PRAGMA user_version` is the only version source.
//
// Step 1 -> 2 adds the tables of the files client (specs/001-nextcloud-files-client/data-model.md; step 2 -> 3 adds two columns to favorites; step 3 -> 4 adds the queue of feature 002). A fresh database is
// created as schema 1 and then migrated, so there is one path to test. Tests add further steps through
// StoreOptions::extra_migrations.

#include "pbc/store.h"

namespace pbc {

namespace {

const char* kStep1To2 = R"SQL(
-- the one account of this feature
CREATE TABLE account(
    id INTEGER PRIMARY KEY CHECK(id = 1),
    base_url TEXT NOT NULL,
    login_name TEXT NOT NULL,
    user_id TEXT NOT NULL,
    server_name TEXT NOT NULL DEFAULT '',
    server_version TEXT NOT NULL DEFAULT '',
    quota_used INTEGER,
    quota_total INTEGER,                    -- NULL = unknown, -1 = unlimited
    checked_at INTEGER,
    state TEXT NOT NULL DEFAULT 'untested' CHECK(state IN ('untested','ok','needs_signin','problem')),
    problem_key TEXT NOT NULL DEFAULT ''
);

-- cached listings of visited folders (bounded; evictable)
CREATE TABLE folders(
    path TEXT PRIMARY KEY,
    etag TEXT NOT NULL DEFAULT '',
    fileid INTEGER,
    fetched_at INTEGER,                     -- NULL = never confirmed by the server ("not synced")
    last_access_at INTEGER NOT NULL,
    entry_count INTEGER NOT NULL DEFAULT 0,
    gone INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE entries(
    parent TEXT NOT NULL REFERENCES folders(path) ON DELETE CASCADE,
    name TEXT NOT NULL CHECK(length(name) > 0 AND instr(name, '/') = 0),
    kind TEXT NOT NULL CHECK(kind IN ('file','folder')),
    size INTEGER,
    mtime INTEGER,
    etag TEXT NOT NULL DEFAULT '',
    fileid INTEGER,
    mime TEXT NOT NULL DEFAULT '',
    permissions TEXT NOT NULL DEFAULT '',
    favorite INTEGER NOT NULL DEFAULT 0,
    checksum TEXT NOT NULL DEFAULT '',
    availability TEXT NOT NULL DEFAULT 'ok' CHECK(availability IN ('ok','no_permission','encrypted','unsupported','unreadable')),
    reason TEXT NOT NULL DEFAULT '',
    PRIMARY KEY(parent, name)
);
CREATE INDEX entries_by_kind_name ON entries(parent, kind, name);

-- durable user data: downloaded copies, kept-offline marks. Never touched by cache eviction.
CREATE TABLE local_files(
    remote_path TEXT PRIMARY KEY,
    local_path TEXT NOT NULL UNIQUE,
    size INTEGER NOT NULL,
    mtime INTEGER NOT NULL,
    remote_etag TEXT NOT NULL,
    remote_size INTEGER,
    remote_mtime INTEGER,
    downloaded_at INTEGER NOT NULL,
    server_missing INTEGER NOT NULL DEFAULT 0,
    seen_etag TEXT NOT NULL DEFAULT ''
);

CREATE TABLE offline_marks(
    remote_path TEXT PRIMARY KEY,
    kind TEXT NOT NULL CHECK(kind IN ('file','folder')),
    marked_at INTEGER NOT NULL
);

CREATE TABLE favorites(
    remote_path TEXT PRIMARY KEY,
    kind TEXT NOT NULL CHECK(kind IN ('file','folder')),
    size INTEGER,
    mtime INTEGER,
    etag TEXT NOT NULL DEFAULT '',
    mime TEXT NOT NULL DEFAULT '',
    position INTEGER NOT NULL
);

-- journal of transfers in progress, written before the first request of a job
CREATE TABLE downloads(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    remote_path TEXT NOT NULL,
    target_path TEXT NOT NULL,
    expected_etag TEXT NOT NULL DEFAULT '',
    expected_size INTEGER,
    received INTEGER NOT NULL DEFAULT 0,
    state TEXT NOT NULL CHECK(state IN ('running','waiting','paused')),
    started_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
)SQL";

// Step 3 -> 4: the queue of the write side (specs/002-manage-server-files/data-model.md) and the server capabilities it needs. The queue
// is durable user data: cache eviction and clearing the cache never touch it.
const char* kStep3To4 = R"SQL(
CREATE TABLE queue_items(
    id INTEGER PRIMARY KEY AUTOINCREMENT,         -- creation order = send order
    group_id INTEGER,                             -- rows of one pick or one multi-entry action
    kind TEXT NOT NULL CHECK(kind IN ('upload','mkdir','rename','move','copy','delete')),
    entry_kind TEXT NOT NULL CHECK(entry_kind IN ('file','folder')),
    state TEXT NOT NULL CHECK(state IN ('queued','sending','done','failed','conflict','unknown')) DEFAULT 'queued',
    step TEXT NOT NULL DEFAULT '',                -- while sending: prepare | chunks | assemble | single | request
    server TEXT NOT NULL,                         -- the account the item was queued for
    user_id TEXT NOT NULL,
    src_local TEXT NOT NULL DEFAULT '',           -- upload: absolute path of the source on the reader
    src_size INTEGER,
    src_mtime INTEGER,
    src_remote TEXT NOT NULL DEFAULT '',          -- rename/move/copy/delete: the entry
    src_etag TEXT NOT NULL DEFAULT '',
    src_fileid INTEGER,
    dst_parent TEXT NOT NULL DEFAULT '',
    dst_name TEXT NOT NULL DEFAULT '',
    "replace" INTEGER NOT NULL DEFAULT 0,         -- 1: the user confirmed replacing an existing file
    dst_etag TEXT NOT NULL DEFAULT '',
    depends_on INTEGER,                           -- an item that must be done first (a mkdir)
    upload_id TEXT NOT NULL DEFAULT '',
    chunk_size INTEGER,
    checksum TEXT NOT NULL DEFAULT '',
    result_etag TEXT NOT NULL DEFAULT '',
    result_fileid INTEGER,
    reason TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE INDEX queue_items_by_state ON queue_items(state, id);
CREATE INDEX queue_items_by_dst_parent ON queue_items(dst_parent);
CREATE INDEX queue_items_by_src_remote ON queue_items(src_remote);
CREATE INDEX queue_items_by_group ON queue_items(group_id);

-- what the last account check learned about the server (one row); an unknown value is NULL or ''
CREATE TABLE server_caps(
    id INTEGER PRIMARY KEY CHECK(id = 1),
    trash INTEGER,
    chunking INTEGER,
    max_chunk INTEGER,
    forbidden_chars TEXT NOT NULL DEFAULT '',
    forbidden_names TEXT NOT NULL DEFAULT '',
    forbidden_basenames TEXT NOT NULL DEFAULT '',
    forbidden_extensions TEXT NOT NULL DEFAULT '',
    checked_at INTEGER
);
)SQL";

// Step 2 -> 3: a favorite the user may not read must stay marked as such (FR-013); rows written by 0.1.0 read as available.
const char* kStep2To3 = R"SQL(
ALTER TABLE favorites ADD COLUMN availability TEXT NOT NULL DEFAULT 'ok' CHECK(availability IN ('ok','no_permission','encrypted','unsupported','unreadable'));
ALTER TABLE favorites ADD COLUMN reason TEXT NOT NULL DEFAULT '';
)SQL";

}  // namespace

const std::vector<Migration>& builtin_migrations() {
    static const std::vector<Migration> list = {{1, 2, kStep1To2}, {2, 3, kStep2To3}, {3, 4, kStep3To4}};
    return list;
}

}  // namespace pbc
