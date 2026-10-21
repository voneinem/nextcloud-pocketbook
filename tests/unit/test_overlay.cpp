// The folder listing with the open queue items merged in (specs/002-manage-server-files/data-model.md "Listing overlay", research R-6.8,
// spec FR-008): a change that is queued is visible at once and marked, an entry keeps its identity under a pending rename, and sorting and
// paging stay in SQL. Constitution Principle III: eviction of cache rows never changes what a queued item shows for itself.
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "pbc/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

pbc::SystemClock g_clock;
const char* kServer = "https://cloud.example.org";

struct Env {
    pbtest::TmpDir dir;
    std::unique_ptr<pbc::Store> store;
    Env() {
        auto r = pbc::Store::open(dir.file("data"), g_clock);
        if (!r.ok()) PB_FAIL_AT("cannot open the store: " + r.error.key);
        store = std::move(r.value);
        pbc::AccountRow a;
        a.base_url = kServer;
        a.login_name = "demo";
        a.user_id = "demo";
        if (!store->save_account(a).ok()) PB_FAIL_AT("save_account failed");
    }
    void cache(const std::string& folder, const std::vector<pbc::RemoteEntry>& entries) {
        pbc::Listing l;
        l.entries = entries;
        if (!store->replace_listing(folder, l, 1000).ok()) PB_FAIL_AT("replace_listing failed");
    }
    int64_t add(const pbc::QueueItem& q) {
        const auto r = store->queue_add(q);
        if (!r.ok()) PB_FAIL_AT("queue_add failed: " + r.error.key);
        return r.value;
    }
    void set_state(int64_t id, pbc::ItemState s) {
        auto it = store->queue_item(id);
        it->state = s;
        if (!store->queue_update(*it).ok()) PB_FAIL_AT("queue_update failed");
    }
};

pbc::RemoteEntry file(const std::string& parent, const std::string& name, int64_t size = 10, int64_t mtime = 100, const std::string& etag = "\"e\"") {
    pbc::RemoteEntry e;
    e.name = name;
    e.path = parent == "/" ? "/" + name : parent + "/" + name;
    e.kind = pbc::Kind::File;
    e.size = size;
    e.mtime = mtime;
    e.etag = etag;
    return e;
}

pbc::RemoteEntry folder(const std::string& parent, const std::string& name) {
    pbc::RemoteEntry e = file(parent, name, 0, 50);
    e.kind = pbc::Kind::Folder;
    e.size.reset();
    return e;
}

pbc::QueueItem base(pbc::ItemKind k) {
    pbc::QueueItem q;
    q.kind = k;
    q.server = kServer;
    q.user_id = "demo";
    return q;
}

pbc::QueueItem upload(const std::string& parent, const std::string& name, int64_t size = 7, bool replace = false) {
    pbc::QueueItem q = base(pbc::ItemKind::Upload);
    q.src_local = "/mnt/ext1/Downloads/" + name;
    q.src_size = size;
    q.dst_parent = parent;
    q.dst_name = name;
    if (replace) {
        q.replace = true;
        q.dst_etag = "\"e\"";
    }
    return q;
}

pbc::QueueItem mkdir_item(const std::string& parent, const std::string& name) {
    pbc::QueueItem q = base(pbc::ItemKind::Mkdir);
    q.entry_kind = pbc::Kind::Folder;
    q.dst_parent = parent;
    q.dst_name = name;
    return q;
}

pbc::QueueItem rename_item(const std::string& path, const std::string& to) {
    pbc::QueueItem q = base(pbc::ItemKind::Rename);
    q.src_remote = path;
    q.src_etag = "\"e\"";
    q.dst_parent = path.substr(0, path.rfind('/') == 0 ? 1 : path.rfind('/'));
    q.dst_name = to;
    return q;
}

