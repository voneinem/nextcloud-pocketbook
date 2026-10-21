// Turning the user's picks into queue items (specs/002-manage-server-files, US1, FR-001 to FR-008, FR-026): the logic behind the Upload action
// lives in the core so that it is proven on the host (constitution Principle II). Nothing here touches the network: a pick only writes the queue.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "pbc/queue_actions.h"
#include "pbc/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

pbc::SystemClock g_clock;
const char* kServer = "https://cloud.example.org";

void touch(const std::string& path, const std::string& data = "x") {
    int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    PB_CHECK(fd >= 0);
    if (!data.empty()) PB_CHECK(::write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size()));
    ::close(fd);
}

void mkdirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0700);
}

struct Env {
    pbtest::TmpDir dir;
    std::unique_ptr<pbc::Store> store;
    pbc::SourceRules rules;
    std::string root;
    Env() {
        root = dir.file("root");
        mkdirs(root + "/Downloads");
        mkdirs(root + "/Notes");
        mkdirs(root + "/system/pbcloud");
        rules.root = root;
        rules.deny = {root + "/system/pbcloud"};
        auto r = pbc::Store::open(dir.file("data"), g_clock);
        if (!r.ok()) PB_FAIL_AT("cannot open the store: " + r.error.key);
        store = std::move(r.value);
        pbc::AccountRow a;
        a.base_url = kServer;
        a.login_name = "demo";
        a.user_id = "demo";
        PB_CHECK(store->save_account(a).ok());
    }
    pbc::QueueActions actions() { return pbc::QueueActions(*store, rules); }
    std::string file(const std::string& rel, const std::string& data = "content") {
        const std::string p = root + "/" + rel;
        mkdirs(p.substr(0, p.rfind('/')));
        touch(p, data);
        return p;
    }
};

std::vector<pbc::QueueItem> all_items(pbc::Store& s) { return s.queue_page(0, 5000).rows; }

}  // namespace

PB_TEST(picked_files_are_queued_for_the_folder_with_their_size_and_nothing_else_happens) {
    Env e;
    const std::string a = e.file("Downloads/a.pdf", "aaaa");
    const std::string b = e.file("Downloads/b.epub", "bbbbbbbb");
    auto act = e.actions();
    const auto r = act.add_uploads("/Docs", {a, b});
    PB_CHECK(r.error.ok());
    PB_CHECK_EQ(r.queued.size(), size_t(2));
    PB_CHECK(r.refused.empty());
    const auto items = all_items(*e.store);
    PB_CHECK_EQ(items.size(), size_t(2));
    PB_CHECK(items[0].kind == pbc::ItemKind::Upload);
    PB_CHECK(items[0].state == pbc::ItemState::Queued);
    PB_CHECK_EQ(items[0].dst_parent, std::string("/Docs"));
    PB_CHECK_EQ(items[0].dst_name, std::string("a.pdf"));
    PB_CHECK_EQ(items[0].src_size.value_or(-1), 4LL);
    PB_CHECK(items[0].src_mtime.has_value());
    PB_CHECK_EQ(items[0].server, std::string(kServer));
    PB_CHECK_EQ(items[0].user_id, std::string("demo"));
    char* real = ::realpath(a.c_str(), nullptr);
    PB_CHECK_EQ(items[0].src_local, std::string(real));   // the canonical path, checked
    std::free(real);
    PB_CHECK_EQ(items[1].dst_name, std::string("b.epub"));
    PB_CHECK_EQ(items[1].src_size.value_or(-1), 8LL);
    // the files on the reader are untouched
    PB_CHECK_EQ(e.dir.read("root/Downloads/a.pdf"), std::string("aaaa"));
}

