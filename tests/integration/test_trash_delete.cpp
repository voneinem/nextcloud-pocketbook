// Deleting one item of the trash for good (specs/006-trash-and-versions, spec Story 4, FR-021 to FR-026, research R-7 and R-8, contracts/trash-requests.md T8), against the
// in-process fake Nextcloud: one DELETE of exactly that item by its unique name, never the collection, no condition header, a lost answer is unknown and never repeated.
// It proves the app follows the contract, never a real server (constitution Principle IX).
#include <string>

#include "test_support.h"
#include "trash_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::TrashEnv;

PB_TEST(one_delete_removes_exactly_the_named_item_and_sends_one_request) {
    TrashEnv e;
    const std::string a = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    const std::string b = e.server.add_trash_file("/Books/b.txt", "bb", e.t(200));
    const auto item = e.item(a);
    e.server.clear_log();
    const auto r = e.api.delete_item(item);
    PB_CHECK(r.error.ok() && !r.outcome_unknown);
    PB_CHECK_EQ(r.removed, 1);
    PB_CHECK_EQ(r.not_removed, 0);
    PB_CHECK(!e.server.trashbin_has(a) && e.server.trashbin_has(b));
    PB_CHECK_EQ(e.server.request_count(), size_t(1));
    const auto& l = e.server.log()[0];
    PB_CHECK_EQ(l.method, std::string("DELETE"));
    PB_CHECK(l.path.find("/remote.php/dav/trashbin/demo/trash/" + a) != std::string::npos);
    PB_CHECK(l.if_match.empty() && l.if_none_match.empty());   // the unique name is the condition (research R-7)
}

PB_TEST(a_folder_is_removed_with_its_content_in_one_request) {
    TrashEnv e;
    const std::string f = e.server.add_trash_folder("/Documents/Old Notes", e.t(100), {{"/a.txt", "a"}, {"/sub/b.txt", "b"}});
    const auto r = e.api.delete_item(e.item(f));
    PB_CHECK(r.error.ok());
    PB_CHECK_EQ(r.removed, 1);
    PB_CHECK_EQ(e.server.trashbin_count(), size_t(0));
    PB_CHECK_EQ(e.server.count("DELETE"), size_t(1));
}

PB_TEST(an_item_that_is_already_gone_counts_as_removed) {
    TrashEnv e;
    const std::string a = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    const auto item = e.item(a);
    pbc::Request del;   // another device did it first
    del.method = "DELETE";
    del.url = e.server.base_url() + "/remote.php/dav/trashbin/demo/trash/" + a;
    del.username = "demo";
    del.password = "pw";
    PB_CHECK_EQ(e.server.perform(del).status, 204L);
    const auto r = e.api.delete_item(item);
    PB_CHECK(r.error.ok());
    PB_CHECK_EQ(r.removed, 1);   // what was asked for has happened
    PB_CHECK_EQ(r.code, std::string("T8-404"));
}

PB_TEST(a_refused_delete_gives_its_key_and_its_code_and_the_item_stays) {
    struct Row { long status; const char* key; };
    for (const Row& row : {Row{403, "forbidden"}, Row{429, "rate_limited"}, Row{500, "server_error"}, Row{401, "auth_failed"}, Row{207, "partial_failure"}, Row{507, "server_storage_full"}}) {
        TrashEnv e;
        const std::string a = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
        const auto item = e.item(a);
        e.server.answer_next("DELETE", "/trashbin/", row.status);
        const auto r = e.api.delete_item(item);
        PB_CHECK_EQ(r.error.key, std::string(row.key));
        PB_CHECK_EQ(r.code, "T8-" + std::to_string(row.status));
        PB_CHECK_EQ(r.removed, 0);
        PB_CHECK_EQ(r.not_removed, 1);
        PB_CHECK(e.server.trashbin_has(a));
        PB_CHECK_EQ(e.server.count("DELETE"), size_t(1));   // one try
    }
}

PB_TEST(a_locked_item_gives_locked) {
    TrashEnv e;
    const std::string a = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    e.server.lock_trash_item(a);
    const auto r = e.api.delete_item(e.item(a));
    PB_CHECK_EQ(r.error.key, std::string("locked"));
    PB_CHECK_EQ(r.not_removed, 1);
    PB_CHECK(e.server.trashbin_has(a));
}

PB_TEST(a_lost_answer_is_unknown_and_the_delete_is_never_repeated) {
    TrashEnv e;
    const std::string a = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    const auto item = e.item(a);
    e.server.lose_next_answer("DELETE", "/trashbin/");
    e.server.clear_log();
    const auto r = e.api.delete_item(item);
    PB_CHECK(r.outcome_unknown);
    PB_CHECK_EQ(r.error.key, std::string("outcome_unknown"));
    PB_CHECK_EQ(r.removed, 0);
    PB_CHECK_EQ(r.not_removed, 0);   // not counted either way: nobody knows
    PB_CHECK_EQ(e.server.count("DELETE"), size_t(1));
    PB_CHECK_EQ(e.server.request_count(), size_t(1));
    PB_CHECK(!e.server.trashbin_has(a));   // the server did it; a refresh shows the truth
}

PB_TEST(an_unreachable_server_is_no_connection_and_not_unknown) {
    TrashEnv e;
    const std::string a = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    const auto item = e.item(a);
    e.server.set_offline(true);
    const auto r = e.api.delete_item(item);
    PB_CHECK_EQ(r.error.key, std::string("no_connection"));
    PB_CHECK(!r.outcome_unknown);
    PB_CHECK_EQ(r.not_removed, 1);
}

PB_TEST(an_item_that_cannot_be_acted_on_sends_nothing) {
    TrashEnv e;
    const std::string a = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    auto item = e.item(a);
    e.server.clear_log();
    item.unreadable = true;
    PB_CHECK_EQ(e.api.delete_item(item).error.key, std::string("trash_not_actionable"));
    item.unreadable = false;
    item.availability = pbc::Availability::NoPermission;
    PB_CHECK_EQ(e.api.delete_item(item).error.key, std::string("trash_not_actionable"));
    item.availability = pbc::Availability::Ok;
    item.trash_name = "../x";   // an unsafe name can never become an address
    PB_CHECK_EQ(e.api.delete_item(item).error.key, std::string("trash_not_actionable"));
    item.trash_name.clear();
    PB_CHECK_EQ(e.api.delete_item(item).error.key, std::string("trash_not_actionable"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(a_name_that_needs_encoding_is_sent_encoded_and_deletes_that_item_only) {
    TrashEnv e;
    const std::string odd = e.server.add_trash_file("/Books/My Story \xc3\xa4.epub", "x", e.t(100));
    const std::string other = e.server.add_trash_file("/Books/My Story.epub", "y", e.t(100));
    const auto r = e.api.delete_item(e.item(odd));
    PB_CHECK(r.error.ok());
    PB_CHECK(!e.server.trashbin_has(odd) && e.server.trashbin_has(other));
}

PB_TEST_MAIN()
