// Opening and refreshing folders (spec US2, FR-009..FR-014) against the fake Nextcloud: a folder is fetched when it is opened for the
// first time or when the user refreshes it, never on its own; what was fetched stays readable offline; a failed refresh changes nothing.
#include <string>

#include "fake_nextcloud_server.h"
#include "pbc/credentials_file.h"
#include "pbc/folder_service.h"
#include "pbc/store.h"
#include "test_support.h"
#include "tmp_dir.h"

PBC_FAKE_SERVER_TEST();

using pbc::Fetch;
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

    Env() {
        server.add_folder("/Books");
        server.add_folder("/Books/Novels");
        server.add_file("/Books/story.epub", "0123456789", 1790000000);
        server.add_file("/readme.txt", "hi", 1790000100);
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
        server.clear_log();
    }
    std::string names(const std::string& folder) {
        std::string s;
        for (const auto& e : store->entries_page(folder, pbc::SortKey::Name, true, 0, 100).rows) s += (s.empty() ? "" : ",") + e.name;
        return s;
    }
};

}  // namespace

PB_TEST(an_unvisited_folder_is_fetched_once_with_depth_1_and_the_second_open_sends_nothing) {
    Env e;
    auto r = e.svc->load("/", Fetch::IfMissing);
    PB_CHECK(r.error.ok());
    PB_CHECK(r.fetched);
    PB_CHECK(r.cached);
    PB_CHECK_EQ(e.server.count("PROPFIND"), size_t(1));
    PB_CHECK_EQ(e.names("/"), std::string("Books,readme.txt"));
    PB_CHECK_EQ(*e.store->folder("/")->fetched_at, e.clock.t);

    e.server.clear_log();
    e.clock.t += 3600;
    r = e.svc->load("/", Fetch::IfMissing);
    PB_CHECK(r.error.ok());
    PB_CHECK(!r.fetched);
    PB_CHECK(r.cached);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK_EQ(*e.store->folder("/")->fetched_at, e.clock.t - 3600);   // opening does not renew the time of the listing

    for (const auto& d : e.server.depths()) PB_CHECK_EQ(d, std::string("1"));   // never infinity, never missing
}

PB_TEST(a_visited_folder_is_shown_from_the_cache_without_any_connection) {
    Env e;
    PB_CHECK(e.svc->load("/Books", Fetch::IfMissing).error.ok());
    e.server.set_offline(true);
    e.server.clear_log();
    const auto r = e.svc->load("/Books", Fetch::IfMissing);
    PB_CHECK(r.error.ok());
    PB_CHECK(r.cached);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));   // not even an attempt: a cached folder needs no connection
    PB_CHECK_EQ(e.names("/Books"), std::string("Novels,story.epub"));
    // never fetched and offline: nothing to show
    const auto none = e.svc->load("/Books/Novels", Fetch::IfMissing);
    PB_CHECK(!none.cached);
    PB_CHECK_EQ(none.error.key, std::string("server_unreachable"));
}

PB_TEST(a_refresh_replaces_the_rows_with_the_servers_current_state) {
    Env e;
    PB_CHECK(e.svc->load("/Books", Fetch::IfMissing).error.ok());
    e.server.add_file("/Books/new.epub", "abc", 1790000200);
    e.server.remove("/Books/story.epub");
    e.clock.t += 600;
    const auto r = e.svc->load("/Books", Fetch::Always);
    PB_CHECK(r.error.ok());
    PB_CHECK(r.fetched);
    PB_CHECK_EQ(e.names("/Books"), std::string("Novels,new.epub"));
    PB_CHECK_EQ(*e.store->folder("/Books")->fetched_at, e.clock.t);
}

PB_TEST(a_failed_refresh_keeps_the_old_rows_and_the_old_time) {
    Env e;
    PB_CHECK(e.svc->load("/Books", Fetch::IfMissing).error.ok());
    const int64_t before = *e.store->folder("/Books")->fetched_at;
    e.clock.t += 600;
    e.server.add_file("/Books/new.epub", "abc");

    e.server.fail_next(503);
    auto r = e.svc->load("/Books", Fetch::Always);
    PB_CHECK_EQ(r.error.key, std::string("maintenance"));
    PB_CHECK(r.cached);
    PB_CHECK(!r.fetched);

    e.server.set_offline(true);
    r = e.svc->load("/Books", Fetch::Always);
    PB_CHECK_EQ(r.error.key, std::string("server_unreachable"));
    e.server.set_offline(false);

    e.server.fail_next(500);
    PB_CHECK_EQ(e.svc->load("/Books", Fetch::Always).error.key, std::string("server_error"));

    PB_CHECK_EQ(e.names("/Books"), std::string("Novels,story.epub"));
    PB_CHECK_EQ(*e.store->folder("/Books")->fetched_at, before);
}

