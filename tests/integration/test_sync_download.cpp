// A download-only sync from compare to result (specs/003-folder-sync US3), against the fake Nextcloud: the first sync copies a tree, the second one finds
// nothing to do, a change and a new file are the only things fetched, what exists only on the reader is never sent and never touched, and the problems of a
// run (an unavailable entry, a full card, a cancel, a change between compare and download) end the entry or the run the way the plan says.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <openssl/evp.h>

#include "sync_env.h"

PBC_FAKE_SERVER_TEST();

using pbc::PlanAction;

using pbtest::SyncEnv;
using pbtest::find;
using pbtest::pattern;
using pbtest::tree;

PB_TEST(the_first_sync_copies_a_tree_of_100_files_over_5_levels_and_never_shows_a_partial_file) {
    SyncEnv e;
    tree(e, 100, 5);
    std::vector<std::string> server_files;
    for (int i = 0; i < 100; ++i) {
        std::string rel;
        for (int l = 0; l < 5; ++l) rel += "L" + std::to_string(l) + "_" + std::to_string(i % (l + 2)) + "/";
        server_files.push_back(rel + "file" + std::to_string(1000 + i) + ".txt");
    }
    e.watch_partial = true;
    const int64_t plan = e.compare_ok();
    PB_CHECK_EQ(e.plan(plan).totals.n_download, 100);
    PB_CHECK_EQ(e.plan(plan).state, std::string("ready"));
    PB_CHECK(e.reader_tree().empty());   // a comparison changes nothing on the card
    PB_CHECK_EQ(e.requests("GET"), size_t(0));
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.downloaded, 100);
    PB_CHECK_EQ(result.failed, 0);
    PB_CHECK_EQ(result.stopped_by_key, std::string(""));
    for (int i = 0; i < 100; ++i) PB_CHECK(e.rd(server_files[static_cast<size_t>(i)]) == pattern(100 + static_cast<size_t>(i), i));
    PB_CHECK(!e.part_file());
    PB_CHECK(!e.target_visible_before_rename);   // the file got its name only when it was complete
    PB_CHECK_EQ(e.store->sync_records(e.pair).size(), size_t(100));
    PB_CHECK(e.store->journal().empty());
    PB_CHECK_EQ(e.store->sync_plan(plan)->state, std::string("done"));
    PB_CHECK_EQ(e.store->sync_pair(e.pair)->state, std::string("idle"));
    PB_CHECK_EQ(e.store->sync_pair(e.pair)->last_outcome, std::string("done"));
    // the hooks in their order
    PB_CHECK(!e.steps.empty());
    PB_CHECK_EQ(e.steps.front(), std::string("plan_written"));
    PB_CHECK_EQ(e.steps.back(), std::string("result_written"));
    PB_CHECK(std::find(e.steps.begin(), e.steps.end(), "approved") != e.steps.end());
    PB_CHECK(std::find(e.steps.begin(), e.steps.end(), "before_download") != e.steps.end());
    PB_CHECK(std::find(e.steps.begin(), e.steps.end(), "record_committed") != e.steps.end());
}

PB_TEST(an_immediate_second_compare_plans_nothing) {
    SyncEnv e;
    tree(e, 30, 3);
    e.sync();
    e.server.clear_log();
    const int64_t plan = e.compare_ok();
    PB_CHECK_EQ(e.plan(plan).totals.n_download, 0);
    PB_CHECK_EQ(e.plan(plan).totals.n_upload, 0);
    PB_CHECK_EQ(e.plan(plan).totals.n_in_sync, 30);
    PB_CHECK(e.entries(plan).empty());
    PB_CHECK_EQ(e.requests("GET"), size_t(0));   // not a byte of content was asked for
    PB_CHECK_EQ(e.store->sync_pair(e.pair)->state, std::string("idle"));   // nothing to approve
}

