// Repeating a sync (specs/003-folder-sync US8, FR-035, SC-007): the same sync twice makes no transfer and creates no file or record, a plan computed
// after a complete run has nothing in it, repeating after a cancel at each item gives the state of an uninterrupted run, and a file that changes on
// either side during a run is reported and left for the next plan.
#include <map>
#include <string>

#include "sync_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::SyncEnv;
using pbtest::find;
using pbtest::pattern;

namespace {

constexpr int kFiles = 7;

void fill(SyncEnv& e) {
    for (int i = 0; i < kFiles; ++i) e.server.add_file("/Books/" + std::string(i % 2 ? "Odd/" : "") + "f" + std::to_string(i) + ".epub", pattern(900 + static_cast<size_t>(i) * 97, i), 1790000000 + i);
}

std::map<std::string, std::string> records_of(SyncEnv& e) {
    std::map<std::string, std::string> out;
    for (const auto& r : e.store->sync_records(e.pair)) out[r.rel] = r.server_etag + ":" + std::to_string(r.server_size.value_or(-1)) + ":" + std::to_string(r.local_size.value_or(-1));
    return out;
}

}  // namespace

PB_TEST(the_same_sync_twice_makes_no_transfer_and_creates_no_file_or_record) {
    SyncEnv e;
    fill(e);
    const auto first = e.sync();
    PB_CHECK_EQ(first.downloaded, kFiles);
    const auto tree = e.reader_tree();
    const auto records = records_of(e);
    e.server.clear_log();
    const int64_t plan = e.compare_ok();
    PB_CHECK_EQ(e.plan(plan).totals.n_in_sync, kFiles);
    PB_CHECK(e.entries(plan).empty());   // only in-sync names: nothing to list
    PB_CHECK_EQ(e.plan(plan).totals.n_download, 0);
    PB_CHECK_EQ(e.plan(plan).totals.n_skip, 0);
    const auto again = e.run_ok(plan);
    (void)again;
    PB_CHECK_EQ(e.requests("GET"), size_t(0));
    for (const char* m : {"PUT", "MKCOL", "MOVE", "COPY", "DELETE"}) PB_CHECK_EQ(e.requests(m), size_t(0));
    PB_CHECK(e.reader_tree() == tree);
    PB_CHECK(records_of(e) == records);
    PB_CHECK(e.store->journal().empty());
    PB_CHECK_EQ(e.store->sync_pair(e.pair)->state, std::string("idle"));
}

PB_TEST(a_cancel_at_each_item_and_a_repeat_give_the_state_of_an_uninterrupted_run) {
    std::map<std::string, std::string> expected_records;
    std::vector<std::string> expected_tree;
    {
        SyncEnv e;
        fill(e);
        e.sync();
        expected_records = records_of(e);
        expected_tree = e.reader_tree();
    }
    for (int stop_at = 1; stop_at <= kFiles; ++stop_at) {
        SyncEnv e;
        fill(e);
        const int64_t plan = e.compare_ok();
        pbtest::CancelFlag cancel;
        int downloaded = 0;
        e.on_step = [&](const char* step) {
            if (std::string(step) == "downloaded" && ++downloaded == stop_at) cancel.flag = true;
        };
        const auto cut = e.run(plan, cancel);
        PB_CHECK(cut.ok());
        PB_CHECK_EQ(cut.value.downloaded, stop_at);
        PB_CHECK_EQ(e.store->sync_records(e.pair).size(), static_cast<size_t>(stop_at));
        PB_CHECK_EQ(e.store->sync_pair(e.pair)->state, std::string(stop_at == kFiles ? "idle" : "interrupted"));   // a cancel after the last item has nothing left to stop
        e.on_step = nullptr;
        e.server.clear_log();
        const auto finished = e.sync();
        PB_CHECK_EQ(finished.downloaded, kFiles - stop_at);   // exactly what was left
        PB_CHECK_EQ(e.requests("GET"), static_cast<size_t>(kFiles - stop_at));
        PB_CHECK(e.reader_tree() == expected_tree);
        PB_CHECK(records_of(e) == expected_records);
        PB_CHECK(!e.part_file());
        PB_CHECK_EQ(e.store->sync_pair(e.pair)->state, std::string("idle"));
    }
}

PB_TEST(a_file_that_changes_on_the_server_during_a_run_is_reported_and_one_the_user_puts_there_is_never_overwritten) {
    SyncEnv e;
    fill(e);
    const int64_t plan = e.compare_ok();
    int before = 0;
    e.on_step = [&](const char* step) {
        if (std::string(step) != "before_download") return;
        if (++before == 1) e.server.touch_file("/Books/Odd/f1.epub", pattern(1500, 99), 1790009000);   // the server changes it
        if (before == 2) e.write_reader("Odd/f3.epub", "mine, written meanwhile");                  // the user puts a file of that name there
    };
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.downloaded, kFiles - 1);
    PB_CHECK_EQ(result.skipped, 1);
    PB_CHECK_EQ(find(e.entries(plan), "Odd/f1.epub")->result_key, std::string("changed_during_sync"));
    PB_CHECK(e.rd("Odd/f3.epub") == "mine, written meanwhile");   // never overwritten; the server's file came beside it under another name
    PB_CHECK(!e.has("Odd/f1.epub"));
    PB_CHECK(!e.part_file());
    PB_CHECK(e.store->sync_record(e.pair, "Odd/f1.epub") == std::nullopt);
    // the next plan sees both again
    e.on_step = nullptr;
    const int64_t next = e.compare_ok();
    PB_CHECK(find(e.entries(next), "Odd/f1.epub") != nullptr);
    PB_CHECK(e.rd("Odd/f3.epub") == "mine, written meanwhile");
}

PB_TEST_MAIN()
