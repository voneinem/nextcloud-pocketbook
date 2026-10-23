// create_picker_folder (specs/003-folder-sync/contracts/core-interfaces.md): the one place that creates a folder for the folder picker's
// "New folder". Hostile names, links on the way, an existing entry and a case-only clash are refused; otherwise exactly one folder appears.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "pbc/local_files.h"
#include "pbc/source_walk.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

struct Env {
    pbtest::TmpDir tmp;
    pbc::SourceRules rules;
    std::string root;
    Env() {
        root = tmp.file("ext1");
        for (const char* d : {"ext1", "ext1/Books", "ext1/system", "ext1/system/pbcloud"}) ::mkdir(tmp.file(d).c_str(), 0755);
        rules.root = root;
        rules.deny = {root + "/system/pbcloud"};
    }
    std::vector<std::string> names(const std::string& dir) const {
        std::vector<std::string> out;
        if (DIR* d = ::opendir(dir.c_str())) {
            while (dirent* e = ::readdir(d)) {
                const std::string n = e->d_name;
                if (n != "." && n != "..") out.push_back(n);
            }
            ::closedir(d);
        }
        std::sort(out.begin(), out.end());
        return out;
    }
    bool is_dir(const std::string& p) const {
        struct stat st;
        return ::lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }
};

}  // namespace

PB_TEST(a_plain_name_creates_exactly_one_folder_below_the_parent) {
    Env e;
    const auto r = pbc::create_picker_folder(e.rules, e.root + "/Books", "Outbox");
    PB_CHECK(r.ok());
    PB_CHECK(e.is_dir(r.value));
    PB_CHECK_EQ(e.names(e.root + "/Books").size(), size_t(1));
    PB_CHECK(r.value.size() >= 7 && r.value.compare(r.value.size() - 7, 7, "/Outbox") == 0);
    // the picker root itself is a valid parent
    const auto top = pbc::create_picker_folder(e.rules, e.root, "Inbox");
    PB_CHECK(top.ok());
    PB_CHECK(e.is_dir(e.root + "/Inbox"));
    // names with a space, an umlaut or a dash are fine
    PB_CHECK(pbc::create_picker_folder(e.rules, e.root, "Meine B\xC3\xBC" "cher - 2026").ok());
}

PB_TEST(hostile_names_are_refused_and_nothing_is_created) {
    Env e;
    const std::vector<std::string> before = e.names(e.root + "/Books");
    for (const std::string& bad : std::vector<std::string>{"", ".", "..", "../x", "a/b", "a\\b", "/abs", "x/", std::string("nul\0byte", 8), "tab\tname", "line\nbreak", "ctl\x01name",
                                                          ".hidden", "CON", "name.", "name ", "a:b", "a*b", "a?b", "a\"b", "a<b", "a>b", "a|b", std::string(300, 'n'),
                                                          ".pbc-1.part", "bad\xC3\x28utf8"}) {
        const auto r = pbc::create_picker_folder(e.rules, e.root + "/Books", bad);
        PB_CHECK(!r.ok());
        PB_CHECK_EQ(r.error.key, std::string("name_forbidden"));
    }
    PB_CHECK(e.names(e.root + "/Books") == before);
}

PB_TEST(an_existing_entry_and_a_case_only_clash_are_refused) {
    Env e;
    PB_CHECK(pbc::create_picker_folder(e.rules, e.root, "Outbox").ok());
    PB_CHECK_EQ(pbc::create_picker_folder(e.rules, e.root, "Outbox").error.key, std::string("name_exists"));
    PB_CHECK_EQ(pbc::create_picker_folder(e.rules, e.root, "OUTBOX").error.key, std::string("name_exists"));    // the storage ignores case
    PB_CHECK_EQ(pbc::create_picker_folder(e.rules, e.root, "outbox").error.key, std::string("name_exists"));
    e.tmp.write("ext1/notes.txt", "x");
    PB_CHECK_EQ(pbc::create_picker_folder(e.rules, e.root, "notes.txt").error.key, std::string("name_exists"));   // a file in the way
    PB_CHECK_EQ(pbc::create_picker_folder(e.rules, e.root, "Notes.TXT").error.key, std::string("name_exists"));
    PB_CHECK(::symlink("/etc", (e.root + "/Link").c_str()) == 0);
    PB_CHECK_EQ(pbc::create_picker_folder(e.rules, e.root, "Link").error.key, std::string("name_exists"));      // a link in the way
    PB_CHECK_EQ(e.names(e.root).size(), size_t(5));   // Books, system, Outbox, notes.txt and Link: nothing else appeared
}

PB_TEST(a_parent_outside_the_root_behind_a_link_or_in_the_data_folder_is_refused) {
    Env e;
    for (const std::string& parent : std::vector<std::string>{"/etc", "/tmp", "relative", "", e.root + "/Books/../system", e.root + "/Nowhere"}) {
        const auto r = pbc::create_picker_folder(e.rules, parent, "Outbox");
        PB_CHECK(!r.ok());
        PB_CHECK(r.error.key == "source_not_allowed" || r.error.key == "source_unreadable");
    }
    PB_CHECK(::symlink("/tmp", (e.root + "/Out").c_str()) == 0);
    PB_CHECK_EQ(pbc::create_picker_folder(e.rules, e.root + "/Out", "Outbox").error.key, std::string("source_not_allowed"));
    PB_CHECK_EQ(pbc::create_picker_folder(e.rules, e.root + "/system/pbcloud", "Outbox").error.key, std::string("source_not_allowed"));   // the app's own data
    e.tmp.write("ext1/Books/file.txt", "x");
    PB_CHECK_EQ(pbc::create_picker_folder(e.rules, e.root + "/Books/file.txt", "Outbox").error.key, std::string("source_not_allowed"));    // a file is no parent
}

PB_TEST_MAIN()
