// Listing the server's trash (specs/006-trash-and-versions, spec Story 1, FR-001 to FR-005, contracts/trash-requests.md T2), against the in-process fake
// Nextcloud: one row per item with name, original location, deletion date and size, newest first, bounded, stored for the offline view. It proves the app
// follows the contract, never a real server (constitution Principle IX).
#include <string>

#include "fake_nextcloud_server.h"
#include "fixtures.h"
#include "pbc/dav_common.h"
#include "pbc/store.h"
#include "pbc/trash_api.h"
#include "test_support.h"
#include "tmp_dir.h"

PBC_FAKE_SERVER_TEST();

using pbtest::FakeNextcloud;

namespace {

struct Env {
    FakeNextcloud server;
    pbc::SystemClock clock;
    pbc::DavSession session;
    pbc::TrashApi api;
    Env() : session(server, server.base_url(), "demo", "pw"), api(session, "demo", clock) {
        server.set_account("demo", "demo", "pw");
        server.set_server_time(1792497600);
    }
    static int64_t t(int64_t ago) { return 1792497600 - ago; }
};

}  // namespace

PB_TEST(every_item_is_listed_with_name_location_date_and_size_newest_first) {
    Env e;
    e.server.add_trash_file("/Books/story.epub", "1234567890", Env::t(100));
    const std::string folder = e.server.add_trash_folder("/Documents/Old Notes", Env::t(200), {{"/a.txt", "a"}});
    e.server.add_trash_file("/Documents/report.pdf", "xx", Env::t(300));
    e.server.add_trash_file("/Documents/report.pdf", "yyyy", Env::t(400));   // the same name deleted earlier: its own item
    const auto r = e.api.list_trash();
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.items.size(), size_t(4));
    PB_CHECK_EQ(r.value.total_seen, 4);
    PB_CHECK(!r.value.truncated && !r.value.from_store);
    PB_CHECK(r.value.items[0].deleted_at == std::optional<int64_t>(Env::t(100)));
    PB_CHECK_EQ(r.value.items[0].name, std::string("story.epub"));
    PB_CHECK_EQ(r.value.items[0].original_dir, std::string("/Books"));
    PB_CHECK(r.value.items[0].size == std::optional<int64_t>(10));
    PB_CHECK_EQ(r.value.items[1].trash_name, folder);
    PB_CHECK(r.value.items[1].kind == pbc::Kind::Folder);
    PB_CHECK_EQ(r.value.items[2].name, r.value.items[3].name);
    PB_CHECK(r.value.items[2].trash_name != r.value.items[3].trash_name);
    PB_CHECK(r.value.items[2].size == std::optional<int64_t>(2) && r.value.items[3].size == std::optional<int64_t>(4));
    // exactly one request: the listing of the trash, Depth 1 (the capabilities are the bridge's first request, not this one's)
    PB_CHECK_EQ(e.server.request_count(), size_t(1));
    PB_CHECK_EQ(e.server.log()[0].method, std::string("PROPFIND"));
    PB_CHECK_EQ(e.server.log()[0].depth, std::string("1"));
    PB_CHECK(e.server.log()[0].path.find("/trashbin/demo/trash/") != std::string::npos);
}

PB_TEST(an_empty_trash_gives_an_empty_list_without_an_error) {
    Env e;
    const auto r = e.api.list_trash();
    PB_CHECK(r.ok());
    PB_CHECK(r.value.items.empty());
    PB_CHECK_EQ(r.value.total_seen, 0);
}

PB_TEST(a_file_deleted_through_the_files_endpoint_shows_where_it_came_from_and_when) {
    Env e;
    e.server.add_file("/Docs/a.txt", "hello");
    pbc::Request del;
    del.method = "DELETE";
    del.url = e.server.base_url() + "/remote.php/dav/files/demo/Docs/a.txt";
    del.username = "demo";
    del.password = "pw";
    PB_CHECK_EQ(e.server.perform(del).status, 204L);
    const auto r = e.api.list_trash();
    PB_CHECK(r.ok() && r.value.items.size() == 1);
    PB_CHECK_EQ(r.value.items[0].original_dir, std::string("/Docs"));
    PB_CHECK_EQ(r.value.items[0].original_name, std::string("a.txt"));
    PB_CHECK(r.value.items[0].deleted_at == std::optional<int64_t>(1792497600));   // the server's time of the deletion
}

