// Deletions are proposals (specs/003-folder-sync US7, R-10), against the fake Nextcloud: a file removed on one side is only listed; approving the plan changes
// nothing on either side; each choice of the user does exactly what it says - copy it back, leave it, delete it on the side that still has it (the server's
// trash through the queue, the reader's copy only while unchanged) - and a folder that is gone as a whole is one proposal.
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "pbc/queue_actions.h"
#include "sync_env.h"

PBC_FAKE_SERVER_TEST();

using pbc::Choice;
using pbc::PlanAction;
using pbtest::SyncEnv;
using pbtest::find;
using pbtest::pattern;

namespace {

struct Two : SyncEnv {
    Two() {
        direction(pbc::Direction::Both);
        pbc::ServerCaps caps;
        caps.trash = true;
        caps.chunking = true;
        if (!store->save_server_caps(caps).ok()) PB_FAIL_AT("cannot save the capabilities");
        for (const char* n : {"a.epub", "b.epub", "c.epub"}) server.add_file(std::string("/Books/") + n, pattern(300, n[0]));
        sync();
        server.clear_log();
    }
    std::map<std::string, std::string> reader_files() const {
        std::map<std::string, std::string> out;
        for (const std::string& f : reader_tree()) out[f] = rd(f);
        return out;
    }
    std::map<std::string, std::string> server_files() const {
        std::map<std::string, std::string> out;
        for (const auto& kv : server.nodes())
            if (!kv.second.folder && kv.first.compare(0, 7, "/Books/") == 0) out[kv.first] = kv.second.content;
        return out;
    }
    void remove_on_reader(const std::string& rel) { ::unlink((reader + "/" + rel).c_str()); }
    pbc::Result<pbc::SyncResult> apply(int64_t plan) {
        pbtest::NoCancel c;
        return runner->apply_choices(plan, c);
    }
    void choose(int64_t plan, const std::string& rel, Choice c) {
        const auto all = entries(plan);
        const auto* e = find(all, rel);
        if (!e) PB_FAIL_AT("no proposal for " + rel);
        const pbc::Error err = store->sync_plan_set_choice(plan, e->seq, c);
        if (!err.ok()) PB_FAIL_AT("cannot choose for " + rel + ": " + err.key);
    }
};

size_t content_gets(const SyncEnv& e) {
    size_t n = 0;
    for (const auto& r : e.server.log())
        if (r.method == "GET" && r.path.find("/dav/files/") != std::string::npos) ++n;
    return n;
}

}  // namespace

PB_TEST(a_file_removed_on_either_side_is_only_a_proposal_and_approving_the_plan_deletes_nothing) {
    Two e;
    e.server.remove("/Books/a.epub");   // gone from the server: the reader still has it
    e.remove_on_reader("b.epub");        // gone from the reader: the server still has it
    const auto reader_before = e.reader_files();
    const auto server_before = e.server_files();
    const int64_t plan = e.compare_ok();
    const auto entries = e.entries(plan);
    const auto* a = find(entries, "a.epub");
    const auto* b = find(entries, "b.epub");
    PB_CHECK(a != nullptr && a->action == PlanAction::ProposeDeleteReader);
    PB_CHECK(b != nullptr && b->action == PlanAction::ProposeDeleteServer);
    PB_CHECK(a->allowed == std::set<Choice>({Choice::Leave, Choice::Delete, Choice::TransferAgain}));
    PB_CHECK(b->allowed == std::set<Choice>({Choice::Leave, Choice::Delete, Choice::TransferAgain}));
    PB_CHECK(find(entries, "c.epub") == nullptr);
    PB_CHECK_EQ(e.plan(plan).totals.n_in_sync, 1);
    // approving the plan: nothing is deleted, nothing is transferred, the proposals stay open
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.deleted, 0);
    PB_CHECK_EQ(result.proposals_open, 2);
    PB_CHECK(e.reader_files() == reader_before);
    PB_CHECK(e.server_files() == server_before);
    PB_CHECK_EQ(e.requests("DELETE"), size_t(0));
    PB_CHECK_EQ(e.requests("PUT"), size_t(0));
    PB_CHECK_EQ(content_gets(e), size_t(0));
    PB_CHECK(e.store->sync_record(e.pair, "a.epub").has_value());   // the records stay
    PB_CHECK(e.store->sync_record(e.pair, "b.epub").has_value());
    // the next comparison proposes the same two things again
    const int64_t again = e.compare_ok();
    PB_CHECK_EQ(e.plan(again).totals.n_prop_reader + e.plan(again).totals.n_prop_server, 2);
}

