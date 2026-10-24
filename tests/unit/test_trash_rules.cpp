// The pure rules of the trash screens (specs/006-trash-and-versions, contracts/core-interfaces.md "trash_rules.h", research R-5, R-6, R-11, R-15): the plan
// of a restore, the check of a chosen target, the content of a confirmation, the key and the code of a refusal. This file grows with the stories: Phase 5
// (US2) holds the restore, Phase 6 (US3) the version confirmation, Phase 7 (US4) the irreversible steps.
#include <string>

#include "pbc/error_keys.h"
#include "pbc/server_caps.h"
#include "pbc/trash_rules.h"
#include "test_support.h"

namespace {

pbc::TrashItem item(const std::string& dir = "/Books", const std::string& name = "story.epub", bool folder = false) {
    pbc::TrashItem i;
    i.trash_name = name + ".d1760000000";
    i.name = name;
    i.original_dir = dir;
    i.original_name = name;
    i.original_known = true;
    i.deleted_at = 1760000000;
    i.kind = folder ? pbc::Kind::Folder : pbc::Kind::File;
    if (!folder) i.size = 10;
    return i;
}

pbc::RemoteEntry entry(const std::string& path) {
    pbc::RemoteEntry e;
    e.path = path;
    e.name = path.substr(path.rfind('/') + 1);
    return e;
}

pbc::RestoreTarget target(const std::string& folder, const std::string& name, bool original = false) {
    pbc::RestoreTarget t;
    t.folder = folder;
    t.name = name;
    t.is_original = original;
    return t;
}

}  // namespace

// ---- the plan of a restore (research R-5) ----

PB_TEST(a_free_original_place_is_proposed) {
    pbc::RestoreProbe p;
    p.original_known = true;
    PB_CHECK(pbc::plan_restore(item(), p) == pbc::RestorePlan::OriginalFree);
    p.parent_exists = true;
    PB_CHECK(pbc::plan_restore(item(), p) == pbc::RestorePlan::OriginalFree);
}

PB_TEST(a_taken_name_sends_the_user_to_another_folder) {
    pbc::RestoreProbe p;
    p.original_known = true;
    p.at_original = entry("/Books/story.epub");
    PB_CHECK(pbc::plan_restore(item(), p) == pbc::RestorePlan::NameTaken);
    p.at_original = entry("/Books/story.epub");   // a folder of that name counts as taken too
    PB_CHECK(pbc::plan_restore(item("/Books", "story.epub", true), p) == pbc::RestorePlan::NameTaken);
}

PB_TEST(a_missing_original_folder_is_never_created) {
    pbc::RestoreProbe p;
    p.original_known = true;
    p.parent_exists = false;
    PB_CHECK(pbc::plan_restore(item(), p) == pbc::RestorePlan::ParentMissing);
    p.at_original = entry("/Books/story.epub");   // a taken name wins over a missing folder (it cannot be both, but the entry is the stronger fact)
    PB_CHECK(pbc::plan_restore(item(), p) == pbc::RestorePlan::NameTaken);
}

PB_TEST(an_unknown_original_place_means_choose_one) {
    pbc::TrashItem i = item();
    i.original_known = false;
    i.original_dir.clear();
    i.original_name.clear();
    pbc::RestoreProbe p;
    PB_CHECK(pbc::plan_restore(i, p) == pbc::RestorePlan::ChoosePlace);
}

PB_TEST(an_item_that_cannot_be_acted_on_has_no_plan) {
    pbc::RestoreProbe p;
    p.original_known = true;
    pbc::TrashItem u = item();
    u.unreadable = true;
    PB_CHECK(pbc::plan_restore(u, p) == pbc::RestorePlan::Unavailable);
    for (auto a : {pbc::Availability::Encrypted, pbc::Availability::NoPermission, pbc::Availability::Unsupported, pbc::Availability::Unreadable}) {
        pbc::TrashItem x = item();
        x.availability = a;
        PB_CHECK(!pbc::actionable(x));
        PB_CHECK(pbc::plan_restore(x, p) == pbc::RestorePlan::Unavailable);
    }
    PB_CHECK(pbc::actionable(item()));
}

// ---- a chosen target (research R-6) ----

