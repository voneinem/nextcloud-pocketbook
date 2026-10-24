// Emptying the trash (specs/006-trash-and-versions, spec Story 4 scenarios 4 to 7, FR-023 to FR-025, research R-8, contracts/trash-requests.md T8), against the in-process
// fake Nextcloud: exactly the named items, one request at a time, never the collection; a locked item is counted and the loop goes on, a login, a pause or a lost answer stops
// it, Cancel stops it between two requests. It proves the app follows the contract, never a real server (constitution Principle IX).
#include <set>
#include <string>
#include <vector>

#include "pbc/trash_rules.h"
#include "test_support.h"
#include "trash_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::TrashEnv;

namespace {

std::vector<pbc::TrashItem> named_items(TrashEnv& e, int n) {
    for (int i = 0; i < n; ++i) e.server.add_trash_file("/F/f" + std::to_string(i) + ".txt", "x", e.t(10 + i));
    const auto l = e.api.list_trash();
    if (!l.ok()) PB_FAIL_AT("cannot list");
    return pbc::empty_set_of(l.value).named;
}

std::vector<std::string> delete_paths(const TrashEnv& e) {
    std::vector<std::string> out;
    for (const auto& l : e.server.log())
        if (l.method == "DELETE") out.push_back(l.path);
    return out;
}

}  // namespace

PB_TEST(exactly_the_named_items_are_removed_one_request_each_in_the_order_of_the_list) {
    TrashEnv e;
    const auto named = named_items(e, 10);
    e.server.clear_log();
    const auto r = e.api.empty_trash(named, nullptr);
    PB_CHECK(r.error.ok());
    PB_CHECK_EQ(r.removed, 10);
    PB_CHECK_EQ(r.not_removed, 0);
    PB_CHECK_EQ(r.not_tried, 0);
    PB_CHECK_EQ(e.server.trashbin_count(), size_t(0));
    const auto paths = delete_paths(e);
    PB_CHECK_EQ(paths.size(), size_t(10));
    for (size_t i = 0; i < named.size(); ++i) PB_CHECK(paths[i].find("/trash/" + named[i].trash_name) != std::string::npos);   // in the order given
    std::set<std::string> distinct(paths.begin(), paths.end());
    PB_CHECK_EQ(distinct.size(), size_t(10));
    PB_CHECK_EQ(e.server.request_count(), size_t(10));   // nothing but the deletes
    PB_CHECK_EQ(static_cast<int>(r.removed_trash_names.size()), 10);
}

PB_TEST(an_item_that_reaches_the_trash_during_the_run_is_never_removed) {
    TrashEnv e;
    const auto named = named_items(e, 6);
    std::string late;
    e.server.before_next("DELETE", "/trashbin/", [&] { late = e.server.add_trash_file("/F/late.txt", "late", e.t(1)); }, 2);   // after the third delete
    const auto r = e.api.empty_trash(named, nullptr);
    PB_CHECK_EQ(r.removed, 6);
    PB_CHECK(e.server.trashbin_has(late));
    PB_CHECK_EQ(e.server.trashbin_count(), size_t(1));
}

PB_TEST(a_locked_item_is_counted_and_the_loop_goes_on) {
    TrashEnv e;
    const auto named = named_items(e, 5);
    e.server.lock_trash_item(named[2].trash_name);
    const auto r = e.api.empty_trash(named, nullptr);
    PB_CHECK(r.error.ok());   // the run went through: the counts say what happened
    PB_CHECK_EQ(r.removed, 4);
    PB_CHECK_EQ(r.not_removed, 1);
    PB_CHECK_EQ(r.not_tried, 0);
    PB_CHECK(e.server.trashbin_has(named[2].trash_name));
    PB_CHECK_EQ(e.server.trashbin_count(), size_t(1));
    PB_CHECK_EQ(r.removed_trash_names.size(), size_t(4));
}

PB_TEST(an_item_that_is_gone_already_counts_as_removed) {
    TrashEnv e;
    const auto named = named_items(e, 3);
    pbc::Request del;
    del.method = "DELETE";
    del.url = e.server.base_url() + "/remote.php/dav/trashbin/demo/trash/" + named[1].trash_name;
    del.username = "demo";
    del.password = "pw";
    PB_CHECK_EQ(e.server.perform(del).status, 204L);
    const auto r = e.api.empty_trash(named, nullptr);
    PB_CHECK_EQ(r.removed, 3);
}

PB_TEST(a_refused_login_stops_the_loop_and_the_rest_is_not_tried) {
    TrashEnv e;
    const auto named = named_items(e, 10);
    e.server.before_next("DELETE", "/trashbin/", [&] { e.server.answer_next("DELETE", "/trashbin/", 401, 100); }, 3);   // the fourth answers 401, every one after it too
    e.server.clear_log();
    const auto r = e.api.empty_trash(named, nullptr);
    PB_CHECK_EQ(r.error.key, std::string("auth_failed"));
    PB_CHECK_EQ(r.removed, 3);
    PB_CHECK_EQ(r.not_removed, 1);   // the one that was refused
    PB_CHECK_EQ(r.not_tried, 6);
    PB_CHECK_EQ(delete_paths(e).size(), size_t(4));   // no request after the refusal
    PB_CHECK_EQ(e.server.trashbin_count(), size_t(7));
}

