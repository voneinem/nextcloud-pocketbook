// Hostile and malformed folder listings (constitution Principle I): the parser returns a result or an error for every input, and a
// well-formed answer with hostile names yields only the entries that are safe to show. Also run in the sanitizer build.
#include <dirent.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "fake_transport.h"
#include "pbc/nc_listing.h"
#include "test_support.h"

using pbtest::FakeTransport;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

pbc::Result<pbc::Listing> parse(const std::string& body, const std::string& path = "/Books") {
    return pbc::parse_listing(FakeTransport::status(207, body), "demo", path);
}

std::string one_entry(const std::string& href_tail) {
    return "<d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>/remote.php/dav/files/demo/Books/" + href_tail +
           "</d:href><d:propstat><d:prop><d:getetag>\"e\"</d:getetag><d:resourcetype/></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>";
}

}  // namespace

PB_TEST(every_malformed_xml_fixture_gives_an_error_or_a_result_and_never_a_crash) {
    const std::string dir = std::string(PBC_FIXTURE_DIR) + "/malformed/xml";
    DIR* d = opendir(dir.c_str());
    PB_CHECK(d != nullptr);
    int seen = 0;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() < 5 || n.compare(n.size() - 4, 4, ".xml") != 0) continue;
        const auto r = parse(slurp(dir + "/" + n));
        if (r.ok()) PB_CHECK(r.error.key.empty());
        else PB_CHECK(!r.error.key.empty());
        ++seen;
    }
    closedir(d);
    PB_CHECK(seen >= 15);
}

PB_TEST(the_half_valid_multistatus_gives_what_is_valid_or_an_error) {
    const auto r = parse(slurp(std::string(PBC_FIXTURE_DIR) + "/malformed/xml/multistatus_half_valid.xml"));
    PB_CHECK(r.ok() || !r.error.key.empty());
}

PB_TEST(a_document_that_nests_too_deeply_or_is_over_8_mib_is_refused) {
    std::string deep = "<d:multistatus xmlns:d=\"DAV:\">";
    for (int i = 0; i < 70; ++i) deep += "<a>";
    for (int i = 0; i < 70; ++i) deep += "</a>";
    deep += "</d:multistatus>";
    PB_CHECK_EQ(parse(deep).error.key, std::string("malformed_xml"));
    PB_CHECK_EQ(parse("<d:multistatus xmlns:d=\"DAV:\">" + std::string(9u << 20, ' ') + "</d:multistatus>").error.key, std::string("malformed_xml"));
}

PB_TEST(a_truncated_answer_is_an_error) {
    const std::string full = one_entry("a.epub");
    for (size_t cut : {size_t(10), full.size() / 2, full.size() - 5})
        PB_CHECK_EQ(parse(full.substr(0, cut)).error.key, std::string("malformed_xml"));
}

PB_TEST(names_that_could_leave_a_folder_or_confuse_the_reader_never_become_entries_that_hurt) {
    // separators, dot names, controls, a name of 4,000 bytes and broken escapes are skipped
    for (const std::string tail : {std::string("..%2Fx"), std::string("%2e%2e"), std::string("%2E"), std::string("a%00b"), std::string("a%0Ab"), std::string("%"),
                                   std::string("a%2"), std::string("a%GG"), std::string(4000, 'x'), std::string("a%5Cb%2F..")}) {
        const auto r = parse(one_entry(tail));
        PB_CHECK(r.ok());
        for (const auto& e : r.value.entries) {
            PB_CHECK(e.name.find('/') == std::string::npos);
            PB_CHECK(e.name != ".." && e.name != "." && !e.name.empty());
            PB_CHECK(e.name.size() <= 1024);
        }
    }
    PB_CHECK_EQ(parse(one_entry("..%2Fx")).value.skipped_hostile, 1);
    PB_CHECK_EQ(parse(one_entry(std::string(4000, 'x'))).value.skipped_hostile, 1);
}

PB_TEST(unusual_but_valid_names_are_kept_exactly_as_sent) {
    // a backslash, a bidirectional override, NFC and NFD forms of the same name, a case-only twin: all are legal names on a server
    const std::string body =
        "<d:multistatus xmlns:d=\"DAV:\">"
        "<d:response><d:href>/remote.php/dav/files/demo/Books/a%5Cb</d:href><d:propstat><d:prop><d:getetag>\"1\"</d:getetag><d:resourcetype/></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
        "<d:response><d:href>/remote.php/dav/files/demo/Books/x%E2%80%AEgpj.exe</d:href><d:propstat><d:prop><d:getetag>\"2\"</d:getetag><d:resourcetype/></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
        "<d:response><d:href>/remote.php/dav/files/demo/Books/%C3%A4</d:href><d:propstat><d:prop><d:getetag>\"3\"</d:getetag><d:resourcetype/></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
        "<d:response><d:href>/remote.php/dav/files/demo/Books/a%CC%88</d:href><d:propstat><d:prop><d:getetag>\"4\"</d:getetag><d:resourcetype/></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
        "<d:response><d:href>/remote.php/dav/files/demo/Books/Same.TXT</d:href><d:propstat><d:prop><d:getetag>\"5\"</d:getetag><d:resourcetype/></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
        "<d:response><d:href>/remote.php/dav/files/demo/Books/same.txt</d:href><d:propstat><d:prop><d:getetag>\"6\"</d:getetag><d:resourcetype/></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
        "</d:multistatus>";
    const auto r = parse(body);
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.entries.size(), size_t(6));   // nothing is normalized or merged
    PB_CHECK_EQ(r.value.entries[0].name, std::string("a\\b"));
    PB_CHECK_EQ(r.value.entries[1].name, std::string("x\xE2\x80\xAEgpj.exe"));
    PB_CHECK(r.value.entries[2].name != r.value.entries[3].name);
}

PB_TEST_MAIN()