PB_TEST(a_chosen_name_passes_the_rename_rules_and_nothing_else) {
    pbc::ServerCaps caps;
    PB_CHECK(pbc::check_restore_target(item(), target("/Other", "new.epub"), caps).ok);
    for (const char* bad : {"", ".", "..", "a/b", "a\\b", "trailing ", "dots.."}) {
        const auto c = pbc::check_restore_target(item(), target("/Other", bad), caps);
        PB_CHECK(!c.ok);
        PB_CHECK_EQ(c.error_key, std::string("name_forbidden"));
    }
    PB_CHECK(!pbc::check_restore_target(item(), target("/Other", std::string(256, 'n')), caps).ok);
    PB_CHECK(!pbc::check_restore_target(item(), target("/Other", std::string("a") + '\x01'), caps).ok);
    caps.forbidden_chars = ":";
    PB_CHECK(!pbc::check_restore_target(item(), target("/Other", "a:b"), caps).ok);   // what the server forbids
    caps.forbidden_extensions = {".part"};
    PB_CHECK(!pbc::check_restore_target(item(), target("/Other", "a.part"), caps).ok);
}

PB_TEST(a_folder_is_not_restored_into_its_own_former_place) {
    pbc::ServerCaps caps;
    const pbc::TrashItem f = item("/Documents", "Old Notes", true);
    for (const char* inside : {"/Documents/Old Notes", "/Documents/Old Notes/sub"}) {
        const auto c = pbc::check_restore_target(f, target(inside, "x"), caps);
        PB_CHECK(!c.ok);
        PB_CHECK_EQ(c.error_key, std::string("restore_into_self"));
    }
    PB_CHECK(pbc::check_restore_target(f, target("/Documents", "Old Notes (2)"), caps).ok);
    PB_CHECK(pbc::check_restore_target(f, target("/Documents/Old Notes 2", "x"), caps).ok);   // a different folder with a similar name
    PB_CHECK(pbc::check_restore_target(f, target("/Documents", "Old Notes", true), caps).ok);   // the original place itself is the normal case
    PB_CHECK(pbc::check_restore_target(item("/Books", "a.txt"), target("/Books/a.txt", "x"), caps).ok);   // a file has no inside
}

PB_TEST(a_target_folder_must_be_a_valid_remote_path) {
    pbc::ServerCaps caps;
    for (const char* bad : {"", "relative", "/a/../b", "/a/./b"}) {
        const auto c = pbc::check_restore_target(item(), target(bad, "x"), caps);
        PB_CHECK(!c.ok);
        PB_CHECK_EQ(c.error_key, std::string("restore_parent_missing"));
    }
    PB_CHECK(pbc::check_restore_target(item(), target("/", "x"), caps).ok);
}

// ---- the confirmation of a restore (research R-15) ----

PB_TEST(the_confirmation_of_a_restore_names_the_item_and_the_place_and_defaults_to_cancel) {
    const auto c = pbc::confirm_for_restore(item(), target("/Books", "story.epub", true));
    PB_CHECK(c.kind == pbc::ConfirmKind::RestoreProposal);
    PB_CHECK_EQ(c.shown_names.size(), size_t(1));
    PB_CHECK_EQ(c.shown_names[0], std::string("story.epub"));
    PB_CHECK_EQ(c.place, std::string("/Books"));
    PB_CHECK(c.original_place);
    PB_CHECK(!c.destructive && c.default_cancel);
    PB_CHECK_EQ(c.stage, 1);
    PB_CHECK_EQ(pbc::confirm_stages(pbc::ConfirmKind::RestoreProposal), 1);
    const auto chosen = pbc::confirm_for_restore(item("/Documents", "Old Notes", true), target("/Other", "Old Notes", false));
    PB_CHECK(!chosen.original_place);
    PB_CHECK_EQ(chosen.folders, 1);
    PB_CHECK_EQ(chosen.items, 1);
    PB_CHECK_EQ(chosen.place, std::string("/Other"));
}

PB_TEST(a_long_name_in_a_confirmation_is_cut_on_a_character_boundary) {
    pbc::TrashItem i = item();
    i.name.clear();
    for (int k = 0; k < 60; ++k) i.name += "\xc3\xa9";   // 120 bytes
    const auto c = pbc::confirm_for_restore(i, target("/Books", "x", true));
    PB_CHECK(c.shown_names[0].size() <= 40);
    PB_CHECK_EQ(c.shown_names[0].size() % 2, size_t(0));   // no half character
}

