// Local names for downloaded files (research R-9, spec FR-020, FR-022): a name from the server becomes one safe path component, the
// remote tree is mirrored under the download root, and a collision (also one that differs only in case, which FAT cannot tell apart)
// gets a suffix instead of replacing anything.
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <string>

#include "pbc/local_name.h"
#include "test_support.h"
#include "tmp_dir.h"

using pbc::choose_local_path;
using pbc::sanitize_component;

namespace {

std::string chosen(const std::string& root, const std::string& remote, const pbc::TakenMap& taken = {}) {
    const auto r = choose_local_path(root, remote, taken);
    return r.ok() ? r.value : "ERROR:" + r.error.key;
}

void make_file(const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    f << "x";
}

}  // namespace

PB_TEST(a_plain_name_stays_as_it_is) {
    PB_CHECK_EQ(sanitize_component("story.epub"), std::string("story.epub"));
    PB_CHECK_EQ(sanitize_component("Meine B\xC3\xBC" "cher (2025)"), std::string("Meine B\xC3\xBC" "cher (2025)"));
    PB_CHECK_EQ(sanitize_component(".hidden"), std::string(".hidden"));
}

PB_TEST(characters_the_storage_does_not_allow_and_control_characters_become_underscores) {
    PB_CHECK_EQ(sanitize_component("a:b*c?d\"e<f>g|h"), std::string("a_b_c_d_e_f_g_h"));
    PB_CHECK_EQ(sanitize_component("a\\b/c"), std::string("a_b_c"));
    PB_CHECK_EQ(sanitize_component(std::string("a\0b", 3)), std::string("a_b"));
    PB_CHECK_EQ(sanitize_component("a\nb\tc\x7f" "d"), std::string("a_b_c_d"));
    PB_CHECK_EQ(sanitize_component("a\xC2\x85" "b"), std::string("a_b"));            // a C1 control character
    PB_CHECK_EQ(sanitize_component("x\xE2\x80\xAE" "gpj.exe"), std::string("x_gpj.exe"));   // a right-to-left override cannot disguise an extension
}

PB_TEST(invalid_utf8_is_replaced_byte_by_byte) {
    PB_CHECK_EQ(sanitize_component("a\xFF" "b"), std::string("a_b"));
    PB_CHECK_EQ(sanitize_component("a\xC3" "b"), std::string("a_b"));                 // a truncated sequence
    PB_CHECK_EQ(sanitize_component("a\xED\xA0\x80" "b"), std::string("a___b"));      // a surrogate half
}

PB_TEST(dot_names_empty_names_and_trailing_dots_and_blanks) {
    PB_CHECK_EQ(sanitize_component(""), std::string("_"));
    PB_CHECK_EQ(sanitize_component("."), std::string("_"));
    PB_CHECK_EQ(sanitize_component(".."), std::string("_"));
    PB_CHECK_EQ(sanitize_component("..."), std::string("_"));
    PB_CHECK_EQ(sanitize_component("name. . "), std::string("name"));
    PB_CHECK_EQ(sanitize_component("   "), std::string("_"));
    PB_CHECK_EQ(sanitize_component(" lead"), std::string(" lead"));
}

PB_TEST(names_reserved_by_other_systems_get_a_prefix) {
    for (const char* n : {"CON", "con", "PRN", "AUX", "NUL", "COM1", "com9", "LPT1", "lpt9", "CON.txt", "nul.tar.gz", "Aux.epub"})
        PB_CHECK_EQ(sanitize_component(n), std::string("_") + n);
    for (const char* n : {"console.txt", "COM0", "COM10", "LPT", "auxiliary", "con-1"}) PB_CHECK_EQ(sanitize_component(n), std::string(n));
}

PB_TEST(a_name_that_looks_like_a_temporary_download_file_cannot_clash_with_one) {
    PB_CHECK_EQ(sanitize_component(".pbc-1.part"), std::string("_.pbc-1.part"));
    PB_CHECK_EQ(sanitize_component(".pbc-anything"), std::string("_.pbc-anything"));
    PB_CHECK_EQ(sanitize_component(".pbd-1.part"), std::string(".pbd-1.part"));
}

PB_TEST(a_long_name_is_cut_at_120_bytes_on_a_character_boundary_and_keeps_its_extension) {
    const std::string longname = std::string(300, 'a') + ".epub";
    const std::string s = sanitize_component(longname);
    PB_CHECK_EQ(s.size(), size_t(120));
    PB_CHECK_EQ(s.substr(s.size() - 5), std::string(".epub"));
    std::string umlauts;
    for (int i = 0; i < 100; ++i) umlauts += "\xC3\xA4";
    umlauts += ".txt";
    const std::string u = sanitize_component(umlauts);
    PB_CHECK(u.size() <= 120);
    PB_CHECK_EQ(u.substr(u.size() - 4), std::string(".txt"));
    for (size_t i = 0; i + 1 < u.size() - 4; i += 2) PB_CHECK_EQ(u.substr(i, 2), std::string("\xC3\xA4"));   // no half characters
    // a very long "extension" is not preserved: it is just part of the name
    const std::string odd = "a." + std::string(200, 'x');
    PB_CHECK_EQ(sanitize_component(odd).size(), size_t(120));
    PB_CHECK_EQ(sanitize_component(std::string(120, 'b')).size(), size_t(120));
}

