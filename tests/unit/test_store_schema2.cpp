// Schema 2 of the store (specs/001-nextcloud-files-client/data-model.md): the tables of the files client, the one-way
// upgrade from schema 1 and the constraints that keep bad rows out (constitution Principles I and III).
#include <sys/stat.h>

#include <cstdio>
#include <memory>
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

std::string read_file(const std::string& path) {
    std::string out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

// A database that stops at schema 1, as an older build wrote it.
std::unique_ptr<pbc::Store> open_schema1(const pbtest::TmpDir& d) {
    pbc::StoreOptions o;
    o.target_version = 1;
    auto r = pbc::Store::open(d.file("data"), g_clock, o);
    if (!r.ok()) PB_FAIL_AT("cannot create the schema-1 database: " + r.error.key);
    return std::move(r.value);
}

std::unique_ptr<pbc::Store> open_current(const pbtest::TmpDir& d) {
    auto r = pbc::Store::open(d.file("data"), g_clock);
    if (!r.ok()) PB_FAIL_AT("cannot open the store: " + r.error.key);
    return std::move(r.value);
}

bool table_exists(pbc::Store& s, const std::string& name) {
    return s.debug_query("SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name = '" + name + "'") == "1";
}

const char* kFolder = "INSERT INTO folders(path, last_access_at) VALUES('/', 1)";

}  // namespace

PB_TEST(a_fresh_database_is_the_current_schema_with_every_table) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK_EQ(pbc::Store::kSchemaVersion, 5);
    PB_CHECK_EQ(s->schema_version(), 5);
    for (const char* t : {"settings", "sync_state", "account", "folders", "entries", "local_files", "offline_marks", "favorites", "downloads"})
        PB_CHECK(table_exists(*s, t));
    PB_CHECK(s->check_integrity().ok());
    PB_CHECK(!exists(d.file("data/pbcloud.db.bak")));   // nothing to back up in a fresh database
}

PB_TEST(a_schema_1_database_upgrades_in_one_step_after_a_backup_and_keeps_its_data) {
    pbtest::TmpDir d;
    {
        auto s = open_schema1(d);
        PB_CHECK_EQ(s->schema_version(), 1);
        PB_CHECK(s->set_setting("ui_language", "de").ok());
        pbc::SyncState st;
        st.outcome = "success";
        st.counts_json = "{\"a\":1}";
        PB_CHECK(s->write_sync_state(st).ok());
    }
    auto s = open_current(d);
    PB_CHECK_EQ(s->schema_version(), 5);
    PB_CHECK_EQ(s->setting("ui_language"), std::string("de"));
    PB_CHECK_EQ(s->sync_state().outcome, std::string("success"));
    PB_CHECK_EQ(s->sync_state().counts_json, std::string("{\"a\":1}"));
    PB_CHECK(table_exists(*s, "entries"));
    PB_CHECK(exists(d.file("data/pbcloud.db.bak")));   // kept until the next successful start (research R-07)
    PB_CHECK(s->check_integrity().ok());
}

// A database as the 0.1.0 build wrote it: schema 2, favorites without availability.
std::unique_ptr<pbc::Store> open_schema2(const pbtest::TmpDir& d) {
    pbc::StoreOptions o;
    o.target_version = 2;
    auto r = pbc::Store::open(d.file("data"), g_clock, o);
    if (!r.ok()) PB_FAIL_AT("cannot create the schema-2 database: " + r.error.key);
    return std::move(r.value);
}