PB_TEST(a_trash_of_1000_items_is_listed_and_one_of_1200_is_cut_and_says_so) {
    Env e;
    for (int i = 0; i < 1000; ++i) e.server.add_trash_file("/Many/f" + std::to_string(i) + ".txt", "x", Env::t(10 + i));
    auto r = e.api.list_trash();
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.items.size(), size_t(1000));
    PB_CHECK(!r.value.truncated);
    for (int i = 1000; i < 1200; ++i) e.server.add_trash_file("/Many/f" + std::to_string(i) + ".txt", "x", Env::t(10 + i));
    r = e.api.list_trash();
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.items.size(), pbc::kMaxTrashRows);
    PB_CHECK(r.value.truncated);
    PB_CHECK_EQ(r.value.total_seen, 1200);
    PB_CHECK(r.value.items.front().deleted_at == std::optional<int64_t>(Env::t(10)));   // the newest are kept
}

PB_TEST(an_answer_above_the_limit_is_refused_with_listing_too_large) {
    Env e;
    const std::string long_name(240, 'n');
    for (int i = 0; i < 4200; ++i) e.server.add_trash_file("/Big/" + long_name + std::to_string(i) + ".txt", "x", Env::t(10 + i));
    const auto r = e.api.list_trash();
    PB_CHECK_EQ(r.error.key, std::string("listing_too_large"));
    PB_CHECK(r.value.items.empty());
}

PB_TEST(a_hostile_item_is_listed_as_unreadable_and_the_others_are_intact) {
    Env e;
    e.server.add_trash_file("/Books/ok.txt", "x", Env::t(10));
    e.server.set_interceptor([](const pbc::Request& r) -> std::optional<pbc::Response> {
        if (r.method != "PROPFIND") return std::nullopt;
        pbc::Response resp;
        resp.status = 207;
        resp.body = pbtest::fixture("nextcloud/trash/trash_list_hostile.unverified.xml");
        return resp;
    });
    const auto r = e.api.list_trash();
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.items.size(), size_t(15));
    int unreadable = 0;
    for (const auto& i : r.value.items) unreadable += i.unreadable ? 1 : 0;
    PB_CHECK_EQ(unreadable, 4);
}

PB_TEST(failures_give_their_key_and_a_code_and_a_missing_endpoint_is_never_off) {
    Env e;
    e.server.add_trash_file("/a.txt", "x", Env::t(10));
    e.server.answer_next("PROPFIND", "/trashbin/", 404);   // the capability said On, the endpoint says nothing: not "off"
    auto r = e.api.list_trash();
    PB_CHECK_EQ(r.error.key, std::string("protocol_error"));
    PB_CHECK_EQ(r.value.code, std::string("T2-404"));
    e.server.answer_next("PROPFIND", "/trashbin/", 500);
    r = e.api.list_trash();
    PB_CHECK_EQ(r.error.key, std::string("server_error"));
    PB_CHECK_EQ(r.value.code, std::string("T2-500"));
    e.server.answer_next("PROPFIND", "/trashbin/", 401);
    r = e.api.list_trash();
    PB_CHECK_EQ(r.error.key, std::string("auth_failed"));
    e.server.answer_next("PROPFIND", "/trashbin/", 403);
    PB_CHECK_EQ(e.api.list_trash().error.key, std::string("forbidden"));
    e.server.set_offline(true);
    r = e.api.list_trash();
    PB_CHECK(r.error.cls == pbc::ErrorClass::Network);
}

PB_TEST(the_stored_list_round_trips_and_a_refresh_replaces_it) {
    pbtest::TmpDir dir;
    Env e;
    pbc::SystemClock clock;
    auto opened = pbc::Store::open(dir.file("data"), clock);
    PB_CHECK(opened.ok());
    auto& store = *opened.value;
    e.server.add_trash_file("/Books/a.txt", "x", Env::t(10));
    e.server.add_trash_file("/Books/b.txt", "xy", Env::t(20));
    auto r = e.api.list_trash();
    PB_CHECK(r.ok());
    PB_CHECK(store.replace_trash(r.value, 1792497600, 1).ok());
    auto stored = store.load_trash(1);
    PB_CHECK(stored.ok() && stored.value.from_store);
    PB_CHECK_EQ(stored.value.items.size(), size_t(2));
    PB_CHECK_EQ(stored.value.items[0].trash_name, r.value.items[0].trash_name);
    PB_CHECK(stored.value.fetched_at == std::optional<int64_t>(1792497600));
    e.server.add_trash_file("/Books/c.txt", "xyz", Env::t(5));
    r = e.api.list_trash();
    PB_CHECK(store.replace_trash(r.value, 1792497700, 1).ok());
    PB_CHECK_EQ(store.load_trash(1).value.items.size(), size_t(3));
}

PB_TEST_MAIN()
