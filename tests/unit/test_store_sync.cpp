// The sync part of the store (specs/003-folder-sync/contracts/core-interfaces.md "The store", data-model.md): pairs and their lock, the
// records, the plans and their paged view, the state machine of a plan and the recovery at start.
#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "pbc/store.h"
#include "pbc/sync_types.h"
#include "pbc/types.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

pbc::SystemClock g_clock;

std::unique_ptr<pbc::Store> open_store(const pbtest::TmpDir& d) {
    auto r = pbc::Store::open(d.file("data"), g_clock);
    if (!r.ok()) PB_FAIL_AT("cannot open the store: " + r.error.key);
    return std::move(r.value);
}

pbc::SyncPair pair(const std::string& remote = "/Books", const std::string& local = "/mnt/ext1/Books") {
    pbc::SyncPair p = pbc::SyncPair();
    p.server = "https://cloud.example.org";
    p.user_id = "demo";
    p.remote_folder = remote;
    p.local_folder = local;
    return p;
}

int64_t add_pair(pbc::Store& s, const std::string& remote = "/Books", const std::string& local = "/mnt/ext1/Books") {
    auto r = s.sync_pair_add(pair(remote, local));
    if (!r.ok()) PB_FAIL_AT("sync_pair_add failed: " + r.error.key);
    return r.value;
}

pbc::SyncRecord record(int64_t pair_id, const std::string& rel, const std::string& local_rel = "") {
    pbc::SyncRecord r;
    r.pair_id = pair_id;
    r.rel = rel;
    r.local_rel = local_rel.empty() ? rel : local_rel;
    r.origin = "download";
    r.server_etag = "\"e1\"";
    r.server_size = 10;
    r.server_checksum = "SHA256:aa";
    r.local_size = 10;
    r.local_mtime = 1700000000;
    r.local_sha256 = "bb";
    r.synced_at = 5;
    return r;
}

pbc::PlanEntry entry(int seq, pbc::PlanAction action, const std::string& rel, int64_t size = 0) {
    pbc::PlanEntry e;
    e.seq = seq;
    e.rel = rel;
    e.local_rel = rel;
    e.action = action;
    e.size = size;
    return e;
}

pbc::SyncPlan plan_of(int64_t pair_id, std::vector<pbc::PlanEntry> entries) {
    pbc::SyncPlan p;
    p.pair_id = pair_id;
    p.created_at = 100;
    p.trigger = "tap";
    for (const auto& e : entries) {
        switch (e.action) {
        case pbc::PlanAction::Download: p.totals.n_download++; p.totals.b_download += e.size.value_or(0); break;
        case pbc::PlanAction::Upload: p.totals.n_upload++; p.totals.b_upload += e.size.value_or(0); break;
        case pbc::PlanAction::Skip: p.totals.n_skip++; break;
        case pbc::PlanAction::KeepBoth: p.totals.n_keep_both++; p.totals.b_keep_both += e.size.value_or(0); break;
        case pbc::PlanAction::ProposeDeleteServer: p.totals.n_prop_server++; break;
        case pbc::PlanAction::ProposeDeleteReader: p.totals.n_prop_reader++; break;
        }
    }
    p.entries = std::move(entries);
    return p;
}

int64_t store_plan(pbc::Store& s, const pbc::SyncPlan& p) {
    auto r = s.sync_plan_store(p);
    if (!r.ok()) PB_FAIL_AT("sync_plan_store failed: " + r.error.key);
    return r.value;
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------------------------ pairs

PB_TEST(a_pair_is_stored_validated_and_survives_a_reopen) {
    pbtest::TmpDir d;
    int64_t id = 0;
    {
        auto s = open_store(d);
        pbc::SyncPair p = pair();
        p.direction = pbc::Direction::Both;
        p.max_files = 120;
        p.max_bytes = 3LL << 30;
        p.startup_sync = true;
        auto r = s->sync_pair_add(p);
        PB_CHECK(r.ok());
        id = r.value;
        PB_CHECK(id > 0);
        PB_CHECK_EQ(s->sync_pairs().size(), size_t(1));
    }
    auto s = open_store(d);
    auto p = s->sync_pair(id);
    PB_CHECK(p.has_value());
    PB_CHECK_EQ(p->remote_folder, std::string("/Books"));
    PB_CHECK_EQ(p->local_folder, std::string("/mnt/ext1/Books"));
    PB_CHECK_EQ(p->server, std::string("https://cloud.example.org"));
    PB_CHECK_EQ(p->user_id, std::string("demo"));
    PB_CHECK(p->direction == pbc::Direction::Both);
    PB_CHECK_EQ(p->max_files, int64_t(120));
    PB_CHECK_EQ(p->max_bytes, int64_t(3LL << 30));
    PB_CHECK(p->startup_sync);
    PB_CHECK(!p->auto_sync);
    PB_CHECK_EQ(p->state, std::string("idle"));
    PB_CHECK_EQ(p->last_outcome, std::string("never"));
    PB_CHECK(p->created_at > 0);
    PB_CHECK(!s->sync_pair(id + 99).has_value());
}

PB_TEST(the_pair_validator_sees_the_other_pairs_may_canonicalize_and_can_refuse) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    std::vector<size_t> seen_others;
    s->set_pair_validator([&](pbc::SyncPair& candidate, const std::vector<pbc::SyncPair>& others) {
        seen_others.push_back(others.size());
        if (candidate.local_folder == "/mnt/ext1/Forbidden") return pbc::make_error(pbc::ErrorClass::Unsupported, "pair_folder_forbidden");
        if (candidate.local_folder.size() < 10 || candidate.local_folder.compare(candidate.local_folder.size() - 10, 10, "/canonical") != 0) candidate.local_folder += "/canonical";
        return pbc::Error();
    });
    const int64_t a = add_pair(*s, "/A", "/mnt/ext1/A");
    PB_CHECK_EQ(s->sync_pair(a)->local_folder, std::string("/mnt/ext1/A/canonical"));   // the validator's form is the stored one
    auto bad = s->sync_pair_add(pair("/B", "/mnt/ext1/Forbidden"));
    PB_CHECK(!bad.ok());
    PB_CHECK_EQ(bad.error.key, std::string("pair_folder_forbidden"));
    PB_CHECK_EQ(s->sync_pairs().size(), size_t(1));   // nothing was stored
    const int64_t b = add_pair(*s, "/B", "/mnt/ext1/B");
    PB_CHECK_EQ(seen_others.back(), size_t(1));
    // an update: the validator gets the others without the pair itself
    auto p = *s->sync_pair(b);
    p.max_files = 10;
    seen_others.clear();
    PB_CHECK(s->sync_pair_update(p).ok());
    PB_CHECK_EQ(seen_others.back(), size_t(1));
    p.local_folder = "/mnt/ext1/Forbidden";
    PB_CHECK_EQ(s->sync_pair_update(p).key, std::string("pair_folder_forbidden"));
    PB_CHECK_EQ(s->sync_pair(b)->local_folder, std::string("/mnt/ext1/B/canonical"));   // unchanged by the refusal
    // the validator never runs for a ninth pair or bad limits: the store refuses first
    s->set_pair_validator(nullptr);
    PB_CHECK(s->sync_pair_add(pair("/C", "/mnt/ext1/C")).ok());
}