PB_TEST(a_schema_2_database_upgrades_to_3_and_its_favorites_read_as_available)
{
    pbtest::TmpDir d;
    {
        auto s = open_schema2(d);
        PB_CHECK_EQ(s->schema_version(), 2);
        PB_CHECK(s->debug_exec("INSERT INTO favorites(remote_path, kind, size, etag, position) VALUES('/Books/a.epub', 'file', 5, '\"e\"', 0)").empty());
        PB_CHECK(s->set_setting("keep", "me").ok());
    }
    auto s = open_current(d);
    PB_CHECK_EQ(s->schema_version(), 5);
    PB_CHECK_EQ(s->setting("keep"), std::string("me"));
    const auto f = s->favorites();
    PB_CHECK_EQ(f.size(), size_t(1));
    PB_CHECK(f[0].availability == pbc::Availability::Ok);
    PB_CHECK_EQ(s->debug_query("SELECT availability || '|' || reason FROM favorites"), std::string("ok|"));
    PB_CHECK(exists(d.file("data/pbcloud.db.bak")));   // kept until the next successful start
    PB_CHECK(s->check_integrity().ok());
}

PB_TEST(a_failing_upgrade_is_rolled_back_and_keeps_the_backup) {
    pbtest::TmpDir d;
    {
        auto s = open_schema1(d);
        PB_CHECK(s->set_setting("keep", "me").ok());
        // a table that collides with the new schema makes the 1 -> 2 step fail
        PB_CHECK(s->debug_exec("CREATE TABLE account(x)").empty());
    }
    auto r = pbc::Store::open(d.file("data"), g_clock);
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("migration_failed"));
    PB_CHECK(exists(d.file("data/pbcloud.db.bak")));

    sqlite3* db = nullptr;
    PB_CHECK(sqlite3_open(d.file("data/pbcloud.db").c_str(), &db) == SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    PB_CHECK(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, nullptr) == SQLITE_OK);
    PB_CHECK(sqlite3_step(st) == SQLITE_ROW);
    PB_CHECK_EQ(sqlite3_column_int(st, 0), 1);   // still schema 1
    sqlite3_finalize(st);
    PB_CHECK(sqlite3_prepare_v2(db, "SELECT count(*) FROM entries", -1, &st, nullptr) != SQLITE_OK);   // no half-built table
    sqlite3_finalize(st);
    sqlite3_close(db);
}

PB_TEST(a_newer_database_is_refused_and_left_byte_identical) {
    pbtest::TmpDir d;
    {
        auto s = open_current(d);
        PB_CHECK(s->debug_exec("PRAGMA user_version = 6").empty());
    }
    const std::string path = d.file("data/pbcloud.db");
    const std::string before = read_file(path);
    auto r = pbc::Store::open(d.file("data"), g_clock);
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("schema_too_new"));
    PB_CHECK(read_file(path) == before);
}

PB_TEST(the_account_row_is_unique_and_its_state_is_constrained) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK(s->debug_exec("INSERT INTO account(id, base_url, login_name, user_id) VALUES(1, 'https://cloud.example.org', 'demo', 'demo')").empty());
    PB_CHECK(!s->debug_exec("INSERT INTO account(id, base_url, login_name, user_id) VALUES(2, 'https://cloud.example.org', 'demo', 'demo')").empty());   // CHECK(id = 1)
    PB_CHECK(!s->debug_exec("UPDATE account SET state = 'weird'").empty());
    for (const char* ok : {"untested", "ok", "needs_signin", "problem"})
        PB_CHECK(s->debug_exec(std::string("UPDATE account SET state = '") + ok + "'").empty());
}

