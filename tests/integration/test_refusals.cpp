// When the server says no (specs/002-manage-server-files US7, FR-041 to FR-044, research R-12): every refusal ends the item with its own key, the item
// stays queued with its reason, nothing the server wrote is shown or logged, and nothing is retried by itself. Fake Nextcloud.
#include <string>

#include "pbc/log.h"
#include "upload_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::UpEnv;

PB_TEST(a_full_account_is_found_out_before_any_byte_is_sent_and_the_items_that_fit_still_go) {
    UpEnv e;
    e.server.set_quota_limit(5000);   // 5000 bytes free
    const auto big = e.queue({e.source("Downloads/big.bin", 10000)});
    const auto s1 = e.queue({e.source("Downloads/s1.bin", 2000)});
    const auto s2 = e.queue({e.source("Downloads/s2.bin", 2000)});
    const auto s3 = e.queue({e.source("Downloads/s3.bin", 2000)});   // 5000 - 2000 - 2000 = 1000 left: this one does not fit any more
    e.server.clear_log();
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.done, 2);
    PB_CHECK_EQ(rep.failed, 2);
    PB_CHECK_EQ(e.item(big[0]).reason, std::string("server_storage_full"));
    PB_CHECK_EQ(e.item(s3[0]).reason, std::string("server_storage_full"));
    PB_CHECK(e.item(s1[0]).state == pbc::ItemState::Done);
    PB_CHECK(e.item(s2[0]).state == pbc::ItemState::Done);
    PB_CHECK_EQ(e.logged("PUT").size(), size_t(2));   // nothing was sent for the two that cannot fit
    PB_CHECK_EQ(e.logged("PROPFIND", "/remote.php/dav/files/demo/").size(), size_t(1));   // the free space was asked once
}

PB_TEST(a_wrong_guess_about_the_free_space_is_the_servers_own_refusal) {
    UpEnv e;
    const auto id = e.queue({e.source("Downloads/a.bin", 100)});
    e.server.answer_next("PUT", "/Docs/a.bin", 507);
    PB_CHECK_EQ(e.run().failed, 1);
    PB_CHECK_EQ(e.item(id[0]).reason, std::string("server_storage_full"));
}

PB_TEST(the_size_the_type_and_the_name_the_server_refuses_each_have_their_own_key) {
    UpEnv e;
    e.server.set_max_file_size(1000);
    e.server.set_blocked_extensions({".exe"});
    const auto big = e.queue({e.source("Downloads/big.bin", 5000)});
    const auto exe = e.queue({e.source("Downloads/setup.exe", 100)});
    e.server.set_forbidden("", {"con.txt"});
    e.server.set_state_forbidden_in_caps(false);   // the server enforces the lists but does not state them: the reader cannot know
    const auto bad = e.queue({e.source("Downloads/con.txt", 100)});
    const auto fine = e.queue({e.source("Downloads/fine.txt", 100)});
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.failed, 3);
    PB_CHECK_EQ(rep.done, 1);
    PB_CHECK_EQ(e.item(big[0]).reason, std::string("file_too_large_server"));
    PB_CHECK_EQ(e.item(exe[0]).reason, std::string("type_not_accepted"));
    PB_CHECK_EQ(e.item(bad[0]).reason, std::string("name_forbidden"));
    PB_CHECK(e.item(fine[0]).state == pbc::ItemState::Done);
    for (int64_t id : {big[0], exe[0], bad[0]}) PB_CHECK(e.item(id).state == pbc::ItemState::Failed);   // held, not repeated
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 0);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(a_refused_name_can_be_edited_and_goes_again_as_a_new_file) {
    UpEnv e;
    e.server.set_forbidden("", {"con.txt"});
    e.server.set_state_forbidden_in_caps(false);
    const std::string src = e.source("Downloads/con.txt", 100, 4);
    const auto id = e.queue({src});
    PB_CHECK_EQ(e.run().failed, 1);
    auto s = e.sender();
    PB_CHECK_EQ(s->resolve_edit(id[0], "bad/name").key, std::string("name_forbidden"));   // the new name is checked before it is queued
    PB_CHECK(e.item(id[0]).state == pbc::ItemState::Failed);
    PB_CHECK(s->resolve_edit(id[0], "console.txt").ok());
    PB_CHECK(e.item(id[0]).state == pbc::ItemState::Queued);
    PB_CHECK_EQ(e.item(id[0]).dst_name, std::string("console.txt"));
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/console.txt"));
}

PB_TEST(a_refusal_without_a_known_reason_is_not_allowed_and_an_unknown_status_shows_only_its_number) {
    UpEnv e;
    const auto a = e.queue({e.source("Downloads/a.txt", 10)});
    const auto b = e.queue({e.source("Downloads/b.txt", 10)});
    e.server.answer_next("PUT", "/Docs/a.txt", 403);   // answer_next holds one rule at a time: one run each
    PB_CHECK_EQ(e.run(a[0]).failed, 1);
    PB_CHECK_EQ(e.item(a[0]).reason, std::string("not_allowed"));
    e.server.answer_next("PUT", "/Docs/b.txt", 418);
    PB_CHECK_EQ(e.run(b[0]).failed, 1);
    PB_CHECK_EQ(e.item(b[0]).reason, std::string("server_refused"));
}