PB_TEST(a_file_the_reader_may_not_send_is_refused_with_its_reason_and_the_others_go_on) {
    Env e;
    const std::string ok = e.file("Downloads/fine.txt", "fine");
    const std::string empty = e.file("Downloads/empty.txt", "");
    const std::string secret = e.file("system/pbcloud/credentials", "app-secret");
    const std::string outside = e.dir.file("outside.txt");
    touch(outside, "out");
    PB_CHECK(::symlink(secret.c_str(), (e.root + "/Downloads/innocent.txt").c_str()) == 0);
    const std::string badname = e.file("Downloads/what?.txt", "x");
    const std::string huge = e.root + "/Downloads/huge.bin";
    {
        int fd = ::open(huge.c_str(), O_CREAT | O_WRONLY, 0600);
        PB_CHECK(fd >= 0);
        PB_CHECK(::ftruncate(fd, 4294967296ll) == 0);
        ::close(fd);
    }
    PB_CHECK(e.store->save_server_caps([] {
        pbc::ServerCaps c;
        c.forbidden_chars = "?";
        return c;
    }()).ok());
    auto act = e.actions();
    const auto r = act.add_uploads("/Docs", {ok, empty, secret, outside, e.root + "/Downloads/innocent.txt", badname, huge, e.root + "/Downloads/missing.txt"});
    PB_CHECK(r.error.ok());
    PB_CHECK_EQ(r.queued.size(), size_t(1));
    PB_CHECK_EQ(all_items(*e.store)[0].dst_name, std::string("fine.txt"));
    auto key_of = [&](const std::string& path) {
        for (const auto& x : r.refused)
            if (x.path == path) return x.key;
        return std::string("(not refused)");
    };
    PB_CHECK_EQ(key_of(empty), std::string("source_unreadable"));
    PB_CHECK_EQ(key_of(secret), std::string("source_not_allowed"));
    PB_CHECK_EQ(key_of(outside), std::string("source_not_allowed"));
    PB_CHECK_EQ(key_of(e.root + "/Downloads/innocent.txt"), std::string("source_not_allowed"));
    PB_CHECK_EQ(key_of(badname), std::string("name_forbidden"));
    PB_CHECK_EQ(key_of(huge), std::string("too_large_for_storage"));
    PB_CHECK_EQ(key_of(e.root + "/Downloads/missing.txt"), std::string("source_unreadable"));
    PB_CHECK_EQ(r.refused.size(), size_t(7));
}

PB_TEST(without_an_account_or_a_valid_target_nothing_is_queued) {
    Env e;
    const std::string a = e.file("Downloads/a.pdf");
    auto act = e.actions();
    PB_CHECK(!act.add_uploads("Docs", {a}).error.ok());          // not a remote path
    PB_CHECK(!act.add_uploads("/a/../b", {a}).error.ok());
    PB_CHECK_EQ(e.store->queue_count_open(), 0);
    PB_CHECK(e.store->clear_account().ok());
    const auto r = act.add_uploads("/Docs", {a});
    PB_CHECK(!r.error.ok());
    PB_CHECK_EQ(r.error.key, std::string("auth_failed"));
    PB_CHECK_EQ(e.store->queue_count_open(), 0);
}

PB_TEST(a_pick_that_does_not_fit_the_queue_is_refused_as_a_whole) {
    Env e;
    auto act = e.actions();
    std::vector<std::string> paths;
    for (int i = 0; i < 5; ++i) paths.push_back(e.file("Downloads/f" + std::to_string(i) + ".txt"));
    for (int i = 0; i < pbc::kMaxQueueItems - 3; ++i) {
        pbc::QueueItem q;
        q.kind = pbc::ItemKind::Upload;
        q.server = kServer;
        q.user_id = "demo";
        q.src_local = "/mnt/ext1/x" + std::to_string(i);
        q.dst_parent = "/Docs";
        q.dst_name = "n" + std::to_string(i);
        PB_CHECK(e.store->queue_add(q).ok());
    }
    const auto r = act.add_uploads("/Docs", paths);
    PB_CHECK(!r.error.ok());
    PB_CHECK_EQ(r.error.key, std::string("queue_full"));
    PB_CHECK_EQ(e.store->queue_count_open(), pbc::kMaxQueueItems - 3);   // not one of the five was queued
    PB_CHECK_EQ(act.headroom(), 3);
}

PB_TEST(a_folder_pick_is_counted_first_and_nothing_is_queued_by_the_count) {
    Env e;
    e.file("Notes/trip/plan.md", "plan");
    e.file("Notes/trip/photos/a.jpg", "aa");
    e.file("Notes/trip/photos/raw/b.nef", "bbbb");
    auto act = e.actions();
    const auto pick = act.count_folder_pick("/Docs", {e.root + "/Notes/trip"});
    PB_CHECK(pick.error.ok());
    PB_CHECK_EQ(pick.folders_count, 3);
    PB_CHECK_EQ(pick.files, 3);
    PB_CHECK_EQ(pick.bytes, 10LL);
    PB_CHECK(!pick.over_limit);
    PB_CHECK_EQ(e.store->queue_count_open(), 0);
}

