// The generic WebDAV plumbing (core/include/pbc/dav_common.h). The expected values were fixed by the CalDAV and CardDAV clients of the baseline; the
// sample paths still name calendars on purpose. Scripted transport only (no server involved).
#include <string>

#include "fake_transport.h"
#include "pbc/clock_check.h"
#include "pbc/dav_common.h"
#include "pbc/error_keys.h"
#include "test_support.h"

using pbc::DavSession;
using pbtest::FakeTransport;

PB_TEST(url_for_keeps_an_address_on_the_same_origin_and_makes_a_path_absolute) {
    FakeTransport http;
    DavSession s(http, "https://dav.example.org/dav/", "alice", "pw");
    PB_CHECK_EQ(s.origin(), std::string("https://dav.example.org"));
    PB_CHECK_EQ(s.url_for("/dav/calendars/alice/"), std::string("https://dav.example.org/dav/calendars/alice/"));
    PB_CHECK_EQ(s.url_for("https://dav.example.org/dav/x"), std::string("https://dav.example.org/dav/x"));
    PB_CHECK_EQ(s.url_for("https://dav.example.org"), std::string("https://dav.example.org"));
    PB_CHECK_EQ(s.url_for("relative/path"), std::string("https://dav.example.org/relative/path"));
}

PB_TEST(url_for_refuses_another_host_and_a_scheme_relative_address) {
    FakeTransport http;
    DavSession s(http, "https://dav.example.org", "alice", "pw");
    PB_CHECK_EQ(s.url_for("//other.example/dav/"), std::string());
    PB_CHECK_EQ(s.url_for("https://other.example/dav/"), std::string());
    PB_CHECK_EQ(s.url_for("https://dav.example.org.evil.example/dav/"), std::string());
    PB_CHECK_EQ(s.url_for("http://elsewhere.example/"), std::string());
    PB_CHECK_EQ(s.url_for(""), std::string());
}

PB_TEST(url_for_of_a_session_without_a_usable_base_address_is_empty) {
    FakeTransport http;
    DavSession s(http, "not an address", "alice", "pw");
    PB_CHECK_EQ(s.url_for("/dav/"), std::string());
}

PB_TEST(send_hands_the_credentials_to_the_transport_never_in_the_url_and_caps_the_answer) {
    FakeTransport http;
    http.set_handler([](const pbc::Request&) { return FakeTransport::status(207, "<x/>", {{"Date", "Wed, 14 Oct 2026 12:00:00 GMT"}}); });
    DavSession s(http, "https://dav.example.org/dav/", "alice", "secret-pw");
    const pbc::Response r = s.send("PROPFIND", "https://dav.example.org/dav/", "<body/>", {{"Depth", "0"}}, 1234);
    PB_CHECK_EQ(r.status, 207L);
    PB_CHECK_EQ(http.requests().size(), static_cast<size_t>(1));
    const pbc::Request& q = http.requests()[0];
    PB_CHECK_EQ(q.method, std::string("PROPFIND"));
    PB_CHECK_EQ(q.username, std::string("alice"));
    PB_CHECK_EQ(q.password, std::string("secret-pw"));
    PB_CHECK_EQ(q.max_response_bytes, 1234LL);
    PB_CHECK_EQ(q.body, std::string("<body/>"));
    PB_CHECK(q.url.find("alice") == std::string::npos);
    PB_CHECK(q.url.find("secret-pw") == std::string::npos);
    PB_CHECK_EQ(pbc::header_value(q.headers, "Depth"), std::string("0"));
}

PB_TEST(send_remembers_the_server_time_of_the_date_header) {
    FakeTransport http;
    http.set_handler([](const pbc::Request&) { return FakeTransport::status(207, "", {{"Date", "Wed, 14 Oct 2026 12:00:00 GMT"}}); });
    DavSession s(http, "https://dav.example.org", "a", "b");
    PB_CHECK(!s.last_server_time().has_value());
    s.send("PROPFIND", "https://dav.example.org/", "", {});
    PB_CHECK(s.last_server_time().has_value());
    PB_CHECK_EQ(*s.last_server_time(), 1791979200LL);
}

PB_TEST(send_without_an_http_answer_keeps_the_server_time_unset) {
    FakeTransport http;
    http.set_handler([](const pbc::Request&) {
        pbc::Response r;
        r.error = pbc::ErrorClass::Network;
        r.error_key = "server_unreachable";
        return r;
    });
    DavSession s(http, "https://dav.example.org", "a", "b");
    s.send("GET", "https://dav.example.org/", "", {});
    PB_CHECK(!s.last_server_time().has_value());
}