PB_TEST(a_locked_file_is_never_retried_by_the_app) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "x");
    e.cache("/Docs");
    e.server.lock("/Docs/a.txt");
    const pbc::AddResult r = e.actions().add_uploads("/Docs", {e.source("Downloads/a.txt", 100)});
    const pbc::AddResult q = e.actions().answer_ask(r.asks[0], pbc::AskChoice::Replace);
    PB_CHECK_EQ(e.run().failed, 1);
    PB_CHECK_EQ(e.item(q.queued[0]).reason, std::string("locked"));
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 0);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(a_refused_login_in_the_middle_stops_the_run_and_leaves_the_queue_untouched) {
    UpEnv e;
    const auto ids = e.queue({e.source("Downloads/a.txt", 10), e.source("Downloads/b.txt", 10), e.source("Downloads/c.txt", 10)});
    e.server.answer_next("PUT", "/Docs/b.txt", 401, 5);   // every further request is refused
    e.server.clear_log();
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.done, 1);
    PB_CHECK_EQ(rep.stopped_by_key, std::string("auth_failed"));
    PB_CHECK(e.item(ids[1]).state == pbc::ItemState::Queued);   // not failed: nothing is wrong with the item
    PB_CHECK(e.item(ids[2]).state == pbc::ItemState::Queued);
    PB_CHECK_EQ(e.logged("PUT").size(), size_t(2));             // no third request after the refusal
}

PB_TEST(names_paths_and_server_texts_never_reach_the_log) {
    std::vector<std::string> lines;
    pbc::log::set_sink([&](const std::string& line) { lines.push_back(line); });
    {
        UpEnv e;
        e.server.set_max_file_size(10);
        e.queue({e.source("Downloads/secret-name-zzz.txt", 500)});
        e.queue({e.source("Downloads/other-secret-qqq.txt", 5)});
        e.run();
        e.server.add_file("/Docs/lost-secret.txt", "x");
        e.cache("/Docs");
        e.server.lose_next_answer("PUT", "other");
    }
    pbc::log::reset_sink();
    PB_CHECK(!lines.empty());
    for (const std::string& l : lines) {
        PB_CHECK(l.find("secret") == std::string::npos);
        PB_CHECK(l.find("/Docs") == std::string::npos);
        PB_CHECK(l.find("Downloads") == std::string::npos);
        PB_CHECK(l.find("https://") == std::string::npos);
        PB_CHECK(l.find("demo") == std::string::npos);
    }
}

PB_TEST(an_item_whose_target_folder_is_gone_can_be_given_another_target_and_what_waits_for_a_new_folder_follows_it) {
    UpEnv e;
    e.server.add_folder("/Docs/Other");
    e.cache("/Docs");
    // the parent does not exist on the server: the new folder fails, its upload waits
    const auto folder = e.actions().new_folder("/Docs/Missing", "Trips");
    const auto up = e.actions().add_uploads("/Docs/Missing/Trips", {e.source("Downloads/a.txt", 100)});
    PB_CHECK_EQ(e.run().failed, 1);
    PB_CHECK_EQ(e.item(folder.queued[0]).reason, std::string("target_gone"));
    auto s = e.sender();
    PB_CHECK_EQ(s->resolve_retarget(folder.queued[0], "relative").key, std::string("protocol_error"));
    PB_CHECK(s->resolve_retarget(folder.queued[0], "/Docs/Other").ok());
    PB_CHECK_EQ(e.item(folder.queued[0]).dst_parent, std::string("/Docs/Other"));
    PB_CHECK(e.item(folder.queued[0]).state == pbc::ItemState::Queued);
    PB_CHECK_EQ(e.item(up.queued[0]).dst_parent, std::string("/Docs/Other/Trips"));   // the upload followed its folder
    PB_CHECK_EQ(e.run().done, 2);
    PB_CHECK_EQ(e.server.node("/Docs/Other/Trips/a.txt").content.size(), size_t(100));
}

PB_TEST(a_new_target_where_the_name_is_taken_or_for_an_item_that_is_not_held_is_refused) {
    UpEnv e;
    e.server.add_folder("/Docs/Other");
    e.server.add_file("/Docs/Other/a.txt", "x");
    e.cache("/Docs");
    e.cache("/Docs/Other");
    const auto up = e.actions().add_uploads("/Missing", {e.source("Downloads/a.txt", 100)});
    PB_CHECK_EQ(e.sender()->resolve_retarget(up.queued[0], "/Docs/Other").key, std::string("protocol_error"));   // still queued, not held
    PB_CHECK_EQ(e.run().failed, 1);
    PB_CHECK_EQ(e.sender()->resolve_retarget(up.queued[0], "/Docs/Other").key, std::string("name_exists"));
    PB_CHECK(e.item(up.queued[0]).state == pbc::ItemState::Failed);
}

PB_TEST_MAIN()
