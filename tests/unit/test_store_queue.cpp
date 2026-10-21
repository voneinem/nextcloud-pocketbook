// The persistent queue of changes the user made (specs/002-manage-server-files/data-model.md "queue_items", research R-6): what a row must look
// like to be accepted, the bound, groups that are inserted whole or not at all, the order and the dependencies in which items are taken, and
// the rules for removing them. Constitution Principle III: nothing is lost, discarded or overwritten until it is confirmed or removed.
#include <string>
#include <vector>

#include "pbc/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

pbc::SystemClock g_clock;

struct Env {
    pbtest::TmpDir dir;
    std::unique_ptr<pbc::Store> store;
    Env() {
        auto r = pbc::Store::open(dir.file("data"), g_clock);
        if (!r.ok()) PB_FAIL_AT("cannot open the store: " + r.error.key);
        store = std::move(r.value);
    }
};

pbc::QueueItem base(pbc::ItemKind kind) {
    pbc::QueueItem q;
    q.kind = kind;
    q.entry_kind = pbc::Kind::File;
    q.server = "https://cloud.example.org";
    q.user_id = "demo";
    return q;
}

pbc::QueueItem upload(const std::string& name = "a.txt", const std::string& parent = "/Docs") {
    pbc::QueueItem q = base(pbc::ItemKind::Upload);
    q.src_local = "/mnt/ext1/Downloads/" + name;
    q.dst_parent = parent;
    q.dst_name = name;
    return q;
}

pbc::QueueItem mkdir(const std::string& name, const std::string& parent = "/") {
    pbc::QueueItem q = base(pbc::ItemKind::Mkdir);
    q.entry_kind = pbc::Kind::Folder;
    q.dst_parent = parent;
    q.dst_name = name;
    return q;
}

pbc::QueueItem renaming(const std::string& from, const std::string& to, const std::string& etag = "\"abc\"") {
    pbc::QueueItem q = base(pbc::ItemKind::Rename);
    q.src_remote = from;
    q.src_etag = etag;
    q.dst_parent = from.substr(0, from.rfind('/') == 0 ? 1 : from.rfind('/'));
    q.dst_name = to;
    return q;
}

pbc::QueueItem move(const std::string& from, const std::string& to_parent, const std::string& etag = "\"abc\"") {
    pbc::QueueItem q = base(pbc::ItemKind::Move);
    q.src_remote = from;
    q.src_etag = etag;
    q.dst_parent = to_parent;
    q.dst_name = from.substr(from.rfind('/') + 1);
    return q;
}

pbc::QueueItem del(const std::string& path, const std::string& etag = "\"abc\"") {
    pbc::QueueItem q = base(pbc::ItemKind::Delete);
    q.src_remote = path;
    q.src_etag = etag;
    return q;
}

}  // namespace

PB_TEST(an_accepted_item_is_stored_before_anything_else_and_reads_back_byte_for_byte) {
    Env e;
    pbc::QueueItem q = upload("Q4 \xC3\xBC" "bersicht'; DROP TABLE queue_items;--.pdf");
    q.src_size = 123;
    const auto id = e.store->queue_add(q);
    PB_CHECK(id.ok());
    PB_CHECK(id.value > 0);
    const auto back = e.store->queue_item(id.value);
    PB_CHECK(back.has_value());
    PB_CHECK_EQ(back->id, id.value);
    PB_CHECK(back->kind == pbc::ItemKind::Upload);
    PB_CHECK(back->state == pbc::ItemState::Queued);
    PB_CHECK_EQ(back->step, std::string());
    PB_CHECK_EQ(back->dst_name, q.dst_name);
    PB_CHECK_EQ(back->src_local, q.src_local);
    PB_CHECK_EQ(back->dst_parent, std::string("/Docs"));
    PB_CHECK_EQ(back->server, std::string("https://cloud.example.org"));
    PB_CHECK_EQ(back->user_id, std::string("demo"));
    PB_CHECK_EQ(back->src_size.value_or(-1), 123LL);
    PB_CHECK(!back->depends_on.has_value());
    PB_CHECK(back->created_at > 0 && back->updated_at >= back->created_at);
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM queue_items"), std::string("1"));
    PB_CHECK_EQ(e.store->queue_count_open(), 1);
}

