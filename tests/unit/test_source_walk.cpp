// The picker's view of the reader's storage (specs/002-manage-server-files, research R-13, constitution Principle IV): which paths may be the
// source of an upload, and the bounded walk of a picked folder. Real temporary directories; links, hidden entries and special files are
// skipped, the app's own data folder can never be a source.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <set>
#include <string>

#include "pbc/source_walk.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

void touch(const std::string& path, const std::string& data = "x") {
    int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    PB_CHECK(fd >= 0);
    if (!data.empty()) PB_CHECK(::write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size()));
    ::close(fd);
}

void mkdirs(const std::string& path) {
    std::string cur;
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            cur = path.substr(0, i);
            ::mkdir(cur.c_str(), 0700);
        }
    }
}

struct Tree {
    pbtest::TmpDir dir;
    std::string root, outside, data_dir;
    pbc::SourceRules rules;
    Tree() {
        root = dir.file("root");
        outside = dir.file("outside");
        data_dir = root + "/system/pbcloud";
        mkdirs(root + "/Downloads");
        mkdirs(data_dir);
        mkdirs(outside);
        touch(root + "/Downloads/book.epub", "book");
        touch(data_dir + "/credentials", "app-secret");
        touch(data_dir + "/pbcloud.db", "db");
        touch(outside + "/secret.txt", "secret");
        rules.root = root;
        rules.deny = {data_dir};
    }
};

bool contains(const pbc::SourceWalk& w, const std::string& rel) {
    for (const auto& e : w.entries)
        if (e.rel == rel) return true;
    return false;
}

const pbc::SourceEntry* find(const pbc::SourceWalk& w, const std::string& rel) {
    for (const auto& e : w.entries)
        if (e.rel == rel) return &e;
    return nullptr;
}

}  // namespace

PB_TEST(a_file_below_the_root_is_an_allowed_source_and_the_canonical_path_comes_back) {
    Tree t;
    const std::string path = t.root + "/Downloads/book.epub";
    const auto r = pbc::check_source_path(path, t.rules);
    PB_CHECK(r.ok());
    char* real = ::realpath(path.c_str(), nullptr);
    PB_CHECK(real != nullptr);
    PB_CHECK_EQ(r.value, std::string(real));
    std::free(real);
}

PB_TEST(paths_outside_the_root_relative_dotted_and_the_root_itself_are_not_allowed) {
    Tree t;
    PB_CHECK_EQ(pbc::check_source_path(t.outside + "/secret.txt", t.rules).error.key, std::string("source_not_allowed"));
    PB_CHECK_EQ(pbc::check_source_path("Downloads/book.epub", t.rules).error.key, std::string("source_not_allowed"));
    PB_CHECK_EQ(pbc::check_source_path(t.root + "/Downloads/../Downloads/book.epub", t.rules).error.key, std::string("source_not_allowed"));
    PB_CHECK_EQ(pbc::check_source_path(t.root + "/Downloads/./book.epub", t.rules).error.key, std::string("source_not_allowed"));
    PB_CHECK_EQ(pbc::check_source_path(t.root + "/../outside/secret.txt", t.rules).error.key, std::string("source_not_allowed"));
    PB_CHECK_EQ(pbc::check_source_path(t.root, t.rules).error.key, std::string("source_not_allowed"));
    PB_CHECK_EQ(pbc::check_source_path("", t.rules).error.key, std::string("source_not_allowed"));
    PB_CHECK_EQ(pbc::check_source_path(t.root + "x/Downloads/book.epub", t.rules).error.key, std::string("source_not_allowed"));   // a sibling that shares the prefix
}

PB_TEST(the_apps_data_folder_with_the_database_and_the_credentials_can_never_be_a_source) {
    Tree t;
    for (const char* f : {"/credentials", "/pbcloud.db"}) PB_CHECK_EQ(pbc::check_source_path(t.data_dir + f, t.rules).error.key, std::string("source_not_allowed"));
    PB_CHECK_EQ(pbc::check_source_path(t.data_dir, t.rules).error.key, std::string("source_not_allowed"));
    PB_CHECK(pbc::check_source_path(t.root + "/Downloads/book.epub", t.rules).ok());
}

