// Schema 6 of the store (specs/006-trash-and-versions/data-model.md): the table of the last trash list, the one-way upgrade from schema 5 and the
// refusal of a newer database by an older build (constitution, Development Workflow).
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

// A database as the 0.5.0 build wrote it: schema 5.
std::unique_ptr<pbc::Store> open_schema5(const pbtest::TmpDir& d) {
    pbc::StoreOptions o;
    o.target_version = 5;
    auto r = pbc::Store::open(d.file("data"), g_clock, o);
    if (!r.ok()) PB_FAIL_AT("cannot create the schema-5 database: " + r.error.key);
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

bool has_column(pbc::Store& s, const std::string& table, const std::string& column) {
    return s.debug_query("SELECT count(*) FROM pragma_table_info('" + table + "') WHERE name = '" + column + "'") == "1";
}

}  // namespace

PB_TEST(a_fresh_database_is_schema_6_with_the_trash_table) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK_EQ(pbc::Store::kSchemaVersion, 6);
    PB_CHECK_EQ(s->schema_version(), 6);
    PB_CHECK(table_exists(*s, "trash_items"));
    for (const char* c : {"position", "trash_name", "name", "original_dir", "original_name", "deleted_at", "kind", "size", "fileid", "flags"})
        PB_CHECK(has_column(*s, "trash_items", c));
    for (const char* t : {"settings", "account", "folders", "entries", "queue_items", "sync_pairs", "sync_plans"}) PB_CHECK(table_exists(*s, t));
    PB_CHECK(s->check_integrity().ok());
    PB_CHECK(!exists(d.file("data/pbcloud.db.bak")));
}

PB_TEST(a_schema_5_database_upgrades_after_a_backup_and_loses_nothing) {
    pbtest::TmpDir d;
    {
        auto s = open_schema5(d);
        PB_CHECK_EQ(s->schema_version(), 5);
        PB_CHECK(!table_exists(*s, "trash_items"));
        PB_CHECK(s->debug_exec("INSERT INTO account(id, base_url, login_name, user_id) VALUES(1, 'https://cloud.example.org', 'demo', 'demo')").empty());
        PB_CHECK(s->debug_exec("INSERT INTO folders(path, last_access_at) VALUES('/', 1)").empty());
        PB_CHECK(s->debug_exec("INSERT INTO entries(parent, name, kind) VALUES('/', 'a.epub', 'file')").empty());
        PB_CHECK(s->debug_exec("INSERT INTO offline_marks(remote_path, kind, marked_at) VALUES('/Books', 'folder', 1)").empty());
        PB_CHECK(s->debug_exec("INSERT INTO queue_items(kind, entry_kind, server, user_id, created_at, updated_at) VALUES('upload', 'file', 'https://cloud.example.org', 'demo', 1, 1)").empty());
        PB_CHECK(s->debug_exec("INSERT INTO sync_pairs(server, user_id, remote_folder, local_folder, created_at) VALUES('https://cloud.example.org', 'demo', '/Books', '/mnt/ext1/Books', 1)").empty());
        PB_CHECK(s->set_setting("ui_language", "de").ok());
    }
    auto s = open_current(d);
    PB_CHECK_EQ(s->schema_version(), 6);
    PB_CHECK(table_exists(*s, "trash_items"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM trash_items"), std::string("0"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM account"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM entries"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM offline_marks"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM queue_items"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM sync_pairs"), std::string("1"));
    PB_CHECK_EQ(s->setting("ui_language"), std::string("de"));
    PB_CHECK(exists(d.file("data/pbcloud.db.bak")));   // kept until the next successful start
    PB_CHECK(s->check_integrity().ok());
}

PB_TEST(a_failing_upgrade_is_rolled_back_and_keeps_the_backup) {
    pbtest::TmpDir d;
    {
        auto s = open_schema5(d);
        PB_CHECK(s->set_setting("keep", "me").ok());
        PB_CHECK(s->debug_exec("CREATE TABLE trash_items(x)").empty());   // collides with the new schema: the 5 -> 6 step fails
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
    PB_CHECK_EQ(sqlite3_column_int(st, 0), 5);   // still schema 5
    sqlite3_finalize(st);
    PB_CHECK(sqlite3_prepare_v2(db, "SELECT position FROM trash_items", -1, &st, nullptr) != SQLITE_OK);   // the table is the stranger's, not ours
    sqlite3_finalize(st);
    sqlite3_close(db);
}

PB_TEST(a_schema_7_database_is_refused_and_left_byte_identical) {
    pbtest::TmpDir d;
    {
        auto s = open_current(d);
        PB_CHECK(s->debug_exec("PRAGMA user_version = 7").empty());
    }
    const std::string path = d.file("data/pbcloud.db");
    const std::string before = read_file(path);
    auto r = pbc::Store::open(d.file("data"), g_clock);
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("schema_too_new"));
    PB_CHECK(read_file(path) == before);
}

PB_TEST(a_schema_5_build_refuses_a_schema_6_database) {
    pbtest::TmpDir d;
    { auto s = open_current(d); }
    pbc::StoreOptions o;
    o.target_version = 5;   // what the 0.5.0 build knows
    auto r = pbc::Store::open(d.file("data"), g_clock, o);
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("schema_too_new"));
}

PB_TEST(the_trash_columns_are_constrained) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    const std::string ok = "INSERT INTO trash_items(position, trash_name, name, kind, flags) VALUES(0, 'a.d1', 'a', 0, 0)";
    PB_CHECK(s->debug_exec(ok).empty());
    PB_CHECK(!s->debug_exec(ok).empty());   // position is the primary key
    PB_CHECK(!s->debug_exec("INSERT INTO trash_items(position, trash_name, name, kind, flags) VALUES(1, 'b.d2', 'b', 2, 0)").empty());   // kind is 0 or 1
    PB_CHECK(!s->debug_exec("INSERT INTO trash_items(position, name, kind, flags) VALUES(2, 'c', 0, 0)").empty());                      // trash_name is NOT NULL
    PB_CHECK(!s->debug_exec("INSERT INTO trash_items(position, trash_name, name, flags) VALUES(3, 'd.d4', 'd', 0)").empty());          // kind is NOT NULL
    PB_CHECK(!s->debug_exec("INSERT INTO trash_items(position, trash_name, name, kind) VALUES(4, 'e.d5', 'e', 0)").empty());           // flags is NOT NULL
}

PB_TEST_MAIN()
