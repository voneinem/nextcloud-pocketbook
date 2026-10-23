// The one place that deletes a user's file for a folder pair (specs/003-folder-sync R-10, rule N5): only an unchanged regular file inside the pair's reader
// folder, reached without a link, and then the folders that became empty - never anything else, and never the server.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <string>

#include "pbc/local_files.h"
#include "pbc/sync_types.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

struct Reader {
    pbtest::TmpDir dir;
    std::string folder;
    Reader() {
        folder = dir.file("Reader");
        ::mkdir(folder.c_str(), 0700);
    }
    void mkdirs(const std::string& rel) {
        std::string path = folder;
        size_t pos = 0;
        while (pos <= rel.size()) {
            const size_t slash = rel.find('/', pos);
            path += "/" + rel.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
            ::mkdir(path.c_str(), 0700);
            if (slash == std::string::npos) break;
            pos = slash + 1;
        }
    }
    void write(const std::string& rel, const std::string& content, int64_t mtime = 1700000000) {
        const size_t slash = rel.rfind('/');
        if (slash != std::string::npos) mkdirs(rel.substr(0, slash));
        std::ofstream(folder + "/" + rel, std::ios::binary | std::ios::trunc) << content;
        struct timespec ts[2] = {{mtime, 0}, {mtime, 0}};
        ::utimensat(AT_FDCWD, (folder + "/" + rel).c_str(), ts, 0);
    }
    bool exists(const std::string& rel) const {
        struct stat st;
        return ::lstat((folder + "/" + rel).c_str(), &st) == 0;
    }
    pbc::SyncRecord record(const std::string& rel, int64_t size, int64_t mtime = 1700000000, const std::string& sha = "") const {
        pbc::SyncRecord r;
        r.pair_id = 1;
        r.rel = r.local_rel = rel;
        r.origin = "download";
        r.local_size = size;
        r.local_mtime = mtime;
        r.local_sha256 = sha;
        return r;
    }
};

}  // namespace

PB_TEST(an_unchanged_file_is_removed_and_the_folders_that_became_empty_go_with_it_up_to_the_pair_folder) {
    Reader r;
    r.write("Novels/Old/a.txt", "hello");
    r.write("Novels/keep.txt", "stays");
    PB_CHECK(pbc::remove_pair_copy(r.folder, "Novels/Old/a.txt", r.record("Novels/Old/a.txt", 5)).ok());
    PB_CHECK(!r.exists("Novels/Old/a.txt"));
    PB_CHECK(!r.exists("Novels/Old"));      // empty: removed
    PB_CHECK(r.exists("Novels/keep.txt"));  // another file keeps its folder
    PB_CHECK(r.exists("Novels"));
    r.write("Solo/only.txt", "x");
    PB_CHECK(pbc::remove_pair_copy(r.folder, "Solo/only.txt", r.record("Solo/only.txt", 1)).ok());
    PB_CHECK(!r.exists("Solo"));
    PB_CHECK(r.exists(""));                  // the pair's own folder is never removed
}

PB_TEST(a_changed_file_a_missing_one_and_a_non_file_are_refused_and_nothing_is_removed) {
    Reader r;
    r.write("a.txt", "hello");
    PB_CHECK_EQ(pbc::remove_pair_copy(r.folder, "a.txt", r.record("a.txt", 99)).key, std::string("local_changed"));              // another size
    PB_CHECK_EQ(pbc::remove_pair_copy(r.folder, "a.txt", r.record("a.txt", 5, 1700009999)).key, std::string("local_changed"));   // another time, no hash to say it is the same
    PB_CHECK(r.exists("a.txt"));
    PB_CHECK_EQ(pbc::remove_pair_copy(r.folder, "missing.txt", r.record("missing.txt", 5)).key, std::string("not_found"));
    r.mkdirs("Folder");
    PB_CHECK_EQ(pbc::remove_pair_copy(r.folder, "Folder", r.record("Folder", 0)).key, std::string("path_unsafe"));
    PB_CHECK(r.exists("Folder"));
    pbc::SyncRecord no_identity = r.record("a.txt", 5);
    no_identity.local_size.reset();
    PB_CHECK_EQ(pbc::remove_pair_copy(r.folder, "a.txt", no_identity).key, std::string("local_changed"));   // nothing says what the copy was
    PB_CHECK(r.exists("a.txt"));
}

PB_TEST(a_stored_hash_makes_a_time_only_difference_count_as_unchanged) {
    Reader r;
    r.write("a.txt", "hello", 1700005000);
    // SHA-256 of "hello"
    const std::string sha = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
    PB_CHECK(pbc::remove_pair_copy(r.folder, "a.txt", r.record("a.txt", 5, 1700000000, sha)).ok());
    PB_CHECK(!r.exists("a.txt"));
    r.write("b.txt", "hellp", 1700005000);   // same size, other bytes: the hash says it changed
    PB_CHECK_EQ(pbc::remove_pair_copy(r.folder, "b.txt", r.record("b.txt", 5, 1700000000, sha)).key, std::string("local_changed"));
    PB_CHECK(r.exists("b.txt"));
}

PB_TEST(a_link_anywhere_on_the_way_or_a_path_that_leaves_the_folder_is_path_unsafe) {
    Reader r;
    r.write("real/a.txt", "hello");
    // a link to a folder in the middle of the path
    PB_CHECK(::symlink((r.folder + "/real").c_str(), (r.folder + "/linkdir").c_str()) == 0);
    PB_CHECK_EQ(pbc::remove_pair_copy(r.folder, "linkdir/a.txt", r.record("linkdir/a.txt", 5)).key, std::string("path_unsafe"));
    PB_CHECK(r.exists("real/a.txt"));
    // a link as the file itself
    PB_CHECK(::symlink((r.folder + "/real/a.txt").c_str(), (r.folder + "/link.txt").c_str()) == 0);
    PB_CHECK_EQ(pbc::remove_pair_copy(r.folder, "link.txt", r.record("link.txt", 5)).key, std::string("path_unsafe"));
    PB_CHECK(r.exists("real/a.txt"));
    // paths that leave the folder, are absolute or empty in a component
    for (const char* bad : {"../x", "a/../../x", "/etc/passwd", "a//b", "", "./a.txt"}) {
        PB_CHECK_EQ(pbc::remove_pair_copy(r.folder, bad, r.record("x", 5)).key, std::string("path_unsafe"));
    }
    // a folder that is itself a link, or does not exist or is not absolute
    PB_CHECK_EQ(pbc::remove_pair_copy(r.folder + "/linkdir", "a.txt", r.record("a.txt", 5)).key, std::string("path_unsafe"));
    PB_CHECK_EQ(pbc::remove_pair_copy("relative/folder", "a.txt", r.record("a.txt", 5)).key, std::string("path_unsafe"));
    PB_CHECK_EQ(pbc::remove_pair_copy(r.folder + "/nowhere", "a.txt", r.record("a.txt", 5)).key, std::string("path_unsafe"));
    PB_CHECK(r.exists("real/a.txt"));
}

PB_TEST_MAIN()
