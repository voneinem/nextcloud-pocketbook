// Schema 5 of the store (specs/003-folder-sync/data-model.md): the four sync tables, the three queue columns, the one-way
// upgrade from schema 4 and the constraints that keep bad rows out (constitution Principles I and III).
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

// A database as the 0.2.0 build wrote it: schema 4.
std::unique_ptr<pbc::Store> open_schema4(const pbtest::TmpDir& d) {
    pbc::StoreOptions o;
    o.target_version = 4;
    auto r = pbc::Store::open(d.file("data"), g_clock, o);
    if (!r.ok()) PB_FAIL_AT("cannot create the schema-4 database: " + r.error.key);
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

bool index_exists(pbc::Store& s, const std::string& name) {
    return s.debug_query("SELECT count(*) FROM sqlite_master WHERE type = 'index' AND name = '" + name + "'") == "1";
}

const std::string kPair =
    "INSERT INTO sync_pairs(server, user_id, remote_folder, local_folder, created_at) VALUES('https://cloud.example.org', 'demo', '/Books', '/mnt/ext1/Books', 1)";

std::string file_row(const std::string& rel, const std::string& local_rel, const std::string& extra_cols = "", const std::string& extra_vals = "") {
    return "INSERT INTO sync_files(pair_id, rel, local_rel, kind, origin, synced_at" + extra_cols + ") VALUES(1, '" + rel + "', '" + local_rel +
           "', 'file', 'download', 1" + extra_vals + ")";
}

}  // namespace

PB_TEST(a_fresh_database_is_schema_5_with_the_sync_tables) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK_EQ(pbc::Store::kSchemaVersion, 5);
    PB_CHECK_EQ(s->schema_version(), 5);
    for (const char* t : {"settings", "sync_state", "account", "folders", "entries", "local_files", "offline_marks", "favorites", "downloads", "queue_items",
                          "server_caps", "sync_pairs", "sync_files", "sync_plans", "sync_plan_entries"})
        PB_CHECK(table_exists(*s, t));
    for (const char* c : {"pair_id", "sync_rel", "sync_role"}) PB_CHECK(has_column(*s, "queue_items", c));
    PB_CHECK(s->check_integrity().ok());
    PB_CHECK(!exists(d.file("data/pbcloud.db.bak")));
}

PB_TEST(a_schema_4_database_upgrades_after_a_backup_and_loses_nothing) {
    pbtest::TmpDir d;
    {
        auto s = open_schema4(d);
        PB_CHECK_EQ(s->schema_version(), 4);
        PB_CHECK(!table_exists(*s, "sync_pairs"));
        PB_CHECK(s->debug_exec("INSERT INTO account(id, base_url, login_name, user_id) VALUES(1, 'https://cloud.example.org', 'demo', 'demo')").empty());
        PB_CHECK(s->debug_exec("INSERT INTO folders(path, last_access_at) VALUES('/', 1)").empty());
        PB_CHECK(s->debug_exec("INSERT INTO entries(parent, name, kind) VALUES('/', 'a.epub', 'file')").empty());
        PB_CHECK(s->debug_exec("INSERT INTO local_files(remote_path, local_path, size, mtime, remote_etag, downloaded_at) VALUES('/a.epub', 'a.epub', 5, 1, '\"e\"', 1)").empty());
        PB_CHECK(s->debug_exec("INSERT INTO offline_marks(remote_path, kind, marked_at) VALUES('/Books', 'folder', 1)").empty());
        PB_CHECK(s->debug_exec("INSERT INTO queue_items(kind, entry_kind, server, user_id, created_at, updated_at) VALUES('upload', 'file', 'https://cloud.example.org', 'demo', 1, 1)").empty());
        PB_CHECK(s->debug_exec("INSERT INTO server_caps(id, trash) VALUES(1, 1)").empty());
        PB_CHECK(s->set_setting("ui_language", "de").ok());
    }
    auto s = open_current(d);
    PB_CHECK_EQ(s->schema_version(), 5);
    for (const char* t : {"sync_pairs", "sync_files", "sync_plans", "sync_plan_entries"}) PB_CHECK(table_exists(*s, t));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM account"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM entries"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM local_files"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM offline_marks"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM server_caps"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM queue_items"), std::string("1"));
    // an old queue row reads as an item of the user's own action
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM queue_items WHERE pair_id IS NULL AND sync_rel = '' AND sync_role = ''"), std::string("1"));
    PB_CHECK_EQ(s->setting("ui_language"), std::string("de"));
    PB_CHECK(exists(d.file("data/pbcloud.db.bak")));
    PB_CHECK(s->check_integrity().ok());
}

