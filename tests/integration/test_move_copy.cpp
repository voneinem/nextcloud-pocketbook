// Move and copy within the server (specs/002-manage-server-files US5, FR-029 to FR-033): nothing passes through the reader, every request carries the
// version of the source and Overwrite: F, a taken name at the destination asks, a changed or vanished source holds the item, and a confirmed move takes the
// reader's own records along. Fake Nextcloud.
#include <fstream>
#include <string>

#include "upload_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::UpEnv;
using pbc::AskChoice;
using pbc::ItemKind;

namespace {

void add_local_copy(UpEnv& e, const std::string& remote, const std::string& local_rel) {
    pbtest::up_mkdirs(e.download_root + "/" + local_rel.substr(0, local_rel.rfind('/')));
    {
        std::ofstream f(e.download_root + "/" + local_rel, std::ios::binary);
        f << "copy";
    }
    pbc::LocalFile lf;
    lf.remote_path = remote;
    lf.local_path = local_rel;
    lf.remote_etag = e.etag(remote);
    lf.size = 4;
    lf.mtime = 1790000000;
    lf.downloaded_at = e.clock.t;
    PB_CHECK(e.store->commit_download(lf, 0).ok());
}

UpEnv& two_folders(UpEnv& e) {
    e.server.add_folder("/Docs/src");
    e.server.add_folder("/Docs/dst");
    return e;
}

}  // namespace

PB_TEST(a_move_sends_one_move_with_destination_overwrite_f_and_the_sources_version_and_no_bytes_pass_the_reader) {
    UpEnv e;
    two_folders(e);
    e.server.add_file("/Docs/src/a.txt", "content");
    e.cache("/Docs");
    e.cache("/Docs/src");
    e.cache("/Docs/dst");
    const std::string etag = e.etag("/Docs/src/a.txt");
    const int64_t id = e.server.node("/Docs/src/a.txt").fileid;
    const pbc::AddResult r = e.actions().move_or_copy(ItemKind::Move, {"/Docs/src/a.txt"}, "/Docs/dst");
    PB_CHECK(r.error.ok());
    PB_CHECK_EQ(r.queued.size(), size_t(1));
    e.server.clear_log();
    const long long up = e.server.bytes_uploaded(), down = e.server.bytes_downloaded();
    PB_CHECK_EQ(e.run().done, 1);
    const auto moves = e.logged("MOVE");
    PB_CHECK_EQ(moves.size(), size_t(1));
    PB_CHECK_EQ(moves[0].overwrite, std::string("F"));
    PB_CHECK_EQ(moves[0].if_match, etag);
    PB_CHECK(moves[0].destination.find("/remote.php/dav/files/demo/Docs/dst/a.txt") != std::string::npos);
    PB_CHECK(!e.server.has("/Docs/src/a.txt"));
    PB_CHECK_EQ(e.server.node("/Docs/dst/a.txt").content, std::string("content"));
    PB_CHECK_EQ(e.server.node("/Docs/dst/a.txt").fileid, id);          // a move keeps the file id
    PB_CHECK_EQ(e.server.bytes_uploaded(), up);                          // nothing went through the reader
    PB_CHECK_EQ(e.server.bytes_downloaded(), down);
    PB_CHECK(!e.store->entry("/Docs/src/a.txt").has_value());
    PB_CHECK(e.store->entry("/Docs/dst/a.txt").has_value());
}

