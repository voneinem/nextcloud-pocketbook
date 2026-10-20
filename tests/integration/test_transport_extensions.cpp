// The additions to the HTTP seam that file downloads need (specs/001-nextcloud-files-client, research R-8): appending to a partial
// file (resume), a progress callback that can cancel, identity encoding, a buffer size, a size cap that counts the resumed bytes, and
// the refusal of a redirect to another host. Real libcurl against a local TLS server. Needs `openssl`.
#include <sys/stat.h>

#include <cstdio>
#include <string>
#include <vector>

#include "pbc/curl_transport.h"
#include "test_support.h"
#include "tls_test_server.h"

using pbc::CurlTransport;
using pbc::ErrorClass;
using pbc::Request;
using pbtest::TlsTestServer;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

long long size_of(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 ? static_cast<long long>(st.st_size) : -1;
}

std::string raw_response(int status, const std::string& body, const std::string& extra = "", long long declared_length = -1) {
    const long long len = declared_length >= 0 ? declared_length : static_cast<long long>(body.size());
    return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Length: " + std::to_string(len) + "\r\nConnection: close\r\n" + extra + "\r\n" + body;
}

struct Fixture {
    pbtest::TmpDir dir;
    pbtest::TestPki pki{dir};
    pbtest::TestPki::Leaf leaf;
    Fixture() {
        if (!pki.available()) PB_SKIP("openssl CLI not available");
        leaf = pki.issue("good", "DNS:localhost,IP:127.0.0.1");
        PB_CHECK(leaf.ok);
    }
    Request get(const TlsTestServer& s, const std::string& target) const {
        Request r;
        r.method = "GET";
        r.url = s.url("/f");
        r.save_body_to = target;
        r.username = "demo";
        r.password = "app-secret";
        return r;
    }
};

}  // namespace

PB_TEST(save_append_continues_a_partial_file_when_the_server_answers_206) {
    Fixture f;
    TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string&) { return raw_response(206, "BBBB", "Content-Range: bytes 4-7/8\r\n"); });
    CurlTransport transport(f.pki.ca_cert());
    const std::string file = f.dir.file("part");
    f.dir.write("part", "AAAA");
    Request req = f.get(server, file);
    req.save_append = true;
    req.resume_from = 4;
    req.headers = {{"Range", "bytes=4-"}};
    const auto resp = transport.perform(req);
    PB_CHECK(resp.error == ErrorClass::None);
    PB_CHECK_EQ(resp.status, 206L);
    PB_CHECK_EQ(f.dir.read("part"), std::string("AAAABBBB"));
}

PB_TEST(save_append_restarts_from_zero_when_the_server_ignores_the_range_and_answers_200) {
    Fixture f;
    TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string&) { return raw_response(200, "CCCCCCCC"); });
    CurlTransport transport(f.pki.ca_cert());
    f.dir.write("part", "AAAA");
    Request req = f.get(server, f.dir.file("part"));
    req.save_append = true;
    req.resume_from = 4;
    req.headers = {{"Range", "bytes=4-"}};
    const auto resp = transport.perform(req);
    PB_CHECK_EQ(resp.status, 200L);
    PB_CHECK_EQ(f.dir.read("part"), std::string("CCCCCCCC"));   // never "AAAACCCCCCCC": a 200 body is not appended
}

PB_TEST(save_append_keeps_the_partial_file_on_an_error_status_and_a_plain_download_does_not) {
    Fixture f;
    TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string&) { return raw_response(416, "range"); });
    CurlTransport transport(f.pki.ca_cert());
    f.dir.write("part", "AAAA");
    Request req = f.get(server, f.dir.file("part"));
    req.save_append = true;
    req.resume_from = 4;
    PB_CHECK_EQ(transport.perform(req).status, 416L);
    PB_CHECK_EQ(f.dir.read("part"), std::string("AAAA"));   // the caller decides what to do with it

    req.save_append = false;
    PB_CHECK_EQ(transport.perform(req).status, 416L);
    PB_CHECK(!f.dir.exists("part"));                        // the old rule: nothing is left behind by a plain download
}

PB_TEST(save_append_keeps_what_arrived_when_the_connection_ends_early_and_reports_the_headers) {
    Fixture f;
    // the server promises 100 bytes and sends 10
    TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string&) { return raw_response(206, "0123456789", "ETag: \"e1\"\r\n", 100); });
    CurlTransport transport(f.pki.ca_cert());
    f.dir.write("part", "AAAA");
    Request req = f.get(server, f.dir.file("part"));
    req.save_append = true;
    req.resume_from = 4;
    const auto resp = transport.perform(req);
    PB_CHECK(resp.error != ErrorClass::None);
    PB_CHECK_EQ(size_of(f.dir.file("part")), 14LL);          // 4 old + 10 new bytes stay for the next resume
    PB_CHECK_EQ(resp.status, 206L);                          // what the server said is kept so the caller can tell which version it was
    PB_CHECK_EQ(pbc::header_value(resp.headers, "ETag"), std::string("\"e1\""));

    req.save_append = false;
    f.dir.write("part", "AAAA");
    const auto plain = transport.perform(req);
    PB_CHECK(plain.error != ErrorClass::None);
    PB_CHECK(!f.dir.exists("part"));                         // a plain download leaves nothing behind
}