// ---- the key and the code of a refusal (research R-11, contracts/trash-requests.md) ----

PB_TEST(a_refused_restore_to_the_original_place_gives_its_key_and_a_code_without_a_name) {
    struct Row { long status; const char* key; };
    for (const Row& r : {Row{404, "trash_item_gone"}, Row{409, "restore_name_taken"}, Row{412, "restore_name_taken"}, Row{403, "forbidden"}, Row{423, "locked"},
                         Row{507, "server_storage_full"}, Row{401, "auth_failed"}, Row{429, "rate_limited"}, Row{500, "server_error"}, Row{503, "server_error"},
                         Row{400, "protocol_error"}}) {
        const auto f = pbc::refusal_for(4, r.status);
        PB_CHECK_EQ(f.key, std::string(r.key));
        PB_CHECK_EQ(f.code, "T4-" + std::to_string(r.status));
    }
}

PB_TEST(a_refused_restore_to_a_chosen_place_gives_its_key) {
    struct Row { long status; const char* key; };
    for (const Row& r : {Row{412, "restore_name_taken"}, Row{409, "restore_parent_missing"}, Row{404, "trash_item_gone"}, Row{423, "locked"},
                         Row{507, "server_storage_full"}, Row{403, "restore_place_unsupported"}, Row{405, "restore_place_unsupported"},
                         Row{501, "restore_place_unsupported"}, Row{502, "restore_place_unsupported"}}) {
        const auto f = pbc::refusal_for(5, r.status);
        PB_CHECK_EQ(f.key, std::string(r.key));
        PB_CHECK_EQ(f.code, "T5-" + std::to_string(r.status));
    }
}

PB_TEST(every_key_a_refusal_gives_is_a_known_error_key) {
    for (int op : {1, 4, 5, 6, 7, 8})
        for (long status : {400, 401, 403, 404, 405, 409, 412, 423, 429, 500, 501, 502, 503, 507, 207})
            PB_CHECK(pbc::is_known_error_key(pbc::refusal_for(op, status).key));
}

// ---- Phase 6 (US3): the confirmation of a version restore ----

PB_TEST(the_confirmation_of_a_version_restore_names_the_file_the_date_and_the_size_and_defaults_to_cancel) {
    pbc::VersionRow row;
    row.version_id = "1760000100";
    row.mtime = 1760000100;
    row.size = 2048;
    const auto c = pbc::confirm_for_version("story.epub", row);
    PB_CHECK(c.kind == pbc::ConfirmKind::RestoreVersion);
    PB_CHECK_EQ(c.shown_names.size(), size_t(1));
    PB_CHECK_EQ(c.shown_names[0], std::string("story.epub"));
    PB_CHECK(c.version_mtime == std::optional<int64_t>(1760000100));
    PB_CHECK(c.version_size == std::optional<int64_t>(2048));
    PB_CHECK(!c.destructive && c.default_cancel);
    PB_CHECK_EQ(c.stage, 1);
    PB_CHECK_EQ(pbc::confirm_stages(pbc::ConfirmKind::RestoreVersion), 1);
    pbc::VersionRow unknown;
    unknown.version_id = "5";
    const auto u = pbc::confirm_for_version("a.txt", unknown);
    PB_CHECK(!u.version_mtime.has_value() && !u.version_size.has_value());
}

PB_TEST(a_long_file_name_in_the_version_confirmation_is_cut_on_a_character_boundary) {
    std::string name;
    for (int k = 0; k < 60; ++k) name += "\xc3\xa9";
    pbc::VersionRow row;
    row.version_id = "5";
    const auto c = pbc::confirm_for_version(name, row);
    PB_CHECK(c.shown_names[0].size() <= 40);
    PB_CHECK_EQ(c.shown_names[0].size() % 2, size_t(0));
}

