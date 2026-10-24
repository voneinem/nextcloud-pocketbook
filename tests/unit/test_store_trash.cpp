// The stored copy of the last trash list (specs/006-trash-and-versions, research R-4, contracts/core-interfaces.md "store additions"): replaced in
// one transaction, bounded to 1,000 rows, bound to the account, shown from the store when there is no connection.
#include <memory>
#include <string>

#include "pbc/store.h"
#include "pbc/trash_types.h"
#include "pbc/types.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

pbc::SystemClock g_clock;

std::unique_ptr<pbc::Store> open_current(const pbtest::TmpDir& d) {
    auto r = pbc::Store::open(d.file("data"), g_clock);
    if (!r.ok()) PB_FAIL_AT("cannot open the store: " + r.error.key);
    return std::move(r.value);
}

pbc::TrashItem item(const std::string& trash_name, int64_t deleted_at, bool folder = false) {
    pbc::TrashItem i;
    i.trash_name = trash_name;
    i.name = trash_name.substr(0, trash_name.rfind('.'));
    i.original_dir = "/Books";
    i.original_name = i.name;
    i.original_known = true;
    i.deleted_at = deleted_at;
    i.kind = folder ? pbc::Kind::Folder : pbc::Kind::File;
    if (!folder) i.size = 100 + deleted_at % 7;
    i.fileid = 9000 + deleted_at % 1000;
    return i;
}

pbc::TrashList list_of(int n) {
    pbc::TrashList l;
    for (int k = 0; k < n; ++k) l.items.push_back(item("f" + std::to_string(k) + ".d" + std::to_string(2000000000 - k), 2000000000 - k));
    l.total_seen = n;
    return l;
}

}  // namespace

PB_TEST(nothing_is_stored_before_the_first_fetch) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    const auto r = s->load_trash(1);
    PB_CHECK(r.ok());
    PB_CHECK(r.value.items.empty());
    PB_CHECK(!r.value.fetched_at.has_value());
}

PB_TEST(a_list_round_trips_with_its_time_and_comes_back_marked_as_stored) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    pbc::TrashList l;
    l.items.push_back(item("a.epub.d300", 300));
    l.items.push_back(item("Old Notes.d200", 200, true));
    pbc::TrashItem odd = item("x.d100", 100);
    odd.original_known = false;
    odd.original_dir.clear();
    odd.original_name.clear();
    odd.deleted_at.reset();
    odd.availability = pbc::Availability::Encrypted;
    l.items.push_back(odd);
    pbc::TrashItem unreadable;
    unreadable.unreadable = true;
    l.items.push_back(unreadable);
    l.total_seen = 4;
    PB_CHECK(s->replace_trash(l, 1760000000, 1).ok());
    const auto r = s->load_trash(1);
    PB_CHECK(r.ok());
    PB_CHECK(r.value.from_store);
    PB_CHECK(r.value.fetched_at == std::optional<int64_t>(1760000000));
    PB_CHECK(!r.value.truncated);
    PB_CHECK_EQ(r.value.items.size(), size_t(4));
    PB_CHECK_EQ(r.value.total_seen, 4);
    const auto& a = r.value.items[0];
    PB_CHECK_EQ(a.trash_name, std::string("a.epub.d300"));
    PB_CHECK_EQ(a.name, std::string("a.epub"));
    PB_CHECK_EQ(a.original_dir, std::string("/Books"));
    PB_CHECK_EQ(a.original_name, std::string("a.epub"));
    PB_CHECK(a.original_known && !a.unreadable);
    PB_CHECK(a.deleted_at == std::optional<int64_t>(300));
    PB_CHECK(a.kind == pbc::Kind::File && a.size == l.items[0].size && a.fileid == l.items[0].fileid);
    PB_CHECK(r.value.items[1].kind == pbc::Kind::Folder && !r.value.items[1].size.has_value());
    const auto& x = r.value.items[2];
    PB_CHECK(!x.original_known && x.original_dir.empty() && !x.deleted_at.has_value());
    PB_CHECK(x.availability == pbc::Availability::Encrypted);
    PB_CHECK(r.value.items[3].unreadable && r.value.items[3].trash_name.empty());
}

