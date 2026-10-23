// The uploads of a pair's approved plan through the queue of feature 002 (specs/003-folder-sync US4, research R-8): one group written in one transaction,
// sent alone, and confirmed together with the pair's record - never the record without the upload, never the upload done without the record.
#include <string>
#include <vector>

#include "pbc/sync_rules.h"
#include "pbc/sync_types.h"
#include "upload_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::UpEnv;

namespace {

struct Out : UpEnv {
    std::string outbox;
    int64_t pair = 0;
    Out() {
        outbox = root + "/Outbox";
        pbtest::up_mkdirs(outbox);
        pbc::SyncPair p = pbc::default_pair(server.base_url(), "demo");
        p.remote_folder = "/Docs";
        p.local_folder = outbox;
        p.direction = pbc::Direction::Up;
        pair = store->sync_pair_add(p).value;
        cache("/Docs");
    }
    pbc::SyncUploadSpec spec(const std::string& rel, long long size, uint8_t seed = 1) {
        pbc::SyncUploadSpec s;
        s.rel = rel;
        s.local_abs = source("Outbox/" + rel, size, seed);
        const size_t slash = rel.rfind('/');
        s.dst_parent = slash == std::string::npos ? "/Docs" : "/Docs/" + rel.substr(0, slash);
        s.dst_name = rel.substr(slash == std::string::npos ? 0 : slash + 1);
        return s;
    }
};

}  // namespace

PB_TEST(a_group_has_a_folder_item_for_every_folder_the_server_lacks_and_uploads_that_wait_for_theirs) {
    Out e;
    e.server.add_folder("/Docs/Known");
    e.cache("/Docs");
    e.cache("/Docs");
    const auto r = e.actions().add_sync_uploads(e.pair, "/Docs", {e.spec("a.txt", 100), e.spec("Trips/day1.txt", 200), e.spec("Trips/Deep/day2.txt", 300), e.spec("Known/k.txt", 50)});
    PB_CHECK(r.ok());
    PB_CHECK(r.value.group_id > 0);
    PB_CHECK_EQ(r.value.items.size(), size_t(4));
    PB_CHECK(r.value.refused.empty());
    const auto page = e.store->queue_page(0, 100);
    PB_CHECK_EQ(page.total, 6);   // Trips and Trips/Deep, and the four files
    int mkdirs = 0;
    for (const auto& it : page.rows) {
        PB_CHECK_EQ(it.group_id.value_or(-1), r.value.group_id);
        PB_CHECK_EQ(it.pair_id.value_or(-1), e.pair);
        if (it.kind == pbc::ItemKind::Mkdir) {
            ++mkdirs;
            PB_CHECK(it.sync_rel.empty());
            PB_CHECK(it.sync_role.empty());
        } else {
            PB_CHECK_EQ(it.sync_role, std::string("file"));
        }
    }
    PB_CHECK_EQ(mkdirs, 2);
    const auto day1 = e.item(r.value.items[1]);
    PB_CHECK_EQ(day1.sync_rel, std::string("Trips/day1.txt"));
    PB_CHECK_EQ(day1.dst_parent, std::string("/Docs/Trips"));
    PB_CHECK(day1.depends_on.has_value());
    PB_CHECK(e.item(*day1.depends_on).kind == pbc::ItemKind::Mkdir);
    const auto day2 = e.item(r.value.items[2]);
    const auto deep = e.item(*day2.depends_on);
    PB_CHECK_EQ(deep.dst_name, std::string("Deep"));
    PB_CHECK_EQ(e.item(*deep.depends_on).dst_name, std::string("Trips"));   // every folder before what it holds
    PB_CHECK(!e.item(r.value.items[0]).depends_on.has_value());            // a file in the pair's own folder waits for nothing
    PB_CHECK(!e.item(r.value.items[3]).depends_on.has_value());            // Known is on the server already
    PB_CHECK_EQ(e.server.request_count(), size_t(0));                      // nothing was sent
}