PB_TEST(the_version_refusals_give_their_keys) {
    struct Row { long status; const char* key; };
    for (const Row& r : {Row{404, "version_gone"}, Row{412, "version_changed_meanwhile"}, Row{403, "forbidden"}, Row{423, "locked"}, Row{507, "server_storage_full"}}) {
        const auto f = pbc::refusal_for(7, r.status);
        PB_CHECK_EQ(f.key, std::string(r.key));
        PB_CHECK_EQ(f.code, "T7-" + std::to_string(r.status));
    }
    PB_CHECK_EQ(pbc::refusal_for(6, 404).key, std::string("file_gone"));
    PB_CHECK_EQ(pbc::refusal_for(6, 500).code, std::string("T6-500"));
}

// ---- Phase 7 (US4): the irreversible steps ----

namespace {

pbc::TrashList list_with(int files, int folders) {
    pbc::TrashList l;
    for (int i = 0; i < files; ++i) l.items.push_back(item("/Books", "f" + std::to_string(i) + ".txt"));
    for (int i = 0; i < folders; ++i) l.items.push_back(item("/Documents", "d" + std::to_string(i), true));
    return l;
}

}  // namespace

PB_TEST(a_file_is_deleted_for_good_after_one_confirmation_that_names_it) {
    const pbc::TrashItem f = item("/Books", "story.epub");
    const auto c = pbc::confirm_for_delete(f, 1);
    PB_CHECK(c.kind == pbc::ConfirmKind::DeleteFile);
    PB_CHECK_EQ(c.shown_names.size(), size_t(1));
    PB_CHECK_EQ(c.shown_names[0], std::string("story.epub"));
    PB_CHECK_EQ(c.items, 1);
    PB_CHECK_EQ(c.folders, 0);
    PB_CHECK_EQ(c.stage, 1);
    PB_CHECK(c.destructive && c.default_cancel);
    PB_CHECK_EQ(pbc::confirm_stages(pbc::ConfirmKind::DeleteFile), 1);
}

PB_TEST(a_folder_needs_two_confirmations_that_both_name_it_and_both_default_to_cancel) {
    const pbc::TrashItem f = item("/Documents", "Old Notes", true);
    const auto a = pbc::confirm_for_delete(f, 1);
    const auto b = pbc::confirm_for_delete(f, 2);
    PB_CHECK(a.kind == pbc::ConfirmKind::DeleteFolder && b.kind == pbc::ConfirmKind::DeleteFolder);
    PB_CHECK_EQ(a.stage, 1);
    PB_CHECK_EQ(b.stage, 2);
    PB_CHECK_EQ(a.folders, 1);
    for (const auto& c : {a, b}) {
        PB_CHECK_EQ(c.shown_names[0], std::string("Old Notes"));
        PB_CHECK(c.destructive && c.default_cancel);
    }
    PB_CHECK_EQ(pbc::confirm_stages(pbc::ConfirmKind::DeleteFolder), 2);
    PB_CHECK_EQ(pbc::confirm_for_delete(f, 7).stage, 2);   // never beyond the last stage
    PB_CHECK_EQ(pbc::confirm_for_delete(item(), 2).stage, 1);   // a file has only one
}

PB_TEST(emptying_the_trash_says_how_many_items_and_folders_and_names_at_most_five) {
    const pbc::TrashList l = list_with(9, 3);
    const auto es = pbc::empty_set_of(l);
    const auto a = pbc::confirm_for_empty(es.named, false, 1);
    PB_CHECK(a.kind == pbc::ConfirmKind::EmptyTrash);
    PB_CHECK_EQ(a.items, 12);
    PB_CHECK_EQ(a.folders, 3);
    PB_CHECK_EQ(a.shown_names.size(), size_t(5));
    PB_CHECK_EQ(a.shown_names[0], std::string("f0.txt"));   // in the order of the list
    PB_CHECK(!a.truncated_notice);
    PB_CHECK(a.destructive && a.default_cancel);
    const auto b = pbc::confirm_for_empty(es.named, true, 2);
    PB_CHECK_EQ(b.stage, 2);
    PB_CHECK(b.truncated_notice);   // "only the newest 1,000 are shown; the others are not affected"
    PB_CHECK_EQ(b.items, 12);
    PB_CHECK_EQ(pbc::confirm_stages(pbc::ConfirmKind::EmptyTrash), 2);
    PB_CHECK_EQ(pbc::confirm_for_empty(es.named, false, 9).stage, 2);
    // a small trash names all of them
    PB_CHECK_EQ(pbc::confirm_for_empty(pbc::empty_set_of(list_with(2, 0)).named, false, 1).shown_names.size(), size_t(2));
}

