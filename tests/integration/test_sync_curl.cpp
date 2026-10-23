// A pair download and a pair upload through the real CurlTransport (specs/003-folder-sync FR-016, FR-017, FR-050): libcurl talks TLS to a local server with
// its own CA; behind it sits the fake Nextcloud, so the whole sync - listings, the planned-version GET, the queue's PUT and MKCOL - runs over the real
// transport. The download goes to the pair folder, the upload goes through the queue, and a redirect to another host or port never carries the credentials.
// Needs `openssl`. Nothing here is verified against a real server.
#include <sys/stat.h>

#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "pbc/curl_transport.h"
#include "pbc/queue_sender.h"
#include "pbc/sync_planner.h"
#include "pbc/sync_rules.h"
#include "pbc/sync_runner.h"
#include "server_dump.h"
#include "sync_env.h"
#include "tls_test_server.h"

PBC_FAKE_SERVER_TEST();

using pbtest::pattern;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The raw request of the TLS server as a request of the fake Nextcloud, and the fake's answer as raw bytes.
std::string through_fake(pbtest::FakeNextcloud& fake, const std::string& raw, const std::string& redirect_path, const std::string& redirect_to) {
    const size_t eol = raw.find("\r\n");
    const size_t sp1 = raw.find(' ');
    const size_t sp2 = raw.find(' ', sp1 + 1);
    pbc::Request req;
    req.method = raw.substr(0, sp1);
    const std::string path = raw.substr(sp1 + 1, sp2 - sp1 - 1);
    req.url = fake.base_url() + path;
    const size_t head_end = raw.find("\r\n\r\n");
    size_t pos = eol + 2;
    while (pos < head_end) {
        const size_t e = raw.find("\r\n", pos);
        const std::string line = raw.substr(pos, e - pos);
        pos = e + 2;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = line.substr(0, colon), value = line.substr(colon + 2);
        const std::string l = lower(name);
        if (l == "authorization") {
            req.username = "demo";
            req.password = fake.app_password();
        } else if (l != "host" && l != "content-length" && l != "connection" && l != "user-agent" && l != "accept") {
            req.headers.push_back({name, value});
        }
    }
    req.body = raw.substr(head_end + 4);
    if (!redirect_path.empty() && path.find(redirect_path) != std::string::npos && req.method == "GET")
        return "HTTP/1.1 307 Temporary Redirect\r\nLocation: " + redirect_to + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    const pbc::Response r = fake.perform(req);
    std::string out = "HTTP/1.1 " + std::to_string(r.status) + " X\r\nContent-Length: " + std::to_string(r.body.size()) + "\r\nConnection: close\r\n";
    for (const auto& h : r.headers) {
        if (lower(h.first) == "content-length" || lower(h.first) == "connection") continue;
        out += h.first + ": " + h.second + "\r\n";
    }
    return out + "\r\n" + r.body;
}

struct Over {
    pbtest::TmpDir dir;
    pbtest::TestPki pki{dir};
    pbtest::TestPki::Leaf leaf;
    pbtest::FakeNextcloud fake;
    pbtest::DlClock clock;
    std::unique_ptr<pbc::Store> store;
    pbtest::FakeStorage storage;
    pbtest::SyncRandom random;
    std::string reader;
    std::string redirect_path, redirect_to;
    std::mutex mu;
    std::unique_ptr<pbtest::TlsTestServer> server;
    std::unique_ptr<pbc::CurlTransport> transport;
    int64_t pair = 0;
    pbc::SourceRules rules;
    std::unique_ptr<pbc::Downloader> dl;
    std::unique_ptr<pbc::QueueSender> sender;
    std::unique_ptr<pbc::SyncRunner> runner;

