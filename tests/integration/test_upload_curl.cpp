// The additions to the HTTP seam that uploads need (specs/002-manage-server-files, research R-1, R-7, contracts/core-interfaces.md):
// a request body that is a range of a file, streamed read-only with a small buffer; progress and cancel while uploading; how far a
// request got when there is no answer (Response::delivery); the write verbs and their headers pass through unchanged; a redirect to
// another host is refused for a write. Real libcurl against a local TLS server. Needs `openssl`.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "pbc/curl_transport.h"
#include "test_support.h"
#include "tls_test_server.h"

using pbc::CurlTransport;
using pbc::Delivery;
using pbc::ErrorClass;
using pbc::Request;
using pbtest::TlsTestServer;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// the body of a raw request (after the blank line)
std::string body_of(const std::string& raw) {
    const size_t e = raw.find("\r\n\r\n");
    return e == std::string::npos ? std::string() : raw.substr(e + 4);
}

// deterministic content: byte i = (i * 31 + 7) & 0xff
std::string pattern(long long offset, long long length) {
    std::string out(static_cast<size_t>(length), '\0');
    for (long long i = 0; i < length; ++i) out[static_cast<size_t>(i)] = static_cast<char>(((offset + i) * 31 + 7) & 0xff);
    return out;
}

long long vm_hwm_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.compare(0, 6, "VmHWM:") == 0) return std::atoll(line.c_str() + 6);
    return -1;
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
    Request put(const TlsTestServer& s, const std::string& file, long long offset, long long length) const {
        Request r;
        r.method = "PUT";
        r.url = s.url("/remote.php/dav/files/demo/up.bin");
        r.body_file = file;
        r.body_offset = offset;
        r.body_length = length;
        r.username = "demo";
        r.password = "app-secret";
        r.headers = {{"Content-Type", "application/octet-stream"}, {"If-None-Match", "*"}};
        return r;
    }
    std::string make_file(const std::string& name, long long size) const {
        dir.write(name, pattern(0, size));
        return dir.file(name);
    }
    std::string make_sparse(const std::string& name, long long size) const {
        const std::string p = dir.file(name);
        int fd = ::open(p.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        PB_CHECK(fd >= 0);
        PB_CHECK(::ftruncate(fd, size) == 0);
        ::close(fd);
        return p;
    }
};

}  // namespace

PB_TEST(a_put_sends_exactly_the_requested_range_of_the_file) {
    Fixture f;
    TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string&) { return TlsTestServer::http_response(201, ""); });
    CurlTransport transport(f.pki.ca_cert());
    const std::string file = f.make_file("src.bin", 5ll << 20);

    auto resp = transport.perform(f.put(server, file, 0, 5ll << 20));
    PB_CHECK(resp.error == ErrorClass::None);
    PB_CHECK_EQ(resp.status, 201L);
    PB_CHECK(resp.delivery == Delivery::FullySent);
    auto reqs = server.requests();
    PB_CHECK_EQ(reqs.size(), size_t(1));
    PB_CHECK(reqs[0].compare(0, 4, "PUT ") == 0);
    PB_CHECK(lower(reqs[0]).find("content-length: " + std::to_string(5ll << 20)) != std::string::npos);
    PB_CHECK(lower(reqs[0]).find("if-none-match: *") != std::string::npos);
    PB_CHECK(body_of(reqs[0]) == pattern(0, 5ll << 20));

    // a chunk from the middle: bytes [3 MiB, 4 MiB)
    resp = transport.perform(f.put(server, file, 3ll << 20, 1ll << 20));
    PB_CHECK_EQ(resp.status, 201L);
    reqs = server.requests();
    PB_CHECK_EQ(reqs.size(), size_t(2));
    PB_CHECK(lower(reqs[1]).find("content-length: " + std::to_string(1ll << 20)) != std::string::npos);
    PB_CHECK(body_of(reqs[1]) == pattern(3ll << 20, 1ll << 20));
}

