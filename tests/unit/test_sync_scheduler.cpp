// When a pair's Auto Sync may compare (specs/003-folder-sync US5, research R-13): a short quiet period after the user's own change, only for the pairs
// that change touched, only with the switches on and the reader online, never otherwise. Time is passed in: nothing here waits.
#include <functional>
#include <string>
#include <vector>

#include "pbc/sync_scheduler.h"
#include "pbc/sync_types.h"
#include "test_support.h"

namespace {

using pbc::SyncDue;

SyncDue next(pbc::SyncScheduler& s, int64_t now, bool global = true, bool wifi = true, bool busy = false,
             const std::function<bool(int64_t)>& pair_on = [](int64_t) { return true; }) {
    return s.next(now, global, wifi, busy, pair_on);
}

pbc::SyncPair pair(int64_t id, const std::string& remote, const std::string& local) {
    pbc::SyncPair p;
    p.id = id;
    p.remote_folder = remote;
    p.local_folder = local;
    return p;
}

}  // namespace

PB_TEST(nothing_is_armed_at_construction_and_time_alone_arms_nothing) {
    pbc::SyncScheduler s;
    for (int64_t t : {0, 1000, 60000, 3600 * 1000}) {
        const SyncDue d = next(s, t);
        PB_CHECK(d.pairs.empty());
        PB_CHECK_EQ(d.wait_ms, -1);
    }
}

PB_TEST(an_action_arms_only_that_pair_and_the_quiet_period_comes_first) {
    pbc::SyncScheduler s(10000);
    s.note_action(7, 1000);
    SyncDue d = next(s, 1000);
    PB_CHECK(d.pairs.empty());
    PB_CHECK_EQ(d.wait_ms, 10000);
    d = next(s, 6000);
    PB_CHECK(d.pairs.empty());
    PB_CHECK_EQ(d.wait_ms, 5000);
    d = next(s, 11000);   // the quiet period is over
    PB_CHECK((d.pairs == std::vector<int64_t>{7}));
    PB_CHECK_EQ(d.wait_ms, -1);
    // it fired once: nothing more is due until the next action
    d = next(s, 12000);
    PB_CHECK(d.pairs.empty());
    PB_CHECK_EQ(d.wait_ms, -1);
    d = next(s, 999999);
    PB_CHECK(d.pairs.empty());
}

PB_TEST(further_actions_inside_the_quiet_period_restart_it_and_one_comparison_covers_them) {
    pbc::SyncScheduler s(10000);
    s.note_action(1, 0);
    s.note_action(1, 6000);
    s.note_action(1, 9000);
    PB_CHECK(next(s, 12000).pairs.empty());                        // 3 seconds after the last action
    PB_CHECK_EQ(next(s, 12000).wait_ms, 7000);
    const SyncDue d = next(s, 19000);
    PB_CHECK((d.pairs == std::vector<int64_t>{1}));                // one comparison for three actions
    PB_CHECK(next(s, 40000).pairs.empty());
}

PB_TEST(two_pairs_are_armed_and_due_independently_and_the_wait_is_the_nearest) {
    pbc::SyncScheduler s(10000);
    s.note_action(1, 0);
    s.note_action(2, 4000);
    SyncDue d = next(s, 5000);
    PB_CHECK(d.pairs.empty());
    PB_CHECK_EQ(d.wait_ms, 5000);   // pair 1 is due first
    d = next(s, 10000);
    PB_CHECK((d.pairs == std::vector<int64_t>{1}));
    PB_CHECK_EQ(d.wait_ms, 4000);   // pair 2 is still waiting
    d = next(s, 14000);
    PB_CHECK((d.pairs == std::vector<int64_t>{2}));
    // both due at once: both returned, in id order
    s.note_action(5, 20000);
    s.note_action(3, 20000);
    d = next(s, 31000);
    PB_CHECK((d.pairs == std::vector<int64_t>{3, 5}));
}

PB_TEST(nothing_is_due_when_the_global_switch_or_the_pairs_switch_is_off) {
    pbc::SyncScheduler s(1000);
    s.note_action(1, 0);
    PB_CHECK(next(s, 5000, false).pairs.empty());   // the global Auto Sync is off: disarmed
    PB_CHECK(next(s, 6000).pairs.empty());          // and it stays disarmed after the switch comes back
    s.note_action(1, 7000);
    PB_CHECK(next(s, 9000, true, true, false, [](int64_t) { return false; }).pairs.empty());   // the pair's own switch is off
    PB_CHECK(next(s, 10000).pairs.empty());
}

