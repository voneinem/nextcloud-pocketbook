// The 001 view of a file a folder pair keeps up to date (specs/003-folder-sync, research R-15): Store::synced_copy, the way the card is read for it
// (resolve_local_file with the pair's folder as root) and the refusal to remove it like a downloaded copy.
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <memory>
#include <string>

#include "pbc/entry_state.h"
#include "pbc/local_files.h"
#include "pbc/store.h"
#include "pbc/sync_rules.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

pbc::SystemClock g_clock;

struct Env {
    pbtest::TmpDir dir;
    std::unique_ptr<pbc::Store> store;
    std::string reader;
    int64_t pair = 0;
    Env() {
        auto r = pbc::Store::open(dir.file("data"), g_clock);
        if (!r.ok()) PB_FAIL_AT("cannot open the store: " + r.error.key);
        store = std::move(r.value);
        pbc::AccountRow a;
        a.base_url = "https://cloud.example.org";
        a.login_name = "demo";
        a.user_id = "demo";
        PB_CHECK(store->save_account(a).ok());
        reader = dir.file("Reader");
        ::mkdir(reader.c_str(), 0700);
        pair = add_pair("/Books", reader);
    }
    int64_t add_pair(const std::string& remote, const std::string& local) {
        pbc::SyncPair p = pbc::default_pair("https://cloud.example.org", "demo");
        p.remote_folder = remote;
        p.local_folder = local;
        auto r = store->sync_pair_add(p);
        if (!r.ok()) PB_FAIL_AT("cannot add the pair: " + r.error.key);
        return r.value;
    }
    void record(int64_t pair_id, const std::string& rel, const std::string& local_rel, const char* origin = "download", const char* one_sided = "") {
        pbc::SyncRecord r;
        r.pair_id = pair_id;
        r.rel = rel;
        r.local_rel = local_rel;
        r.origin = origin;
        r.one_sided = one_sided;
        r.server_etag = "\"e1\"";
        r.local_size = 5;
        r.local_mtime = 1700000000;
        r.synced_at = 1;
        PB_CHECK(store->sync_record_put(r).ok());
    }
    void write(const std::string& rel, const std::string& data) {
        std::string path = reader + "/" + rel;
        for (size_t i = reader.size() + 1; i < path.size(); ++i)
            if (path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0700);
        std::ofstream(path) << data;
    }
};

}  // namespace

PB_TEST(a_path_inside_a_pairs_server_folder_with_a_record_has_a_synced_copy) {
    Env e;
    e.record(e.pair, "Novels/Dracula.txt", "Novels/Dracula (2).txt");
    const auto c = e.store->synced_copy("/Books/Novels/Dracula.txt");
    PB_CHECK(c.has_value());
    PB_CHECK_EQ(c->root, e.reader);
    PB_CHECK_EQ(c->local_rel, std::string("Novels/Dracula (2).txt"));
    PB_CHECK_EQ(c->etag, std::string("\"e1\""));
    PB_CHECK_EQ(c->size, int64_t(5));
    PB_CHECK_EQ(c->mtime, int64_t(1700000000));
    PB_CHECK_EQ(c->pair_id, e.pair);
    // nothing for a name without a record, outside the folder, a longer name that merely starts alike, or the folder itself
    PB_CHECK(!e.store->synced_copy("/Books/Novels/Other.txt").has_value());
    PB_CHECK(!e.store->synced_copy("/Elsewhere/Novels/Dracula.txt").has_value());
    PB_CHECK(!e.store->synced_copy("/Books2/Novels/Dracula.txt").has_value());
    PB_CHECK(!e.store->synced_copy("/Books").has_value());
}

PB_TEST(a_copy_that_exists_only_on_the_reader_is_not_a_synced_copy_of_a_server_path) {
    Env e;
    e.record(e.pair, "a (reader copy).txt", "a (reader copy).txt", "conflict_copy", "reader");
    PB_CHECK(!e.store->synced_copy("/Books/a (reader copy).txt").has_value());
}

PB_TEST(the_pair_with_the_longest_matching_folder_wins_and_only_the_current_account_counts) {
    Env e;
    // two pairs cannot overlap, so the second one lives beside the first
    const std::string other = e.dir.file("Other");
    ::mkdir(other.c_str(), 0700);
    const int64_t second = e.add_pair("/Docs", other);
    e.record(second, "x.txt", "x.txt");
    e.record(e.pair, "x.txt", "x.txt");
    PB_CHECK_EQ(e.store->synced_copy("/Docs/x.txt")->root, other);
    PB_CHECK_EQ(e.store->synced_copy("/Books/x.txt")->root, e.reader);
    // another account: the pairs are marked and never used
    pbc::AccountRow b;
    b.base_url = "https://cloud.example.org";
    b.login_name = "someone";
    b.user_id = "someone";
    PB_CHECK(e.store->save_account(b).ok());
    PB_CHECK(!e.store->synced_copy("/Books/x.txt").has_value());
    PB_CHECK(!e.store->synced_copy("/Docs/x.txt").has_value());
    PB_CHECK(e.store->clear_account().ok());
    PB_CHECK(!e.store->synced_copy("/Books/x.txt").has_value());
}