PB_TEST(a_failing_upgrade_is_rolled_back_and_keeps_the_backup) {
    pbtest::TmpDir d;
    {
        auto s = open_schema4(d);
        PB_CHECK(s->set_setting("keep", "me").ok());
        // a table that collides with the new schema makes the 4 -> 5 step fail
        PB_CHECK(s->debug_exec("CREATE TABLE sync_plans(x)").empty());
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
    PB_CHECK_EQ(sqlite3_column_int(st, 0), 4);   // still schema 4
    sqlite3_finalize(st);
    PB_CHECK(sqlite3_prepare_v2(db, "SELECT count(*) FROM sync_pairs", -1, &st, nullptr) != SQLITE_OK);   // no half-built table
    sqlite3_finalize(st);
    PB_CHECK(sqlite3_prepare_v2(db, "SELECT pair_id FROM queue_items", -1, &st, nullptr) != SQLITE_OK);   // the added columns were rolled back too
    sqlite3_finalize(st);
    sqlite3_close(db);
}

PB_TEST(a_schema_6_database_is_refused_and_left_byte_identical) {
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

PB_TEST(a_schema_4_build_refuses_a_schema_5_database) {
    pbtest::TmpDir d;
    { auto s = open_current(d); }
    pbc::StoreOptions o;
    o.target_version = 4;   // what the 0.2.0 build knows
    auto r = pbc::Store::open(d.file("data"), g_clock, o);
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("schema_too_new"));
}

PB_TEST(a_pair_row_has_the_documented_defaults) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK(s->debug_exec(kPair).empty());
    PB_CHECK_EQ(s->debug_query("SELECT direction || '|' || max_files || '|' || max_bytes || '|' || startup_sync || '|' || auto_sync || '|' || state || '|' || "
                               "problem_key || '|' || last_outcome || '|' || last_result_json FROM sync_pairs"),
                std::string("down|500|2147483648|0|0|idle||never|{}"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM sync_pairs WHERE last_compared_at IS NULL AND last_sync_at IS NULL AND held_files IS NULL AND held_bytes IS NULL"),
                std::string("1"));
}

PB_TEST(the_pair_columns_are_constrained) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    for (const char* dir : {"down", "up", "both"})
        PB_CHECK(s->debug_exec(std::string("INSERT INTO sync_pairs(server, user_id, remote_folder, local_folder, direction, created_at) VALUES('s', 'u', '/', '/x', '") + dir + "', 1)").empty());
    PB_CHECK(!s->debug_exec("INSERT INTO sync_pairs(server, user_id, remote_folder, local_folder, direction, created_at) VALUES('s', 'u', '/', '/x', 'sideways', 1)").empty());
    for (const char* st : {"idle", "comparing", "plan_ready", "running", "interrupted"})
        PB_CHECK(s->debug_exec(std::string("UPDATE sync_pairs SET state = '") + st + "' WHERE id = 1").empty());
    PB_CHECK(!s->debug_exec("UPDATE sync_pairs SET state = 'paused' WHERE id = 1").empty());
    PB_CHECK(!s->debug_exec("INSERT INTO sync_pairs(user_id, remote_folder, local_folder, created_at) VALUES('u', '/', '/x', 1)").empty());   // server is NOT NULL
    PB_CHECK(!s->debug_exec("INSERT INTO sync_pairs(server, user_id, remote_folder, local_folder) VALUES('s', 'u', '/', '/x')").empty());      // created_at is NOT NULL
}

