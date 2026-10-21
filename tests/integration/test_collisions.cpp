// Never overwrite silently (specs/002-manage-server-files US3, FR-021 to FR-025, research R-8): a name that is taken asks - keep both under a new
// name, replace after a confirmation, or skip - and queues nothing until the user has answered. Replace is only ever offered for a file whose version
// the reader knows. Fake Nextcloud for the cached listing and the arrival; the questions themselves are core logic.
#include <string>

#include "upload_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::UpEnv;
using pbc::AskChoice;

PB_TEST(a_name_in_the_cached_listing_asks_and_queues_nothing_until_answered) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "on the server");
    e.cache("/Docs");
    const std::string src = e.source("Downloads/a.txt", 100);
    const pbc::AddResult r = e.actions().add_uploads("/Docs", {src, e.source("Downloads/b.txt", 10)});
    PB_CHECK(r.error.ok());
    PB_CHECK_EQ(r.queued.size(), size_t(1));          // b.txt is free and queued
    PB_CHECK_EQ(r.asks.size(), size_t(1));
    const pbc::NameAsk& ask = r.asks[0];
    PB_CHECK_EQ(ask.name, std::string("a.txt"));
    PB_CHECK_EQ(ask.target_folder, std::string("/Docs"));
    PB_CHECK_EQ(ask.suggested, std::string("a (2).txt"));
    PB_CHECK(ask.replace_possible);
    PB_CHECK(!ask.existing_is_folder);
    PB_CHECK_EQ(ask.existing_size.value_or(-1), 13LL);
    PB_CHECK_EQ(e.store->queue_count_open(), 1);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(a_name_taken_by_an_open_item_asks_too) {
    UpEnv e;
    const std::string src = e.source("Downloads/a.txt", 100);
    PB_CHECK_EQ(e.actions().add_uploads("/Docs", {src}).queued.size(), size_t(1));
    const pbc::AddResult again = e.actions().add_uploads("/Docs", {src});
    PB_CHECK_EQ(again.queued.size(), size_t(0));
    PB_CHECK_EQ(again.asks.size(), size_t(1));
    PB_CHECK(again.asks[0].in_queue);
    PB_CHECK(!again.asks[0].replace_possible);    // there is nothing on the server to replace yet
    PB_CHECK_EQ(again.asks[0].suggested, std::string("a (2).txt"));
}

PB_TEST(two_picks_with_the_same_name_in_one_call_ask_for_the_second) {
    UpEnv e;
    const pbc::AddResult r = e.actions().add_uploads("/Docs", {e.source("Downloads/a.txt", 10), e.source("Notes/a.txt", 20)});
    PB_CHECK_EQ(r.queued.size(), size_t(1));
    PB_CHECK_EQ(r.asks.size(), size_t(1));
}

PB_TEST(keep_both_queues_the_new_name_and_both_files_exist_on_the_server_afterwards) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "on the server");
    e.cache("/Docs");
    const std::string src = e.source("Downloads/a.txt", 100, 7);
    const pbc::AddResult r = e.actions().add_uploads("/Docs", {src});
    const pbc::AddResult kept = e.actions().answer_ask(r.asks[0], AskChoice::KeepBoth);
    PB_CHECK(kept.error.ok());
    PB_CHECK_EQ(kept.queued.size(), size_t(1));
    PB_CHECK_EQ(e.item(kept.queued[0]).dst_name, std::string("a (2).txt"));
    PB_CHECK(!e.item(kept.queued[0]).replace);
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.server.node("/Docs/a.txt").content, std::string("on the server"));
    PB_CHECK_EQ(e.sha_of(src), e.server.sha256_of("/Docs/a (2).txt"));
}

PB_TEST(keep_both_with_a_name_the_user_typed_checks_it_and_asks_again_when_it_is_taken_too) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "x");
    e.server.add_file("/Docs/mine.txt", "y");
    e.cache("/Docs");
    const pbc::AddResult r = e.actions().add_uploads("/Docs", {e.source("Downloads/a.txt", 10)});
    const pbc::AddResult taken = e.actions().answer_ask(r.asks[0], AskChoice::KeepBoth, "mine.txt");
    PB_CHECK_EQ(taken.queued.size(), size_t(0));
    PB_CHECK_EQ(taken.asks.size(), size_t(1));
    PB_CHECK_EQ(taken.asks[0].name, std::string("mine.txt"));
    PB_CHECK_EQ(taken.asks[0].suggested, std::string("mine (2).txt"));
    const pbc::AddResult bad = e.actions().answer_ask(r.asks[0], AskChoice::KeepBoth, "a/b.txt");
    PB_CHECK_EQ(bad.error.key, std::string("name_forbidden"));
    PB_CHECK_EQ(bad.queued.size(), size_t(0));
    const pbc::AddResult ok = e.actions().answer_ask(r.asks[0], AskChoice::KeepBoth, "fresh.txt");
    PB_CHECK_EQ(ok.queued.size(), size_t(1));
    PB_CHECK_EQ(e.item(ok.queued[0]).dst_name, std::string("fresh.txt"));
}

