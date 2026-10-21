// Names the user types or the reader supplies for the server (specs/002-manage-server-files, research R-8 and R-11): the rules every
// new name passes before it is queued, and the name "keep both" proposes. Pure.
#include <functional>
#include <set>
#include <string>

#include "pbc/new_name.h"
#include "test_support.h"

namespace {

pbc::ServerCaps caps_with_lists() {
    pbc::ServerCaps c;
    c.forbidden_chars = "<>:\"|?*";
    c.forbidden_names = {".htaccess", "Thumbs.db"};
    c.forbidden_basenames = {"con", "prn", "aux"};
    c.forbidden_extensions = {".part", "tmp"};
    return c;
}

bool ok(const std::string& name, const pbc::ServerCaps& caps = pbc::ServerCaps()) { return pbc::check_new_name(name, caps).ok; }

}  // namespace

PB_TEST(an_ordinary_name_is_accepted_and_the_check_names_its_reason_otherwise) {
    PB_CHECK(ok("Report 2026.pdf"));
    PB_CHECK(ok("a"));
    PB_CHECK(ok(".bashrc"));
    PB_CHECK(ok("name with spaces inside"));
    PB_CHECK(ok(" leading space is fine"));
    const auto r = pbc::check_new_name("a/b", pbc::ServerCaps());
    PB_CHECK(!r.ok);
    PB_CHECK_EQ(r.error_key, std::string("name_forbidden"));
}

PB_TEST(empty_dot_names_separators_and_control_characters_are_refused) {
    for (const std::string& bad : {std::string(""), std::string("."), std::string(".."), std::string("a/b"), std::string("/"), std::string("a\\b"),
                                   std::string("tab\there"), std::string("nul\0x", 5), std::string("new\nline"), std::string("\nlead"), std::string("trail\n"),
                                   std::string("del\x7f"), std::string("esc\x1b[0m")})
        PB_CHECK(!ok(bad));
}

PB_TEST(a_trailing_space_or_dot_and_names_made_of_them_are_refused) {
    for (const char* bad : {"name ", "name.", "name . ", " ", "   ", "...", ". .", "x.."}) PB_CHECK(!ok(bad));
    PB_CHECK(ok("name.txt"));
    PB_CHECK(ok("a b"));
}

PB_TEST(a_name_over_255_bytes_is_refused_and_one_of_255_is_not) {
    PB_CHECK(ok(std::string(255, 'a')));
    PB_CHECK(!ok(std::string(256, 'a')));
    std::string umlauts;   // 128 x 2 bytes = 256 bytes
    for (int i = 0; i < 128; ++i) umlauts += "\xC3\xA4";
    PB_CHECK(!ok(umlauts));
    umlauts.resize(254);
    PB_CHECK(ok(umlauts));
}

PB_TEST(non_ascii_names_are_valid_and_are_never_normalized) {
    PB_CHECK(ok("M\xC3\xBC" "nchen.txt"));                       // NFC
    PB_CHECK(ok("Mu\xCC\x88" "nchen.txt"));                      // NFD: the same word, other bytes
    PB_CHECK(ok("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.pdf"));
    PB_CHECK(ok("e\xCC\x81\xCC\x81\xCC\x81.txt"));               // combining marks
    PB_CHECK(!ok("bad\xC3(utf8.txt"));                           // invalid UTF-8 cannot be a name on the server
    PB_CHECK(!ok("\xED\xA0\x80.txt"));                           // a surrogate
}

PB_TEST(the_server_lists_forbid_characters_whole_names_base_names_and_extensions_case_insensitively) {
    const pbc::ServerCaps c = caps_with_lists();
    PB_CHECK(!ok("a<b.txt", c));
    PB_CHECK(!ok("what?.txt", c));
    PB_CHECK(!ok("pipe|d", c));
    PB_CHECK(!ok(".HTACCESS", c));            // whole names, compared without regard to case
    PB_CHECK(!ok("thumbs.DB", c));
    PB_CHECK(ok("my thumbs.db", c));
    PB_CHECK(!ok("CON", c));                  // base names: the part before the first dot
    PB_CHECK(!ok("con.txt", c));
    PB_CHECK(!ok("Aux.tar.gz", c));
    PB_CHECK(ok("console.txt", c));
    PB_CHECK(!ok("download.PART", c));        // extensions, with or without the leading dot in the list
    PB_CHECK(!ok("x.tmp", c));
    PB_CHECK(ok("tmp", c));                   // a name that merely equals the extension without a dot is not an extension
    PB_CHECK(ok("partial.txt", c));
    // the refusal names the same key as the server's own refusal
    PB_CHECK_EQ(pbc::check_new_name("a<b", c).error_key, std::string("name_forbidden"));
    // without lists nothing from the lists is refused (a missing list is unknown, not "everything allowed" in the other direction)
    PB_CHECK(ok("a<b.txt"));
    PB_CHECK(ok("con"));
}

