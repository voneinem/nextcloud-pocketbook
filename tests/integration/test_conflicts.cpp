// A change on the server since the reader last saw it holds the item with both versions intact until the user decides (specs/002-manage-server-files
// US3, FR-023/FR-024, research R-3, R-8.4, R-8.5): keep both under a new name, replace anyway after a second confirmation on a version the user has
// seen, or skip. Fake Nextcloud: the conditions are the fake's, which is what the contract tests prove; whether a real server honors the same ones on
// the final MOVE is LV-W6.
#include <string>

#include "upload_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::UpEnv;
using pbc::AskChoice;

namespace {
constexpr long long kMiB = 1ll << 20;

// A replace of /Docs/a.txt queued against the version the reader saw.
int64_t queue_replace(UpEnv& e, const std::string& src) {
    const pbc::AddResult r = e.actions().add_uploads("/Docs", {src});
    PB_CHECK_EQ(r.asks.size(), size_t(1));
    const pbc::AddResult q = e.actions().answer_ask(r.asks[0], AskChoice::Replace);
    PB_CHECK_EQ(q.queued.size(), size_t(1));
    return q.queued[0];
}
}  // namespace

PB_TEST(a_replace_of_a_version_that_changed_is_held_as_a_conflict_and_nothing_is_overwritten) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "version one");
    e.cache("/Docs");
    const std::string src = e.source("Downloads/a.txt", 100, 3);
    const int64_t id = queue_replace(e, src);
    e.server.set_file("/Docs/a.txt", "version two, written by someone else", 1790000500);   // after the reader's listing
    const pbc::SendReport rep = e.run();
    PB_CHECK_EQ(rep.conflict, 1);
    PB_CHECK(e.item(id).state == pbc::ItemState::Conflict);
    PB_CHECK_EQ(e.item(id).reason, std::string("changed_on_server"));
    PB_CHECK_EQ(e.server.node("/Docs/a.txt").content, std::string("version two, written by someone else"));
    PB_CHECK_EQ(e.server.overwrites(), 0);
    PB_CHECK(::access(src.c_str(), R_OK) == 0);   // the reader's file is where it was
    // a run never takes a held item again
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 0);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(keep_both_uploads_under_a_new_name_and_leaves_the_server_version_alone) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "version one");
    e.cache("/Docs");
    const std::string src = e.source("Downloads/a.txt", 100, 3);
    const int64_t id = queue_replace(e, src);
    e.server.set_file("/Docs/a.txt", "version two", 1790000500);
    e.run();
    auto s = e.sender();
    PB_CHECK(s->resolve_keep_both(id, std::string()).ok());
    const pbc::QueueItem it = e.item(id);
    PB_CHECK(it.state == pbc::ItemState::Queued);
    PB_CHECK_EQ(it.dst_name, std::string("a (2).txt"));
    PB_CHECK(!it.replace);
    PB_CHECK(it.dst_etag.empty());
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.server.node("/Docs/a.txt").content, std::string("version two"));
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/a (2).txt"));
}

PB_TEST(replace_anyway_refreshes_the_version_at_the_moment_of_the_choice_and_is_conditional_on_it) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "version one");
    e.cache("/Docs");
    const std::string src = e.source("Downloads/a.txt", 100, 3);
    const int64_t id = queue_replace(e, src);
    e.server.set_file("/Docs/a.txt", "version two, a bit longer", 1790000500);
    e.run();
    auto s = e.sender();
    const auto seen = s->refresh_target(id);   // what the user is shown in the second confirmation
    PB_CHECK(seen.ok());
    PB_CHECK(seen.value.has_value());
    PB_CHECK_EQ(seen.value->size.value_or(-1), 25LL);
    PB_CHECK_EQ(seen.value->etag, e.etag("/Docs/a.txt"));
    PB_CHECK(e.item(id).state == pbc::ItemState::Conflict);   // looking changes nothing
    // the server version changes once more before the user confirms: the replace of the shown version is a conflict again
    e.server.set_file("/Docs/a.txt", "version three", 1790000600);
    PB_CHECK(s->resolve_replace(id, seen.value->etag).ok());
    PB_CHECK(e.item(id).state == pbc::ItemState::Queued);
    PB_CHECK(e.item(id).replace);
    PB_CHECK_EQ(e.item(id).dst_etag, seen.value->etag);
    PB_CHECK_EQ(e.run().conflict, 1);
    PB_CHECK_EQ(e.server.node("/Docs/a.txt").content, std::string("version three"));
    // the user looks again and confirms the version they saw: now it goes through
    const auto again = s->refresh_target(id);
    PB_CHECK(s->resolve_replace(id, again.value->etag).ok());
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/a.txt"));
}