PB_TEST(a_symbolic_link_anywhere_on_the_path_is_not_followed) {
    Tree t;
    // a link to the credentials inside an allowed folder
    PB_CHECK(::symlink((t.data_dir + "/credentials").c_str(), (t.root + "/Downloads/innocent.txt").c_str()) == 0);
    PB_CHECK_EQ(pbc::check_source_path(t.root + "/Downloads/innocent.txt", t.rules).error.key, std::string("source_not_allowed"));
    // a link to a file outside
    PB_CHECK(::symlink((t.outside + "/secret.txt").c_str(), (t.root + "/Downloads/out.txt").c_str()) == 0);
    PB_CHECK_EQ(pbc::check_source_path(t.root + "/Downloads/out.txt", t.rules).error.key, std::string("source_not_allowed"));
    // a link to a folder, and a file reached through it
    PB_CHECK(::symlink(t.data_dir.c_str(), (t.root + "/Downloads/dirlink").c_str()) == 0);
    PB_CHECK_EQ(pbc::check_source_path(t.root + "/Downloads/dirlink", t.rules).error.key, std::string("source_not_allowed"));
    PB_CHECK_EQ(pbc::check_source_path(t.root + "/Downloads/dirlink/credentials", t.rules).error.key, std::string("source_not_allowed"));
}

PB_TEST(a_path_that_does_not_exist_is_unreadable_not_forbidden) {
    Tree t;
    PB_CHECK_EQ(pbc::check_source_path(t.root + "/Downloads/missing.epub", t.rules).error.key, std::string("source_unreadable"));
}

PB_TEST(the_walk_lists_the_folder_first_then_every_folder_before_what_it_contains) {
    Tree t;
    const std::string pick = t.root + "/Notes/trip";
    mkdirs(pick + "/photos/raw");
    touch(pick + "/plan.md", "plan");
    touch(pick + "/photos/a.jpg", "aa");
    touch(pick + "/photos/raw/b.nef", "bbbb");
    const auto w = pbc::walk_source(pick, t.rules);
    PB_CHECK(!w.over_limit);
    PB_CHECK_EQ(w.folders, 3);
    PB_CHECK_EQ(w.files, 3);
    PB_CHECK_EQ(w.bytes, 4 + 2 + 4);
    PB_CHECK_EQ(w.skipped, 0);
    PB_CHECK_EQ(w.entries.size(), size_t(6));
    PB_CHECK(w.entries[0].kind == pbc::SourceKind::Folder);
    PB_CHECK_EQ(w.entries[0].rel, std::string());
    PB_CHECK_EQ(w.entries[0].name, std::string("trip"));
    std::set<std::string> seen = {""};
    for (size_t i = 1; i < w.entries.size(); ++i) {
        const std::string& rel = w.entries[i].rel;
        const size_t slash = rel.rfind('/');
        const std::string parent = slash == std::string::npos ? std::string() : rel.substr(0, slash);
        PB_CHECK(seen.count(parent) == 1);   // the folder of an entry has been listed before it
        seen.insert(rel);
    }
    PB_CHECK(contains(w, "photos/raw/b.nef"));
    PB_CHECK_EQ(find(w, "plan.md")->size, 4LL);
    PB_CHECK_EQ(find(w, "plan.md")->name, std::string("plan.md"));
}

