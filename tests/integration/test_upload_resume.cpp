// An interrupted chunked upload continues where it stopped (specs/002-manage-server-files US2, SC-005, research R-4.4/R-4.6): only the chunks the
// server does not hold are sent, a source that changed starts over, an upload folder that expired is made again. Fake Nextcloud.
#include <string>

#include "upload_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::UpEnv;

namespace {
constexpr long long kMiB = 1ll << 20;
}

PB_TEST(a_cut_connection_in_chunk_two_stops_the_run_and_the_next_send_sends_only_what_is_missing) {
    UpEnv e;
    const std::string src = e.source("Downloads/big.bin", 25 * kMiB, 4);
    const auto ids = e.queue({src});
    e.server.before_next("PUT", "/00002", [&] { e.server.drop_after_bytes(4 * kMiB); });
    const pbc::SendReport first = e.run();
    PB_CHECK_EQ(first.stopped_by_key, std::string("connection_lost"));
    PB_CHECK_EQ(first.done + first.failed + first.conflict + first.unknown, 0);
    const auto held = e.item(ids[0]);
    PB_CHECK(held.state == pbc::ItemState::Queued);   // an upload with chunks on the server is resumable, not unknown
    PB_CHECK(!held.upload_id.empty());
    PB_CHECK(!e.server.has("/Docs/big.bin"));         // nothing incomplete is visible in the user's folder
    PB_CHECK_EQ(e.server.chunk_names(held.upload_id).size(), size_t(1));

    e.server.clear_log();
    const pbc::SendReport second = e.run();
    PB_CHECK_EQ(second.done, 1);
    PB_CHECK_EQ(e.logged("MKCOL").size(), size_t(0));            // the upload folder is still there
    PB_CHECK_EQ(e.logged("PROPFIND", "/dav/uploads/").size(), size_t(1));
    PB_CHECK_EQ(e.logged("PUT").size(), size_t(2));              // chunks 2 and 3 only
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/big.bin"));
}

PB_TEST(a_chunk_of_the_wrong_size_is_sent_again) {
    UpEnv e;
    const std::string src = e.source("Downloads/big.bin", 25 * kMiB, 4);
    const auto ids = e.queue({src});
    e.server.set_partial_chunks_visible(true);
    e.server.before_next("PUT", "/00002", [&] { e.server.drop_after_bytes(4 * kMiB); });
    e.run();
    const std::string id = e.item(ids[0]).upload_id;
    PB_CHECK_EQ(e.server.chunk_names(id).size(), size_t(2));      // the cut chunk is there, 4 MiB of 10
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.logged("PUT").size(), size_t(2));
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/big.bin"));
}

PB_TEST(an_upload_folder_that_expired_is_made_again_and_everything_is_sent) {
    UpEnv e;
    const std::string src = e.source("Downloads/big.bin", 25 * kMiB, 4);
    const auto ids = e.queue({src});
    e.server.before_next("PUT", "/00002", [&] { e.server.drop_after_bytes(1 * kMiB); });
    e.run();
    e.server.expire_uploads();   // 24 hours without a request
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.logged("MKCOL").size(), size_t(1));
    PB_CHECK_EQ(e.logged("PUT").size(), size_t(3));
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/big.bin"));
    (void)ids;
}

PB_TEST(a_source_that_changed_starts_the_upload_over_with_a_new_folder_and_the_old_one_is_removed) {
    UpEnv e;
    const std::string src = e.source("Downloads/big.bin", 25 * kMiB, 4);
    const auto ids = e.queue({src});
    e.server.before_next("PUT", "/00002", [&] { e.server.drop_after_bytes(1 * kMiB); });
    e.run();
    const std::string old_id = e.item(ids[0]).upload_id;
    e.rewrite(src, 22 * kMiB, 8);   // the user edited the file
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.logged("DELETE", old_id).size(), size_t(1));    // the app's own upload folder
    PB_CHECK_EQ(e.logged("MKCOL").size(), size_t(1));
    PB_CHECK_EQ(e.server.node("/Docs/big.bin").size(), 22 * kMiB);
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/big.bin"));
    PB_CHECK(e.item(ids[0]).upload_id != old_id);
    PB_CHECK_EQ(e.server.upload_sessions(), size_t(0));
}