PB_TEST(replace_anyway_after_the_server_file_vanished_just_uploads_it) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "version one");
    e.cache("/Docs");
    const std::string src = e.source("Downloads/a.txt", 100, 3);
    const int64_t id = queue_replace(e, src);
    e.server.set_file("/Docs/a.txt", "changed", 1790000500);
    e.run();
    e.server.remove("/Docs/a.txt");
    auto s = e.sender();
    const auto seen = s->refresh_target(id);
    PB_CHECK(seen.ok());
    PB_CHECK(!seen.value.has_value());   // nothing to show: the name is free now
    PB_CHECK(s->resolve_replace(id, std::string()).ok());
    PB_CHECK(!e.item(id).replace);       // a plain upload now
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/a.txt"));
}

PB_TEST(skip_removes_the_held_item_and_touches_neither_side) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "version one");
    e.cache("/Docs");
    const std::string src = e.source("Downloads/a.txt", 100, 3);
    const int64_t id = queue_replace(e, src);
    e.server.set_file("/Docs/a.txt", "changed", 1790000500);
    e.run();
    PB_CHECK(e.sender()->resolve_discard(id).ok());
    PB_CHECK(!e.store->queue_item(id).has_value());
    PB_CHECK_EQ(e.server.node("/Docs/a.txt").content, std::string("changed"));
    PB_CHECK(::access(src.c_str(), R_OK) == 0);
}

PB_TEST(a_name_that_appeared_after_queuing_holds_the_new_file_as_name_exists_and_keep_both_resolves_it) {
    UpEnv e;
    e.cache("/Docs");   // an empty listing: the name looked free
    const std::string src = e.source("Downloads/a.txt", 100, 3);
    const pbc::AddResult r = e.actions().add_uploads("/Docs", {src});
    PB_CHECK_EQ(r.queued.size(), size_t(1));
    e.server.add_file("/Docs/a.txt", "theirs");
    PB_CHECK_EQ(e.run().conflict, 1);
    PB_CHECK_EQ(e.item(r.queued[0]).reason, std::string("name_exists"));
    PB_CHECK_EQ(e.server.node("/Docs/a.txt").content, std::string("theirs"));
    // the proposed name is looked for on the server now, not only in the listing the reader had
    PB_CHECK(e.sender()->resolve_keep_both(r.queued[0], std::string()).ok());
    PB_CHECK_EQ(e.item(r.queued[0]).dst_name, std::string("a (2).txt"));
    PB_CHECK_EQ(e.run().done, 1);
}

PB_TEST(a_chunked_replace_deletes_the_old_version_into_the_trash_on_its_etag_and_assembles_without_overwrite) {
    UpEnv e;
    e.server.add_file("/Docs/big.bin", "the old big file");
    e.cache("/Docs");
    const std::string src = e.source("Downloads/big.bin", 25 * kMiB, 5);
    const int64_t id = queue_replace(e, src);
    const std::string old_etag = e.etag("/Docs/big.bin");
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/big.bin"));
    const auto deletes = e.logged("DELETE", "/dav/files/");
    PB_CHECK_EQ(deletes.size(), size_t(1));
    PB_CHECK_EQ(deletes[0].if_match, old_etag);
    PB_CHECK(e.server.trash_has("/Docs/big.bin"));        // the old version is in the trash, not gone
    PB_CHECK_EQ(e.server.permanent_deletes(), 0);
    const auto moves = e.logged("MOVE");
    PB_CHECK_EQ(moves.size(), size_t(1));
    PB_CHECK_EQ(moves[0].overwrite, std::string("F"));    // never Overwrite: T
    PB_CHECK_EQ(e.server.overwrites(), 0);
    PB_CHECK(e.item(id).state == pbc::ItemState::Done);
}

PB_TEST(a_chunked_replace_of_a_version_that_changed_is_held_before_the_first_byte_is_sent) {
    UpEnv e;
    e.server.add_file("/Docs/big.bin", "the old big file");
    e.cache("/Docs");
    const int64_t id = queue_replace(e, e.source("Downloads/big.bin", 25 * kMiB, 5));
    e.server.set_file("/Docs/big.bin", "somebody's newer version", 1790000700);
    e.server.clear_log();
    PB_CHECK_EQ(e.run().conflict, 1);
    PB_CHECK_EQ(e.item(id).reason, std::string("changed_on_server"));
    PB_CHECK_EQ(e.logged("PUT").size() + e.logged("MKCOL").size() + e.logged("DELETE").size(), size_t(0));
    PB_CHECK_EQ(e.server.node("/Docs/big.bin").content, std::string("somebody's newer version"));
}