PB_TEST(another_account_saved_or_signed_out_marks_the_pairs_in_the_same_transaction) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    pbc::AccountRow acc;
    acc.base_url = "https://cloud.example.org";
    acc.login_name = "demo";
    acc.user_id = "demo";
    PB_CHECK(s->save_account(acc).ok());
    const int64_t a = add_pair(*s, "/A", "/mnt/ext1/A");
    PB_CHECK(s->save_account(acc).ok());                       // the same account again marks nothing
    PB_CHECK_EQ(s->sync_pair(a)->problem_key, std::string(""));
    pbc::AccountRow other = acc;
    other.user_id = "someone";
    other.login_name = "someone";
    PB_CHECK(s->save_account(other).ok());                     // another user
    PB_CHECK_EQ(s->sync_pair(a)->problem_key, std::string("account_changed"));
    PB_CHECK(s->save_account(acc).ok());                       // and back
    PB_CHECK_EQ(s->sync_pair(a)->problem_key, std::string(""));
    pbc::AccountRow elsewhere = acc;
    elsewhere.base_url = "https://other.example.org";
    PB_CHECK(s->save_account(elsewhere).ok());                 // another server
    PB_CHECK_EQ(s->sync_pair(a)->problem_key, std::string("account_changed"));
    PB_CHECK(s->save_account(acc).ok());
    PB_CHECK(s->clear_account().ok());                         // signing out: no account, no usable pair
    PB_CHECK_EQ(s->sync_pair(a)->problem_key, std::string("account_changed"));
    PB_CHECK_EQ(s->sync_pairs().size(), size_t(1));            // nothing deleted
}

PB_TEST(a_ninth_pair_and_bad_limits_are_refused) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    for (int i = 0; i < pbc::kMaxSyncPairs; ++i) add_pair(*s, "/F" + std::to_string(i), "/mnt/ext1/F" + std::to_string(i));
    auto ninth = s->sync_pair_add(pair("/Nine", "/mnt/ext1/Nine"));
    PB_CHECK(!ninth.ok());
    PB_CHECK_EQ(ninth.error.key, std::string("pair_limit"));
    PB_CHECK_EQ(s->sync_pairs().size(), size_t(pbc::kMaxSyncPairs));

    pbtest::TmpDir d2;
    auto s2 = open_store(d2);
    for (int64_t files : std::vector<int64_t>{0, -1, 5001}) {
        pbc::SyncPair p = pair();
        p.max_files = files;
        auto r = s2->sync_pair_add(p);
        PB_CHECK(!r.ok());
        PB_CHECK_EQ(r.error.key, std::string("pair_limit_invalid"));
    }
    for (int64_t bytes : std::vector<int64_t>{0, (1LL << 20) - 1, (64LL << 30) + 1}) {
        pbc::SyncPair p = pair();
        p.max_bytes = bytes;
        auto r = s2->sync_pair_add(p);
        PB_CHECK(!r.ok());
        PB_CHECK_EQ(r.error.key, std::string("pair_limit_invalid"));
    }
    pbc::SyncPair edge = pair();
    edge.max_files = 5000;
    edge.max_bytes = 64LL << 30;
    PB_CHECK(s2->sync_pair_add(edge).ok());
    PB_CHECK_EQ(s2->sync_pairs().size(), size_t(1));
}