PB_TEST(a_long_path_and_unusual_unicode_are_kept_exactly) {
    Env e;
    pbc::QueueItem q = upload("Mu\xCC\x88nchen.txt");   // NFD: stored as it is
    q.src_local = "/mnt/ext1/" + std::string(3900, 'd') + "/file.txt";
    const auto id = e.store->queue_add(q);
    PB_CHECK(id.ok());
    const auto back = e.store->queue_item(id.value);
    PB_CHECK_EQ(back->src_local, q.src_local);
    PB_CHECK_EQ(back->dst_name, std::string("Mu\xCC\x88nchen.txt"));
}

PB_TEST(an_item_needs_an_account_a_valid_name_and_a_valid_target) {
    Env e;
    pbc::QueueItem q = upload();
    q.server.clear();
    PB_CHECK(!e.store->queue_add(q).ok());
    q = upload();
    q.user_id.clear();
    PB_CHECK(!e.store->queue_add(q).ok());
    for (const char* bad : {"", ".", "..", "a/b", "a\\b", "trail.", "trail ", "ctl\x01"}) {
        q = upload();
        q.dst_name = bad;
        const auto r = e.store->queue_add(q);
        PB_CHECK(!r.ok());
        PB_CHECK_EQ(r.error.key, std::string("name_forbidden"));
    }
    for (const char* bad : {"", "Docs", "/Docs/", "/a//b", "/a/../b", "/a/./b"}) {
        q = upload();
        q.dst_parent = bad;
        PB_CHECK(!e.store->queue_add(q).ok());
    }
    q = upload();
    q.src_local.clear();   // an upload needs a source
    PB_CHECK(!e.store->queue_add(q).ok());
    q = upload();
    q.src_local = "relative/path.txt";
    PB_CHECK(!e.store->queue_add(q).ok());
    PB_CHECK_EQ(e.store->queue_count_open(), 0);   // nothing half-inserted
}

PB_TEST(the_names_the_server_forbids_are_refused_with_the_saved_capabilities) {
    Env e;
    pbc::ServerCaps caps;
    caps.forbidden_names = {".htaccess"};
    caps.forbidden_chars = "?";
    PB_CHECK(e.store->save_server_caps(caps).ok());
    pbc::QueueItem q = upload(".htaccess");
    PB_CHECK_EQ(e.store->queue_add(q).error.key, std::string("name_forbidden"));
    q = upload("what?.txt");
    PB_CHECK_EQ(e.store->queue_add(q).error.key, std::string("name_forbidden"));
    PB_CHECK(e.store->queue_add(upload("fine.txt")).ok());
}

PB_TEST(a_replace_needs_a_strong_etag_and_a_file) {
    Env e;
    pbc::QueueItem q = upload();
    q.replace = true;
    PB_CHECK_EQ(e.store->queue_add(q).error.key, std::string("refresh_first"));   // no etag
    q.dst_etag = "W/\"weak\"";
    PB_CHECK_EQ(e.store->queue_add(q).error.key, std::string("refresh_first"));   // weak
    q.dst_etag = "\"strong\"";
    PB_CHECK(e.store->queue_add(q).ok());
    pbc::QueueItem folder = move("/A/dir", "/B");
    folder.entry_kind = pbc::Kind::Folder;
    folder.replace = true;
    folder.dst_etag = "\"strong\"";
    PB_CHECK(!e.store->queue_add(folder).ok());   // a folder is never replaced
    pbc::QueueItem mk = mkdir("x");
    mk.replace = true;
    mk.dst_etag = "\"strong\"";
    PB_CHECK(!e.store->queue_add(mk).ok());       // nor can a new folder replace anything
}

PB_TEST(rename_move_copy_and_delete_need_a_strong_etag_of_the_entry) {
    Env e;
    for (const char* etag : {"", "W/\"weak\""}) {
        PB_CHECK_EQ(e.store->queue_add(renaming("/A/old.txt", "new.txt", etag)).error.key, std::string("refresh_first"));
        PB_CHECK_EQ(e.store->queue_add(move("/A/old.txt", "/B", etag)).error.key, std::string("refresh_first"));
        pbc::QueueItem c = move("/A/old.txt", "/B", etag);
        c.kind = pbc::ItemKind::Copy;
        PB_CHECK_EQ(e.store->queue_add(c).error.key, std::string("refresh_first"));
        PB_CHECK_EQ(e.store->queue_add(del("/A/old.txt", etag)).error.key, std::string("refresh_first"));
    }
    PB_CHECK(e.store->queue_add(renaming("/A/old.txt", "new.txt")).ok());
    PB_CHECK(e.store->queue_add(move("/A/old.txt", "/B")).ok());
    PB_CHECK(e.store->queue_add(del("/A/old.txt")).ok());
    // the entry must name something below the root
    PB_CHECK(!e.store->queue_add(del("/")).ok());
    PB_CHECK(!e.store->queue_add(del("")).ok());
}

