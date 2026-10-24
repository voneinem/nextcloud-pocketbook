// A restore whose answer never arrived (specs/006-trash-and-versions, spec Story 2 scenario 7, FR-013, research R-10), against the in-process fake Nextcloud: the outcome is
// unknown, the app does not repeat the request, and what really happened is found by listing the trash and the folder. It proves the app follows the contract, never a
// real server (constitution Principle IX).
#include <string>

#include "test_support.h"
#include "trash_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::TrashEnv;

PB_TEST(a_lost_answer_of_a_restore_to_the_original_place_is_unknown_and_never_repeated) {
    TrashEnv e;
    e.server.add_folder("/Books");
    const std::string name = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    const auto item = e.item(name);
    e.server.lose_next_answer("MOVE", "/trashbin/");
    e.server.clear_log();
    const auto r = e.api.restore_original(item);
    PB_CHECK(r.outcome_unknown);
    PB_CHECK_EQ(r.error.key, std::string("outcome_unknown"));
    PB_CHECK_EQ(e.moves(), size_t(1));
    PB_CHECK_EQ(e.server.request_count(), size_t(1));   // no verifying probe, no second MOVE: the app does not guess
    // what really happened is on the server: the restore was done
    PB_CHECK_EQ(e.server.content_of("/Books/a.txt"), std::string("a"));
    const auto l = e.api.list_trash();
    PB_CHECK(l.ok() && l.value.items.empty());   // a refresh shows the truth
    PB_CHECK_EQ(e.moves(), size_t(1));
}

PB_TEST(a_lost_answer_of_a_restore_to_a_chosen_place_is_unknown_and_never_repeated) {
    TrashEnv e;
    e.server.add_folder("/Other");
    const std::string name = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    const auto item = e.item(name);
    e.server.lose_next_answer("MOVE", "/trashbin/");
    pbc::RestoreTarget t;
    t.folder = "/Other";
    t.name = "a.txt";
    e.server.clear_log();
    const auto r = e.api.restore_to(item, t);
    PB_CHECK(r.outcome_unknown);
    PB_CHECK_EQ(r.error.key, std::string("outcome_unknown"));
    PB_CHECK_EQ(e.moves(), size_t(1));
    PB_CHECK_EQ(e.server.request_count(), size_t(1));
    PB_CHECK_EQ(e.server.content_of("/Other/a.txt"), std::string("a"));
}

PB_TEST(a_lost_answer_that_is_a_refusal_in_disguise_is_just_as_unknown) {
    TrashEnv e;
    e.server.add_folder("/Books");
    const std::string name = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    e.server.add_file("/Books/a.txt", "taken");   // the restore the server executes is refused, but the answer is lost too
    const auto item = e.item(name);
    e.server.lose_next_answer("MOVE", "/trashbin/");
    const auto r = e.api.restore_original(item);
    PB_CHECK(r.outcome_unknown);   // the app cannot know: the user checks
    PB_CHECK(e.server.trashbin_has(name));
}

PB_TEST(a_connection_that_broke_after_the_request_was_sent_is_unknown_and_one_that_never_started_is_not) {
    TrashEnv e;
    e.server.add_folder("/Books");
    const std::string name = e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    const auto item = e.item(name);
    e.server.set_interceptor([](const pbc::Request&) -> std::optional<pbc::Response> {
        pbc::Response r;
        r.error = pbc::ErrorClass::Network;
        r.error_key = "connection_lost";
        r.delivery = pbc::Delivery::PartlySent;
        return r;
    });
    PB_CHECK(e.api.restore_original(item).outcome_unknown);
    e.server.set_interceptor([](const pbc::Request&) -> std::optional<pbc::Response> {
        pbc::Response r;
        r.error = pbc::ErrorClass::Network;
        r.error_key = "server_unreachable";
        r.delivery = pbc::Delivery::NotSent;
        return r;
    });
    const auto r = e.api.restore_original(item);
    PB_CHECK(!r.outcome_unknown);
    PB_CHECK_EQ(r.error.key, std::string("no_connection"));
}

PB_TEST_MAIN()