PB_TEST(a_pair_can_be_updated_within_the_same_rules) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t id = add_pair(*s);
    auto p = *s->sync_pair(id);
    p.direction = pbc::Direction::Up;
    p.max_files = 10;
    p.auto_sync = true;
    PB_CHECK(s->sync_pair_update(p).ok());
    auto q = *s->sync_pair(id);
    PB_CHECK(q.direction == pbc::Direction::Up);
    PB_CHECK_EQ(q.max_files, int64_t(10));
    PB_CHECK(q.auto_sync);
    p.max_files = 0;
    PB_CHECK_EQ(s->sync_pair_update(p).key, std::string("pair_limit_invalid"));
    PB_CHECK_EQ(s->sync_pair(id)->max_files, int64_t(10));   // unchanged
    p.id = id + 50;
    p.max_files = 10;
    PB_CHECK(!s->sync_pair_update(p).ok());
}

PB_TEST(the_lock_is_taken_from_idle_plan_ready_or_interrupted_and_only_by_one) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t id = add_pair(*s);
    PB_CHECK(s->sync_pair_lock(id, "comparing").ok());
    PB_CHECK_EQ(s->sync_pair(id)->state, std::string("comparing"));
    PB_CHECK_EQ(s->sync_pair_lock(id, "comparing").key, std::string("sync_busy"));
    PB_CHECK_EQ(s->sync_pair_lock(id, "running").key, std::string("sync_busy"));
    PB_CHECK(s->sync_pair_unlock(id, "plan_ready", "").ok());
    PB_CHECK_EQ(s->sync_pair(id)->state, std::string("plan_ready"));
    PB_CHECK(s->sync_pair_lock(id, "running").ok());
    PB_CHECK(s->sync_pair_unlock(id, "interrupted", "").ok());
    PB_CHECK(s->sync_pair_lock(id, "comparing").ok());
    PB_CHECK(s->sync_pair_unlock(id, "idle", "server_folder_missing").ok());
    PB_CHECK_EQ(s->sync_pair(id)->problem_key, std::string("server_folder_missing"));
    PB_CHECK(!s->sync_pair_lock(id, "idle").ok());   // only comparing and running can be locked
    PB_CHECK(!s->sync_pair_lock(id + 9, "comparing").ok());
}

PB_TEST(two_threads_racing_for_the_lock_have_exactly_one_winner) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t id = add_pair(*s);
    for (int round = 0; round < 20; ++round) {
        std::atomic<int> wins{0}, busy{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t)
            threads.emplace_back([&] {
                const pbc::Error e = s->sync_pair_lock(id, "comparing");
                if (e.ok()) wins++;
                else if (e.key == "sync_busy") busy++;
            });
        for (auto& t : threads) t.join();
        PB_CHECK_EQ(wins.load(), 1);
        PB_CHECK_EQ(busy.load(), 3);
        PB_CHECK(s->sync_pair_unlock(id, "idle", "").ok());
    }
}

PB_TEST(unlock_and_the_result_are_stored) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t id = add_pair(*s);
    PB_CHECK(s->sync_pair_lock(id, "running").ok());
    PB_CHECK(s->sync_set_result(id, "done", "{\"downloaded\":3}", 777).ok());
    PB_CHECK(s->sync_pair_unlock(id, "idle", "").ok());
    auto p = *s->sync_pair(id);
    PB_CHECK_EQ(p.last_outcome, std::string("done"));
    PB_CHECK_EQ(p.last_result_json, std::string("{\"downloaded\":3}"));
    PB_CHECK_EQ(p.last_sync_at.value_or(0), int64_t(777));
    PB_CHECK(!s->sync_pair_unlock(id, "idle", "").ok());   // not locked any more
    PB_CHECK(s->sync_pair_compared(id, 800, 12, 3456).ok());
    p = *s->sync_pair(id);
    PB_CHECK_EQ(p.last_compared_at.value_or(0), int64_t(800));
    PB_CHECK_EQ(p.held_files.value_or(-1), int64_t(12));
    PB_CHECK_EQ(p.held_bytes.value_or(-1), int64_t(3456));
}

PB_TEST(removing_a_pair_removes_its_rows_and_nothing_else) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t a = add_pair(*s, "/A", "/mnt/ext1/A");
    const int64_t b = add_pair(*s, "/B", "/mnt/ext1/B");
    for (int64_t id : {a, b}) {
        PB_CHECK(s->sync_record_put(record(id, "x.epub")).ok());
        store_plan(*s, plan_of(id, {entry(1, pbc::PlanAction::Download, "x.epub", 5)}));
    }
    PB_CHECK(s->debug_exec("INSERT INTO queue_items(kind, entry_kind, server, user_id, created_at, updated_at, pair_id, sync_rel, sync_role) "
                           "VALUES('upload', 'file', 'https://cloud.example.org', 'demo', 1, 1, " + std::to_string(a) + ", 'x.epub', 'file')").empty());
    PB_CHECK(s->set_setting("keep", "me").ok());
    PB_CHECK(s->sync_pair_remove(a).ok());
    PB_CHECK(!s->sync_pair(a).has_value());
    PB_CHECK(s->sync_pair(b).has_value());
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM sync_files WHERE pair_id = " + std::to_string(a)), std::string("0"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM sync_plans WHERE pair_id = " + std::to_string(a)), std::string("0"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM sync_plan_entries"), std::string("1"));   // only pair b's entry is left
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM sync_files WHERE pair_id = " + std::to_string(b)), std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM queue_items WHERE pair_id = " + std::to_string(a)), std::string("1"));   // the queue item stays
    PB_CHECK_EQ(s->setting("keep"), std::string("me"));
    PB_CHECK(s->sync_pair_remove(a).ok());   // idempotent
}

