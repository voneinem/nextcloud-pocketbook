// Reviewing a plan before anything happens (specs/003-folder-sync US2, research R-12): when a plan can be approved, taking entries out of it, cancelling
// it, what a result holds and that it outlives a restart.
#include <sys/stat.h>

#include <string>
#include <vector>

#include "pbc/sync_planner.h"
#include "sync_env.h"

PBC_FAKE_SERVER_TEST();

using pbc::PlanAction;
using pbtest::SyncEnv;
using pbtest::find;
using pbtest::pattern;

namespace {

pbc::SyncPlan ready_plan(int64_t created_at = 1000) {
    pbc::SyncPlan p;
    p.id = 1;
    p.pair_id = 1;
    p.state = "ready";
    p.created_at = created_at;
    return p;
}

// a hash of what is on the card (names, sizes, times) to prove that nothing changed
std::string card_state(const SyncEnv& e) {
    std::string out;
    for (const auto& f : e.reader_tree()) {
        struct stat st;
        ::stat((e.reader + "/" + f).c_str(), &st);
        out += f + ":" + std::to_string(st.st_size) + ":" + std::to_string(st.st_mtime) + "\n";
    }
    return out;
}

}  // namespace

PB_TEST(a_ready_recent_plan_that_is_complete_can_be_approved) {
    const auto a = pbc::can_approve(ready_plan(1000), 1000 + 60);
    PB_CHECK(a.ok);
    PB_CHECK_EQ(a.error_key, std::string(""));
}

PB_TEST(a_plan_that_is_not_ready_cannot_be_approved) {
    for (const char* state : {"approved", "running", "done", "interrupted", "cancelled", "superseded"}) {
        pbc::SyncPlan p = ready_plan();
        p.state = state;
        const auto a = pbc::can_approve(p, 1001);
        PB_CHECK(!a.ok);
        PB_CHECK_EQ(a.error_key, std::string("plan_not_approvable"));
    }
}

PB_TEST(a_plan_older_than_an_hour_is_stale) {
    PB_CHECK(pbc::can_approve(ready_plan(1000), 1000 + pbc::kSyncPlanMaxAgeS).ok);          // exactly the limit still counts
    const auto a = pbc::can_approve(ready_plan(1000), 1000 + pbc::kSyncPlanMaxAgeS + 1);
    PB_CHECK(!a.ok);
    PB_CHECK_EQ(a.error_key, std::string("plan_stale"));
}

PB_TEST(a_truncated_plan_an_over_limit_plan_and_a_plan_with_unapprovable_entries_cannot_be_approved) {
    pbc::SyncPlan t = ready_plan();
    t.truncated = true;
    PB_CHECK_EQ(pbc::can_approve(t, 1001).error_key, std::string("plan_too_large"));
    pbc::SyncPlan o = ready_plan();
    o.over_limit = "files";
    PB_CHECK_EQ(pbc::can_approve(o, 1001).error_key, std::string("over_limit"));
    pbc::SyncPlan u = ready_plan();
    u.unapprovable = 2;
    PB_CHECK_EQ(pbc::can_approve(u, 1001).error_key, std::string("plan_not_approvable"));
    u.unapprovable = 0;   // the user took them out
    PB_CHECK(pbc::can_approve(u, 1001).ok);
}

PB_TEST(an_incomplete_plan_with_proposals_cannot_be_approved_but_one_with_transfers_only_can) {
    pbc::SyncPlan p = ready_plan();
    p.incomplete = true;
    p.incomplete_folders = 2;
    p.totals.n_download = 3;
    PB_CHECK(pbc::can_approve(p, 1001).ok);   // the transfers of what was compared are fine
    p.totals.n_prop_reader = 1;               // a proposal built on a comparison that did not see everything
    PB_CHECK_EQ(pbc::can_approve(p, 1001).error_key, std::string("plan_not_approvable"));
}

