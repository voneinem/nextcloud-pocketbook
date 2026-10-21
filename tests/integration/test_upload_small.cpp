// A file that fits one request is sent with one PUT (specs/002-manage-server-files US2, contracts/server-requests.md W3): its conditions, headers and
// effects, an upload into a folder that is queued too, and what happens when that folder cannot be made. Fake Nextcloud: this proves the app against
// the contract, not against a real server.
#include <string>

#include "upload_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::UpEnv;

PB_TEST(a_small_file_is_one_put_with_the_conditions_and_headers_of_the_contract_and_arrives_identical) {
    UpEnv e;
    const std::string src = e.source("Downloads/report.pdf", 300 * 1024, 5);
    const auto ids = e.queue({src});
    e.server.clear_log();
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.done, 1);
    PB_CHECK_EQ(rep.failed + rep.conflict + rep.unknown, 0);
    PB_CHECK(rep.stopped_by_key.empty());
    PB_CHECK_EQ(rep.left, 0);
    const auto puts = e.logged("PUT");
    PB_CHECK_EQ(puts.size(), size_t(1));
    PB_CHECK_EQ(puts[0].if_none_match, std::string("*"));
    PB_CHECK(puts[0].if_match.empty());
    PB_CHECK(!puts[0].mtime_header.empty());
    PB_CHECK_EQ(puts[0].checksum_header, "SHA256:" + e.sha_of(src));
    PB_CHECK_EQ(puts[0].body_bytes, 300LL * 1024);
    PB_CHECK_EQ(e.logged("MKCOL").size(), size_t(0));      // no upload folder for a small file
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/report.pdf"));
    PB_CHECK_EQ(e.server.node("/Docs/report.pdf").content.size(), size_t(300 * 1024));
    const auto it = e.item(ids[0]);
    PB_CHECK(it.state == pbc::ItemState::Done);
    PB_CHECK_EQ(it.result_etag, e.server.etag_of("/Docs/report.pdf"));
    PB_CHECK(it.result_fileid.has_value());
    PB_CHECK(it.step.empty());
}

PB_TEST(the_row_is_sending_before_the_request_and_done_in_the_same_step_as_its_effects) {
    UpEnv e;
    const auto ids = e.queue({e.source("Downloads/a.txt", 100)});
    std::vector<std::string> states;
    e.on_hook = [&](const char* step) { states.push_back(std::string(step) + ":" + pbc::item_state_text(e.item(ids[0]).state) + ":" + e.item(ids[0]).step); };
    e.run();
    // the hooks see the committed row: sending/prepare, then sending/single before the request, then the confirmation still in sending
    bool saw_request = false;
    for (const std::string& s : states)
        if (s == "request:sending:single") saw_request = true;
    PB_CHECK(saw_request);
    PB_CHECK(e.item(ids[0]).state == pbc::ItemState::Done);
}

PB_TEST(without_an_etag_header_the_version_comes_from_a_propfind) {
    UpEnv e;
    e.server.set_answer_etag_headers(false);
    const auto ids = e.queue({e.source("Downloads/a.txt", 100)});
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.logged("PROPFIND", "/Docs/a.txt").size(), size_t(1));   // the version of the new file (the free space was asked as well)
    PB_CHECK_EQ(e.logged("PROPFIND", "/Docs/a.txt")[0].depth, std::string("0"));
    PB_CHECK_EQ(e.item(ids[0]).result_etag, e.server.etag_of("/Docs/a.txt"));
    PB_CHECK(e.item(ids[0]).result_fileid.has_value());
}

PB_TEST(a_confirmed_upload_enters_the_cached_listing_without_changing_its_age) {
    UpEnv e;
    pbc::Listing l;
    pbc::RemoteEntry existing;
    existing.name = "old.txt";
    existing.path = "/Docs/old.txt";
    existing.etag = "\"x\"";
    l.entries.push_back(existing);
    PB_CHECK(e.store->replace_listing("/Docs", l, 1000).ok());
    const auto ids = e.queue({e.source("Downloads/new.txt", 42)});
    PB_CHECK_EQ(e.run().done, 1);
    const auto folder = e.store->folder("/Docs");
    PB_CHECK_EQ(folder->fetched_at.value_or(-1), 1000LL);   // the listing is no fresher than it was
    const auto entry = e.store->entry("/Docs/new.txt");
    PB_CHECK(entry.has_value());
    PB_CHECK_EQ(entry->size.value_or(-1), 42LL);
    PB_CHECK_EQ(entry->etag, e.server.etag_of("/Docs/new.txt"));
    PB_CHECK(entry->fileid.has_value());
    // and it is no longer an overlay row: the one row is the cached one
    const auto page = e.store->entries_page_overlay("/Docs", pbc::SortKey::Name, true, 0, 10);
    PB_CHECK_EQ(page.total, 2);
    for (const auto& r : page.rows) PB_CHECK(r.item_id == 0);
    (void)ids;
}

