// What a sync must not conclude from an incomplete picture (specs/003-folder-sync FR-029, invariant 8), against the fake Nextcloud: a listing that is partial,
// cut off or failed, a server folder that is gone, a reader folder that is not there (the card was taken out) and an account that changed give no proposal and
// no dropped record, show the reason, and leave every file alone.
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <string>

#include "pbc/queue_actions.h"
#include "sync_env.h"

PBC_FAKE_SERVER_TEST();

using pbc::Choice;
using pbc::PlanAction;
using pbtest::SyncEnv;
using pbtest::find;
using pbtest::pattern;

namespace {

struct Synced : SyncEnv {
    Synced() {
        direction(pbc::Direction::Both);
        pbc::ServerCaps caps;
        caps.trash = true;
        if (!store->save_server_caps(caps).ok()) PB_FAIL_AT("cannot save the capabilities");
        for (const char* n : {"a.epub", "Sub/b.epub", "Sub/Deep/c.epub", "Other/d.epub"}) server.add_file(std::string("/Books/") + n, pattern(200, n[0]));
        sync();
        server.clear_log();
    }
    size_t proposals(const pbc::SyncPlan& p) const { return static_cast<size_t>(p.totals.n_prop_server + p.totals.n_prop_reader); }
    std::map<std::string, std::string> reader_files() const {
        std::map<std::string, std::string> out;
        for (const std::string& f : reader_tree()) out[f] = rd(f);
        return out;
    }
};

}  // namespace

PB_TEST(a_listing_that_is_partial_cut_off_or_failed_gives_no_proposal_and_drops_no_record) {
    for (const char* mode : {"partial", "truncated", "error"}) {
        Synced e;
        ::unlink((e.reader + "/a.epub").c_str());   // gone from the reader: a proposal when the server side is known
        e.server.remove("/Books/Other/d.epub");      // gone from the server: only known when that folder was listed
        e.server.set_listing_failure("/Books/Sub", mode);
        const size_t records = e.store->sync_records(e.pair).size();
        const auto before = e.reader_files();
        const int64_t plan = e.compare_ok();
        const auto p = e.plan(plan);
        PB_CHECK(p.incomplete);
        PB_CHECK(p.incomplete_folders >= 1);   // the number of folders that were not compared, never their names
        // what is absent from the incomplete server side is unknown, not missing: no proposal to delete the reader's copy of d.epub
        PB_CHECK_EQ(p.totals.n_prop_reader, 0);
        PB_CHECK(find(e.entries(plan), "Other/d.epub") == nullptr);
        // what the reader lost is known from the complete reader scan and the server's own listing: still only a proposal
        const auto planned = e.entries(plan);
        PB_CHECK(find(planned, "a.epub") != nullptr && find(planned, "a.epub")->action == PlanAction::ProposeDeleteServer);
        // an incomplete plan that holds a proposal cannot be approved as it is: a proposal needs a complete comparison
        PB_CHECK(!pbc::can_approve(p, e.clock.now()).ok);
        PB_CHECK_EQ(e.store->sync_records(e.pair).size(), records);   // no record was dropped
        PB_CHECK(e.reader_files().size() == before.size());           // and no file changed
    }
}

PB_TEST(a_server_folder_that_is_gone_and_a_reader_folder_that_is_missing_give_a_problem_not_proposals) {
    Synced e;
    e.server.remove("/Books");
    e.server.add_folder("/Elsewhere");
    const auto lost = e.compare();
    PB_CHECK(!lost.ok());
    PB_CHECK_EQ(lost.error.key, std::string("server_folder_missing"));
    PB_CHECK_EQ(e.store->sync_pair(e.pair)->problem_key, std::string("server_folder_missing"));
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM sync_plans WHERE state = 'ready'"), std::string("0"));
    PB_CHECK_EQ(e.store->sync_records(e.pair).size(), size_t(4));   // nothing was dropped
    PB_CHECK_EQ(e.reader_files().size(), size_t(4));
    // the card is out: the reader folder is not there
    Synced f;
    const auto saved = f.reader + "-away";
    PB_CHECK(::rename(f.reader.c_str(), saved.c_str()) == 0);
    const auto gone = f.compare();
    PB_CHECK(!gone.ok());
    PB_CHECK_EQ(gone.error.key, std::string("reader_folder_missing"));
    PB_CHECK_EQ(f.store->sync_pair(f.pair)->problem_key, std::string("reader_folder_missing"));
    PB_CHECK_EQ(f.store->sync_records(f.pair).size(), size_t(4));
    PB_CHECK_EQ(f.requests("DELETE") + f.requests("PUT") + f.requests("MKCOL"), size_t(0));
}

PB_TEST(an_account_that_changed_stops_compare_run_and_apply_choices_before_any_request_queue_item_or_file_change) {
    Synced e;
    ::unlink((e.reader + "/a.epub").c_str());
    const int64_t plan = e.compare_ok();   // a plan with a proposal, stored before the account changed
    const auto planned = e.entries(plan);
    const auto* a = find(planned, "a.epub");
    PB_CHECK(a != nullptr);
    PB_CHECK(e.store->sync_plan_set_choice(plan, a->seq, Choice::Delete).ok());
    const auto before = e.reader_files();
    const auto records = e.store->sync_records(e.pair).size();
    e.server.clear_log();
    pbc::AccountRow other;
    other.base_url = e.server.base_url();
    other.login_name = "someone";
    other.user_id = "someone";
    PB_CHECK(e.store->save_account(other).ok());
    PB_CHECK_EQ(e.compare().error.key, std::string("account_changed"));
    PB_CHECK_EQ(e.run(plan).error.key, std::string("account_changed"));
    pbtest::NoCancel cancel;
    PB_CHECK_EQ(e.runner->apply_choices(plan, cancel).error.key, std::string("account_changed"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK_EQ(e.store->queue_count_open(), 0);
    PB_CHECK(e.reader_files() == before);
    PB_CHECK_EQ(e.store->sync_records(e.pair).size(), records);
    pbc::QueueActions actions(*e.store, e.rules);
    PB_CHECK_EQ(actions.add_sync_delete(e.pair, "/Books/a.epub", "\"x\"").error.key, std::string("not_found"));   // not even a queue item for another account's pair
}

PB_TEST(an_incomplete_plan_can_be_approved_for_its_transfers_and_says_how_many_folders_were_not_compared) {
    Synced e;
    e.server.add_file("/Books/new.epub", pattern(90, 1));
    e.server.set_listing_failure("/Books/Sub/Deep", "error");
    const int64_t plan = e.compare_ok();
    const auto p = e.plan(plan);
    PB_CHECK(p.incomplete);
    PB_CHECK_EQ(p.incomplete_folders, 1);
    PB_CHECK(pbc::can_approve(p, e.clock.now()).ok);   // transfers only: there is no proposal in it
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.downloaded, 1);
    PB_CHECK(e.rd("new.epub") == pattern(90, 1));
}

PB_TEST_MAIN()
