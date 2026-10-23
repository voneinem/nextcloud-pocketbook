// An upload-only sync from compare to result (specs/003-folder-sync US4), against the fake Nextcloud: the files of an Outbox reach the server and stay
// where they are, a second sync sends nothing, a change is sent as a conditional replace, what cannot be sent is a skip with its reason, a lost answer is
// left to the user and never repeated, and an upload-only pair never downloads.
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "sync_env.h"

PBC_FAKE_SERVER_TEST();

using pbc::PlanAction;
using pbtest::SyncEnv;
using pbtest::find;
using pbtest::pattern;

namespace {

struct Outbox : SyncEnv {
    Outbox() {
        server.add_folder("/Books");
        direction(pbc::Direction::Up);
        write_reader("a.txt", pattern(300, 1));
        write_reader("b.txt", pattern(400, 2));
        write_reader("c.txt", pattern(500, 3));
        write_reader("Sub/d.txt", pattern(600, 4));
        server.clear_log();
    }
    std::map<std::string, std::string> contents() const {
        std::map<std::string, std::string> out;
        for (const std::string& f : reader_tree()) out[f] = rd(f);
        return out;
    }
};

// The downloads of file content: GET on the WebDAV tree (the sender reads the capabilities with a GET as well).
size_t content_gets(const SyncEnv& e) {
    size_t n = 0;
    for (const auto& r : e.server.log())
        if (r.method == "GET" && r.path.find("/dav/files/") != std::string::npos) ++n;
    return n;
}

}  // namespace

PB_TEST(an_outbox_with_files_and_a_folder_reaches_the_server_identical_and_stays_unchanged) {
    Outbox e;
    const auto before = e.contents();
    const int64_t plan = e.compare_ok();
    PB_CHECK_EQ(e.plan(plan).totals.n_upload, 4);
    PB_CHECK_EQ(e.plan(plan).totals.n_download, 0);
    PB_CHECK_EQ(e.plan(plan).totals.folders_new, 1);
    PB_CHECK_EQ(e.requests("PUT"), size_t(0));   // the comparison sends nothing
    e.steps.clear();
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.uploaded, 4);
    PB_CHECK_EQ(result.failed, 0);
    PB_CHECK_EQ(result.stopped_by_key, std::string(""));
    PB_CHECK(e.server.node("/Books/a.txt").content == pattern(300, 1));
    PB_CHECK(e.server.node("/Books/b.txt").content == pattern(400, 2));
    PB_CHECK(e.server.node("/Books/c.txt").content == pattern(500, 3));
    PB_CHECK(e.server.node("/Books/Sub/d.txt").content == pattern(600, 4));
    PB_CHECK_EQ(e.requests("MKCOL"), size_t(1));
    PB_CHECK(e.contents() == before);   // nothing moved, renamed, deleted or rewritten
    PB_CHECK_EQ(content_gets(e), size_t(0));
    PB_CHECK_EQ(e.store->sync_records(e.pair).size(), size_t(4));
    PB_CHECK_EQ(e.store->sync_record(e.pair, "Sub/d.txt")->origin, std::string("upload"));
    PB_CHECK_EQ(e.store->sync_plan(plan)->state, std::string("done"));
    PB_CHECK_EQ(e.store->sync_pair(e.pair)->state, std::string("idle"));
    PB_CHECK_EQ(e.store->queue_count_open(), 0);
    // the hooks: the group before the first request
    PB_CHECK(std::find(e.steps.begin(), e.steps.end(), "group_created") != e.steps.end());
    // a second sync sends nothing
    e.server.clear_log();
    const int64_t again = e.compare_ok();
    PB_CHECK_EQ(e.plan(again).totals.n_upload, 0);
    PB_CHECK_EQ(e.plan(again).totals.n_in_sync, 4);
    PB_CHECK(e.entries(again).empty());
    PB_CHECK_EQ(e.requests("PUT"), size_t(0));
    PB_CHECK_EQ(e.requests("MKCOL"), size_t(0));
}

PB_TEST(an_edited_file_is_a_conditional_replace_and_a_server_that_changed_meanwhile_keeps_the_item_for_the_user) {
    Outbox e;
    e.sync();
    const std::string etag_before = e.server.etag_of("/Books/a.txt");
    // edited on the reader: a new size and time
    e.write_reader("a.txt", pattern(350, 9));
    ::utimensat(AT_FDCWD, (e.reader + "/a.txt").c_str(), (struct timespec[2]){{e.clock.t + 50, 0}, {e.clock.t + 50, 0}}, 0);
    const int64_t plan = e.compare_ok();
    const auto entries = e.entries(plan);
    const auto* entry = find(entries, "a.txt");
    PB_CHECK(entry != nullptr && entry->action == PlanAction::Upload);
    PB_CHECK_EQ(entry->plan_etag, etag_before);
    PB_CHECK_EQ(e.plan(plan).totals.n_upload, 1);
    e.server.clear_log();
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.uploaded, 1);
    PB_CHECK(e.server.node("/Books/a.txt").content == pattern(350, 9));
    const auto puts = e.server.log();
    bool conditional = false;
    for (const auto& r : puts)
        if (r.method == "PUT" && r.if_match == etag_before) conditional = true;
    PB_CHECK(conditional);   // replaced only while the server still had the version the record knows
    PB_CHECK_EQ(e.store->sync_record(e.pair, "a.txt")->server_etag, e.server.etag_of("/Books/a.txt"));
    // edited again, but the server's file changed after the comparison: a 002 conflict, nothing overwritten, the record as it was
    e.write_reader("b.txt", pattern(450, 8));
    ::utimensat(AT_FDCWD, (e.reader + "/b.txt").c_str(), (struct timespec[2]){{e.clock.t + 90, 0}, {e.clock.t + 90, 0}}, 0);
    const int64_t plan2 = e.compare_ok();
    PB_CHECK_EQ(e.plan(plan2).totals.n_upload, 1);
    const auto record_before = e.store->sync_record(e.pair, "b.txt");
    e.server.touch_file("/Books/b.txt", "changed by someone else", 1790009000);
    const auto result2 = e.run_ok(plan2);
    PB_CHECK_EQ(result2.uploaded, 0);
    PB_CHECK_EQ(result2.failed, 1);
    PB_CHECK(e.server.node("/Books/b.txt").content == "changed by someone else");   // never overwritten
    PB_CHECK_EQ(e.store->sync_record(e.pair, "b.txt")->server_etag, record_before->server_etag);
    PB_CHECK_EQ(e.store->queue_count_open(), 1);   // the item waits in the Queue for the user's choice (keep both versions)
    const auto open = e.store->queue_page(0, 50);
    bool conflict = false;
    for (const auto& it : open.rows)
        if (it.state == pbc::ItemState::Conflict) conflict = true;
    PB_CHECK(conflict);
}

