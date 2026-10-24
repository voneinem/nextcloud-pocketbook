// Restoring to a place the user chose (specs/006-trash-and-versions, spec Story 2 scenarios 3 to 5, FR-008 to FR-011, research R-6, contracts/trash-requests.md T5), against
// the in-process fake Nextcloud: a free name in the chosen folder, a taken name that is never replaced, a missing folder that is never created, a server that cannot
// restore to a chosen place. It proves the app follows the contract, never a real server (constitution Principle IX).
#include <string>

#include "test_support.h"
#include "trash_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::TrashEnv;

namespace {

pbc::RestoreTarget target(const std::string& folder, const std::string& name) {
    pbc::RestoreTarget t;
    t.folder = folder;
    t.name = name;
    return t;
}

}  // namespace

PB_TEST(a_file_goes_to_the_chosen_folder_under_a_new_name_with_overwrite_forbidden) {
    TrashEnv e;
    e.server.add_folder("/Other");
    const std::string name = e.server.add_trash_file("/Books/story.epub", "the story", e.t(100));
    const auto item = e.item(name);
    e.server.clear_log();
    const auto r = e.api.restore_to(item, target("/Other", "renamed.epub"));
    PB_CHECK(r.error.ok() && !r.landed_elsewhere);
    PB_CHECK_EQ(e.server.content_of("/Other/renamed.epub"), std::string("the story"));
    PB_CHECK(!e.server.trashbin_has(name));
    PB_CHECK_EQ(e.moves(), size_t(1));
    const auto& m = e.server.log()[0];
    PB_CHECK_EQ(m.method, std::string("MOVE"));
    PB_CHECK_EQ(m.overwrite, std::string("F"));
    PB_CHECK(m.destination.find("/remote.php/dav/files/demo/Other/renamed.epub") != std::string::npos);
    PB_CHECK_EQ(e.server.request_count(), size_t(2));   // the MOVE and one probe of the new place
}

PB_TEST(a_folder_goes_to_the_chosen_folder_with_its_content) {
    TrashEnv e;
    e.server.add_folder("/Other");
    const std::string name = e.server.add_trash_folder("/Documents/Old Notes", e.t(100), {{"/a.txt", "a"}, {"/sub/b.txt", "bb"}});
    const auto r = e.api.restore_to(e.item(name), target("/Other", "Notes"));
    PB_CHECK(r.error.ok());
    PB_CHECK_EQ(e.server.content_of("/Other/Notes/sub/b.txt"), std::string("bb"));
    PB_CHECK_EQ(e.server.trashbin_count(), size_t(0));
}

PB_TEST(a_place_in_the_root_works_too) {
    TrashEnv e;
    const std::string name = e.server.add_trash_file("/Books/x.txt", "x", e.t(100));
    PB_CHECK(e.api.restore_to(e.item(name), target("/", "x.txt")).error.ok());
    PB_CHECK_EQ(e.server.content_of("/x.txt"), std::string("x"));
}

PB_TEST(a_taken_name_in_the_chosen_folder_is_seen_by_the_probe_and_never_replaced) {
    TrashEnv e;
    e.server.add_file("/Other/exists.txt", "the existing one");
    const std::string name = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    const auto item = e.item(name);
    const auto plan = e.api.probe_place("/Other", "exists.txt");
    PB_CHECK(plan.ok() && plan.value == pbc::RestorePlan::NameTaken);
    PB_CHECK_EQ(e.moves(), size_t(0));
    // a stale probe: the app sends the request anyway; Overwrite: F makes the server refuse, the existing entry stays
    const auto r = e.api.restore_to(item, target("/Other", "exists.txt"));
    PB_CHECK_EQ(r.error.key, std::string("restore_name_taken"));
    PB_CHECK_EQ(r.code, std::string("T5-412"));
    PB_CHECK_EQ(e.server.content_of("/Other/exists.txt"), std::string("the existing one"));
    PB_CHECK(e.server.trashbin_has(name));
    PB_CHECK_EQ(e.server.overwrites(), 0);   // the server never removed a target
}