PB_TEST(a_changed_file_and_a_new_file_are_the_only_ones_fetched_the_next_time) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(1000, 1));
    e.server.add_file("/Books/Sub/b.epub", pattern(2000, 2));
    e.server.add_file("/Books/Sub/c.epub", pattern(3000, 3));
    e.sync();
    e.server.touch_file("/Books/Sub/b.epub", pattern(2500, 9), 1790009999);
    e.server.add_file("/Books/Sub/new.epub", pattern(10, 4));
    e.server.clear_log();
    const int64_t plan = e.compare_ok();
    const auto es = e.entries(plan);
    PB_CHECK_EQ(es.size(), size_t(2));
    PB_CHECK(find(es, "Sub/b.epub")->action == PlanAction::Download);
    PB_CHECK(find(es, "Sub/new.epub")->action == PlanAction::Download);
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.downloaded, 2);
    PB_CHECK_EQ(e.requests("GET"), size_t(2));
    PB_CHECK(e.rd("Sub/b.epub") == pattern(2500, 9));
    PB_CHECK(e.rd("Sub/new.epub") == pattern(10, 4));
    PB_CHECK(e.rd("a.epub") == pattern(1000, 1));
    PB_CHECK_EQ(e.store->sync_record(e.pair, "Sub/b.epub")->server_size.value_or(-1), int64_t(2500));
}

PB_TEST(a_file_only_on_the_reader_is_never_sent_never_offered_for_deletion_and_never_touched) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(100));
    e.write_reader("mine.epub", "only here");
    e.write_reader("Folder/also-mine.txt", "also only here");
    const int64_t plan = e.compare_ok();
    const auto es = e.entries(plan);
    PB_CHECK_EQ(es.size(), size_t(1));   // only the download
    for (const auto& en : es) {
        PB_CHECK(en.action == PlanAction::Download);
    }
    PB_CHECK_EQ(e.plan(plan).totals.n_prop_reader + e.plan(plan).totals.n_prop_server, 0);
    e.run_ok(plan);
    PB_CHECK_EQ(e.rd("mine.epub"), std::string("only here"));
    PB_CHECK_EQ(e.rd("Folder/also-mine.txt"), std::string("also only here"));
    // and again after the sync, still nothing about them
    const int64_t again = e.compare_ok();
    PB_CHECK(e.entries(again).empty());
    for (const char* m : {"PUT", "MKCOL", "MOVE", "COPY", "DELETE"}) PB_CHECK_EQ(e.requests(m), size_t(0));
}

PB_TEST(unavailable_entries_are_skipped_with_their_reason_and_do_not_stop_the_rest) {
    SyncEnv e;
    e.server.add_file("/Books/ok.epub", pattern(100));
    e.server.add_file("/Books/secret.epub", pattern(100));
    e.server.add_file("/Books/Vault/inside.epub", pattern(100));
    e.server.add_file("/Books/z-after.epub", pattern(100));
    e.server.set_unavailable("/Books/secret.epub", "no_permission");
    e.server.set_unavailable("/Books/Vault", "encrypted");
    const int64_t plan = e.compare_ok();
    const auto es = e.entries(plan);
    PB_CHECK_EQ(find(es, "secret.epub")->reason, std::string("unavailable"));
    PB_CHECK_EQ(find(es, "Vault")->reason, std::string("unavailable"));
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.downloaded, 2);
    PB_CHECK(result.skipped >= 2);
    PB_CHECK(e.has("ok.epub") && e.has("z-after.epub"));
    PB_CHECK(!e.has("secret.epub") && !e.has("Vault"));
}

PB_TEST(a_file_larger_than_the_free_space_is_a_per_file_refusal_and_the_run_goes_on) {
    SyncEnv e;
    e.server.add_file("/Books/a-small.epub", pattern(1000));
    e.server.add_file("/Books/b-big.epub", pattern(4000));
    e.server.add_file("/Books/c-small.epub", pattern(1000));
    const int64_t plan = e.compare_ok();
    PB_CHECK_EQ(e.plan(plan).unapprovable, 0);
    // by the time of the run the card has less room: the big file no longer fits above the reserve (256 MiB on a 1 GiB volume), the small ones do
    e.storage.total = 1ll << 30;
    e.on_step = [&](const char* step) {
        if (std::string(step) == "approved") e.storage.free = (256ll << 20) + 2000;   // 1,000 bytes fit, 4,000 do not
    };
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.downloaded, 2);
    PB_CHECK_EQ(result.stopped_by_key, std::string(""));
    PB_CHECK_EQ(find(e.entries(plan), "b-big.epub")->state, std::string("skipped"));
    PB_CHECK_EQ(find(e.entries(plan), "b-big.epub")->result_key, std::string("no_space"));
    PB_CHECK(e.has("a-small.epub") && e.has("c-small.epub") && !e.has("b-big.epub"));
    PB_CHECK(!e.store->sync_record(e.pair, "b-big.epub").has_value());
}