PB_TEST(keep_both_names_skip_what_is_taken_in_the_listing_the_queue_and_the_servers_forbidden_names) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "x");
    e.server.add_file("/Docs/a (2).txt", "x");
    e.cache("/Docs");
    PB_CHECK_EQ(e.actions().add_uploads("/Docs", {e.source("Notes/a (3).txt", 5)}).queued.size(), size_t(1));   // an open item holds "a (3).txt"
    pbc::ServerCaps caps = *e.store->server_caps();
    caps.forbidden_names = {"a (4).txt"};
    PB_CHECK(e.store->save_server_caps(caps).ok());
    const pbc::AddResult r = e.actions().add_uploads("/Docs", {e.source("Downloads/a.txt", 10)});
    PB_CHECK_EQ(r.asks[0].suggested, std::string("a (5).txt"));
}

PB_TEST(replace_needs_a_strong_etag_and_queues_a_conditional_replace_of_that_version) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "on the server");
    e.cache("/Docs");
    const std::string etag = e.etag("/Docs/a.txt");
    const pbc::AddResult r = e.actions().add_uploads("/Docs", {e.source("Downloads/a.txt", 100)});
    const pbc::AddResult replaced = e.actions().answer_ask(r.asks[0], AskChoice::Replace);
    PB_CHECK(replaced.error.ok());
    PB_CHECK_EQ(replaced.queued.size(), size_t(1));
    const pbc::QueueItem it = e.item(replaced.queued[0]);
    PB_CHECK(it.replace);
    PB_CHECK_EQ(it.dst_etag, etag);
    PB_CHECK_EQ(it.dst_name, std::string("a.txt"));
    // the listing now says the entry is being replaced, marked not synced
    const auto page = e.store->entries_page_overlay("/Docs", pbc::SortKey::Name, true, 0, 10);
    PB_CHECK_EQ(page.total, 1);
    PB_CHECK_EQ(page.rows[0].pending, std::string("replacing"));
}

PB_TEST(a_weak_or_unknown_version_is_not_replaced_and_says_to_refresh_first) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "on the server");
    e.server.set_weak_etag("/Docs/a.txt", true);
    e.cache("/Docs");
    const pbc::AddResult r = e.actions().add_uploads("/Docs", {e.source("Downloads/a.txt", 100)});
    PB_CHECK_EQ(r.asks.size(), size_t(1));
    PB_CHECK(!r.asks[0].replace_possible);
    PB_CHECK(r.asks[0].refresh_needed);
    const pbc::AddResult replaced = e.actions().answer_ask(r.asks[0], AskChoice::Replace);
    PB_CHECK_EQ(replaced.error.key, std::string("refresh_first"));
    PB_CHECK_EQ(replaced.queued.size(), size_t(0));
    PB_CHECK_EQ(e.store->queue_count_open(), 0);
}

PB_TEST(a_folder_is_never_replaced_and_a_file_never_goes_over_a_folder) {
    UpEnv e;
    e.server.add_folder("/Docs/a.txt");
    e.cache("/Docs");
    const pbc::AddResult r = e.actions().add_uploads("/Docs", {e.source("Downloads/a.txt", 100)});
    PB_CHECK_EQ(r.asks.size(), size_t(1));
    PB_CHECK(r.asks[0].existing_is_folder);
    PB_CHECK(!r.asks[0].replace_possible);
    PB_CHECK_EQ(e.actions().answer_ask(r.asks[0], AskChoice::Replace).error.key, std::string("not_allowed"));
    PB_CHECK_EQ(e.actions().answer_ask(r.asks[0], AskChoice::KeepBoth).queued.size(), size_t(1));
}

PB_TEST(skip_queues_nothing) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "x");
    e.cache("/Docs");
    const pbc::AddResult r = e.actions().add_uploads("/Docs", {e.source("Downloads/a.txt", 100)});
    const pbc::AddResult skipped = e.actions().answer_ask(r.asks[0], AskChoice::Skip);
    PB_CHECK(skipped.error.ok());
    PB_CHECK_EQ(skipped.queued.size(), size_t(0));
    PB_CHECK_EQ(e.store->queue_count_open(), 0);
}