PB_TEST(what_cannot_be_sent_is_a_skip_with_its_reason_and_the_rest_goes_on) {
    Outbox e;
    e.write_reader("empty.txt", "");
    e.write_reader("ends-with-dot.", "x");
    e.write_reader("unreadable.txt", "secret");
    ::chmod((e.reader + "/unreadable.txt").c_str(), 0000);
    const std::string huge = e.reader + "/huge.bin";
    {
        std::ofstream(huge, std::ios::binary) << "x";
        ::truncate(huge.c_str(), (4LL << 30) + 10);
    }
    const int64_t plan = e.compare_ok();
    const auto entries = e.entries(plan);
    PB_CHECK_EQ(find(entries, "empty.txt")->reason, std::string("source_unreadable"));
    PB_CHECK_EQ(find(entries, "ends-with-dot.")->reason, std::string("name_forbidden"));
    PB_CHECK_EQ(find(entries, "huge.bin")->reason, std::string("too_large_for_storage"));
    PB_CHECK(find(entries, "unreadable.txt") != nullptr);
    PB_CHECK(find(entries, "unreadable.txt")->action == PlanAction::Skip);
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.uploaded, 4);   // the four ordinary files
    PB_CHECK(!e.server.has("/Books/empty.txt"));
    PB_CHECK(!e.server.has("/Books/huge.bin"));
    PB_CHECK(!e.server.has("/Books/unreadable.txt"));
    ::chmod((e.reader + "/unreadable.txt").c_str(), 0600);
}

PB_TEST(a_lost_answer_leaves_one_unknown_item_that_later_syncs_do_not_repeat) {
    Outbox e;
    e.server.lose_next_answer("PUT", "b.txt");
    const int64_t plan = e.compare_ok();
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.uploaded, 3);
    PB_CHECK_EQ(result.failed, 1);
    bool no_answer = false;
    for (const auto& f : result.failure_keys)
        if (f.first == "no_answer") no_answer = true;
    PB_CHECK(no_answer);
    PB_CHECK(!e.store->sync_record(e.pair, "b.txt").has_value());   // not synced until the user decides
    size_t unknown = 0;
    for (const auto& it : e.store->queue_page(0, 50).rows)
        if (it.state == pbc::ItemState::Unknown) ++unknown;
    PB_CHECK_EQ(unknown, size_t(1));
    // the next sync leaves it alone
    e.server.clear_log();
    const int64_t next = e.compare_ok();
    PB_CHECK_EQ(find(e.entries(next), "b.txt")->reason, std::string("change_pending"));
    PB_CHECK_EQ(e.plan(next).totals.n_upload, 0);
    e.sync();
    PB_CHECK_EQ(e.requests("PUT"), size_t(0));
    unknown = 0;
    for (const auto& it : e.store->queue_page(0, 50).rows)
        if (it.state == pbc::ItemState::Unknown) ++unknown;
    PB_CHECK_EQ(unknown, size_t(1));
}

PB_TEST(a_file_for_a_folder_the_server_has_goes_in_without_a_new_folder_even_when_the_folder_was_never_opened) {
    Outbox e;
    e.server.add_folder("/Books/Existing");
    e.server.add_folder("/Books/Existing/Deeper");
    e.write_reader("Existing/x.txt", pattern(50, 1));
    e.write_reader("Existing/Deeper/y.txt", pattern(60, 2));
    e.write_reader("Existing/New/z.txt", pattern(70, 3));
    const int64_t plan = e.compare_ok();
    PB_CHECK_EQ(e.plan(plan).totals.folders_new, 2);   // Sub and Existing/New
    e.server.clear_log();
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.uploaded, 7);
    PB_CHECK_EQ(result.failed, 0);
    PB_CHECK_EQ(e.requests("MKCOL"), size_t(2));
    PB_CHECK(e.server.node("/Books/Existing/Deeper/y.txt").content == pattern(60, 2));
    PB_CHECK(e.server.node("/Books/Existing/New/z.txt").content == pattern(70, 3));
}

PB_TEST(an_upload_only_pair_never_downloads_and_never_changes_the_reader) {
    Outbox e;
    e.server.add_file("/Books/from-server.epub", pattern(100, 5));
    e.server.add_file("/Books/a.txt", "different on the server", 1790001234);
    const auto before = e.contents();
    const int64_t plan = e.compare_ok();
    PB_CHECK_EQ(e.plan(plan).totals.n_download, 0);
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.downloaded, 0);
    PB_CHECK_EQ(content_gets(e), size_t(0));
    PB_CHECK(e.contents() == before);
    PB_CHECK(!e.has("from-server.epub"));
}

PB_TEST_MAIN()