PB_TEST(delete_removes_the_file_on_the_side_that_still_has_it_the_server_copy_into_the_trash_and_the_reader_copy_for_good) {
    Two e;
    e.server.remove("/Books/a.epub");
    e.remove_on_reader("b.epub");
    const int64_t plan = e.compare_ok();
    e.choose(plan, "a.epub", Choice::Delete);   // delete it on the reader
    e.choose(plan, "b.epub", Choice::Delete);   // delete it on the server
    e.server.clear_log();
    const auto result = e.apply(plan);
    PB_CHECK(result.ok());
    PB_CHECK_EQ(result.value.deleted, 2);
    PB_CHECK_EQ(result.value.failed, 0);
    PB_CHECK(!e.has("a.epub"));                       // the reader copy is gone
    PB_CHECK(!e.server.has("/Books/b.epub"));
    PB_CHECK(e.server.trash_has("/Books/b.epub"));    // the server copy is in the trash
    PB_CHECK(e.has("c.epub") && e.server.has("/Books/c.epub"));
    PB_CHECK(!e.store->sync_record(e.pair, "a.epub").has_value());
    PB_CHECK(!e.store->sync_record(e.pair, "b.epub").has_value());
    PB_CHECK(e.store->sync_record(e.pair, "c.epub").has_value());
    // the DELETE was conditional on the version the record knows
    bool conditional = false;
    for (const auto& r : e.server.log())
        if (r.method == "DELETE" && !r.if_match.empty()) conditional = true;
    PB_CHECK(conditional);
    PB_CHECK_EQ(e.store->queue_count_open(), 0);
    // nothing is proposed afterwards
    const int64_t again = e.compare_ok();
    PB_CHECK(e.entries(again).empty());
    PB_CHECK_EQ(e.plan(again).totals.n_in_sync, 1);
}

PB_TEST(transfer_again_puts_the_file_back_on_the_side_that_lacks_it_through_the_normal_paths) {
    Two e;
    e.server.remove("/Books/a.epub");
    e.remove_on_reader("b.epub");
    const int64_t plan = e.compare_ok();
    e.choose(plan, "a.epub", Choice::TransferAgain);   // send it to the server again
    e.choose(plan, "b.epub", Choice::TransferAgain);   // copy it back to the reader
    e.server.clear_log();
    const auto result = e.apply(plan);
    PB_CHECK(result.ok());
    PB_CHECK_EQ(result.value.uploaded, 1);
    PB_CHECK_EQ(result.value.downloaded, 1);
    PB_CHECK(e.server.node("/Books/a.epub").content == pattern(300, 'a'));
    PB_CHECK(e.rd("b.epub") == pattern(300, 'b'));
    PB_CHECK(e.store->sync_record(e.pair, "a.epub").has_value());
    PB_CHECK(e.store->sync_record(e.pair, "b.epub").has_value());
    PB_CHECK(!e.part_file());
    const int64_t again = e.compare_ok();
    PB_CHECK(e.entries(again).empty());   // both are in sync again
}

PB_TEST(leave_makes_the_record_quiet_so_that_the_next_plan_is_silent_until_the_present_file_changes) {
    Two e;
    e.server.remove("/Books/a.epub");
    e.remove_on_reader("b.epub");
    const int64_t plan = e.compare_ok();
    e.choose(plan, "a.epub", Choice::Leave);
    e.choose(plan, "b.epub", Choice::Leave);
    const auto before_reader = e.reader_files();
    const auto before_server = e.server_files();
    const auto result = e.apply(plan);
    PB_CHECK(result.ok());
    PB_CHECK_EQ(result.value.deleted, 0);
    PB_CHECK(e.reader_files() == before_reader && e.server_files() == before_server);
    PB_CHECK(e.store->sync_record(e.pair, "a.epub")->quiet);
    PB_CHECK(e.store->sync_record(e.pair, "b.epub")->quiet);
    const int64_t quiet = e.compare_ok();
    PB_CHECK(e.entries(quiet).empty());
    PB_CHECK_EQ(e.plan(quiet).totals.n_prop_reader + e.plan(quiet).totals.n_prop_server, 0);
    // the server file that is still there changes: proposed again, and never for deletion
    e.server.touch_file("/Books/b.epub", pattern(400, 'x'), 1790009000);
    const int64_t changed = e.compare_ok();
    const auto after = e.entries(changed);
    const auto* b = find(after, "b.epub");
    PB_CHECK(b != nullptr && b->action == PlanAction::ProposeDeleteServer);
    PB_CHECK(b->allowed.count(Choice::Delete) == 0);
}