PB_TEST(a_card_that_fills_up_during_a_download_stops_the_run_safely) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(1000));
    e.server.add_file("/Books/b.epub", pattern(1000));
    e.server.add_file("/Books/c.epub", pattern(1000));
    const int64_t plan = e.compare_ok();
    int downloads = 0;
    e.on_step = [&](const char* step) {
        if (std::string(step) == "before_download" && ++downloads == 2) e.server.transport().simulate_storage_full = true;
    };
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.downloaded, 1);
    PB_CHECK_EQ(result.stopped_by_key, std::string("storage_full"));
    PB_CHECK_EQ(downloads, 2);   // the third download was never started
    PB_CHECK(e.has("a.epub") && !e.has("b.epub") && !e.has("c.epub"));
    PB_CHECK(!e.part_file());
    PB_CHECK(!e.store->sync_record(e.pair, "b.epub").has_value());
    PB_CHECK(e.store->journal().empty());
    PB_CHECK_EQ(e.store->sync_plan(plan)->state, std::string("interrupted"));
    PB_CHECK_EQ(e.store->sync_pair(e.pair)->state, std::string("interrupted"));
    // the card is fine again: the next sync finishes the job with the same final state as an uninterrupted run
    e.server.transport().simulate_storage_full = false;
    e.on_step = nullptr;
    const auto again = e.sync();
    PB_CHECK_EQ(again.downloaded, 2);
    PB_CHECK_EQ(e.store->sync_records(e.pair).size(), size_t(3));
}

PB_TEST(cancel_stops_after_the_item_in_flight_and_what_is_done_stays_done) {
    SyncEnv e;
    for (int i = 0; i < 5; ++i) e.server.add_file("/Books/f" + std::to_string(i) + ".epub", pattern(500, i));
    const int64_t plan = e.compare_ok();
    pbtest::CancelFlag cancel;
    int downloaded = 0;
    e.on_step = [&](const char* step) {
        if (std::string(step) == "downloaded" && ++downloaded == 2) cancel.flag = true;
    };
    const auto r = e.run(plan, cancel);
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.downloaded, 2);
    PB_CHECK_EQ(r.value.stopped_by_key, std::string("cancelled"));
    PB_CHECK_EQ(e.store->sync_plan(plan)->state, std::string("interrupted"));
    PB_CHECK_EQ(e.store->sync_pair(e.pair)->state, std::string("interrupted"));
    PB_CHECK_EQ(e.store->sync_records(e.pair).size(), size_t(2));
    PB_CHECK(!e.part_file());
    // entries that were not reached stay planned
    size_t planned = 0;
    for (const auto& en : e.entries(plan))
        if (en.state == "planned") ++planned;
    PB_CHECK_EQ(planned, size_t(3));
}

PB_TEST(a_file_that_changes_between_compare_and_download_is_skipped_and_its_record_is_untouched) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(1000, 1));
    e.server.add_file("/Books/b.epub", pattern(1000, 2));
    e.sync();
    e.server.touch_file("/Books/a.epub", pattern(1200, 3), 1790005000);
    e.server.touch_file("/Books/b.epub", pattern(1200, 4), 1790005001);
    const int64_t plan = e.compare_ok();
    const auto old_record = e.store->sync_record(e.pair, "a.epub");
    e.server.touch_file("/Books/a.epub", pattern(1300, 5), 1790006000);   // after the compare, before the download
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.downloaded, 1);
    PB_CHECK(result.skipped >= 1);
    PB_CHECK_EQ(find(e.entries(plan), "a.epub")->state, std::string("skipped"));
    PB_CHECK_EQ(find(e.entries(plan), "a.epub")->result_key, std::string("changed_during_sync"));
    PB_CHECK(e.rd("a.epub") == pattern(1000, 1));   // the old copy is still there
    PB_CHECK_EQ(e.store->sync_record(e.pair, "a.epub")->server_etag, old_record->server_etag);
    PB_CHECK(e.rd("b.epub") == pattern(1200, 4));
    PB_CHECK(!e.part_file());
    // the next compare lists it again
    const int64_t next = e.compare_ok();
    PB_CHECK(find(e.entries(next), "a.epub") != nullptr);
}