pbc::QueueItem move_item(const std::string& path, const std::string& to_parent, pbc::Kind kind = pbc::Kind::File) {
    pbc::QueueItem q = base(pbc::ItemKind::Move);
    q.entry_kind = kind;
    q.src_remote = path;
    q.src_etag = "\"e\"";
    q.dst_parent = to_parent;
    q.dst_name = path.substr(path.rfind('/') + 1);
    return q;
}

pbc::QueueItem copy_item(const std::string& path, const std::string& to_parent) {
    pbc::QueueItem q = move_item(path, to_parent);
    q.kind = pbc::ItemKind::Copy;
    return q;
}

pbc::QueueItem delete_item(const std::string& path) {
    pbc::QueueItem q = base(pbc::ItemKind::Delete);
    q.src_remote = path;
    q.src_etag = "\"e\"";
    return q;
}

std::vector<std::string> names(const pbc::OverlayPage& p) {
    std::vector<std::string> out;
    for (const auto& r : p.rows) out.push_back(r.entry.name);
    return out;
}

const pbc::OverlayRow* row(const pbc::OverlayPage& p, const std::string& name) {
    for (const auto& r : p.rows)
        if (r.entry.name == name) return &r;
    return nullptr;
}

}  // namespace

PB_TEST(without_queue_items_the_overlay_is_the_cached_page) {
    Env e;
    e.cache("/Docs", {file("/Docs", "b.txt"), file("/Docs", "a.txt"), folder("/Docs", "sub")});
    const auto plain = e.store->entries_page("/Docs", pbc::SortKey::Name, true, 0, 50);
    const auto over = e.store->entries_page_overlay("/Docs", pbc::SortKey::Name, true, 0, 50);
    PB_CHECK_EQ(over.total, plain.total);
    PB_CHECK_EQ(over.rows.size(), plain.rows.size());
    for (size_t i = 0; i < plain.rows.size(); ++i) {
        PB_CHECK_EQ(over.rows[i].entry.name, plain.rows[i].name);
        PB_CHECK_EQ(over.rows[i].entry.path, plain.rows[i].path);
        PB_CHECK_EQ(over.rows[i].pending, std::string());
        PB_CHECK_EQ(over.rows[i].sync_state, std::string());
        PB_CHECK_EQ(over.rows[i].item_id, int64_t(0));
    }
}

PB_TEST(a_queued_upload_and_a_queued_folder_appear_as_pending_rows_in_the_target) {
    Env e;
    e.cache("/Docs", {file("/Docs", "a.txt"), folder("/Docs", "old")});
    const int64_t up = e.add(upload("/Docs", "new.pdf", 2048));
    const int64_t mk = e.add(mkdir_item("/Docs", "fresh"));
    const auto p = e.store->entries_page_overlay("/Docs", pbc::SortKey::Name, true, 0, 50);
    PB_CHECK_EQ(p.total, 4);
    const auto* f = row(p, "new.pdf");
    PB_CHECK(f != nullptr);
    PB_CHECK_EQ(f->pending, std::string("new"));
    PB_CHECK_EQ(f->sync_state, std::string("queued"));
    PB_CHECK_EQ(f->item_id, up);
    PB_CHECK(f->entry.kind == pbc::Kind::File);
    PB_CHECK_EQ(f->entry.size.value_or(-1), 2048LL);
    PB_CHECK(!f->entry.mtime.has_value());
    PB_CHECK_EQ(f->entry.path, std::string("/Docs/new.pdf"));   // the future path
    PB_CHECK(f->pending_row);                                    // it exists only in the queue
    PB_CHECK(!row(p, "a.txt")->pending_row);
    const auto* d = row(p, "fresh");
    PB_CHECK(d != nullptr);
    PB_CHECK(d->entry.kind == pbc::Kind::Folder);
    PB_CHECK_EQ(d->pending, std::string("new"));
    PB_CHECK_EQ(d->item_id, mk);
    // folders first, then files, each by name
    PB_CHECK((names(p) == std::vector<std::string>{"fresh", "old", "a.txt", "new.pdf"}));
    // other folders are not affected
    PB_CHECK_EQ(e.store->entries_page_overlay("/Other", pbc::SortKey::Name, true, 0, 50).total, 0);
}

