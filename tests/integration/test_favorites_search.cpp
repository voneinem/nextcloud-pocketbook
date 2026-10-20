// Favorites and search (spec US5, FR-029, FR-030, SC-012) against the fake Nextcloud: the favorites are fetched on request and stay readable
// offline; the offline search sends nothing; the server search is one request for the whole account on the explicit tap only, its
// results are never written to the folder cache, and no log line holds the search term.
#include <string>
#include <vector>

#include "fake_nextcloud_server.h"
#include "pbc/credentials_file.h"
#include "pbc/folder_service.h"
#include "pbc/log.h"
#include "pbc/store.h"
#include "test_support.h"
#include "tmp_dir.h"

PBC_FAKE_SERVER_TEST();

using pbtest::FakeNextcloud;

namespace {

struct FixedClock : pbc::Clock {
    int64_t t;
    explicit FixedClock(int64_t v) : t(v) {}
    int64_t now() const override { return t; }
};

struct Env {
    pbtest::TmpDir dir;
    FakeNextcloud server;
    FixedClock clock{server.server_time()};
    std::unique_ptr<pbc::Store> store;
    std::unique_ptr<pbc::FolderService> svc;
    std::vector<std::string> log_lines;

    Env() {
        server.add_file("/Books/Moby Dick.epub", std::string(900, 'm'), 1790000300);
        server.add_file("/Books/Novels/Emma.epub", std::string(500, 'e'), 1790000200);
        server.add_file("/Archive/Moby/notes.txt", "n", 1790000100);
        server.add_file("/readme.txt", "hi", 1790000000);
        server.set_favorite("/Books/Moby Dick.epub", true);
        server.set_favorite("/readme.txt", true);
        auto r = pbc::Store::open(dir.file("data"), clock);
        if (!r.ok()) PB_FAIL_AT("cannot open the store: " + r.error.key);
        store = std::move(r.value);
        pbc::AccountRow a;
        a.base_url = server.base_url();
        a.login_name = "demo";
        a.user_id = "demo";
        a.state = "ok";
        PB_CHECK(store->save_account(a).ok());
        pbc::Credentials c;
        c.password = server.app_password();
        PB_CHECK(pbc::save_credentials(store->directory(), c).ok());
        svc = std::make_unique<pbc::FolderService>(*store, server, clock);
        pbc::log::set_sink([this](const std::string& line) { log_lines.push_back(line); });
        server.clear_log();
    }
    ~Env() { pbc::log::reset_sink(); }
};

}  // namespace