PB_TEST(a_changed_file_is_a_replace_conditional_on_the_given_version_and_a_new_one_is_not) {
    Out e;
    e.server.add_file("/Docs/old.txt", "old content");
    e.cache("/Docs");
    pbc::SyncUploadSpec changed = e.spec("old.txt", 120, 3);
    changed.replace_etag = e.etag("/Docs/old.txt");
    const auto r = e.actions().add_sync_uploads(e.pair, "/Docs", {changed, e.spec("new.txt", 10)});
    PB_CHECK(r.ok());
    const auto a = e.item(r.value.items[0]);
    PB_CHECK(a.replace);
    PB_CHECK_EQ(a.dst_etag, e.etag("/Docs/old.txt"));
    PB_CHECK(!e.item(r.value.items[1]).replace);
}

PB_TEST(a_source_that_cannot_be_read_is_refused_alone_and_a_call_that_does_not_fit_queues_nothing) {
    Out e;
    pbc::SyncUploadSpec gone = e.spec("gone.txt", 10);
    ::unlink(gone.local_abs.c_str());
    const auto r = e.actions().add_sync_uploads(e.pair, "/Docs", {gone, e.spec("here.txt", 10)});
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.items[0], int64_t(0));
    PB_CHECK(r.value.items[1] > 0);
    PB_CHECK_EQ(r.value.refused.size(), size_t(1));
    PB_CHECK_EQ(r.value.refused[0].path, std::string("gone.txt"));
    PB_CHECK_EQ(r.value.refused[0].key, std::string("source_unreadable"));
    // too many for the queue: nothing at all
    Out f;
    const pbc::SyncUploadSpec one = f.spec("one.txt", 5);
    std::vector<pbc::SyncUploadSpec> many;
    for (int i = 0; i < pbc::kMaxQueueItems + 1; ++i) {
        pbc::SyncUploadSpec s = one;
        s.rel = s.dst_name = "n" + std::to_string(i) + ".txt";
        many.push_back(s);
    }
    const auto full = f.actions().add_sync_uploads(f.pair, "/Docs", many);
    PB_CHECK(!full.ok());
    PB_CHECK_EQ(full.error.key, std::string("queue_full"));
    PB_CHECK_EQ(f.store->queue_page(0, 10).total, 0);
}

PB_TEST(a_sync_run_sends_only_its_own_group_and_leaves_the_users_other_items_queued) {
    Out e;
    const auto mine = e.queue({e.source("Downloads/mine.txt", 40)});   // the user's own upload, queued before
    const auto g = e.actions().add_sync_uploads(e.pair, "/Docs", {e.spec("a.txt", 100), e.spec("Sub/b.txt", 200)});
    PB_CHECK(g.ok());
    pbtest::UpNoCancel cancel;
    const pbc::SendReport rep = e.sender()->run(cancel, 0, g.value.group_id);
    PB_CHECK_EQ(rep.done, 3);   // the folder and the two files
    PB_CHECK(e.server.has("/Docs/a.txt"));
    PB_CHECK(e.server.has("/Docs/Sub/b.txt"));
    PB_CHECK(!e.server.has("/Docs/mine.txt"));
    PB_CHECK(e.item(mine[0]).state == pbc::ItemState::Queued);
    PB_CHECK_EQ(rep.left, 1);
    // the default run still takes everything that is left
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK(e.server.has("/Docs/mine.txt"));
}

PB_TEST(the_confirmation_writes_the_record_in_the_same_step_as_the_done_row) {
    Out e;
    const auto g = e.actions().add_sync_uploads(e.pair, "/Docs", {e.spec("Trips/day1.txt", 200, 4)});
    std::vector<std::string> seen;
    e.on_hook = [&](const char* step) {
        if (std::string(step) != "confirm") return;
        seen.push_back(e.store->sync_record(e.pair, "Trips/day1.txt") ? "record" : "no record");   // before the confirmation: nothing is recorded
    };
    pbtest::UpNoCancel cancel;
    PB_CHECK_EQ(e.sender()->run(cancel, 0, g.value.group_id).done, 2);
    PB_CHECK_EQ(seen.size(), size_t(2));   // the folder and the file
    PB_CHECK_EQ(seen[1], std::string("no record"));
    const auto rec = e.store->sync_record(e.pair, "Trips/day1.txt");
    PB_CHECK(rec.has_value());
    PB_CHECK_EQ(rec->origin, std::string("upload"));
    PB_CHECK_EQ(rec->local_rel, std::string("Trips/day1.txt"));
    PB_CHECK_EQ(rec->server_etag, e.etag("/Docs/Trips/day1.txt"));
    PB_CHECK_EQ(rec->server_size.value_or(-1), int64_t(200));
    PB_CHECK_EQ(rec->local_size.value_or(-1), int64_t(200));
    PB_CHECK(rec->local_mtime.has_value());
    PB_CHECK_EQ(rec->local_sha256, e.sha_of(e.outbox + "/Trips/day1.txt"));
    PB_CHECK(!e.store->sync_record(e.pair, "Trips").has_value());   // folders are not records
    PB_CHECK(e.item(g.value.items[0]).state == pbc::ItemState::Done);
}