PB_TEST(a_changed_reader_file_is_never_offered_for_deletion_and_a_choice_that_is_not_allowed_is_refused) {
    Two e;
    e.server.remove("/Books/a.epub");
    e.write_reader("a.epub", pattern(350, 9));   // changed on the reader since the sync
    ::utimensat(AT_FDCWD, (e.reader + "/a.epub").c_str(), (struct timespec[2]){{e.clock.t + 77, 0}, {e.clock.t + 77, 0}}, 0);
    const int64_t plan = e.compare_ok();
    const auto planned = e.entries(plan);
    const auto* a = find(planned, "a.epub");
    PB_CHECK(a != nullptr && a->action == PlanAction::ProposeDeleteReader);
    PB_CHECK(a->allowed.count(Choice::Delete) == 0);
    PB_CHECK_EQ(e.store->sync_plan_set_choice(plan, a->seq, Choice::Delete).key, std::string("not_allowed"));
    PB_CHECK(e.store->sync_plan_set_choice(plan, a->seq, Choice::Leave).ok());
    // a proposal that is not a proposal takes no choice
    PB_CHECK_EQ(e.store->sync_plan_set_choice(plan, 999, Choice::Leave).key, std::string("not_found"));
    // and even a forced delete re-checks the file: it changed after the plan, so nothing is removed
    Two f;
    f.server.remove("/Books/a.epub");
    const int64_t plan2 = f.compare_ok();
    f.choose(plan2, "a.epub", Choice::Delete);
    f.write_reader("a.epub", pattern(333, 5));   // changed between the plan and the confirmation
    ::utimensat(AT_FDCWD, (f.reader + "/a.epub").c_str(), (struct timespec[2]){{f.clock.t + 88, 0}, {f.clock.t + 88, 0}}, 0);
    const auto result = f.apply(plan2);
    PB_CHECK(result.ok());
    PB_CHECK_EQ(result.value.deleted, 0);
    PB_CHECK_EQ(result.value.skipped, 1);
    PB_CHECK(f.rd("a.epub") == pattern(333, 5));
    PB_CHECK(f.store->sync_record(f.pair, "a.epub").has_value());
}

PB_TEST(without_a_trash_a_deletion_on_the_server_is_not_offered_and_not_queued) {
    Two e;
    pbc::ServerCaps caps;
    caps.trash = false;
    PB_CHECK(e.store->save_server_caps(caps).ok());
    e.remove_on_reader("b.epub");
    const int64_t plan = e.compare_ok();
    const auto planned = e.entries(plan);
    const auto* b = find(planned, "b.epub");
    PB_CHECK(b != nullptr && b->action == PlanAction::ProposeDeleteServer);
    PB_CHECK(b->allowed.count(Choice::Delete) == 0);
    pbc::QueueActions actions(*e.store, e.rules);
    PB_CHECK_EQ(actions.add_sync_delete(e.pair, "/Books/b.epub", e.server.etag_of("/Books/b.epub")).error.key, std::string("trash_unavailable"));
    PB_CHECK_EQ(e.store->queue_count_open(), 0);
}

PB_TEST(a_folder_that_is_gone_as_a_whole_is_one_proposal_and_its_deletion_removes_only_synced_unchanged_files) {
    Two e;
    // Novels holds three synced files; the server loses the whole folder
    e.server.add_file("/Books/Novels/n1.epub", pattern(100, 1));
    e.server.add_file("/Books/Novels/n2.epub", pattern(110, 2));
    e.server.add_file("/Books/Novels/Deep/n3.epub", pattern(120, 3));
    e.sync();
    e.server.remove("/Books/Novels");
    e.write_reader("Novels/n2.epub", pattern(111, 9));   // changed since: it stays
    ::utimensat(AT_FDCWD, (e.reader + "/Novels/n2.epub").c_str(), (struct timespec[2]){{e.clock.t + 55, 0}, {e.clock.t + 55, 0}}, 0);
    e.write_reader("Novels/new.epub", "added later");     // never synced: it stays
    const int64_t plan = e.compare_ok();
    const auto entries = e.entries(plan);
    const auto* n = find(entries, "Novels");
    PB_CHECK(n != nullptr && n->action == PlanAction::ProposeDeleteReader);
    PB_CHECK_EQ(n->records_below, 3);
    PB_CHECK(find(entries, "Novels/n1.epub") == nullptr);   // not listed on their own
    PB_CHECK(n->allowed == std::set<Choice>({Choice::Leave, Choice::Delete, Choice::TransferAgain}));
    e.choose(plan, "Novels", Choice::Delete);
    const auto result = e.apply(plan);
    PB_CHECK(result.ok());
    PB_CHECK_EQ(result.value.deleted, 2);   // n1 and n3: unchanged and synced
    PB_CHECK_EQ(result.value.skipped, 1);   // n2 changed: kept
    PB_CHECK(!e.has("Novels/n1.epub") && !e.has("Novels/Deep/n3.epub"));
    PB_CHECK(e.has("Novels/n2.epub") && e.has("Novels/new.epub"));
    PB_CHECK(!e.store->sync_record(e.pair, "Novels/n1.epub").has_value());
    PB_CHECK(e.store->sync_record(e.pair, "Novels/n2.epub").has_value());
}