PB_TEST(a_name_in_the_emptying_confirmation_is_cut_on_a_character_boundary) {
    pbc::TrashList l = list_with(1, 0);
    l.items[0].name.clear();
    for (int k = 0; k < 60; ++k) l.items[0].name += "\xc3\xa9";
    const auto c = pbc::confirm_for_empty(pbc::empty_set_of(l).named, false, 1);
    PB_CHECK(c.shown_names[0].size() <= 40);
    PB_CHECK_EQ(c.shown_names[0].size() % 2, size_t(0));
}

PB_TEST(the_empty_set_is_exactly_the_actionable_items_in_order_and_counts_the_rest) {
    pbc::TrashList l = list_with(3, 1);
    pbc::TrashItem unreadable;
    unreadable.unreadable = true;
    l.items.insert(l.items.begin() + 1, unreadable);
    pbc::TrashItem locked = item("/Books", "locked.txt");
    locked.availability = pbc::Availability::NoPermission;
    l.items.push_back(locked);
    pbc::TrashItem enc = item("/Books", "enc", true);
    enc.availability = pbc::Availability::Encrypted;
    l.items.push_back(enc);
    const auto es = pbc::empty_set_of(l);
    PB_CHECK_EQ(es.named.size(), size_t(4));
    PB_CHECK_EQ(es.not_actionable, 3);
    PB_CHECK_EQ(es.named[0].name, std::string("f0.txt"));
    PB_CHECK_EQ(es.named[3].name, std::string("d0"));
    PB_CHECK(pbc::empty_set_of(pbc::TrashList()).named.empty());
}

PB_TEST(the_emptying_loop_goes_on_after_a_refusal_and_stops_for_a_login_a_pause_and_an_unknown_outcome) {
    pbc::OperationResult ok;
    PB_CHECK(pbc::loop_step_after(ok) == pbc::LoopStep::Continue);
    for (const char* key : {"forbidden", "locked", "server_error", "partial_failure", "trash_item_gone", "server_storage_full", "protocol_error"}) {
        pbc::OperationResult r;
        r.error = pbc::error_from_key(key);
        PB_CHECK(pbc::loop_step_after(r) == pbc::LoopStep::Continue);
    }
    pbc::OperationResult auth;
    auth.error = pbc::error_from_key("auth_failed");
    PB_CHECK(pbc::loop_step_after(auth) == pbc::LoopStep::StopAuth);
    pbc::OperationResult pause;
    pause.error = pbc::error_from_key("rate_limited");
    PB_CHECK(pbc::loop_step_after(pause) == pbc::LoopStep::StopRateLimited);
    pbc::OperationResult unknown;
    unknown.error = pbc::error_from_key("outcome_unknown");
    unknown.outcome_unknown = true;
    PB_CHECK(pbc::loop_step_after(unknown) == pbc::LoopStep::StopUnknown);
    pbc::OperationResult no_connection;
    no_connection.error = pbc::error_from_key("no_connection");
    PB_CHECK(pbc::loop_step_after(no_connection) == pbc::LoopStep::StopConnection);   // nothing was sent and the next request would fail the same way
    pbc::OperationResult lost_link;
    lost_link.error = pbc::error_from_key("server_unreachable");
    PB_CHECK(pbc::loop_step_after(lost_link) == pbc::LoopStep::StopConnection);
}

PB_TEST(the_delete_refusals_give_their_keys_and_codes) {
    struct Row { long status; const char* key; };
    for (const Row& r : {Row{403, "forbidden"}, Row{423, "locked"}, Row{429, "rate_limited"}, Row{500, "server_error"}, Row{207, "partial_failure"}, Row{401, "auth_failed"}}) {
        const auto f = pbc::refusal_for(8, r.status);
        PB_CHECK_EQ(f.key, std::string(r.key));
        PB_CHECK_EQ(f.code, "T8-" + std::to_string(r.status));
    }
}

PB_TEST_MAIN()
