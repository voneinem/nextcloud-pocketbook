// Hostile names against the download root (spec SC-016, constitution Principle IV): whatever a server calls a file, the app never
// writes outside the download root, never replaces another file, and never follows a link planted in the tree.
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <string>
#include <vector>

#include "pbc/local_name.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

bool inside(const std::string& rel) {
    if (rel.empty() || rel[0] == '/') return false;
    size_t pos = 0;
    while (pos <= rel.size()) {
        size_t next = rel.find('/', pos);
        if (next == std::string::npos) next = rel.size();
        const std::string part = rel.substr(pos, next - pos);
        if (part.empty() || part == "." || part == "..") return false;
        for (unsigned char c : part)
            if (c < 0x20 || c == 0x7f || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
        pos = next + 1;
    }
    return true;
}

}  // namespace

PB_TEST(whatever_the_name_the_result_is_a_clean_relative_path_inside_the_root) {
    pbtest::TmpDir d;
    const std::string root = d.file("PBCloud");
    ::mkdir(root.c_str(), 0700);
    std::vector<std::string> names = {
        "..", ".", "...", "a/b", "a\\b", "CON.txt", std::string("a\0b", 3), "a\nb", std::string(1, '\x01'), "x\xE2\x80\xAE" "gpj", "\xFF\xFE",
        std::string(4000, 'x'), std::string(1, '.') + std::string(500, '.'), "A.txt", "a.TXT", " ", "trailing. ", ".pbc-1.part",
        "a\xCC\x88" ".txt" /* NFD a-umlaut */, "\xC3\xA4.txt" /* NFC */, "COM1", "lpt3.txt", "name:stream", "~$temp", "-rf", "$(rm -rf)", "`id`",
    };
    for (int len : {1, 50, 119, 120, 121, 255, 1000}) names.push_back(std::string(static_cast<size_t>(len), 'n'));
    for (const std::string& name : names) {
        for (const std::string prefix : {std::string("/"), std::string("/Books/"), std::string("/a/b/c/")}) {
            // the name is one segment; the callers have already refused separators, but even a raw one must not escape
            const auto r = pbc::choose_local_path(root, prefix + name, {});
            if (r.ok()) PB_CHECK(inside(r.value));
            else PB_CHECK(!r.error.key.empty());
        }
    }
}

PB_TEST(two_names_that_the_storage_cannot_tell_apart_never_get_one_local_path) {
    pbtest::TmpDir d;
    const std::string root = d.file("PBCloud");
    ::mkdir(root.c_str(), 0700);
    pbc::TakenMap taken;
    std::vector<std::string> seen;
    // each chosen path is recorded before the next one is chosen, as the downloader does
    for (const char* remote : {"/A.txt", "/a.TXT", "/a.txt", "/A.TXT", "/\xC3\x84.txt", "/\xC3\xA4.txt"}) {
        const auto r = pbc::choose_local_path(root, remote, taken);
        PB_CHECK(r.ok());
        std::string folded = r.value;
        for (char& c : folded) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const std::string& s : seen) PB_CHECK(s != folded);
        seen.push_back(folded);
        taken = pbc::build_taken([&] {
            std::vector<std::pair<std::string, std::string>> v;
            for (size_t i = 0; i < seen.size(); ++i) v.push_back({"/rec" + std::to_string(i), seen[i]});
            return v;
        }());
    }
    PB_CHECK_EQ(seen.size(), size_t(6));
}

PB_TEST(a_link_planted_in_the_tree_is_never_followed) {
    pbtest::TmpDir d;
    const std::string root = d.file("PBCloud");
    const std::string elsewhere = d.file("elsewhere");
    ::mkdir(root.c_str(), 0700);
    ::mkdir(elsewhere.c_str(), 0700);
    PB_CHECK(::symlink(elsewhere.c_str(), (root + "/Books").c_str()) == 0);
    auto r = pbc::choose_local_path(root, "/Books/x.epub", {});
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("path_unsafe"));
    // a link at the file level: the name is taken, so the new file gets another name and the link is not touched
    PB_CHECK(::symlink((elsewhere + "/target").c_str(), (root + "/book.epub").c_str()) == 0);
    r = pbc::choose_local_path(root, "/book.epub", {});
    PB_CHECK(!r.ok() || r.value != "book.epub");
    // a regular file where a folder is needed
    {
        std::ofstream f(root + "/Docs");
        f << "x";
    }
    PB_CHECK_EQ(pbc::choose_local_path(root, "/Docs/a.txt", {}).error.key, std::string("path_unsafe"));
}

PB_TEST(a_download_root_that_is_itself_a_link_is_refused) {
    pbtest::TmpDir d;
    const std::string real = d.file("real");
    ::mkdir(real.c_str(), 0700);
    const std::string root = d.file("PBCloud");
    PB_CHECK(::symlink(real.c_str(), root.c_str()) == 0);
    PB_CHECK_EQ(pbc::choose_local_path(root, "/a.txt", {}).error.key, std::string("path_unsafe"));
    PB_CHECK_EQ(pbc::choose_local_path(d.file("missing"), "/a.txt", {}).error.key, std::string("path_unsafe"));   // and a root that does not exist
}

PB_TEST_MAIN()