PB_TEST(a_confirmed_folder_pick_queues_a_folder_per_folder_and_an_upload_per_file_parents_first) {
    Env e;
    e.file("Notes/trip/plan.md", "plan");
    e.file("Notes/trip/photos/a.jpg", "aa");
    e.file("Notes/trip/photos/raw/b.nef", "bbbb");
    auto act = e.actions();
    const auto pick = act.count_folder_pick("/Docs", {e.root + "/Notes/trip"});
    const auto r = act.confirm_folder_pick(pick);
    PB_CHECK(r.error.ok());
    PB_CHECK_EQ(r.queued.size(), size_t(6));
    const auto items = all_items(*e.store);
    PB_CHECK_EQ(items.size(), size_t(6));
    // one group, creation order = parents before what they contain
    const int64_t group = items[0].group_id.value_or(-1);
    PB_CHECK(group > 0);
    std::vector<std::string> mkdirs_made, files_up;
    for (const auto& q : items) {
        PB_CHECK_EQ(q.group_id.value_or(-2), group);
        if (q.kind == pbc::ItemKind::Mkdir) {
            PB_CHECK(q.entry_kind == pbc::Kind::Folder);
            mkdirs_made.push_back(q.dst_parent + (q.dst_parent == "/" ? "" : "/") + q.dst_name);
        } else {
            PB_CHECK(q.kind == pbc::ItemKind::Upload);
            files_up.push_back(q.dst_parent + "/" + q.dst_name);
        }
    }
    PB_CHECK((mkdirs_made == std::vector<std::string>{"/Docs/trip", "/Docs/trip/photos", "/Docs/trip/photos/raw"}));
    std::sort(files_up.begin(), files_up.end());
    PB_CHECK((files_up == std::vector<std::string>{"/Docs/trip/photos/a.jpg", "/Docs/trip/photos/raw/b.nef", "/Docs/trip/plan.md"}));
    // each item waits for the creation of its folder; the top folder waits for nothing
    std::map<std::string, int64_t> made;
    for (const auto& q : items)
        if (q.kind == pbc::ItemKind::Mkdir) made[q.dst_parent + (q.dst_parent == "/" ? "" : "/") + q.dst_name] = q.id;
    for (const auto& q : items) {
        if (q.kind == pbc::ItemKind::Mkdir && q.dst_name == "trip") PB_CHECK(!q.depends_on.has_value());
        else PB_CHECK_EQ(q.depends_on.value_or(-1), made.at(q.dst_parent));
        if (q.depends_on) PB_CHECK(*q.depends_on < q.id);
    }
    // a second, identical confirmation is the same pick again, queued as new items (the user asked twice)
    PB_CHECK_EQ(e.store->queue_count_open(), 6);
}

PB_TEST(an_existing_server_folder_in_the_stored_listing_is_used_and_not_created_again) {
    Env e;
    e.file("Notes/trip/plan.md", "plan");
    e.file("Notes/trip/photos/a.jpg", "aa");
    pbc::Listing l;
    pbc::RemoteEntry trip;
    trip.name = "trip";
    trip.path = "/Docs/trip";
    trip.kind = pbc::Kind::Folder;
    l.entries.push_back(trip);
    PB_CHECK(e.store->replace_listing("/Docs", l, 1000).ok());
    auto act = e.actions();
    const auto r = act.confirm_folder_pick(act.count_folder_pick("/Docs", {e.root + "/Notes/trip"}));
    PB_CHECK(r.error.ok());
    int mkdirs_made = 0;
    for (const auto& q : all_items(*e.store)) {
        if (q.kind == pbc::ItemKind::Mkdir) {
            ++mkdirs_made;
            PB_CHECK_EQ(q.dst_name, std::string("photos"));   // only what is new
            PB_CHECK(!q.depends_on.has_value());               // its parent exists already
        } else if (q.dst_name == "plan.md") {
            PB_CHECK(!q.depends_on.has_value());               // into the existing folder
            PB_CHECK_EQ(q.dst_parent, std::string("/Docs/trip"));
        }
    }
    PB_CHECK_EQ(mkdirs_made, 1);
}