PB_TEST(a_multibyte_forbidden_character_is_matched_as_one_character) {
    pbc::ServerCaps c;
    c.forbidden_chars = "\xE2\x80\xA6" "?";   // the ellipsis and the question mark
    PB_CHECK(!ok("wait\xE2\x80\xA6.txt", c));
    PB_CHECK(ok("wait.txt", c));
    PB_CHECK(ok("\xE2\x80" "x", c) == false);   // invalid UTF-8 is refused anyway
}

PB_TEST(keep_both_adds_a_number_before_the_last_extension) {
    const pbc::ServerCaps c;
    std::set<std::string> taken = {"report.pdf"};
    auto is_taken = [&](std::string_view n) { return taken.count(std::string(n)) > 0; };
    PB_CHECK_EQ(pbc::keep_both_name("report.pdf", is_taken, c), std::string("report (2).pdf"));
    taken.insert("report (2).pdf");
    PB_CHECK_EQ(pbc::keep_both_name("report.pdf", is_taken, c), std::string("report (3).pdf"));
    taken.insert("report (3).pdf");
    taken.insert("report (4).pdf");
    PB_CHECK_EQ(pbc::keep_both_name("report.pdf", is_taken, c), std::string("report (5).pdf"));
    PB_CHECK_EQ(pbc::keep_both_name("archive.tar.gz", [](std::string_view) { return false; }, c), std::string("archive.tar (2).gz"));
}

PB_TEST(keep_both_puts_the_number_at_the_end_of_a_name_without_extension_or_a_dotfile) {
    const pbc::ServerCaps c;
    PB_CHECK_EQ(pbc::keep_both_name("README", [](std::string_view) { return false; }, c), std::string("README (2)"));
    PB_CHECK_EQ(pbc::keep_both_name(".bashrc", [](std::string_view) { return false; }, c), std::string(".bashrc (2)"));
    PB_CHECK_EQ(pbc::keep_both_name("trailing.", [](std::string_view) { return false; }, c), std::string());   // a trailing dot is not a valid name to start from
}

PB_TEST(keep_both_never_proposes_a_name_the_server_forbids_or_that_exists_in_another_case) {
    pbc::ServerCaps c;
    c.forbidden_names = {"report (2).pdf"};
    std::set<std::string> taken = {"report.pdf"};
    auto is_taken = [&](std::string_view n) { return taken.count(std::string(n)) > 0; };
    PB_CHECK_EQ(pbc::keep_both_name("report.pdf", is_taken, c), std::string("report (3).pdf"));   // (2) is forbidden by the list
    // names are compared byte-exactly: "Report (2).pdf" is another name on a case-sensitive server
    taken.insert("Report (3).pdf");
    PB_CHECK_EQ(pbc::keep_both_name("report.pdf", is_taken, c), std::string("report (3).pdf"));
}

PB_TEST(keep_both_stays_within_255_bytes_by_shortening_the_stem_not_the_extension) {
    const pbc::ServerCaps c;
    const std::string stem(251, 'a');
    const std::string name = stem + ".pdf";   // 255 bytes
    const std::string out = pbc::keep_both_name(name, [](std::string_view) { return false; }, c);
    PB_CHECK(out.size() <= 255);
    PB_CHECK(out.size() > 8);
    PB_CHECK_EQ(out.substr(out.size() - 8), std::string(" (2).pdf"));
    PB_CHECK(pbc::check_new_name(out, c).ok);
    // multi-byte stems are cut on a character boundary
    std::string umlauts;
    for (int i = 0; i < 125; ++i) umlauts += "\xC3\xA4";   // 250 bytes, 254 with the extension
    const std::string out2 = pbc::keep_both_name(umlauts + ".txt", [](std::string_view) { return false; }, c);
    PB_CHECK(!out2.empty());
    PB_CHECK(out2.size() <= 255);
    PB_CHECK(pbc::check_new_name(out2, c).ok);   // valid UTF-8: the cut did not fall inside a character
    PB_CHECK_EQ(out2.substr(out2.size() - 8), std::string(" (2).txt"));
}

PB_TEST(keep_both_gives_up_with_an_empty_name_when_every_candidate_is_taken) {
    const pbc::ServerCaps c;
    int asked = 0;
    const std::string out = pbc::keep_both_name("a.txt", [&](std::string_view) { ++asked; return true; }, c);
    PB_CHECK_EQ(out, std::string());
    PB_CHECK(asked <= 10001);
    PB_CHECK(asked > 100);
}

PB_TEST(keep_both_of_an_invalid_name_is_empty) {
    const pbc::ServerCaps c;
    PB_CHECK_EQ(pbc::keep_both_name("a/b", [](std::string_view) { return false; }, c), std::string());
    PB_CHECK_EQ(pbc::keep_both_name("", [](std::string_view) { return false; }, c), std::string());
}

PB_TEST_MAIN()
