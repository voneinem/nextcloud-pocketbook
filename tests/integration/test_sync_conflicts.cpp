// Two versions of one name (specs/003-folder-sync US6, research R-9), against the fake Nextcloud: a file changed on both sides and a name created on both
// sides before the first sync end with both versions on each side the direction allows, under distinct names, the originals keeping their own side's version;
// repeating the sync makes nothing new (SC-005); identical files are in sync without a transfer.
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>

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

void touch_mtime(const std::string& path, int64_t t) {
    struct timespec ts[2] = {{t, 0}, {t, 0}};
    ::utimensat(AT_FDCWD, path.c_str(), ts, 0);
}

// a.epub synced both ways, then changed on both sides with different content
void change_both(SyncEnv& e) {
    e.server.add_file("/Books/a.epub", pattern(1000, 1));
    e.sync();
    e.write_reader("a.epub", pattern(1200, 7));
    touch_mtime(e.reader + "/a.epub", e.clock.t + 100);
    e.server.touch_file("/Books/a.epub", pattern(1100, 8), 1790007777);
}

size_t content_gets(const SyncEnv& e) {
    size_t n = 0;
    for (const auto& r : e.server.log())
        if (r.method == "GET" && r.path.find("/dav/files/") != std::string::npos) ++n;
    return n;
}

}  // namespace

PB_TEST(a_file_changed_on_both_sides_ends_with_both_versions_on_both_sides_and_a_repeat_makes_nothing_new) {
    SyncEnv e;
    e.direction(pbc::Direction::Both);
    change_both(e);
    const int64_t plan = e.compare_ok();
    const auto entries = e.entries(plan);
    const auto* a = find(entries, "a.epub");
    PB_CHECK(a != nullptr && a->action == PlanAction::KeepBoth);
    const std::string reader_copy = a->conflict_name, server_copy = a->conflict_name_server;
    PB_CHECK(!reader_copy.empty() && !server_copy.empty() && reader_copy != server_copy);
    e.server.clear_log();
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.kept_both, 1);
    PB_CHECK_EQ(result.failed, 0);
    // the originals keep their own side's version
    PB_CHECK(e.rd("a.epub") == pattern(1200, 7));
    PB_CHECK(e.server.node("/Books/a.epub").content == pattern(1100, 8));
    // and each side has the other's under a new name
    PB_CHECK(e.rd(reader_copy) == pattern(1100, 8));
    PB_CHECK(e.server.node("/Books/" + server_copy).content == pattern(1200, 7));
    // the bookkeeping
    const auto original = e.store->sync_record(e.pair, "a.epub");
    PB_CHECK(original.has_value() && original->diverged);
    const auto rc = e.store->sync_record(e.pair, reader_copy);
    PB_CHECK(rc.has_value());
    PB_CHECK_EQ(rc->origin, std::string("conflict_copy"));
    PB_CHECK_EQ(rc->one_sided, std::string("reader"));
    PB_CHECK_EQ(rc->copy_of, std::string("a.epub"));
    const auto sc = e.store->sync_record(e.pair, server_copy);
    PB_CHECK(sc.has_value());
    PB_CHECK_EQ(sc->origin, std::string("conflict_copy"));
    PB_CHECK_EQ(sc->one_sided, std::string("server"));
    PB_CHECK_EQ(sc->copy_of, std::string("a.epub"));
    PB_CHECK(!e.part_file());
    PB_CHECK_EQ(e.store->queue_count_open(), 0);
    // repeating creates nothing: the originals and the copies are kept apart and nothing is transferred
    const auto tree = e.reader_tree();
    for (int round = 0; round < 2; ++round) {
        e.server.clear_log();
        const int64_t again = e.compare_ok();
        for (const auto& en : e.entries(again)) {
            PB_CHECK(en.action == PlanAction::Skip);
            PB_CHECK_EQ(en.reason, std::string("kept_both"));
        }
        e.run_ok(again);
        PB_CHECK_EQ(content_gets(e), size_t(0));
        for (const char* m : {"PUT", "MKCOL", "MOVE", "COPY", "DELETE"}) PB_CHECK_EQ(e.requests(m), size_t(0));
        PB_CHECK(e.reader_tree() == tree);
    }
}

