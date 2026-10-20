// The cached listings (data-model.md "folders", "entries"): replaced as a whole in one transaction, sorted and paged for the list,
// bounded with the oldest folders going first, and never touching what belongs to the user.
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

pbc::RemoteEntry entry(const std::string& folder, const std::string& name, bool is_folder, std::optional<int64_t> size = std::nullopt, std::optional<int64_t> mtime = std::nullopt) {
    pbc::RemoteEntry e;
    e.name = name;
    e.path = pbc::join_remote(folder, name);
    e.kind = is_folder ? pbc::Kind::Folder : pbc::Kind::File;
    e.size = size;
    e.mtime = mtime;
    e.etag = "\"e-" + name + "\"";
    return e;
}

pbc::Listing listing(const std::string& folder, const std::vector<pbc::RemoteEntry>& entries) {
    pbc::Listing l;
    l.folder_etag = "\"folder-" + folder + "\"";
    l.entries = entries;
    return l;
}

std::unique_ptr<pbc::Store> open(const pbtest::TmpDir& d, TickClock& clock, int max_entries = 50000, int max_folders = 2000) {
    pbc::StoreOptions o;
    o.max_cached_entries = max_entries;
    o.max_cached_folders = max_folders;
    auto r = pbc::Store::open(d.file("data"), clock, o);
    if (!r.ok()) PB_FAIL_AT("cannot open the store: " + r.error.key);
    return std::move(r.value);
}

std::string names(const pbc::EntryPage& p) {
    std::string s;
    for (const auto& e : p.rows) s += (s.empty() ? "" : ",") + e.name;
    return s;
}

}  // namespace

PB_TEST(a_replaced_listing_is_stored_and_read_back_with_its_time) {
    pbtest::TmpDir d;
    TickClock clock;
    auto s = open(d, clock);
    PB_CHECK(!s->folder("/Books").has_value());
    PB_CHECK(s->replace_listing("/Books", listing("/Books", {entry("/Books", "a.epub", false, 10, 100)}), 5000).ok());
    const auto f = s->folder("/Books");
    PB_CHECK(f.has_value());
    PB_CHECK_EQ(*f->fetched_at, int64_t(5000));
    PB_CHECK_EQ(f->etag, std::string("\"folder-/Books\""));
    PB_CHECK_EQ(f->entry_count, 1);
    PB_CHECK(!f->gone);
    const auto page = s->entries_page("/Books", pbc::SortKey::Name, true, 0, 10);
    PB_CHECK_EQ(page.total, 1);
    PB_CHECK_EQ(page.rows[0].name, std::string("a.epub"));
    PB_CHECK_EQ(page.rows[0].path, std::string("/Books/a.epub"));
    PB_CHECK_EQ(*page.rows[0].size, int64_t(10));
}

PB_TEST(a_new_listing_replaces_the_old_rows_as_a_whole) {
    pbtest::TmpDir d;
    TickClock clock;
    auto s = open(d, clock);
    PB_CHECK(s->replace_listing("/", listing("/", {entry("/", "old1", false), entry("/", "old2", false)}), 1).ok());
    PB_CHECK(s->replace_listing("/", listing("/", {entry("/", "new", false)}), 2).ok());
    PB_CHECK_EQ(names(s->entries_page("/", pbc::SortKey::Name, true, 0, 10)), std::string("new"));
    PB_CHECK_EQ(*s->folder("/")->fetched_at, int64_t(2));
}

PB_TEST(a_failed_replace_keeps_the_old_rows_and_the_old_time) {
    pbtest::TmpDir d;
    TickClock clock;
    auto s = open(d, clock);
    PB_CHECK(s->replace_listing("/", listing("/", {entry("/", "keep", false)}), 1).ok());
    // two entries of one name break the primary key halfway through the insert
    const auto bad = s->replace_listing("/", listing("/", {entry("/", "x", false), entry("/", "x", false)}), 2);
    PB_CHECK(!bad.ok());
    PB_CHECK_EQ(names(s->entries_page("/", pbc::SortKey::Name, true, 0, 10)), std::string("keep"));
    PB_CHECK_EQ(*s->folder("/")->fetched_at, int64_t(1));
}