PB_TEST(a_copy_gets_a_new_file_id_and_leaves_the_source_alone_and_a_folder_copy_brings_its_content) {
    UpEnv e;
    two_folders(e);
    e.server.add_file("/Docs/src/a.txt", "content");
    e.server.add_folder("/Docs/src/sub");
    e.server.add_file("/Docs/src/sub/b.txt", "inner");
    e.cache("/Docs/src");
    e.cache("/Docs/dst");
    const auto r = e.actions().move_or_copy(ItemKind::Copy, {"/Docs/src/a.txt", "/Docs/src/sub"}, "/Docs/dst");
    PB_CHECK_EQ(r.queued.size(), size_t(2));
    PB_CHECK_EQ(e.run().done, 2);
    PB_CHECK(e.server.has("/Docs/src/a.txt"));
    PB_CHECK(e.server.has("/Docs/dst/a.txt"));
    PB_CHECK(e.server.node("/Docs/dst/a.txt").fileid != e.server.node("/Docs/src/a.txt").fileid);
    PB_CHECK_EQ(e.server.node("/Docs/dst/sub/b.txt").content, std::string("inner"));
    PB_CHECK(e.server.has("/Docs/src/sub/b.txt"));
    PB_CHECK_EQ(e.server.bytes_uploaded(), 0LL);
    const auto copy = e.store->entry("/Docs/dst/a.txt");
    PB_CHECK(copy.has_value());
    PB_CHECK(copy->fileid.has_value());
    PB_CHECK(copy->fileid != e.store->entry("/Docs/src/a.txt")->fileid);
}

PB_TEST(a_destination_that_is_the_source_inside_it_or_the_same_place_is_refused_before_queuing) {
    UpEnv e;
    e.server.add_folder("/Docs/a");
    e.server.add_folder("/Docs/a/b");
    e.server.add_file("/Docs/f.txt", "x");
    e.cache("/Docs");
    e.cache("/Docs/a");
    auto act = e.actions();
    auto key_of = [&](ItemKind k, const std::string& path, const std::string& target) {
        const pbc::AddResult r = act.move_or_copy(k, {path}, target);
        return r.refused.empty() ? std::string() : r.refused[0].key;
    };
    PB_CHECK_EQ(key_of(ItemKind::Move, "/Docs/a", "/Docs/a"), std::string("not_allowed"));     // into itself
    PB_CHECK_EQ(key_of(ItemKind::Move, "/Docs/a", "/Docs/a/b"), std::string("not_allowed"));   // inside it
    PB_CHECK_EQ(key_of(ItemKind::Copy, "/Docs/a", "/Docs/a/b"), std::string("not_allowed"));
    PB_CHECK_EQ(key_of(ItemKind::Move, "/Docs/f.txt", "/Docs"), std::string("name_exists"));    // the same place
    PB_CHECK_EQ(e.store->queue_count_open(), 0);
}

PB_TEST(an_entry_without_a_known_version_or_with_a_change_already_waiting_is_left_out_and_says_why) {
    UpEnv e;
    two_folders(e);
    e.server.add_file("/Docs/src/a.txt", "x");
    e.server.add_file("/Docs/src/w.txt", "x");
    e.server.set_weak_etag("/Docs/src/w.txt", true);
    e.cache("/Docs/src");
    e.cache("/Docs/dst");
    auto act = e.actions();
    PB_CHECK_EQ(act.rename_entry("/Docs/src/a.txt", "a2.txt").queued.size(), size_t(1));
    const auto r = act.move_or_copy(ItemKind::Move, {"/Docs/src/a.txt", "/Docs/src/w.txt", "/Docs/src/nope.txt"}, "/Docs/dst");
    PB_CHECK_EQ(r.queued.size(), size_t(0));
    PB_CHECK_EQ(r.refused.size(), size_t(3));
    PB_CHECK_EQ(r.refused[0].key, std::string("change_pending"));
    PB_CHECK_EQ(r.refused[1].key, std::string("refresh_first"));
    PB_CHECK_EQ(r.refused[2].key, std::string("refresh_first"));
}

