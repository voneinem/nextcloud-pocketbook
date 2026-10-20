// The fake Nextcloud server of the tests (tests/support/fake_nextcloud_server.h) implements sections 1-5 and the plain GET of
// specs/001-nextcloud-files-client/contracts/server-requests.md. It is test infrastructure: what it proves is the app against this
// harness, never against a real server (constitution Principle IX).
#include <string>

#include "fake_nextcloud_server.h"
#include "pbc/dav_common.h"
#include "pbc/http_transport.h"
#include "test_support.h"

PBC_FAKE_SERVER_TEST();

using pbtest::FakeNextcloud;

namespace {

pbc::Request request(const FakeNextcloud& s, const std::string& method, const std::string& path, bool auth = true) {
    pbc::Request r;
    r.method = method;
    r.url = s.base_url() + path;
    if (auth) {
        r.username = s.login_name();
        r.password = s.app_password();
    }
    return r;
}

pbc::Request propfind(const FakeNextcloud& s, const std::string& path, const std::string& depth) {
    pbc::Request r = request(s, "PROPFIND", "/remote.php/dav/files/" + s.user_id() + path);
    r.headers = {{"Depth", depth}, {"Content-Type", "application/xml"}};
    r.body = "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:getetag/></d:prop></d:propfind>";
    return r;
}

size_t count_of(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
    return n;
}

}  // namespace

PB_TEST(status_php_needs_no_credentials_and_says_what_a_nextcloud_says) {
    FakeNextcloud s;
    const auto r = s.perform(request(s, "GET", "/status.php", false));
    PB_CHECK_EQ(r.status, 200L);
    PB_CHECK(r.body.find("\"installed\":true") != std::string::npos);
    PB_CHECK(r.body.find("\"maintenance\":false") != std::string::npos);
    PB_CHECK(r.body.find("\"version\":\"31.0.4.1\"") != std::string::npos);
    s.set_maintenance(true);
    PB_CHECK(s.perform(request(s, "GET", "/status.php", false)).body.find("\"maintenance\":true") != std::string::npos);
    PB_CHECK_EQ(s.perform(request(s, "PROPFIND", "/remote.php/dav/files/demo/")).status, 503L);   // DAV answers 503 in maintenance mode
}

PB_TEST(ocs_calls_check_the_app_password) {
    FakeNextcloud s;
    const auto caps = s.perform(request(s, "GET", "/ocs/v1.php/cloud/capabilities?format=json"));
    PB_CHECK_EQ(caps.status, 200L);
    PB_CHECK(caps.body.find("\"statuscode\":100") != std::string::npos);
    PB_CHECK(caps.body.find("\"string\":\"31.0.4\"") != std::string::npos);
    const auto user = s.perform(request(s, "GET", "/ocs/v1.php/cloud/user?format=json"));
    PB_CHECK(user.body.find("\"id\":\"demo\"") != std::string::npos);

    pbc::Request bad = request(s, "GET", "/ocs/v1.php/cloud/capabilities");
    bad.password = "wrong";
    PB_CHECK_EQ(s.perform(bad).status, 401L);
    PB_CHECK_EQ(s.perform(request(s, "GET", "/ocs/v1.php/cloud/capabilities", false)).status, 401L);
}

PB_TEST(a_depth_1_propfind_lists_the_folder_itself_and_its_children_with_the_properties_of_the_contract) {
    FakeNextcloud s;
    s.add_folder("/Books");
    s.add_file("/Books/story one.epub", "0123456789", 1790000000);
    s.add_file("/readme.txt", "hi", 1790000100);
    s.set_favorite("/readme.txt", true);
    const auto r = s.perform(propfind(s, "/", "1"));
    PB_CHECK_EQ(r.status, 207L);
    PB_CHECK_EQ(count_of(r.body, "<d:response>"), size_t(3));   // the folder itself + 2 children
    PB_CHECK(r.body.find("<d:href>/remote.php/dav/files/demo/</d:href>") != std::string::npos);
    PB_CHECK(r.body.find("<d:href>/remote.php/dav/files/demo/Books/</d:href>") != std::string::npos);   // folders end in "/"
    PB_CHECK(r.body.find("<d:href>/remote.php/dav/files/demo/readme.txt</d:href>") != std::string::npos);
    PB_CHECK(r.body.find("<d:getcontentlength>2</d:getcontentlength>") != std::string::npos);
    PB_CHECK(r.body.find("<oc:favorite>1</oc:favorite>") != std::string::npos);
    PB_CHECK(r.body.find("<oc:fileid>") != std::string::npos);
    PB_CHECK(r.body.find("<oc:permissions>") != std::string::npos);
    PB_CHECK(r.body.find("<d:collection/>") != std::string::npos);
    const auto deeper = s.perform(propfind(s, "/Books", "1"));
    PB_CHECK_EQ(count_of(deeper.body, "<d:response>"), size_t(2));
    PB_CHECK(deeper.body.find("story%20one.epub") != std::string::npos);   // each segment is percent-encoded
    // the answer is a multistatus the app's own parser reads
    PB_CHECK(pbc::dav::parse_multistatus(r).ok());
}

