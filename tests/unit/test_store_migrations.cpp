// One-way schema migrations (data-model.md, "Migration policy"): backup first, one transaction, integrity check,
// backup removed on success and kept on failure (T021).
#include <sys/stat.h>

#include <string>

#include <sqlite3.h>

#include "pbc/store.h"
#include "pbc/types.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

pbc::SystemClock g_clock;

bool exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

// A test-only step 6 -> 7 on top of the built-in schema 6: the runner is the same for built-in and extra steps.
pbc::StoreOptions v7(const std::string& sql) {
    pbc::StoreOptions o;
    o.target_version = 7;
    o.extra_migrations.push_back({6, 7, sql});
    return o;
}

}  // namespace

PB_TEST(a_registered_migration_runs_after_a_backup_and_the_backup_goes_on_success) {
    pbtest::TmpDir d;
    {
        auto r = pbc::Store::open(d.file("data"), g_clock);
        PB_CHECK(r.ok());
        PB_CHECK(r.value->debug_exec("INSERT INTO settings(key, value) VALUES('keep', 'me')").empty());
    }
    auto r = pbc::Store::open(d.file("data"), g_clock, v7("ALTER TABLE settings ADD COLUMN note TEXT NOT NULL DEFAULT 'n'"));
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value->schema_version(), 7);
    PB_CHECK_EQ(r.value->debug_query("PRAGMA user_version"), std::string("7"));
    PB_CHECK_EQ(r.value->debug_query("SELECT value || note FROM settings WHERE key = 'keep'"), std::string("men"));   // data kept, column added
    PB_CHECK(exists(d.file("data/pbcloud.db.bak")));   // kept until the next successful start (research R-07)
    PB_CHECK(r.value->check_integrity().ok());
}

PB_TEST(a_failing_migration_changes_nothing_and_keeps_the_backup) {
    pbtest::TmpDir d;
    {
        auto r = pbc::Store::open(d.file("data"), g_clock);
        PB_CHECK(r.ok());
        PB_CHECK(r.value->debug_exec("INSERT INTO settings(key, value) VALUES('keep', 'me')").empty());
    }
    auto bad = pbc::Store::open(d.file("data"), g_clock, v7("ALTER TABLE settings ADD COLUMN ok TEXT; ALTER TABLE no_such_table ADD COLUMN x TEXT"));
    PB_CHECK(!bad.ok());
    PB_CHECK_EQ(bad.error.key, std::string("migration_failed"));
    PB_CHECK(exists(d.file("data/pbcloud.db.bak")));

    // the database is still at version 6 (the build's own schema) and the first statement of the failed script was rolled back
    sqlite3* db = nullptr;
    PB_CHECK(sqlite3_open(d.file("data/pbcloud.db").c_str(), &db) == SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    PB_CHECK(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, nullptr) == SQLITE_OK);
    PB_CHECK(sqlite3_step(st) == SQLITE_ROW);
    PB_CHECK_EQ(sqlite3_column_int(st, 0), 6);
    sqlite3_finalize(st);
    PB_CHECK(sqlite3_prepare_v2(db, "SELECT ok FROM settings", -1, &st, nullptr) != SQLITE_OK);   // column was rolled back
    sqlite3_finalize(st);
    sqlite3_close(db);

    // and the app can still open it with the current schema
    auto again = pbc::Store::open(d.file("data"), g_clock);
    PB_CHECK(again.ok());
    PB_CHECK_EQ(again.value->debug_query("SELECT value FROM settings WHERE key = 'keep'"), std::string("me"));
}

PB_TEST(a_fresh_database_goes_straight_to_the_target_version_without_a_backup) {
    pbtest::TmpDir d;
    auto r = pbc::Store::open(d.file("data"), g_clock, v7("ALTER TABLE settings ADD COLUMN note TEXT NOT NULL DEFAULT 'n'"));
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value->schema_version(), 7);
    PB_CHECK(!exists(d.file("data/pbcloud.db.bak")));
}

PB_TEST(an_older_build_refuses_a_database_its_migrations_do_not_reach) {
    pbtest::TmpDir d;
    {
        auto r = pbc::Store::open(d.file("data"), g_clock, v7("ALTER TABLE settings ADD COLUMN note TEXT NOT NULL DEFAULT 'n'"));
        PB_CHECK(r.ok());
    }
    auto older = pbc::Store::open(d.file("data"), g_clock);   // target 6 < stored 7
    PB_CHECK(!older.ok());
    PB_CHECK_EQ(older.error.key, std::string("schema_too_new"));
}

PB_TEST_MAIN()