PB_TEST(a_move_into_itself_or_to_the_same_place_and_a_rename_to_the_same_name_are_refused) {
    Env e;
    pbc::QueueItem q = move("/A/dir", "/A/dir");
    q.entry_kind = pbc::Kind::Folder;
    PB_CHECK(!e.store->queue_add(q).ok());
    q = move("/A/dir", "/A/dir/sub");
    q.entry_kind = pbc::Kind::Folder;
    PB_CHECK(!e.store->queue_add(q).ok());
    PB_CHECK(!e.store->queue_add(move("/A/file.txt", "/A")).ok());                // the same place
    PB_CHECK(!e.store->queue_add(renaming("/A/file.txt", "file.txt")).ok());        // the same name
    PB_CHECK(e.store->queue_add(renaming("/A/file.txt", "File.txt")).ok());         // a case-only change is another name
    q = move("/A/dir", "/A/dirt");                                                // a sibling whose name only starts the same
    q.entry_kind = pbc::Kind::Folder;
    PB_CHECK(e.store->queue_add(q).ok());
}

PB_TEST(the_queue_is_bounded_and_a_full_queue_refuses_without_dropping_anything) {
    Env e;
    for (int i = 0; i < pbc::kMaxQueueItems; ++i) {
        const auto r = e.store->queue_add(upload("f" + std::to_string(i) + ".txt"));
        if (!r.ok()) PB_FAIL_AT("item " + std::to_string(i) + " refused: " + r.error.key);
    }
    PB_CHECK_EQ(e.store->queue_count_open(), pbc::kMaxQueueItems);
    const auto over = e.store->queue_add(upload("one-too-many.txt"));
    PB_CHECK(!over.ok());
    PB_CHECK_EQ(over.error.key, std::string("queue_full"));
    PB_CHECK_EQ(e.store->queue_count_open(), pbc::kMaxQueueItems);
    PB_CHECK_EQ(e.store->queue_page(0, 1).rows[0].dst_name, std::string("f0.txt"));   // the oldest is still there
    // done rows count too, until they are cleared
    PB_CHECK(e.store->debug_exec("UPDATE queue_items SET state = 'done' WHERE id <= 10").empty());
    PB_CHECK(!e.store->queue_add(upload("still-full.txt")).ok());
    PB_CHECK(e.store->queue_clear_done().ok());
    PB_CHECK(e.store->queue_add(upload("room-again.txt")).ok());
}

PB_TEST(a_group_is_inserted_whole_or_not_at_all_and_its_dependencies_point_to_the_new_ids) {
    Env e;
    std::vector<pbc::QueueItem> items = {mkdir("trip", "/"), upload("a.jpg", "/trip"), mkdir("raw", "/trip"), upload("b.nef", "/trip/raw")};
    const std::vector<int> deps = {-1, 0, 0, 2};
    const auto ids = e.store->queue_add_group(items, deps);
    PB_CHECK(ids.ok());
    PB_CHECK_EQ(ids.value.size(), size_t(4));
    const auto a = e.store->queue_item(ids.value[1]);
    PB_CHECK_EQ(a->depends_on.value_or(-1), ids.value[0]);
    PB_CHECK_EQ(e.store->queue_item(ids.value[3])->depends_on.value_or(-1), ids.value[2]);
    PB_CHECK(!e.store->queue_item(ids.value[0])->depends_on.has_value());
    const int64_t gid = e.store->queue_item(ids.value[0])->group_id.value_or(-1);
    PB_CHECK(gid > 0);
    for (int64_t id : ids.value) PB_CHECK_EQ(e.store->queue_item(id)->group_id.value_or(-2), gid);

    // one bad member refuses the whole group
    std::vector<pbc::QueueItem> bad = {mkdir("ok", "/"), upload("fine.txt", "/ok"), upload("bad/name.txt", "/ok")};
    const auto r = e.store->queue_add_group(bad, {-1, 0, 0});
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(e.store->queue_count_open(), 4);   // nothing of it was inserted

    // a group that does not fit refuses as a whole too
    for (int i = 4; i < pbc::kMaxQueueItems - 1; ++i) PB_CHECK(e.store->queue_add(upload("g" + std::to_string(i) + ".txt")).ok());
    std::vector<pbc::QueueItem> two = {upload("x1.txt"), upload("x2.txt")};
    const auto full = e.store->queue_add_group(two, {-1, -1});
    PB_CHECK(!full.ok());
    PB_CHECK_EQ(full.error.key, std::string("queue_full"));
    PB_CHECK_EQ(e.store->queue_count_open(), pbc::kMaxQueueItems - 1);
    // a dependency that points forward or outside the group is refused
    Env e2;
    PB_CHECK(!e2.store->queue_add_group({upload("a.txt"), upload("b.txt")}, {1, -1}).ok());
    PB_CHECK(!e2.store->queue_add_group({upload("a.txt")}, {5}).ok());
}

