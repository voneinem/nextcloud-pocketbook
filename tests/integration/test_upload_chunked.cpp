// A large file goes to the server in chunks of its own upload folder and is assembled by one final MOVE (specs/002-manage-server-files US2,
// contracts/server-requests.md W4, research R-4). Fake Nextcloud: this proves the app against the contract; which conditions a real server honors on
// the final MOVE is LV-W6.
#include <string>

#include "upload_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::UpEnv;

namespace {
constexpr long long kMiB = 1ll << 20;
}

PB_TEST(a_large_file_is_sent_in_ten_mib_chunks_to_the_upload_area_and_assembled_by_one_move) {
    UpEnv e;
    const std::string src = e.source("Downloads/big.bin", 25 * kMiB, 3);
    const auto ids = e.queue({src});
    e.server.clear_log();
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.done, 1);
    PB_CHECK(rep.stopped_by_key.empty());
    const auto mkcols = e.logged("MKCOL");
    PB_CHECK_EQ(mkcols.size(), size_t(1));
    PB_CHECK(mkcols[0].path.find("/remote.php/dav/uploads/demo/pbc-") != std::string::npos);
    const auto puts = e.logged("PUT");
    PB_CHECK_EQ(puts.size(), size_t(3));
    for (const auto& p : puts) {
        PB_CHECK(p.path.find("/remote.php/dav/uploads/demo/pbc-") != std::string::npos);   // never in the user's folder
        PB_CHECK_EQ(p.total_length, std::to_string(25 * kMiB));
        PB_CHECK(!p.destination.empty());
    }
    PB_CHECK(puts[0].path.find("/00001") != std::string::npos);
    PB_CHECK(puts[2].path.find("/00003") != std::string::npos);
    PB_CHECK_EQ(puts[0].body_bytes, 10 * kMiB);
    PB_CHECK_EQ(puts[2].body_bytes, 5 * kMiB);
    const auto moves = e.logged("MOVE");
    PB_CHECK_EQ(moves.size(), size_t(1));
    PB_CHECK(moves[0].path.find("/.file") != std::string::npos);
    PB_CHECK_EQ(moves[0].overwrite, std::string("F"));
    PB_CHECK_EQ(moves[0].total_length, std::to_string(25 * kMiB));
    PB_CHECK(!moves[0].mtime_header.empty());
    PB_CHECK_EQ(moves[0].checksum_header, "SHA256:" + e.sha_of(src));
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/big.bin"));
    PB_CHECK_EQ(e.server.upload_sessions(), size_t(0));   // the MOVE took the upload folder with it
    const auto it = e.item(ids[0]);
    PB_CHECK(it.state == pbc::ItemState::Done);
    PB_CHECK_EQ(it.result_etag, e.server.etag_of("/Docs/big.bin"));
    PB_CHECK(it.result_fileid.has_value());
}

PB_TEST(the_server_may_ask_for_smaller_chunks_but_never_below_five_mib) {
    UpEnv e;
    pbc::ServerCaps caps;
    caps.trash = true;
    caps.chunking = true;
    caps.max_chunk = 6 * kMiB;
    PB_CHECK(e.store->save_server_caps(caps).ok());
    e.queue({e.source("Downloads/big.bin", 25 * kMiB)});
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.logged("PUT").size(), size_t(5));   // 6 + 6 + 6 + 6 + 1
    caps.max_chunk = 1 * kMiB;
    PB_CHECK(e.store->save_server_caps(caps).ok());
    e.queue({e.source("Downloads/big2.bin", 25 * kMiB)});
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.logged("PUT").size(), size_t(5));   // 5 MiB is the floor
}