PB_TEST(the_record_columns_are_constrained) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK(s->debug_exec(kPair).empty());
    PB_CHECK(s->debug_exec(file_row("a.epub", "a.epub")).empty());
    PB_CHECK_EQ(s->debug_query("SELECT server_etag || '|' || server_checksum || '|' || local_sha256 || '|' || diverged || '|' || one_sided || '|' || quiet FROM sync_files"),
                std::string("|||0||0"));
    PB_CHECK(!s->debug_exec(file_row("a.epub", "other.epub")).empty());   // (pair_id, rel) is the primary key
    PB_CHECK(!s->debug_exec("INSERT INTO sync_files(pair_id, rel, local_rel, kind, origin, synced_at) VALUES(1, 'b', 'b', 'link', 'download', 1)").empty());
    for (const char* o : {"download", "upload", "identical", "conflict_copy"})
        PB_CHECK(s->debug_exec(std::string("INSERT INTO sync_files(pair_id, rel, local_rel, kind, origin, synced_at) VALUES(1, 'o-") + o + "', 'o-" + o + "', 'file', '" + o + "', 1)").empty());
    PB_CHECK(!s->debug_exec("INSERT INTO sync_files(pair_id, rel, local_rel, kind, origin, synced_at) VALUES(1, 'c', 'c', 'file', 'copied', 1)").empty());
    for (const char* o : {"", "reader", "server"})
        PB_CHECK(s->debug_exec(std::string("UPDATE sync_files SET one_sided = '") + o + "' WHERE rel = 'a.epub'").empty());
    PB_CHECK(!s->debug_exec("UPDATE sync_files SET one_sided = 'both' WHERE rel = 'a.epub'").empty());
}

PB_TEST(two_records_whose_local_names_differ_only_by_case_are_refused) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK(s->debug_exec(kPair).empty());
    PB_CHECK(s->debug_exec(file_row("Book.epub", "Book.epub")).empty());
    PB_CHECK(!s->debug_exec(file_row("book.epub", "book.EPUB")).empty());   // same name on a storage that ignores case
    PB_CHECK(!s->debug_exec(file_row("other", "BOOK.epub")).empty());
    PB_CHECK(s->debug_exec(file_row("other", "other.epub")).empty());
    // the same local name in another pair is fine
    PB_CHECK(s->debug_exec(kPair).empty());
    PB_CHECK(s->debug_exec("INSERT INTO sync_files(pair_id, rel, local_rel, kind, origin, synced_at) VALUES(2, 'Book.epub', 'Book.epub', 'file', 'download', 1)").empty());
}

PB_TEST(the_plan_columns_are_constrained) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK(s->debug_exec(kPair).empty());
    for (const char* t : {"tap", "startup", "auto"})
        PB_CHECK(s->debug_exec(std::string("INSERT INTO sync_plans(pair_id, trigger, state, created_at) VALUES(1, '") + t + "', 'ready', 1)").empty());
    PB_CHECK(!s->debug_exec("INSERT INTO sync_plans(pair_id, trigger, state, created_at) VALUES(1, 'timer', 'ready', 1)").empty());
    for (const char* st : {"ready", "approved", "running", "done", "interrupted", "cancelled", "superseded"})
        PB_CHECK(s->debug_exec(std::string("UPDATE sync_plans SET state = '") + st + "' WHERE id = 1").empty());
    PB_CHECK(!s->debug_exec("UPDATE sync_plans SET state = 'queued' WHERE id = 1").empty());
    PB_CHECK_EQ(s->debug_query("SELECT incomplete || '|' || incomplete_note || '|' || over_limit || '|' || n_download || n_upload || n_skip || n_keep_both || n_prop_server || "
                               "n_prop_reader || n_failed || '|' || b_download || b_upload || b_keep_both || '|' || folders_new || '|' || truncated FROM sync_plans WHERE id = 2"),
                std::string("0||" "|0000000|000|0|0"));
}