PB_TEST(a_taken_name_at_the_destination_asks_keep_both_replace_for_files_or_skip) {
    UpEnv e;
    two_folders(e);
    e.server.add_file("/Docs/src/a.txt", "mine");
    e.server.add_file("/Docs/dst/a.txt", "theirs");
    e.server.add_folder("/Docs/src/d");
    e.server.add_folder("/Docs/dst/d");
    e.cache("/Docs/src");
    e.cache("/Docs/dst");
    auto act = e.actions();
    const auto r = act.move_or_copy(ItemKind::Move, {"/Docs/src/a.txt", "/Docs/src/d"}, "/Docs/dst");
    PB_CHECK_EQ(r.queued.size(), size_t(0));
    PB_CHECK_EQ(r.asks.size(), size_t(2));
    PB_CHECK(r.asks[0].replace_possible);
    PB_CHECK(!r.asks[1].replace_possible);                     // a folder is never replaced
    PB_CHECK_EQ(r.asks[0].suggested, std::string("a (2).txt"));
    // keep both: moved under the new name
    const auto kept = act.answer_ask(r.asks[0], AskChoice::KeepBoth);
    PB_CHECK_EQ(kept.queued.size(), size_t(1));
    PB_CHECK_EQ(e.item(kept.queued[0]).dst_name, std::string("a (2).txt"));
    PB_CHECK(e.item(kept.queued[0]).kind == ItemKind::Move);
    PB_CHECK_EQ(act.answer_ask(r.asks[1], AskChoice::Replace).error.key, std::string("not_allowed"));
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.server.node("/Docs/dst/a.txt").content, std::string("theirs"));
    PB_CHECK_EQ(e.server.node("/Docs/dst/a (2).txt").content, std::string("mine"));
}

PB_TEST(replace_by_move_deletes_the_old_version_into_the_trash_on_its_etag_and_never_overwrites_blindly) {
    UpEnv e;
    two_folders(e);
    e.server.add_file("/Docs/src/a.txt", "mine");
    e.server.add_file("/Docs/dst/a.txt", "theirs");
    e.cache("/Docs/src");
    e.cache("/Docs/dst");
    const std::string old_etag = e.etag("/Docs/dst/a.txt");
    auto act = e.actions();
    const auto r = act.move_or_copy(ItemKind::Move, {"/Docs/src/a.txt"}, "/Docs/dst");
    const auto q = act.answer_ask(r.asks[0], AskChoice::Replace);
    PB_CHECK_EQ(q.queued.size(), size_t(1));
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 1);
    const auto del = e.logged("DELETE");
    PB_CHECK_EQ(del.size(), size_t(1));
    PB_CHECK_EQ(del[0].if_match, old_etag);
    PB_CHECK(e.server.trash_has("/Docs/dst/a.txt"));
    PB_CHECK_EQ(e.logged("MOVE")[0].overwrite, std::string("F"));
    PB_CHECK_EQ(e.server.overwrites(), 0);
    PB_CHECK_EQ(e.server.node("/Docs/dst/a.txt").content, std::string("mine"));
    // a destination that changed after the reader saw it holds the item instead
    UpEnv f;
    two_folders(f);
    f.server.add_file("/Docs/src/a.txt", "mine");
    f.server.add_file("/Docs/dst/a.txt", "theirs");
    f.cache("/Docs/src");
    f.cache("/Docs/dst");
    auto act2 = f.actions();
    const auto r2 = act2.move_or_copy(ItemKind::Move, {"/Docs/src/a.txt"}, "/Docs/dst");
    const auto q2 = act2.answer_ask(r2.asks[0], AskChoice::Replace);
    f.server.set_file("/Docs/dst/a.txt", "theirs, changed", 1790000900);
    PB_CHECK_EQ(f.run().conflict, 1);
    PB_CHECK_EQ(f.item(q2.queued[0]).reason, std::string("changed_on_server"));
    PB_CHECK_EQ(f.server.node("/Docs/dst/a.txt").content, std::string("theirs, changed"));
    PB_CHECK(f.server.has("/Docs/src/a.txt"));
}

PB_TEST(a_source_that_changed_or_vanished_before_sending_is_held_and_not_applied) {
    UpEnv e;
    two_folders(e);
    e.server.add_file("/Docs/src/a.txt", "x");
    e.server.add_file("/Docs/src/b.txt", "x");
    e.cache("/Docs/src");
    e.cache("/Docs/dst");
    const auto r = e.actions().move_or_copy(ItemKind::Move, {"/Docs/src/a.txt", "/Docs/src/b.txt"}, "/Docs/dst");
    e.server.set_file("/Docs/src/a.txt", "changed", 1790000500);
    e.server.remove("/Docs/src/b.txt");
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.conflict, 1);
    PB_CHECK_EQ(rep.failed, 1);
    PB_CHECK_EQ(e.item(r.queued[0]).reason, std::string("changed_on_server"));
    PB_CHECK_EQ(e.item(r.queued[1]).reason, std::string("source_gone"));
    PB_CHECK(e.server.has("/Docs/src/a.txt"));
    PB_CHECK(!e.server.has("/Docs/dst/a.txt"));
}