PB_TEST(a_running_pair_cannot_be_removed) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t id = add_pair(*s);
    PB_CHECK(s->sync_pair_lock(id, "running").ok());
    PB_CHECK_EQ(s->sync_pair_remove(id).key, std::string("sync_busy"));
    PB_CHECK(s->sync_pair(id).has_value());
}

PB_TEST(another_account_marks_every_pair_and_the_same_account_marks_none) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t a = add_pair(*s, "/A", "/mnt/ext1/A");
    const int64_t b = add_pair(*s, "/B", "/mnt/ext1/B");
    PB_CHECK(s->sync_pairs_mark_account("https://cloud.example.org", "demo").ok());
    PB_CHECK_EQ(s->sync_pair(a)->problem_key, std::string(""));
    PB_CHECK(s->sync_pairs_mark_account("https://cloud.example.org", "other").ok());
    PB_CHECK_EQ(s->sync_pair(a)->problem_key, std::string("account_changed"));
    PB_CHECK_EQ(s->sync_pair(b)->problem_key, std::string("account_changed"));
    PB_CHECK_EQ(s->sync_pairs().size(), size_t(2));   // nothing is deleted
    PB_CHECK(s->sync_pairs_mark_account("https://cloud.example.org", "demo").ok());   // the original account is back
    PB_CHECK_EQ(s->sync_pair(a)->problem_key, std::string(""));
}

// ---------------------------------------------------------------------------------------------------------------------------- records

PB_TEST(a_record_round_trips_every_column) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t id = add_pair(*s);
    pbc::SyncRecord r = record(id, "Sub/a.epub", "Sub/a (1).epub");
    r.origin = "conflict_copy";
    r.diverged = true;
    r.one_sided = "reader";
    r.quiet = true;
    r.kind = pbc::Kind::File;
    PB_CHECK(s->sync_record_put(r).ok());
    auto g = s->sync_record(id, "Sub/a.epub");
    PB_CHECK(g.has_value());
    PB_CHECK_EQ(g->local_rel, std::string("Sub/a (1).epub"));
    PB_CHECK_EQ(g->origin, std::string("conflict_copy"));
    PB_CHECK_EQ(g->server_etag, std::string("\"e1\""));
    PB_CHECK_EQ(g->server_size.value_or(-1), int64_t(10));
    PB_CHECK_EQ(g->server_checksum, std::string("SHA256:aa"));
    PB_CHECK_EQ(g->local_size.value_or(-1), int64_t(10));
    PB_CHECK_EQ(g->local_mtime.value_or(-1), int64_t(1700000000));
    PB_CHECK_EQ(g->local_sha256, std::string("bb"));
    PB_CHECK(g->diverged);
    PB_CHECK_EQ(g->one_sided, std::string("reader"));
    PB_CHECK(g->quiet);
    PB_CHECK_EQ(g->synced_at, int64_t(5));
    // a put on the same key replaces the row
    r.server_etag = "\"e2\"";
    PB_CHECK(s->sync_record_put(r).ok());
    PB_CHECK_EQ(s->sync_records(id).size(), size_t(1));
    PB_CHECK_EQ(s->sync_record(id, "Sub/a.epub")->server_etag, std::string("\"e2\""));
    // optional columns stay empty
    pbc::SyncRecord bare = record(id, "b");
    bare.server_size.reset();
    bare.local_size.reset();
    bare.local_mtime.reset();
    PB_CHECK(s->sync_record_put(bare).ok());
    auto bg = s->sync_record(id, "b");
    PB_CHECK(!bg->server_size.has_value() && !bg->local_size.has_value() && !bg->local_mtime.has_value());
    PB_CHECK(!s->sync_record(id, "missing").has_value());
}

PB_TEST(records_come_back_sorted_and_can_be_dropped) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t id = add_pair(*s);
    for (const char* n : {"b", "a/z", "a/b", "C"}) PB_CHECK(s->sync_record_put(record(id, n)).ok());
    auto all = s->sync_records(id);
    PB_CHECK_EQ(all.size(), size_t(4));
    PB_CHECK_EQ(all[0].rel, std::string("C"));   // bytes order
    PB_CHECK_EQ(all[1].rel, std::string("a/b"));
    PB_CHECK_EQ(all[2].rel, std::string("a/z"));
    PB_CHECK_EQ(all[3].rel, std::string("b"));
    PB_CHECK_EQ(s->sync_record_count(), int64_t(4));
    PB_CHECK(s->sync_record_drop(id, "a/z").ok());
    PB_CHECK(s->sync_record_drop(id, "a/z").ok());   // idempotent
    PB_CHECK_EQ(s->sync_records(id).size(), size_t(3));
}

PB_TEST(records_are_bounded_and_names_that_differ_only_by_case_are_refused) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t id = add_pair(*s);
    PB_CHECK(s->sync_record_put(record(id, "Book.epub")).ok());
    auto clash = s->sync_record_put(record(id, "other-rel", "BOOK.epub"));
    PB_CHECK_EQ(clash.key, std::string("name_collision"));
    PB_CHECK_EQ(s->sync_records(id).size(), size_t(1));
    // the bound: fill the table by SQL (fast), then one more through the API
    PB_CHECK(s->debug_exec("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < " + std::to_string(pbc::kSyncMaxRecords - 1) +
                           ") INSERT INTO sync_files(pair_id, rel, local_rel, kind, origin, synced_at) SELECT " + std::to_string(id) +
                           ", 'f' || i, 'f' || i, 'file', 'download', 1 FROM n").empty());
    PB_CHECK_EQ(s->sync_record_count(), int64_t(pbc::kSyncMaxRecords));
    PB_CHECK_EQ(s->sync_record_put(record(id, "one-too-many")).key, std::string("records_full"));
    PB_CHECK(s->sync_record_put(record(id, "f1")).ok());   // replacing an existing row is not growth
    PB_CHECK_EQ(s->sync_record_count(), int64_t(pbc::kSyncMaxRecords));
}