PB_TEST(the_walk_skips_hidden_entries_links_and_special_files_and_counts_them) {
    Tree t;
    const std::string pick = t.root + "/Notes/mixed";
    mkdirs(pick + "/sub");
    mkdirs(pick + "/.hidden_dir");
    touch(pick + "/ok.txt", "ok");
    touch(pick + "/.hidden.txt", "h");
    touch(pick + "/.hidden_dir/in.txt", "i");
    touch(pick + "/sub/inner.txt", "in");
    PB_CHECK(::symlink((t.outside + "/secret.txt").c_str(), (pick + "/link.txt").c_str()) == 0);
    PB_CHECK(::symlink(t.outside.c_str(), (pick + "/linked_dir").c_str()) == 0);
    PB_CHECK(::mkfifo((pick + "/pipe").c_str(), 0600) == 0);
    const auto w = pbc::walk_source(pick, t.rules);
    PB_CHECK(contains(w, "ok.txt"));
    PB_CHECK(contains(w, "sub/inner.txt"));
    for (const char* hidden : {".hidden.txt", ".hidden_dir", ".hidden_dir/in.txt", "link.txt", "linked_dir", "linked_dir/secret.txt", "pipe"}) PB_CHECK(!contains(w, hidden));
    PB_CHECK_EQ(w.skipped, 5);   // .hidden.txt, .hidden_dir, link.txt, linked_dir, pipe
    PB_CHECK_EQ(w.files, 2);
    PB_CHECK_EQ(w.folders, 2);   // the picked folder and sub
}

PB_TEST(an_unreadable_file_is_skipped_and_the_rest_goes_on) {
    Tree t;
    const std::string pick = t.root + "/Notes/perm";
    mkdirs(pick);
    touch(pick + "/open.txt", "ok");
    touch(pick + "/closed.txt", "no");
    ::chmod((pick + "/closed.txt").c_str(), 0);
    const bool readable_anyway = ::access((pick + "/closed.txt").c_str(), R_OK) == 0;   // running as root
    const auto w = pbc::walk_source(pick, t.rules);
    ::chmod((pick + "/closed.txt").c_str(), 0600);
    PB_CHECK(contains(w, "open.txt"));
    if (!readable_anyway) {
        PB_CHECK(!contains(w, "closed.txt"));
        PB_CHECK_EQ(w.skipped, 1);
    }
}

PB_TEST(a_denied_folder_inside_a_pick_is_left_out) {
    Tree t;
    const std::string pick = t.root + "/Notes/outer";
    mkdirs(pick + "/inner");
    touch(pick + "/inner/a.txt", "a");
    touch(pick + "/b.txt", "b");
    pbc::SourceRules rules = t.rules;
    rules.deny.push_back(pick + "/inner");
    const auto w = pbc::walk_source(pick, rules);
    PB_CHECK(contains(w, "b.txt"));
    PB_CHECK(!contains(w, "inner"));
    PB_CHECK(!contains(w, "inner/a.txt"));
    PB_CHECK_EQ(w.skipped, 1);
}

PB_TEST(the_walk_stops_at_the_item_limit_and_at_the_depth_limit) {
    Tree t;
    const std::string many = t.root + "/Notes/many";
    mkdirs(many);
    for (int i = 0; i < 20; ++i) touch(many + "/f" + std::to_string(100 + i) + ".txt", "x");
    pbc::SourceRules limited = t.rules;
    limited.max_items = 10;
    const auto w = pbc::walk_source(many, limited);
    PB_CHECK(w.over_limit);
    PB_CHECK(w.entries.size() <= 11);   // it stops, it does not list everything first
    PB_CHECK(pbc::walk_source(many, t.rules).over_limit == false);
    PB_CHECK_EQ(pbc::walk_source(many, t.rules).files, 20);

    std::string deep = t.root + "/Notes/deep";
    for (int i = 0; i < 14; ++i) deep += "/d";
    mkdirs(deep);
    touch(deep + "/leaf.txt", "x");
    const auto d = pbc::walk_source(t.root + "/Notes/deep", t.rules);   // default depth 12
    PB_CHECK(d.over_limit);
}

PB_TEST(a_file_pick_is_not_a_walk) {
    Tree t;
    const auto w = pbc::walk_source(t.root + "/Downloads/book.epub", t.rules);   // not a folder
    PB_CHECK(w.entries.empty());
    PB_CHECK_EQ(w.files, 0);
    PB_CHECK_EQ(w.folders, 0);
}