PB_TEST(a_folder_made_by_the_queue_is_created_first_and_the_files_follow_into_it) {
    UpEnv e;
    e.source("Notes/trip/plan.md", 20);
    e.source("Notes/trip/photos/a.jpg", 30);
    auto act = e.actions();
    const auto r = act.confirm_folder_pick(act.count_folder_pick("/Docs", {e.root + "/Notes/trip"}));
    PB_CHECK(r.error.ok());
    e.server.clear_log();
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.done, 4);   // two folders, two files
    PB_CHECK(e.server.node("/Docs/trip").folder);
    PB_CHECK(e.server.node("/Docs/trip/photos").folder);
    PB_CHECK_EQ(e.server.node("/Docs/trip/plan.md").content.size(), size_t(20));
    PB_CHECK_EQ(e.server.node("/Docs/trip/photos/a.jpg").content.size(), size_t(30));
    // the order of the requests: a folder before what is inside it
    std::vector<std::string> order;
    for (const auto& l : e.server.log())
        if (l.method == "MKCOL" || l.method == "PUT") order.push_back(l.method + " " + l.path);
    auto pos = [&](const std::string& part) {
        for (size_t i = 0; i < order.size(); ++i)
            if (order[i].find(part) != std::string::npos) return static_cast<int>(i);
        return -1;
    };
    PB_CHECK(pos("MKCOL /remote.php/dav/files/demo/Docs/trip/") < pos("PUT /remote.php/dav/files/demo/Docs/trip/plan.md"));
    PB_CHECK(pos("MKCOL /remote.php/dav/files/demo/Docs/trip/photos/") < pos("a.jpg"));
    PB_CHECK(pos("MKCOL /remote.php/dav/files/demo/Docs/trip/") < pos("MKCOL /remote.php/dav/files/demo/Docs/trip/photos/"));
    // a created folder reports its version and file id (its ETag changes with what is put inside, so it is not compared with the server's now)
    PB_CHECK(!e.item(r.queued[0]).result_etag.empty());
    PB_CHECK(e.item(r.queued[0]).result_fileid.has_value());
}

PB_TEST(a_folder_that_cannot_be_made_fails_and_what_waits_for_it_stays_queued_and_unsent) {
    UpEnv e;
    e.source("Notes/trip/plan.md", 20);
    auto act = e.actions();
    // the target folder does not exist on the server: MKCOL answers 409
    const auto r = act.confirm_folder_pick(act.count_folder_pick("/Missing/Deep", {e.root + "/Notes/trip"}));
    PB_CHECK(r.error.ok());
    e.server.clear_log();
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.done, 0);
    PB_CHECK_EQ(rep.failed, 1);
    PB_CHECK_EQ(rep.left, 0);                                      // the upload is not sendable: it waits for its folder
    PB_CHECK(e.item(r.queued[0]).state == pbc::ItemState::Failed);
    PB_CHECK_EQ(e.item(r.queued[0]).reason, std::string("target_gone"));
    PB_CHECK(e.item(r.queued[1]).state == pbc::ItemState::Queued);
    PB_CHECK_EQ(e.logged("PUT").size(), size_t(0));
}

PB_TEST(a_server_without_chunking_takes_a_large_file_as_one_put) {
    UpEnv e(false);   // dav.chunking absent
    const std::string src = e.source("Downloads/big.bin", 25ll << 20, 9);
    const auto ids = e.queue({src});
    e.server.clear_log();
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.done, 1);
    PB_CHECK_EQ(e.logged("PUT").size(), size_t(1));
    PB_CHECK_EQ(e.logged("MKCOL").size(), size_t(0));
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/big.bin"));
    (void)ids;
}

PB_TEST(a_file_that_changed_between_the_pick_and_the_send_is_sent_as_it_is_then) {
    UpEnv e;
    const std::string src = e.source("Downloads/a.txt", 100, 1);
    const auto ids = e.queue({src});
    e.rewrite(src, 250, 7);   // the user edited it after queuing
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.server.node("/Docs/a.txt").content.size(), size_t(250));
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/a.txt"));
    PB_CHECK_EQ(e.item(ids[0]).src_size.value_or(-1), 250LL);
}

PB_TEST(a_run_with_nothing_to_send_makes_no_request_and_a_second_run_sends_nothing_again) {
    UpEnv e;
    PB_CHECK_EQ(e.run().done, 0);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    e.queue({e.source("Downloads/a.txt", 10)});
    PB_CHECK_EQ(e.run().done, 1);
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 0);
    PB_CHECK_EQ(e.logged("PUT").size(), size_t(0));
}

PB_TEST(the_last_send_is_remembered_for_the_queue_screen) {
    UpEnv e;
    e.queue({e.source("Downloads/a.txt", 10), e.source("Downloads/b.txt", 10)});
    e.run();
    PB_CHECK_EQ(e.store->setting("last_send_at"), std::to_string(e.clock.t));
    PB_CHECK(e.store->setting("last_send_counts").find("\"done\":2") != std::string::npos);
}

PB_TEST_MAIN()