PB_TEST(an_answer_is_checked_again_against_what_the_reader_knows_now) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "x");
    e.cache("/Docs");
    const std::string src = e.source("Downloads/a.txt", 100);
    const pbc::AddResult r = e.actions().add_uploads("/Docs", {src});
    // while the question was open the source went away
    PB_CHECK(::unlink(src.c_str()) == 0);
    const pbc::AddResult kept = e.actions().answer_ask(r.asks[0], AskChoice::KeepBoth);
    PB_CHECK_EQ(kept.queued.size(), size_t(0));
    PB_CHECK_EQ(kept.refused.size(), size_t(1));
    PB_CHECK_EQ(kept.refused[0].key, std::string("source_unreadable"));
}

PB_TEST(a_folder_pick_into_an_existing_folder_merges_and_asks_per_file_or_applies_one_answer_to_all) {
    UpEnv e;
    e.server.add_folder("/Docs/trip");
    e.server.add_file("/Docs/trip/plan.md", "old plan");
    e.server.add_file("/Docs/trip/map.png", "old map");
    e.cache("/Docs");
    e.cache("/Docs/trip");
    e.source("Notes/trip/plan.md", 20);
    e.source("Notes/trip/map.png", 30);
    e.source("Notes/trip/new.txt", 5);
    auto act = e.actions();
    const pbc::FolderPick pick = act.count_folder_pick("/Docs", {e.root + "/Notes/trip"});
    // asking: the free file is queued, the two that collide come back as questions; the existing folder is used, not created
    {
        const pbc::AddResult r = act.confirm_folder_pick(pick, pbc::PickPolicy::Ask);
        PB_CHECK(r.error.ok());
        PB_CHECK_EQ(r.queued.size(), size_t(1));
        PB_CHECK_EQ(r.asks.size(), size_t(2));
        for (int64_t id : r.queued) PB_CHECK(e.item(id).kind == pbc::ItemKind::Upload);
    }
    UpEnv f;
    f.server.add_folder("/Docs/trip");
    f.server.add_file("/Docs/trip/plan.md", "old plan");
    f.server.add_file("/Docs/trip/map.png", "old map");
    f.cache("/Docs");
    f.cache("/Docs/trip");
    f.source("Notes/trip/plan.md", 20);
    f.source("Notes/trip/map.png", 30);
    auto act2 = f.actions();
    const pbc::FolderPick pick2 = act2.count_folder_pick("/Docs", {f.root + "/Notes/trip"});
    const pbc::AddResult both = act2.confirm_folder_pick(pick2, pbc::PickPolicy::KeepBoth);
    PB_CHECK_EQ(both.queued.size(), size_t(2));
    PB_CHECK(both.asks.empty());
    std::set<std::string> names;
    for (int64_t id : both.queued) names.insert(f.item(id).dst_name);
    PB_CHECK(names.count("plan (2).md") == 1 && names.count("map (2).png") == 1);

    UpEnv g;
    g.server.add_folder("/Docs/trip");
    g.server.add_file("/Docs/trip/plan.md", "old plan");
    g.cache("/Docs");
    g.cache("/Docs/trip");
    g.source("Notes/trip/plan.md", 20);
    auto act3 = g.actions();
    const pbc::AddResult rep = act3.confirm_folder_pick(act3.count_folder_pick("/Docs", {g.root + "/Notes/trip"}), pbc::PickPolicy::Replace);
    PB_CHECK_EQ(rep.queued.size(), size_t(1));
    PB_CHECK(g.item(rep.queued[0]).replace);
    UpEnv h;
    h.server.add_folder("/Docs/trip");
    h.server.add_file("/Docs/trip/plan.md", "old plan");
    h.cache("/Docs");
    h.cache("/Docs/trip");
    h.source("Notes/trip/plan.md", 20);
    auto act4 = h.actions();
    const pbc::AddResult skip = act4.confirm_folder_pick(act4.count_folder_pick("/Docs", {h.root + "/Notes/trip"}), pbc::PickPolicy::Skip);
    PB_CHECK_EQ(skip.queued.size(), size_t(0));
    PB_CHECK(skip.asks.empty());
}

PB_TEST(a_listing_that_was_never_fetched_cannot_ask_and_the_condition_of_the_request_decides) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "x");   // on the server, but the reader never listed /Docs
    const std::string src = e.source("Downloads/a.txt", 100);
    const pbc::AddResult r = e.actions().add_uploads("/Docs", {src});
    PB_CHECK_EQ(r.queued.size(), size_t(1));
    PB_CHECK(r.asks.empty());
    PB_CHECK_EQ(e.run().conflict, 1);   // the server's 412 holds it; nothing is overwritten
    PB_CHECK_EQ(e.server.node("/Docs/a.txt").content, std::string("x"));
}

PB_TEST_MAIN()