PB_TEST(a_large_put_is_streamed_and_the_memory_does_not_grow_with_the_file) {
    Fixture f;
    TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string&) { return TlsTestServer::http_response(201, ""); });
    server.set_discard_body(true);
    CurlTransport transport(f.pki.ca_cert());
    const long long size = 200ll << 20;
    const std::string file = f.make_sparse("big.bin", size);

    const long long before = vm_hwm_kb();
    Request req = f.put(server, file, 0, size);
    req.total_timeout_s = 120;
    const auto resp = transport.perform(req);
    const long long after = vm_hwm_kb();
    PB_CHECK_EQ(resp.status, 201L);
    PB_CHECK_EQ(server.body_bytes_received(), size);
    PB_CHECK(before > 0 && after > 0);
    PB_CHECK(after - before < 16 * 1024);   // less than 16 MB more for a 200 MB upload
}

PB_TEST(the_progress_callback_sees_the_upload_and_returning_false_cancels_it) {
    Fixture f;
    TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string&) { return TlsTestServer::http_response(201, ""); });
    server.set_discard_body(true);
    CurlTransport transport(f.pki.ca_cert());
    const long long size = 64ll << 20;
    const std::string file = f.make_sparse("p.bin", size);

    long long last_done = 0, last_total = 0;
    int calls = 0;
    Request ok = f.put(server, file, 0, size);
    ok.progress = [&](long long done, long long total) {
        ++calls;
        last_done = done;
        last_total = total;
        return true;
    };
    auto resp = transport.perform(ok);
    PB_CHECK_EQ(resp.status, 201L);
    PB_CHECK(calls > 1);
    PB_CHECK_EQ(last_total, size);
    PB_CHECK_EQ(last_done, size);

    Request cancel = f.put(server, file, 0, size);
    cancel.progress = [&](long long done, long long) { return done < (4ll << 20); };
    resp = transport.perform(cancel);
    PB_CHECK(resp.cancelled);
    PB_CHECK(resp.error == ErrorClass::Cancelled);
}

PB_TEST(a_body_file_that_is_not_a_readable_regular_file_of_the_right_length_is_refused_before_anything_is_sent) {
    Fixture f;
    TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string&) { return TlsTestServer::http_response(201, ""); });
    CurlTransport transport(f.pki.ca_cert());
    const std::string file = f.make_file("src.bin", 1000);

    auto expect_refused = [&](const Request& r, const char* what) {
        const auto resp = transport.perform(r);
        if (resp.status != 0 || resp.error_key != "source_unreadable" || resp.delivery != Delivery::NotSent)
            PB_FAIL_AT(std::string("not refused: ") + what + " status " + std::to_string(resp.status) + " key " + resp.error_key);
    };
    expect_refused(f.put(server, f.dir.file("missing.bin"), 0, 10), "missing file");
    expect_refused(f.put(server, f.dir.path(), 0, 10), "directory");
    expect_refused(f.put(server, file, 900, 200), "range beyond the end");
    expect_refused(f.put(server, file, 0, 1001), "longer than the file");
    PB_CHECK(::symlink(file.c_str(), f.dir.file("link.bin").c_str()) == 0);
    expect_refused(f.put(server, f.dir.file("link.bin"), 0, 10), "symbolic link");
    ::chmod(file.c_str(), 0);
    if (::access(file.c_str(), R_OK) != 0) expect_refused(f.put(server, file, 0, 10), "unreadable file");   // not as root
    ::chmod(file.c_str(), 0600);
    PB_CHECK_EQ(server.requests().size(), size_t(0));   // nothing reached the server
}

