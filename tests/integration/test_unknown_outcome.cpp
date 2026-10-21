// A write whose answer did not arrive may have happened (specs/002-manage-server-files US2, SC-006, constitution Principle III, research R-7): the item
// is kept as "outcome unknown", no run repeats it, and the user chooses with the help of a read-only check. Fake Nextcloud.
#include <string>

#include "upload_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::UpEnv;

namespace {
constexpr long long kMiB = 1ll << 20;
}

PB_TEST(a_lost_answer_of_a_small_put_keeps_the_item_as_unknown_and_no_run_repeats_it) {
    UpEnv e;
    const auto ids = e.queue({e.source("Downloads/a.txt", 1000, 2)});
    e.server.lose_next_answer("PUT", "/Docs/");
    e.server.clear_log();
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.unknown, 1);
    PB_CHECK_EQ(rep.done, 0);
    PB_CHECK(e.item(ids[0]).state == pbc::ItemState::Unknown);
    PB_CHECK_EQ(e.item(ids[0]).reason, std::string("no_answer"));
    PB_CHECK_EQ(e.logged("PUT").size(), size_t(1));
    PB_CHECK(e.server.has("/Docs/a.txt"));   // the server did it
    // neither a run nor a run of that one item takes it again
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 0);
    PB_CHECK_EQ(e.run(ids[0]).done, 0);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(check_the_server_reports_what_it_finds_and_changes_nothing) {
    UpEnv e;
    const auto ids = e.queue({e.source("Downloads/a.txt", 1000, 2), e.source("Downloads/b.txt", 1000, 3), e.source("Downloads/c.txt", 1000, 4)});
    e.server.lose_next_answer("PUT", "/Docs/a.txt");
    e.run();
    // the first arrived; the second is set up as "not there" and the third as "something else is there"
    auto b = e.item(ids[1]);
    PB_CHECK(b.state == pbc::ItemState::Done);   // the others were sent normally
    e.server.remove("/Docs/b.txt");
    b.state = pbc::ItemState::Unknown;
    b.reason = "no_answer";
    PB_CHECK(e.store->queue_update(b).ok());
    auto c = e.item(ids[2]);
    c.state = pbc::ItemState::Unknown;
    c.reason = "no_answer";
    PB_CHECK(e.store->queue_update(c).ok());
    e.server.set_file("/Docs/c.txt", "different size", 1790000000);

    auto s = e.sender();
    e.server.clear_log();
    PB_CHECK(s->check_unknown(ids[0]) == pbc::Found::Arrived);
    PB_CHECK(s->check_unknown(ids[1]) == pbc::Found::NotThere);
    PB_CHECK(s->check_unknown(ids[2]) == pbc::Found::SomethingElse);
    for (const auto& l : e.server.log()) {
        PB_CHECK_EQ(l.method, std::string("PROPFIND"));
        PB_CHECK_EQ(l.depth, std::string("0"));
    }
    PB_CHECK(e.item(ids[0]).state == pbc::ItemState::Unknown);   // the check only reports
    // an item that is not unknown has nothing to check
    PB_CHECK(s->check_unknown(999) == pbc::Found::Unclear);
}

PB_TEST(a_check_without_a_connection_is_unclear_not_a_guess) {
    UpEnv e;
    const auto ids = e.queue({e.source("Downloads/a.txt", 1000)});
    e.server.lose_next_answer("PUT");
    e.run();
    e.server.set_offline(true);
    PB_CHECK(e.sender()->check_unknown(ids[0]) == pbc::Found::Unclear);
}