PB_TEST(removing_every_transfer_leaves_a_plan_that_can_be_approved_as_a_no_op) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(100));
    e.server.add_file("/Books/b.epub", pattern(100));
    const int64_t plan = e.compare_ok();
    PB_CHECK(e.store->sync_plan_remove_entry(plan, 1, true).ok());
    PB_CHECK(e.store->sync_plan_remove_entry(plan, 2, true).ok());
    PB_CHECK_EQ(e.store->sync_plan(plan)->totals.n_download, 0);
    PB_CHECK(pbc::can_approve(*e.store->sync_plan(plan), e.clock.now()).ok);
    e.server.clear_log();
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.downloaded, 0);
    PB_CHECK_EQ(result.skipped, 2);   // both reported as skipped by the user
    PB_CHECK_EQ(e.requests("GET"), size_t(0));
    PB_CHECK(e.reader_tree().empty());
}

PB_TEST(a_removed_entry_can_be_put_back_and_the_counts_follow) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(100));
    e.server.add_file("/Books/b.epub", pattern(300));
    const int64_t plan = e.compare_ok();
    PB_CHECK(e.store->sync_plan_remove_entry(plan, 2, true).ok());
    PB_CHECK_EQ(e.store->sync_plan(plan)->totals.n_download, 1);
    PB_CHECK_EQ(e.store->sync_plan(plan)->totals.b_download, int64_t(100));
    PB_CHECK(e.store->sync_plan_page(plan, "transfers", 0, 10).rows[1].removed);
    PB_CHECK(e.store->sync_plan_remove_entry(plan, 2, false).ok());
    PB_CHECK_EQ(e.store->sync_plan(plan)->totals.n_download, 2);
    PB_CHECK_EQ(e.store->sync_plan(plan)->totals.b_download, int64_t(400));
}

PB_TEST(a_removed_entry_is_skipped_by_the_user_and_is_in_the_next_plan_again) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(100));
    e.server.add_file("/Books/b.epub", pattern(100));
    e.server.add_file("/Books/c.epub", pattern(100));
    const int64_t plan = e.compare_ok();
    PB_CHECK(e.store->sync_plan_remove_entry(plan, 2, true).ok());   // b.epub
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.downloaded, 2);
    PB_CHECK_EQ(result.skipped, 1);
    PB_CHECK(!e.has("b.epub"));
    PB_CHECK_EQ(find(e.entries(plan), "b.epub")->state, std::string("skipped"));
    PB_CHECK_EQ(find(e.entries(plan), "b.epub")->result_key, std::string("by_user"));
    const int64_t next = e.compare_ok();
    PB_CHECK_EQ(e.entries(next).size(), size_t(1));
    PB_CHECK_EQ(e.entries(next)[0].rel, std::string("b.epub"));
    PB_CHECK(e.entries(next)[0].action == PlanAction::Download);
}

PB_TEST(cancelling_a_plan_marks_it_and_changes_no_file_no_record_and_no_queue_item) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(100));
    e.write_reader("mine.epub", "mine");
    e.sync();   // a record to protect
    e.server.add_file("/Books/b.epub", pattern(100));
    const int64_t plan = e.compare_ok();
    const std::string card = card_state(e);
    const auto records = e.store->sync_records(e.pair);
    const std::string queue_rows = e.store->debug_query("SELECT count(*) FROM queue_items");
    e.server.clear_log();
    PB_CHECK(e.store->sync_plan_cancel(plan).ok());
    PB_CHECK_EQ(e.store->sync_plan(plan)->state, std::string("cancelled"));
    PB_CHECK_EQ(e.store->sync_pair(e.pair)->state, std::string("idle"));   // plan_ready goes back to idle
    PB_CHECK_EQ(card_state(e), card);
    PB_CHECK_EQ(e.store->sync_records(e.pair).size(), records.size());
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM queue_items"), queue_rows);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    // a cancelled plan cannot be approved or cancelled again
    PB_CHECK(!pbc::can_approve(*e.store->sync_plan(plan), e.clock.now()).ok);
    const auto run = e.run(plan);
    PB_CHECK(!run.ok());
    PB_CHECK_EQ(run.error.key, std::string("plan_not_approvable"));
    PB_CHECK(!e.store->sync_plan_cancel(plan).ok());
    PB_CHECK(!e.store->sync_plan_cancel(plan + 99).ok());
}

