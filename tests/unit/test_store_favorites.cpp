// The cached favorites and the offline search (data-model.md "favorites", contracts/core-interfaces.md): replaced as a whole in one
// transaction, in the order the server gave; the search looks at what the reader knows (cached listings, favorites, downloaded copies)
// and at nothing else; results of a server search live in memory only.
#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "pbc/nc_listing.h"
#include "pbc/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

struct TickClock : pbc::Clock {
    int64_t t = 1792497600;
    int64_t now() const override { return t; }
};

pbc::RemoteEntry entry(const std::string& folder, const std::string& name, bool is_folder = false, int64_t size = 10) {
    pbc::RemoteEntry e;
    e.name = name;
    e.path = pbc::join_remote(folder, name);
    e.kind = is_folder ? pbc::Kind::Folder : pbc::Kind::File;
    e.size = size;
    e.mtime = 1792000000;
    e.etag = "\"e-" + name + "\"";
    e.mime = "text/plain";
    return e;
}

std::unique_ptr<pbc::Store> open(const pbtest::TmpDir& d, TickClock& clock, int max_entries = 50000) {
    pbc::StoreOptions o;
    o.max_cached_entries = max_entries;
    auto r = pbc::Store::open(d.file("data"), clock, o);
    if (!r.ok()) PB_FAIL_AT("cannot open the store: " + r.error.key);
    return std::move(r.value);
}

void cache(pbc::Store& s, const std::string& folder, const std::vector<pbc::RemoteEntry>& entries) {
    pbc::Listing l;
    l.folder_etag = "\"f\"";
    l.entries = entries;
    PB_CHECK(s.replace_listing(folder, l, 1792497600).ok());
}

std::string paths(const std::vector<pbc::SearchHit>& v) {
    std::string s;
    for (const auto& h : v) s += (s.empty() ? "" : ",") + h.entry.path;
    return s;
}

}  // namespace

PB_TEST(favorites_are_replaced_as_a_whole_in_the_order_received) {
    pbtest::TmpDir d;
    TickClock clock;
    auto s = open(d, clock);
    PB_CHECK(s->favorites().empty());
    PB_CHECK(!s->favorites_fetched_at().has_value());
    PB_CHECK(s->replace_favorites({entry("/B", "z.txt"), entry("/A", "a.txt"), entry("/", "m", true)}, 1792497700).ok());
    auto v = s->favorites();
    PB_CHECK_EQ(v.size(), size_t(3));
    PB_CHECK_EQ(v[0].path, std::string("/B/z.txt"));   // not sorted: the order of the server
    PB_CHECK_EQ(v[1].path, std::string("/A/a.txt"));
    PB_CHECK(v[2].kind == pbc::Kind::Folder);
    PB_CHECK_EQ(*s->favorites_fetched_at(), int64_t(1792497700));
    PB_CHECK_EQ(*v[0].size, int64_t(10));

    PB_CHECK(s->replace_favorites({entry("/C", "c.txt")}, 1792497800).ok());
    v = s->favorites();
    PB_CHECK_EQ(v.size(), size_t(1));
    PB_CHECK_EQ(v[0].path, std::string("/C/c.txt"));
    PB_CHECK_EQ(*s->favorites_fetched_at(), int64_t(1792497800));
}

PB_TEST(a_failed_replace_keeps_the_old_rows_and_the_old_time) {
    pbtest::TmpDir d;
    TickClock clock;
    auto s = open(d, clock);
    PB_CHECK(s->replace_favorites({entry("/A", "a.txt")}, 100).ok());
    const auto bad = s->replace_favorites({entry("/B", "b.txt"), entry("/B", "b.txt")}, 200);   // the same path twice
    PB_CHECK(!bad.ok());
    PB_CHECK_EQ(s->favorites().size(), size_t(1));
    PB_CHECK_EQ(s->favorites()[0].path, std::string("/A/a.txt"));
    PB_CHECK_EQ(*s->favorites_fetched_at(), int64_t(100));
}