PB_TEST(apply_anyway_after_a_change_uses_the_version_the_user_was_shown) {
    UpEnv e;
    two_folders(e);
    e.server.add_file("/Docs/src/a.txt", "x");
    e.cache("/Docs/src");
    e.cache("/Docs/dst");
    const auto r = e.actions().move_or_copy(ItemKind::Move, {"/Docs/src/a.txt"}, "/Docs/dst");
    e.server.set_file("/Docs/src/a.txt", "changed", 1790000500);
    PB_CHECK_EQ(e.run().conflict, 1);
    auto s = e.sender();
    const auto shown = s->refresh_source(r.queued[0]);
    PB_CHECK(shown.ok() && shown.value.has_value());
    PB_CHECK(s->resolve_apply_anyway(r.queued[0], shown.value->etag).ok());
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.server.node("/Docs/dst/a.txt").content, std::string("changed"));
}

PB_TEST(a_full_destination_leaves_the_source_unchanged_and_says_storage_full) {
    UpEnv e;
    two_folders(e);
    e.server.add_synthetic_file("/Docs/src/big.bin", 3 << 20);
    e.cache("/Docs/src");
    e.cache("/Docs/dst");
    e.server.set_quota_limit(5 << 20);   // the copy would need 3 MiB more than is free
    const auto r = e.actions().move_or_copy(ItemKind::Copy, {"/Docs/src/big.bin"}, "/Docs/dst");
    PB_CHECK_EQ(e.run().failed, 1);
    PB_CHECK_EQ(e.item(r.queued[0]).reason, std::string("server_storage_full"));
    PB_CHECK(e.server.has("/Docs/src/big.bin"));
    PB_CHECK(!e.server.has("/Docs/dst/big.bin"));
    PB_CHECK(e.store->entry("/Docs/dst/big.bin") == std::nullopt);
}

PB_TEST(member_failures_of_a_folder_copy_end_as_partial_failure_and_the_cache_is_not_patched) {
    UpEnv e;
    two_folders(e);
    e.server.add_folder("/Docs/src/sub");
    e.server.add_file("/Docs/src/sub/locked.txt", "x");
    e.server.lock("/Docs/src/sub/locked.txt");
    e.cache("/Docs/src");
    e.cache("/Docs/dst");
    const auto r = e.actions().move_or_copy(ItemKind::Copy, {"/Docs/src/sub"}, "/Docs/dst");
    PB_CHECK_EQ(e.run().failed, 1);
    PB_CHECK_EQ(e.item(r.queued[0]).reason, std::string("partial_failure"));
    PB_CHECK(!e.store->entry("/Docs/dst/sub").has_value());
}

PB_TEST(a_confirmed_move_takes_the_copy_on_the_reader_and_the_offline_mark_along_and_a_copy_creates_none) {
    UpEnv e;
    two_folders(e);
    e.server.add_file("/Docs/src/a.txt", "content");
    e.server.add_file("/Docs/src/c.txt", "content");
    e.cache("/Docs/src");
    e.cache("/Docs/dst");
    add_local_copy(e, "/Docs/src/a.txt", "Docs/src/a.txt");
    PB_CHECK(e.store->set_mark("/Docs/src/a.txt", pbc::Kind::File, true).ok());
    e.actions().move_or_copy(ItemKind::Move, {"/Docs/src/a.txt"}, "/Docs/dst");
    e.actions().move_or_copy(ItemKind::Copy, {"/Docs/src/c.txt"}, "/Docs/dst");
    PB_CHECK_EQ(e.run().done, 2);
    const auto copy = e.store->local_file("/Docs/dst/a.txt");
    PB_CHECK(copy.has_value());
    PB_CHECK_EQ(copy->local_path, std::string("Docs/src/a.txt"));   // the file on the card is not moved
    PB_CHECK(::access((e.download_root + "/Docs/src/a.txt").c_str(), R_OK) == 0);
    PB_CHECK(e.store->is_marked("/Docs/dst/a.txt"));
    PB_CHECK(!e.store->is_marked("/Docs/src/a.txt"));
    PB_CHECK(!e.store->local_file("/Docs/dst/c.txt").has_value());   // a copy creates no local copy and no mark
    PB_CHECK(!e.store->is_marked("/Docs/dst/c.txt"));
}

