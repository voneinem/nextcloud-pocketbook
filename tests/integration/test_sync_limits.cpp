// The limits of a pair (specs/003-folder-sync US9, R-11, SC-009, SC-013), against the fake Nextcloud: a plan over a limit is refused as a whole and transfers
// nothing; taking entries out is how it is made to fit; the reserve is looked at again before every download; lowering a limit deletes nothing.
#include <string>

#include "sync_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::SyncEnv;
using pbtest::pattern;

namespace {

void limits(SyncEnv& e, int64_t files, int64_t bytes) {
    auto p = e.store->sync_pair(e.pair);
    p->max_files = files;
    p->max_bytes = bytes;
    if (!e.store->sync_pair_update(*p).ok()) PB_FAIL_AT("cannot set the limits");
}
void fill(SyncEnv& e, int n) {
    for (int i = 0; i < n; ++i) e.server.add_file("/Books/f" + std::to_string(1000 + i) + ".epub", pattern(200, i));
}
size_t content_gets(const SyncEnv& e) {
    size_t n = 0;
    for (const auto& r : e.server.log())
        if (r.method == "GET" && r.path.find("/dav/files/") != std::string::npos) ++n;
    return n;
}

}  // namespace

PB_TEST(a_plan_over_the_limit_is_refused_as_a_whole_and_taking_entries_out_makes_it_fit) {
    SyncEnv e;
    limits(e, 20, 100LL << 20);
    fill(e, 50);
    const int64_t plan = e.compare_ok();
    PB_CHECK_EQ(e.plan(plan).over_limit, std::string("files"));
    PB_CHECK_EQ(e.plan(plan).state, std::string("ready"));   // stored and shown, but it cannot be approved
    e.server.clear_log();
    const auto refused = e.run(plan);
    PB_CHECK(!refused.ok());
    PB_CHECK_EQ(refused.error.key, std::string("over_limit"));
    PB_CHECK_EQ(content_gets(e), size_t(0));
    PB_CHECK(e.reader_tree().empty());
    // the user takes 30 entries out: the plan fits
    const auto entries = e.entries(plan);
    for (int i = 0; i < 30; ++i) PB_CHECK(e.store->sync_plan_remove_entry(plan, entries[static_cast<size_t>(i)].seq, true).ok());
    PB_CHECK_EQ(e.plan(plan).over_limit, std::string(""));
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.downloaded, 20);
    PB_CHECK_EQ(e.reader_tree().size(), size_t(20));   // never more than the limit
}

PB_TEST(lowering_a_limit_deletes_nothing_and_only_stops_further_downloads) {
    SyncEnv e;
    fill(e, 12);
    e.sync();
    PB_CHECK_EQ(e.reader_tree().size(), size_t(12));
    limits(e, 5, 100LL << 20);
    const int64_t plan = e.compare_ok();
    PB_CHECK_EQ(e.plan(plan).over_limit, std::string(""));   // it adds nothing
    PB_CHECK(pbc::can_approve(e.plan(plan), e.clock.now()).ok);
    e.run_ok(plan);
    PB_CHECK_EQ(e.reader_tree().size(), size_t(12));          // not one file was removed
    // a new server file would add to a folder that is over the limit already: refused
    e.server.add_file("/Books/new.epub", pattern(100, 1));
    PB_CHECK_EQ(e.plan(e.compare_ok()).over_limit, std::string("files"));
}

PB_TEST(a_start_up_comparison_over_the_limit_stores_a_ready_plan_and_transfers_nothing) {
    SyncEnv e;
    limits(e, 3, 100LL << 20);
    fill(e, 10);
    const auto r = e.compare("startup");
    PB_CHECK(r.ok());
    PB_CHECK_EQ(e.plan(r.value).over_limit, std::string("files"));
    PB_CHECK_EQ(e.plan(r.value).trigger, std::string("startup"));
    PB_CHECK_EQ(content_gets(e), size_t(0));
    PB_CHECK(e.reader_tree().empty());
}

PB_TEST(the_reserve_is_looked_at_again_before_every_download_and_a_file_that_no_longer_fits_ends_no_space) {
    SyncEnv e;
    for (int i = 0; i < 4; ++i) e.server.add_file("/Books/f" + std::to_string(i) + ".epub", pattern(1000, i));
    e.storage.total = 1LL << 30;
    e.storage.free = (256LL << 20) + 5000;   // the reserve is 256 MiB: room for five files of 1000 bytes
    const int64_t plan = e.compare_ok();
    PB_CHECK_EQ(e.plan(plan).unapprovable, 0);
    int seen = 0;
    e.on_step = [&](const char* step) {
        if (std::string(step) == "downloaded" && ++seen == 2) e.storage.free = (256LL << 20) + 500;   // the card filled up meanwhile
    };
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.downloaded, 2);
    PB_CHECK_EQ(result.skipped, 2);   // the rest ended no_space, the run went on to the end
    PB_CHECK_EQ(result.stopped_by_key, std::string(""));
}

PB_TEST_MAIN()