PB_TEST(replacing_an_upload_marks_the_existing_row_and_adds_no_second_one) {
    Env e;
    e.cache("/Docs", {file("/Docs", "a.txt")});
    const int64_t id = e.add(upload("/Docs", "a.txt", 99, true));
    const auto p = e.store->entries_page_overlay("/Docs", pbc::SortKey::Name, true, 0, 50);
    PB_CHECK_EQ(p.total, 1);
    PB_CHECK_EQ(p.rows[0].pending, std::string("replacing"));
    PB_CHECK_EQ(p.rows[0].item_id, id);
    PB_CHECK_EQ(p.rows[0].entry.etag, std::string("\"e\""));   // the cached facts stay
    PB_CHECK_EQ(p.rows[0].entry.size.value_or(-1), 10LL);
}

PB_TEST(a_rename_shows_the_new_name_keeps_the_old_path_and_sorts_by_the_new_name) {
    Env e;
    e.cache("/Docs", {file("/Docs", "a.txt"), file("/Docs", "m.txt"), file("/Docs", "z.txt")});
    const int64_t id = e.add(rename_item("/Docs/a.txt", "zz-last.txt"));
    const auto p = e.store->entries_page_overlay("/Docs", pbc::SortKey::Name, true, 0, 50);
    PB_CHECK_EQ(p.total, 3);   // renamed, not duplicated
    PB_CHECK((names(p) == std::vector<std::string>{"m.txt", "z.txt", "zz-last.txt"}));
    const auto* r = row(p, "zz-last.txt");
    PB_CHECK_EQ(r->pending, std::string("renamed"));
    PB_CHECK_EQ(r->item_id, id);
    PB_CHECK_EQ(r->entry.path, std::string("/Docs/a.txt"));   // the identity a tap uses
    PB_CHECK_EQ(r->entry.size.value_or(-1), 10LL);
    PB_CHECK(row(p, "a.txt") == nullptr);
}

PB_TEST(a_move_keeps_the_entry_in_the_source_and_adds_a_pending_row_in_the_target) {
    Env e;
    e.cache("/A", {file("/A", "f.txt"), folder("/A", "dir")});
    e.add(move_item("/A/f.txt", "/B"));
    e.add(move_item("/A/dir", "/B", pbc::Kind::Folder));
    const auto src = e.store->entries_page_overlay("/A", pbc::SortKey::Name, true, 0, 50);
    PB_CHECK_EQ(src.total, 2);
    PB_CHECK_EQ(row(src, "f.txt")->pending, std::string("moving"));
    PB_CHECK(!row(src, "f.txt")->pending_row);   // the source is a real entry
    PB_CHECK_EQ(row(src, "dir")->pending, std::string("moving"));
    const auto dst = e.store->entries_page_overlay("/B", pbc::SortKey::Name, true, 0, 50);   // /B was never cached
    PB_CHECK_EQ(dst.total, 2);
    PB_CHECK_EQ(row(dst, "f.txt")->pending, std::string("moving"));
    PB_CHECK(row(dst, "f.txt")->pending_row);    // the target is a pending row
    PB_CHECK(row(dst, "dir")->entry.kind == pbc::Kind::Folder);
    PB_CHECK_EQ(row(dst, "f.txt")->entry.path, std::string("/B/f.txt"));
}