PB_TEST(a_folder_removed_on_the_reader_is_deleted_on_the_server_into_the_trash_when_the_user_confirms) {
    Two e;
    e.server.add_file("/Books/Poems/p1.epub", pattern(100, 1));
    e.server.add_file("/Books/Poems/p2.epub", pattern(110, 2));
    e.sync();
    ::unlink((e.reader + "/Poems/p1.epub").c_str());
    ::unlink((e.reader + "/Poems/p2.epub").c_str());
    ::rmdir((e.reader + "/Poems").c_str());
    const int64_t plan = e.compare_ok();
    const auto planned = e.entries(plan);
    const auto* p = find(planned, "Poems");
    PB_CHECK(p != nullptr && p->action == PlanAction::ProposeDeleteServer && p->records_below == 2);
    PB_CHECK(p->allowed == std::set<Choice>({Choice::Leave, Choice::Delete, Choice::TransferAgain}));
    e.choose(plan, "Poems", Choice::Delete);
    const auto result = e.apply(plan);
    PB_CHECK(result.ok());
    PB_CHECK_EQ(result.value.deleted, 1);   // the one folder
    PB_CHECK(!e.server.has("/Books/Poems"));
    PB_CHECK(e.server.trash_has("/Books/Poems"));
    PB_CHECK(!e.store->sync_record(e.pair, "Poems/p1.epub").has_value());
    PB_CHECK(!e.store->sync_record(e.pair, "Poems/p2.epub").has_value());
}

PB_TEST(a_folder_proposal_can_be_transferred_again_in_both_directions_with_the_files_it_held) {
    Two e;
    e.server.add_file("/Books/Novels/n1.epub", pattern(100, 1));
    e.server.add_file("/Books/Novels/Deep/n2.epub", pattern(110, 2));
    e.server.add_file("/Books/Poems/p1.epub", pattern(120, 3));
    e.sync();
    // the reader loses Novels, the server loses Poems
    ::unlink((e.reader + "/Novels/n1.epub").c_str());
    ::unlink((e.reader + "/Novels/Deep/n2.epub").c_str());
    ::rmdir((e.reader + "/Novels/Deep").c_str());
    ::rmdir((e.reader + "/Novels").c_str());
    e.server.remove("/Books/Poems");
    const int64_t plan = e.compare_ok();
    const auto planned = e.entries(plan);
    PB_CHECK(find(planned, "Novels") != nullptr && find(planned, "Poems") != nullptr);
    e.choose(plan, "Novels", Choice::TransferAgain);   // copy it back to the reader
    e.choose(plan, "Poems", Choice::TransferAgain);    // send it to the server again
    e.server.clear_log();
    const auto result = e.apply(plan);
    PB_CHECK(result.ok());
    PB_CHECK_EQ(result.value.downloaded, 2);
    PB_CHECK_EQ(result.value.uploaded, 1);
    PB_CHECK_EQ(result.value.failed, 0);
    PB_CHECK(e.rd("Novels/n1.epub") == pattern(100, 1));
    PB_CHECK(e.rd("Novels/Deep/n2.epub") == pattern(110, 2));
    PB_CHECK(e.server.node("/Books/Poems/p1.epub").content == pattern(120, 3));
    PB_CHECK(!e.part_file());
    const auto after = e.entries(plan);
    PB_CHECK_EQ(find(after, "Novels")->state, std::string("done"));
    PB_CHECK_EQ(find(after, "Poems")->state, std::string("done"));
    // everything is in sync again
    const int64_t again = e.compare_ok();
    PB_CHECK(e.entries(again).empty());
}

PB_TEST_MAIN()
