// The name of a conflict copy (specs/003-folder-sync R-9): "<stem> (server copy YYYY-MM-DD)<ext>" or "(reader copy ...)", a number while the name is taken, the
// extension and the 255-byte limit kept, and a result that both sides accept.
#include <set>
#include <string>

#include "pbc/conflict_name.h"
#include "pbc/local_name.h"
#include "pbc/new_name.h"
#include "test_support.h"

using pbc::conflict_name;

namespace {
const auto none = [](std::string_view) { return false; };
}

PB_TEST(a_server_copy_and_a_reader_copy_carry_their_marker_and_the_date_before_the_extension) {
    PB_CHECK_EQ(conflict_name("story.epub", true, "2026-10-21", none), std::string("story (server copy 2026-10-21).epub"));
    PB_CHECK_EQ(conflict_name("story.epub", false, "2026-10-21", none), std::string("story (reader copy 2026-10-21).epub"));
    PB_CHECK_EQ(conflict_name("a.tar.gz", true, "2026-01-02", none), std::string("a.tar (server copy 2026-01-02).gz"));   // the last extension
}

PB_TEST(a_name_without_an_extension_a_dotfile_and_a_trailing_dot_get_the_marker_at_the_end) {
    PB_CHECK_EQ(conflict_name("README", true, "2026-10-21", none), std::string("README (server copy 2026-10-21)"));
    PB_CHECK_EQ(conflict_name(".bashrc", false, "2026-10-21", none), std::string(".bashrc (reader copy 2026-10-21)"));
    PB_CHECK_EQ(conflict_name("odd.", true, "2026-10-21", none), std::string("odd. (server copy 2026-10-21)"));
}

PB_TEST(a_taken_name_gets_a_number_before_the_extension_until_one_is_free) {
    std::set<std::string> taken = {"story (server copy 2026-10-21).epub", "story (server copy 2026-10-21) (2).epub"};
    const auto is_taken = [&](std::string_view n) { return taken.count(std::string(n)) == 1; };
    PB_CHECK_EQ(conflict_name("story.epub", true, "2026-10-21", is_taken), std::string("story (server copy 2026-10-21) (3).epub"));
    taken.insert("story (server copy 2026-10-21) (3).epub");
    PB_CHECK_EQ(conflict_name("story.epub", true, "2026-10-21", is_taken), std::string("story (server copy 2026-10-21) (4).epub"));
}

PB_TEST(the_255_byte_limit_shortens_the_stem_on_a_character_boundary_and_never_the_extension) {
    std::string stem;
    for (int i = 0; i < 100; ++i) stem += "\xC3\xA4";   // 100 two-byte characters: 200 bytes
    const std::string name = stem + std::string(80, 'x') + ".epub";
    const std::string out = conflict_name(name, true, "2026-10-21", none);
    PB_CHECK(out.size() <= pbc::kMaxNameBytes);
    PB_CHECK(out.size() > 200);
    PB_CHECK(out.size() >= 5 && out.compare(out.size() - 5, 5, ".epub") == 0);
    PB_CHECK(out.find(" (server copy 2026-10-21).epub") != std::string::npos);
    PB_CHECK(pbc::check_new_name(out, pbc::ServerCaps()).ok);   // valid UTF-8: no character was cut
    // a numbered one keeps the limit too
    const auto first_taken = [&](std::string_view n) { return n == out; };
    const std::string second = conflict_name(name, true, "2026-10-21", first_taken);
    PB_CHECK(second.size() <= pbc::kMaxNameBytes);
    PB_CHECK(second != out);
    PB_CHECK(second.find(" (2).epub") != std::string::npos);
    PB_CHECK(pbc::check_new_name(second, pbc::ServerCaps()).ok);
}

PB_TEST(the_result_is_a_name_the_server_and_the_reader_accept_and_equal_input_gives_an_equal_name) {
    for (const char* n : {"story.epub", "Ünï cödé.pdf", "with space .txt", "a", ".hidden", "x.y.z"}) {
        for (bool server : {true, false}) {
            const std::string out = conflict_name(n, server, "2026-10-21", none);
            PB_CHECK(!out.empty());
            PB_CHECK(pbc::check_new_name(out, pbc::ServerCaps()).ok);
            PB_CHECK_EQ(pbc::sanitize_component(out), out);   // the reader would not change it
            PB_CHECK_EQ(conflict_name(n, server, "2026-10-21", none), out);
        }
    }
}

PB_TEST(a_name_that_cannot_get_a_copy_name_gives_an_empty_answer) {
    PB_CHECK_EQ(conflict_name("", true, "2026-10-21", none), std::string(""));
    PB_CHECK_EQ(conflict_name("a/b", true, "2026-10-21", none), std::string(""));
    PB_CHECK_EQ(conflict_name("a.epub", true, "20261021", none), std::string(""));   // not a date
    PB_CHECK_EQ(conflict_name("a.epub", true, "2026-10-21", [](std::string_view) { return true; }), std::string(""));   // everything is taken
    PB_CHECK_EQ(conflict_name("a." + std::string(250, 'e'), true, "2026-10-21", none), std::string(""));   // the extension alone leaves no room
}

PB_TEST_MAIN()
