// The rules of a pair (specs/003-folder-sync/research.md R-2, contracts/core-interfaces.md "Rules of a pair"): which folders may be paired,
// when two pairs overlap, the limits, the directions and the free-space reserve. Pure functions plus real temporary directories.
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "pbc/source_walk.h"
#include "pbc/sync_rules.h"
#include "pbc/sync_types.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

// A storage root with the app's data folder, the download root and a few folders of the user.
struct Env {
    pbtest::TmpDir tmp;
    pbc::SourceRules rules;
    std::string root, data, download;
    Env() {
        root = tmp.file("ext1");
        data = root + "/system/pbcloud";
        download = root + "/PBCloud";
        for (const char* d : {"ext1", "ext1/system", "ext1/system/pbcloud", "ext1/PBCloud", "ext1/PBCloud/Books", "ext1/Books", "ext1/Books/Sub", "ext1/Outbox",
                              "ext1/Other", "ext1/books", "ext1/Deep", "ext1/Deep/Inner"})
            ::mkdir(tmp.file(d).c_str(), 0755);
        rules.root = root;
        rules.deny = {data};
    }
    pbc::SyncPair pair(const std::string& local, const std::string& remote = "/Books") const {
        pbc::SyncPair p = pbc::default_pair("https://cloud.example.org", "demo");
        p.local_folder = local;
        p.remote_folder = remote;
        return p;
    }
    pbc::PairCheck check(const pbc::SyncPair& p, const std::vector<pbc::SyncPair>& others = {}) const { return pbc::check_pair(p, others, rules, download); }
};

pbc::SyncPair existing(int64_t id, const std::string& local, const std::string& remote) {
    pbc::SyncPair p = pbc::default_pair("https://cloud.example.org", "demo");
    p.id = id;
    p.local_folder = local;
    p.remote_folder = remote;
    return p;
}

}  // namespace

PB_TEST(a_valid_pair_is_accepted_and_the_reader_folder_comes_back_canonical) {
    Env e;
    const auto c = e.check(e.pair(e.root + "/Books"));
    PB_CHECK(c.ok);
    PB_CHECK_EQ(c.error_key, std::string(""));
    PB_CHECK(!c.local_folder.empty());
    PB_CHECK(c.local_folder.size() >= std::string("/Books").size());
    PB_CHECK(c.local_folder.compare(c.local_folder.size() - 6, 6, "/Books") == 0);
    PB_CHECK(e.check(e.pair(e.root + "/Books/")).ok);   // a trailing slash is not a different folder
    PB_CHECK(e.check(e.pair(e.root + "/Outbox", "/Outbox")).ok);
}

PB_TEST(a_reader_folder_outside_the_picker_root_or_behind_a_link_is_not_allowed) {
    Env e;
    for (const char* bad : {"/etc", "/tmp", "relative/Books", "", "/"}) PB_CHECK_EQ(e.check(e.pair(bad)).error_key, std::string("source_not_allowed"));
    PB_CHECK_EQ(e.check(e.pair(e.root + "/Books/../Other")).error_key, std::string("source_not_allowed"));
    PB_CHECK_EQ(e.check(e.pair(e.root + "/./Books")).error_key, std::string("source_not_allowed"));
    PB_CHECK(::symlink("/etc", (e.root + "/Link").c_str()) == 0);
    PB_CHECK_EQ(e.check(e.pair(e.root + "/Link")).error_key, std::string("source_not_allowed"));
    PB_CHECK(::symlink((e.root + "/Books").c_str(), (e.root + "/BooksLink").c_str()) == 0);
    PB_CHECK_EQ(e.check(e.pair(e.root + "/BooksLink")).error_key, std::string("source_not_allowed"));
    PB_CHECK_EQ(e.check(e.pair(e.root + "/BooksLink/Sub")).error_key, std::string("source_not_allowed"));   // a link on the way
    // a file is no folder
    e.tmp.write("ext1/file.txt", "x");
    PB_CHECK_EQ(e.check(e.pair(e.root + "/file.txt")).error_key, std::string("source_not_allowed"));
    // a folder that is not there (yet)
    PB_CHECK_EQ(e.check(e.pair(e.root + "/Nowhere")).error_key, std::string("reader_folder_missing"));
}

