// Update (spec US4, FR-024..FR-028; research R-10): started by the user only, it walks what is kept offline, downloads what is new or
// changed, leaves a conflict and the user's changes alone, flags what vanished on the server, and can be repeated after an interruption.
#include <string>
#include <vector>

#include "download_env.h"
#include "pbc/folder_service.h"
#include "pbc/update.h"

PBC_FAKE_SERVER_TEST();

using pbtest::DlEnv;

namespace {

std::string pattern(size_t n, int seed) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 31 + static_cast<size_t>(seed)) & 0xff);
    return s;
}

struct UEnv : DlEnv {
    std::unique_ptr<pbc::Updater> up;
    UEnv() {
        server.add_file("/Books/a.epub", pattern(3000, 1), 1790000000);
        server.add_file("/Books/Sub/b.epub", pattern(2000, 2), 1790000000);
        server.add_file("/Books/Sub/Deep/c.epub", pattern(1000, 3), 1790000000);
        server.add_file("/other/x.epub", pattern(500, 4), 1790000000);
        server.add_file("/readme.txt", "read me", 1790000000);
        up = std::make_unique<pbc::Updater>(*store, server, *dl, storage, clock);
        server.clear_log();
    }
    pbc::UpdateReport update() {
        pbtest::NoCancel c;
        return up->run(c, [](const pbc::UpdateReport&) {});
    }
    size_t gets() { return server.count("GET"); }
};

}  // namespace

