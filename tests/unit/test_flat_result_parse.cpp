// Favorites and server search (contracts/server-requests.md sections 7 and 8): the two request bodies and what a multi-folder answer
// becomes. The fixtures were written from the API notes and carry the .unverified marker until a live run produced them
// (docs/live-server-checklist.md LV-N8, LV-N9).
#include <fstream>
#include <sstream>
#include <string>

#include "fake_transport.h"
#include "pbc/nc_listing.h"
#include "test_support.h"

using pbc::Availability;
using pbc::Kind;
using pbtest::FakeTransport;

namespace {

std::string fixture(const std::string& dir, const std::string& name) {
    std::ifstream f(std::string(PBC_FIXTURE_DIR) + "/nextcloud/" + dir + "/" + name, std::ios::binary);
    if (!f) PB_FAIL_AT("missing fixture " + name);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

pbc::Result<std::vector<pbc::RemoteEntry>> parse(const std::string& body, size_t cap = 2000, long status = 207) {
    return pbc::parse_flat_result(FakeTransport::status(status, body), "demo", cap);
}

const pbc::RemoteEntry* find(const std::vector<pbc::RemoteEntry>& v, const std::string& path) {
    for (const auto& e : v)
        if (e.path == path) return &e;
    return nullptr;
}

std::string many(int n) {
    std::string x = "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\">";
    for (int i = 0; i < n; ++i)
        x += "<d:response><d:href>/remote.php/dav/files/demo/D" + std::to_string(i % 7) + "/f" + std::to_string(i) +
             ".txt</d:href><d:propstat><d:prop><d:getetag>\"e\"</d:getetag><d:resourcetype/></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
    return x + "</d:multistatus>";
}

}  // namespace

PB_TEST(a_favorites_answer_gives_entries_from_many_folders_with_their_full_paths) {
    const auto r = parse(fixture("report", "favorites_basic.unverified.xml"));
    PB_CHECK(r.ok());
    const auto& v = r.value;
    const auto* story = find(v, "/Books/My Story \xC3\xA4.epub");
    PB_CHECK(story != nullptr);
    PB_CHECK(story->kind == Kind::File);
    PB_CHECK_EQ(story->name, std::string("My Story \xC3\xA4.epub"));
    PB_CHECK_EQ(*story->size, int64_t(152000));
    PB_CHECK(story->favorite);
    const auto* reports = find(v, "/Work/Reports");
    PB_CHECK(reports != nullptr);
    PB_CHECK(reports->kind == Kind::Folder);
    PB_CHECK(find(v, "/notes.txt") != nullptr);   // directly in the root
}

PB_TEST(entries_outside_the_account_prefix_and_hostile_names_are_dropped_and_the_order_is_kept) {
    const auto r = parse(fixture("report", "favorites_basic.unverified.xml"));
    PB_CHECK(r.ok());
    for (const auto& e : r.value) {
        PB_CHECK(e.path.find("Secret") == std::string::npos);   // another user's tree
        PB_CHECK(e.path.find("..") == std::string::npos);       // a dot-dot segment
    }
    PB_CHECK_EQ(r.value.size(), size_t(4));   // story, Reports, notes, shared-secret (no read permission: listed, marked)
    PB_CHECK_EQ(r.value[0].path, std::string("/Books/My Story \xC3\xA4.epub"));
    PB_CHECK_EQ(r.value[2].path, std::string("/notes.txt"));
    const auto* shared = find(r.value, "/Private/shared-secret.docx");
    PB_CHECK(shared != nullptr);
    PB_CHECK(shared->availability == Availability::NoPermission);
}

PB_TEST(a_search_answer_is_parsed_like_a_favorites_answer) {
    const auto r = parse(fixture("search", "search_basic.unverified.xml"), 100);
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.size(), size_t(2));
    PB_CHECK_EQ(r.value[0].path, std::string("/Books/Novels/Moby Dick.epub"));
    PB_CHECK_EQ(*r.value[0].size, int64_t(800000));
    PB_CHECK(r.value[1].kind == Kind::Folder);
}

PB_TEST(the_cap_keeps_the_first_entries_only) {
    PB_CHECK_EQ(parse(many(30), 10).value.size(), size_t(10));
    PB_CHECK_EQ(parse(many(30), 10).value[0].name, std::string("f0.txt"));
    PB_CHECK_EQ(parse(many(30), 100).value.size(), size_t(30));
}

PB_TEST(errors_are_error_keys_and_never_a_crash) {
    PB_CHECK_EQ(parse("<html/>", 10, 200).error.key, std::string("protocol_error"));
    PB_CHECK_EQ(parse("<d:multistatus", 10).error.key, std::string("malformed_xml"));
    PB_CHECK(parse("<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\"/>", 10).value.empty());
    pbc::Response big = FakeTransport::status(207, "");
    big.body_too_large = true;
    PB_CHECK_EQ(pbc::parse_flat_result(big, "demo", 10).error.key, std::string("listing_too_large"));
}

PB_TEST(the_favorites_body_asks_for_favorites_with_the_listing_properties) {
    const std::string b = pbc::favorites_request_body();
    PB_CHECK(b.find("<oc:filter-files") != std::string::npos);
    PB_CHECK(b.find("<oc:filter-rules><oc:favorite>1</oc:favorite></oc:filter-rules>") != std::string::npos);
    for (const char* prop : {"<d:getetag/>", "<d:getlastmodified/>", "<d:getcontentlength/>", "<d:getcontenttype/>", "<d:resourcetype/>", "<oc:fileid/>", "<oc:permissions/>", "<oc:size/>", "<oc:favorite/>"})
        PB_CHECK(b.find(prop) != std::string::npos);
    PB_CHECK(b.find("xmlns:oc=\"http://owncloud.org/ns\"") != std::string::npos);
}

PB_TEST(the_search_body_is_the_basicsearch_of_the_contract) {
    const std::string b = pbc::search_request_body("demo", "moby");
    PB_CHECK(b.find("<d:searchrequest") != std::string::npos);
    PB_CHECK(b.find("<d:href>/files/demo</d:href><d:depth>infinity</d:depth>") != std::string::npos);
    PB_CHECK(b.find("<d:like><d:prop><d:displayname/></d:prop><d:literal>%moby%</d:literal></d:like>") != std::string::npos);
    PB_CHECK(b.find("<d:nresults>101</d:nresults>") != std::string::npos);
    PB_CHECK(b.find("<d:orderby>") != std::string::npos);
}

PB_TEST_MAIN()
