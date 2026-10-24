// The refusals of a restore (specs/006-trash-and-versions, spec Story 2 scenario 6, FR-012, research R-11, contracts/trash-requests.md T4 and T5), against the in-process
// fake Nextcloud: each answer gives its plain key and a short code without a name, the item stays in the trash and nothing on the server changes. It proves the app
// follows the contract, never a real server (constitution Principle IX).
#include <string>

#include "test_support.h"
#include "trash_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::TrashEnv;

namespace {

struct Row {
    long status;
    const char* key;
};

}  // namespace

PB_TEST(a_refused_restore_to_the_original_place_gives_its_key_and_its_code_and_changes_nothing) {
    for (const Row& row : {Row{403, "forbidden"}, Row{507, "server_storage_full"}, Row{401, "auth_failed"}, Row{429, "rate_limited"}, Row{500, "server_error"},
                           Row{404, "trash_item_gone"}, Row{423, "locked"}}) {
        TrashEnv e;
        e.server.add_folder("/Books");
        const std::string name = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
        const auto item = e.item(name);
        e.server.answer_next("MOVE", "/trashbin/", row.status);
        const auto r = e.api.restore_original(item);
        PB_CHECK_EQ(r.error.key, std::string(row.key));
        PB_CHECK_EQ(r.code, "T4-" + std::to_string(row.status));
        PB_CHECK(!r.outcome_unknown);
        PB_CHECK(e.server.trashbin_has(name));
        PB_CHECK(!e.server.has("/Books/a.txt"));
        PB_CHECK_EQ(e.moves(), size_t(1));   // one try, never repeated by the app
    }
}

PB_TEST(a_locked_item_gives_locked_from_the_server_itself) {
    TrashEnv e;
    e.server.add_folder("/Books");
    const std::string name = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    e.server.lock_trash_item(name);
    const auto r = e.api.restore_original(e.item(name));
    PB_CHECK_EQ(r.error.key, std::string("locked"));
    PB_CHECK(e.server.trashbin_has(name));
}

PB_TEST(a_refused_restore_to_a_chosen_place_gives_its_key) {
    pbc::RestoreTarget t;
    t.folder = "/Other";
    t.name = "a.txt";
    for (const Row& row : {Row{507, "server_storage_full"}, Row{423, "locked"}, Row{404, "trash_item_gone"}, Row{500, "server_error"}, Row{429, "rate_limited"}}) {
        TrashEnv e;
        e.server.add_folder("/Other");
        const std::string name = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
        const auto item = e.item(name);
        e.server.answer_next("MOVE", "/trashbin/", row.status);
        const auto r = e.api.restore_to(item, t);
        PB_CHECK_EQ(r.error.key, std::string(row.key));
        PB_CHECK_EQ(r.code, "T5-" + std::to_string(row.status));
        PB_CHECK(e.server.trashbin_has(name));
        PB_CHECK(!e.server.has("/Other/a.txt"));
    }
}

PB_TEST(an_item_that_was_removed_elsewhere_meanwhile_leaves_the_trash_view_with_trash_item_gone) {
    TrashEnv e;
    e.server.add_folder("/Books");
    const std::string name = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    const auto item = e.item(name);
    pbc::Request del;   // another device deleted it for good
    del.method = "DELETE";
    del.url = e.server.base_url() + "/remote.php/dav/trashbin/demo/trash/" + name;
    del.username = "demo";
    del.password = "pw";
    PB_CHECK_EQ(e.server.perform(del).status, 204L);
    const auto r = e.api.restore_original(item);
    PB_CHECK_EQ(r.error.key, std::string("trash_item_gone"));
    PB_CHECK(!e.server.has("/Books/a.txt"));
}

PB_TEST(a_server_error_text_never_reaches_the_result) {
    TrashEnv e;
    e.server.add_folder("/Books");
    const std::string name = e.server.add_trash_file("/Books/secret-name.txt", "a", e.t(100));
    e.server.set_interceptor([](const pbc::Request& r) -> std::optional<pbc::Response> {
        if (r.method != "MOVE") return std::nullopt;
        pbc::Response resp;
        resp.status = 500;
        resp.body = "<?xml version=\"1.0\"?><d:error xmlns:d=\"DAV:\" xmlns:s=\"http://sabredav.org/ns\"><s:message>cannot move /Books/secret-name.txt for alice</s:message></d:error>";
        return resp;
    });
    const auto r = e.api.restore_original(e.item(name));
    PB_CHECK_EQ(r.error.key, std::string("server_error"));
    PB_CHECK(r.error.detail.find("secret-name") == std::string::npos && r.error.detail.find("alice") == std::string::npos);
    PB_CHECK(r.code.find("secret") == std::string::npos);
}

PB_TEST(an_unreachable_server_is_no_connection_not_an_unknown_outcome) {
    TrashEnv e;
    e.server.add_folder("/Books");
    const std::string name = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    const auto item = e.item(name);
    e.server.set_offline(true);
    const auto r = e.api.restore_original(item);
    PB_CHECK_EQ(r.error.key, std::string("no_connection"));   // nothing was sent: the user may simply try again
    PB_CHECK(!r.outcome_unknown);
}

PB_TEST_MAIN()