    Over() {
        if (!pki.available()) PB_SKIP("openssl CLI not available");
        leaf = pki.issue("good", "DNS:localhost,IP:127.0.0.1");
        PB_CHECK(leaf.ok);
        clock.t = fake.server_time();
        server = std::make_unique<pbtest::TlsTestServer>(leaf.cert, leaf.key, [this](const std::string& raw) {
            std::lock_guard<std::mutex> lock(mu);
            return through_fake(fake, raw, redirect_path, redirect_to);
        });
        PB_CHECK(server->running());
        transport = std::make_unique<pbc::CurlTransport>(pki.ca_cert());
        auto r = pbc::Store::open(dir.file("data"), clock);
        PB_CHECK(r.ok());
        store = std::move(r.value);
        pbc::AccountRow a;
        a.base_url = "https://localhost:" + std::to_string(server->port());
        a.login_name = a.user_id = "demo";
        a.state = "ok";
        PB_CHECK(store->save_account(a).ok());
        pbc::Credentials c;
        c.password = fake.app_password();
        PB_CHECK(pbc::save_credentials(store->directory(), c).ok());
        pbc::ServerCaps caps;
        caps.trash = true;
        caps.chunking = true;
        PB_CHECK(store->save_server_caps(caps).ok());
        reader = dir.file("Reader");
        ::mkdir(reader.c_str(), 0700);
        fake.add_folder("/Books");
        pbc::SyncPair p = pbc::default_pair(a.base_url, "demo");
        p.remote_folder = "/Books";
        p.local_folder = reader;
        p.direction = pbc::Direction::Both;
        pair = store->sync_pair_add(p).value;
        rules.root = dir.path();
        rules.deny = {dir.file("data")};
        pbc::DownloaderOptions o;
        o.delays_s = {1};
        o.sleep_ms = [](int) {};
        dl = std::make_unique<pbc::Downloader>(*store, *transport, storage, clock, dir.file("PBCloud"), [] { return true; }, o);
        sender = std::make_unique<pbc::QueueSender>(*store, *transport, clock, random, rules, dir.file("PBCloud"), pbc::SenderOptions());
        runner = std::make_unique<pbc::SyncRunner>(*store, *transport, *dl, storage, clock, rules);
        runner->attach_queue(sender.get());
    }
    pbc::SyncResult sync() {
        pbtest::NoCancel c;
        const auto plan = runner->compare(pair, "tap", c);
        if (!plan.ok()) PB_FAIL_AT("compare failed: " + plan.error.key);
        const auto result = runner->run(plan.value, c);
        if (!result.ok()) PB_FAIL_AT("run failed: " + result.error.key);
        return result.value;
    }
};

}  // namespace

PB_TEST(a_pair_download_and_a_pair_upload_run_over_the_real_transport_and_land_where_they_belong) {
    Over e;
    e.fake.add_file("/Books/Novels/from-server.epub", pattern(5000, 1));
    ::mkdir((e.reader + "/Out").c_str(), 0700);
    std::ofstream(e.reader + "/Out/from-reader.txt", std::ios::binary) << pattern(3000, 2);
    const auto result = e.sync();
    PB_CHECK_EQ(result.downloaded, 1);
    PB_CHECK_EQ(result.uploaded, 1);
    PB_CHECK_EQ(result.failed, 0);
    std::ifstream in(e.reader + "/Novels/from-server.epub", std::ios::binary);   // the download went to the pair folder, not to PBCloud/
    const std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    PB_CHECK(got == pattern(5000, 1));
    PB_CHECK(e.fake.node("/Books/Out/from-reader.txt").content == pattern(3000, 2));   // the upload went through the queue
    PB_CHECK_EQ(e.store->queue_count_open(), 0);
    PB_CHECK_EQ(e.store->sync_records(e.pair).size(), size_t(2));
    PB_CHECK(e.fake.count("MKCOL") >= 1);
    // a second sync has nothing to do
    e.fake.clear_log();
    const auto again = e.sync();
    PB_CHECK_EQ(again.downloaded + again.uploaded, 0);
}

PB_TEST(a_redirect_to_another_host_or_port_never_carries_the_credentials) {
    Over e;
    e.fake.add_file("/Books/redir.epub", pattern(2000, 3));
    std::vector<std::string> seen;
    std::mutex seen_mu;
    pbtest::TlsTestServer other(e.leaf.cert, e.leaf.key, [&](const std::string& raw) {
        std::lock_guard<std::mutex> lock(seen_mu);
        seen.push_back(raw);
        return pbtest::TlsTestServer::http_response(200, "not the file");
    });
    PB_CHECK(other.running());
    e.redirect_path = "/Books/redir.epub";
    e.redirect_to = other.url("/stolen");
    pbtest::NoCancel c;
    const auto plan = e.runner->compare(e.pair, "tap", c);
    PB_CHECK(plan.ok());
    const auto result = e.runner->run(plan.value, c);
    PB_CHECK(result.ok());
    PB_CHECK_EQ(result.value.downloaded, 0);   // the bytes that came from elsewhere are not the planned file
    std::lock_guard<std::mutex> lock(seen_mu);
    for (const std::string& raw : seen) PB_CHECK(lower(raw).find("authorization") == std::string::npos);
    std::ifstream f(e.reader + "/redir.epub");
    PB_CHECK(!f.good());
}

PB_TEST_MAIN()