PB_TEST(a_copy_adds_a_pending_row_only_in_the_target_and_a_delete_keeps_the_entry_marked) {
    Env e;
    e.cache("/A", {file("/A", "f.txt"), file("/A", "gone.txt")});
    e.add(copy_item("/A/f.txt", "/B"));
    e.add(delete_item("/A/gone.txt"));
    const auto src = e.store->entries_page_overlay("/A", pbc::SortKey::Name, true, 0, 50);
    PB_CHECK_EQ(src.total, 2);
    PB_CHECK_EQ(row(src, "f.txt")->pending, std::string());       // a copy does not touch its source
    PB_CHECK_EQ(row(src, "gone.txt")->pending, std::string("deleting"));
    const auto dst = e.store->entries_page_overlay("/B", pbc::SortKey::Name, true, 0, 50);
    PB_CHECK_EQ(dst.total, 1);
    PB_CHECK_EQ(dst.rows[0].pending, std::string("copying"));
}

PB_TEST(the_sync_state_follows_the_item_and_done_items_are_not_an_overlay) {
    Env e;
    e.cache("/Docs", {});
    const int64_t a = e.add(upload("/Docs", "a.txt"));
    const int64_t b = e.add(upload("/Docs", "b.txt"));
    const int64_t c = e.add(upload("/Docs", "c.txt"));
    const int64_t d = e.add(upload("/Docs", "d.txt"));
    const int64_t f = e.add(upload("/Docs", "e.txt"));
    e.set_state(a, pbc::ItemState::Sending);
    e.set_state(b, pbc::ItemState::Failed);
    e.set_state(c, pbc::ItemState::Conflict);
    e.set_state(d, pbc::ItemState::Unknown);
    e.set_state(f, pbc::ItemState::Done);
    const auto p = e.store->entries_page_overlay("/Docs", pbc::SortKey::Name, true, 0, 50);
    PB_CHECK_EQ(p.total, 4);
    PB_CHECK_EQ(row(p, "a.txt")->sync_state, std::string("sending"));
    PB_CHECK_EQ(row(p, "b.txt")->sync_state, std::string("failed"));
    PB_CHECK_EQ(row(p, "c.txt")->sync_state, std::string("conflict"));
    PB_CHECK_EQ(row(p, "d.txt")->sync_state, std::string("unknown"));
    PB_CHECK(row(p, "e.txt") == nullptr);
}

PB_TEST(sorting_by_date_and_size_keeps_folders_first_and_places_unknown_values_as_the_smallest) {
    Env e;
    e.cache("/Docs", {file("/Docs", "old.txt", 500, 100), file("/Docs", "new.txt", 5, 900), folder("/Docs", "sub")});
    e.add(upload("/Docs", "pending.txt", 100));
    const auto by_date_asc = e.store->entries_page_overlay("/Docs", pbc::SortKey::Date, true, 0, 50);
    PB_CHECK((names(by_date_asc) == std::vector<std::string>{"sub", "pending.txt", "old.txt", "new.txt"}));   // no date = the smallest
    const auto by_date_desc = e.store->entries_page_overlay("/Docs", pbc::SortKey::Date, false, 0, 50);
    PB_CHECK((names(by_date_desc) == std::vector<std::string>{"sub", "new.txt", "old.txt", "pending.txt"}));
    const auto by_size_desc = e.store->entries_page_overlay("/Docs", pbc::SortKey::Size, false, 0, 50);
    PB_CHECK((names(by_size_desc) == std::vector<std::string>{"sub", "old.txt", "pending.txt", "new.txt"}));
}

PB_TEST(paging_over_cached_and_pending_rows_is_consistent) {
    Env e;
    std::vector<pbc::RemoteEntry> entries;
    for (int i = 0; i < 30; ++i) entries.push_back(file("/Docs", "c" + std::to_string(100 + i) + ".txt"));
    e.cache("/Docs", entries);
    for (int i = 0; i < 10; ++i) e.add(upload("/Docs", "p" + std::to_string(100 + i) + ".txt"));
    std::vector<std::string> all;
    for (int offset = 0; offset < 40; offset += 12) {
        const auto page = e.store->entries_page_overlay("/Docs", pbc::SortKey::Name, true, offset, 12);
        PB_CHECK_EQ(page.total, 40);
        for (const auto& r : page.rows) all.push_back(r.entry.name);
    }
    PB_CHECK_EQ(all.size(), size_t(40));
    std::vector<std::string> sorted = all;
    std::sort(sorted.begin(), sorted.end());
    PB_CHECK(all == sorted);   // c100.. before p100.., one sequence over the pages
    PB_CHECK(e.store->entries_page_overlay("/Docs", pbc::SortKey::Name, true, 100, 12).rows.empty());
}

