// The app dies at every step of a send (specs/002-manage-server-files SC-002, SC-014; constitution Principle III). The death is simulated at the
// sender's test seam by an exception that unwinds the run, after which the data folder is opened anew: every state change is committed in its own
// transaction, so what is left is exactly what a SIGKILL leaves, and the fake server (whose state a forked child would lose) stays. After the restart
// recovery sends nothing, no item is lost or duplicated, "unknown" appears exactly for the steps that could have changed the server, and the server
// never shows a partial file.
#include <string>

#include "upload_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::UpEnv;

namespace {

constexpr long long kMiB = 1ll << 20;

struct Killed {};

// Runs until `step` (the n-th time) and dies there. Returns true if it died.
bool die_at(UpEnv& e, const std::string& step, int nth = 1) {
    int seen = 0;
    e.on_hook = [&](const char* s) {
        if (step == s && ++seen == nth) throw Killed();
    };
    bool died = false;
    try {
        e.run();
    } catch (const Killed&) {
        died = true;
    }
    e.on_hook = nullptr;
    return died;
}

}  // namespace

PB_TEST(death_while_preparing_requeues_and_the_next_run_sends_the_file_once) {
    UpEnv e;
    const std::string src = e.source("Downloads/a.txt", 1000, 5);
    const auto ids = e.queue({src});
    PB_CHECK(die_at(e, "sending"));
    PB_CHECK(e.item(ids[0]).state == pbc::ItemState::Sending);
    e.server.clear_log();
    const pbc::RecoveryReport rec = e.restart();
    PB_CHECK_EQ(rec.requeued, 1);
    PB_CHECK_EQ(rec.unknown, 0);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));   // recovery sends nothing
    PB_CHECK(e.item(ids[0]).state == pbc::ItemState::Queued);
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.logged("PUT").size(), size_t(1));
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/a.txt"));
}

PB_TEST(death_just_before_the_single_put_is_unknown_and_the_check_says_it_is_not_there) {
    UpEnv e;
    const std::string src = e.source("Downloads/a.txt", 1000, 5);
    const auto ids = e.queue({src});
    PB_CHECK(die_at(e, "request"));
    PB_CHECK(!e.server.has("/Docs/a.txt"));
    const pbc::RecoveryReport rec = e.restart();
    PB_CHECK_EQ(rec.unknown, 1);
    PB_CHECK(e.item(ids[0]).state == pbc::ItemState::Unknown);
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 0);   // an unknown item is skipped
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(e.sender()->check_unknown(ids[0]) == pbc::Found::NotThere);
    PB_CHECK(e.sender()->send_again(ids[0]).ok());
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/a.txt"));
}

PB_TEST(death_after_the_server_confirmed_but_before_the_row_did_is_unknown_and_the_check_finds_the_file) {
    UpEnv e;
    const std::string src = e.source("Downloads/a.txt", 1000, 5);
    const auto ids = e.queue({src});
    PB_CHECK(die_at(e, "confirm"));
    PB_CHECK(e.server.has("/Docs/a.txt"));
    PB_CHECK_EQ(e.restart().unknown, 1);
    PB_CHECK(e.sender()->check_unknown(ids[0]) == pbc::Found::Arrived);
    PB_CHECK(e.sender()->mark_unknown_done(ids[0]).ok());
    PB_CHECK(e.item(ids[0]).state == pbc::ItemState::Done);
    // one file, one PUT in all
    PB_CHECK_EQ(e.count("PUT"), size_t(1));
}

PB_TEST(death_in_the_middle_of_the_chunks_requeues_and_the_next_run_resumes) {
    UpEnv e;
    const std::string src = e.source("Downloads/big.bin", 25 * kMiB, 6);
    const auto ids = e.queue({src});
    PB_CHECK(die_at(e, "chunk", 2));   // chunk 1 is on the server, chunk 2 not yet
    PB_CHECK(!e.server.has("/Docs/big.bin"));
    const pbc::RecoveryReport rec = e.restart();
    PB_CHECK_EQ(rec.requeued, 1);
    PB_CHECK_EQ(rec.unknown, 0);
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.logged("PUT").size(), size_t(2));   // chunks 2 and 3
    PB_CHECK_EQ(e.logged("MKCOL").size(), size_t(0));
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/big.bin"));
    (void)ids;
}

PB_TEST(death_after_the_upload_id_was_written_but_before_the_folder_was_made_requeues) {
    UpEnv e;
    const std::string src = e.source("Downloads/big.bin", 25 * kMiB, 6);
    const auto ids = e.queue({src});
    PB_CHECK(die_at(e, "upload_id"));
    PB_CHECK(!e.item(ids[0]).upload_id.empty());   // written before the first request
    PB_CHECK_EQ(e.server.upload_sessions(), size_t(0));
    PB_CHECK_EQ(e.restart().requeued, 1);
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/big.bin"));
}

PB_TEST(death_at_the_final_move_is_unknown_the_chunks_stay_and_sending_again_assembles_them) {
    UpEnv e;
    const std::string src = e.source("Downloads/big.bin", 25 * kMiB, 6);
    const auto ids = e.queue({src});
    PB_CHECK(die_at(e, "assemble"));
    PB_CHECK(!e.server.has("/Docs/big.bin"));      // never a partial file in the user's folder
    PB_CHECK_EQ(e.restart().unknown, 1);
    PB_CHECK(e.sender()->check_unknown(ids[0]) == pbc::Found::NotThere);
    PB_CHECK(e.sender()->send_again(ids[0]).ok());
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.logged("PUT").size(), size_t(0));   // the three chunks were still there
    PB_CHECK_EQ(e.logged("MOVE").size(), size_t(1));
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/big.bin"));
}

PB_TEST(death_before_a_mkcol_is_unknown_and_what_waits_for_it_stays_queued_and_unsent) {
    UpEnv e;
    e.source("Notes/trip/plan.md", 20);
    auto act = e.actions();
    const auto r = act.confirm_folder_pick(act.count_folder_pick("/Docs", {e.root + "/Notes/trip"}));
    PB_CHECK(r.error.ok());
    PB_CHECK(die_at(e, "request"));
    PB_CHECK_EQ(e.restart().unknown, 1);
    PB_CHECK(e.item(r.queued[0]).state == pbc::ItemState::Unknown);
    PB_CHECK(e.item(r.queued[1]).state == pbc::ItemState::Queued);
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 0);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(every_item_is_still_there_after_a_death_with_a_full_queue_and_none_is_sent_twice) {
    UpEnv e;
    std::vector<std::string> srcs;
    for (int i = 0; i < 6; ++i) srcs.push_back(e.source("Downloads/f" + std::to_string(i) + ".txt", 100 + i, static_cast<uint8_t>(i)));
    const auto ids = e.queue(srcs);
    PB_CHECK(die_at(e, "confirm", 3));   // the third file reached the server and the row did not hear of it
    PB_CHECK_EQ(e.restart().unknown, 1);
    const pbc::SendReport rep = e.run();   // the rest goes on; the unknown one is skipped
    PB_CHECK_EQ(rep.done, 3);
    PB_CHECK_EQ(rep.unknown, 0);
    int done = 0, unknown = 0;
    for (int64_t id : ids) {
        const auto it = e.item(id);
        if (it.state == pbc::ItemState::Done) ++done;
        if (it.state == pbc::ItemState::Unknown) ++unknown;
    }
    PB_CHECK_EQ(done, 5);
    PB_CHECK_EQ(unknown, 1);
    PB_CHECK_EQ(e.count("PUT"), size_t(6));
}

PB_TEST_MAIN()