PB_TEST(items_are_taken_oldest_first_and_only_when_their_dependency_is_done) {
    Env e;
    const auto ids = e.store->queue_add_group({mkdir("trip"), upload("a.jpg", "/trip"), upload("loose.txt", "/Docs")}, {-1, 0, -1});
    PB_CHECK(ids.ok());
    const std::string server = "https://cloud.example.org";
    PB_CHECK_EQ(e.store->queue_count_sendable(server, "demo"), 2);   // the mkdir and the loose file; a.jpg waits for the folder
    auto next = e.store->queue_next(server, "demo");
    PB_CHECK(next.has_value());
    PB_CHECK_EQ(next->id, ids.value[0]);

    // the folder is being sent: it is not taken again and its dependant still waits; the independent item can go
    auto it = e.store->queue_item(ids.value[0]);
    it->state = pbc::ItemState::Sending;
    it->step = "request";
    PB_CHECK(e.store->queue_update(*it).ok());
    next = e.store->queue_next(server, "demo");
    PB_CHECK_EQ(next->id, ids.value[2]);

    // the folder is held (a conflict): the dependant never becomes sendable, the independent one still is
    it->state = pbc::ItemState::Conflict;
    it->reason = "name_exists";
    PB_CHECK(e.store->queue_update(*it).ok());
    PB_CHECK_EQ(e.store->queue_count_sendable(server, "demo"), 1);
    PB_CHECK_EQ(e.store->queue_next(server, "demo")->id, ids.value[2]);

    // the folder is done: the dependant is now the oldest sendable item
    it->state = pbc::ItemState::Done;
    PB_CHECK(e.store->queue_update(*it).ok());
    PB_CHECK_EQ(e.store->queue_next(server, "demo")->id, ids.value[1]);
    PB_CHECK_EQ(e.store->queue_count_sendable(server, "demo"), 2);
}

PB_TEST(failed_conflict_and_unknown_items_are_never_taken) {
    Env e;
    const auto a = e.store->queue_add(upload("a.txt"));
    const auto b = e.store->queue_add(upload("b.txt"));
    const auto c = e.store->queue_add(upload("c.txt"));
    const auto d = e.store->queue_add(upload("d.txt"));
    const std::pair<int64_t, pbc::ItemState> held[] = {{a.value, pbc::ItemState::Failed}, {b.value, pbc::ItemState::Conflict}, {c.value, pbc::ItemState::Unknown}};
    for (const auto& h : held) {
        auto it = e.store->queue_item(h.first);
        it->state = h.second;
        PB_CHECK(e.store->queue_update(*it).ok());
    }
    PB_CHECK_EQ(e.store->queue_next("https://cloud.example.org", "demo")->id, d.value);
    PB_CHECK_EQ(e.store->queue_count_sendable("https://cloud.example.org", "demo"), 1);
    PB_CHECK_EQ(e.store->queue_count_open(), 4);
}

