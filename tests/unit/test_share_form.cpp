// The form body of the share requests (specs/004-share-links, contracts/share-requests.md "Conventions"): the only place that turns a password
// into bytes. Every byte except A-Z a-z 0-9 - . _ ~ is percent-encoded; a space is %20, never +.
#include <string>
#include <utility>
#include <vector>

#include "pbc/share_api.h"
#include "test_support.h"

namespace {
using Fields = std::vector<std::pair<std::string, std::string>>;
}

PB_TEST(unreserved_characters_stay_as_they_are) {
    PB_CHECK_EQ(pbc::form_encode({{"a", "AZaz09-._~"}}), std::string("a=AZaz09-._~"));
}

PB_TEST(everything_else_is_percent_encoded_in_upper_case_hex) {
    PB_CHECK_EQ(pbc::form_encode({{"p", "a b"}}), std::string("p=a%20b"));
    PB_CHECK_EQ(pbc::form_encode({{"p", "a+b"}}), std::string("p=a%2Bb"));
    PB_CHECK_EQ(pbc::form_encode({{"p", "&=%#?/"}}), std::string("p=%26%3D%25%23%3F%2F"));
    PB_CHECK_EQ(pbc::form_encode({{"p", "\n\t"}}), std::string("p=%0A%09"));
    PB_CHECK_EQ(pbc::form_encode({{"p", std::string("\0", 1)}}), std::string("p=%00"));
}

PB_TEST(multi_byte_characters_are_encoded_byte_by_byte) {
    PB_CHECK_EQ(pbc::form_encode({{"p", "\xc3\xa4"}}), std::string("p=%C3%A4"));                       // ä
    PB_CHECK_EQ(pbc::form_encode({{"p", "\xf0\x9f\x99\x82"}}), std::string("p=%F0%9F%99%82"));          // an emoji
}

PB_TEST(names_are_encoded_too) {
    PB_CHECK_EQ(pbc::form_encode({{"a b", "1"}}), std::string("a%20b=1"));
}

PB_TEST(the_field_order_is_preserved_and_pairs_are_joined_with_ampersands) {
    PB_CHECK_EQ(pbc::form_encode({{"path", "/Docs/a b.pdf"}, {"shareType", "3"}, {"permissions", "1"}}),
                std::string("path=%2FDocs%2Fa%20b.pdf&shareType=3&permissions=1"));
}

PB_TEST(an_empty_value_gives_name_equals_and_an_empty_list_gives_nothing) {
    PB_CHECK_EQ(pbc::form_encode({{"password", ""}}), std::string("password="));
    PB_CHECK_EQ(pbc::form_encode(Fields{}), std::string());
}

PB_TEST_MAIN()