PB_TEST(marking_an_unknown_item_done_runs_the_effects_of_a_confirmation) {
    UpEnv e;
    pbc::Listing l;
    PB_CHECK(e.store->replace_listing("/Docs", l, 1000).ok());
    const auto ids = e.queue({e.source("Downloads/a.txt", 1000, 2)});
    e.server.lose_next_answer("PUT");
    e.run();
    PB_CHECK(e.sender()->mark_unknown_done(ids[0]).ok());
    const auto it = e.item(ids[0]);
    PB_CHECK(it.state == pbc::ItemState::Done);
    PB_CHECK_EQ(it.result_etag, e.server.etag_of("/Docs/a.txt"));
    const auto entry = e.store->entry("/Docs/a.txt");
    PB_CHECK(entry.has_value());
    PB_CHECK_EQ(entry->etag, e.server.etag_of("/Docs/a.txt"));
    PB_CHECK_EQ(e.store->folder("/Docs")->fetched_at.value_or(-1), 1000LL);
    // only an unknown item can be decided this way
    PB_CHECK(!e.sender()->mark_unknown_done(ids[0]).ok());
}

PB_TEST(sending_again_keeps_the_conditions_so_an_upload_that_arrived_ends_in_the_collision_question_not_in_a_copy) {
    UpEnv e;
    const std::string src = e.source("Downloads/a.txt", 1000, 2);
    const auto ids = e.queue({src});
    e.server.lose_next_answer("PUT");
    e.run();
    const std::string before = e.server.sha256_of("/Docs/a.txt");
    PB_CHECK(e.sender()->send_again(ids[0]).ok());
    PB_CHECK(e.item(ids[0]).state == pbc::ItemState::Queued);
    e.server.clear_log();
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.conflict, 1);
    PB_CHECK_EQ(e.item(ids[0]).reason, std::string("name_exists"));
    PB_CHECK_EQ(e.logged("PUT")[0].if_none_match, std::string("*"));
    PB_CHECK_EQ(e.server.sha256_of("/Docs/a.txt"), before);
    PB_CHECK_EQ(e.server.overwrites(), 0);
    // no second copy was made anywhere
    PB_CHECK(!e.server.has("/Docs/a (2).txt"));
}

PB_TEST(a_lost_answer_of_the_final_move_is_unknown_and_the_check_finds_the_file) {
    UpEnv e;
    const std::string src = e.source("Downloads/big.bin", 25 * kMiB, 4);
    const auto ids = e.queue({src});
    e.server.lose_next_answer("MOVE", "/.file");
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.unknown, 1);
    PB_CHECK(e.item(ids[0]).state == pbc::ItemState::Unknown);
    PB_CHECK(e.sender()->check_unknown(ids[0]) == pbc::Found::Arrived);
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 0);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    // sending again: the chunks are gone with the assembly, so they go up again, and the MOVE is refused by the pre-check
    PB_CHECK(e.sender()->send_again(ids[0]).ok());
    e.server.clear_log();
    PB_CHECK_EQ(e.run().conflict, 1);
    PB_CHECK_EQ(e.logged("MOVE").size(), size_t(0));
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/big.bin"));
}

PB_TEST(a_lost_answer_of_a_mkcol_holds_the_folder_and_what_waits_for_it_is_not_sent) {
    UpEnv e;
    e.source("Notes/trip/plan.md", 20);
    auto act = e.actions();
    const auto r = act.confirm_folder_pick(act.count_folder_pick("/Docs", {e.root + "/Notes/trip"}));
    PB_CHECK(r.error.ok());
    e.server.lose_next_answer("MKCOL", "/Docs/trip");
    e.server.clear_log();
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.unknown, 1);
    PB_CHECK_EQ(rep.left, 0);   // the upload waits for its folder
    PB_CHECK_EQ(e.logged("PUT").size(), size_t(0));
    PB_CHECK(e.item(r.queued[0]).state == pbc::ItemState::Unknown);
    PB_CHECK(e.sender()->check_unknown(r.queued[0]) == pbc::Found::Arrived);
    PB_CHECK(e.sender()->mark_unknown_done(r.queued[0]).ok());
    PB_CHECK_EQ(e.run().done, 1);   // now the file follows
    PB_CHECK_EQ(e.server.node("/Docs/trip/plan.md").content.size(), size_t(20));
}