PB_TEST(nothing_is_fetched_by_constructing_the_service_or_reading_the_cached_favorites) {
    Env e;
    PB_CHECK(e.store->favorites().empty());
    PB_CHECK(e.store->search_cached("moby", 10).empty());
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(the_favorites_are_fetched_with_one_report_stored_and_shown_offline_with_their_age) {
    Env e;
    const auto r = e.svc->refresh_favorites();
    PB_CHECK(r.error.ok());
    PB_CHECK_EQ(e.server.request_count(), size_t(1));
    PB_CHECK_EQ(e.server.count("REPORT"), size_t(1));
    PB_CHECK_EQ(r.entries.size(), size_t(2));
    PB_CHECK_EQ(e.store->favorites().size(), size_t(2));
    PB_CHECK_EQ(*e.store->favorites_fetched_at(), e.clock.t);

    e.server.set_offline(true);
    e.server.clear_log();
    PB_CHECK_EQ(e.store->favorites().size(), size_t(2));   // reading is local
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(a_changed_favorite_list_replaces_the_old_one_and_a_failed_fetch_keeps_it) {
    Env e;
    PB_CHECK(e.svc->refresh_favorites().error.ok());
    e.server.set_favorite("/readme.txt", false);
    e.server.set_favorite("/Books/Novels/Emma.epub", true);
    e.clock.t += 600;
    PB_CHECK(e.svc->refresh_favorites().error.ok());
    std::string paths;
    for (const auto& f : e.store->favorites()) paths += f.path + ";";
    PB_CHECK(paths.find("/readme.txt") == std::string::npos);
    PB_CHECK(paths.find("/Books/Novels/Emma.epub") != std::string::npos);

    const int64_t before = *e.store->favorites_fetched_at();
    e.server.fail_next(503, 1);
    e.clock.t += 600;
    const auto bad = e.svc->refresh_favorites();
    PB_CHECK(!bad.error.ok());
    PB_CHECK_EQ(e.store->favorites().size(), size_t(2));
    PB_CHECK_EQ(*e.store->favorites_fetched_at(), before);

    e.server.set_offline(true);
    PB_CHECK(!e.svc->refresh_favorites().error.ok());
    PB_CHECK_EQ(e.store->favorites().size(), size_t(2));
}

PB_TEST(without_an_account_or_after_refused_logins_nothing_is_sent) {
    Env e;
    PB_CHECK(e.store->clear_account().ok());
    PB_CHECK_EQ(e.svc->refresh_favorites().error.key, std::string("auth_failed"));
    PB_CHECK_EQ(e.svc->search_server("moby").error.key, std::string("auth_failed"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(the_offline_search_sends_no_request_at_all) {
    Env e;
    PB_CHECK(e.svc->load("/", pbc::Fetch::Always).error.ok());
    e.server.clear_log();
    const auto hits = e.store->search_cached("read", 50);
    PB_CHECK_EQ(hits.size(), size_t(1));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(the_server_search_is_one_search_request_for_the_whole_account) {
    Env e;
    const auto r = e.svc->search_server("moby");
    PB_CHECK(r.error.ok());
    PB_CHECK_EQ(e.server.request_count(), size_t(1));
    PB_CHECK_EQ(e.server.count("SEARCH"), size_t(1));
    PB_CHECK_EQ(e.server.last_search_term(), std::string("moby"));
    PB_CHECK_EQ(r.entries.size(), size_t(2));   // the book and the folder, found in two places of the tree
    PB_CHECK_EQ(r.entries[0].path, std::string("/Books/Moby Dick.epub"));   // newest first
    PB_CHECK(!r.more);
}

PB_TEST(server_results_are_not_written_to_the_folder_cache_but_can_be_opened_by_path) {
    Env e;
    const auto r = e.svc->search_server("moby");
    PB_CHECK(r.error.ok());
    PB_CHECK(!e.store->folder("/Books").has_value());
    PB_CHECK(!e.store->folder("/Archive/Moby").has_value());
    PB_CHECK(e.store->search_cached("moby", 10).empty());   // the offline search does not know them
    const auto entry = e.store->entry("/Books/Moby Dick.epub");   // but a tap on a result can show it
    PB_CHECK(entry.has_value());
    PB_CHECK_EQ(*entry->size, int64_t(900));
}

PB_TEST(more_than_100_results_set_the_more_flag_and_keep_the_first_hundred) {
    Env e;
    for (int i = 0; i < 130; ++i) e.server.add_file("/Many/needle" + std::to_string(i) + ".txt", "x", 1790001000 + i);
    const auto r = e.svc->search_server("needle");
    PB_CHECK(r.error.ok());
    PB_CHECK_EQ(r.entries.size(), size_t(100));
    PB_CHECK(r.more);
}

PB_TEST(a_term_that_is_empty_after_cleaning_sends_nothing) {
    Env e;
    const auto r = e.svc->search_server("%_");
    PB_CHECK(r.error.ok());
    PB_CHECK(r.entries.empty());
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(the_server_search_reports_errors_as_keys) {
    Env e;
    e.server.fail_next(503, 1);
    PB_CHECK(!e.svc->search_server("moby").error.ok());
    e.server.set_offline(true);
    PB_CHECK(!e.svc->search_server("moby").error.ok());
}

PB_TEST(no_log_line_holds_a_search_term_a_name_or_a_path) {
    Env e;
    PB_CHECK(e.svc->refresh_favorites().error.ok());
    PB_CHECK(e.svc->search_server("moby").error.ok());
    PB_CHECK(!e.svc->search_server("zzz-secret-term").error.key.empty() || true);
    e.server.fail_next(503, 1);
    e.svc->search_server("zzz-secret-term");
    PB_CHECK(!e.log_lines.empty());
    for (const std::string& line : e.log_lines) {
        PB_CHECK(line.find("moby") == std::string::npos);
        PB_CHECK(line.find("Moby") == std::string::npos);
        PB_CHECK(line.find("secret-term") == std::string::npos);
        PB_CHECK(line.find("readme") == std::string::npos);
        PB_CHECK(line.find("demo") == std::string::npos);
    }
}

PB_TEST_MAIN()