PB_TEST(the_plan_entry_columns_are_constrained) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK(s->debug_exec(kPair).empty());
    PB_CHECK(s->debug_exec("INSERT INTO sync_plans(pair_id, trigger, state, created_at) VALUES(1, 'tap', 'ready', 1)").empty());
    const std::string head = "INSERT INTO sync_plan_entries(plan_id, seq, rel, local_rel, kind, action";
    PB_CHECK(s->debug_exec(head + ") VALUES(1, 1, 'a', 'a', 'file', 'download')").empty());
    PB_CHECK_EQ(s->debug_query("SELECT reason || '|' || plan_etag || '|' || conflict_name || '|' || conflict_name_server || '|' || records_below || '|' || allowed || '|' || choice || '|' || removed || '|' || "
                               "unapprovable || '|' || state || '|' || result_key FROM sync_plan_entries"),
                std::string("||||0|||0||planned|"));
    PB_CHECK(!s->debug_exec(head + ") VALUES(1, 1, 'dup', 'dup', 'file', 'download')").empty());   // (plan_id, seq) is the primary key
    int seq = 10;
    for (const char* a : {"download", "upload", "skip", "keep_both", "propose_delete_server", "propose_delete_reader"})
        PB_CHECK(s->debug_exec(head + ") VALUES(1, " + std::to_string(seq++) + ", 'y', 'y', 'file', '" + a + "')").empty());
    PB_CHECK(!s->debug_exec(head + ") VALUES(1, 50, 'z', 'z', 'file', 'delete')").empty());
    PB_CHECK(!s->debug_exec(head + ") VALUES(1, 51, 'z', 'z', 'socket', 'skip')").empty());
    for (const char* st : {"planned", "done", "failed", "skipped"})
        PB_CHECK(s->debug_exec(std::string("UPDATE sync_plan_entries SET state = '") + st + "' WHERE seq = 1").empty());
    PB_CHECK(!s->debug_exec("UPDATE sync_plan_entries SET state = 'running' WHERE seq = 1").empty());
}

PB_TEST(the_queue_columns_for_sync_are_constrained) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    const std::string base = "INSERT INTO queue_items(kind, entry_kind, server, user_id, created_at, updated_at";
    PB_CHECK(s->debug_exec(base + ") VALUES('upload', 'file', 'a', 'b', 1, 1)").empty());
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM queue_items WHERE pair_id IS NULL AND sync_rel = '' AND sync_role = ''"), std::string("1"));
    for (const char* r : {"", "file", "conflict_copy", "delete"})
        PB_CHECK(s->debug_exec(base + ", sync_role) VALUES('upload', 'file', 'a', 'b', 1, 1, '" + r + "')").empty());
    PB_CHECK(!s->debug_exec(base + ", sync_role) VALUES('upload', 'file', 'a', 'b', 1, 1, 'folder')").empty());
}

PB_TEST(the_documented_indexes_exist) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    for (const char* i : {"sync_pairs_by_account", "sync_files_local_rel_ci", "sync_files_by_one_sided", "sync_plan_entries_by_action", "queue_items_by_pair"})
        PB_CHECK(index_exists(*s, i));
}

PB_TEST(sync_rows_survive_the_cache_being_cleared) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK(s->debug_exec(kPair).empty());
    PB_CHECK(s->debug_exec(file_row("a", "a")).empty());
    PB_CHECK(s->debug_exec("INSERT INTO sync_plans(pair_id, trigger, state, created_at) VALUES(1, 'tap', 'ready', 1)").empty());
    PB_CHECK(s->clear_cache().ok());
    PB_CHECK_EQ(s->debug_query("SELECT (SELECT count(*) FROM sync_pairs) || (SELECT count(*) FROM sync_files) || (SELECT count(*) FROM sync_plans)"), std::string("111"));
}

PB_TEST_MAIN()