PB_TEST(a_cut_body_is_unknown_because_the_request_may_have_been_acted_on) {
    UpEnv e;
    const auto a = e.queue({e.source("Downloads/a.txt", 3 * kMiB)});
    e.server.drop_after_bytes(1 * kMiB);
    PB_CHECK_EQ(e.run().unknown, 1);
    PB_CHECK(!e.server.has("/Docs/a.txt"));
    PB_CHECK(e.item(a[0]).state == pbc::ItemState::Unknown);
    PB_CHECK(e.sender()->check_unknown(a[0]) == pbc::Found::NotThere);
}

PB_TEST(a_gateway_error_on_a_write_is_unknown) {
    UpEnv e;
    const auto b = e.queue({e.source("Downloads/b.txt", 100)});
    e.server.answer_next("PUT", "/Docs/b.txt", 502);
    PB_CHECK_EQ(e.run().unknown, 1);
    PB_CHECK(e.item(b[0]).state == pbc::ItemState::Unknown);
}

PB_TEST(a_server_that_says_it_did_not_execute_the_request_is_a_failure_and_stops_the_run) {
    UpEnv e;
    const auto ids = e.queue({e.source("Downloads/a.txt", 100), e.source("Downloads/b.txt", 100)});
    e.server.answer_next("PUT", "/Docs/a.txt", 503);
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.stopped_by_key, std::string("maintenance"));
    PB_CHECK_EQ(rep.failed, 1);
    PB_CHECK_EQ(rep.unknown, 0);
    PB_CHECK_EQ(e.item(ids[0]).reason, std::string("maintenance"));
    PB_CHECK(e.item(ids[1]).state == pbc::ItemState::Queued);   // not tried: the run stopped

    UpEnv f;
    const auto g = f.queue({f.source("Downloads/a.txt", 100), f.source("Downloads/b.txt", 100)});
    f.server.answer_next("PUT", "/Docs/a.txt", 429, 1, "30");
    const pbc::SendReport limited = f.run();
    PB_CHECK_EQ(limited.stopped_by_key, std::string("rate_limited"));
    PB_CHECK(f.item(g[1]).state == pbc::ItemState::Queued);
    // and nothing sends again until the user taps again
    f.server.clear_log();
    PB_CHECK_EQ(f.run().done, 1);   // the tap: the first one is failed (held), the second goes
    PB_CHECK_EQ(f.logged("PUT").size(), size_t(1));
}

PB_TEST(a_refused_login_stops_the_run_and_keeps_the_item_queued) {
    UpEnv e;
    const auto ids = e.queue({e.source("Downloads/a.txt", 100)});
    e.server.answer_next("PUT", "/Docs/a.txt", 401);
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.stopped_by_key, std::string("auth_failed"));
    PB_CHECK(e.item(ids[0]).state == pbc::ItemState::Queued);
}

PB_TEST(removing_an_unknown_upload_with_an_upload_folder_deletes_that_folder_first) {
    UpEnv e;
    const auto ids = e.queue({e.source("Downloads/big.bin", 25 * kMiB)});
    e.server.lose_next_answer("MOVE", "/.file");
    // make the MOVE fail to take the folder: the answer is lost AND nothing was assembled
    e.server.before_next("MOVE", "/.file", [&] { e.server.answer_next("MOVE", "/.file", 502); });
    e.run();
    PB_CHECK(e.item(ids[0]).state == pbc::ItemState::Unknown);
    e.server.clear_log();
    PB_CHECK(e.sender()->remove_item(ids[0]).ok());
    PB_CHECK_EQ(e.logged("DELETE", "/dav/uploads/").size(), size_t(1));
    PB_CHECK(!e.store->queue_item(ids[0]).has_value());
    PB_CHECK_EQ(e.server.upload_sessions(), size_t(0));
}

PB_TEST_MAIN()