PB_TEST(entries_are_constrained_and_follow_their_folder) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK(s->debug_exec(kFolder).empty());
    PB_CHECK(s->debug_exec("INSERT INTO entries(parent, name, kind) VALUES('/', 'a.epub', 'file')").empty());
    PB_CHECK(!s->debug_exec("INSERT INTO entries(parent, name, kind) VALUES('/', 'b', 'symlink')").empty());                       // kind
    PB_CHECK(!s->debug_exec("INSERT INTO entries(parent, name, kind, availability) VALUES('/', 'c', 'file', 'weird')").empty());   // availability
    for (const char* a : {"ok", "no_permission", "encrypted", "unsupported", "unreadable"})
        PB_CHECK(s->debug_exec(std::string("INSERT INTO entries(parent, name, kind, availability) VALUES('/', 'x_") + a + "', 'file', '" + a + "')").empty());
    PB_CHECK(!s->debug_exec("INSERT INTO entries(parent, name, kind) VALUES('/', 'a.epub', 'file')").empty());                     // primary key
    PB_CHECK(!s->debug_exec("INSERT INTO entries(parent, name, kind) VALUES('/', '', 'file')").empty());                           // empty name
    PB_CHECK(!s->debug_exec("INSERT INTO entries(parent, name, kind) VALUES('/', 'a/b', 'file')").empty());                        // separator in a name
    PB_CHECK(!s->debug_exec("INSERT INTO entries(parent, name, kind) VALUES('/missing', 'a', 'file')").empty());                   // unknown folder (foreign key)
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM entries"), std::string("6"));
    PB_CHECK(s->debug_exec("DELETE FROM folders WHERE path = '/'").empty());
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM entries"), std::string("0"));   // ON DELETE CASCADE
}

PB_TEST(marks_favorites_and_downloads_are_constrained) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK(s->debug_exec("INSERT INTO offline_marks(remote_path, kind, marked_at) VALUES('/Books', 'folder', 1)").empty());
    PB_CHECK(!s->debug_exec("INSERT INTO offline_marks(remote_path, kind, marked_at) VALUES('/x', 'link', 1)").empty());
    PB_CHECK(s->debug_exec("INSERT INTO favorites(remote_path, kind, position) VALUES('/a', 'file', 0)").empty());
    PB_CHECK(!s->debug_exec("INSERT INTO favorites(remote_path, kind, position) VALUES('/b', 'thing', 1)").empty());
    for (const char* st : {"running", "waiting", "paused"})
        PB_CHECK(s->debug_exec(std::string("INSERT INTO downloads(remote_path, target_path, state, started_at, updated_at) VALUES('/a', 'a', '") + st + "', 1, 1)").empty());
    PB_CHECK(!s->debug_exec("INSERT INTO downloads(remote_path, target_path, state, started_at, updated_at) VALUES('/a', 'a', 'done', 1, 1)").empty());
    PB_CHECK_EQ(s->debug_query("SELECT min(id) FROM downloads"), std::string("1"));   // AUTOINCREMENT: ids start at 1 and are not reused
}

PB_TEST(local_copies_have_a_unique_local_path_and_survive_the_cache) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK(s->debug_exec(kFolder).empty());
    PB_CHECK(s->debug_exec("INSERT INTO entries(parent, name, kind) VALUES('/', 'a.epub', 'file')").empty());
    PB_CHECK(s->debug_exec("INSERT INTO local_files(remote_path, local_path, size, mtime, remote_etag, downloaded_at) VALUES('/a.epub', 'a.epub', 10, 20, '\"e1\"', 30)").empty());
    PB_CHECK(!s->debug_exec("INSERT INTO local_files(remote_path, local_path, size, mtime, remote_etag, downloaded_at) VALUES('/b.epub', 'a.epub', 10, 20, '\"e2\"', 30)").empty());   // UNIQUE
    PB_CHECK(s->debug_exec("DELETE FROM folders").empty());   // what cache eviction does
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM local_files"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT server_missing || '|' || seen_etag FROM local_files"), std::string("0|"));
}

PB_TEST(the_name_fold_function_is_available_for_sorting) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK(s->debug_exec(kFolder).empty());
    PB_CHECK(s->debug_exec("INSERT INTO entries(parent, name, kind) VALUES('/', 'banana', 'file'), ('/', 'Apple', 'file'), ('/', 'cherry', 'file')").empty());
    PB_CHECK_EQ(s->debug_query("SELECT group_concat(name, ',') FROM (SELECT name FROM entries ORDER BY pbc_fold(name))"), std::string("Apple,banana,cherry"));
}

PB_TEST_MAIN()
