// New folder and Rename (specs/002-manage-server-files US4, FR-026 to FR-028): queued at once, validated against the server's rules before queuing,
// sent with their conditions, and - when the server confirms - reflected in the reader's own records (the cached listing, the downloaded copies and
// the offline marks follow a renamed entry). Fake Nextcloud.
#include <fstream>
#include <string>

#include "upload_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::UpEnv;

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

}  // namespace

PB_TEST(a_new_folder_is_created_with_mkcol_and_enters_the_cache_with_its_version_and_id) {
    UpEnv e;
    e.cache("/Docs");
    const pbc::AddResult r = e.actions().new_folder("/Docs", "Trips");
    PB_CHECK(r.error.ok());
    PB_CHECK_EQ(r.queued.size(), size_t(1));
    PB_CHECK(e.item(r.queued[0]).kind == pbc::ItemKind::Mkdir);
    const auto pending = e.store->entries_page_overlay("/Docs", pbc::SortKey::Name, true, 0, 10);
    PB_CHECK_EQ(pending.total, 1);
    PB_CHECK_EQ(pending.rows[0].pending, std::string("new"));      // shown at once, marked not synced
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK(e.server.node("/Docs/Trips").folder);
    const auto entry = e.store->entry("/Docs/Trips");
    PB_CHECK(entry.has_value());
    PB_CHECK(entry->kind == pbc::Kind::Folder);
    PB_CHECK(!entry->etag.empty());
    PB_CHECK(entry->fileid.has_value());
    const auto page = e.store->entries_page_overlay("/Docs", pbc::SortKey::Name, true, 0, 10);
    PB_CHECK_EQ(page.total, 1);
    PB_CHECK(page.rows[0].pending.empty());                        // the mark went with the confirmation
}

PB_TEST(a_name_that_exists_queues_nothing_and_a_name_the_server_forbids_is_refused_at_once) {
    UpEnv e;
    e.server.add_folder("/Docs/Trips");
    e.server.add_file("/Docs/a.txt", "x");
    e.cache("/Docs");
    const pbc::AddResult exists = e.actions().new_folder("/Docs", "Trips");
    PB_CHECK_EQ(exists.error.key, std::string("name_exists"));
    PB_CHECK_EQ(exists.queued.size(), size_t(0));
    PB_CHECK_EQ(e.actions().new_folder("/Docs", "a.txt").error.key, std::string("name_exists"));
    pbc::ServerCaps caps = *e.store->server_caps();
    caps.forbidden_chars = "?<>:*|\"";
    caps.forbidden_names = {".htaccess"};
    caps.forbidden_extensions = {".part"};
    PB_CHECK(e.store->save_server_caps(caps).ok());
    for (const char* bad : {"", ".", "..", "a/b", "back\\slash", "ends with dot.", "what?", ".htaccess", "x.part", "tab\there", "   "})
        PB_CHECK_EQ(e.actions().new_folder("/Docs", bad).error.key, std::string("name_forbidden"));
    PB_CHECK_EQ(e.store->queue_count_open(), 0);
    // a name that is queued but not sent yet is taken as well
    PB_CHECK(e.actions().new_folder("/Docs", "Fresh").error.ok());
    PB_CHECK_EQ(e.actions().new_folder("/Docs", "Fresh").error.key, std::string("name_exists"));
}

PB_TEST(an_upload_into_a_folder_that_is_still_queued_waits_for_it_and_follows_it) {
    UpEnv e;
    e.cache("/Docs");
    const auto folder = e.actions().new_folder("/Docs", "Trips");
    const pbc::AddResult up = e.actions().add_uploads("/Docs/Trips", {e.source("Downloads/a.txt", 100)});
    PB_CHECK_EQ(up.queued.size(), size_t(1));
    PB_CHECK_EQ(e.item(up.queued[0]).depends_on.value_or(-1), folder.queued[0]);
    PB_CHECK_EQ(e.store->queue_count_sendable(e.server.base_url(), "demo"), 1);   // only the folder
    PB_CHECK_EQ(e.run().done, 2);
    PB_CHECK_EQ(e.server.node("/Docs/Trips/a.txt").content.size(), size_t(100));
}

PB_TEST(an_upload_into_a_folder_whose_mkcol_fails_stays_queued_and_waiting) {
    UpEnv e;
    e.cache("/Docs");
    e.actions().new_folder("/Docs/Missing", "Trips");   // the parent does not exist on the server
    const pbc::AddResult up = e.actions().add_uploads("/Docs/Missing/Trips", {e.source("Downloads/a.txt", 100)});
    e.server.clear_log();
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.failed, 1);
    PB_CHECK_EQ(rep.left, 0);
    PB_CHECK(e.item(up.queued[0]).state == pbc::ItemState::Queued);
    PB_CHECK_EQ(e.logged("PUT").size(), size_t(0));
}