PB_TEST(the_copy_is_read_from_the_pair_folder_and_never_through_a_link) {
    Env e;
    e.write("Novels/a.txt", "hello");
    e.record(e.pair, "Novels/a.txt", "Novels/a.txt");
    const auto c = e.store->synced_copy("/Books/Novels/a.txt");
    const auto ok = pbc::resolve_local_file(c->root, c->local_rel);
    PB_CHECK(ok.ok());
    PB_CHECK_EQ(ok.value, e.reader + "/Novels/a.txt");
    PB_CHECK_EQ(pbc::resolve_local_file(c->root, "Novels/missing.txt").error.key, std::string("not_found"));
    PB_CHECK_EQ(pbc::resolve_local_file(c->root, "../Reader/Novels/a.txt").error.key, std::string("path_unsafe"));
    PB_CHECK_EQ(pbc::resolve_local_file(c->root, "/etc/passwd").error.key, std::string("path_unsafe"));
    // a link in the way, and a link as the file itself
    e.write("real/b.txt", "b");
    PB_CHECK(::symlink((e.reader + "/real").c_str(), (e.reader + "/via").c_str()) == 0);
    PB_CHECK_EQ(pbc::resolve_local_file(c->root, "via/b.txt").error.key, std::string("path_unsafe"));
    PB_CHECK(::symlink((e.reader + "/real/b.txt").c_str(), (e.reader + "/file-link.txt").c_str()) == 0);
    PB_CHECK_EQ(pbc::resolve_local_file(c->root, "file-link.txt").error.key, std::string("path_unsafe"));
}

PB_TEST(the_state_of_a_synced_file_comes_from_the_same_rules_as_for_a_downloaded_one) {
    Env e;
    e.write("a.txt", "hello");
    struct stat st;
    PB_CHECK(::stat((e.reader + "/a.txt").c_str(), &st) == 0);
    pbc::SyncRecord r;
    r.pair_id = e.pair;
    r.rel = "a.txt";
    r.local_rel = "a.txt";
    r.origin = "download";
    r.server_etag = "\"e1\"";
    r.local_size = st.st_size;
    r.local_mtime = st.st_mtime;
    r.synced_at = 1;
    PB_CHECK(e.store->sync_record_put(r).ok());
    const auto c = e.store->synced_copy("/Books/a.txt");
    const pbc::LocalFile lf = pbc::as_local_file("/Books/a.txt", *c);
    pbc::RemoteEntry entry;
    entry.path = "/Books/a.txt";
    entry.etag = "\"e1\"";
    PB_CHECK_EQ(std::string(pbc::state_key(pbc::compute_state(&entry, &lf, pbc::stat_file(c->root + "/" + lf.local_path)))), std::string("downloaded"));
    entry.etag = "\"e2\"";
    PB_CHECK_EQ(std::string(pbc::state_key(pbc::compute_state(&entry, &lf, pbc::stat_file(c->root + "/" + lf.local_path)))), std::string("outdated"));
    e.write("a.txt", "hello, changed");
    PB_CHECK_EQ(std::string(pbc::state_key(pbc::compute_state(&entry, &lf, pbc::stat_file(c->root + "/" + lf.local_path)))), std::string("conflict"));
}

PB_TEST(a_synced_copy_is_not_removed_like_a_downloaded_copy) {
    Env e;
    e.write("a.txt", "hello");
    e.record(e.pair, "a.txt", "a.txt");
    const pbc::Error err = pbc::remove_local_copy(*e.store, e.dir.file("PBCloud"), "/Books/a.txt", true);
    PB_CHECK_EQ(err.key, std::string("managed_by_sync"));
    PB_CHECK_EQ(e.dir.read("Reader/a.txt"), std::string("hello"));   // the file is where it was
    PB_CHECK(e.store->sync_record(e.pair, "a.txt").has_value());
    // a path nobody keeps: still nothing to do
    PB_CHECK(pbc::remove_local_copy(*e.store, e.dir.file("PBCloud"), "/Elsewhere/b.txt", false).ok());
}

PB_TEST_MAIN()