PB_TEST(a_record_needs_an_existing_pair) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK(!s->sync_record_put(record(42, "a")).ok());
}

// ------------------------------------------------------------------------------------------------------------------------------ plans

PB_TEST(a_plan_is_stored_with_its_header_and_entries_in_one_transaction) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t id = add_pair(*s);
    pbc::SyncPlan p = plan_of(id, {entry(1, pbc::PlanAction::Download, "a", 100), entry(2, pbc::PlanAction::Upload, "b", 50), entry(3, pbc::PlanAction::Skip, "c")});
    p.trigger = "startup";
    p.incomplete = true;
    p.incomplete_folders = 3;
    p.over_limit = "files";
    p.free_bytes = 5000;
    p.reserve_bytes = 256;
    p.quota_free = 99;
    p.totals.folders_new = 2;
    const int64_t pid = store_plan(*s, p);
    auto g = s->sync_plan(pid);
    PB_CHECK(g.has_value());
    PB_CHECK_EQ(g->pair_id, id);
    PB_CHECK_EQ(g->trigger, std::string("startup"));
    PB_CHECK_EQ(g->state, std::string("ready"));
    PB_CHECK(g->incomplete);
    PB_CHECK_EQ(g->incomplete_folders, 3);
    PB_CHECK_EQ(g->over_limit, std::string("files"));
    PB_CHECK_EQ(g->free_bytes.value_or(-1), int64_t(5000));
    PB_CHECK_EQ(g->reserve_bytes, int64_t(256));
    PB_CHECK_EQ(g->quota_free.value_or(-1), int64_t(99));
    PB_CHECK_EQ(g->totals.n_download, 1);
    PB_CHECK_EQ(g->totals.b_download, int64_t(100));
    PB_CHECK_EQ(g->totals.n_upload, 1);
    PB_CHECK_EQ(g->totals.b_upload, int64_t(50));
    PB_CHECK_EQ(g->totals.n_skip, 1);
    PB_CHECK_EQ(g->totals.folders_new, 2);
    PB_CHECK(g->entries.empty());   // header and totals only
    PB_CHECK(!g->truncated);
    PB_CHECK(!s->sync_plan(pid + 77).has_value());
}

PB_TEST(a_proposal_keeps_its_allowed_set_and_a_keep_both_its_two_names) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t id = add_pair(*s);
    pbc::PlanEntry prop = entry(1, pbc::PlanAction::ProposeDeleteReader, "gone.epub", 7);
    prop.allowed = {pbc::Choice::Leave, pbc::Choice::Delete};
    prop.reason = "deleted_on_server";
    prop.plan_lsize = 7;
    prop.plan_lmtime = 1234;
    prop.records_below = 0;
    pbc::PlanEntry both = entry(2, pbc::PlanAction::KeepBoth, "x.epub", 9);
    both.conflict_name = "x (server copy 2026-10-22).epub";
    both.conflict_name_server = "x (reader copy 2026-10-22).epub";
    both.plan_etag = "\"abc\"";
    const int64_t pid = store_plan(*s, plan_of(id, {prop, both}));
    auto page = s->sync_plan_page(pid, "all", 0, 10);
    PB_CHECK_EQ(page.total, 2);
    PB_CHECK_EQ(page.rows.size(), size_t(2));
    PB_CHECK(page.rows[0].action == pbc::PlanAction::ProposeDeleteReader);
    PB_CHECK((page.rows[0].allowed == std::set<pbc::Choice>{pbc::Choice::Leave, pbc::Choice::Delete}));
    PB_CHECK_EQ(page.rows[0].reason, std::string("deleted_on_server"));
    PB_CHECK_EQ(page.rows[0].plan_lsize.value_or(-1), int64_t(7));
    PB_CHECK_EQ(page.rows[0].plan_lmtime.value_or(-1), int64_t(1234));
    PB_CHECK(page.rows[0].choice == pbc::Choice::None);
    PB_CHECK_EQ(page.rows[1].conflict_name, std::string("x (server copy 2026-10-22).epub"));
    PB_CHECK_EQ(page.rows[1].conflict_name_server, std::string("x (reader copy 2026-10-22).epub"));
    PB_CHECK_EQ(page.rows[1].plan_etag, std::string("\"abc\""));
}

PB_TEST(storing_a_plan_supersedes_the_ready_plan_of_the_same_pair_only) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t a = add_pair(*s, "/A", "/mnt/ext1/A");
    const int64_t b = add_pair(*s, "/B", "/mnt/ext1/B");
    const int64_t pa1 = store_plan(*s, plan_of(a, {entry(1, pbc::PlanAction::Download, "x", 1)}));
    const int64_t pb1 = store_plan(*s, plan_of(b, {entry(1, pbc::PlanAction::Download, "x", 1)}));
    const int64_t pa2 = store_plan(*s, plan_of(a, {entry(1, pbc::PlanAction::Download, "y", 1)}));
    PB_CHECK_EQ(s->sync_plan(pa1)->state, std::string("superseded"));
    PB_CHECK_EQ(s->sync_plan(pa2)->state, std::string("ready"));
    PB_CHECK_EQ(s->sync_plan(pb1)->state, std::string("ready"));
}