PB_TEST(a_folder_that_appeared_on_the_server_meanwhile_is_a_conflict_name_exists) {
    UpEnv e;
    e.cache("/Docs");
    const auto r = e.actions().new_folder("/Docs", "Trips");
    e.server.add_folder("/Docs/Trips");
    PB_CHECK_EQ(e.run().conflict, 1);
    PB_CHECK_EQ(e.item(r.queued[0]).reason, std::string("name_exists"));
}

PB_TEST(rename_is_a_move_in_the_same_folder_with_the_destination_overwrite_f_and_the_entrys_version) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "content");
    e.cache("/Docs");
    const std::string etag = e.etag("/Docs/a.txt");
    const std::string old_id = std::to_string(e.server.node("/Docs/a.txt").fileid);
    const pbc::AddResult r = e.actions().rename_entry("/Docs/a.txt", "b.txt");
    PB_CHECK(r.error.ok());
    PB_CHECK_EQ(r.queued.size(), size_t(1));
    // the listing shows the new name under the old identity until the server confirms
    const auto page = e.store->entries_page_overlay("/Docs", pbc::SortKey::Name, true, 0, 10);
    PB_CHECK_EQ(page.rows[0].entry.name, std::string("b.txt"));
    PB_CHECK_EQ(page.rows[0].pending, std::string("renamed"));
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 1);
    const auto moves = e.logged("MOVE");
    PB_CHECK_EQ(moves.size(), size_t(1));
    PB_CHECK_EQ(moves[0].overwrite, std::string("F"));
    PB_CHECK_EQ(moves[0].if_match, etag);
    PB_CHECK(moves[0].destination.find("/remote.php/dav/files/demo/Docs/b.txt") != std::string::npos);
    PB_CHECK(!e.server.has("/Docs/a.txt"));
    PB_CHECK_EQ(e.server.node("/Docs/b.txt").content, std::string("content"));
    PB_CHECK_EQ(std::to_string(e.server.node("/Docs/b.txt").fileid), old_id);   // the same entry: its file id is kept
    PB_CHECK(!e.store->entry("/Docs/a.txt").has_value());
    const auto moved = e.store->entry("/Docs/b.txt");
    PB_CHECK(moved.has_value());
    PB_CHECK_EQ(moved->etag, e.etag("/Docs/b.txt"));
}

PB_TEST(a_case_only_rename_is_a_different_name_and_not_a_collision) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "content");
    e.cache("/Docs");
    const pbc::AddResult r = e.actions().rename_entry("/Docs/a.txt", "A.txt");
    PB_CHECK(r.error.ok());
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK(e.server.has("/Docs/A.txt"));
    PB_CHECK(!e.server.has("/Docs/a.txt"));
}

PB_TEST(a_rename_is_refused_for_the_same_name_a_taken_name_a_bad_name_or_an_unknown_version) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "content");
    e.server.add_file("/Docs/b.txt", "content");
    e.server.add_file("/Docs/w.txt", "content");
    e.server.set_weak_etag("/Docs/w.txt", true);
    e.cache("/Docs");
    PB_CHECK_EQ(e.actions().rename_entry("/Docs/a.txt", "a.txt").error.key, std::string("name_exists"));
    PB_CHECK(e.actions().rename_entry("/Docs/a.txt", "b.txt").error.ok());   // a taken name is a question now, see below
    PB_CHECK_EQ(e.actions().rename_entry("/Docs/a.txt", "x/y").error.key, std::string("name_forbidden"));
    PB_CHECK_EQ(e.actions().rename_entry("/Docs/w.txt", "z.txt").error.key, std::string("refresh_first"));
    PB_CHECK_EQ(e.actions().rename_entry("/Docs/unknown.txt", "z.txt").error.key, std::string("refresh_first"));   // not in the listing the reader has
    PB_CHECK_EQ(e.store->queue_count_open(), 0);
}

PB_TEST(an_entry_that_changed_or_vanished_before_the_rename_is_held_and_not_applied) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "content");
    e.server.add_file("/Docs/c.txt", "content");
    e.cache("/Docs");
    const auto changed = e.actions().rename_entry("/Docs/a.txt", "b.txt");
    const auto gone = e.actions().rename_entry("/Docs/c.txt", "d.txt");
    e.server.set_file("/Docs/a.txt", "somebody changed it", 1790000500);
    e.server.remove("/Docs/c.txt");
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.conflict, 1);
    PB_CHECK_EQ(rep.failed, 1);
    PB_CHECK_EQ(e.item(changed.queued[0]).reason, std::string("changed_on_server"));
    PB_CHECK_EQ(e.item(gone.queued[0]).reason, std::string("source_gone"));
    PB_CHECK(e.server.has("/Docs/a.txt"));
    PB_CHECK(!e.server.has("/Docs/b.txt"));
}

