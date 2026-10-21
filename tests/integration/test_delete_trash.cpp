// Delete on the server (specs/002-manage-server-files US6, FR-034 to FR-040, research R-9): only where the server has a trash, always on the version the
// reader saw, held when the entry changed, never permanent. The cached rows go with the confirmation; the reader's own copies are never touched.
// Fake Nextcloud.
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

PB_TEST(a_delete_is_one_delete_on_the_entrys_version_and_lands_in_the_trash_and_the_cache_row_goes) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "content");
    e.server.add_file("/Docs/b.txt", "content");
    e.cache("/Docs");
    const std::string etag = e.etag("/Docs/a.txt");
    const pbc::AddResult r = e.actions().delete_entries({"/Docs/a.txt"});
    PB_CHECK(r.error.ok());
    PB_CHECK_EQ(r.queued.size(), size_t(1));
    // the row stays in the listing, marked, until the server has done it
    const auto before = e.store->entries_page_overlay("/Docs", pbc::SortKey::Name, true, 0, 10);
    PB_CHECK_EQ(before.total, 2);
    PB_CHECK_EQ(before.rows[0].pending, std::string("deleting"));
    const auto folder_before = e.store->folder("/Docs");
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 1);
    const auto del = e.logged("DELETE");
    PB_CHECK_EQ(del.size(), size_t(1));
    PB_CHECK_EQ(del[0].if_match, etag);
    PB_CHECK(e.server.trash_has("/Docs/a.txt"));
    PB_CHECK_EQ(e.server.permanent_deletes(), 0);
    PB_CHECK(!e.store->entry("/Docs/a.txt").has_value());
    PB_CHECK(e.store->entry("/Docs/b.txt").has_value());
    PB_CHECK_EQ(e.store->folder("/Docs")->fetched_at.value_or(-1), folder_before->fetched_at.value_or(-2));   // the age of the listing is not touched
}

PB_TEST(a_folder_goes_with_everything_in_it_and_its_cached_listing_goes_too) {
    UpEnv e;
    e.server.add_folder("/Docs/old");
    e.server.add_file("/Docs/old/x.txt", "x");
    e.cache("/Docs");
    e.cache("/Docs/old");
    PB_CHECK(e.store->folder("/Docs/old").has_value());
    e.actions().delete_entries({"/Docs/old"});
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK(!e.server.has("/Docs/old"));
    PB_CHECK(e.server.trash_has("/Docs/old/x.txt"));
    PB_CHECK(!e.store->entry("/Docs/old").has_value());
    PB_CHECK(!e.store->folder("/Docs/old").has_value());
}

PB_TEST(an_entry_that_is_already_gone_counts_as_deleted) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "x");
    e.cache("/Docs");
    e.actions().delete_entries({"/Docs/a.txt"});
    e.server.remove("/Docs/a.txt");
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK(!e.store->entry("/Docs/a.txt").has_value());
}

PB_TEST(an_entry_that_changed_is_held_and_keep_entry_or_delete_anyway_decide) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "x");
    e.server.add_file("/Docs/b.txt", "x");
    e.cache("/Docs");
    const auto r = e.actions().delete_entries({"/Docs/a.txt", "/Docs/b.txt"});
    e.server.set_file("/Docs/a.txt", "somebody changed it", 1790000500);
    e.server.set_file("/Docs/b.txt", "somebody changed it", 1790000500);
    PB_CHECK_EQ(e.run().conflict, 2);
    PB_CHECK_EQ(e.item(r.queued[0]).reason, std::string("changed_on_server"));
    PB_CHECK(e.server.has("/Docs/a.txt"));
    PB_CHECK_EQ(e.server.trash_count(), 0LL);
    // keep the entry: the item is dropped
    auto s = e.sender();
    PB_CHECK(s->resolve_discard(r.queued[0]).ok());
    PB_CHECK(!e.store->queue_item(r.queued[0]).has_value());
    // delete anyway: the version is looked at again and the delete is on that version
    const auto seen = s->refresh_source(r.queued[1]);
    PB_CHECK(seen.ok() && seen.value.has_value());
    PB_CHECK(s->resolve_apply_anyway(r.queued[1], seen.value->etag).ok());
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK(e.server.has("/Docs/a.txt"));
    PB_CHECK(!e.server.has("/Docs/b.txt"));
    PB_CHECK(e.server.trash_has("/Docs/b.txt"));
}

