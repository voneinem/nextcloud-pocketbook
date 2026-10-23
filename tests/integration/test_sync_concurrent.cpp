// One run at a time per pair (specs/003-folder-sync US8, FR-034): a second trigger for a pair that is being compared or synced is refused with sync_busy
// and starts no plan and no transfer; another pair is not held by it; a lock left by a death between taking and releasing it becomes "interrupted" at
// the next start, and the pair can be synced again.
#include <string>

#include "sync_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::SyncEnv;
using pbtest::pattern;

PB_TEST(a_second_trigger_during_a_comparison_or_a_run_is_refused_and_starts_nothing) {
    SyncEnv e;
    for (int i = 0; i < 4; ++i) e.server.add_file("/Books/f" + std::to_string(i) + ".epub", pattern(400, i));
    const int64_t plan = e.compare_ok();
    // during the run a tap, an Auto Sync and a start-up comparison all find the pair locked
    int tried = 0;
    e.on_step = [&](const char* step) {
        if (std::string(step) != "before_download" || tried++ != 0) return;
        for (const char* trigger : {"tap", "auto", "startup"}) {
            const auto busy = e.compare(trigger);
            PB_CHECK(!busy.ok());
            PB_CHECK_EQ(busy.error.key, std::string("sync_busy"));
        }
        const auto twice = e.run(plan);
        PB_CHECK(!twice.ok());
        PB_CHECK_EQ(twice.error.key, std::string("sync_busy"));
    };
    const size_t props_before = e.requests("PROPFIND");
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.downloaded, 4);
    PB_CHECK_EQ(e.requests("PROPFIND"), props_before);   // no second comparison ran
    PB_CHECK_EQ(e.requests("GET"), size_t(4));           // each file once
    PB_CHECK_EQ(e.store->sync_plan(plan)->state, std::string("done"));
    PB_CHECK_EQ(e.store->sync_pair(e.pair)->state, std::string("idle"));
}

PB_TEST(a_pair_that_is_held_does_not_hold_another_pair) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(300, 1));
    e.server.add_folder("/Other");
    e.server.add_file("/Other/b.epub", pattern(300, 2));
    e.mkdirs(e.reader + "2");
    pbc::SyncPair p = pbc::default_pair(e.server.base_url(), "demo");
    p.remote_folder = "/Other";
    p.local_folder = e.reader + "2";
    const int64_t other = e.store->sync_pair_add(p).value;
    PB_CHECK(e.store->sync_pair_lock(e.pair, "comparing").ok());
    pbtest::NoCancel cancel;
    const auto held = e.runner->compare(e.pair, "tap", cancel);
    PB_CHECK_EQ(held.error.key, std::string("sync_busy"));
    const auto free_pair = e.runner->compare(other, "tap", cancel);
    PB_CHECK(free_pair.ok());
    PB_CHECK(e.store->sync_pair_unlock(e.pair, "idle", "").ok());
}

PB_TEST(a_lock_left_by_a_death_becomes_interrupted_at_start_and_the_pair_syncs_again) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(300, 1));
    PB_CHECK(e.store->sync_pair_lock(e.pair, "running").ok());   // the process died here: no unlock
    pbtest::NoCancel cancel;
    PB_CHECK_EQ(e.runner->compare(e.pair, "tap", cancel).error.key, std::string("sync_busy"));
    e.requests("GET");
    const auto report = e.store->sync_recover();
    PB_CHECK_EQ(report.pairs_interrupted, 1);
    PB_CHECK_EQ(e.store->sync_pair(e.pair)->state, std::string("interrupted"));
    const auto result = e.sync();
    PB_CHECK_EQ(result.downloaded, 1);
    PB_CHECK_EQ(e.store->sync_pair(e.pair)->state, std::string("idle"));
}

PB_TEST_MAIN()
