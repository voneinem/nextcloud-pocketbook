// Restoring a deleted file or folder to its original place (specs/006-trash-and-versions, spec Story 2, FR-007 to FR-014, research R-5, contracts/trash-requests.md
// T4), against the in-process fake Nextcloud: never an overwrite, never a created folder, a late collision leaves the existing entry untouched. It proves the
// app follows the contract, never a real server (constitution Principle IX).
#include <string>

#include "test_support.h"
#include "trash_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::TrashEnv;

PB_TEST(a_free_original_place_is_probed_then_the_file_comes_back_and_leaves_the_trash) {
    TrashEnv e;
    e.server.add_folder("/Books");
    const std::string name = e.server.add_trash_file("/Books/story.epub", "the story", e.t(100));
    const auto item = e.item(name);
    e.server.clear_log();
    const auto plan = e.api.probe_restore(item);
    PB_CHECK(plan.ok() && plan.value == pbc::RestorePlan::OriginalFree);
    PB_CHECK_EQ(e.server.request_count(), size_t(2));   // the entry first (absent), then its folder
    PB_CHECK_EQ(e.server.log()[0].method, std::string("PROPFIND"));
    PB_CHECK_EQ(e.server.log()[0].depth, std::string("0"));
    PB_CHECK(e.server.log()[0].path.find("/dav/files/demo/Books/story.epub") != std::string::npos);
    PB_CHECK(e.server.log()[1].path.find("/dav/files/demo/Books") != std::string::npos);
    e.server.clear_log();
    const auto r = e.api.restore_original(item);
    PB_CHECK(r.error.ok() && !r.outcome_unknown && !r.landed_elsewhere);
    PB_CHECK_EQ(e.server.content_of("/Books/story.epub"), std::string("the story"));
    PB_CHECK(!e.server.trashbin_has(name));
    // the request: one MOVE to the restore collection with Overwrite: F, then one probe of the place
    PB_CHECK_EQ(e.moves(), size_t(1));
    PB_CHECK_EQ(e.server.log()[0].method, std::string("MOVE"));
    PB_CHECK_EQ(e.server.log()[0].overwrite, std::string("F"));
    PB_CHECK(e.server.log()[0].destination.find("/trashbin/demo/restore/") != std::string::npos);
    PB_CHECK_EQ(e.server.log()[1].method, std::string("PROPFIND"));
    PB_CHECK_EQ(e.server.request_count(), size_t(2));
}

PB_TEST(a_folder_comes_back_with_its_whole_content_as_one_item) {
    TrashEnv e;
    e.server.add_folder("/Documents");
    const std::string name = e.server.add_trash_folder("/Documents/Old Notes", e.t(100), {{"/a.txt", "a"}, {"/sub/b.txt", "bb"}});
    const auto r = e.api.restore_original(e.item(name));
    PB_CHECK(r.error.ok() && !r.landed_elsewhere);
    PB_CHECK_EQ(e.server.content_of("/Documents/Old Notes/a.txt"), std::string("a"));
    PB_CHECK_EQ(e.server.content_of("/Documents/Old Notes/sub/b.txt"), std::string("bb"));
    PB_CHECK_EQ(e.server.trashbin_count(), size_t(0));
}

PB_TEST(a_taken_name_is_a_plan_and_sends_no_move) {
    TrashEnv e;
    e.server.add_file("/Books/story.epub", "the current one");
    const std::string name = e.server.add_trash_file("/Books/story.epub", "the old one", e.t(100));
    const auto item = e.item(name);
    e.server.clear_log();
    const auto plan = e.api.probe_restore(item);
    PB_CHECK(plan.ok() && plan.value == pbc::RestorePlan::NameTaken);
    PB_CHECK_EQ(e.server.request_count(), size_t(1));   // the entry was there: no need to look at its folder
    PB_CHECK_EQ(e.moves(), size_t(0));
    PB_CHECK_EQ(e.server.content_of("/Books/story.epub"), std::string("the current one"));
    PB_CHECK(e.server.trashbin_has(name));
}

PB_TEST(a_folder_of_that_name_also_counts_as_taken) {
    TrashEnv e;
    e.server.add_folder("/Books/story.epub");
    const std::string name = e.server.add_trash_file("/Books/story.epub", "x", e.t(100));
    PB_CHECK(e.api.probe_restore(e.item(name)).value == pbc::RestorePlan::NameTaken);
}

PB_TEST(a_missing_original_folder_is_a_plan_and_no_folder_is_created) {
    TrashEnv e;
    const std::string name = e.server.add_trash_file("/Nowhere/deep/x.txt", "x", e.t(100));
    const auto item = e.item(name);
    e.server.clear_log();
    const auto plan = e.api.probe_restore(item);
    PB_CHECK(plan.ok() && plan.value == pbc::RestorePlan::ParentMissing);
    PB_CHECK_EQ(e.moves(), size_t(0));
    PB_CHECK(!e.server.has("/Nowhere"));
    PB_CHECK_EQ(e.server.count("MKCOL"), size_t(0));
    // forced anyway (the user's tap came after a stale probe): the server refuses and nothing is created either
    const auto r = e.api.restore_original(item);
    PB_CHECK_EQ(r.error.key, std::string("restore_name_taken"));   // 409: the place cannot take it
    PB_CHECK(!e.server.has("/Nowhere"));
    PB_CHECK(e.server.trashbin_has(name));
}