PB_TEST(favorites_survive_the_eviction_of_the_listing_cache_and_a_reopen) {
    pbtest::TmpDir d;
    TickClock clock;
    {
        auto s = open(d, clock, 5);
        PB_CHECK(s->replace_favorites({entry("/A", "a.txt")}, 100).ok());
        cache(*s, "/x", {entry("/x", "1"), entry("/x", "2"), entry("/x", "3")});
        clock.t += 10;
        cache(*s, "/y", {entry("/y", "1"), entry("/y", "2"), entry("/y", "3")});   // evicts /x
        PB_CHECK_EQ(s->favorites().size(), size_t(1));
    }
    auto s = open(d, clock);
    PB_CHECK_EQ(s->favorites().size(), size_t(1));
}

PB_TEST(the_offline_search_reads_listings_favorites_and_downloaded_copies_and_says_where_each_came_from) {
    pbtest::TmpDir d;
    TickClock clock;
    auto s = open(d, clock);
    cache(*s, "/Books", {entry("/Books", "Moby Dick.epub"), entry("/Books", "Other.txt"), entry("/Books", "Moby", true)});
    PB_CHECK(s->replace_favorites({entry("/Fav", "moby notes.txt"), entry("/Books", "Moby Dick.epub")}, 100).ok());
    pbc::LocalFile lf;
    lf.remote_path = "/Old/Moby Old.pdf";
    lf.local_path = "Old/Moby Old.pdf";
    lf.size = 5;
    lf.mtime = 1;
    lf.remote_etag = "\"x\"";
    lf.downloaded_at = 1;
    PB_CHECK(s->commit_download(lf, 0).ok());

    const auto hits = s->search_cached("moby", 50);
    PB_CHECK_EQ(hits.size(), size_t(4));
    std::map<std::string, std::string> source;
    for (const auto& h : hits) source[h.entry.path] = h.source;
    PB_CHECK_EQ(source["/Books/Moby Dick.epub"], std::string("reader"));   // once, although it is a favorite too
    PB_CHECK_EQ(source["/Books/Moby"], std::string("reader"));
    PB_CHECK_EQ(source["/Fav/moby notes.txt"], std::string("favorites"));
    PB_CHECK_EQ(source["/Old/Moby Old.pdf"], std::string("download"));
    PB_CHECK(source.count("/Books/Other.txt") == 0);
}

PB_TEST(the_match_is_case_and_accent_insensitive_and_a_wildcard_is_just_a_character) {
    pbtest::TmpDir d;
    TickClock clock;
    auto s = open(d, clock);
    cache(*s, "/D", {entry("/D", "\xC3\x9C" "ber uns.txt"), entry("/D", "50%_off.txt"), entry("/D", "500 off.txt"), entry("/D", "a_b.txt"), entry("/D", "axb.txt")});
    PB_CHECK_EQ(paths(s->search_cached("UBER", 10)), std::string("/D/\xC3\x9C" "ber uns.txt"));
    PB_CHECK_EQ(paths(s->search_cached("50%_off", 10)), std::string("/D/50%_off.txt"));   // % and _ match themselves only
    PB_CHECK_EQ(paths(s->search_cached("a_b", 10)), std::string("/D/a_b.txt"));
    PB_CHECK(s->search_cached("", 10).empty());
    PB_CHECK(s->search_cached("   ", 10).empty());
    PB_CHECK(s->search_cached("nothing like this", 10).empty());
}

PB_TEST(the_limit_is_honored) {
    pbtest::TmpDir d;
    TickClock clock;
    auto s = open(d, clock);
    std::vector<pbc::RemoteEntry> many;
    for (int i = 0; i < 30; ++i) many.push_back(entry("/D", "book" + std::to_string(i) + ".epub"));
    cache(*s, "/D", many);
    PB_CHECK_EQ(s->search_cached("book", 7).size(), size_t(7));
    PB_CHECK_EQ(s->search_cached("book", 100).size(), size_t(30));
}