PB_TEST(status_error_maps_the_http_status_to_the_stable_keys) {
    FakeTransport http;
    DavSession s(http, "https://dav.example.org", "a", "b");
    auto key = [&](long status) { return s.status_error(FakeTransport::status(status)).key; };
    PB_CHECK_EQ(key(401), std::string("auth_failed"));
    PB_CHECK_EQ(key(403), std::string("forbidden"));
    PB_CHECK_EQ(key(404), std::string("not_found"));
    PB_CHECK_EQ(key(500), std::string("server_error"));
    PB_CHECK_EQ(key(502), std::string("server_error"));
    PB_CHECK_EQ(key(504), std::string("server_error"));
    PB_CHECK_EQ(key(503), std::string("maintenance"));   // the caller may refine it with status.php
    PB_CHECK_EQ(key(429), std::string("rate_limited"));
    PB_CHECK_EQ(key(423), std::string("locked"));
    PB_CHECK_EQ(key(408), std::string("server_error"));
    PB_CHECK_EQ(key(418), std::string("protocol_error"));
    PB_CHECK(s.status_error(FakeTransport::status(207)).ok());
    PB_CHECK(s.status_error(FakeTransport::status(204)).ok());

    bool handled = true;
    s.status_error(FakeTransport::status(207), &handled);
    PB_CHECK(!handled);   // a 2xx status is not handled here: the caller reads the answer
    s.status_error(FakeTransport::status(401), &handled);
    PB_CHECK(handled);

    pbc::Response big = FakeTransport::status(207);
    big.body_too_large = true;
    PB_CHECK_EQ(s.status_error(big).key, std::string("too_large"));

    pbc::Response none;
    none.error = pbc::ErrorClass::Tls;
    none.error_key = "cert_expired";
    const pbc::Error e = s.status_error(none);
    PB_CHECK_EQ(e.key, std::string("cert_expired"));
    PB_CHECK(e.cls == pbc::ErrorClass::Tls);
    pbc::Response unreachable;
    PB_CHECK_EQ(s.status_error(unreachable).key, std::string("server_unreachable"));
}

PB_TEST(parse_multistatus_accepts_only_a_multistatus_root) {
    const char* ok = "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\"><d:response/></d:multistatus>";
    PB_CHECK(pbc::dav::parse_multistatus(FakeTransport::status(207, ok)).ok());
    const auto other = pbc::dav::parse_multistatus(FakeTransport::status(207, "<html><body/></html>"));
    PB_CHECK(!other.ok());
    PB_CHECK_EQ(other.error.key, std::string("malformed_xml"));
    const auto wrong_ns = pbc::dav::parse_multistatus(FakeTransport::status(207, "<multistatus xmlns=\"urn:other\"/>"));
    PB_CHECK_EQ(wrong_ns.error.key, std::string("malformed_xml"));
    PB_CHECK_EQ(pbc::dav::parse_multistatus(FakeTransport::status(207, "<d:multistatus")).error.key, std::string("malformed_xml"));
}

PB_TEST(prop_in_ok_propstat_ignores_a_propstat_with_a_failure_status) {
    const char* xml =
        "<d:multistatus xmlns:d=\"DAV:\" xmlns:cs=\"http://calendarserver.org/ns/\"><d:response>"
        "<d:propstat><d:prop><cs:getctag>stale</cs:getctag></d:prop><d:status>HTTP/1.1 404 Not Found</d:status></d:propstat>"
        "<d:propstat><d:prop><d:displayname>Work</d:displayname></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
        "</d:response></d:multistatus>";
    const auto ms = pbc::dav::parse_multistatus(FakeTransport::status(207, xml));
    PB_CHECK(ms.ok());
    const pbc::XmlNode* resp = ms.value.child(pbc::dav::kNsDav, "response");
    PB_CHECK(resp != nullptr);
    PB_CHECK(pbc::dav::prop_in_ok_propstat(*resp, pbc::dav::kNsCs, "getctag") == nullptr);
    const pbc::XmlNode* name = pbc::dav::prop_in_ok_propstat(*resp, pbc::dav::kNsDav, "displayname");
    PB_CHECK(name != nullptr);
    PB_CHECK_EQ(pbc::dav::trim(name->text), std::string("Work"));
    PB_CHECK(pbc::dav::ok_prop_set(*resp) != nullptr);
}