PB_TEST(a_plan_of_more_than_5000_entries_is_stored_truncated_without_entries) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t id = add_pair(*s);
    std::vector<pbc::PlanEntry> many;
    for (int i = 1; i <= pbc::kSyncMaxPlanEntries + 1; ++i) many.push_back(entry(i, pbc::PlanAction::Skip, "f" + std::to_string(i)));
    const int64_t pid = store_plan(*s, plan_of(id, many));
    auto g = s->sync_plan(pid);
    PB_CHECK(g->truncated);
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM sync_plan_entries WHERE plan_id = " + std::to_string(pid)), std::string("0"));
    // exactly 5000 is fine
    many.pop_back();
    const int64_t ok = store_plan(*s, plan_of(id, many));
    PB_CHECK(!s->sync_plan(ok)->truncated);
    PB_CHECK_EQ(s->sync_plan_page(ok, "all", 0, 1).total, pbc::kSyncMaxPlanEntries);
}

PB_TEST(at_most_two_finished_plans_per_pair_are_kept) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t id = add_pair(*s);
    std::vector<int64_t> ids;
    for (int i = 0; i < 5; ++i) {
        const int64_t pid = store_plan(*s, plan_of(id, {entry(1, pbc::PlanAction::Download, "x" + std::to_string(i), 1)}));
        ids.push_back(pid);
        PB_CHECK(s->sync_plan_state(pid, "approved").ok());
        PB_CHECK(s->sync_plan_state(pid, "running").ok());
        PB_CHECK(s->sync_plan_state(pid, "done").ok());
    }
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM sync_plans WHERE pair_id = " + std::to_string(id)), std::to_string(pbc::kSyncFinalPlansKept));
    PB_CHECK(s->sync_plan(ids[4]).has_value());
    PB_CHECK(s->sync_plan(ids[3]).has_value());
    PB_CHECK(!s->sync_plan(ids[0]).has_value());
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM sync_plan_entries"), std::string("2"));   // the entries of removed plans went with them
}

PB_TEST(the_paged_view_returns_sections_in_plan_order_with_totals) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t id = add_pair(*s);
    std::vector<pbc::PlanEntry> es;
    es.push_back(entry(1, pbc::PlanAction::Download, "a"));
    es.push_back(entry(2, pbc::PlanAction::Skip, "b"));
    es.push_back(entry(3, pbc::PlanAction::Upload, "c"));
    es.push_back(entry(4, pbc::PlanAction::ProposeDeleteServer, "d"));
    es.push_back(entry(5, pbc::PlanAction::KeepBoth, "e"));
    es.push_back(entry(6, pbc::PlanAction::ProposeDeleteReader, "f"));
    es.push_back(entry(7, pbc::PlanAction::Skip, "g"));
    const int64_t pid = store_plan(*s, plan_of(id, es));
    auto t = s->sync_plan_page(pid, "transfers", 0, 10);
    PB_CHECK_EQ(t.total, 3);
    PB_CHECK_EQ(t.rows[0].rel + t.rows[1].rel + t.rows[2].rel, std::string("ace"));
    auto sk = s->sync_plan_page(pid, "skips", 0, 10);
    PB_CHECK_EQ(sk.total, 2);
    PB_CHECK_EQ(sk.rows[0].rel + sk.rows[1].rel, std::string("bg"));
    auto pr = s->sync_plan_page(pid, "proposals", 0, 10);
    PB_CHECK_EQ(pr.total, 2);
    PB_CHECK_EQ(pr.rows[0].rel + pr.rows[1].rel, std::string("df"));
    auto all = s->sync_plan_page(pid, "all", 2, 3);
    PB_CHECK_EQ(all.total, 7);
    PB_CHECK_EQ(all.rows.size(), size_t(3));
    PB_CHECK_EQ(all.rows[0].seq, 3);
    PB_CHECK_EQ(all.rows[2].seq, 5);
    PB_CHECK_EQ(s->sync_plan_page(pid, "transfers", 2, 10).rows.size(), size_t(1));
    PB_CHECK_EQ(s->sync_plan_page(pid, "nonsense", 0, 10).total, 0);
    PB_CHECK_EQ(s->sync_plan_page(pid + 5, "all", 0, 10).total, 0);
}

PB_TEST(removing_an_entry_works_for_transfers_of_a_ready_plan_and_updates_the_counts) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t id = add_pair(*s);
    const int64_t pid = store_plan(*s, plan_of(id, {entry(1, pbc::PlanAction::Download, "a", 100), entry(2, pbc::PlanAction::Download, "b", 50),
                                                     entry(3, pbc::PlanAction::Upload, "c", 7), entry(4, pbc::PlanAction::KeepBoth, "d", 9),
                                                     entry(5, pbc::PlanAction::Skip, "e"), entry(6, pbc::PlanAction::ProposeDeleteReader, "f")}));
    PB_CHECK(s->sync_plan_remove_entry(pid, 1, true).ok());
    auto g = *s->sync_plan(pid);
    PB_CHECK_EQ(g.totals.n_download, 1);
    PB_CHECK_EQ(g.totals.b_download, int64_t(50));
    PB_CHECK_EQ(g.totals.n_skip, 2);   // the skip and the removed entry
    PB_CHECK(s->sync_plan_page(pid, "transfers", 0, 10).rows[0].removed);
    PB_CHECK(s->sync_plan_remove_entry(pid, 1, false).ok());   // put it back
    g = *s->sync_plan(pid);
    PB_CHECK_EQ(g.totals.n_download, 2);
    PB_CHECK_EQ(g.totals.b_download, int64_t(150));
    PB_CHECK_EQ(g.totals.n_skip, 1);
    PB_CHECK(s->sync_plan_remove_entry(pid, 3, true).ok());
    PB_CHECK(s->sync_plan_remove_entry(pid, 4, true).ok());
    // not a transfer, or not there
    PB_CHECK(!s->sync_plan_remove_entry(pid, 5, true).ok());
    PB_CHECK(!s->sync_plan_remove_entry(pid, 6, true).ok());
    PB_CHECK(!s->sync_plan_remove_entry(pid, 99, true).ok());
    // not while the plan is not ready
    PB_CHECK(s->sync_plan_state(pid, "approved").ok());
    PB_CHECK(!s->sync_plan_remove_entry(pid, 2, true).ok());
}