PB_TEST(a_listing_with_a_bad_name_stores_nothing_of_it_and_counts_no_more_than_the_limit) {
    pbtest::TmpDir d;
    TickClock clock;
    auto s = open(d, clock);
    pbc::Listing l = listing("/", {entry("/", "ok", false), entry("/", "a/b", false), entry("/", "", false), entry("/", "..", false)});
    PB_CHECK(s->replace_listing("/", l, 1).ok());
    PB_CHECK_EQ(names(s->entries_page("/", pbc::SortKey::Name, true, 0, 10)), std::string("ok"));
    PB_CHECK_EQ(s->folder("/")->entry_count, 1);
    std::vector<pbc::RemoteEntry> many;
    for (int i = 0; i < 10001; ++i) many.push_back(entry("/", "f" + std::to_string(i), false));
    PB_CHECK_EQ(s->replace_listing("/", listing("/", many), 3).key, std::string("listing_too_large"));
    PB_CHECK_EQ(names(s->entries_page("/", pbc::SortKey::Name, true, 0, 10)), std::string("ok"));
}

PB_TEST(a_folder_that_is_gone_stays_readable_and_is_marked) {
    pbtest::TmpDir d;
    TickClock clock;
    auto s = open(d, clock);
    PB_CHECK(s->replace_listing("/Old", listing("/Old", {entry("/Old", "a", false)}), 7).ok());
    PB_CHECK(s->mark_folder_gone("/Old").ok());
    PB_CHECK(s->folder("/Old")->gone);
    PB_CHECK_EQ(s->entries_page("/Old", pbc::SortKey::Name, true, 0, 10).total, 1);
    PB_CHECK(s->mark_folder_gone("/never-seen").ok());   // nothing is created for a folder that was never cached
    PB_CHECK(!s->folder("/never-seen").has_value());
    PB_CHECK(s->replace_listing("/Old", listing("/Old", {entry("/Old", "b", false)}), 9).ok());   // it came back
    PB_CHECK(!s->folder("/Old")->gone);
}

PB_TEST(the_page_is_sorted_with_folders_first_by_name_date_or_size) {
    pbtest::TmpDir d;
    TickClock clock;
    auto s = open(d, clock);
    PB_CHECK(s->replace_listing("/", listing("/", {
        entry("/", "banana.txt", false, 300, 10), entry("/", "Apple.txt", false, 100, 30), entry("/", "cherry.txt", false, 200, 20),
        entry("/", "Zeta", true, 5000, 5), entry("/", "alpha", true, 100, 50), entry("/", "unknown.txt", false, std::nullopt, std::nullopt)}), 1).ok());
    using pbc::SortKey;
    PB_CHECK_EQ(names(s->entries_page("/", SortKey::Name, true, 0, 20)), std::string("alpha,Zeta,Apple.txt,banana.txt,cherry.txt,unknown.txt"));   // case-folded
    PB_CHECK_EQ(names(s->entries_page("/", SortKey::Name, false, 0, 20)), std::string("Zeta,alpha,unknown.txt,cherry.txt,banana.txt,Apple.txt"));   // folders stay first
    PB_CHECK_EQ(names(s->entries_page("/", SortKey::Date, true, 0, 20)), std::string("Zeta,alpha,unknown.txt,banana.txt,cherry.txt,Apple.txt"));   // unknown dates count as the oldest
    PB_CHECK_EQ(names(s->entries_page("/", SortKey::Date, false, 0, 20)), std::string("alpha,Zeta,Apple.txt,cherry.txt,banana.txt,unknown.txt"));
    PB_CHECK_EQ(names(s->entries_page("/", SortKey::Size, true, 0, 20)), std::string("alpha,Zeta,unknown.txt,Apple.txt,cherry.txt,banana.txt"));
    PB_CHECK_EQ(names(s->entries_page("/", SortKey::Size, false, 0, 20)), std::string("Zeta,alpha,banana.txt,cherry.txt,Apple.txt,unknown.txt"));
}

PB_TEST(paging_uses_offset_and_limit_and_reports_the_total) {
    pbtest::TmpDir d;
    TickClock clock;
    auto s = open(d, clock);
    std::vector<pbc::RemoteEntry> v;
    for (int i = 0; i < 25; ++i) v.push_back(entry("/", (i < 10 ? "f0" : "f") + std::to_string(i), false));
    PB_CHECK(s->replace_listing("/", listing("/", v), 1).ok());
    const auto p0 = s->entries_page("/", pbc::SortKey::Name, true, 0, 10);
    const auto p2 = s->entries_page("/", pbc::SortKey::Name, true, 20, 10);
    PB_CHECK_EQ(p0.total, 25);
    PB_CHECK_EQ(p0.rows.size(), size_t(10));
    PB_CHECK_EQ(p2.rows.size(), size_t(5));
    PB_CHECK_EQ(p0.rows[0].name, std::string("f00"));
    PB_CHECK_EQ(p2.rows[4].name, std::string("f24"));
    PB_CHECK_EQ(s->entries_page("/", pbc::SortKey::Name, true, 40, 10).rows.size(), size_t(0));
    PB_CHECK_EQ(s->entries_page("/missing", pbc::SortKey::Name, true, 0, 10).total, 0);
}