PB_TEST(a_pause_the_server_asks_for_stops_the_loop) {
    TrashEnv e;
    const auto named = named_items(e, 8);
    e.server.before_next("DELETE", "/trashbin/", [&] { e.server.answer_next("DELETE", "/trashbin/", 429, 100); }, 1);
    const auto r = e.api.empty_trash(named, nullptr);
    PB_CHECK_EQ(r.error.key, std::string("rate_limited"));
    PB_CHECK_EQ(r.removed, 1);
    PB_CHECK_EQ(r.not_removed, 1);
    PB_CHECK_EQ(r.not_tried, 6);
}

PB_TEST(a_lost_answer_stops_the_loop_marks_one_unknown_and_never_repeats) {
    TrashEnv e;
    const auto named = named_items(e, 8);
    e.server.before_next("DELETE", "/trashbin/", [&] { e.server.lose_next_answer("DELETE", "/trashbin/"); }, 4);   // the fifth delete is executed, its answer is lost
    e.server.clear_log();
    const auto r = e.api.empty_trash(named, nullptr);
    PB_CHECK(r.outcome_unknown);
    PB_CHECK_EQ(r.error.key, std::string("outcome_unknown"));
    PB_CHECK_EQ(r.removed, 4);
    PB_CHECK_EQ(r.not_removed, 0);
    PB_CHECK_EQ(r.not_tried, 3);
    PB_CHECK_EQ(delete_paths(e).size(), size_t(5));   // the fifth is not repeated, the sixth to eighth are not tried
    PB_CHECK_EQ(e.server.trashbin_count(), size_t(3));
}

PB_TEST(a_server_that_cannot_be_reached_stops_the_loop) {
    TrashEnv e;
    const auto named = named_items(e, 6);
    e.server.before_next("DELETE", "/trashbin/", [&] { e.server.set_offline(true); }, 1);   // acts before the second delete, which is still served; the third finds no server
    const auto r = e.api.empty_trash(named, nullptr);
    PB_CHECK_EQ(r.removed, 2);
    PB_CHECK(!r.error.ok());
    PB_CHECK(!r.outcome_unknown);   // nothing was sent: it is simply the connection
    PB_CHECK_EQ(r.not_tried + r.not_removed, 4);
}

PB_TEST(cancel_stops_between_two_requests_and_reports_what_was_done) {
    TrashEnv e;
    const auto named = named_items(e, 10);
    int calls = 0;
    const auto r = e.api.empty_trash(named, [&](const pbc::EmptyProgress& p) {
        ++calls;
        PB_CHECK_EQ(p.total, 10);
        return p.done < 2;   // after the second request: stop
    });
    PB_CHECK_EQ(r.removed, 2);
    PB_CHECK_EQ(r.not_tried, 8);
    PB_CHECK_EQ(r.error.key, std::string("cancelled"));
    PB_CHECK_EQ(e.server.trashbin_count(), size_t(8));
    PB_CHECK_EQ(calls, 2);
}

PB_TEST(the_progress_callback_sees_every_item_in_order) {
    TrashEnv e;
    const auto named = named_items(e, 5);
    std::vector<int> done, removed;
    e.api.empty_trash(named, [&](const pbc::EmptyProgress& p) {
        done.push_back(p.done);
        removed.push_back(p.removed);
        return true;
    });
    PB_CHECK_EQ(done.size(), size_t(5));
    for (int i = 0; i < 5; ++i) PB_CHECK_EQ(done[static_cast<size_t>(i)], i + 1);
    PB_CHECK_EQ(removed.back(), 5);
}

PB_TEST(a_trash_of_1000_items_is_emptied_with_1000_single_requests_and_never_the_collection) {
    TrashEnv e;
    const auto named = named_items(e, 1000);
    e.server.clear_log();
    const auto r = e.api.empty_trash(named, nullptr);
    PB_CHECK_EQ(r.removed, 1000);
    PB_CHECK_EQ(e.server.count("DELETE"), size_t(1000));
    for (const std::string& p : delete_paths(e)) PB_CHECK(p.size() > std::string("/remote.php/dav/trashbin/demo/trash/").size());
    PB_CHECK_EQ(e.server.trashbin_count(), size_t(0));
}

PB_TEST(an_empty_set_sends_nothing) {
    TrashEnv e;
    const auto r = e.api.empty_trash({}, nullptr);
    PB_CHECK(r.error.ok());
    PB_CHECK_EQ(r.removed + r.not_removed + r.not_tried, 0);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST_MAIN()