PB_TEST(a_folder_with_a_locked_member_is_a_partial_failure_and_the_cache_is_not_patched) {
    UpEnv e;
    e.server.add_folder("/Docs/old");
    e.server.add_file("/Docs/old/x.txt", "x");
    e.server.lock("/Docs/old/x.txt");
    e.cache("/Docs");
    e.cache("/Docs/old");
    const auto r = e.actions().delete_entries({"/Docs/old"});
    PB_CHECK_EQ(e.run().failed, 1);
    PB_CHECK_EQ(e.item(r.queued[0]).reason, std::string("partial_failure"));
    PB_CHECK(e.store->entry("/Docs/old").has_value());
    PB_CHECK(e.store->folder("/Docs/old").has_value());
    PB_CHECK(e.server.has("/Docs/old"));
}

PB_TEST(a_locked_file_is_refused_and_only_retried_on_the_users_tap) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "x");
    e.server.lock("/Docs/a.txt");
    e.cache("/Docs");
    const auto r = e.actions().delete_entries({"/Docs/a.txt"});
    PB_CHECK_EQ(e.run().failed, 1);
    PB_CHECK_EQ(e.item(r.queued[0]).reason, std::string("locked"));
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 0);   // not by itself
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    e.server.lock("/Docs/a.txt", false);
    // the tap: Retry puts it back in the queue and sends it alone
    auto it = e.item(r.queued[0]);
    it.state = pbc::ItemState::Queued;
    it.reason.clear();
    PB_CHECK(e.store->queue_update(it).ok());
    PB_CHECK_EQ(e.run(r.queued[0]).done, 1);
}

PB_TEST(without_a_trash_nothing_is_queued_and_no_delete_is_ever_sent) {
    UpEnv e;
    pbc::ServerCaps none;
    none.trash = false;
    PB_CHECK(e.store->save_server_caps(none).ok());
    e.server.set_trash(false);
    e.server.add_file("/Docs/a.txt", "x");
    e.cache("/Docs");
    const auto r = e.actions().delete_entries({"/Docs/a.txt"});
    PB_CHECK_EQ(r.error.key, std::string("trash_unavailable"));
    PB_CHECK_EQ(r.queued.size(), size_t(0));
    PB_CHECK(!e.actions().delete_preview({"/Docs/a.txt"}).offered);
    // an unknown trash counts as none
    pbc::ServerCaps unknown;
    PB_CHECK(e.store->save_server_caps(unknown).ok());
    PB_CHECK_EQ(e.actions().delete_entries({"/Docs/a.txt"}).error.key, std::string("trash_unavailable"));
    PB_CHECK_EQ(e.server.count("DELETE"), size_t(0));
}

PB_TEST(a_delete_row_of_an_earlier_session_is_held_when_the_server_has_no_trash_any_more) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "x");
    e.cache("/Docs");
    const auto r = e.actions().delete_entries({"/Docs/a.txt"});
    PB_CHECK_EQ(r.queued.size(), size_t(1));
    pbc::ServerCaps none;
    none.trash = false;
    PB_CHECK(e.store->save_server_caps(none).ok());   // the account check since then found no trash
    e.server.clear_log();
    PB_CHECK_EQ(e.run().failed, 1);
    PB_CHECK_EQ(e.item(r.queued[0]).reason, std::string("trash_unavailable"));
    PB_CHECK_EQ(e.logged("DELETE").size(), size_t(0));
    PB_CHECK(e.server.has("/Docs/a.txt"));
}

PB_TEST(the_confirmation_data_names_the_entries_the_folders_and_the_queued_items_inside) {
    UpEnv e;
    e.server.add_folder("/Docs/old");
    e.server.add_file("/Docs/old/x.txt", "x");
    e.server.add_file("/Docs/a.txt", "x");
    e.cache("/Docs");
    e.cache("/Docs/old");
    PB_CHECK(e.actions().rename_entry("/Docs/old/x.txt", "y.txt").error.ok());
    PB_CHECK(e.actions().add_uploads("/Docs/old", {e.source("Downloads/new.txt", 10)}).error.ok());
    const pbc::DeletePreview p = e.actions().delete_preview({"/Docs/old", "/Docs/a.txt"});
    PB_CHECK(p.offered);
    PB_CHECK_EQ(p.entries, 2);
    PB_CHECK_EQ(p.folders, 1);
    PB_CHECK_EQ(p.items_inside, 2);   // the rename and the upload fail when the folder is gone
    PB_CHECK_EQ(p.names.size(), size_t(2));
}