PB_TEST(items_of_another_account_are_never_taken) {
    Env e;
    pbc::QueueItem other = upload("other.txt");
    other.user_id = "someone";
    PB_CHECK(e.store->queue_add(other).ok());
    pbc::QueueItem elsewhere = upload("elsewhere.txt");
    elsewhere.server = "https://other.example.org";
    PB_CHECK(e.store->queue_add(elsewhere).ok());
    const auto mine = e.store->queue_add(upload("mine.txt"));
    PB_CHECK_EQ(e.store->queue_count_sendable("https://cloud.example.org", "demo"), 1);
    PB_CHECK_EQ(e.store->queue_next("https://cloud.example.org", "demo")->id, mine.value);
    PB_CHECK(!e.store->queue_next("https://cloud.example.org", "nobody").has_value());
    PB_CHECK_EQ(e.store->queue_count_open(), 3);   // the other account's items are kept, not dropped
}

PB_TEST(the_queue_pages_in_creation_order_with_the_total) {
    Env e;
    for (int i = 0; i < 25; ++i) PB_CHECK(e.store->queue_add(upload("p" + std::to_string(100 + i) + ".txt")).ok());
    auto p = e.store->queue_page(0, 10);
    PB_CHECK_EQ(p.total, 25);
    PB_CHECK_EQ(p.rows.size(), size_t(10));
    PB_CHECK_EQ(p.rows[0].dst_name, std::string("p100.txt"));
    p = e.store->queue_page(20, 10);
    PB_CHECK_EQ(p.rows.size(), size_t(5));
    PB_CHECK_EQ(p.rows[4].dst_name, std::string("p124.txt"));
    PB_CHECK(e.store->queue_page(100, 10).rows.empty());
}

PB_TEST(update_changes_only_the_permitted_fields_and_validates_a_new_name) {
    Env e;
    const auto id = e.store->queue_add(upload("a.txt"));
    auto it = e.store->queue_item(id.value);
    it->state = pbc::ItemState::Sending;
    it->step = "chunks";
    it->upload_id = "pbc-0123";
    it->chunk_size = 10 << 20;
    it->checksum = "abcd";
    it->src_size = 99;
    it->src_mtime = 1790000000;
    it->result_etag = "\"new\"";
    it->result_fileid = 77;
    it->reason = "";
    // fields that must not change through an update
    it->id = id.value;
    it->server = "https://evil.example.org";
    it->user_id = "intruder";
    it->kind = pbc::ItemKind::Delete;
    it->created_at = 1;
    PB_CHECK(e.store->queue_update(*it).ok());
    auto back = e.store->queue_item(id.value);
    PB_CHECK(back->state == pbc::ItemState::Sending);
    PB_CHECK_EQ(back->step, std::string("chunks"));
    PB_CHECK_EQ(back->upload_id, std::string("pbc-0123"));
    PB_CHECK_EQ(back->chunk_size.value_or(-1), 10LL << 20);
    PB_CHECK_EQ(back->result_fileid.value_or(-1), 77LL);
    PB_CHECK_EQ(back->server, std::string("https://cloud.example.org"));
    PB_CHECK_EQ(back->user_id, std::string("demo"));
    PB_CHECK(back->kind == pbc::ItemKind::Upload);
    PB_CHECK(back->created_at != 1);
    // a new name is validated
    back->dst_name = "bad/name";
    PB_CHECK_EQ(e.store->queue_update(*back).key, std::string("name_forbidden"));
    back->dst_name = "renamed.txt";
    PB_CHECK(e.store->queue_update(*back).ok());
    PB_CHECK_EQ(e.store->queue_item(id.value)->dst_name, std::string("renamed.txt"));
    // an item that is gone cannot be updated
    pbc::QueueItem ghost = *back;
    ghost.id = 9999;
    PB_CHECK(!e.store->queue_update(ghost).ok());
}

