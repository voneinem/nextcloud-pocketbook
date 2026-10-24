// Schema 4 of the store (specs/002-manage-server-files/data-model.md): the queue and the server capabilities, the one-way
// upgrade from schema 3 and the constraints that keep bad rows out (constitution Principles I and III).
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

// A database as the 0.1.0 build wrote it: schema 3.
std::unique_ptr<pbc::Store> open_schema3(const pbtest::TmpDir& d) {
    pbc::StoreOptions o;
    o.target_version = 3;
    auto r = pbc::Store::open(d.file("data"), g_clock, o);
    if (!r.ok()) PB_FAIL_AT("cannot create the schema-3 database: " + r.error.key);
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

bool index_on(pbc::Store& s, const std::string& table, const std::string& column) {
    // an index of the table whose first column is `column`
    return s.debug_query("SELECT count(*) FROM pragma_index_list('" + table + "') il, pragma_index_info(il.name) ii WHERE ii.seqno = 0 AND ii.name = '" +
                         column + "'") != "0";
}

const std::string kItem =
    "INSERT INTO queue_items(kind, entry_kind, server, user_id, created_at, updated_at) VALUES('upload', 'file', 'https://cloud.example.org', 'demo', 1, 1)";

}  // namespace

PB_TEST(a_fresh_database_has_the_queue_tables) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK(pbc::Store::kSchemaVersion >= 4);
    PB_CHECK(s->schema_version() >= 4);
    for (const char* t : {"settings", "sync_state", "account", "folders", "entries", "local_files", "offline_marks", "favorites", "downloads", "queue_items",
                          "server_caps"})
        PB_CHECK(table_exists(*s, t));
    PB_CHECK(s->check_integrity().ok());
    PB_CHECK(!exists(d.file("data/pbcloud.db.bak")));   // nothing to back up in a fresh database
}

PB_TEST(a_schema_3_database_upgrades_after_a_backup_and_keeps_all_of_its_data) {
    pbtest::TmpDir d;
    {
        auto s = open_schema3(d);
        PB_CHECK_EQ(s->schema_version(), 3);
        PB_CHECK(!table_exists(*s, "queue_items"));
        PB_CHECK(s->debug_exec("INSERT INTO account(id, base_url, login_name, user_id) VALUES(1, 'https://cloud.example.org', 'demo', 'demo')").empty());
        PB_CHECK(s->debug_exec("INSERT INTO folders(path, last_access_at) VALUES('/', 1)").empty());
        PB_CHECK(s->debug_exec("INSERT INTO entries(parent, name, kind) VALUES('/', 'a.epub', 'file')").empty());
        PB_CHECK(s->debug_exec("INSERT INTO local_files(remote_path, local_path, size, mtime, remote_etag, downloaded_at) VALUES('/a.epub', 'a.epub', 5, 1, '\"e\"', 1)").empty());
        PB_CHECK(s->debug_exec("INSERT INTO offline_marks(remote_path, kind, marked_at) VALUES('/Books', 'folder', 1)").empty());
        PB_CHECK(s->set_setting("ui_language", "de").ok());
    }
    auto s = open_current(d);
    PB_CHECK(s->schema_version() >= 4);
    PB_CHECK(table_exists(*s, "queue_items"));
    PB_CHECK(table_exists(*s, "server_caps"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM account"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM entries"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM local_files"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM offline_marks"), std::string("1"));
    PB_CHECK_EQ(s->setting("ui_language"), std::string("de"));
    PB_CHECK(exists(d.file("data/pbcloud.db.bak")));   // kept until the next successful start
    PB_CHECK(s->check_integrity().ok());
}