PB_TEST(the_owncloud_and_nextcloud_properties_are_read_through_their_namespaces) {
    const char* xml =
        "<d:multistatus xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\" xmlns:nc=\"http://nextcloud.org/ns\"><d:response>"
        "<d:propstat><d:prop><oc:size>1234</oc:size><nc:is-encrypted>1</nc:is-encrypted></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
        "</d:response></d:multistatus>";
    const auto ms = pbc::dav::parse_multistatus(FakeTransport::status(207, xml));
    PB_CHECK(ms.ok());
    const pbc::XmlNode* resp = ms.value.child(pbc::dav::kNsDav, "response");
    PB_CHECK(resp != nullptr);
    PB_CHECK_EQ(std::string(pbc::dav::kNsOc), std::string("http://owncloud.org/ns"));
    PB_CHECK_EQ(std::string(pbc::dav::kNsNc), std::string("http://nextcloud.org/ns"));
    const pbc::XmlNode* size = pbc::dav::prop_in_ok_propstat(*resp, pbc::dav::kNsOc, "size");
    PB_CHECK(size != nullptr);
    PB_CHECK_EQ(size->text, std::string("1234"));
    const pbc::XmlNode* enc = pbc::dav::prop_in_ok_propstat(*resp, pbc::dav::kNsNc, "is-encrypted");
    PB_CHECK(enc != nullptr);
    PB_CHECK_EQ(enc->text, std::string("1"));
}

PB_TEST(retry_after_is_read_as_seconds_or_a_date_and_capped_at_fifteen_minutes) {
    const int64_t now = 1792000000;   // 2026-10-14 ... any fixed instant
    auto with = [](const std::string& v) { return pbc::Headers{{"Retry-After", v}}; };
    PB_CHECK_EQ(*pbc::parse_retry_after(with("30"), now), int64_t(30));
    PB_CHECK_EQ(*pbc::parse_retry_after(with("0"), now), int64_t(0));
    PB_CHECK_EQ(*pbc::parse_retry_after(with("  120 "), now), int64_t(120));
    PB_CHECK_EQ(*pbc::parse_retry_after(with("900"), now), int64_t(900));
    PB_CHECK_EQ(*pbc::parse_retry_after(with("86400"), now), int64_t(900));   // capped
    PB_CHECK_EQ(*pbc::parse_retry_after(with("99999999999999999999"), now), int64_t(900));   // absurdly long: still just the cap
    // an HTTP date 90 seconds ahead, and one in the past
    const auto ahead = pbc::parse_http_date("Wed, 14 Oct 2026 12:00:00 GMT");
    PB_CHECK(ahead.has_value());
    PB_CHECK_EQ(*pbc::parse_retry_after(with("Wed, 14 Oct 2026 12:00:00 GMT"), *ahead - 90), int64_t(90));
    PB_CHECK_EQ(*pbc::parse_retry_after(with("Wed, 14 Oct 2026 12:00:00 GMT"), *ahead + 500), int64_t(0));
    PB_CHECK(!pbc::parse_retry_after(pbc::Headers{}, now).has_value());
    for (const char* bad : {"", "-5", "+5", "abc", "12abc", "1e3", "Wed, 99 Oct 2026 12:00:00 GMT"})
        PB_CHECK(!pbc::parse_retry_after(with(bad), now).has_value());
}

PB_TEST(small_helpers_keep_their_behaviour) {
    PB_CHECK_EQ(pbc::dav::escape("a&b<c>\"d"), std::string("a&amp;b&lt;c&gt;&quot;d"));
    PB_CHECK_EQ(pbc::dav::trim("  x y \n"), std::string("x y"));
    PB_CHECK(pbc::dav::status_ok("HTTP/1.1 200 OK"));
    PB_CHECK(pbc::dav::status_ok("HTTP/1.1 204 No Content"));
    PB_CHECK(!pbc::dav::status_ok("HTTP/1.1 404 Not Found"));
    PB_CHECK(!pbc::dav::status_ok("garbage"));
    PB_CHECK_EQ(pbc::dav::status_code("HTTP/1.1 507 Insufficient Storage"), 507);
    PB_CHECK_EQ(pbc::dav::status_code("garbage"), 0);
    PB_CHECK_EQ(pbc::dav::path_of("https://dav.example.org", "https://dav.example.org/dav/x"), std::string("/dav/x"));
    PB_CHECK_EQ(pbc::dav::path_of("https://dav.example.org", "/dav/x"), std::string("/dav/x"));
    const pbc::Headers h = pbc::dav::dav_headers("1");
    PB_CHECK_EQ(pbc::header_value(h, "Depth"), std::string("1"));
    PB_CHECK_EQ(pbc::header_value(h, "Content-Type"), std::string("application/xml; charset=utf-8"));
    PB_CHECK(std::string(pbc::dav::kXmlHeader).find("<?xml") == 0);
}

