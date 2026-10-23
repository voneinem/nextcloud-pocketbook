// The scan of a pair's reader folder (specs/003-folder-sync, research R-4): real temporary directories, links never followed, hidden names left out,
// the same bounds as the server side, nothing changed on the card.
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <string>
#include <vector>

#include "pbc/source_walk.h"
#include "pbc/sync_scan.h"
#include "pbc/sync_types.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

struct NoCancel : pbc::Cancel {
    bool requested() const override { return false; }
};
struct FlagCancel : pbc::Cancel {
    bool flag = false;
    bool requested() const override { return flag; }
};

struct Env {
    pbtest::TmpDir tmp;
    std::string root, reader;
    pbc::SourceRules rules;
    Env() {
        root = tmp.file("ext1");
        reader = root + "/Books";
        mkdirs(reader);
        rules.root = root;
    }
    static void mkdirs(const std::string& path) {
        for (size_t i = 1; i <= path.size(); ++i)
            if (i == path.size() || path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0755);
    }
    void file(const std::string& rel, const std::string& data = "x") {
        const std::string p = reader + "/" + rel;
        mkdirs(p.substr(0, p.rfind('/')));
        std::ofstream(p, std::ios::binary) << data;
    }
    pbc::SyncPair pair() const {
        pbc::SyncPair p;
        p.local_folder = reader;
        p.remote_folder = "/Books";
        return p;
    }
    pbc::Scan scan() {
        NoCancel c;
        return pbc::scan_reader(pair(), rules, c);
    }
};

const pbc::ScanEntry* find(const pbc::Scan& s, const std::string& rel) {
    for (const auto& e : s.entries)
        if (e.rel == rel) return &e;
    return nullptr;
}
std::vector<std::string> rels(const pbc::Scan& s) {
    std::vector<std::string> out;
    for (const auto& e : s.entries) out.push_back(e.rel);
    return out;
}
// every path below `dir` with size and mtime, to prove that a scan changes nothing
void walk(const std::string& dir, const std::string& rel, std::vector<std::string>* out) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        struct stat st;
        const std::string p = dir + "/" + n;
        if (::lstat(p.c_str(), &st) != 0) continue;
        out->push_back(rel + "/" + n + ":" + std::to_string(st.st_size) + ":" + std::to_string(st.st_mtime) + ":" + std::to_string(st.st_mode));
        if (S_ISDIR(st.st_mode)) walk(p, rel + "/" + n, out);
    }
    ::closedir(d);
}

}  // namespace

PB_TEST(the_folder_is_walked_and_the_entries_come_back_sorted_with_size_and_time) {
    Env e;
    e.file("b.epub", "bb");
    e.file("a.epub", "aaa");
    e.file("Novels/Dracula.txt", "dracula");
    e.file("Novels/Deep/x.txt", "x");
    const pbc::Scan s = e.scan();
    PB_CHECK(!s.incomplete);
    PB_CHECK_EQ(s.stopped_by_key, std::string(""));
    PB_CHECK((rels(s) == std::vector<std::string>{"Novels", "Novels/Deep", "Novels/Deep/x.txt", "Novels/Dracula.txt", "a.epub", "b.epub"}));
    PB_CHECK_EQ(find(s, "a.epub")->size, int64_t(3));
    PB_CHECK(find(s, "a.epub")->mtime.has_value());
    PB_CHECK(find(s, "a.epub")->kind == pbc::Kind::File);
    PB_CHECK(find(s, "Novels")->kind == pbc::Kind::Folder);
    PB_CHECK(find(s, "a.epub")->skip_reason.empty());
}

PB_TEST(hidden_names_links_special_files_and_unreadable_entries_are_not_followed_and_counted) {
    Env e;
    e.file("a.epub");
    e.file(".hidden", "h");
    e.file(".pbc-12.part", "partial");
    e.file("Novels/.pbc-7.part", "partial");
    e.file("Novels/b.epub");
    PB_CHECK(::symlink("/etc/passwd", (e.reader + "/etc-link").c_str()) == 0);
    PB_CHECK(::symlink((e.reader + "/a.epub").c_str(), (e.reader + "/file-link.epub").c_str()) == 0);
    PB_CHECK(::symlink("/tmp", (e.reader + "/folder-link").c_str()) == 0);
    PB_CHECK(::mkfifo((e.reader + "/pipe").c_str(), 0600) == 0);
    e.file("locked.epub");
    PB_CHECK(::chmod((e.reader + "/locked.epub").c_str(), 0000) == 0);
    const pbc::Scan s = e.scan();
    PB_CHECK(find(s, ".hidden") == nullptr && find(s, ".pbc-12.part") == nullptr && find(s, "Novels/.pbc-7.part") == nullptr);
    PB_CHECK_EQ(s.skipped_hidden, 3);
    for (const char* n : {"etc-link", "file-link.epub", "folder-link", "pipe"}) {
        PB_CHECK(find(s, n) != nullptr);
        PB_CHECK_EQ(find(s, n)->skip_reason, std::string("not_regular"));
    }
    PB_CHECK(find(s, "folder-link/passwd") == nullptr);   // never entered
    PB_CHECK(find(s, "Novels/b.epub") != nullptr);
    if (::geteuid() != 0) {   // root can read anything
        PB_CHECK_EQ(find(s, "locked.epub")->skip_reason, std::string("source_unreadable"));
    }
    PB_CHECK(::chmod((e.reader + "/locked.epub").c_str(), 0600) == 0);
}