PB_TEST(a_rename_onto_a_name_that_appeared_meanwhile_is_a_conflict_and_nothing_is_overwritten) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "mine");
    e.cache("/Docs");
    const auto r = e.actions().rename_entry("/Docs/a.txt", "b.txt");
    e.server.add_file("/Docs/b.txt", "theirs");
    PB_CHECK_EQ(e.run().conflict, 1);
    PB_CHECK_EQ(e.item(r.queued[0]).reason, std::string("name_exists"));
    PB_CHECK_EQ(e.server.node("/Docs/b.txt").content, std::string("theirs"));
    PB_CHECK_EQ(e.server.node("/Docs/a.txt").content, std::string("mine"));
}

PB_TEST(the_downloaded_copy_and_the_offline_mark_of_a_renamed_file_follow_it_and_the_file_on_the_card_stays) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "content");
    e.cache("/Docs");
    add_local_copy(e, "/Docs/a.txt", "Docs/a.txt");
    PB_CHECK(e.store->set_mark("/Docs/a.txt", pbc::Kind::File, true).ok());
    e.actions().rename_entry("/Docs/a.txt", "b.txt");
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK(!e.store->local_file("/Docs/a.txt").has_value());
    const auto copy = e.store->local_file("/Docs/b.txt");
    PB_CHECK(copy.has_value());
    PB_CHECK_EQ(copy->local_path, std::string("Docs/a.txt"));   // the file on the card keeps its name
    PB_CHECK(::access((e.download_root + "/Docs/a.txt").c_str(), R_OK) == 0);
    PB_CHECK(e.store->is_marked("/Docs/b.txt"));
    PB_CHECK(!e.store->is_marked("/Docs/a.txt"));
}

PB_TEST(renaming_a_folder_moves_the_records_below_it_by_prefix_and_not_those_of_a_sibling_with_a_longer_name) {
    UpEnv e;
    e.server.add_folder("/Docs/proj");
    e.server.add_file("/Docs/proj/x.txt", "x");
    e.server.add_folder("/Docs/proj2");
    e.server.add_file("/Docs/proj2/y.txt", "y");
    e.cache("/Docs");
    e.cache("/Docs/proj");
    add_local_copy(e, "/Docs/proj/x.txt", "Docs/proj/x.txt");
    add_local_copy(e, "/Docs/proj2/y.txt", "Docs/proj2/y.txt");
    PB_CHECK(e.store->set_mark("/Docs/proj", pbc::Kind::Folder, true).ok());
    PB_CHECK(e.store->set_mark("/Docs/proj2", pbc::Kind::Folder, true).ok());
    e.actions().rename_entry("/Docs/proj", "plans");
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK(e.store->local_file("/Docs/plans/x.txt").has_value());
    PB_CHECK(!e.store->local_file("/Docs/proj/x.txt").has_value());
    PB_CHECK(e.store->local_file("/Docs/proj2/y.txt").has_value());       // untouched
    PB_CHECK(e.store->is_marked("/Docs/plans"));
    PB_CHECK(e.store->is_marked("/Docs/proj2"));
    PB_CHECK(!e.store->is_marked("/Docs/proj"));
    PB_CHECK(e.server.has("/Docs/plans/x.txt"));
}

PB_TEST(a_lost_answer_of_a_rename_is_unknown_and_the_check_finds_the_entry_under_its_new_name_by_file_id) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "content");
    e.cache("/Docs");
    const auto r = e.actions().rename_entry("/Docs/a.txt", "b.txt");
    e.server.lose_next_answer("MOVE");
    PB_CHECK_EQ(e.run().unknown, 1);
    PB_CHECK(e.sender()->check_unknown(r.queued[0]) == pbc::Found::Arrived);
    PB_CHECK(e.sender()->mark_unknown_done(r.queued[0]).ok());
    PB_CHECK(e.store->entry("/Docs/b.txt").has_value());
    PB_CHECK(!e.store->entry("/Docs/a.txt").has_value());
}

