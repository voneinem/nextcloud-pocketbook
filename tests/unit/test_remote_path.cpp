// Remote paths of the files client (specs/001-nextcloud-files-client, data-model.md "Paths"): normalization, encoding for requests,
// and decoding of the d:href values a server sends. Hostile hrefs must be refused, never guessed at.
#include <string>

#include "pbc/remote_path.h"
#include "test_support.h"

using pbc::decode_href;
using pbc::encode_path;
using pbc::is_valid_name;
using pbc::join_remote;
using pbc::normalize_remote_path;
using pbc::parent_of;

namespace {
std::string decoded(const std::string& href, const std::string& uid = "demo") {
    const auto r = decode_href(href, uid);
    return r.ok() ? r.value : "ERROR:" + r.error.key;
}
}  // namespace

PB_TEST(normalize_gives_the_canonical_form) {
    PB_CHECK_EQ(normalize_remote_path(""), std::string("/"));
    PB_CHECK_EQ(normalize_remote_path("/"), std::string("/"));
    PB_CHECK_EQ(normalize_remote_path("a"), std::string("/a"));
    PB_CHECK_EQ(normalize_remote_path("/a/b/"), std::string("/a/b"));
    PB_CHECK_EQ(normalize_remote_path("//a///b//"), std::string("/a/b"));
    PB_CHECK_EQ(normalize_remote_path("///"), std::string("/"));
    PB_CHECK_EQ(normalize_remote_path("/Bücher/Ä"), std::string("/Bücher/Ä"));   // the bytes are kept, nothing is normalized
}

PB_TEST(parent_and_join) {
    PB_CHECK_EQ(parent_of("/a/b"), std::string("/a"));
    PB_CHECK_EQ(parent_of("/a"), std::string("/"));
    PB_CHECK_EQ(parent_of("/"), std::string("/"));
    PB_CHECK_EQ(parent_of("a/b/"), std::string("/a"));
    PB_CHECK_EQ(join_remote("/", "a"), std::string("/a"));
    PB_CHECK_EQ(join_remote("/a", "b.txt"), std::string("/a/b.txt"));
    PB_CHECK_EQ(join_remote("/a/", "b"), std::string("/a/b"));
}

PB_TEST(encoding_percent_encodes_each_segment_as_utf8_and_never_uses_plus) {
    PB_CHECK_EQ(encode_path("/"), std::string("/"));
    PB_CHECK_EQ(encode_path(""), std::string("/"));
    PB_CHECK_EQ(encode_path("/Books/My Book+.epub"), std::string("/Books/My%20Book%2B.epub"));
    PB_CHECK_EQ(encode_path("/ä"), std::string("/%C3%A4"));
    PB_CHECK_EQ(encode_path("/a-b_c.d~e"), std::string("/a-b_c.d~e"));   // unreserved characters stay
    PB_CHECK_EQ(encode_path("/a%b"), std::string("/a%25b"));
    PB_CHECK_EQ(encode_path("/a?b#c"), std::string("/a%3Fb%23c"));
    PB_CHECK_EQ(encode_path("/a/b"), std::string("/a/b"));
}

PB_TEST(decode_href_strips_the_files_prefix_and_decodes_each_segment) {
    PB_CHECK_EQ(decoded("/remote.php/dav/files/demo/"), std::string("/"));
    PB_CHECK_EQ(decoded("/remote.php/dav/files/demo"), std::string("/"));
    PB_CHECK_EQ(decoded("/remote.php/dav/files/demo/Books/"), std::string("/Books"));
    PB_CHECK_EQ(decoded("/remote.php/dav/files/demo/Books/My%20Book%2B.epub"), std::string("/Books/My Book+.epub"));
    PB_CHECK_EQ(decoded("/remote.php/dav/files/demo/%C3%A4"), std::string("/ä"));
    PB_CHECK_EQ(decoded("https://cloud.example.org/remote.php/dav/files/demo/a"), std::string("/a"));   // an absolute address is accepted
    PB_CHECK_EQ(decoded("/remote.php/dav/files/d%C3%A9mo/x", "démo"), std::string("/x"));              // the user id is compared decoded
}

PB_TEST(decode_href_refuses_a_foreign_prefix_or_another_user) {
    PB_CHECK_EQ(decoded("/remote.php/dav/files/other/a"), std::string("ERROR:foreign_href"));
    PB_CHECK_EQ(decoded("/remote.php/dav/files/Demo/a"), std::string("ERROR:foreign_href"));   // case-sensitive
    PB_CHECK_EQ(decoded("/remote.php/webdav/demo/a"), std::string("ERROR:foreign_href"));
    PB_CHECK_EQ(decoded("/index.php/a"), std::string("ERROR:foreign_href"));
    PB_CHECK_EQ(decoded(""), std::string("ERROR:foreign_href"));
    PB_CHECK_EQ(decoded("a/b"), std::string("ERROR:foreign_href"));
}

PB_TEST(decode_href_refuses_hostile_segments) {
    for (const char* bad : {
             "/remote.php/dav/files/demo/%2e%2e/x",     // ".." in disguise
             "/remote.php/dav/files/demo/../x",
             "/remote.php/dav/files/demo/./x",
             "/remote.php/dav/files/demo/a%2Fb",        // an encoded separator inside one name
             "/remote.php/dav/files/demo/a%2fb",
             "/remote.php/dav/files/demo/a%00b",        // NUL
             "/remote.php/dav/files/demo/a%0Ab",        // control character
             "/remote.php/dav/files/demo/a%zz",         // not hex
             "/remote.php/dav/files/demo/a%4",          // truncated escape
             "/remote.php/dav/files/demo/a%",
             "/remote.php/dav/files/demo/a//b",         // empty segment inside the path
             "/remote.php/dav/files/demo/a?b=1",        // a query is not part of a name
             "/remote.php/dav/files/demo/a#b",
         })
        PB_CHECK_EQ(decoded(bad), std::string("ERROR:protocol_error"));
}

PB_TEST(names_are_valid_only_if_they_can_be_one_path_segment) {
    PB_CHECK(is_valid_name("book.epub"));
    PB_CHECK(is_valid_name("Ä Ö ü ß"));
    PB_CHECK(is_valid_name("a..b"));
    PB_CHECK(is_valid_name(".hidden"));
    PB_CHECK(!is_valid_name(""));
    PB_CHECK(!is_valid_name("."));
    PB_CHECK(!is_valid_name(".."));
    PB_CHECK(!is_valid_name("a/b"));
    PB_CHECK(!is_valid_name(std::string("a\0b", 3)));
    PB_CHECK(!is_valid_name("a\nb"));
    PB_CHECK(!is_valid_name("a\x7f"));
    PB_CHECK(!is_valid_name(std::string(1025, 'x')));
    PB_CHECK(is_valid_name(std::string(1024, 'x')));
}

PB_TEST_MAIN()
