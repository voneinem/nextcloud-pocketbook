// Store::open creates schema 1 with its pragmas, constraints and permissions (constitution Principle III and the Security & Data Handling
// constraints), and the settings and the sync state row work on it.
#include <sys/stat.h>

#include <fstream>
#include <iterator>
#include <string>

#include <sqlite3.h>

#include "pbc/store.h"
#include "pbc/types.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

pbc::SystemClock g_clock;

std::unique_ptr<pbc::Store> open_store(const pbtest::TmpDir& d, const std::string& sub = "data") {
    auto r = pbc::Store::open(d.file(sub), g_clock);
    if (!r.ok()) PB_FAIL_AT("Store::open failed: " + r.error.key);
    return std::move(r.value);
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

mode_t mode_of(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) PB_FAIL_AT("cannot stat " + path);
    return st.st_mode & 0777;
}

}  // namespace

PB_TEST(open_creates_every_table_of_the_current_schema) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    for (const char* t : {"settings", "sync_state", "account", "folders", "entries", "local_files", "offline_marks", "favorites", "downloads"}) {
        const std::string n = s->debug_query("SELECT count(*) FROM sqlite_master WHERE name = '" + std::string(t) + "'");
        PB_CHECK_EQ(n, std::string("1"));
    }
}

PB_TEST(open_sets_the_pragmas_and_the_user_version) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK_EQ(s->debug_query("PRAGMA journal_mode"), std::string("truncate"));
    PB_CHECK_EQ(s->debug_query("PRAGMA synchronous"), std::string("2"));  // FULL
    PB_CHECK_EQ(s->debug_query("PRAGMA foreign_keys"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("PRAGMA secure_delete"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("PRAGMA user_version"), std::string("3"));
    PB_CHECK_EQ(s->schema_version(), 3);
    PB_CHECK(s->check_integrity().ok());
}

PB_TEST(the_data_directory_and_database_are_private_where_the_file_system_allows) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK_EQ(mode_of(d.file("data")) & 077, mode_t(0));
    PB_CHECK_EQ(mode_of(d.file("data/pbcloud.db")) & 077, mode_t(0));
}

PB_TEST(data_survives_a_reopen) {
    pbtest::TmpDir d;
    {
        auto s = open_store(d);
        PB_CHECK(s->set_setting("auto_sync", "1").ok());
    }
    auto s = open_store(d);
    PB_CHECK_EQ(s->setting("auto_sync"), std::string("1"));
}

PB_TEST(settings_are_set_replaced_and_empty_when_unknown) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK_EQ(s->setting("ui_language"), std::string());
    PB_CHECK(s->set_setting("ui_language", "de").ok());
    PB_CHECK(s->set_setting("ui_language", "en").ok());
    PB_CHECK_EQ(s->setting("ui_language"), std::string("en"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM settings"), std::string("1"));
}

PB_TEST(the_sync_state_row_holds_the_outcome_and_the_clock_skew) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK_EQ(s->sync_state().outcome, std::string("never"));   // no row yet: the defaults
    PB_CHECK(!s->sync_state().clock_skew_s.has_value());

    PB_CHECK(s->set_clock_skew(-90).ok());
    PB_CHECK_EQ(*s->sync_state().clock_skew_s, static_cast<int64_t>(-90));
    PB_CHECK_EQ(s->sync_state().outcome, std::string("never"));

    pbc::SyncState st;
    st.last_started = 100;
    st.last_finished = 130;
    st.last_success = 130;
    st.outcome = "success";
    st.counts_json = "{\"pulled\":3}";
    PB_CHECK(s->write_sync_state(st).ok());
    const pbc::SyncState back = s->sync_state();
    PB_CHECK_EQ(back.outcome, std::string("success"));
    PB_CHECK_EQ(*back.last_started, static_cast<int64_t>(100));
    PB_CHECK_EQ(back.counts_json, std::string("{\"pulled\":3}"));
    PB_CHECK(!back.clock_skew_s.has_value());   // write_sync_state replaces the whole row

    PB_CHECK(s->set_clock_skew(std::nullopt).ok());
    PB_CHECK(!s->sync_state().clock_skew_s.has_value());
}

PB_TEST(the_sync_state_outcome_is_constrained) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK(!s->debug_exec("INSERT INTO sync_state(id, outcome) VALUES(1, 'weird')").empty());   // CHECK(outcome IN (...))
    PB_CHECK(!s->debug_exec("INSERT INTO sync_state(id, outcome) VALUES(2, 'success')").empty());  // CHECK(id = 1)
    PB_CHECK(s->debug_exec("INSERT INTO sync_state(id, outcome) VALUES(1, 'success')").empty());
}

PB_TEST(a_newer_database_is_refused_and_left_byte_identical) {
    pbtest::TmpDir d;
    ::mkdir(d.file("data").c_str(), 0700);
    const std::string path = d.file("data/pbcloud.db");
    {
        sqlite3* db = nullptr;
        PB_CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
        PB_CHECK(sqlite3_exec(db, "CREATE TABLE future(x); PRAGMA user_version = 4;", nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
    }
    const std::string before = read_file(path);
    auto r = pbc::Store::open(d.file("data"), g_clock);
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("schema_too_new"));
    PB_CHECK(r.value == nullptr);
    PB_CHECK(read_file(path) == before);
}

PB_TEST_MAIN()