PB_TEST(depth_0_lists_only_the_item_and_infinity_or_a_missing_depth_is_refused) {
    FakeNextcloud s;
    s.add_folder("/Books");
    PB_CHECK_EQ(count_of(s.perform(propfind(s, "/Books", "0")).body, "<d:response>"), size_t(1));
    PB_CHECK_EQ(s.perform(propfind(s, "/Books", "infinity")).status, 403L);
    pbc::Request none = propfind(s, "/Books", "1");
    none.headers.clear();
    PB_CHECK_EQ(s.perform(none).status, 403L);
    PB_CHECK_EQ(s.perform(propfind(s, "/missing", "1")).status, 404L);
}

PB_TEST(a_root_propfind_for_quota_returns_the_quota_properties) {
    FakeNextcloud s;
    s.set_quota(1000, 9000);
    pbc::Request q = propfind(s, "/", "0");
    q.body = "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:quota-available-bytes/><d:quota-used-bytes/></d:prop></d:propfind>";
    const auto r = s.perform(q);
    PB_CHECK(r.body.find("<d:quota-used-bytes>1000</d:quota-used-bytes>") != std::string::npos);
    PB_CHECK(r.body.find("<d:quota-available-bytes>9000</d:quota-available-bytes>") != std::string::npos);
}

PB_TEST(get_returns_the_file_with_etag_length_and_date_and_changes_the_etag_when_the_file_changes) {
    FakeNextcloud s;
    s.add_file("/a.txt", "hello", 1790000000);
    const auto r = s.perform(request(s, "GET", "/remote.php/dav/files/demo/a.txt"));
    PB_CHECK_EQ(r.status, 200L);
    PB_CHECK_EQ(r.body, std::string("hello"));
    PB_CHECK_EQ(pbc::header_value(r.headers, "Content-Length"), std::string("5"));
    const std::string etag = pbc::header_value(r.headers, "ETag");
    PB_CHECK(etag.size() > 2 && etag.front() == '"' && etag.back() == '"');
    PB_CHECK(!pbc::header_value(r.headers, "Last-Modified").empty());
    s.set_file("/a.txt", "hello!", 1790000500);
    PB_CHECK(pbc::header_value(s.perform(request(s, "GET", "/remote.php/dav/files/demo/a.txt")).headers, "ETag") != etag);
    PB_CHECK_EQ(s.perform(request(s, "GET", "/remote.php/dav/files/demo/nope.txt")).status, 404L);
}

PB_TEST(requests_are_counted_by_method_and_a_foreign_host_is_unreachable) {
    FakeNextcloud s;
    s.perform(request(s, "GET", "/status.php", false));
    s.perform(propfind(s, "/", "1"));
    s.perform(propfind(s, "/", "1"));
    PB_CHECK_EQ(s.request_count(), size_t(3));
    PB_CHECK_EQ(s.count("PROPFIND"), size_t(2));
    PB_CHECK_EQ(s.count("GET"), size_t(1));
    pbc::Request other;
    other.method = "GET";
    other.url = "https://elsewhere.example.org/status.php";
    const auto r = s.perform(other);
    PB_CHECK_EQ(r.status, 0L);
    PB_CHECK(r.error != pbc::ErrorClass::None);
    s.set_offline(true);
    PB_CHECK_EQ(s.perform(request(s, "GET", "/status.php", false)).status, 0L);   // the whole server can be switched off
}

PB_TEST_MAIN()