PB_TEST(a_rename_onto_a_taken_name_asks_keep_both_replace_or_skip_and_queues_nothing_before_the_answer) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "mine");
    e.server.add_file("/Docs/b.txt", "theirs");
    e.server.add_folder("/Docs/d");
    e.server.add_folder("/Docs/f");
    e.cache("/Docs");
    auto act = e.actions();
    const auto r = act.rename_entry("/Docs/a.txt", "b.txt");
    PB_CHECK(r.error.ok());
    PB_CHECK_EQ(r.queued.size(), size_t(0));
    PB_CHECK_EQ(r.asks.size(), size_t(1));
    PB_CHECK(r.asks[0].kind == pbc::AskKind::Rename);
    PB_CHECK(r.asks[0].replace_possible);
    PB_CHECK_EQ(r.asks[0].suggested, std::string("b (2).txt"));
    PB_CHECK_EQ(e.store->queue_count_open(), 0);
    PB_CHECK_EQ(act.answer_ask(r.asks[0], pbc::AskChoice::Skip).queued.size(), size_t(0));
    const auto kept = act.answer_ask(r.asks[0], pbc::AskChoice::KeepBoth);
    PB_CHECK_EQ(kept.queued.size(), size_t(1));
    PB_CHECK(e.item(kept.queued[0]).kind == pbc::ItemKind::Rename);
    PB_CHECK_EQ(e.item(kept.queued[0]).dst_name, std::string("b (2).txt"));
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.server.node("/Docs/b.txt").content, std::string("theirs"));
    PB_CHECK_EQ(e.server.node("/Docs/b (2).txt").content, std::string("mine"));
    // a folder is never replaced, a file never goes over a folder
    const auto over_folder = act.rename_entry("/Docs/b (2).txt", "d");
    PB_CHECK_EQ(over_folder.asks.size(), size_t(1));
    PB_CHECK(!over_folder.asks[0].replace_possible);
    PB_CHECK_EQ(act.answer_ask(over_folder.asks[0], pbc::AskChoice::Replace).error.key, std::string("not_allowed"));
    const auto folder_over = act.rename_entry("/Docs/d", "f");
    PB_CHECK(!folder_over.asks[0].replace_possible);
}

PB_TEST(a_rename_that_replaces_deletes_the_old_version_into_the_trash_on_its_etag_and_moves_with_overwrite_f) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "mine");
    e.server.add_file("/Docs/b.txt", "theirs");
    e.cache("/Docs");
    const std::string old_etag = e.etag("/Docs/b.txt");
    auto act = e.actions();
    const auto r = act.rename_entry("/Docs/a.txt", "b.txt");
    const auto q = act.answer_ask(r.asks[0], pbc::AskChoice::Replace);
    PB_CHECK_EQ(q.queued.size(), size_t(1));
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.logged("DELETE").size(), size_t(1));
    PB_CHECK_EQ(e.logged("DELETE")[0].if_match, old_etag);
    PB_CHECK_EQ(e.logged("MOVE")[0].overwrite, std::string("F"));
    PB_CHECK(e.server.trash_has("/Docs/b.txt"));
    PB_CHECK_EQ(e.server.node("/Docs/b.txt").content, std::string("mine"));
    PB_CHECK(!e.server.has("/Docs/a.txt"));
    PB_CHECK_EQ(e.server.overwrites(), 0);
    PB_CHECK(!e.store->entry("/Docs/a.txt").has_value());
}

PB_TEST(a_rename_held_because_the_name_appeared_meanwhile_can_keep_both_or_replace_anyway) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "mine");
    e.server.add_file("/Docs/c.txt", "mine too");
    e.cache("/Docs");
    const auto keep = e.actions().rename_entry("/Docs/a.txt", "b.txt");
    const auto rep = e.actions().rename_entry("/Docs/c.txt", "d.txt");
    e.server.add_file("/Docs/b.txt", "theirs");
    e.server.add_file("/Docs/d.txt", "theirs too");
    PB_CHECK_EQ(e.run().conflict, 2);
    auto s = e.sender();
    PB_CHECK(s->resolve_keep_both(keep.queued[0], std::string()).ok());
    PB_CHECK_EQ(e.item(keep.queued[0]).dst_name, std::string("b (2).txt"));
    const auto seen = s->refresh_target(rep.queued[0]);
    PB_CHECK(seen.ok() && seen.value.has_value());
    PB_CHECK(s->resolve_replace(rep.queued[0], seen.value->etag).ok());
    PB_CHECK_EQ(e.run().done, 2);
    PB_CHECK_EQ(e.server.node("/Docs/b.txt").content, std::string("theirs"));
    PB_CHECK_EQ(e.server.node("/Docs/b (2).txt").content, std::string("mine"));
    PB_CHECK_EQ(e.server.node("/Docs/d.txt").content, std::string("mine too"));
    PB_CHECK(e.server.trash_has("/Docs/d.txt"));
}

PB_TEST_MAIN()