PB_TEST(a_new_comparison_supersedes_a_ready_plan_and_its_entries_stay_readable_until_then) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(100));
    const int64_t first = e.compare_ok();
    PB_CHECK_EQ(e.entries(first).size(), size_t(1));
    const int64_t second = e.compare_ok();
    PB_CHECK_EQ(e.store->sync_plan(first)->state, std::string("superseded"));
    PB_CHECK_EQ(e.store->sync_plan(second)->state, std::string("ready"));
    const auto run = e.run(first);
    PB_CHECK(!run.ok());
    PB_CHECK_EQ(run.error.key, std::string("plan_not_approvable"));
}

PB_TEST(a_stale_plan_is_refused_by_the_run_and_nothing_happens) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(100));
    const int64_t plan = e.compare_ok();
    e.clock.t += pbc::kSyncPlanMaxAgeS + 5;
    e.server.clear_log();
    const auto run = e.run(plan);
    PB_CHECK(!run.ok());
    PB_CHECK_EQ(run.error.key, std::string("plan_stale"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(e.reader_tree().empty());
    PB_CHECK_EQ(e.store->sync_pair(e.pair)->state, std::string("plan_ready"));   // the pair is not stuck
}

PB_TEST(a_plan_with_an_entry_that_does_not_fit_cannot_be_approved_until_the_user_removes_it) {
    SyncEnv e;
    e.storage.total = 10ll << 30;                  // the reserve is 256 MiB
    e.storage.free = (256ll << 20) + 1500;
    e.server.add_file("/Books/a.epub", pattern(1000));
    e.server.add_file("/Books/b.epub", pattern(1000));   // the second one does not fit above the reserve
    const int64_t plan = e.compare_ok();
    const auto es = e.entries(plan);
    PB_CHECK(es[0].unapprovable.empty());
    PB_CHECK_EQ(es[1].unapprovable, std::string("no_space"));
    PB_CHECK_EQ(e.store->sync_plan(plan)->unapprovable, 1);
    auto run = e.run(plan);
    PB_CHECK(!run.ok());
    PB_CHECK_EQ(run.error.key, std::string("plan_not_approvable"));
    PB_CHECK(e.reader_tree().empty());
    PB_CHECK(e.store->sync_plan_remove_entry(plan, 2, true).ok());
    PB_CHECK_EQ(e.store->sync_plan(plan)->unapprovable, 0);
    PB_CHECK(e.run(plan).ok());
    PB_CHECK(e.has("a.epub") && !e.has("b.epub"));
}

PB_TEST(the_result_counts_and_the_time_are_stored_with_the_pair_and_survive_a_restart) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(100));
    e.server.add_file("/Books/b.epub", pattern(100));
    e.server.add_file("/Books/c.epub", pattern(100));
    e.server.set_unavailable("/Books/c.epub", "no_permission");
    const int64_t plan = e.compare_ok();
    PB_CHECK(e.store->sync_plan_remove_entry(plan, 2, true).ok());
    e.clock.t += 7;
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.downloaded, 1);
    PB_CHECK_EQ(result.skipped, 2);   // the one the user removed and the unavailable one
    PB_CHECK_EQ(result.failed, 0);
    PB_CHECK_EQ(result.uploaded + result.kept_both + result.proposals_open, 0);
    PB_CHECK(result.seconds >= 0);
    const std::string json = e.store->sync_pair(e.pair)->last_result_json;
    PB_CHECK(json.find("\"downloaded\":1") != std::string::npos);
    PB_CHECK(json.find("\"skipped\":2") != std::string::npos);
    PB_CHECK(json.find("\"failed\":0") != std::string::npos);
    PB_CHECK(json.find("\"seconds\":") != std::string::npos);
    // a restart: the store is opened again from its file
    const std::string path = e.store->directory();
    e.runner.reset();
    e.dl.reset();
    e.sync_dl.reset();
    e.store.reset();
    auto reopened = pbc::Store::open(path, e.clock);
    PB_CHECK(reopened.ok());
    const auto pair = reopened.value->sync_pair(e.pair);
    PB_CHECK_EQ(pair->last_result_json, json);
    PB_CHECK(pair->last_sync_at.has_value());
    PB_CHECK_EQ(pair->last_outcome, std::string("done"));
    PB_CHECK_EQ(reopened.value->sync_plan(plan)->state, std::string("done"));
}

PB_TEST_MAIN()