PB_TEST(a_choice_is_accepted_only_for_a_proposal_and_only_from_its_allowed_set) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t id = add_pair(*s);
    pbc::PlanEntry prop = entry(1, pbc::PlanAction::ProposeDeleteServer, "gone", 3);
    prop.allowed = {pbc::Choice::Leave, pbc::Choice::TransferAgain};
    const int64_t pid = store_plan(*s, plan_of(id, {prop, entry(2, pbc::PlanAction::Download, "dl", 1)}));
    PB_CHECK(s->sync_plan_set_choice(pid, 1, pbc::Choice::Leave).ok());
    PB_CHECK(s->sync_plan_page(pid, "proposals", 0, 10).rows[0].choice == pbc::Choice::Leave);
    PB_CHECK(s->sync_plan_set_choice(pid, 1, pbc::Choice::TransferAgain).ok());
    PB_CHECK(!s->sync_plan_set_choice(pid, 1, pbc::Choice::Delete).ok());   // not allowed for this proposal
    PB_CHECK(s->sync_plan_page(pid, "proposals", 0, 10).rows[0].choice == pbc::Choice::TransferAgain);   // unchanged by the refusal
    PB_CHECK(s->sync_plan_set_choice(pid, 1, pbc::Choice::None).ok());   // clearing is always possible
    PB_CHECK(s->sync_plan_page(pid, "proposals", 0, 10).rows[0].choice == pbc::Choice::None);
    PB_CHECK(!s->sync_plan_set_choice(pid, 2, pbc::Choice::Leave).ok());   // a download is no proposal
    PB_CHECK(!s->sync_plan_set_choice(pid, 9, pbc::Choice::Leave).ok());
}

PB_TEST(the_plan_state_machine_refuses_every_illegal_transition) {
    const std::vector<std::string> states = {"ready", "approved", "running", "done", "interrupted", "cancelled", "superseded"};
    const std::vector<std::pair<std::string, std::string>> legal = {{"ready", "approved"},    {"ready", "cancelled"},    {"ready", "superseded"},
                                                                    {"approved", "running"},  {"approved", "interrupted"}, {"running", "done"},
                                                                    {"running", "interrupted"}, {"interrupted", "superseded"}};
    for (const auto& from : states)
        for (const auto& to : states) {
            bool want = false;
            for (const auto& l : legal)
                if (l.first == from && l.second == to) want = true;
            PB_CHECK_EQ(pbc::plan_transition_allowed(from, to), want);
        }
    // and the store enforces it: walk a plan to every state and try every target
    for (const auto& from : states) {
        for (const auto& to : states) {
            pbtest::TmpDir d;
            auto s = open_store(d);
            const int64_t id = add_pair(*s);
            const int64_t pid = store_plan(*s, plan_of(id, {entry(1, pbc::PlanAction::Download, "a", 1)}));
            if (from == "approved" || from == "running" || from == "done" || from == "interrupted") PB_CHECK(s->sync_plan_state(pid, "approved").ok());
            if (from == "running" || from == "done") PB_CHECK(s->sync_plan_state(pid, "running").ok());
            if (from == "done") PB_CHECK(s->sync_plan_state(pid, "done").ok());
            if (from == "interrupted") PB_CHECK(s->sync_plan_state(pid, "interrupted").ok());
            if (from == "cancelled") PB_CHECK(s->sync_plan_state(pid, "cancelled").ok());
            if (from == "superseded") PB_CHECK(s->sync_plan_state(pid, "superseded").ok());
            PB_CHECK_EQ(s->sync_plan(pid)->state, from);
            const pbc::Error e = s->sync_plan_state(pid, to);
            PB_CHECK_EQ(e.ok(), pbc::plan_transition_allowed(from, to));
            PB_CHECK_EQ(s->sync_plan(pid)->state, e.ok() ? to : from);
        }
    }
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK(!s->sync_plan_state(77, "approved").ok());
    const int64_t id = add_pair(*s);
    const int64_t pid = store_plan(*s, plan_of(id, {}));
    PB_CHECK(!s->sync_plan_state(pid, "bogus").ok());
}

PB_TEST(approval_and_finishing_leave_their_times) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t id = add_pair(*s);
    const int64_t pid = store_plan(*s, plan_of(id, {entry(1, pbc::PlanAction::Download, "a", 1)}));
    PB_CHECK(!s->sync_plan(pid)->approved_at.has_value());
    PB_CHECK(s->sync_plan_state(pid, "approved").ok());
    PB_CHECK(s->sync_plan(pid)->approved_at.has_value());
    PB_CHECK(!s->sync_plan(pid)->finished_at.has_value());
    PB_CHECK(s->sync_plan_state(pid, "running").ok());
    PB_CHECK(s->sync_plan_state(pid, "done").ok());
    PB_CHECK(s->sync_plan(pid)->finished_at.has_value());
}