PB_TEST(unreadable_linked_hidden_and_badly_named_entries_of_a_folder_pick_are_skipped_and_reported) {
    Env e;
    e.file("Notes/mix/ok.txt", "ok");
    e.file("Notes/mix/.hidden.txt", "h");
    e.file("Notes/mix/q?.txt", "bad name");
    e.file("Notes/mix/bad?dir/in.txt", "inside a badly named folder");
    PB_CHECK(::symlink((e.root + "/system/pbcloud").c_str(), (e.root + "/Notes/mix/link").c_str()) == 0);
    pbc::ServerCaps c;
    c.forbidden_chars = "?";
    PB_CHECK(e.store->save_server_caps(c).ok());
    auto act = e.actions();
    const auto pick = act.count_folder_pick("/Docs", {e.root + "/Notes/mix"});
    PB_CHECK(pick.error.ok());
    PB_CHECK(pick.skipped >= 2);                                 // hidden file and link (the walk's count)
    const auto r = act.confirm_folder_pick(pick);
    PB_CHECK(r.error.ok());
    std::vector<std::string> names;
    for (const auto& q : all_items(*e.store)) names.push_back(q.dst_name);
    std::sort(names.begin(), names.end());
    PB_CHECK((names == std::vector<std::string>{"mix", "ok.txt"}));   // the badly named file and folder (with its content) were left out
    int forbidden = 0;
    for (const auto& x : r.refused)
        if (x.key == "name_forbidden") ++forbidden;
    PB_CHECK_EQ(forbidden, 2);
}

PB_TEST(a_pick_over_the_walk_limit_or_the_queue_headroom_is_refused_as_a_whole) {
    Env e;
    for (int i = 0; i < 30; ++i) e.file("Notes/many/f" + std::to_string(100 + i) + ".txt");
    pbc::SourceRules small = e.rules;
    small.max_items = 10;
    pbc::QueueActions limited(*e.store, small);
    const auto over = limited.count_folder_pick("/Docs", {e.root + "/Notes/many"});
    PB_CHECK(over.over_limit);
    const auto refused = limited.confirm_folder_pick(over);
    PB_CHECK(!refused.error.ok());
    PB_CHECK_EQ(refused.error.key, std::string("queue_full"));
    PB_CHECK_EQ(e.store->queue_count_open(), 0);

    auto act = e.actions();
    for (int i = 0; i < pbc::kMaxQueueItems - 5; ++i) {
        pbc::QueueItem q;
        q.kind = pbc::ItemKind::Upload;
        q.server = kServer;
        q.user_id = "demo";
        q.src_local = "/mnt/ext1/x" + std::to_string(i);
        q.dst_parent = "/Docs";
        q.dst_name = "n" + std::to_string(i);
        PB_CHECK(e.store->queue_add(q).ok());
    }
    const auto pick = act.count_folder_pick("/Docs", {e.root + "/Notes/many"});
    PB_CHECK(!pick.over_limit);
    PB_CHECK(pick.items() > act.headroom());
    const auto r = act.confirm_folder_pick(pick);
    PB_CHECK_EQ(r.error.key, std::string("queue_full"));
    PB_CHECK_EQ(e.store->queue_count_open(), pbc::kMaxQueueItems - 5);   // nothing was queued partially
}

PB_TEST(a_picked_path_that_is_not_a_folder_or_not_allowed_is_reported_and_does_not_stop_the_other_picks) {
    Env e;
    e.file("Notes/ok/a.txt");
    const std::string plain = e.file("Downloads/plain.txt");
    auto act = e.actions();
    const auto pick = act.count_folder_pick("/Docs", {e.root + "/Notes/ok", plain, e.root + "/system/pbcloud", e.root + "/Notes/missing"});
    PB_CHECK(pick.error.ok());
    PB_CHECK_EQ(pick.folders_count, 1);
    PB_CHECK_EQ(pick.files, 1);
    PB_CHECK_EQ(pick.refused.size(), size_t(3));
}

PB_TEST(removing_a_queued_item_leaves_the_source_alone) {
    Env e;
    const std::string a = e.file("Downloads/a.pdf", "aaaa");
    auto act = e.actions();
    const auto r = act.add_uploads("/Docs", {a});
    PB_CHECK(act.remove_item(r.queued[0]).ok());
    PB_CHECK_EQ(e.store->queue_count_open(), 0);
    PB_CHECK_EQ(e.dir.read("root/Downloads/a.pdf"), std::string("aaaa"));
    PB_CHECK(act.remove_item(r.queued[0]).ok());   // idempotent
}

PB_TEST_MAIN()