PB_TEST(a_name_created_on_both_sides_before_the_first_sync_with_different_content_is_kept_in_both_versions) {
    SyncEnv e;
    e.direction(pbc::Direction::Both);
    e.server.add_file("/Books/Sub/clash.epub", pattern(500, 2));
    e.write_reader("Sub/clash.epub", pattern(600, 3));
    e.write_reader("Sub/mine.epub", pattern(100, 4));
    const int64_t plan = e.compare_ok();
    const auto entries = e.entries(plan);
    const auto* c = find(entries, "Sub/clash.epub");
    PB_CHECK(c != nullptr && c->action == PlanAction::KeepBoth);
    PB_CHECK(find(entries, "Sub/mine.epub")->action == PlanAction::Upload);
    const std::string reader_copy = c->conflict_name, server_copy = c->conflict_name_server;
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.kept_both, 1);
    PB_CHECK_EQ(result.uploaded, 1);
    PB_CHECK(e.rd("Sub/clash.epub") == pattern(600, 3));
    PB_CHECK(e.server.node("/Books/Sub/clash.epub").content == pattern(500, 2));
    PB_CHECK(e.rd(reader_copy) == pattern(500, 2));
    PB_CHECK(e.server.node("/Books/" + server_copy).content == pattern(600, 3));
    PB_CHECK(reader_copy.compare(0, 4, "Sub/") == 0 && server_copy.compare(0, 4, "Sub/") == 0);   // beside the original
    // the next sync has nothing to do
    e.server.clear_log();
    const int64_t again = e.compare_ok();
    PB_CHECK_EQ(e.plan(again).totals.n_download + e.plan(again).totals.n_upload + e.plan(again).totals.n_keep_both, 0);
}

PB_TEST(a_down_pair_adds_the_server_version_to_the_reader_only_and_an_up_pair_adds_the_reader_version_to_the_server_only) {
    {
        SyncEnv e;   // down
        e.server.add_file("/Books/a.epub", pattern(500, 2));
        e.write_reader("a.epub", pattern(600, 3));
        e.server.clear_log();
        const int64_t plan = e.compare_ok();
        const std::string copy = find(e.entries(plan), "a.epub")->conflict_name;
        PB_CHECK(!copy.empty());
        PB_CHECK(find(e.entries(plan), "a.epub")->conflict_name_server.empty());
        e.run_ok(plan);
        PB_CHECK(e.rd(copy) == pattern(500, 2));
        PB_CHECK(e.rd("a.epub") == pattern(600, 3));
        for (const char* m : {"PUT", "MKCOL", "MOVE", "COPY", "DELETE"}) PB_CHECK_EQ(e.requests(m), size_t(0));   // the server is not written
        PB_CHECK(e.store->sync_record(e.pair, "a.epub")->diverged);
        const int64_t again = e.compare_ok();
        PB_CHECK_EQ(e.plan(again).totals.n_download + e.plan(again).totals.n_upload + e.plan(again).totals.n_keep_both, 0);
    }
    {
        SyncEnv e;   // up
        e.server.add_folder("/Books");
        e.direction(pbc::Direction::Up);
        e.server.add_file("/Books/a.epub", pattern(500, 2));
        e.write_reader("a.epub", pattern(600, 3));
        e.server.clear_log();
        const int64_t plan = e.compare_ok();
        const std::string copy = find(e.entries(plan), "a.epub")->conflict_name_server;
        PB_CHECK(!copy.empty());
        PB_CHECK(find(e.entries(plan), "a.epub")->conflict_name.empty());
        const auto before = e.reader_tree();
        e.run_ok(plan);
        PB_CHECK(e.server.node("/Books/" + copy).content == pattern(600, 3));
        PB_CHECK(e.server.node("/Books/a.epub").content == pattern(500, 2));
        PB_CHECK(e.reader_tree() == before);   // the reader is not written
        PB_CHECK_EQ(content_gets(e), size_t(0));
        PB_CHECK(e.store->sync_record(e.pair, "a.epub")->diverged);
        const int64_t again = e.compare_ok();
        PB_CHECK_EQ(e.plan(again).totals.n_download + e.plan(again).totals.n_upload + e.plan(again).totals.n_keep_both, 0);
    }
}