PB_TEST(constructing_an_updater_and_marking_things_sends_nothing) {
    UEnv e;
    PB_CHECK(e.store->set_mark("/Books", pbc::Kind::Folder, true).ok());
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(a_first_update_downloads_the_marked_file_and_everything_below_the_marked_folder_and_nothing_else) {
    UEnv e;
    PB_CHECK(e.store->set_mark("/Books", pbc::Kind::Folder, true).ok());
    PB_CHECK(e.store->set_mark("/readme.txt", pbc::Kind::File, true).ok());
    const auto r = e.update();
    PB_CHECK_EQ(r.folders, 4);    // /Books, /Books/Sub, /Books/Sub/Deep and the parent of the marked file (/)
    PB_CHECK_EQ(r.files, 4);
    PB_CHECK_EQ(r.added, 4);
    PB_CHECK_EQ(r.updated, 0);
    PB_CHECK_EQ(r.failed, 0);
    PB_CHECK(r.stopped_by_key.empty());
    PB_CHECK(e.read("Books/a.epub") == pattern(3000, 1));
    PB_CHECK(e.read("Books/Sub/Deep/c.epub") == pattern(1000, 3));
    PB_CHECK_EQ(e.read("readme.txt"), std::string("read me"));
    PB_CHECK(!e.exists("other/x.epub"));
    PB_CHECK_EQ(e.store->local_file_count(), 4);
    for (const auto& d : e.server.depths()) PB_CHECK_EQ(d, std::string("1"));
}

PB_TEST(folders_that_the_user_never_opened_are_not_cached_by_an_update) {
    UEnv e;
    PB_CHECK(e.store->set_mark("/Books", pbc::Kind::Folder, true).ok());
    e.update();
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM folders"), std::string("0"));   // Principle V: only visited folders are cached
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM entries"), std::string("0"));
}

PB_TEST(a_folder_that_was_cached_before_is_refreshed_like_an_explicit_refresh) {
    UEnv e;
    PB_CHECK(e.store->set_mark("/Books", pbc::Kind::Folder, true).ok());
    pbc::FolderService svc(*e.store, e.server, e.clock);
    PB_CHECK(svc.load("/Books", pbc::Fetch::Always).error.ok());
    e.server.add_file("/Books/new.epub", pattern(100, 9), 1790000500);
    e.server.clear_log();
    e.clock.t += 120;
    e.update();
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM folders"), std::string("1"));   // still only the one the user had opened
    PB_CHECK(e.store->entry("/Books/new.epub").has_value());   // and its rows are current
    PB_CHECK_EQ(*e.store->folder("/Books")->fetched_at, e.clock.t);
}

PB_TEST(a_second_update_without_changes_downloads_nothing) {
    UEnv e;
    PB_CHECK(e.store->set_mark("/Books", pbc::Kind::Folder, true).ok());
    e.update();
    e.server.clear_log();
    const auto r = e.update();
    PB_CHECK_EQ(r.skipped, 3);
    PB_CHECK_EQ(r.added + r.updated, 0);
    PB_CHECK_EQ(e.gets(), size_t(0));
}

PB_TEST(only_what_changed_or_is_new_is_downloaded) {
    UEnv e;
    PB_CHECK(e.store->set_mark("/Books", pbc::Kind::Folder, true).ok());
    e.update();
    e.server.set_file("/Books/a.epub", pattern(3500, 8), 1790009000);              // changed
    e.server.add_file("/Books/Sub/Deep/d.epub", pattern(400, 5), 1790009100);      // new, deep inside
    e.server.clear_log();
    const auto r = e.update();
    PB_CHECK_EQ(r.updated, 1);
    PB_CHECK_EQ(r.added, 1);
    PB_CHECK_EQ(r.skipped, 2);
    PB_CHECK_EQ(e.gets(), size_t(2));
    PB_CHECK(e.read("Books/a.epub") == pattern(3500, 8));
    PB_CHECK(e.read("Books/Sub/Deep/d.epub") == pattern(400, 5));
}

PB_TEST(twenty_kept_files_three_changed_downloads_exactly_those_three) {
    UEnv e;
    for (int i = 0; i < 20; ++i) e.server.add_file("/Kept/f" + std::to_string(i) + ".epub", pattern(500 + static_cast<size_t>(i), i), 1790000000);
    PB_CHECK(e.store->set_mark("/Kept", pbc::Kind::Folder, true).ok());
    e.update();
    for (int i : {3, 9, 17}) e.server.set_file("/Kept/f" + std::to_string(i) + ".epub", pattern(700 + static_cast<size_t>(i), 50 + i), 1790009000);
    e.server.clear_log();
    const auto r = e.update();
    PB_CHECK_EQ(r.files, 20);        // 20 checked
    PB_CHECK_EQ(r.updated, 3);       // 3 updated
    PB_CHECK_EQ(r.skipped, 17);
    PB_CHECK_EQ(e.gets(), size_t(3));
}

PB_TEST(a_copy_changed_on_the_reader_and_on_the_server_is_a_conflict_that_is_left_alone) {
    UEnv e;
    PB_CHECK(e.store->set_mark("/Books", pbc::Kind::Folder, true).ok());
    e.update();
    e.write_local("Books/a.epub", "my notes");
    e.server.set_file("/Books/a.epub", pattern(3500, 8), 1790009000);
    e.server.clear_log();
    const auto r = e.update();
    PB_CHECK_EQ(r.need_decision, 1);
    PB_CHECK_EQ(r.updated, 0);
    PB_CHECK_EQ(e.read("Books/a.epub"), std::string("my notes"));
    PB_CHECK_EQ(e.gets(), size_t(0));
    PB_CHECK_EQ(r.decision_paths.size(), size_t(1));
    PB_CHECK_EQ(r.decision_paths[0], std::string("/Books/a.epub"));
}

PB_TEST(a_copy_the_user_changed_while_the_server_stayed_the_same_is_just_skipped) {
    UEnv e;
    PB_CHECK(e.store->set_mark("/Books", pbc::Kind::Folder, true).ok());
    e.update();
    e.write_local("Books/a.epub", "my notes");
    e.server.clear_log();
    const auto r = e.update();
    PB_CHECK_EQ(r.need_decision, 0);
    PB_CHECK_EQ(r.skipped, 3);
    PB_CHECK_EQ(e.read("Books/a.epub"), std::string("my notes"));
}

PB_TEST(what_vanished_on_the_server_is_flagged_and_its_copy_stays) {
    UEnv e;
    PB_CHECK(e.store->set_mark("/Books", pbc::Kind::Folder, true).ok());
    e.update();
    e.server.remove("/Books/Sub/b.epub");
    e.server.remove("/Books/Sub/Deep");   // a whole folder
    const auto r = e.update();
    PB_CHECK_EQ(r.removed, 2);
    PB_CHECK(e.store->local_file("/Books/Sub/b.epub")->server_missing);
    PB_CHECK(e.store->local_file("/Books/Sub/Deep/c.epub")->server_missing);
    PB_CHECK(!e.store->local_file("/Books/a.epub")->server_missing);
    PB_CHECK(e.exists("Books/Sub/b.epub"));          // nothing is deleted on the reader
    PB_CHECK(e.exists("Books/Sub/Deep/c.epub"));
    // and when it comes back the flag goes
    e.server.add_file("/Books/Sub/b.epub", pattern(2000, 2), 1790000000);
    e.update();
    PB_CHECK(!e.store->local_file("/Books/Sub/b.epub")->server_missing);
}

PB_TEST(a_kept_folder_that_is_gone_flags_the_copies_below_it) {
    UEnv e;
    PB_CHECK(e.store->set_mark("/Books", pbc::Kind::Folder, true).ok());
    e.update();
    e.server.remove("/Books");
    const auto r = e.update();
    PB_CHECK_EQ(r.removed, 3);
    PB_CHECK(e.store->local_file("/Books/a.epub")->server_missing);
    PB_CHECK(e.exists("Books/a.epub"));
}

PB_TEST(at_most_500_folders_per_update_and_the_rest_is_reported) {
    UEnv e;
    for (int i = 0; i < 500; ++i) e.server.add_folder("/Big/f" + std::to_string(i));
    PB_CHECK(e.store->set_mark("/Big", pbc::Kind::Folder, true).ok());
    const auto r = e.update();
    PB_CHECK_EQ(r.folders, 500);              // /Big and 499 of its 500 subfolders
    PB_CHECK_EQ(r.unchecked_folders, 1);
}

PB_TEST(a_chain_of_folders_deeper_than_20_is_not_followed_further) {
    UEnv e;
    std::string path = "/Deep";
    for (int i = 0; i < 25; ++i) path += "/d";
    e.server.add_file(path + "/leaf.epub", pattern(10, 1), 1790000000);
    PB_CHECK(e.store->set_mark("/Deep", pbc::Kind::Folder, true).ok());
    const auto r = e.update();
    PB_CHECK(r.unchecked_folders >= 1);
    PB_CHECK_EQ(r.added, 0);
}

PB_TEST(one_file_that_fails_does_not_stop_the_others_but_a_general_problem_does) {
    {
        UEnv e;
        PB_CHECK(e.store->set_mark("/Books", pbc::Kind::Folder, true).ok());
        e.server.fail_next(403, 1, "/Books/a.epub");
        const auto r = e.update();
        PB_CHECK_EQ(r.failed, 1);
        PB_CHECK_EQ(r.added, 2);
        PB_CHECK(r.stopped_by_key.empty());
    }
    {
        UEnv e;
        PB_CHECK(e.store->set_mark("/Books", pbc::Kind::Folder, true).ok());
        e.storage.free = 1ll << 20;   // no room for any of them
        const auto r = e.update();
        PB_CHECK_EQ(r.stopped_by_key, std::string("not_enough_space"));
        PB_CHECK_EQ(r.added, 0);
        PB_CHECK_EQ(e.gets(), size_t(0));   // refused before any byte, and the rest is not even tried
    }
    {
        UEnv e;
        PB_CHECK(e.store->set_mark("/Books", pbc::Kind::Folder, true).ok());
        e.server.set_maintenance(true);
        const auto r = e.update();
        PB_CHECK_EQ(r.stopped_by_key, std::string("maintenance"));
        PB_CHECK_EQ(r.added, 0);
    }
    {
        UEnv e;
        PB_CHECK(e.store->set_mark("/Books", pbc::Kind::Folder, true).ok());
        e.server.set_account("demo", "demo", "changed");   // the app password no longer works
        const auto r = e.update();
        PB_CHECK_EQ(r.stopped_by_key, std::string("auth_failed"));
    }
}

PB_TEST(a_repeated_update_after_an_interruption_ends_as_an_uninterrupted_one_and_downloads_nothing_twice) {
    UEnv e;
    PB_CHECK(e.store->set_mark("/Books", pbc::Kind::Folder, true).ok());
    // the card fills up after the second file
    int committed = 0;
    pbc::DownloaderOptions o;
    o.delays_s = {1};
    o.sleep_ms = [](int) {};
    o.hook = [&](const char* step) {
        if (std::string(step) == "committed" && ++committed == 2) e.storage.free = 1ll << 20;
    };
    pbc::Downloader dl2(*e.store, e.server, e.storage, e.clock, e.root, [] { return true; }, o);
    pbc::Updater up2(*e.store, e.server, dl2, e.storage, e.clock);
    pbtest::NoCancel c;
    auto first = up2.run(c, [](const pbc::UpdateReport&) {});
    PB_CHECK_EQ(first.added, 2);
    PB_CHECK_EQ(first.stopped_by_key, std::string("not_enough_space"));
    PB_CHECK_EQ(e.store->local_file_count(), 2);
    e.storage.free = 1ll << 40;
    e.server.clear_log();
    auto second = up2.run(c, [](const pbc::UpdateReport&) {});
    PB_CHECK_EQ(second.added, 1);        // only the one that was missing
    PB_CHECK_EQ(second.skipped, 2);      // the two that were done are not fetched again
    PB_CHECK_EQ(e.gets(), size_t(1));
    PB_CHECK(e.read("Books/a.epub") == pattern(3000, 1));
    PB_CHECK(e.read("Books/Sub/b.epub") == pattern(2000, 2));
    PB_CHECK(e.read("Books/Sub/Deep/c.epub") == pattern(1000, 3));
    PB_CHECK(e.store->journal().empty());
    PB_CHECK(!e.any_part_file());
}

PB_TEST(a_cancel_stops_the_update_and_the_report_says_so) {
    UEnv e;
    PB_CHECK(e.store->set_mark("/Books", pbc::Kind::Folder, true).ok());
    pbtest::CancelFlag c;
    c.flag = true;
    const auto r = e.up->run(c, [](const pbc::UpdateReport&) {});
    PB_CHECK_EQ(r.stopped_by_key, std::string("cancelled"));
    PB_CHECK_EQ(r.added, 0);
    PB_CHECK_EQ(e.store->local_file_count(), 0);
}

PB_TEST(the_plan_tells_the_totals_before_anything_is_downloaded) {
    UEnv e;
    PB_CHECK(e.store->set_mark("/Books", pbc::Kind::Folder, true).ok());
    pbtest::NoCancel c;
    auto plan = e.up->plan(c);
    PB_CHECK_EQ(plan.downloads.size(), size_t(3));
    PB_CHECK_EQ(plan.bytes, int64_t(3000 + 2000 + 1000));
    PB_CHECK_EQ(e.gets(), size_t(0));
    PB_CHECK_EQ(e.store->local_file_count(), 0);
    const auto r = e.up->execute(plan, c, [](const pbc::UpdateReport&) {});
    PB_CHECK_EQ(r.added, 3);
}

PB_TEST(the_result_is_recorded_for_the_home_page) {
    UEnv e;
    PB_CHECK(e.store->set_mark("/Books", pbc::Kind::Folder, true).ok());
    e.update();
    const auto st = e.store->sync_state();
    PB_CHECK_EQ(st.outcome, std::string("success"));
    PB_CHECK(st.counts_json.find("\"added\":3") != std::string::npos);
    PB_CHECK(st.last_success.has_value());
    PB_CHECK_EQ(st.problem_area, std::string(""));
}

PB_TEST_MAIN()