PB_TEST(an_item_whose_pair_was_removed_finishes_as_an_ordinary_item_and_writes_no_record) {
    Out e;
    const auto g = e.actions().add_sync_uploads(e.pair, "/Docs", {e.spec("a.txt", 100)});
    PB_CHECK(e.store->sync_pair_remove(e.pair).ok());
    pbtest::UpNoCancel cancel;
    PB_CHECK_EQ(e.sender()->run(cancel, 0, g.value.group_id).done, 1);
    PB_CHECK(e.server.has("/Docs/a.txt"));
    PB_CHECK(e.item(g.value.items[0]).state == pbc::ItemState::Done);
    PB_CHECK_EQ(e.store->sync_record_count(), int64_t(0));
}

PB_TEST(failed_conflict_and_unknown_items_leave_the_record_as_it_was) {
    Out e;
    // an earlier sync recorded old.txt; the server changed it afterwards, so a conditional replace meets a different version
    e.server.add_file("/Docs/old.txt", "old content");
    e.cache("/Docs");
    pbc::SyncRecord before;
    before.pair_id = e.pair;
    before.rel = before.local_rel = "old.txt";
    before.origin = "upload";
    before.server_etag = e.etag("/Docs/old.txt");
    before.server_size = 11;
    before.local_size = 11;
    before.local_mtime = 1;
    before.synced_at = 5;
    PB_CHECK(e.store->sync_record_put(before).ok());
    pbc::SyncUploadSpec changed = e.spec("old.txt", 120, 3);
    changed.replace_etag = before.server_etag;
    e.server.set_file("/Docs/old.txt", "someone else changed it", 1790000500);
    const auto conflict = e.actions().add_sync_uploads(e.pair, "/Docs", {changed});
    pbtest::UpNoCancel cancel;
    e.sender()->run(cancel, 0, conflict.value.group_id);
    PB_CHECK(e.item(conflict.value.items[0]).state == pbc::ItemState::Conflict);
    PB_CHECK_EQ(e.store->sync_record(e.pair, "old.txt")->server_etag, before.server_etag);
    PB_CHECK_EQ(e.store->sync_record(e.pair, "old.txt")->synced_at, int64_t(5));
    // a lost answer: the upload may have arrived, the item is unknown, no record
    const auto lost = e.actions().add_sync_uploads(e.pair, "/Docs", {e.spec("lost.txt", 60)});
    e.server.lose_next_answer("PUT", "lost.txt");
    e.sender()->run(cancel, 0, lost.value.group_id);
    PB_CHECK(e.item(lost.value.items[0]).state == pbc::ItemState::Unknown);
    PB_CHECK(!e.store->sync_record(e.pair, "lost.txt").has_value());
    // a source that vanished: failed, no record
    pbc::SyncUploadSpec vanishing = e.spec("v.txt", 60);
    const auto fail = e.actions().add_sync_uploads(e.pair, "/Docs", {vanishing});
    ::unlink(vanishing.local_abs.c_str());
    e.sender()->run(cancel, 0, fail.value.group_id);
    PB_CHECK(e.item(fail.value.items[0]).state == pbc::ItemState::Failed);
    PB_CHECK(!e.store->sync_record(e.pair, "v.txt").has_value());
    // the user marking the unknown one as arrived is a confirmation like any other: the record follows
    PB_CHECK(e.sender()->mark_unknown_done(lost.value.items[0]).ok());
    PB_CHECK(e.store->sync_record(e.pair, "lost.txt").has_value());
}

PB_TEST_MAIN()
