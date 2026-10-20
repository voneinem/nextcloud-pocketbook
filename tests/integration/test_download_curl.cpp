// The download engine on the real libcurl transport against a local TLS server (spec SC-006, SC-007; research R-8): streaming to disk,
// identity encoding, resume with a ranged request, the progress/cancel callback, and the refusal of a redirect to another host. What the
// fake server cannot show: how libcurl itself behaves. Needs `openssl`.
#include <sys/stat.h>

#include <atomic>
#include <fstream>
#include <string>

#include "download_env.h"
#include "pbc/curl_transport.h"
#include "tls_test_server.h"

using pbc::JobState;

namespace {

std::string pattern(size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 31 + 7) & 0xff);
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string header_of(const std::string& req, const std::string& name) {
    const std::string low = lower(req.substr(0, req.find("\r\n\r\n")));
    const size_t p = low.find("\r\n" + lower(name) + ":");
    if (p == std::string::npos) return "";
    const size_t start = p + 2 + name.size() + 1;
    size_t end = low.find("\r\n", start);
    if (end == std::string::npos) end = low.size();   // the last header of the block
    std::string v = req.substr(start, end - start);
    while (!v.empty() && v.front() == ' ') v.erase(0, 1);
    return v;
}

// A tiny WebDAV file server: GET with ETag, Range and If-Range on one file.
struct FileServer {
    std::string content;
    std::string etag = "\"etag-1\"";
    std::atomic<int> gets{0};
    std::atomic<bool> lie_once{false};   // announce the full length, send 30 %: a connection that ends early
    std::vector<std::string> ranges, if_ranges, encodings;

    std::string handle(const std::string& req) {
        if (req.compare(0, 4, "GET ") != 0) return pbtest::TlsTestServer::http_response(404, "");
        ++gets;
        ranges.push_back(header_of(req, "Range"));
        if_ranges.push_back(header_of(req, "If-Range"));
        encodings.push_back(header_of(req, "Accept-Encoding"));
        const std::string range = header_of(req, "Range");
        const std::string if_range = header_of(req, "If-Range");
        if (!range.empty() && range.compare(0, 6, "bytes=") == 0 && (if_range.empty() || if_range == etag)) {
            const size_t from = static_cast<size_t>(std::atoll(range.c_str() + 6));
            if (from >= content.size()) return pbtest::TlsTestServer::http_response(416, "", "Content-Range: bytes */" + std::to_string(content.size()) + "\r\n");
            const std::string body = content.substr(from);
            return "HTTP/1.1 206 Partial Content\r\nContent-Length: " + std::to_string(body.size()) + "\r\nContent-Range: bytes " + std::to_string(from) + "-" +
                   std::to_string(content.size() - 1) + "/" + std::to_string(content.size()) + "\r\nETag: " + etag + "\r\nConnection: close\r\n\r\n" + body;
        }
        if (lie_once.exchange(false)) {
            const std::string part = content.substr(0, content.size() * 3 / 10);
            return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(content.size()) + "\r\nETag: " + etag + "\r\nConnection: close\r\n\r\n" + part;
        }
        return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(content.size()) + "\r\nETag: " + etag + "\r\nConnection: close\r\n\r\n" + content;
    }
};

struct Env {
    pbtest::TmpDir dir;
    pbtest::TestPki pki{dir};
    pbtest::TestPki::Leaf leaf;
    Env() {
        if (!pki.available()) PB_SKIP("openssl CLI not available");
        leaf = pki.issue("good", "DNS:localhost,IP:127.0.0.1");
        PB_CHECK(leaf.ok);
    }
};

struct Setup {
    pbtest::TmpDir data;
    std::string root;
    pbtest::DlClock clock;
    std::unique_ptr<pbc::Store> store;
    pbtest::FakeStorage storage;
    Setup(const std::string& base_url) {
        auto r = pbc::Store::open(data.file("data"), clock);
        PB_CHECK(r.ok());
        store = std::move(r.value);
        pbc::AccountRow a;
        a.base_url = base_url;
        a.login_name = "demo";
        a.user_id = "demo";
        a.state = "ok";
        PB_CHECK(store->save_account(a).ok());
        pbc::Credentials c;
        c.password = "app-pass";
        PB_CHECK(pbc::save_credentials(store->directory(), c).ok());
        root = data.file("PBCloud");
        ::mkdir(root.c_str(), 0700);
    }
};

}  // namespace