PB_TEST(identical_files_are_in_sync_without_a_transfer_and_equal_size_without_a_checksum_is_a_conflict) {
    SyncEnv e;
    e.direction(pbc::Direction::Both);
    const std::string content = pattern(900, 5);
    e.server.add_file("/Books/same.epub", content);
    e.write_reader("same.epub", content);
    {   // the listing carries the SHA-256 of the file: equal name, size and checksum
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        EVP_Digest(content.data(), content.size(), md, &len, EVP_sha256(), nullptr);
        static const char* hex = "0123456789abcdef";
        std::string sum;
        for (unsigned int i = 0; i < len; ++i) { sum += hex[md[i] >> 4]; sum += hex[md[i] & 15]; }
        e.server.set_checksums("/Books/same.epub", sum);
    }
    e.server.add_file("/Books/lookalike.epub", pattern(900, 6));
    e.write_reader("lookalike.epub", pattern(900, 7));   // the same size, other bytes, and no checksum in the listing to tell
    const int64_t plan = e.compare_ok();
    const auto entries = e.entries(plan);
    PB_CHECK(find(entries, "lookalike.epub")->action == PlanAction::KeepBoth);
    PB_CHECK(find(entries, "same.epub")->action == PlanAction::Skip);
    PB_CHECK_EQ(find(entries, "same.epub")->reason, std::string("identical"));
    e.server.clear_log();
    e.run_ok(plan);
    PB_CHECK_EQ(content_gets(e), size_t(1));   // the lookalike's server version only
}

PB_TEST(a_kept_both_sync_that_stopped_after_one_copy_makes_only_the_missing_copy_the_next_time) {
    SyncEnv e;
    e.direction(pbc::Direction::Both);
    change_both(e);
    const int64_t plan = e.compare_ok();
    const std::string reader_copy = find(e.entries(plan), "a.epub")->conflict_name;
    // the connection drops while the server copy is being queued: the reader copy exists, the original is not marked yet
    pbtest::CancelFlag cancel;
    e.on_step = [&](const char* step) {
        if (std::string(step) == "group_created") cancel.flag = true;
    };
    const auto cut = e.run(plan, cancel);
    PB_CHECK(cut.ok());
    PB_CHECK(e.has(reader_copy));
    PB_CHECK(!e.store->sync_record(e.pair, "a.epub")->diverged);
    e.on_step = nullptr;
    // whatever is still queued is sent, as the user would; then syncing again
    pbtest::NoCancel none;
    e.sender->run(none);
    e.server.clear_log();
    const int64_t next = e.compare_ok();
    const auto next_entries = e.entries(next);
    const auto* a = find(next_entries, "a.epub");
    PB_CHECK(a != nullptr);
    PB_CHECK(a->action == PlanAction::Skip && a->reason == "kept_both");   // everything exists: only the mark is left
    e.run_ok(next);
    PB_CHECK(e.store->sync_record(e.pair, "a.epub")->diverged);
    PB_CHECK_EQ(content_gets(e), size_t(0));
    for (const char* m : {"PUT", "MKCOL"}) PB_CHECK_EQ(e.requests(m), size_t(0));
    size_t copies = 0;
    for (const std::string& f : e.reader_tree())
        if (f.find("(server copy") != std::string::npos) ++copies;
    PB_CHECK_EQ(copies, size_t(1));
}

PB_TEST_MAIN()
