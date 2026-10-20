// The built-in schema upgrades. Every step is a script that Store::open runs inside ONE transaction after a copy of the
// database file, followed by an integrity check. Steps only go forward; an older build refuses a database of a newer
// version (schema_too_new). `PRAGMA user_version` is the only version source.
//
// Step 1 -> 2 adds the tables of the files client (specs/001-nextcloud-files-client/data-model.md; step 2 -> 3 below adds two columns to favorites). A fresh database is
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

// Step 2 -> 3: a favorite the user may not read must stay marked as such (FR-013); rows written by 0.1.0 read as available.
const char* kStep2To3 = R"SQL(
ALTER TABLE favorites ADD COLUMN availability TEXT NOT NULL DEFAULT 'ok' CHECK(availability IN ('ok','no_permission','encrypted','unsupported','unreadable'));
ALTER TABLE favorites ADD COLUMN reason TEXT NOT NULL DEFAULT '';
)SQL";

}  // namespace

const std::vector<Migration>& builtin_migrations() {
    static const std::vector<Migration> list = {{1, 2, kStep1To2}, {2, 3, kStep2To3}};
    return list;
}

}  // namespace pbc