PB_TEST(a_refresh_replaces_the_list_in_the_order_given) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK(s->replace_trash(list_of(5), 100, 1).ok());
    pbc::TrashList l;
    l.items.push_back(item("n2.d9", 9));
    l.items.push_back(item("n1.d8", 8));
    PB_CHECK(s->replace_trash(l, 200, 1).ok());
    const auto r = s->load_trash(1);
    PB_CHECK_EQ(r.value.items.size(), size_t(2));
    PB_CHECK_EQ(r.value.items[0].trash_name, std::string("n2.d9"));
    PB_CHECK_EQ(r.value.items[1].trash_name, std::string("n1.d8"));
    PB_CHECK(r.value.fetched_at == std::optional<int64_t>(200));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM trash_items"), std::string("2"));
}

PB_TEST(an_empty_trash_is_stored_as_known_empty_with_its_time) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK(s->replace_trash(list_of(3), 100, 1).ok());
    PB_CHECK(s->replace_trash(pbc::TrashList(), 300, 1).ok());
    const auto r = s->load_trash(1);
    PB_CHECK(r.value.items.empty());
    PB_CHECK(r.value.fetched_at == std::optional<int64_t>(300));
}

PB_TEST(a_replace_is_one_transaction_a_failure_keeps_the_old_list) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK(s->replace_trash(list_of(4), 100, 1).ok());
    // the third row of the new list cannot be inserted
    PB_CHECK(s->debug_exec("CREATE TRIGGER stop_at_two BEFORE INSERT ON trash_items WHEN new.position = 2 BEGIN SELECT RAISE(ABORT, 'no'); END").empty());
    pbc::TrashList l;
    for (int k = 0; k < 5; ++k) l.items.push_back(item("new" + std::to_string(k) + ".d" + std::to_string(50 - k), 50 - k));
    PB_CHECK(!s->replace_trash(l, 999, 1).ok());
    const auto r = s->load_trash(1);
    PB_CHECK_EQ(r.value.items.size(), size_t(4));   // the old list, whole
    PB_CHECK_EQ(r.value.items[0].trash_name.substr(0, 1), std::string("f"));
    PB_CHECK(r.value.fetched_at == std::optional<int64_t>(100));   // and its time
}

PB_TEST(at_most_1000_rows_are_stored_and_the_cut_is_remembered) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    pbc::TrashList l = list_of(1200);
    l.truncated = true;
    PB_CHECK(s->replace_trash(l, 100, 1).ok());
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM trash_items"), std::string("1000"));
    const auto r = s->load_trash(1);
    PB_CHECK_EQ(r.value.items.size(), pbc::kMaxTrashRows);
    PB_CHECK(r.value.truncated);
    PB_CHECK_EQ(r.value.items.front().trash_name, l.items.front().trash_name);   // the first 1,000 rows are the ones kept
    PB_CHECK_EQ(r.value.items.back().trash_name, l.items[999].trash_name);
}

PB_TEST(a_list_is_shown_only_to_the_account_it_belongs_to) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    PB_CHECK(s->replace_trash(list_of(3), 100, 1).ok());
    const auto other = s->load_trash(2);
    PB_CHECK(other.ok());
    PB_CHECK(other.value.items.empty() && !other.value.fetched_at.has_value());
    PB_CHECK_EQ(s->load_trash(1).value.items.size(), size_t(3));
    // a list stored for another account replaces it
    PB_CHECK(s->replace_trash(list_of(2), 200, 2).ok());
    PB_CHECK(s->load_trash(1).value.items.empty());
    PB_CHECK_EQ(s->load_trash(2).value.items.size(), size_t(2));
}

PB_TEST(one_item_is_removed_by_its_trash_name_and_nothing_else) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    pbc::TrashList l = list_of(3);
    pbc::TrashItem u1, u2;
    u1.unreadable = u2.unreadable = true;
    l.items.push_back(u1);
    l.items.push_back(u2);
    PB_CHECK(s->replace_trash(l, 100, 1).ok());
    PB_CHECK(s->remove_trash_item(l.items[1].trash_name).ok());
    auto r = s->load_trash(1);
    PB_CHECK_EQ(r.value.items.size(), size_t(4));
    for (const auto& i : r.value.items) PB_CHECK(i.trash_name != l.items[1].trash_name);
    // an empty trash name (an unreadable item) never removes anything: it cannot name one item
    PB_CHECK(s->remove_trash_item("").ok());
    PB_CHECK_EQ(s->load_trash(1).value.items.size(), size_t(4));
    // an unknown name is not an error
    PB_CHECK(s->remove_trash_item("nope.d1").ok());
    PB_CHECK_EQ(s->load_trash(1).value.items.size(), size_t(4));
}