// The write side (specs/002-manage-server-files, contracts/server-requests.md W10): the status alone, one layer below the request-specific keys.
PB_TEST(status_error_maps_the_statuses_of_the_write_requests) {
    FakeTransport http;
    DavSession s(http, "https://dav.example.org", "a", "b");
    auto key = [&](long status) { return s.status_error(FakeTransport::status(status)).key; };
    PB_CHECK_EQ(key(405), std::string("protocol_error"));   // name_exists for an MKCOL is decided by the request kind, not here
    PB_CHECK_EQ(key(409), std::string("target_gone"));
    PB_CHECK_EQ(key(412), std::string("changed_on_server"));
    PB_CHECK_EQ(key(413), std::string("file_too_large_server"));
    PB_CHECK_EQ(key(415), std::string("type_not_accepted"));
    PB_CHECK_EQ(key(507), std::string("server_storage_full"));
    // the existing mappings do not change
    PB_CHECK_EQ(key(401), std::string("auth_failed"));
    PB_CHECK_EQ(key(403), std::string("forbidden"));
    PB_CHECK_EQ(key(404), std::string("not_found"));
    PB_CHECK_EQ(key(423), std::string("locked"));
    PB_CHECK_EQ(key(429), std::string("rate_limited"));
    PB_CHECK_EQ(key(503), std::string("maintenance"));
    // 201, 204 and 207 are answers the caller reads
    for (long ok : {200L, 201L, 204L, 207L}) {
        bool handled = true;
        PB_CHECK(s.status_error(FakeTransport::status(ok), &handled).ok());
        PB_CHECK(!handled);
    }
    PB_CHECK(pbc::class_of("target_gone") == pbc::ErrorClass::Server);
    PB_CHECK(s.status_error(FakeTransport::status(507)).cls == pbc::ErrorClass::Server);
}

PB_TEST(send_file_hands_a_file_range_to_the_transport_with_the_credentials_outside_the_url) {
    FakeTransport http;
    http.set_handler([](const pbc::Request&) { return FakeTransport::status(201, "", {{"Date", "Wed, 14 Oct 2026 12:00:00 GMT"}}); });
    DavSession s(http, "https://dav.example.org", "alice", "secret-pw");
    const pbc::Response r = s.send_file("PUT", "https://dav.example.org/remote.php/dav/files/alice/a.bin", "/tmp/source", 10, 20,
                                        {{"If-None-Match", "*"}}, [](long long, long long) { return true; });
    PB_CHECK_EQ(r.status, 201L);
    PB_CHECK_EQ(http.requests().size(), static_cast<size_t>(1));
    const pbc::Request& q = http.requests()[0];
    PB_CHECK_EQ(q.method, std::string("PUT"));
    PB_CHECK_EQ(q.body_file, std::string("/tmp/source"));
    PB_CHECK_EQ(q.body_offset, 10LL);
    PB_CHECK_EQ(q.body_length, 20LL);
    PB_CHECK(q.body.empty());
    PB_CHECK_EQ(q.username, std::string("alice"));
    PB_CHECK(q.progress != nullptr);
    PB_CHECK(q.url.find("secret-pw") == std::string::npos);
    PB_CHECK_EQ(pbc::header_value(q.headers, "If-None-Match"), std::string("*"));
    PB_CHECK(s.last_server_time().has_value());   // the Date header is remembered as for send()
    // an address on another host is never used
    const pbc::Response other = s.send_file("PUT", "https://evil.example.net/x", "/tmp/source", 0, 1, {}, nullptr);
    PB_CHECK_EQ(other.status, 0L);
    PB_CHECK_EQ(http.requests().size(), static_cast<size_t>(1));
}

PB_TEST_MAIN()