PB_TEST(the_progress_callback_sees_the_bytes_and_can_cancel_the_transfer) {
    Fixture f;
    const std::string big(2u << 20, 'x');
    TlsTestServer server(f.leaf.cert, f.leaf.key, [&](const std::string&) { return raw_response(200, big); });
    CurlTransport transport(f.pki.ca_cert());
    Request req = f.get(server, f.dir.file("out"));
    req.save_append = true;   // the way the downloader always asks: the partial file belongs to the caller
    long long last_done = -1;
    int calls = 0;
    bool monotonic = true;
    req.progress = [&](long long done, long long total) {
        ++calls;
        if (done < last_done) monotonic = false;
        last_done = done;
        (void)total;
        return done < 64 * 1024;   // cancel once 64 KiB have arrived
    };
    const auto resp = transport.perform(req);
    PB_CHECK(resp.cancelled);
    PB_CHECK(resp.error == ErrorClass::Cancelled);
    PB_CHECK_EQ(resp.error_key, std::string("cancelled"));
    PB_CHECK(calls >= 1);
    PB_CHECK(monotonic);
    PB_CHECK(last_done >= 64 * 1024);
    const long long on_disk = size_of(f.dir.file("out"));
    PB_CHECK(on_disk >= 0 && on_disk < static_cast<long long>(big.size()));   // partial data stays for the caller to remove

    f.dir.write("out", "");
    req.save_append = false;   // a plain download leaves nothing behind
    last_done = -1;
    PB_CHECK(transport.perform(req).cancelled);
    PB_CHECK(!f.dir.exists("out"));
}

PB_TEST(the_progress_callback_reports_the_total_and_the_resumed_offset) {
    Fixture f;
    TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string&) { return raw_response(206, std::string(1000, 'y'), "Content-Range: bytes 500-1499/1500\r\n"); });
    CurlTransport transport(f.pki.ca_cert());
    f.dir.write("part", std::string(500, 'x'));
    Request req = f.get(server, f.dir.file("part"));
    req.save_append = true;
    req.resume_from = 500;
    long long done_max = 0, total_max = 0;
    req.progress = [&](long long done, long long total) {
        done_max = std::max(done_max, done);
        total_max = std::max(total_max, total);
        return true;
    };
    PB_CHECK_EQ(transport.perform(req).status, 206L);
    PB_CHECK_EQ(done_max, 1500LL);    // the offset is part of the count
    PB_CHECK_EQ(total_max, 1500LL);
}

PB_TEST(identity_encoding_asks_for_the_bytes_as_stored) {
    Fixture f;
    TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string&) { return raw_response(200, "ok"); });
    CurlTransport transport(f.pki.ca_cert());
    Request req = f.get(server, f.dir.file("out"));
    req.identity_encoding = true;
    req.buffer_size = 65536;
    PB_CHECK_EQ(transport.perform(req).status, 200L);
    PB_CHECK(lower(server.requests().back()).find("\r\naccept-encoding: identity\r\n") != std::string::npos);
}

PB_TEST(the_size_cap_counts_the_resumed_bytes) {
    Fixture f;
    TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string&) { return raw_response(206, "12345678", "Content-Range: bytes 4-11/12\r\n"); });
    CurlTransport transport(f.pki.ca_cert());
    f.dir.write("part", "AAAA");
    Request req = f.get(server, f.dir.file("part"));
    req.save_append = true;
    req.resume_from = 4;
    req.max_body_bytes = 10;   // 4 + 8 = 12 > 10
    const auto resp = transport.perform(req);
    PB_CHECK(resp.body_too_large);
}

PB_TEST(a_redirect_to_another_host_or_scheme_is_refused_and_never_followed) {
    Fixture f;
    TlsTestServer b(f.leaf.cert, f.leaf.key, [](const std::string&) { return raw_response(200, "other"); });
    const std::string target = b.url("/f");
    TlsTestServer a(f.leaf.cert, f.leaf.key, [&](const std::string&) { return raw_response(307, "", "Location: " + target + "\r\n"); });
    CurlTransport transport(f.pki.ca_cert());
    Request req = f.get(a, f.dir.file("out"));
    const auto resp = transport.perform(req);
    PB_CHECK(resp.error == ErrorClass::Tls);
    PB_CHECK_EQ(resp.error_key, std::string("redirect_other_host"));
    PB_CHECK(b.requests().empty());        // the other host was never contacted
    PB_CHECK(!b.saw_authorization());
    PB_CHECK(!f.dir.exists("out"));

    TlsTestServer c(f.leaf.cert, f.leaf.key, [](const std::string&) { return raw_response(302, "", "Location: http://localhost/plain\r\n"); });
    Request req2 = f.get(c, f.dir.file("out2"));
    const auto resp2 = transport.perform(req2);
    PB_CHECK_EQ(resp2.error_key, std::string("redirect_other_host"));   // a downgrade to http is a different origin as well

    // a redirect that stays on the same host and port, absolute or relative, is still followed
    std::string self_url;
    TlsTestServer d(f.leaf.cert, f.leaf.key, [&](const std::string& rq) {
        if (rq.find("GET /final") == 0) return raw_response(200, "done");
        if (rq.find("GET /abs") == 0) return raw_response(302, "", "Location: " + self_url + "/final\r\n");
        return raw_response(302, "", "Location: /final\r\n");
    });
    self_url = d.url("").substr(0, d.url("").size());
    Request rel = f.get(d, f.dir.file("rel"));
    rel.url = d.url("/rel");
    PB_CHECK_EQ(transport.perform(rel).status, 200L);
    Request abs = f.get(d, f.dir.file("abs"));
    abs.url = d.url("/abs");
    PB_CHECK_EQ(transport.perform(abs).status, 200L);
    PB_CHECK_EQ(f.dir.read("abs"), std::string("done"));
}

PB_TEST(an_error_status_does_not_create_the_file) {
    Fixture f;
    TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string&) { return raw_response(404, "gone"); });
    CurlTransport transport(f.pki.ca_cert());
    Request req = f.get(server, f.dir.file("out"));
    PB_CHECK_EQ(transport.perform(req).status, 404L);
    PB_CHECK(!f.dir.exists("out"));
}

PB_TEST_MAIN()