PB_TEST(a_folder_that_is_gone_on_the_server_stays_readable_and_is_marked) {
    Env e;
    PB_CHECK(e.svc->load("/Books", Fetch::IfMissing).error.ok());
    e.server.remove("/Books");
    const auto r = e.svc->load("/Books", Fetch::Always);
    PB_CHECK_EQ(r.error.key, std::string("not_found"));
    PB_CHECK(r.cached);
    PB_CHECK(e.store->folder("/Books")->gone);
    PB_CHECK_EQ(e.names("/Books"), std::string("Novels,story.epub"));
    // a never-cached folder that is gone is just an error
    const auto missing = e.svc->load("/Nowhere", Fetch::IfMissing);
    PB_CHECK_EQ(missing.error.key, std::string("not_found"));
    PB_CHECK(!missing.cached);
    PB_CHECK(!e.store->folder("/Nowhere").has_value());
}

PB_TEST(entries_the_user_cannot_use_come_back_marked_and_the_rest_works) {
    Env e;
    e.server.add_folder("/Vault");
    e.server.set_encrypted("/Vault", true);
    e.server.add_folder("/Shared");
    e.server.set_permissions("/Shared", "SRD");
    e.server.add_folder("/Broken");
    e.server.set_quirk("/Broken", "no_etag_no_type");
    e.server.add_folder("/Locked");
    e.server.set_quirk("/Locked", "no_propstat_ok");
    const auto r = e.svc->load("/", Fetch::IfMissing);
    PB_CHECK(r.error.ok());
    PB_CHECK(r.partial);
    int unavailable = 0;
    for (const auto& row : e.store->entries_page("/", pbc::SortKey::Name, true, 0, 100).rows) {
        if (row.availability != pbc::Availability::Ok) ++unavailable;
        if (row.name == "Vault") PB_CHECK(row.availability == pbc::Availability::Encrypted);
        if (row.name == "Shared") PB_CHECK(row.availability == pbc::Availability::NoPermission);
        if (row.name == "Broken") PB_CHECK(row.availability == pbc::Availability::Unsupported);
        if (row.name == "Locked") PB_CHECK(row.availability == pbc::Availability::Unreadable);
        if (row.name == "readme.txt") PB_CHECK(row.availability == pbc::Availability::Ok);
    }
    PB_CHECK_EQ(unavailable, 4);
}

PB_TEST(a_listing_over_the_limit_is_refused_and_nothing_is_stored_for_it) {
    Env e;
    e.server.add_folder("/Big");
    for (int i = 0; i < 10001; ++i) e.server.add_file("/Big/f" + std::to_string(i), "", 1790000000);
    const auto r = e.svc->load("/Big", Fetch::IfMissing);
    PB_CHECK_EQ(r.error.key, std::string("listing_too_large"));
    PB_CHECK(!r.cached);
    PB_CHECK(!e.store->folder("/Big").has_value());
}

PB_TEST(without_credentials_or_after_two_refused_logins_no_request_is_sent) {
    Env e;
    PB_CHECK(pbc::forget_credentials(e.store->directory()).ok());
    auto r = e.svc->load("/", Fetch::IfMissing);
    PB_CHECK_EQ(r.error.key, std::string("auth_failed"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));

    pbc::Credentials c;
    c.password = "wrong";
    PB_CHECK(pbc::save_credentials(e.store->directory(), c).ok());
    PB_CHECK_EQ(e.svc->load("/", Fetch::IfMissing).error.key, std::string("auth_failed"));
    PB_CHECK_EQ(e.svc->load("/", Fetch::IfMissing).error.key, std::string("auth_failed"));
    const size_t sent = e.server.request_count();
    PB_CHECK_EQ(sent, size_t(2));
    PB_CHECK_EQ(e.svc->load("/", Fetch::IfMissing).error.key, std::string("auth_failed"));   // blocked: nothing more is sent
    PB_CHECK_EQ(e.server.request_count(), sent);
    PB_CHECK_EQ(e.store->account()->state, std::string("needs_signin"));
}

PB_TEST(no_account_means_no_request) {
    Env e;
    e.store->clear_account();
    PB_CHECK_EQ(e.svc->load("/", Fetch::Always).error.key, std::string("auth_failed"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST_MAIN()