PB_TEST(delivery_tells_how_far_a_request_got_when_there_is_no_answer) {
    Fixture f;
    CurlTransport transport(f.pki.ca_cert());
    const std::string file = f.make_file("src.bin", 4096);

    // nothing listening: the request never left
    {
        Request r = f.put(TlsTestServer(f.leaf.cert, f.leaf.key, [](const std::string&) { return std::string(); }), file, 0, 4096);
        // the server object above is already gone, so the port refuses the connection
        r.connect_timeout_s = 3;
        const auto resp = transport.perform(r);
        PB_CHECK_EQ(resp.status, 0L);
        PB_CHECK(resp.delivery == Delivery::NotSent);
    }
    // the server reads the whole body and hangs up without an answer: the request may have been processed
    {
        TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string&) { return std::string(); });
        const auto resp = transport.perform(f.put(server, file, 0, 4096));
        PB_CHECK_EQ(resp.status, 0L);
        PB_CHECK(resp.delivery == Delivery::FullySent);
        // a write verb without a body behaves the same
        Request mv;
        mv.method = "MOVE";
        mv.url = server.url("/remote.php/dav/files/demo/a");
        mv.username = "demo";
        mv.password = "app-secret";
        const auto resp2 = transport.perform(mv);
        PB_CHECK_EQ(resp2.status, 0L);
        PB_CHECK(resp2.delivery == Delivery::FullySent);
    }
    // the server dies in the middle of a large body
    {
        TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string&) { return std::string(); });
        server.set_discard_body(true);
        server.set_body_read_limit(256 << 10);
        const long long size = 48ll << 20;
        const std::string big = f.make_sparse("big.bin", size);
        Request r = f.put(server, big, 0, size);
        r.total_timeout_s = 30;
        const auto resp = transport.perform(r);
        PB_CHECK_EQ(resp.status, 0L);
        PB_CHECK(resp.delivery == Delivery::PartlySent);
    }
    // an answer always means the request arrived
    {
        TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string&) { return TlsTestServer::http_response(412, ""); });
        const auto resp = transport.perform(f.put(server, file, 0, 4096));
        PB_CHECK_EQ(resp.status, 412L);
        PB_CHECK(resp.delivery == Delivery::FullySent);
    }
}

PB_TEST(the_write_verbs_and_their_headers_pass_through_unchanged) {
    Fixture f;
    TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string&) { return TlsTestServer::http_response(201, ""); });
    CurlTransport transport(f.pki.ca_cert());
    for (const char* verb : {"MOVE", "COPY", "DELETE", "MKCOL"}) {
        Request r;
        r.method = verb;
        r.url = server.url("/remote.php/dav/files/demo/a%20b");
        r.username = "demo";
        r.password = "app-secret";
        r.headers = {{"Destination", server.url("/remote.php/dav/files/demo/c")}, {"Overwrite", "F"}, {"If-Match", "\"abc123\""}};
        const auto resp = transport.perform(r);
        PB_CHECK_EQ(resp.status, 201L);
    }
    const auto reqs = server.requests();
    PB_CHECK_EQ(reqs.size(), size_t(4));
    const char* verbs[] = {"MOVE", "COPY", "DELETE", "MKCOL"};
    for (size_t i = 0; i < 4; ++i) {
        PB_CHECK(reqs[i].compare(0, std::string(verbs[i]).size() + 1, std::string(verbs[i]) + " ") == 0);
        PB_CHECK(reqs[i].find("/remote.php/dav/files/demo/a%20b HTTP/1.1") != std::string::npos);
        const std::string low = lower(reqs[i]);
        PB_CHECK(low.find("overwrite: f") != std::string::npos);
        PB_CHECK(reqs[i].find("If-Match: \"abc123\"") != std::string::npos);
        PB_CHECK(low.find("destination: https://localhost:") != std::string::npos);
    }
}

PB_TEST(a_redirect_to_another_host_is_refused_for_a_put_and_nothing_is_forwarded) {
    Fixture f;
    TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string&) {
        return TlsTestServer::http_response(307, "", "Location: https://other.example.org/stolen\r\n");
    });
    CurlTransport transport(f.pki.ca_cert());
    const std::string file = f.make_file("src.bin", 1000);
    const auto resp = transport.perform(f.put(server, file, 0, 1000));
    PB_CHECK_EQ(resp.status, 0L);
    PB_CHECK_EQ(resp.error_key, std::string("redirect_other_host"));
    PB_CHECK_EQ(server.requests().size(), size_t(1));   // one request, not repeated anywhere
}

PB_TEST_MAIN()