PB_TEST(entry_results_update_the_entry_and_the_failed_count) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t id = add_pair(*s);
    const int64_t pid = store_plan(*s, plan_of(id, {entry(1, pbc::PlanAction::Download, "a", 1), entry(2, pbc::PlanAction::Download, "b", 1),
                                                     entry(3, pbc::PlanAction::Download, "c", 1)}));
    PB_CHECK(s->sync_entry_result(pid, 1, "done", "").ok());
    PB_CHECK(s->sync_entry_result(pid, 2, "failed", "storage_full").ok());
    PB_CHECK(s->sync_entry_result(pid, 3, "skipped", "changed_during_sync").ok());
    auto rows = s->sync_plan_page(pid, "all", 0, 10).rows;
    PB_CHECK_EQ(rows[0].state, std::string("done"));
    PB_CHECK_EQ(rows[1].state, std::string("failed"));
    PB_CHECK_EQ(rows[1].result_key, std::string("storage_full"));
    PB_CHECK_EQ(rows[2].state, std::string("skipped"));
    PB_CHECK_EQ(s->sync_plan(pid)->totals.n_failed, 1);
    PB_CHECK(!s->sync_entry_result(pid, 1, "weird", "").ok());
    PB_CHECK(!s->sync_entry_result(pid, 77, "done", "").ok());
}

// ------------------------------------------------------------------------------------------------------------------------ queue and recovery

PB_TEST(the_open_queue_names_of_a_pair_are_listed) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const int64_t a = add_pair(*s, "/A", "/mnt/ext1/A");
    const int64_t b = add_pair(*s, "/B", "/mnt/ext1/B");
    auto item = [&](int64_t pair_id, const char* rel, const char* state) {
        PB_CHECK(s->debug_exec(std::string("INSERT INTO queue_items(kind, entry_kind, server, user_id, created_at, updated_at, state, pair_id, sync_rel, sync_role) VALUES("
                                           "'upload', 'file', 'https://cloud.example.org', 'demo', 1, 1, '") + state + "', " + std::to_string(pair_id) + ", '" + rel + "', 'file')").empty());
    };
    item(a, "one.epub", "queued");
    item(a, "two.epub", "sending");
    item(a, "three.epub", "unknown");
    item(a, "done.epub", "done");
    item(b, "other.epub", "queued");
    PB_CHECK(s->debug_exec("INSERT INTO queue_items(kind, entry_kind, server, user_id, created_at, updated_at) VALUES('upload', 'file', 'https://cloud.example.org', 'demo', 1, 1)").empty());
    const auto rels = s->queue_sync_open_rels(a);
    PB_CHECK((rels == std::set<std::string>{"one.epub", "two.epub", "three.epub"}));
    PB_CHECK((s->queue_sync_open_rels(b) == std::set<std::string>{"other.epub"}));
    PB_CHECK(s->queue_sync_open_rels(99).empty());
}

PB_TEST(recovery_at_start_interrupts_what_a_kill_left_running_and_touches_nothing_else) {
    pbtest::TmpDir d;
    int64_t a = 0, b = 0, c = 0, pa = 0, pc = 0;
    {
        auto s = open_store(d);
        a = add_pair(*s, "/A", "/mnt/ext1/A");
        b = add_pair(*s, "/B", "/mnt/ext1/B");
        c = add_pair(*s, "/C", "/mnt/ext1/C");
        PB_CHECK(s->sync_pair_lock(a, "comparing").ok());
        PB_CHECK(s->sync_pair_lock(c, "running").ok());
        pa = store_plan(*s, plan_of(a, {entry(1, pbc::PlanAction::Download, "x", 1)}));
        pc = store_plan(*s, plan_of(c, {entry(1, pbc::PlanAction::Download, "y", 1)}));
        PB_CHECK(s->sync_plan_state(pc, "approved").ok());
        PB_CHECK(s->sync_plan_state(pc, "running").ok());
        PB_CHECK(s->sync_record_put(record(c, "kept")).ok());
    }
    auto s = open_store(d);   // the next start
    const auto report = s->sync_recover();
    PB_CHECK_EQ(report.pairs_interrupted, 2);
    PB_CHECK_EQ(report.plans_interrupted, 1);
    PB_CHECK_EQ(s->sync_pair(a)->state, std::string("interrupted"));
    PB_CHECK_EQ(s->sync_pair(b)->state, std::string("idle"));
    PB_CHECK_EQ(s->sync_pair(c)->state, std::string("interrupted"));
    PB_CHECK_EQ(s->sync_plan(pa)->state, std::string("ready"));   // a plan that was only stored is untouched
    PB_CHECK_EQ(s->sync_plan(pc)->state, std::string("interrupted"));
    PB_CHECK_EQ(s->sync_records(c).size(), size_t(1));
    const auto again = s->sync_recover();   // idempotent
    PB_CHECK_EQ(again.pairs_interrupted, 0);
    PB_CHECK_EQ(again.plans_interrupted, 0);
    PB_CHECK(s->sync_pair_lock(a, "comparing").ok());   // an interrupted pair can be locked again
}

PB_TEST_MAIN()