PB_TEST(an_offline_reader_disarms_and_nothing_retries_by_itself) {
    pbc::SyncScheduler s(1000);
    s.note_action(1, 0);
    PB_CHECK(next(s, 5000, true, false).pairs.empty());   // due, but Wi-Fi is off: the pair waits for the user
    PB_CHECK_EQ(next(s, 5000, true, false).wait_ms, -1);
    PB_CHECK(next(s, 9000, true, true).pairs.empty());    // Wi-Fi coming up arms nothing and fires nothing
    PB_CHECK_EQ(next(s, 9000, true, true).wait_ms, -1);
}

PB_TEST(a_busy_run_holds_the_comparison_until_it_ends) {
    pbc::SyncScheduler s(1000);
    s.note_action(1, 0);
    PB_CHECK(next(s, 5000, true, true, true).pairs.empty());    // a run is going: nothing starts, the pair stays armed
    const SyncDue d = next(s, 6000, true, true, false);          // the run ended: the single follow-up
    PB_CHECK((d.pairs == std::vector<int64_t>{1}));
}

PB_TEST(forget_drops_a_pair_that_was_removed) {
    pbc::SyncScheduler s(1000);
    s.note_action(1, 0);
    s.note_action(2, 0);
    s.forget(1);
    const SyncDue d = next(s, 5000);
    PB_CHECK((d.pairs == std::vector<int64_t>{2}));
    s.forget(2);
    s.forget(99);   // an unknown pair is harmless
    PB_CHECK(next(s, 9000).pairs.empty());
}

PB_TEST(a_path_matches_the_pairs_whose_folder_holds_it_component_wise) {
    const std::vector<pbc::SyncPair> pairs = {pair(1, "/Books", "/mnt/ext1/Books"), pair(2, "/Books2", "/mnt/ext1/Books2"), pair(3, "/Docs/Sub", "/mnt/ext1/Docs"),
                                              pair(4, "/", "/mnt/ext1/All")};
    // server paths: byte-exact, prefix by whole components
    PB_CHECK((pbc::pairs_for_server_path(pairs, "/Books") == std::vector<int64_t>{1, 4}));
    PB_CHECK((pbc::pairs_for_server_path(pairs, "/Books/Novels/a.epub") == std::vector<int64_t>{1, 4}));
    PB_CHECK((pbc::pairs_for_server_path(pairs, "/Books2/x") == std::vector<int64_t>{2, 4}));   // /Books does not match /Books2
    PB_CHECK((pbc::pairs_for_server_path(pairs, "/books") == std::vector<int64_t>{4}));         // case-sensitive on the server
    PB_CHECK((pbc::pairs_for_server_path(pairs, "/Docs/Sub/x") == std::vector<int64_t>{3, 4}));
    PB_CHECK((pbc::pairs_for_server_path(pairs, "/Docs") == std::vector<int64_t>{4}));          // above the pair's folder: not inside it
    PB_CHECK((pbc::pairs_for_server_path(pairs, "/Other") == std::vector<int64_t>{4}));
    // reader paths: the storage ignores case
    PB_CHECK((pbc::pairs_for_reader_path(pairs, "/mnt/ext1/Books/a.epub") == std::vector<int64_t>{1}));
    PB_CHECK((pbc::pairs_for_reader_path(pairs, "/mnt/ext1/BOOKS/a.epub") == std::vector<int64_t>{1}));
    PB_CHECK((pbc::pairs_for_reader_path(pairs, "/mnt/ext1/Books2") == std::vector<int64_t>{2}));
    PB_CHECK((pbc::pairs_for_reader_path(pairs, "/mnt/ext1/Books") == std::vector<int64_t>{1}));   // the folder itself
    PB_CHECK((pbc::pairs_for_reader_path(pairs, "/mnt/ext1").empty()));
    PB_CHECK((pbc::pairs_for_reader_path(pairs, "/mnt/ext1/Music/a.mp3").empty()));
    PB_CHECK((pbc::pairs_for_reader_path(pairs, "").empty()));
}

PB_TEST_MAIN()