PB_TEST(a_change_between_the_last_pre_check_and_the_delete_is_caught_by_the_deletes_condition) {
    UpEnv e;
    e.server.add_file("/Docs/big.bin", "the old big file");
    e.cache("/Docs");
    const int64_t id = queue_replace(e, e.source("Downloads/big.bin", 25 * kMiB, 5));
    e.server.before_next("DELETE", "/Docs/big.bin", [&] { e.server.set_file("/Docs/big.bin", "changed at the last moment", 1790000800); });
    PB_CHECK_EQ(e.run().conflict, 1);
    PB_CHECK_EQ(e.item(id).reason, std::string("changed_on_server"));
    PB_CHECK_EQ(e.server.node("/Docs/big.bin").content, std::string("changed at the last moment"));
    PB_CHECK_EQ(e.server.trash_count(), 0LL);
    // the chunks are still on the server: the item continues from them after the user's choice
    PB_CHECK_EQ(e.server.upload_sessions(), size_t(1));
}

PB_TEST(without_a_trash_a_chunked_replace_is_refused_instead_of_deleting_for_good) {
    UpEnv e;
    pbc::ServerCaps caps;
    caps.trash = false;
    caps.chunking = true;
    PB_CHECK(e.store->save_server_caps(caps).ok());
    e.server.set_trash(false);
    e.server.add_file("/Docs/big.bin", "the old big file");
    e.cache("/Docs");
    const int64_t id = queue_replace(e, e.source("Downloads/big.bin", 25 * kMiB, 5));
    e.server.clear_log();
    PB_CHECK_EQ(e.run().failed, 1);
    PB_CHECK_EQ(e.item(id).reason, std::string("trash_unavailable"));
    PB_CHECK_EQ(e.logged("DELETE", "/dav/files/").size(), size_t(0));
    PB_CHECK_EQ(e.server.permanent_deletes(), 0);
    PB_CHECK_EQ(e.server.node("/Docs/big.bin").content, std::string("the old big file"));
}

PB_TEST(death_between_the_delete_and_the_move_leaves_the_old_version_in_the_trash_and_a_resend_finishes_the_replace) {
    UpEnv e;
    e.server.add_file("/Docs/big.bin", "the old big file");
    e.cache("/Docs");
    const std::string src = e.source("Downloads/big.bin", 25 * kMiB, 5);
    const int64_t id = queue_replace(e, src);
    struct Killed {};
    e.on_hook = [&](const char* step) {
        if (std::string(step) == "assemble") throw Killed();
    };
    bool died = false;
    try {
        e.run();
    } catch (const Killed&) {
        died = true;
    }
    PB_CHECK(died);
    e.on_hook = nullptr;
    PB_CHECK_EQ(e.restart().unknown, 1);
    PB_CHECK(e.item(id).state == pbc::ItemState::Unknown);
    PB_CHECK(e.sender()->send_again(id).ok());
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/big.bin"));
    PB_CHECK_EQ(e.server.overwrites(), 0);
}

PB_TEST(only_the_version_marker_decides_not_the_size_the_date_or_the_name) {
    // the server's file has the same name, the same size and the same date as the reader's listing says, and another version marker: held, for every kind of change
    UpEnv e;
    e.server.add_folder("/Docs/dst");
    for (const char* n : {"r.txt", "m.txt", "d.txt", "u.txt"}) e.server.add_file(std::string("/Docs/") + n, "content", 1790000000);
    e.cache("/Docs");
    e.cache("/Docs/dst");
    auto act = e.actions();
    const auto ren = act.rename_entry("/Docs/r.txt", "r2.txt");
    const auto mov = act.move_or_copy(pbc::ItemKind::Move, {"/Docs/m.txt"}, "/Docs/dst");
    const auto del = act.delete_entries({"/Docs/d.txt"});
    const auto up = act.add_uploads("/Docs", {e.source("Downloads/u.txt", 10)});
    const auto rep = act.answer_ask(up.asks[0], AskChoice::Replace);
    for (const char* n : {"r.txt", "m.txt", "d.txt", "u.txt"}) e.server.set_file(std::string("/Docs/") + n, "content", 1790000000);   // same bytes, same date: a new marker
    const pbc::SendReport rep_run = e.run();
    PB_CHECK_EQ(rep_run.conflict, 4);
    for (int64_t id : {ren.queued[0], mov.queued[0], del.queued[0], rep.queued[0]}) PB_CHECK_EQ(e.item(id).reason, std::string("changed_on_server"));
    PB_CHECK(e.server.has("/Docs/r.txt") && e.server.has("/Docs/m.txt") && e.server.has("/Docs/d.txt"));
    PB_CHECK_EQ(e.server.trash_count(), 0LL);
}

PB_TEST(a_listing_that_shows_another_size_and_date_but_the_same_marker_does_not_hold_the_change) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "content", 1790000000);
    e.cache("/Docs");
    PB_CHECK(e.store->debug_exec("UPDATE entries SET size = 999999, mtime = 1 WHERE name = 'a.txt'").empty());   // what the reader shows differs, the marker does not
    const auto r = e.actions().rename_entry("/Docs/a.txt", "b.txt");
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK(e.server.has("/Docs/b.txt"));
    (void)r;
}

PB_TEST_MAIN()
