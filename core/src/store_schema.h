#pragma once
// Schema 1: the settings table and the sync state row. The tables of the files client (schema 2) are added by the migration
// in store_migrations.cpp: a fresh database is created as schema 1 and then migrated, so there is one path to test.

namespace pbc {

inline const char* kSchemaSql = R"SQL(
CREATE TABLE settings(key TEXT PRIMARY KEY, value TEXT NOT NULL);
-- keys: auto_sync, first_run_done, ui_language, last_integrity_check, schema_backup_pending

CREATE TABLE sync_state(
    id INTEGER PRIMARY KEY CHECK(id = 1),
    last_started INTEGER, last_finished INTEGER, last_success INTEGER,
    outcome TEXT NOT NULL DEFAULT 'never' CHECK(outcome IN ('never','running','success','partial','failed','offline')),
    problem_key TEXT NOT NULL DEFAULT '',      -- first problem: auth_failed, cert_expired, server_unreachable, ...
    problem_area TEXT NOT NULL DEFAULT '',
    counts_json TEXT NOT NULL DEFAULT '{}',
    clock_skew_s INTEGER                       -- device clock vs a server time stamp; warn above 600 s (clock_check.h)
);
)SQL";

}  // namespace pbc