PB_TEST(the_picker_root_the_data_folder_and_the_download_folder_are_forbidden) {
    Env e;
    PB_CHECK_EQ(e.check(e.pair(e.root)).error_key, std::string("pair_folder_forbidden"));            // the picker root itself
    PB_CHECK_EQ(e.check(e.pair(e.root + "/")).error_key, std::string("pair_folder_forbidden"));
    PB_CHECK_EQ(e.check(e.pair(e.data)).error_key, std::string("pair_folder_forbidden"));            // the app's data folder
    PB_CHECK_EQ(e.check(e.pair(e.root + "/system")).error_key, std::string("pair_folder_forbidden"));   // a folder that contains it
    ::mkdir((e.data + "/cache").c_str(), 0755);
    PB_CHECK_EQ(e.check(e.pair(e.data + "/cache")).error_key, std::string("pair_folder_forbidden"));   // a folder inside it
    PB_CHECK_EQ(e.check(e.pair(e.download)).error_key, std::string("pair_folder_forbidden"));        // the download root of feature 001
    PB_CHECK_EQ(e.check(e.pair(e.download + "/Books")).error_key, std::string("pair_folder_forbidden"));
    // a download root deeper down: its parent contains it
    Env d;
    ::mkdir((d.root + "/Media").c_str(), 0755);
    ::mkdir((d.root + "/Media/PBCloud").c_str(), 0755);
    PB_CHECK_EQ(pbc::check_pair(d.pair(d.root + "/Media"), {}, d.rules, d.root + "/Media/PBCloud").error_key, std::string("pair_folder_forbidden"));
    PB_CHECK(pbc::check_pair(d.pair(d.root + "/Books"), {}, d.rules, d.root + "/Media/PBCloud").ok);
}

PB_TEST(reader_folders_that_equal_contain_or_lie_inside_another_pair_overlap) {
    Env e;
    const std::vector<pbc::SyncPair> others = {existing(1, e.root + "/Books", "/Books")};
    PB_CHECK_EQ(e.check(e.pair(e.root + "/Books", "/Else"), others).error_key, std::string("pair_overlap"));         // equal
    PB_CHECK_EQ(e.check(e.pair(e.root + "/Books/Sub", "/Else"), others).error_key, std::string("pair_overlap"));     // inside
    PB_CHECK(e.check(e.pair(e.root + "/Other", "/Else"), others).ok);                                                // next to it
    PB_CHECK(e.check(e.pair(e.root + "/Books2x", "/Else"), others).error_key == "reader_folder_missing");            // another name, not a prefix match
    ::mkdir((e.root + "/Books2").c_str(), 0755);
    PB_CHECK(e.check(e.pair(e.root + "/Books2", "/Else"), others).ok);                                               // "Books2" is not inside "Books"
    // containing: the other pair lies below the candidate
    const std::vector<pbc::SyncPair> deep = {existing(1, e.root + "/Deep/Inner", "/Inner")};
    PB_CHECK_EQ(e.check(e.pair(e.root + "/Deep", "/Else"), deep).error_key, std::string("pair_overlap"));
}

PB_TEST(reader_folders_are_compared_without_regard_to_case_because_the_storage_ignores_it) {
    Env e;   // ext1/Books and ext1/books exist side by side on this file system; the reader's FAT would not allow that
    const std::vector<pbc::SyncPair> others = {existing(1, e.root + "/Books", "/Books")};
    PB_CHECK_EQ(e.check(e.pair(e.root + "/books", "/Else"), others).error_key, std::string("pair_overlap"));
}

PB_TEST(server_folders_are_compared_component_wise_and_byte_exact) {
    Env e;
    const std::vector<pbc::SyncPair> others = {existing(1, e.root + "/Books", "/Books")};
    PB_CHECK_EQ(e.check(e.pair(e.root + "/Other", "/Books"), others).error_key, std::string("pair_overlap"));         // equal
    PB_CHECK_EQ(e.check(e.pair(e.root + "/Other", "/Books/Sub"), others).error_key, std::string("pair_overlap"));     // inside
    PB_CHECK_EQ(e.check(e.pair(e.root + "/Other", "/"), others).error_key, std::string("pair_overlap"));              // the root contains every folder
    PB_CHECK(e.check(e.pair(e.root + "/Other", "/Books2"), others).ok);                                               // a longer name is no prefix
    PB_CHECK(e.check(e.pair(e.root + "/Other", "/books"), others).ok);                                                // byte-exact: the server tells them apart
    const std::vector<pbc::SyncPair> sub = {existing(1, e.root + "/Books", "/Books/Sub")};
    PB_CHECK_EQ(e.check(e.pair(e.root + "/Other", "/Books"), sub).error_key, std::string("pair_overlap"));            // containing
    // the server root as a pair of its own, when no other pair exists
    PB_CHECK(e.check(e.pair(e.root + "/Other", "/")).ok);
    const std::vector<pbc::SyncPair> all = {existing(1, e.root + "/Books", "/")};
    PB_CHECK_EQ(e.check(e.pair(e.root + "/Other", "/Anything"), all).error_key, std::string("pair_overlap"));
}

PB_TEST(a_pair_does_not_overlap_itself_when_it_is_changed) {
    Env e;
    pbc::SyncPair p = e.pair(e.root + "/Books", "/Books");
    p.id = 1;
    const std::vector<pbc::SyncPair> others = {existing(1, e.root + "/Books", "/Books"), existing(2, e.root + "/Other", "/Other")};
    PB_CHECK(e.check(p, others).ok);
    p.local_folder = e.root + "/Other";   // now it does overlap the other pair
    PB_CHECK_EQ(e.check(p, others).error_key, std::string("pair_overlap"));
}

