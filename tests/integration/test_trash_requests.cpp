// The recorder of every request of the trash and versions feature (specs/006-trash-and-versions, contracts/trash-requests.md "Write conditions"): in each flow the
// requests are only the documented ones, on the documented paths, and every write carries its condition. Phases 6 and 7 add the version and delete flows.
// It proves the app follows the contract against the fake Nextcloud, never a real server (constitution Principle IX).
#include <set>
#include <string>

#include "pbc/trash_rules.h"
#include "test_support.h"
#include "trash_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::TrashEnv;

namespace {

// the invariants of every flow: methods, paths and conditions
void check_log(const TrashEnv& e) {
    static const std::set<std::string> allowed = {"GET", "PROPFIND", "MOVE", "DELETE"};
    for (const auto& l : e.server.log()) {
        PB_CHECK(allowed.count(l.method) == 1);   // never PUT, MKCOL, COPY, PROPPATCH, LOCK, UNLOCK
        const bool ours = l.path.find("/remote.php/dav/trashbin/demo/") == 0 || l.path.find("/remote.php/dav/files/demo/") == 0 ||
                          l.path.find("/remote.php/dav/versions/demo/") == 0 || l.path.find("/ocs/v1.php/cloud/capabilities") == 0;
        PB_CHECK(ours);   // nothing outside the account's trash, versions and files
        if (l.method == "MOVE") {
            PB_CHECK_EQ(l.overwrite, std::string("F"));   // nothing is ever replaced
            PB_CHECK(!l.destination.empty());
        }
        if (l.method == "DELETE") PB_CHECK(l.path.size() > std::string("/remote.php/dav/trashbin/demo/trash/").size());   // never the collection itself
        if (l.method == "PROPFIND") PB_CHECK(l.depth == "0" || l.depth == "1");
    }
}

}  // namespace

PB_TEST(the_requests_of_the_listing_and_of_a_restore_are_the_documented_ones) {
    TrashEnv e;
    e.server.add_folder("/Books");
    e.server.add_folder("/Other");
    const std::string a = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    const std::string b = e.server.add_trash_file("/Books/b.txt", "bb", e.t(200));
    const std::string c = e.server.add_trash_file("/Nowhere/c.txt", "ccc", e.t(300));
    PB_CHECK(e.api.fetch_availability().ok());
    PB_CHECK(e.api.list_trash().ok());
    PB_CHECK(e.api.probe_restore(e.item(a)).ok());
    PB_CHECK(e.api.restore_original(e.item(a)).error.ok());
    pbc::RestoreTarget t;
    t.folder = "/Other";
    t.name = "b.txt";
    PB_CHECK(e.api.restore_to(e.item(b), t).error.ok());
    PB_CHECK(e.api.probe_restore(e.item(c)).ok());   // a missing folder: a plan, no request that creates anything
    PB_CHECK(e.moves() == 2);
    check_log(e);
    PB_CHECK_EQ(e.server.count("PUT") + e.server.count("MKCOL") + e.server.count("COPY"), size_t(0));
}

PB_TEST(a_refused_or_lost_restore_still_stays_within_the_documented_requests) {
    TrashEnv e;
    e.server.add_folder("/Books");
    const std::string a = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    const auto item = e.item(a);
    e.server.answer_next("MOVE", "/trashbin/", 507);
    PB_CHECK(!e.api.restore_original(item).error.ok());
    e.server.lose_next_answer("MOVE", "/trashbin/");
    PB_CHECK(e.api.restore_original(item).outcome_unknown);
    check_log(e);
    PB_CHECK_EQ(e.moves(), size_t(2));   // one per tap, none by the app on its own
}

PB_TEST(the_delete_flows_send_only_deletes_of_single_items_by_their_names) {
    TrashEnv e;
    const std::string a = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    for (int i = 0; i < 5; ++i) e.server.add_trash_file("/F/f" + std::to_string(i) + ".txt", "x", e.t(200 + i));
    const auto first = e.item(a);
    PB_CHECK(e.api.delete_item(first).error.ok());
    const auto l = e.api.list_trash();
    PB_CHECK(l.ok());
    const auto r = e.api.empty_trash(pbc::empty_set_of(l.value).named, nullptr);
    PB_CHECK_EQ(r.removed, 5);
    PB_CHECK_EQ(e.server.count("DELETE"), size_t(6));
    check_log(e);   // never the collection, nothing outside the account's paths
    for (const auto& line : e.server.log())
        if (line.method == "DELETE") PB_CHECK(line.if_match.empty());   // a trash item has no ETag: its unique name is the condition
}

PB_TEST(nothing_is_requested_by_an_environment_that_is_only_created) {
    TrashEnv e;
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST_MAIN()