PB_TEST(a_name_that_is_taken_is_found_out_before_the_first_byte_is_sent) {
    UpEnv e;
    e.server.add_file("/Docs/big.bin", "other");
    const auto ids = e.queue({e.source("Downloads/big.bin", 25 * kMiB)});
    e.server.clear_log();
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.conflict, 1);
    PB_CHECK(e.item(ids[0]).state == pbc::ItemState::Conflict);
    PB_CHECK_EQ(e.item(ids[0]).reason, std::string("name_exists"));
    PB_CHECK_EQ(e.logged("MKCOL").size() + e.logged("PUT").size() + e.logged("MOVE").size(), size_t(0));
    PB_CHECK_EQ(e.server.node("/Docs/big.bin").content, std::string("other"));
}

PB_TEST(a_file_that_appears_while_the_chunks_go_up_is_not_overwritten_by_the_final_move) {
    UpEnv e;
    const auto ids = e.queue({e.source("Downloads/big.bin", 25 * kMiB)});
    // somebody else makes the name between the pre-check and the MOVE: the Overwrite: F of the MOVE refuses
    e.server.before_next("MOVE", "/.file", [&] { e.server.add_file("/Docs/big.bin", "theirs"); });
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.conflict, 1);
    PB_CHECK_EQ(e.item(ids[0]).reason, std::string("name_exists"));
    PB_CHECK_EQ(e.server.node("/Docs/big.bin").content, std::string("theirs"));
    PB_CHECK_EQ(e.server.overwrites(), 0);
}

PB_TEST(a_quota_refusal_at_the_first_chunk_ends_the_item_with_the_storage_message_and_keeps_the_rest_of_the_queue_going) {
    UpEnv e;
    e.server.set_quota_limit(5 * kMiB);
    const auto big = e.queue({e.source("Downloads/big.bin", 25 * kMiB)});
    const auto small = e.queue({e.source("Downloads/small.txt", 1000)});
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.failed, 1);
    PB_CHECK_EQ(rep.done, 1);
    PB_CHECK_EQ(e.item(big[0]).reason, std::string("server_storage_full"));
    PB_CHECK(e.item(small[0]).state == pbc::ItemState::Done);
}

PB_TEST(progress_is_reported_per_chunk_and_ends_at_the_whole_size) {
    UpEnv e;
    const auto ids = e.queue({e.source("Downloads/big.bin", 25 * kMiB)});
    e.run();
    long long last = -1;
    bool saw_chunks = false;
    for (const auto& p : e.progress)
        if (p.step == "chunks") {
            saw_chunks = true;
            last = p.done;
            PB_CHECK_EQ(p.total, 25 * kMiB);
        }
    PB_CHECK(saw_chunks);
    PB_CHECK_EQ(last, 25 * kMiB);
    (void)ids;
}

PB_TEST(the_source_is_only_ever_read) {
    UpEnv e;
    const std::string src = e.source("Downloads/big.bin", 12 * kMiB, 9);
    struct stat before {};
    PB_CHECK(::stat(src.c_str(), &before) == 0);
    e.queue({src});
    PB_CHECK_EQ(e.run().done, 1);
    struct stat after {};
    PB_CHECK(::stat(src.c_str(), &after) == 0);
    PB_CHECK_EQ(before.st_size, after.st_size);
    PB_CHECK_EQ(before.st_mtime, after.st_mtime);
    PB_CHECK_EQ(before.st_ino, after.st_ino);
}

PB_TEST(capabilities_the_reader_never_learned_are_asked_once_when_a_send_starts_so_that_a_large_file_goes_in_chunks) {
    UpEnv e;
    PB_CHECK(e.store->debug_exec("DELETE FROM server_caps").empty());
    PB_CHECK(!e.store->server_caps().has_value());
    const auto ids = e.queue({e.source("Downloads/big.bin", 12 * kMiB)});
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK(e.store->server_caps().has_value());
    PB_CHECK_EQ(e.logged("PUT", "/dav/uploads/").size(), size_t(2));   // chunked: the capabilities said so
    PB_CHECK_EQ(e.logged("GET", "capabilities").size(), size_t(1));
    e.server.clear_log();
    e.queue({e.source("Downloads/big2.bin", 12 * kMiB)});
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.logged("GET", "capabilities").size(), size_t(0));   // known now
    (void)ids;
}

PB_TEST_MAIN()