PB_TEST(the_server_folder_must_be_a_remote_path) {
    Env e;
    for (const char* bad : {"", "Books", "/Books/", "/Books//Sub", "/a/../b", "/a/./b"}) PB_CHECK_EQ(e.check(e.pair(e.root + "/Books", bad)).error_key, std::string("protocol_error"));
    PB_CHECK(e.check(e.pair(e.root + "/Books", "/")).ok);
}

PB_TEST(limits_are_bounded_and_so_is_the_number_of_pairs) {
    Env e;
    pbc::SyncPair p = e.pair(e.root + "/Books");
    for (int64_t files : std::vector<int64_t>{0, -3, 5001}) {
        p.max_files = files;
        PB_CHECK_EQ(e.check(p).error_key, std::string("pair_limit_invalid"));
    }
    p.max_files = 1;
    PB_CHECK(e.check(p).ok);
    p.max_files = 5000;
    PB_CHECK(e.check(p).ok);
    p.max_files = 500;
    for (int64_t bytes : std::vector<int64_t>{0, (1LL << 20) - 1, (64LL << 30) + 1}) {
        p.max_bytes = bytes;
        PB_CHECK_EQ(e.check(p).error_key, std::string("pair_limit_invalid"));
    }
    p.max_bytes = 1LL << 20;
    PB_CHECK(e.check(p).ok);
    p.max_bytes = 64LL << 30;
    PB_CHECK(e.check(p).ok);

    // a ninth pair
    std::vector<pbc::SyncPair> eight;
    for (int i = 1; i <= pbc::kMaxSyncPairs; ++i) eight.push_back(existing(i, e.root + "/Elsewhere" + std::to_string(i), "/E" + std::to_string(i)));
    PB_CHECK_EQ(e.check(e.pair(e.root + "/Books", "/Nine"), eight).error_key, std::string("pair_limit"));
    pbc::SyncPair changed = eight[3];
    changed.local_folder = e.root + "/Other";
    changed.remote_folder = "/Changed";
    PB_CHECK(e.check(changed, eight).ok);   // changing one of the eight is not a ninth
}

PB_TEST(a_sending_direction_needs_the_confirmation_unless_it_was_sending_already) {
    using pbc::Direction;
    const Direction all[] = {Direction::Down, Direction::Up, Direction::Both};
    for (Direction from : all)
        for (Direction to : all) {
            const bool sends_now = to == Direction::Up || to == Direction::Both;
            const bool sent_before = from == Direction::Up || from == Direction::Both;
            PB_CHECK_EQ(pbc::needs_send_confirmation(from, to), sends_now && !sent_before);
        }
}

PB_TEST(the_directions_say_which_way_files_may_go) {
    using pbc::Direction;
    PB_CHECK(pbc::direction_allows_to_reader(Direction::Down));
    PB_CHECK(!pbc::direction_allows_to_server(Direction::Down));
    PB_CHECK(!pbc::direction_allows_to_reader(Direction::Up));
    PB_CHECK(pbc::direction_allows_to_server(Direction::Up));
    PB_CHECK(pbc::direction_allows_to_reader(Direction::Both));
    PB_CHECK(pbc::direction_allows_to_server(Direction::Both));
}

PB_TEST(a_new_pair_downloads_only_with_the_working_limits_and_both_switches_off) {
    const pbc::SyncPair p = pbc::default_pair("https://cloud.example.org", "demo");
    PB_CHECK(p.direction == pbc::Direction::Down);
    PB_CHECK_EQ(p.max_files, int64_t(500));
    PB_CHECK_EQ(p.max_bytes, int64_t(2LL << 30));
    PB_CHECK(!p.startup_sync);
    PB_CHECK(!p.auto_sync);
    PB_CHECK_EQ(p.server, std::string("https://cloud.example.org"));
    PB_CHECK_EQ(p.user_id, std::string("demo"));
    PB_CHECK_EQ(p.state, std::string("idle"));
}

PB_TEST(the_reserve_is_the_larger_of_256_mib_and_two_percent_of_the_volume) {
    PB_CHECK_EQ(pbc::reserve_for(0), int64_t(256) << 20);
    PB_CHECK_EQ(pbc::reserve_for(1LL << 30), int64_t(256) << 20);          // 2 % of 1 GiB is about 20 MiB
    PB_CHECK_EQ(pbc::reserve_for(12LL << 30), int64_t(256) << 20);         // 2 % of 12 GiB is about 245 MiB, still below the minimum
    PB_CHECK_EQ(pbc::reserve_for(32LL << 30), (32LL << 30) / 100 * 2);     // 2 % of 32 GiB
    PB_CHECK(pbc::reserve_for(32LL << 30) > (int64_t(256) << 20));
    PB_CHECK_EQ(pbc::reserve_for(-5), int64_t(256) << 20);                 // an unknown volume
}

PB_TEST_MAIN()