PB_TEST(the_probe_of_a_free_place_and_of_a_missing_folder) {
    TrashEnv e;
    e.server.add_folder("/Other");
    PB_CHECK(e.api.probe_place("/Other", "free.txt").value == pbc::RestorePlan::OriginalFree);
    PB_CHECK(e.api.probe_place("/Missing", "free.txt").value == pbc::RestorePlan::ParentMissing);
    PB_CHECK(e.api.probe_place("/", "free.txt").value == pbc::RestorePlan::OriginalFree);
}

PB_TEST(a_missing_chosen_folder_is_refused_and_not_created) {
    TrashEnv e;
    const std::string name = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    const auto r = e.api.restore_to(e.item(name), target("/Gone", "a.txt"));
    PB_CHECK_EQ(r.error.key, std::string("restore_parent_missing"));
    PB_CHECK(!e.server.has("/Gone"));
    PB_CHECK(e.server.trashbin_has(name));
    PB_CHECK_EQ(e.server.count("MKCOL"), size_t(0));
}

PB_TEST(a_server_that_cannot_restore_to_a_chosen_place_says_so_and_changes_nothing) {
    TrashEnv e;
    e.server.add_folder("/Other");
    e.server.set_restore_to_files_status(403);
    const std::string name = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    e.server.clear_log();
    const auto r = e.api.restore_to(e.item(name), target("/Other", "a.txt"));
    PB_CHECK_EQ(r.error.key, std::string("restore_place_unsupported"));
    PB_CHECK_EQ(r.code, std::string("T5-403"));
    PB_CHECK(!e.server.has("/Other/a.txt"));
    PB_CHECK(e.server.trashbin_has(name));
    PB_CHECK_EQ(e.moves(), size_t(1));   // one try, no workaround
    for (long status : {405L, 501L, 502L}) {
        e.server.set_restore_to_files_status(status);
        PB_CHECK_EQ(e.api.restore_to(e.item(name), target("/Other", "a.txt")).error.key, std::string("restore_place_unsupported"));
    }
}

PB_TEST(a_403_that_did_restore_the_item_is_not_called_unsupported) {
    TrashEnv e;
    e.server.add_folder("/Other");
    const std::string name = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    const auto item = e.item(name);
    // the restore happened but the answer said 403 (a proxy): the re-probe finds the item, so it is done, not "unsupported"
    bool fired = false;
    e.server.set_interceptor([&](const pbc::Request& r) -> std::optional<pbc::Response> {
        if (r.method != "MOVE" || fired) return std::nullopt;
        fired = true;
        // the server does it: the same request is handled by the fake itself (the interceptor is skipped once `fired` is set)
        (void)e.server.perform(r);
        pbc::Response resp;
        resp.status = 403;
        return resp;
    });
    const auto r = e.api.restore_to(item, target("/Other", "a.txt"));
    PB_CHECK(r.error.ok());
    PB_CHECK_EQ(e.server.content_of("/Other/a.txt"), std::string("a"));
}

PB_TEST(a_target_with_a_name_the_rules_refuse_sends_nothing) {
    TrashEnv e;
    e.server.add_folder("/Other");
    const std::string name = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    const auto item = e.item(name);
    e.server.clear_log();
    for (const char* bad : {"", "..", "a/b", "x\\y"}) PB_CHECK_EQ(e.api.restore_to(item, target("/Other", bad)).error.key, std::string("name_forbidden"));
    PB_CHECK_EQ(e.api.restore_to(item, target("relative", "a.txt")).error.key, std::string("restore_parent_missing"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    auto bad_item = item;
    bad_item.unreadable = true;
    PB_CHECK_EQ(e.api.restore_to(bad_item, target("/Other", "a.txt")).error.key, std::string("trash_not_actionable"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(an_item_without_a_known_original_place_can_still_go_to_a_chosen_one) {
    TrashEnv e;
    e.server.add_folder("/Other");
    const std::string name = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    auto item = e.item(name);
    item.original_known = false;   // the location was unsafe or missing: only a chosen place is possible
    item.original_dir.clear();
    item.original_name.clear();
    PB_CHECK(e.api.restore_to(item, target("/Other", "a.txt")).error.ok());
    PB_CHECK_EQ(e.server.content_of("/Other/a.txt"), std::string("a"));
}

PB_TEST_MAIN()