PB_TEST(an_item_in_the_root_needs_no_probe_of_its_folder) {
    TrashEnv e;
    const std::string name = e.server.add_trash_file("/readme.txt", "x", e.t(100));
    const auto item = e.item(name);
    e.server.clear_log();
    PB_CHECK(e.api.probe_restore(item).value == pbc::RestorePlan::OriginalFree);
    PB_CHECK_EQ(e.server.request_count(), size_t(1));
}

PB_TEST(an_unknown_original_place_is_a_plan_without_a_request) {
    TrashEnv e;
    const std::string name = e.server.add_trash_file("/Books/x.txt", "x", e.t(100));
    auto item = e.item(name);
    item.original_known = false;
    item.original_dir.clear();
    item.original_name.clear();
    e.server.clear_log();
    PB_CHECK(e.api.probe_restore(item).value == pbc::RestorePlan::ChoosePlace);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    const auto r = e.api.restore_original(item);   // it cannot be restored to a place nobody knows
    PB_CHECK_EQ(r.error.key, std::string("trash_not_actionable"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(an_item_that_cannot_be_acted_on_sends_nothing) {
    TrashEnv e;
    const std::string name = e.server.add_trash_file("/Books/x.txt", "x", e.t(100));
    auto item = e.item(name);
    item.unreadable = true;
    e.server.clear_log();
    PB_CHECK(e.api.probe_restore(item).value == pbc::RestorePlan::Unavailable);
    PB_CHECK_EQ(e.api.restore_original(item).error.key, std::string("trash_not_actionable"));
    item.unreadable = false;
    item.availability = pbc::Availability::Encrypted;
    PB_CHECK_EQ(e.api.restore_original(item).error.key, std::string("trash_not_actionable"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(a_name_that_appears_between_the_probe_and_the_restore_is_a_conflict_and_nothing_existing_changes) {
    TrashEnv e;
    e.server.add_folder("/Books");
    const std::string name = e.server.add_trash_file("/Books/story.epub", "old", e.t(100));
    const auto item = e.item(name);
    PB_CHECK(e.api.probe_restore(item).value == pbc::RestorePlan::OriginalFree);
    e.server.add_file("/Books/story.epub", "somebody else made this");   // after the probe
    const auto r = e.api.restore_original(item);
    PB_CHECK_EQ(r.error.key, std::string("restore_name_taken"));
    PB_CHECK_EQ(e.server.content_of("/Books/story.epub"), std::string("somebody else made this"));
    PB_CHECK(e.server.trashbin_has(name));   // the item stays in the trash
}

PB_TEST(a_server_that_restores_under_another_name_is_reported_and_the_app_renames_nothing) {
    TrashEnv e;
    e.server.add_folder("/Books");
    e.server.set_restore_renames(true);
    const std::string name = e.server.add_trash_file("/Books/story.epub", "old", e.t(100));   // 3 bytes
    const auto item = e.item(name);
    PB_CHECK(e.api.probe_restore(item).value == pbc::RestorePlan::OriginalFree);
    e.server.add_file("/Books/story.epub", "a different file made meanwhile");   // another size: not the item
    e.server.clear_log();
    const auto r = e.api.restore_original(item);
    PB_CHECK(r.error.ok());
    PB_CHECK(r.landed_elsewhere);   // the answer was fine, the place does not hold the item
    PB_CHECK_EQ(e.server.content_of("/Books/story.epub"), std::string("a different file made meanwhile"));
    PB_CHECK_EQ(e.server.content_of("/Books/story (restored).epub"), std::string("old"));
    PB_CHECK_EQ(e.moves(), size_t(1));   // no MOVE of the app's own to rename or fix anything
    PB_CHECK_EQ(e.server.count("DELETE"), size_t(0));
}

PB_TEST(a_restored_folder_is_checked_by_its_kind_and_a_restored_file_by_its_size) {
    TrashEnv e;
    e.server.add_folder("/Docs");
    const std::string f = e.server.add_trash_folder("/Docs/Notes", e.t(100), {{"/a.txt", "a"}});
    const std::string g = e.server.add_trash_file("/Docs/a.txt", "12345", e.t(200));
    PB_CHECK(!e.api.restore_original(e.item(f)).landed_elsewhere);
    PB_CHECK(!e.api.restore_original(e.item(g)).landed_elsewhere);
}

PB_TEST(a_server_without_the_trash_app_is_not_asked_to_restore_but_the_endpoint_can_vanish_meanwhile) {
    TrashEnv e;
    const std::string name = e.server.add_trash_file("/Books/x.txt", "x", e.t(100));
    e.server.add_folder("/Books");
    const auto item = e.item(name);
    e.server.set_trash(false);   // the app was turned off after the list was read
    const auto r = e.api.restore_original(item);
    PB_CHECK(!r.error.ok());
    PB_CHECK_EQ(r.error.key, std::string("trash_item_gone"));   // a 404: the item is not there to restore
    PB_CHECK_EQ(r.code, std::string("T4-404"));
}

PB_TEST_MAIN()