PB_TEST(a_file_is_streamed_to_disk_as_stored_and_arrives_identical) {
    Env e;
    FileServer fs;
    fs.content = pattern(3 * 1024 * 1024);
    pbtest::TlsTestServer server(e.leaf.cert, e.leaf.key, [&](const std::string& r) { return fs.handle(r); });
    pbc::CurlTransport transport(e.pki.ca_cert());
    Setup s("https://localhost:" + std::to_string(server.port()));
    pbc::Downloader dl(*s.store, transport, s.storage, s.clock, s.root, [] { return true; }, pbc::DownloaderOptions());
    pbc::DownloadRequest r;
    r.remote_path = "/Books/big.bin";
    r.size = static_cast<int64_t>(fs.content.size());
    pbtest::NoCancel cancel;
    const auto out = dl.run(r, cancel, [](const pbc::JobProgress&) {});
    PB_CHECK(out.state == JobState::Done);
    std::ifstream in(s.root + "/Books/big.bin", std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    PB_CHECK(got == fs.content);
    PB_CHECK_EQ(fs.encodings[0], std::string("identity"));
    PB_CHECK_EQ(s.store->local_file("/Books/big.bin")->remote_etag, std::string("\"etag-1\""));
    PB_CHECK(s.store->journal().empty());
}

PB_TEST(a_connection_that_ends_early_is_continued_with_a_206_and_the_file_is_identical) {
    Env e;
    FileServer fs;
    fs.content = pattern(2 * 1024 * 1024);
    fs.lie_once = true;
    pbtest::TlsTestServer server(e.leaf.cert, e.leaf.key, [&](const std::string& r) { return fs.handle(r); });
    pbc::CurlTransport transport(e.pki.ca_cert());
    Setup s("https://localhost:" + std::to_string(server.port()));
    pbc::DownloaderOptions o;
    o.sleep_ms = [](int) {};
    o.delays_s = {1, 1, 1};
    pbc::Downloader dl(*s.store, transport, s.storage, s.clock, s.root, [] { return true; }, o);
    pbc::DownloadRequest r;
    r.remote_path = "/f.bin";
    r.size = static_cast<int64_t>(fs.content.size());
    pbtest::NoCancel cancel;
    const auto out = dl.run(r, cancel, [](const pbc::JobProgress&) {});
    PB_CHECK(out.state == JobState::Done);
    std::ifstream in(s.root + "/f.bin", std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    PB_CHECK(got == fs.content);
    PB_CHECK_EQ(fs.gets.load(), 2);
    PB_CHECK(fs.ranges[1].compare(0, 6, "bytes=") == 0);    // the second request continues
    PB_CHECK_EQ(fs.if_ranges[1], std::string("\"etag-1\""));
}

PB_TEST(the_progress_callback_cancels_a_running_transfer_and_nothing_stays) {
    Env e;
    FileServer fs;
    fs.content = pattern(4 * 1024 * 1024);
    pbtest::TlsTestServer server(e.leaf.cert, e.leaf.key, [&](const std::string& r) { return fs.handle(r); });
    pbc::CurlTransport transport(e.pki.ca_cert());
    Setup s("https://localhost:" + std::to_string(server.port()));
    pbc::Downloader dl(*s.store, transport, s.storage, s.clock, s.root, [] { return true; }, pbc::DownloaderOptions());
    pbc::DownloadRequest r;
    r.remote_path = "/f.bin";
    r.size = static_cast<int64_t>(fs.content.size());
    struct After : pbc::Cancel {
        mutable int polls = 0;
        bool requested() const override { return ++polls > 5; }
    } cancel;
    const auto out = dl.run(r, cancel, [](const pbc::JobProgress&) {});
    PB_CHECK(out.state == JobState::Cancelled);
    std::vector<std::string> files;
    pbtest::list_tree(s.root, "", &files);
    PB_CHECK(files.empty());
    PB_CHECK(s.store->journal().empty());
}

PB_TEST(a_redirect_to_another_host_is_refused_and_ends_the_job) {
    Env e;
    FileServer other;
    other.content = "other";
    pbtest::TlsTestServer b(e.leaf.cert, e.leaf.key, [&](const std::string& r) { return other.handle(r); });
    const std::string target = b.url("/f.bin");
    pbtest::TlsTestServer a(e.leaf.cert, e.leaf.key, [&](const std::string&) { return pbtest::TlsTestServer::http_response(307, "", "Location: " + target + "\r\n"); });
    pbc::CurlTransport transport(e.pki.ca_cert());
    Setup s("https://localhost:" + std::to_string(a.port()));
    pbc::Downloader dl(*s.store, transport, s.storage, s.clock, s.root, [] { return true; }, pbc::DownloaderOptions());
    pbc::DownloadRequest r;
    r.remote_path = "/f.bin";
    pbtest::NoCancel cancel;
    const auto out = dl.run(r, cancel, [](const pbc::JobProgress&) {});
    PB_CHECK(out.state == JobState::Failed);
    PB_CHECK_EQ(out.error_key, std::string("redirect_other_host"));
    PB_CHECK(b.requests().empty());
    PB_CHECK(!b.saw_authorization());
    PB_CHECK(s.store->journal().empty());
}

PB_TEST(a_server_with_an_untrusted_certificate_ends_the_job_without_waiting) {
    Env e;
    FileServer fs;
    fs.content = "x";
    pbtest::TestPki other_pki(e.dir, "other");   // a different authority: the server's certificate is not trusted
    PB_CHECK(other_pki.available());
    auto bad = other_pki.issue("bad", "DNS:localhost,IP:127.0.0.1");
    PB_CHECK(bad.ok);
    pbtest::TlsTestServer server(bad.cert, bad.key, [&](const std::string& r) { return fs.handle(r); });
    pbc::CurlTransport transport(e.pki.ca_cert());
    Setup s("https://localhost:" + std::to_string(server.port()));
    int waited = 0;
    pbc::DownloaderOptions o;
    o.sleep_ms = [&](int) { ++waited; };
    pbc::Downloader dl(*s.store, transport, s.storage, s.clock, s.root, [] { return true; }, o);
    pbc::DownloadRequest r;
    r.remote_path = "/f.bin";
    pbtest::NoCancel cancel;
    const auto out = dl.run(r, cancel, [](const pbc::JobProgress&) {});
    PB_CHECK(out.state == JobState::Failed);
    PB_CHECK_EQ(out.error_key, std::string("cert_untrusted"));
    PB_CHECK_EQ(waited, 0);
}

PB_TEST_MAIN()