PB_TEST(an_entry_without_a_known_version_or_with_a_change_waiting_is_left_out) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "x");
    e.server.add_file("/Docs/w.txt", "x");
    e.server.set_weak_etag("/Docs/w.txt", true);
    e.cache("/Docs");
    PB_CHECK(e.actions().rename_entry("/Docs/a.txt", "a2.txt").error.ok());
    const auto r = e.actions().delete_entries({"/Docs/a.txt", "/Docs/w.txt", "/Docs/unknown.txt"});
    PB_CHECK_EQ(r.queued.size(), size_t(0));
    PB_CHECK_EQ(r.refused.size(), size_t(3));
    PB_CHECK_EQ(r.refused[0].key, std::string("change_pending"));
    PB_CHECK_EQ(r.refused[1].key, std::string("refresh_first"));
}

PB_TEST(the_downloaded_copy_and_its_mark_are_never_touched_by_a_delete) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "x");
    e.cache("/Docs");
    add_local_copy(e, "/Docs/a.txt", "Docs/a.txt");
    PB_CHECK(e.store->set_mark("/Docs/a.txt", pbc::Kind::File, true).ok());
    e.actions().delete_entries({"/Docs/a.txt"});
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK(e.store->local_file("/Docs/a.txt").has_value());
    PB_CHECK(e.store->is_marked("/Docs/a.txt"));
    PB_CHECK(::access((e.download_root + "/Docs/a.txt").c_str(), R_OK) == 0);
}

PB_TEST(a_lost_answer_of_a_delete_is_unknown_and_the_check_finds_the_entry_gone) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "x");
    e.cache("/Docs");
    const auto r = e.actions().delete_entries({"/Docs/a.txt"});
    e.server.lose_next_answer("DELETE");
    PB_CHECK_EQ(e.run().unknown, 1);
    PB_CHECK(e.sender()->check_unknown(r.queued[0]) == pbc::Found::Arrived);
    PB_CHECK(e.sender()->mark_unknown_done(r.queued[0]).ok());
    PB_CHECK(!e.store->entry("/Docs/a.txt").has_value());
}

PB_TEST(a_trash_the_reader_never_learned_is_told_apart_from_a_server_that_has_none) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "x");
    e.cache("/Docs");
    PB_CHECK(e.store->debug_exec("DELETE FROM server_caps").empty());
    auto p = e.actions().delete_preview({"/Docs/a.txt"});
    PB_CHECK(!p.offered);
    PB_CHECK(!p.trash_known);
    pbc::ServerCaps none;
    none.trash = false;
    PB_CHECK(e.store->save_server_caps(none).ok());
    p = e.actions().delete_preview({"/Docs/a.txt"});
    PB_CHECK(!p.offered);
    PB_CHECK(p.trash_known);
}

PB_TEST(items_queued_inside_a_folder_that_is_deleted_end_as_failed_with_a_reason_and_are_not_lost) {
    UpEnv e;
    e.server.add_folder("/Docs/old");
    e.server.add_file("/Docs/old/x.txt", "x");
    e.server.add_file("/Docs/old/y.txt", "y");
    e.cache("/Docs");
    e.cache("/Docs/old");
    const auto rename = e.actions().rename_entry("/Docs/old/x.txt", "x2.txt");
    const auto up = e.actions().add_uploads("/Docs/old", {e.source("Downloads/new.txt", 10)});
    PB_CHECK(rename.error.ok() && up.queued.size() == 1);
    // the delete is queued last, so the items inside go first; to meet the case "the folder goes first" the delete is sent alone
    const auto del = e.actions().delete_entries({"/Docs/old"});
    PB_CHECK_EQ(del.queued.size(), size_t(1));
    PB_CHECK_EQ(e.run(del.queued[0]).done, 1);
    PB_CHECK(!e.server.has("/Docs/old"));
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.failed, 2);
    PB_CHECK_EQ(rep.done, 0);
    PB_CHECK(e.item(rename.queued[0]).state == pbc::ItemState::Failed);
    PB_CHECK(e.item(up.queued[0]).state == pbc::ItemState::Failed);
    PB_CHECK(!e.item(rename.queued[0]).reason.empty());
    PB_CHECK(!e.item(up.queued[0]).reason.empty());
    PB_CHECK(::access((e.root + "/Downloads/new.txt").c_str(), R_OK) == 0);   // the reader's file is where it was
}

PB_TEST_MAIN()