PB_TEST(the_remote_tree_is_mirrored_under_the_root) {
    pbtest::TmpDir d;
    const std::string root = d.file("PBCloud");
    ::mkdir(root.c_str(), 0700);
    PB_CHECK_EQ(chosen(root, "/Books/Novels/story.epub"), std::string("Books/Novels/story.epub"));
    PB_CHECK_EQ(chosen(root, "/top.txt"), std::string("top.txt"));
    PB_CHECK_EQ(chosen(root, "/Books/a:b.txt"), std::string("Books/a_b.txt"));
    PB_CHECK_EQ(chosen(root, "/B\xC3\xBC" "cher/\xC3\x84.txt"), std::string("B\xC3\xBC" "cher/\xC3\x84.txt"));
    PB_CHECK_EQ(chosen(root, "/"), std::string("ERROR:protocol_error"));   // the root is not a file
    PB_CHECK_EQ(chosen(root, ""), std::string("ERROR:protocol_error"));
}

PB_TEST(a_recorded_mapping_is_reused) {
    pbtest::TmpDir d;
    const std::string root = d.file("PBCloud");
    ::mkdir(root.c_str(), 0700);
    const auto taken = pbc::build_taken({{"/Books/story.epub", "Books/story.epub"}, {"/Books/other.epub", "Books/other.epub"}});
    PB_CHECK_EQ(chosen(root, "/Books/story.epub", taken), std::string("Books/story.epub"));   // the same file again: the same place
    PB_CHECK_EQ(chosen(root, "/Books/new.epub", taken), std::string("Books/new.epub"));       // a neighbour joins the same folder
}

PB_TEST(a_name_that_differs_only_in_case_gets_a_suffix_before_the_extension) {
    pbtest::TmpDir d;
    const std::string root = d.file("PBCloud");
    ::mkdir(root.c_str(), 0700);
    auto taken = pbc::build_taken({{"/Books/story.epub", "Books/story.epub"}});
    PB_CHECK_EQ(chosen(root, "/Books/STORY.EPUB", taken), std::string("Books/STORY (2).EPUB"));
    taken = pbc::build_taken({{"/Books/story.epub", "Books/story.epub"}, {"/Books/STORY.EPUB", "Books/STORY (2).EPUB"}});
    PB_CHECK_EQ(chosen(root, "/Books/Story.Epub", taken), std::string("Books/Story (3).Epub"));
    PB_CHECK_EQ(chosen(root, "/Books/STORY.EPUB", taken), std::string("Books/STORY (2).EPUB"));   // and the second one keeps its place
}

PB_TEST(a_folder_that_differs_only_in_case_is_another_folder_locally_too) {
    pbtest::TmpDir d;
    const std::string root = d.file("PBCloud");
    ::mkdir(root.c_str(), 0700);
    const auto taken = pbc::build_taken({{"/Books/story.epub", "Books/story.epub"}});
    PB_CHECK_EQ(chosen(root, "/books/story.epub", taken), std::string("books (2)/story.epub"));
}

PB_TEST(a_file_on_the_card_that_the_app_does_not_know_is_never_replaced) {
    pbtest::TmpDir d;
    const std::string root = d.file("PBCloud");
    ::mkdir(root.c_str(), 0700);
    ::mkdir((root + "/Books").c_str(), 0700);
    make_file(root + "/Books/story.epub");
    make_file(root + "/Books/NOVEL.EPUB");
    PB_CHECK_EQ(chosen(root, "/Books/story.epub"), std::string("Books/story (2).epub"));
    PB_CHECK_EQ(chosen(root, "/Books/novel.epub"), std::string("Books/novel (2).epub"));       // case-insensitive, like the storage
    PB_CHECK_EQ(chosen(root, "/Books/free.epub"), std::string("Books/free.epub"));
}

PB_TEST(a_path_over_4000_bytes_is_refused_and_not_cut) {
    pbtest::TmpDir d;
    const std::string root = d.file("PBCloud");
    ::mkdir(root.c_str(), 0700);
    std::string remote;
    for (int i = 0; i < 70; ++i) remote += "/" + std::string(60, static_cast<char>('a' + i % 26));
    remote += "/file.epub";
    PB_CHECK_EQ(chosen(root, remote), std::string("ERROR:path_too_long"));
    std::string ok;
    for (int i = 0; i < 20; ++i) ok += "/" + std::string(60, 'd');
    PB_CHECK_EQ(chosen(root, ok + "/file.epub").substr(0, 6), std::string("dddddd"));
}

PB_TEST_MAIN()