PB_TEST(a_comparison_changes_no_file_no_record_and_sends_no_write_request) {
    SyncEnv e;
    tree(e, 10, 2);
    e.write_reader("mine.txt", "mine");
    const auto before = e.reader_tree();
    e.compare_ok();
    e.compare_ok();
    PB_CHECK(e.reader_tree() == before);
    PB_CHECK(e.store->sync_records(e.pair).empty());
    PB_CHECK_EQ(e.store->queue_count_open(), 0);
    PB_CHECK_EQ(e.requests("GET"), size_t(0));
    for (const char* m : {"PUT", "MKCOL", "MOVE", "COPY", "DELETE"}) PB_CHECK_EQ(e.requests(m), size_t(0));
    PB_CHECK(e.store->journal().empty());
}

PB_TEST(a_new_comparison_replaces_the_ready_plan_and_a_busy_pair_refuses_a_second_one) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(100));
    const int64_t first = e.compare_ok();
    const int64_t second = e.compare_ok();
    PB_CHECK(second != first);
    PB_CHECK_EQ(e.store->sync_plan(first)->state, std::string("superseded"));
    PB_CHECK_EQ(e.store->sync_plan(second)->state, std::string("ready"));
    PB_CHECK_EQ(e.store->sync_pair(e.pair)->state, std::string("plan_ready"));
    PB_CHECK(e.store->sync_pair_lock(e.pair, "comparing").ok());   // another comparison or run holds the pair
    const auto busy = e.compare();
    PB_CHECK(!busy.ok());
    PB_CHECK_EQ(busy.error.key, std::string("sync_busy"));
    const auto run_busy = e.run(second);
    PB_CHECK(!run_busy.ok());
    PB_CHECK_EQ(run_busy.error.key, std::string("sync_busy"));
    PB_CHECK(e.store->sync_pair_unlock(e.pair, "idle", "").ok());
}

PB_TEST(a_missing_server_folder_or_reader_folder_gives_the_pair_a_problem_and_no_plan) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(100));
    e.server.remove("/Books");
    auto r = e.compare();
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("server_folder_missing"));
    PB_CHECK_EQ(e.store->sync_pair(e.pair)->problem_key, std::string("server_folder_missing"));
    PB_CHECK_EQ(e.store->sync_pair(e.pair)->state, std::string("idle"));
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM sync_plans"), std::string("0"));
    // the folder comes back: the problem is gone with the next successful comparison
    e.server.add_file("/Books/a.epub", pattern(100));
    PB_CHECK(e.compare().ok());
    PB_CHECK_EQ(e.store->sync_pair(e.pair)->problem_key, std::string(""));
    // the reader folder is gone (the card was taken out)
    ::rmdir(e.reader.c_str());
    r = e.compare();
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("reader_folder_missing"));
    PB_CHECK_EQ(e.store->sync_pair(e.pair)->problem_key, std::string("reader_folder_missing"));
}

PB_TEST(a_refused_login_stops_the_comparison_with_the_general_key_and_nothing_is_planned) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(100));
    e.server.set_account("demo", "demo", "another-password");   // the server no longer accepts the saved app password
    const auto r = e.compare();
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("auth_failed"));
    PB_CHECK_EQ(e.store->sync_pair(e.pair)->state, std::string("idle"));
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM sync_plans"), std::string("0"));
}