PB_TEST(the_folder_limit_drops_the_oldest_folders_first_but_never_the_root_or_the_one_being_written) {
    pbtest::TmpDir d;
    TickClock clock;
    auto s = open(d, clock, 50000, 3);
    clock.t = 100;
    PB_CHECK(s->replace_listing("/", listing("/", {entry("/", "r", false)}), 1).ok());
    clock.t = 200;
    PB_CHECK(s->replace_listing("/a", listing("/a", {entry("/a", "x", false)}), 1).ok());
    clock.t = 300;
    PB_CHECK(s->replace_listing("/b", listing("/b", {entry("/b", "x", false)}), 1).ok());
    clock.t = 400;
    s->folder("/a");   // opening a folder renews it: /b is now the oldest
    clock.t = 500;
    PB_CHECK(s->replace_listing("/c", listing("/c", {entry("/c", "x", false)}), 1).ok());
    PB_CHECK(s->folder("/").has_value());
    PB_CHECK(s->folder("/a").has_value());
    PB_CHECK(!s->folder("/b").has_value());
    PB_CHECK(s->folder("/c").has_value());
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM entries WHERE parent = '/b'"), std::string("0"));   // its rows went with it
}

PB_TEST(the_entry_limit_evicts_whole_folders_and_keeps_the_folder_just_written) {
    pbtest::TmpDir d;
    TickClock clock;
    auto s = open(d, clock, 10, 2000);
    for (int f = 0; f < 4; ++f) {
        clock.t += 10;
        const std::string path = "/f" + std::to_string(f);
        std::vector<pbc::RemoteEntry> v;
        for (int i = 0; i < 4; ++i) v.push_back(entry(path, "e" + std::to_string(i), false));
        PB_CHECK(s->replace_listing(path, listing(path, v), 1).ok());
    }
    PB_CHECK(std::atoi(s->debug_query("SELECT count(*) FROM entries").c_str()) <= 10);
    PB_CHECK(s->folder("/f3").has_value());   // the last written survives
    PB_CHECK_EQ(s->entries_page("/f3", pbc::SortKey::Name, true, 0, 10).total, 4);
    PB_CHECK(!s->folder("/f0").has_value());  // the oldest went first
}

PB_TEST(eviction_never_touches_downloads_marks_favorites_or_the_journal) {
    pbtest::TmpDir d;
    TickClock clock;
    auto s = open(d, clock, 2, 2000);
    PB_CHECK(s->debug_exec("INSERT INTO local_files(remote_path, local_path, size, mtime, remote_etag, downloaded_at) VALUES('/f0/x', 'x', 1, 2, '\"e\"', 3)").empty());
    PB_CHECK(s->debug_exec("INSERT INTO offline_marks(remote_path, kind, marked_at) VALUES('/f0', 'folder', 1)").empty());
    PB_CHECK(s->debug_exec("INSERT INTO favorites(remote_path, kind, position) VALUES('/f0/x', 'file', 0)").empty());
    PB_CHECK(s->debug_exec("INSERT INTO downloads(remote_path, target_path, state, started_at, updated_at) VALUES('/f0/x', 'x', 'paused', 1, 1)").empty());
    for (int f = 0; f < 5; ++f) {
        clock.t += 10;
        const std::string path = "/f" + std::to_string(f);
        PB_CHECK(s->replace_listing(path, listing(path, {entry(path, "a", false), entry(path, "b", false)}), 1).ok());
    }
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM local_files"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM offline_marks"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM favorites"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM downloads"), std::string("1"));
}

PB_TEST(sort_and_last_folder_settings_are_kept) {
    pbtest::TmpDir d;
    TickClock clock;
    {
        auto s = open(d, clock);
        PB_CHECK(s->set_setting("sort_key", "size").ok());
        PB_CHECK(s->set_setting("sort_dir", "desc").ok());
        PB_CHECK(s->set_setting("last_folder", "/Books").ok());
    }
    auto s = open(d, clock);
    PB_CHECK_EQ(s->setting("sort_key"), std::string("size"));
    PB_CHECK_EQ(s->setting("last_folder"), std::string("/Books"));
}

PB_TEST_MAIN()