PB_TEST(a_folder_that_was_never_cached_still_shows_its_pending_rows) {
    Env e;
    e.add(upload("/Never/Seen", "a.txt"));
    const auto p = e.store->entries_page_overlay("/Never/Seen", pbc::SortKey::Name, true, 0, 50);
    PB_CHECK_EQ(p.total, 1);
    PB_CHECK_EQ(p.rows[0].pending, std::string("new"));
}

PB_TEST(eviction_of_cache_rows_does_not_change_what_a_queued_item_shows_for_itself) {
    Env e;
    e.cache("/Docs", {file("/Docs", "a.txt")});
    const int64_t up = e.add(upload("/Docs", "new.pdf"));
    e.add(rename_item("/Docs/a.txt", "b.txt"));
    PB_CHECK(e.store->clear_cache().ok());   // the cache is dropped as eviction would
    const auto p = e.store->entries_page_overlay("/Docs", pbc::SortKey::Name, true, 0, 50);
    const auto* r = row(p, "new.pdf");
    PB_CHECK(r != nullptr);
    PB_CHECK_EQ(r->item_id, up);
    PB_CHECK_EQ(r->pending, std::string("new"));
    PB_CHECK_EQ(e.store->queue_count_open(), 2);   // and both items are still there
}

PB_TEST(items_of_another_account_are_not_shown) {
    Env e;
    pbc::QueueItem other = upload("/Docs", "foreign.txt");
    other.user_id = "someone";
    e.add(other);
    PB_CHECK_EQ(e.store->entries_page_overlay("/Docs", pbc::SortKey::Name, true, 0, 50).total, 0);
    e.add(upload("/Docs", "mine.txt"));
    PB_CHECK_EQ(e.store->entries_page_overlay("/Docs", pbc::SortKey::Name, true, 0, 50).total, 1);
}

PB_TEST(a_page_of_a_big_cache_with_a_full_queue_is_fast) {
    Env e;
    pbc::Listing l;
    for (int i = 0; i < 10000; ++i) l.entries.push_back(file("/Big", "f" + std::to_string(100000 + i) + ".txt"));
    PB_CHECK(e.store->replace_listing("/Big", l, 1000).ok());
    // 2,000 open items by SQL: this measures the query, not the validation
    PB_CHECK(e.store->debug_exec("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 2000) "
                                 "INSERT INTO queue_items(kind, entry_kind, state, server, user_id, src_local, dst_parent, dst_name, created_at, updated_at) "
                                 "SELECT 'upload', 'file', 'queued', 'https://cloud.example.org', 'demo', '/mnt/ext1/x', '/Big', 'q' || i || '.txt', 1, 1 FROM n").empty());
    const auto t0 = std::chrono::steady_clock::now();
    const auto page = e.store->entries_page_overlay("/Big", pbc::SortKey::Name, true, 0, 12);
    const auto page2 = e.store->entries_page_overlay("/Big", pbc::SortKey::Date, false, 4000, 12);
    const long ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    std::printf("overlay: two pages of 12 over 10,000 cached rows and 2,000 open items in %ld ms (unverified until measured on the device)\n", ms);
    PB_CHECK_EQ(page.total, 12000);
    PB_CHECK_EQ(page.rows.size(), size_t(12));
    PB_CHECK_EQ(page2.rows.size(), size_t(12));
    PB_CHECK(ms < 1000);
}

PB_TEST_MAIN()