PB_TEST(an_unreadable_folder_is_listed_with_its_reason_and_makes_the_scan_incomplete) {
    if (::geteuid() == 0) PB_SKIP("root can read every folder");
    Env e;
    e.file("a.epub");
    e.file("Closed/inside.txt");
    PB_CHECK(::chmod((e.reader + "/Closed").c_str(), 0000) == 0);
    const pbc::Scan s = e.scan();
    PB_CHECK_EQ(find(s, "Closed")->skip_reason, std::string("source_unreadable"));
    PB_CHECK(find(s, "Closed/inside.txt") == nullptr);
    PB_CHECK(s.incomplete);
    PB_CHECK_EQ(s.folders_not_listed, 1);
    PB_CHECK(::chmod((e.reader + "/Closed").c_str(), 0700) == 0);
}

PB_TEST(a_missing_or_unusable_pair_folder_ends_the_scan_with_reader_folder_missing) {
    Env e;
    pbc::SyncPair p = e.pair();
    NoCancel c;
    p.local_folder = e.root + "/Nowhere";
    pbc::Scan s = pbc::scan_reader(p, e.rules, c);
    PB_CHECK_EQ(s.stopped_by_key, std::string("reader_folder_missing"));
    PB_CHECK(s.entries.empty());
    PB_CHECK(s.incomplete);
    e.file("plain.txt");
    p.local_folder = e.reader + "/plain.txt";   // a file is no folder
    PB_CHECK_EQ(pbc::scan_reader(p, e.rules, c).stopped_by_key, std::string("reader_folder_missing"));
    PB_CHECK(::symlink(e.reader.c_str(), (e.root + "/Link").c_str()) == 0);
    p.local_folder = e.root + "/Link";            // the pair folder itself became a link
    PB_CHECK_EQ(pbc::scan_reader(p, e.rules, c).stopped_by_key, std::string("reader_folder_missing"));
    p.local_folder = "";
    PB_CHECK_EQ(pbc::scan_reader(p, e.rules, c).stopped_by_key, std::string("reader_folder_missing"));
    p.local_folder = "relative";
    PB_CHECK_EQ(pbc::scan_reader(p, e.rules, c).stopped_by_key, std::string("reader_folder_missing"));
}

PB_TEST(the_bounds_are_the_same_as_on_the_server_side) {
    {   // 500 folders
        Env e;
        for (int i = 0; i < 520; ++i) e.file("d" + std::to_string(1000 + i) + "/f.txt");
        const pbc::Scan s = e.scan();
        PB_CHECK(s.incomplete);
        PB_CHECK(s.folders_not_listed >= 20);
    }
    {   // depth 20
        Env e;
        std::string rel;
        for (int i = 0; i < 23; ++i) rel += "l" + std::to_string(i) + "/";
        e.file(rel + "deep.txt");
        const pbc::Scan s = e.scan();
        PB_CHECK(s.incomplete);
        PB_CHECK(s.folders_not_listed >= 1);
    }
    {   // 5,000 files
        Env e;
        for (int d = 0; d < 51; ++d)
            for (int f = 0; f < 100; ++f) e.file("d" + std::to_string(100 + d) + "/f" + std::to_string(1000 + f));
        const pbc::Scan s = e.scan();
        PB_CHECK(s.incomplete);
        size_t files = 0;
        for (const auto& en : s.entries)
            if (en.kind == pbc::Kind::File) ++files;
        PB_CHECK_EQ(files, size_t(pbc::kSyncMaxFiles));
    }
}

PB_TEST(two_names_that_differ_only_by_case_are_reported_as_a_collision_for_the_second) {
    Env e;
    e.file("Book.epub", "1");
    e.file("book.epub", "2");   // possible on this file system, not on the reader's
    e.file("Sub/A.txt", "3");
    e.file("Sub/a.txt", "4");
    const pbc::Scan s = e.scan();
    PB_CHECK(find(s, "Book.epub")->skip_reason.empty());               // "B" sorts before "b": the first keeps its place
    PB_CHECK_EQ(find(s, "book.epub")->skip_reason, std::string("name_collision"));
    PB_CHECK(find(s, "Sub/A.txt")->skip_reason.empty());
    PB_CHECK_EQ(find(s, "Sub/a.txt")->skip_reason, std::string("name_collision"));
}

PB_TEST(a_scan_changes_nothing_on_the_card_and_honors_cancel) {
    Env e;
    e.file("a.epub", "a");
    e.file("Novels/b.epub", "b");
    PB_CHECK(::symlink("/etc", (e.reader + "/link").c_str()) == 0);
    std::vector<std::string> before, after;
    walk(e.reader, "", &before);
    e.scan();
    e.scan();
    walk(e.reader, "", &after);
    PB_CHECK(before == after);
    FlagCancel cancel;
    cancel.flag = true;
    const pbc::Scan s = pbc::scan_reader(e.pair(), e.rules, cancel);
    PB_CHECK_EQ(s.stopped_by_key, std::string("cancelled"));
    PB_CHECK(s.incomplete);
}

PB_TEST(a_reader_folder_of_a_hostile_name_on_disk_is_listed_like_any_other_name) {
    Env e;
    e.file("caf\xC3\xA9.epub");
    e.file("sp ace/ctrl\x01.txt");   // a control character in a name on the card
    const pbc::Scan s = e.scan();
    PB_CHECK(find(s, "caf\xC3\xA9.epub") != nullptr);
    PB_CHECK(find(s, "sp ace/ctrl\x01.txt") != nullptr);   // the planner decides what can be sent; the scan reports what is there
}

PB_TEST_MAIN()