PB_TEST(clearing_removes_the_rows_and_every_setting_of_the_list) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    pbc::TrashList l = list_of(3);
    l.truncated = true;
    PB_CHECK(s->replace_trash(l, 100, 1).ok());
    PB_CHECK_EQ(s->setting("trash_fetched_at"), std::string("100"));
    PB_CHECK_EQ(s->setting("trash_truncated"), std::string("1"));
    PB_CHECK_EQ(s->setting("trash_account"), std::string("1"));
    PB_CHECK(s->clear_trash().ok());
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM trash_items"), std::string("0"));
    for (const char* k : {"trash_fetched_at", "trash_truncated", "trash_account"}) PB_CHECK_EQ(s->setting(k), std::string());
    const auto r = s->load_trash(1);
    PB_CHECK(r.value.items.empty() && !r.value.fetched_at.has_value());
}

PB_TEST(signing_out_or_changing_the_account_drops_the_stored_list) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    pbc::AccountRow a;
    a.base_url = "https://cloud.example.org";
    a.login_name = "demo";
    a.user_id = "demo";
    PB_CHECK(s->save_account(a).ok());
    const int64_t id = 1;   // the one account row
    PB_CHECK(s->replace_trash(list_of(3), 100, id).ok());
    PB_CHECK_EQ(s->load_trash(id).value.items.size(), size_t(3));
    PB_CHECK(s->clear_account().ok());   // the trash names belong to the account that was signed out
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM trash_items"), std::string("0"));
    PB_CHECK(s->save_account(a).ok());
    PB_CHECK(s->replace_trash(list_of(2), 100, id).ok());
    pbc::AccountRow b = a;
    b.base_url = "https://other.example.org";
    PB_CHECK(s->save_account(b).ok());   // another server: the cached listings go, so does the trash list
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM trash_items"), std::string("0"));
}

// ---- after a restore (research R-14): the cache of the folder the item came back to ----

namespace {

pbc::RemoteEntry folder_entry(const std::string& parent, const std::string& name, const std::string& etag) {
    pbc::RemoteEntry e;
    e.name = name;
    e.path = (parent == "/" ? "" : parent) + "/" + name;
    e.kind = pbc::Kind::Folder;
    e.etag = etag;
    return e;
}

}  // namespace

PB_TEST(a_change_in_a_folder_drops_its_cached_listing_and_forgets_the_versions_above) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    pbc::Listing root;
    root.folder_etag = "r1";
    root.entries.push_back(folder_entry("/", "Books", "b1"));
    PB_CHECK(s->replace_listing("/", root, 100).ok());
    pbc::Listing books;
    books.folder_etag = "b1";
    books.entries.push_back(folder_entry("/Books", "Sub", "s1"));
    pbc::RemoteEntry file;
    file.name = "a.epub";
    file.path = "/Books/a.epub";
    file.etag = "f1";
    books.entries.push_back(file);
    PB_CHECK(s->replace_listing("/Books", books, 100).ok());
    pbc::Listing sub;
    sub.folder_etag = "s1";
    PB_CHECK(s->replace_listing("/Books/Sub", sub, 100).ok());
    PB_CHECK(s->forget_folder_after_change("/Books").ok());
    PB_CHECK(!s->folder("/Books").has_value());                                   // the next visit fetches it again
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM entries WHERE parent = '/Books'"), std::string("0"));
    PB_CHECK(s->folder("/Books/Sub").has_value());                                 // a folder below is not touched
    PB_CHECK_EQ(s->debug_query("SELECT etag FROM folders WHERE path = '/'"), std::string(""));            // the versions above are forgotten
    PB_CHECK_EQ(s->debug_query("SELECT etag FROM entries WHERE parent = '/' AND name = 'Books'"), std::string(""));
    PB_CHECK(s->folder("/").has_value());                                          // the root listing stays
}

PB_TEST(a_change_in_the_root_forgets_its_version_and_keeps_the_listing) {
    pbtest::TmpDir d;
    auto s = open_current(d);
    pbc::Listing root;
    root.folder_etag = "r1";
    root.entries.push_back(folder_entry("/", "Books", "b1"));
    PB_CHECK(s->replace_listing("/", root, 100).ok());
    PB_CHECK(s->forget_folder_after_change("/").ok());
    PB_CHECK(s->folder("/").has_value());
    PB_CHECK_EQ(s->debug_query("SELECT etag FROM folders WHERE path = '/'"), std::string(""));
    PB_CHECK(s->forget_folder_after_change("/Never/Cached").ok());   // nothing cached: nothing to do, no error
}

PB_TEST_MAIN()