PB_TEST(moving_a_folder_moves_the_records_below_it_and_a_lost_answer_is_unknown_then_found_by_the_file_id) {
    UpEnv e;
    two_folders(e);
    e.server.add_folder("/Docs/src/proj");
    e.server.add_file("/Docs/src/proj/x.txt", "x");
    e.cache("/Docs/src");
    e.cache("/Docs/dst");
    add_local_copy(e, "/Docs/src/proj/x.txt", "Docs/src/proj/x.txt");
    const auto r = e.actions().move_or_copy(ItemKind::Move, {"/Docs/src/proj"}, "/Docs/dst");
    e.server.lose_next_answer("MOVE");
    PB_CHECK_EQ(e.run().unknown, 1);
    PB_CHECK(e.sender()->check_unknown(r.queued[0]) == pbc::Found::Arrived);
    PB_CHECK(e.sender()->mark_unknown_done(r.queued[0]).ok());
    PB_CHECK(e.store->local_file("/Docs/dst/proj/x.txt").has_value());
    PB_CHECK(!e.store->local_file("/Docs/src/proj/x.txt").has_value());
    PB_CHECK(e.store->entry("/Docs/dst/proj").has_value());
}

PB_TEST(what_the_reader_put_into_a_folder_makes_the_cached_version_of_that_folder_unknown_so_it_asks_for_a_refresh_instead_of_a_false_conflict) {
    UpEnv e;
    e.server.add_folder("/Docs/dst");
    e.server.add_folder("/Docs/proj");
    e.cache("/Docs");
    e.queue({e.source("Downloads/a.txt", 10)}, "/Docs/proj");
    PB_CHECK_EQ(e.run().done, 1);   // the server's version of /Docs/proj changed with this upload
    PB_CHECK(e.store->entry("/Docs/proj")->etag.empty());
    auto act = e.actions();
    const auto before = act.move_or_copy(ItemKind::Copy, {"/Docs/proj"}, "/Docs/dst");
    PB_CHECK_EQ(before.refused.size(), size_t(1));
    PB_CHECK_EQ(before.refused[0].key, std::string("refresh_first"));
    e.cache("/Docs");   // the refresh
    const auto after = act.move_or_copy(ItemKind::Copy, {"/Docs/proj"}, "/Docs/dst");
    PB_CHECK_EQ(after.queued.size(), size_t(1));
    PB_CHECK_EQ(e.run().done, 1);
}

PB_TEST(a_lost_answer_of_a_copy_is_unknown_found_by_the_check_and_sending_again_ends_in_the_collision_question_not_in_a_second_copy) {
    UpEnv e;
    two_folders(e);
    e.server.add_file("/Docs/src/a.txt", "content");
    e.cache("/Docs/src");
    e.cache("/Docs/dst");
    const auto r = e.actions().move_or_copy(ItemKind::Copy, {"/Docs/src/a.txt"}, "/Docs/dst");
    e.server.lose_next_answer("COPY");
    e.server.clear_log();
    PB_CHECK_EQ(e.run().unknown, 1);
    PB_CHECK_EQ(e.logged("COPY").size(), size_t(1));
    PB_CHECK(e.item(r.queued[0]).state == pbc::ItemState::Unknown);
    PB_CHECK(e.sender()->check_unknown(r.queued[0]) == pbc::Found::Arrived);   // a copy has an id of its own: existence and kind are what can be seen
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 0);                                             // neither Send nor Auto Sync takes it again
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(e.sender()->send_again(r.queued[0]).ok());
    PB_CHECK_EQ(e.run().conflict, 1);
    PB_CHECK_EQ(e.item(r.queued[0]).reason, std::string("name_exists"));
    PB_CHECK(!e.server.has("/Docs/dst/a (2).txt"));
    PB_CHECK_EQ(e.server.overwrites(), 0);
}

PB_TEST_MAIN()
