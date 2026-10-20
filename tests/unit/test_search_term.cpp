// What becomes of a search term before it is sent (contracts/server-requests.md section 8) and how the offline match reads it.
#include <string>

#include "pbc/nc_listing.h"
#include "test_support.h"

PB_TEST(wildcards_and_the_escape_are_removed_so_the_term_cannot_widen_the_search) {
    const std::string b = pbc::search_request_body("demo", "50%_off\\");
    PB_CHECK(b.find("<d:literal>%50off%</d:literal>") != std::string::npos);
}

PB_TEST(xml_special_characters_are_escaped_and_never_appear_raw) {
    const std::string b = pbc::search_request_body("demo", "a&b <c> \"d\" 'e'");
    PB_CHECK(b.find("<d:literal>%a&amp;b &lt;c&gt; &quot;d&quot; &apos;e&apos;%</d:literal>") != std::string::npos);
    PB_CHECK(b.find("a&b") == std::string::npos);
    PB_CHECK(b.find("<c>") == std::string::npos);
}

PB_TEST(an_empty_term_or_one_that_is_empty_after_cleaning_sends_nothing) {
    PB_CHECK(pbc::search_request_body("demo", "").empty());
    PB_CHECK(pbc::search_request_body("demo", "   ").empty());
    PB_CHECK(pbc::search_request_body("demo", "%_\\").empty());
}

PB_TEST(the_term_is_trimmed_and_a_control_character_is_dropped) {
    const std::string b = pbc::search_request_body("demo", "  moby\x01 dick \n");
    PB_CHECK(b.find("<d:literal>%moby dick%</d:literal>") != std::string::npos);
}

PB_TEST(a_very_long_term_is_cut) {
    const std::string b = pbc::search_request_body("demo", std::string(500, 'x'));
    PB_CHECK(b.find(std::string(100, 'x')) != std::string::npos);
    PB_CHECK(b.find(std::string(101, 'x')) == std::string::npos);
}

PB_TEST(the_user_id_is_escaped_in_the_scope) {
    const std::string b = pbc::search_request_body("de&mo", "x");
    PB_CHECK(b.find("<d:href>/files/de&amp;mo</d:href>") != std::string::npos);
}

PB_TEST_MAIN()
