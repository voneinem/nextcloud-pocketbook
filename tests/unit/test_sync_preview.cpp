// The offline preview (specs/003-folder-sync US2 scenario 8): what the stored listings of the folders the user has opened would give. It is flagged stale
// and incomplete, writes no plan row, makes no request and can never be approved.
#include <string>

#include "pbc/folder_service.h"
#include "sync_env.h"

PBC_FAKE_SERVER_TEST();

using pbc::PlanAction;
using pbtest::SyncEnv;
using pbtest::find;
using pbtest::pattern;

namespace {

void visit(SyncEnv& e, const std::string& folder) {
    pbc::FolderService fs(*e.store, e.server, e.clock);
    PB_CHECK(fs.load(folder, pbc::Fetch::Always).error.ok());
}

pbc::Result<pbc::SyncPlan> preview(SyncEnv& e) {
    pbtest::NoCancel c;
    return e.runner->preview_from_cache(e.pair, c);
}

const pbc::PlanEntry* entry(const pbc::SyncPlan& p, const std::string& rel) {
    for (const auto& e : p.entries)
        if (e.rel == rel) return &e;
    return nullptr;
}

}  // namespace

PB_TEST(the_preview_plans_from_the_listings_the_user_has_opened_and_flags_the_rest) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(100));
    e.server.add_file("/Books/Novels/b.epub", pattern(200));
    e.server.add_file("/Books/Unvisited/c.epub", pattern(300));
    visit(e, "/Books");
    visit(e, "/Books/Novels");
    e.server.clear_log();
    const auto r = preview(e);
    PB_CHECK(r.ok());
    const pbc::SyncPlan& p = r.value;
    PB_CHECK_EQ(p.state, std::string("preview"));
    PB_CHECK(p.incomplete);
    PB_CHECK_EQ(p.incomplete_folders, 1);   // /Books/Unvisited was never listed
    PB_CHECK(entry(p, "a.epub") != nullptr && entry(p, "a.epub")->action == PlanAction::Download);
    PB_CHECK(entry(p, "Novels/b.epub") != nullptr);
    PB_CHECK(entry(p, "Unvisited/c.epub") == nullptr);   // nothing is known about what is in it
    PB_CHECK_EQ(p.totals.n_download, 2);
    PB_CHECK_EQ(p.totals.b_download, int64_t(300));
}

PB_TEST(the_preview_writes_no_plan_makes_no_request_and_changes_no_file) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(100));
    e.write_reader("mine.epub", "mine");
    visit(e, "/Books");
    e.server.clear_log();
    const auto card = e.reader_tree();
    const auto r = preview(e);
    PB_CHECK(r.ok());
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM sync_plans"), std::string("0"));
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM sync_plan_entries"), std::string("0"));
    PB_CHECK(e.store->sync_records(e.pair).empty());
    PB_CHECK(e.reader_tree() == card);
    PB_CHECK_EQ(e.store->sync_pair(e.pair)->state, std::string("idle"));   // the pair is not locked or marked
}

PB_TEST(the_preview_reports_the_age_of_the_oldest_listing_and_cannot_be_approved) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(100));
    e.server.add_file("/Books/Novels/b.epub", pattern(200));
    visit(e, "/Books/Novels");           // fetched at the clock's first time
    const int64_t first = e.clock.t;
    e.clock.t += 5000;
    visit(e, "/Books");
    const auto r = preview(e);
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.created_at, first);   // the oldest of the listings used
    const auto a = pbc::can_approve(r.value, e.clock.t);
    PB_CHECK(!a.ok);
    PB_CHECK_EQ(a.error_key, std::string("plan_not_approvable"));
}

PB_TEST(a_pair_whose_server_folder_was_never_listed_previews_an_empty_incomplete_plan) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(100));
    const auto r = preview(e);   // nothing was ever opened
    PB_CHECK(r.ok());
    PB_CHECK(r.value.entries.empty());
    PB_CHECK(r.value.incomplete);
    PB_CHECK_EQ(r.value.incomplete_folders, 1);
}

PB_TEST(a_stored_listing_is_compared_with_the_records_like_a_fresh_one_but_never_proposes_deletions) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(100));
    e.server.add_file("/Books/gone.epub", pattern(100));
    e.sync();   // both are on the reader and recorded
    visit(e, "/Books");
    e.server.remove("/Books/gone.epub");   // the cache still lists it; and a later listing would not
    e.server.touch_file("/Books/a.epub", pattern(150, 2), 1790009999);
    visit(e, "/Books");   // the listing the preview will use: a.epub changed, gone.epub absent
    const auto r = preview(e);
    PB_CHECK(r.ok());
    PB_CHECK(entry(r.value, "a.epub") != nullptr && entry(r.value, "a.epub")->action == PlanAction::Download);
    PB_CHECK(entry(r.value, "gone.epub") == nullptr);   // an incomplete picture proposes nothing and drops no record
}

PB_TEST(an_account_that_changed_cannot_be_previewed) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(100));
    visit(e, "/Books");
    pbc::AccountRow other;
    other.base_url = e.server.base_url();
    other.login_name = "someone";
    other.user_id = "someone";
    PB_CHECK(e.store->save_account(other).ok());
    const auto r = preview(e);
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("account_changed"));
}

PB_TEST_MAIN()