PB_TEST(remove_refuses_a_sending_item_and_fails_the_dependants_of_a_removed_folder) {
    Env e;
    const auto ids = e.store->queue_add_group({mkdir("trip"), upload("a.jpg", "/trip"), upload("b.jpg", "/trip")}, {-1, 0, 0});
    PB_CHECK(ids.ok());
    auto folder = e.store->queue_item(ids.value[0]);
    folder->state = pbc::ItemState::Sending;
    folder->step = "request";
    PB_CHECK(e.store->queue_update(*folder).ok());
    PB_CHECK(!e.store->queue_remove(ids.value[0]).ok());                        // being sent: not removable
    PB_CHECK(e.store->queue_item(ids.value[0]).has_value());
    folder->state = pbc::ItemState::Queued;
    folder->step.clear();
    PB_CHECK(e.store->queue_update(*folder).ok());
    PB_CHECK(e.store->queue_remove(ids.value[0]).ok());
    PB_CHECK(!e.store->queue_item(ids.value[0]).has_value());
    for (int i : {1, 2}) {
        const auto dep = e.store->queue_item(ids.value[i]);
        PB_CHECK(dep.has_value());                                              // not lost silently ...
        PB_CHECK(dep->state == pbc::ItemState::Failed);                         // ... but failed, with the reason
        PB_CHECK_EQ(dep->reason, std::string("target_gone"));
    }
    PB_CHECK(e.store->queue_remove(ids.value[1]).ok());                         // removing an upload touches nothing else
    PB_CHECK(e.store->queue_item(ids.value[2]).has_value());
    PB_CHECK(e.store->queue_remove(424242).ok());                               // idempotent
}

PB_TEST(clearing_done_rows_keeps_everything_else) {
    Env e;
    const auto a = e.store->queue_add(upload("a.txt"));
    const auto b = e.store->queue_add(upload("b.txt"));
    const auto c = e.store->queue_add(upload("c.txt"));
    auto done = e.store->queue_item(a.value);
    done->state = pbc::ItemState::Done;
    PB_CHECK(e.store->queue_update(*done).ok());
    auto held = e.store->queue_item(b.value);
    held->state = pbc::ItemState::Unknown;
    PB_CHECK(e.store->queue_update(*held).ok());
    PB_CHECK(e.store->queue_clear_done().ok());
    PB_CHECK(!e.store->queue_item(a.value).has_value());
    PB_CHECK(e.store->queue_item(b.value).has_value());
    PB_CHECK(e.store->queue_item(c.value).has_value());
    PB_CHECK_EQ(e.store->queue_count_open(), 2);
}

PB_TEST(open_items_about_an_entry_and_below_a_folder_are_found) {
    Env e;
    PB_CHECK(e.store->queue_add(upload("in.txt", "/A/sub")).ok());
    PB_CHECK(e.store->queue_add(renaming("/A/old.txt", "new.txt")).ok());
    PB_CHECK(e.store->queue_add(move("/A/sub/deep.txt", "/B")).ok());
    PB_CHECK(e.store->queue_add(upload("elsewhere.txt", "/Other")).ok());
    PB_CHECK(e.store->queue_add(upload("close.txt", "/Asub")).ok());   // a sibling folder whose name starts the same
    const auto about = e.store->queue_items_for_source("/A/old.txt");
    PB_CHECK_EQ(about.size(), size_t(1));
    const auto below = e.store->queue_items_below("/A");
    PB_CHECK_EQ(below.size(), size_t(3));   // the upload into /A/sub, the rename of /A/old.txt, the move of /A/sub/deep.txt
    for (const auto& q : below) PB_CHECK(q.dst_name != "elsewhere.txt" && q.dst_name != "close.txt");
    PB_CHECK_EQ(e.store->queue_items_below("/").size(), size_t(5));
    // done rows are not "open"
    auto it = e.store->queue_page(0, 1).rows[0];
    it.state = pbc::ItemState::Done;
    PB_CHECK(e.store->queue_update(it).ok());
    PB_CHECK_EQ(e.store->queue_items_below("/A").size(), size_t(2));
}

PB_TEST(the_queue_survives_clearing_the_cache_signing_out_and_a_reopened_store) {
    pbtest::TmpDir d;
    {
        auto r = pbc::Store::open(d.file("data"), g_clock);
        PB_CHECK(r.ok());
        PB_CHECK(r.value->queue_add(upload("keep.txt")).ok());
        PB_CHECK(r.value->clear_cache().ok());
        PB_CHECK(r.value->clear_account().ok());
        PB_CHECK_EQ(r.value->queue_count_open(), 1);
    }
    auto again = pbc::Store::open(d.file("data"), g_clock);
    PB_CHECK(again.ok());
    PB_CHECK_EQ(again.value->queue_count_open(), 1);
    PB_CHECK_EQ(again.value->queue_page(0, 5).rows[0].dst_name, std::string("keep.txt"));
}

PB_TEST_MAIN()