PB_TEST(a_source_that_changes_between_chunks_of_one_send_starts_over_once) {
    UpEnv e;
    const std::string src = e.source("Downloads/big.bin", 25 * kMiB, 4);
    const auto ids = e.queue({src});
    int chunks = 0;
    e.on_hook = [&](const char* step) {
        if (std::string(step) == "chunk" && ++chunks == 2) e.rewrite(src, 21 * kMiB, 6);
    };
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.server.node("/Docs/big.bin").size(), 21 * kMiB);
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/big.bin"));
    (void)ids;
}

PB_TEST(a_source_that_keeps_changing_fails_the_item_instead_of_looping) {
    UpEnv e;
    const std::string src = e.source("Downloads/big.bin", 25 * kMiB, 4);
    const auto ids = e.queue({src});
    int chunks = 0;
    e.on_hook = [&](const char* step) {
        if (std::string(step) == "chunk") e.rewrite(src, (20 + (++chunks % 3)) * kMiB, static_cast<uint8_t>(chunks));
    };
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.failed, 1);
    PB_CHECK_EQ(e.item(ids[0]).reason, std::string("source_changed"));
    PB_CHECK(!e.server.has("/Docs/big.bin"));
}

PB_TEST(cancel_stops_after_the_chunk_in_flight_and_the_item_stays_resumable) {
    UpEnv e;
    const std::string src = e.source("Downloads/big.bin", 25 * kMiB, 4);
    const auto ids = e.queue({src});
    pbtest::UpCancelFlag cancel;
    int chunks = 0;
    e.on_hook = [&](const char* step) {
        if (std::string(step) == "chunk" && ++chunks == 2) cancel.flag = true;
    };
    const pbc::SendReport rep = e.sender()->run(cancel);
    PB_CHECK_EQ(rep.stopped_by_key, std::string("cancelled"));
    PB_CHECK(e.item(ids[0]).state == pbc::ItemState::Queued);
    PB_CHECK_EQ(e.server.chunk_names(e.item(ids[0]).upload_id).size(), size_t(1));
    e.on_hook = nullptr;
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.logged("PUT").size(), size_t(2));
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/big.bin"));
}

PB_TEST(wifi_going_down_between_chunks_stops_the_run_with_the_item_queued) {
    UpEnv e;
    const std::string src = e.source("Downloads/big.bin", 25 * kMiB, 4);
    const auto ids = e.queue({src});
    const auto small = e.queue({e.source("Downloads/small.txt", 10)});
    int chunks = 0;
    e.on_hook = [&](const char* step) {
        if (std::string(step) == "chunk" && ++chunks == 1) e.wifi = false;
    };
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.stopped_by_key, std::string("no_connection"));
    PB_CHECK(e.item(ids[0]).state == pbc::ItemState::Queued);
    PB_CHECK(e.item(small[0]).state == pbc::ItemState::Queued);   // nothing else was tried
    e.wifi = true;
    e.on_hook = nullptr;
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 2);
    PB_CHECK_EQ(e.logged("PUT", "/dav/uploads/").size(), size_t(2));
}

PB_TEST(no_connection_at_all_leaves_everything_queued_and_sends_nothing_unknown) {
    UpEnv e;
    const auto ids = e.queue({e.source("Downloads/a.txt", 100), e.source("Downloads/b.txt", 100)});
    e.server.set_offline(true);
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.stopped_by_key, std::string("no_connection"));
    PB_CHECK_EQ(rep.unknown + rep.failed, 0);
    PB_CHECK(e.item(ids[0]).state == pbc::ItemState::Queued);
    PB_CHECK(e.item(ids[1]).state == pbc::ItemState::Queued);
    e.server.set_offline(false);
    PB_CHECK_EQ(e.run().done, 2);
}

PB_TEST_MAIN()