PB_TEST(the_picker_lists_one_folder_folders_first_in_case_insensitive_order_without_hidden_links_or_special_files) {
    Tree t;
    const std::string dir = t.root + "/Notes/list";
    mkdirs(dir + "/beta");
    mkdirs(dir + "/Alpha");
    mkdirs(dir + "/.hidden");
    touch(dir + "/zeta.txt", "zz");
    touch(dir + "/Eta.txt", "e");
    touch(dir + "/.dot.txt", "d");
    PB_CHECK(::symlink((t.outside + "/secret.txt").c_str(), (dir + "/link.txt").c_str()) == 0);
    PB_CHECK(::symlink(t.outside.c_str(), (dir + "/linkdir").c_str()) == 0);
    PB_CHECK(::mkfifo((dir + "/pipe").c_str(), 0600) == 0);
    const auto l = pbc::list_source_dir(dir, t.rules, {});
    PB_CHECK(!l.unreadable);
    std::vector<std::string> names;
    for (const auto& e : l.entries) names.push_back(e.name);
    PB_CHECK((names == std::vector<std::string>{"Alpha", "beta", "Eta.txt", "zeta.txt"}));
    PB_CHECK(l.entries[0].kind == pbc::SourceKind::Folder);
    PB_CHECK(l.entries[2].kind == pbc::SourceKind::File);
    PB_CHECK_EQ(l.entries[3].size, 2LL);
}

PB_TEST(at_the_root_the_picker_lists_only_the_existing_top_folders_whatever_their_case) {
    Tree t;
    mkdirs(t.root + "/Audio Books");
    mkdirs(t.root + "/photo");
    mkdirs(t.root + "/PBCloud");
    mkdirs(t.root + "/Other");
    touch(t.root + "/Autorun.inf", "x");
    touch(t.root + "/Music", "a file named like a top folder");   // not a folder: not listed
    const std::vector<std::string> top = {"Audio Books", "Downloads", "Music", "My pictures", "Notes", "Photo", "screens", "PBCloud"};
    const auto l = pbc::list_source_dir(t.root, t.rules, top);
    std::vector<std::string> names;
    for (const auto& e : l.entries) names.push_back(e.name);
    PB_CHECK((names == std::vector<std::string>{"Audio Books", "Downloads", "PBCloud", "photo"}));   // Downloads exists from the fixture
    for (const auto& e : l.entries) PB_CHECK(e.kind == pbc::SourceKind::Folder);
    // an empty list of top folders means the root is listed unfiltered (below it nothing is filtered anyway)
    PB_CHECK(pbc::list_source_dir(t.root, t.rules, {}).entries.size() > l.entries.size());
}

PB_TEST(a_folder_that_is_missing_unreadable_denied_or_a_link_lists_as_unreadable_and_empty) {
    Tree t;
    PB_CHECK(pbc::list_source_dir(t.root + "/missing", t.rules, {}).unreadable);
    PB_CHECK(pbc::list_source_dir(t.root + "/Downloads/book.epub", t.rules, {}).unreadable);   // a file
    PB_CHECK(pbc::list_source_dir(t.outside, t.rules, {}).unreadable);                         // outside the root
    PB_CHECK(pbc::list_source_dir(t.data_dir, t.rules, {}).unreadable);                        // the app's own folder
    mkdirs(t.root + "/Notes");
    PB_CHECK(::symlink(t.outside.c_str(), (t.root + "/Notes/out").c_str()) == 0);
    PB_CHECK(pbc::list_source_dir(t.root + "/Notes/out", t.rules, {}).unreadable);
    PB_CHECK(pbc::list_source_dir("", t.rules, {}).unreadable);
    PB_CHECK(pbc::list_source_dir(t.root + "/Notes", t.rules, {}).entries.empty());
}

PB_TEST_MAIN()