PB_TEST(a_failing_upgrade_is_rolled_back_and_keeps_the_backup) {
    pbtest::TmpDir d;
    {
        auto s = open_schema3(d);
        PB_CHECK(s->set_setting("keep", "me").ok());
        // a table that collides with the new schema makes the 3 -> 4 step fail
        PB_CHECK(s->debug_exec("CREATE TABLE server_caps(x)").empty());
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
    PB_CHECK_EQ(sqlite3_column_int(st, 0), 3);   // still schema 3
    sqlite3_finalize(st);
    PB_CHECK(sqlite3_prepare_v2(db, "SELECT count(*) FROM queue_items", -1, &st, nullptr) != SQLITE_OK);   // no half-built table
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

PB_TEST(a_queue_row_has_the_documented_defaults) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK(s->debug_exec(kItem).empty());
    PB_CHECK_EQ(s->debug_query("SELECT state || '|' || step || '|' || replace || '|' || src_local || '|' || src_etag || '|' || dst_etag || '|' || upload_id || "
                               "'|' || checksum || '|' || result_etag || '|' || reason FROM queue_items"),
                std::string("queued||0|||||||"));
    PB_CHECK_EQ(s->debug_query("SELECT min(id) FROM queue_items"), std::string("1"));   // AUTOINCREMENT: ids start at 1 and are not reused
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM queue_items WHERE group_id IS NULL AND depends_on IS NULL AND src_size IS NULL AND src_mtime IS NULL "
                               "AND src_fileid IS NULL AND chunk_size IS NULL AND result_fileid IS NULL"),
                std::string("1"));
}

PB_TEST(the_queue_columns_are_constrained) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    for (const char* k : {"upload", "mkdir", "rename", "move", "copy", "delete"})
        PB_CHECK(s->debug_exec(std::string("INSERT INTO queue_items(kind, entry_kind, server, user_id, created_at, updated_at) VALUES('") + k +
                               "', 'folder', 'https://cloud.example.org', 'demo', 1, 1)").empty());
    PB_CHECK(!s->debug_exec("INSERT INTO queue_items(kind, entry_kind, server, user_id, created_at, updated_at) VALUES('trash', 'file', 'a', 'b', 1, 1)").empty());
    PB_CHECK(!s->debug_exec("INSERT INTO queue_items(kind, entry_kind, server, user_id, created_at, updated_at) VALUES('upload', 'link', 'a', 'b', 1, 1)").empty());
    PB_CHECK(!s->debug_exec("INSERT INTO queue_items(kind, entry_kind, state, server, user_id, created_at, updated_at) VALUES('upload', 'file', 'weird', 'a', 'b', 1, 1)").empty());
    for (const char* st : {"queued", "sending", "done", "failed", "conflict", "unknown"})
        PB_CHECK(s->debug_exec(std::string("UPDATE queue_items SET state = '") + st + "' WHERE id = 1").empty());
    PB_CHECK(!s->debug_exec("INSERT INTO queue_items(kind, entry_kind, user_id, created_at, updated_at) VALUES('upload', 'file', 'b', 1, 1)").empty());   // server is NOT NULL
    PB_CHECK(!s->debug_exec("INSERT INTO queue_items(kind, entry_kind, server, created_at, updated_at) VALUES('upload', 'file', 'a', 1, 1)").empty());      // user_id is NOT NULL
}

PB_TEST(the_server_caps_row_is_unique) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK(s->debug_exec("INSERT INTO server_caps(id) VALUES(1)").empty());
    PB_CHECK(!s->debug_exec("INSERT INTO server_caps(id) VALUES(2)").empty());   // CHECK(id = 1)
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM server_caps WHERE trash IS NULL AND chunking IS NULL AND max_chunk IS NULL AND checked_at IS NULL"),
                std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT forbidden_chars || forbidden_names || forbidden_basenames || forbidden_extensions FROM server_caps"), std::string(""));
}

PB_TEST(the_documented_indexes_exist) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK(index_on(*s, "queue_items", "state"));
    PB_CHECK(index_on(*s, "queue_items", "dst_parent"));
    PB_CHECK(index_on(*s, "queue_items", "src_remote"));
    PB_CHECK(index_on(*s, "queue_items", "group_id"));
}

PB_TEST(the_new_settings_read_as_unset_until_they_are_written) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    for (const char* k : {"auto_sync", "picker_view", "last_send_at", "last_send_counts"}) PB_CHECK_EQ(s->setting(k), std::string(""));
    PB_CHECK(s->set_setting("auto_sync", "0").ok());
    PB_CHECK(s->set_setting("picker_view", "tile").ok());
    PB_CHECK_EQ(s->setting("auto_sync"), std::string("0"));
    PB_CHECK_EQ(s->setting("picker_view"), std::string("tile"));
}

PB_TEST(queue_rows_survive_the_cache_being_cleared) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK(s->debug_exec(kItem).empty());
    PB_CHECK(s->debug_exec("INSERT INTO server_caps(id, trash) VALUES(1, 1)").empty());
    PB_CHECK(s->clear_cache().ok());
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM queue_items"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM server_caps"), std::string("1"));
}

PB_TEST_MAIN()