PB_TEST(a_pair_of_another_account_is_neither_compared_nor_run_and_no_request_is_made) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(100));
    const int64_t plan = e.compare_ok();
    e.server.clear_log();
    pbc::AccountRow other;
    other.base_url = e.server.base_url();
    other.login_name = "someone";
    other.user_id = "someone";
    PB_CHECK(e.store->save_account(other).ok());
    const auto c = e.compare();
    PB_CHECK(!c.ok());
    PB_CHECK_EQ(c.error.key, std::string("account_changed"));
    const auto r = e.run(plan);
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("account_changed"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(e.reader_tree().empty());
}

PB_TEST(the_result_is_stored_with_the_pair_and_the_plan_counts_agree) {
    SyncEnv e;
    e.server.add_file("/Books/a.epub", pattern(100));
    e.server.add_file("/Books/b.epub", pattern(200));
    const auto result = e.sync();
    PB_CHECK_EQ(result.downloaded, 2);
    const auto pair = *e.store->sync_pair(e.pair);
    PB_CHECK(pair.last_sync_at.has_value());
    PB_CHECK(pair.last_result_json.find("\"downloaded\":2") != std::string::npos);
    PB_CHECK(pair.last_result_json.find("\"failed\":0") != std::string::npos);
    PB_CHECK(pair.held_files.has_value());
}

PB_TEST(a_file_that_is_already_on_both_sides_with_the_same_content_is_remembered_not_downloaded) {
    SyncEnv e;
    const std::string same = pattern(2000, 3);
    e.server.add_file("/Books/same.epub", same);
    e.server.add_file("/Books/different.epub", pattern(2000, 4));
    e.server.add_file("/Books/no-checksum.epub", pattern(2000, 5));
    e.write_reader("same.epub", same);                         // equal content
    e.write_reader("different.epub", pattern(2000, 99));       // equal size, other bytes
    e.write_reader("no-checksum.epub", pattern(2000, 5));
    // the server lists no checksum for no-checksum.epub: equal size is not enough
    auto sha256_hex = [](const std::string& content) {   // the SHA-256 the server would list
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        EVP_Digest(content.data(), content.size(), md, &len, EVP_sha256(), nullptr);
        static const char* hex = "0123456789abcdef";
        std::string out;
        for (unsigned int i = 0; i < len; ++i) { out += hex[md[i] >> 4]; out += hex[md[i] & 15]; }
        return out;
    };
    e.server.set_checksums("/Books/same.epub", sha256_hex(same));
    e.server.set_checksums("/Books/different.epub", sha256_hex(pattern(2000, 4)));   // a listed checksum that is not the reader file's
    const int64_t plan = e.compare_ok();
    const auto es = e.entries(plan);
    PB_CHECK_EQ(find(es, "same.epub")->reason, std::string("identical"));
    PB_CHECK(find(es, "different.epub")->action == PlanAction::KeepBoth);       // a clash: both versions are kept
    PB_CHECK(find(es, "no-checksum.epub")->action == PlanAction::KeepBoth);     // equal size is not enough
    e.server.clear_log();
    const auto result = e.run_ok(plan);
    PB_CHECK_EQ(result.downloaded, 0);
    PB_CHECK_EQ(result.kept_both, 2);
    PB_CHECK_EQ(e.requests("GET"), size_t(2));                  // only the two server versions that are added as copies; the identical file is not fetched
    PB_CHECK(e.store->sync_record(e.pair, "same.epub").has_value());
    PB_CHECK_EQ(e.store->sync_record(e.pair, "same.epub")->origin, std::string("identical"));
    PB_CHECK(e.store->sync_record(e.pair, "different.epub")->diverged);
    PB_CHECK_EQ(e.rd("different.epub"), pattern(2000, 99));      // never overwritten
    PB_CHECK_EQ(e.rd(find(es, "different.epub")->conflict_name), pattern(2000, 4));   // the server's version beside it
    // the next plan calls the identical one in sync
    const int64_t next = e.compare_ok();
    PB_CHECK(find(e.entries(next), "same.epub") == nullptr);
    PB_CHECK_EQ(e.plan(next).totals.n_in_sync, 1);
    PB_CHECK_EQ(e.plan(next).totals.n_download + e.plan(next).totals.n_keep_both, 0);   // the clashes are kept apart, not made again
}

PB_TEST_MAIN()