PB_TEST(five_thousand_rows_are_searched_within_a_tenth_of_a_second_on_the_host) {
    pbtest::TmpDir d;
    TickClock clock;
    auto s = open(d, clock);
    for (int f = 0; f < 5; ++f) {
        std::vector<pbc::RemoteEntry> many;
        for (int i = 0; i < 1000; ++i) many.push_back(entry("/F" + std::to_string(f), "name" + std::to_string(i) + (i == 777 ? "-needle" : "") + ".txt"));
        cache(*s, "/F" + std::to_string(f), many);
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto hits = s->search_cached("needle", 100);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    PB_CHECK_EQ(hits.size(), size_t(5));
    std::printf("search of 5000 rows: %.1f ms (unverified for the device)\n", ms);
    PB_CHECK(ms <= 100.0);
}

PB_TEST(server_search_results_are_held_in_memory_only_and_can_be_looked_up_by_path) {
    pbtest::TmpDir d;
    TickClock clock;
    {
        auto s = open(d, clock);
        PB_CHECK(!s->entry("/Far/away.epub").has_value());
        s->set_search_results({entry("/Far", "away.epub"), entry("/Far", "farther.epub")});
        const auto e = s->entry("/Far/away.epub");
        PB_CHECK(e.has_value());
        PB_CHECK_EQ(e->etag, std::string("\"e-away.epub\""));
        PB_CHECK_EQ(*e->size, int64_t(10));
        PB_CHECK(!s->folder("/Far").has_value());   // no listing row is made
        PB_CHECK(s->search_cached("away", 10).empty());   // and the offline search does not find them
        s->set_search_results({});
        PB_CHECK(!s->entry("/Far/away.epub").has_value());
        s->set_search_results({entry("/Far", "away.epub")});
    }
    auto s = open(d, clock);   // nothing of it survives a restart
    PB_CHECK(!s->entry("/Far/away.epub").has_value());
}

PB_TEST(an_entry_of_the_favorites_is_found_by_path_when_no_listing_has_it) {
    pbtest::TmpDir d;
    TickClock clock;
    auto s = open(d, clock);
    PB_CHECK(s->replace_favorites({entry("/Fav", "x.epub")}, 5).ok());
    const auto e = s->entry("/Fav/x.epub");
    PB_CHECK(e.has_value());
    PB_CHECK_EQ(e->name, std::string("x.epub"));
    PB_CHECK(e->favorite);
    cache(*s, "/Fav", {entry("/Fav", "x.epub")});   // a listing is the better source once it exists
    PB_CHECK(s->entry("/Fav/x.epub").has_value());
}

PB_TEST(saving_another_account_drops_the_cached_lists_and_the_favorites_with_their_time_but_not_the_downloads) {
    pbtest::TmpDir d;
    TickClock clock;
    auto s = open(d, clock);
    pbc::AccountRow a;
    a.base_url = "https://cloud.example.org";
    a.login_name = "demo";
    a.user_id = "demo";
    PB_CHECK(s->save_account(a).ok());
    cache(*s, "/Books", {entry("/Books", "a.epub")});
    PB_CHECK(s->replace_favorites({entry("/Books", "a.epub")}, 100).ok());
    pbc::LocalFile lf;
    lf.remote_path = "/Books/a.epub";
    lf.local_path = "Books/a.epub";
    lf.size = 5;
    lf.mtime = 1;
    lf.remote_etag = "\"x\"";
    lf.downloaded_at = 1;
    PB_CHECK(s->commit_download(lf, 0).ok());

    a.user_id = "other";   // another user on the same server
    PB_CHECK(s->save_account(a).ok());
    PB_CHECK(!s->folder("/Books").has_value());
    PB_CHECK(s->favorites().empty());
    PB_CHECK(!s->favorites_fetched_at().has_value());   // "never fetched", so the page fetches them once
    PB_CHECK(s->local_file("/Books/a.epub").has_value());
}

PB_TEST_MAIN()
