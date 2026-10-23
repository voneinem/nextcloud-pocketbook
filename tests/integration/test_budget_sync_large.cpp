// Budget of a large file in a folder sync (specs/003-folder-sync SC-011, plan.md "Constraints"): a 500 MB file goes through SyncRunner in either direction
// without the process memory growing with it, and a transfer that is stopped in the middle is repeated by the next sync and ends complete. The download runs
// over the real CurlTransport against a TLS server that generates the bytes while it sends them (the fake Nextcloud would hold the body in memory); the upload
// runs against the in-process fake, which hashes and counts the chunks and keeps none of them. Measured on the HOST. The device budget (500 MB, 150 MB peak)
// stays UNVERIFIED until measured on the InkPad One (docs/device-test-checklist.md DT-S6). The download needs `openssl`.
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

#include "pbc/curl_transport.h"
#include "sync_env.h"
#include "tls_test_server.h"

PBC_FAKE_SERVER_TEST();

namespace {

// Under the sanitizers the shadow memory counts into the peak: the memory budget is only asserted in a normal build.
#if defined(__SANITIZE_ADDRESS__)
constexpr bool kSanitized = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif
#else
constexpr bool kSanitized = false;
#endif

constexpr long long kSize = 500ll << 20;   // 500 MB
constexpr unsigned char kSeed = 9;

long vm_hwm_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.compare(0, 6, "VmHWM:") == 0) return std::atol(line.c_str() + 6);
    return -1;
}

bool enough_space() {
    struct statvfs vfs;
    return ::statvfs("/tmp", &vfs) == 0 && static_cast<long long>(vfs.f_bavail) * static_cast<long long>(vfs.f_frsize) >= 3ll << 30;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The raw request of the TLS server as a request of the fake Nextcloud, and the fake's answer as raw bytes.
std::string through_fake(pbtest::FakeNextcloud& fake, const std::string& raw) {
    const size_t eol = raw.find("\r\n");
    const size_t sp1 = raw.find(' ');
    const size_t sp2 = raw.find(' ', sp1 + 1);
    pbc::Request req;
    req.method = raw.substr(0, sp1);
    req.url = fake.base_url() + raw.substr(sp1 + 1, sp2 - sp1 - 1);
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
    const pbc::Response r = fake.perform(req);
    std::string out = "HTTP/1.1 " + std::to_string(r.status) + " X\r\nContent-Length: " + std::to_string(r.body.size()) + "\r\nConnection: close\r\n";
    for (const auto& h : r.headers) {
        if (lower(h.first) == "content-length" || lower(h.first) == "connection") continue;
        out += h.first + ": " + h.second + "\r\n";
    }
    return out + "\r\n" + r.body;
}

// The file the server holds, byte i = (i * 31 + seed) & 0xff, compared with the one on the reader in 1 MiB steps.
bool file_is_generated(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::string buf(1 << 20, '\0');
    long long at = 0;
    while (in.read(&buf[0], static_cast<std::streamsize>(buf.size())) || in.gcount() > 0) {
        const size_t n = static_cast<size_t>(in.gcount());
        for (size_t i = 0; i < n; ++i)
            if (static_cast<unsigned char>(buf[i]) != static_cast<unsigned char>(((at + static_cast<long long>(i)) * 31 + kSeed) & 0xff)) return false;
        at += static_cast<long long>(n);
    }
    return at == kSize;
}

struct CancelAt : pbc::Cancel {
    bool flag = false;
    bool requested() const override { return flag; }
};

}  // namespace

PB_TEST(a_500_mb_file_is_downloaded_by_a_sync_without_growing_memory_and_an_interrupted_one_ends_complete) {
    if (!enough_space()) PB_SKIP("less than 3 GiB free in /tmp");
    pbtest::TmpDir dir;
    pbtest::TestPki pki(dir);
    if (!pki.available()) PB_SKIP("openssl CLI not available");
    const auto leaf = pki.issue("good", "DNS:localhost,IP:127.0.0.1");
    PB_CHECK(leaf.ok);
    pbtest::FakeNextcloud fake;
    fake.add_folder("/Books");
    fake.add_synthetic_file("/Books/big.bin", kSize, kSeed);
    const std::string etag = fake.etag_of("/Books/big.bin");
    std::mutex mu;
    int gets = 0;
    pbtest::TlsTestServer server(leaf.cert, leaf.key, [&](const std::string& raw, const std::function<bool(const char*, size_t)>& write) {
        if (raw.compare(0, 4, "GET ") == 0 && raw.find("/Books/big.bin") != std::string::npos) {   // the bytes are made while they are sent; a Range is not honoured
            {
                std::lock_guard<std::mutex> lock(mu);
                ++gets;
            }
            const std::string head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(kSize) + "\r\nETag: " + etag + "\r\nConnection: close\r\n\r\n";
            if (!write(head.data(), head.size())) return;
            std::string chunk(1 << 20, '\0');
            for (long long sent = 0; sent < kSize; sent += static_cast<long long>(chunk.size())) {
                for (size_t i = 0; i < chunk.size(); ++i) chunk[i] = static_cast<char>((((sent + static_cast<long long>(i)) * 31) + kSeed) & 0xff);
                if (!write(chunk.data(), chunk.size())) return;
            }
            return;
        }
        std::lock_guard<std::mutex> lock(mu);
        const std::string answer = through_fake(fake, raw);
        write(answer.data(), answer.size());
    });
    PB_CHECK(server.running());
    pbc::CurlTransport transport(pki.ca_cert());

    pbtest::DlClock clock;
    clock.t = fake.server_time();
    auto opened = pbc::Store::open(dir.file("data"), clock);
    PB_CHECK(opened.ok());
    auto store = std::move(opened.value);
    pbc::AccountRow a;
    a.base_url = "https://localhost:" + std::to_string(server.port());
    a.login_name = a.user_id = "demo";
    a.state = "ok";
    PB_CHECK(store->save_account(a).ok());
    pbc::Credentials c;
    c.password = fake.app_password();
    PB_CHECK(pbc::save_credentials(store->directory(), c).ok());
    const std::string reader = dir.file("Reader");
    ::mkdir(reader.c_str(), 0700);
    pbc::SyncPair p = pbc::default_pair(a.base_url, "demo");
    p.remote_folder = "/Books";
    p.local_folder = reader;
    p.max_bytes = 2ll << 30;
    const int64_t pair = store->sync_pair_add(p).value;
    pbtest::FakeStorage storage;
    pbc::SourceRules rules;
    rules.root = dir.path();
    rules.deny = {dir.file("data")};
    pbc::DownloaderOptions o;
    o.delays_s = {1};
    o.sleep_ms = [](int) {};
    pbc::Downloader dl(*store, transport, storage, clock, dir.file("PBCloud"), [] { return true; }, o);
    CancelAt cancel;
    int reports = 0;
    pbc::RunnerOptions ro;
    ro.on_file_progress = [&](const pbc::JobProgress& pr) {
        ++reports;
        if (pr.total > 0 && pr.done >= pr.total / 2) cancel.flag = true;   // the run is stopped in the middle of the file
    };
    pbc::SyncRunner runner(*store, transport, dl, storage, clock, rules, ro);

    const long before = vm_hwm_kb();
    const auto t0 = std::chrono::steady_clock::now();
    const auto plan1 = runner.compare(pair, "tap", cancel);
    PB_CHECK(plan1.ok());
    PB_CHECK_EQ(store->sync_plan(plan1.value)->totals.n_download, 1);
    const auto first = runner.run(plan1.value, cancel);
    PB_CHECK(first.ok());
    PB_CHECK_EQ(first.value.downloaded, 0);   // stopped: the file has no final name and no record
    std::ifstream none(reader + "/big.bin");
    PB_CHECK(!none.good());
    PB_CHECK_EQ(store->sync_records(pair).size(), size_t(0));

    cancel.flag = false;
    pbc::SyncRunner again(*store, transport, dl, storage, clock, rules);   // the next sync, as after a restart
    const auto plan2 = again.compare(pair, "tap", cancel);
    PB_CHECK(plan2.ok());
    const auto second = again.run(plan2.value, cancel);
    PB_CHECK(second.ok());
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const long after = vm_hwm_kb();
    PB_CHECK_EQ(second.value.downloaded, 1);
    PB_CHECK_EQ(second.value.failed, 0);
    struct stat st;
    PB_CHECK(::stat((reader + "/big.bin").c_str(), &st) == 0);
    PB_CHECK_EQ(static_cast<long long>(st.st_size), kSize);
    PB_CHECK(file_is_generated(reader + "/big.bin"));
    PB_CHECK_EQ(store->sync_records(pair).size(), size_t(1));
    {
        std::lock_guard<std::mutex> lock(mu);
        PB_CHECK(gets >= 2);
    }
    std::printf("[budget] sync download of 500 MB, stopped at half and repeated: %.1f s, peak memory %ld MB (was %ld MB), %d progress reports of the first run\n", seconds,
                after / 1024, before / 1024, reports);
    PB_CHECK(kSanitized || after < 150 * 1024);
    PB_CHECK(kSanitized || after - before < 100 * 1024);
}

PB_TEST(a_500_mb_file_is_uploaded_by_a_sync_without_growing_memory_and_an_interrupted_one_ends_complete) {
    if (!enough_space()) PB_SKIP("less than 3 GiB free in /tmp");
    pbtest::SyncEnv e;
    e.server.add_folder("/Books");
    e.server.set_keep_bytes_limit(1 << 20);   // the fake hashes and counts what it receives and keeps none of it
    pbc::ServerCaps caps;
    caps.trash = true;
    caps.chunking = true;
    PB_CHECK(e.store->save_server_caps(caps).ok());
    e.server.set_chunking(true);
    e.direction(pbc::Direction::Up);
    auto pr = e.store->sync_pair(e.pair);
    pr->max_bytes = 2ll << 30;
    PB_CHECK(e.store->sync_pair_update(*pr).ok());
    {   // a sparse file: nothing of it is on the disk
        const int fd = ::open((e.reader + "/big.bin").c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        PB_CHECK(fd >= 0);
        PB_CHECK(::ftruncate(fd, static_cast<off_t>(kSize)) == 0);
        ::close(fd);
    }
    CancelAt cancel;
    int chunks = 0;
    e.on_sender_step = [&](const char* step) {
        if (std::string(step) == "chunk" && ++chunks == 20) cancel.flag = true;   // the run is stopped in the middle of the file
    };
    const long before = vm_hwm_kb();
    const auto t0 = std::chrono::steady_clock::now();
    const auto plan1 = e.runner->compare(e.pair, "tap", cancel);
    PB_CHECK(plan1.ok());
    PB_CHECK_EQ(e.store->sync_plan(plan1.value)->totals.n_upload, 1);
    const auto first = e.runner->run(plan1.value, cancel);
    PB_CHECK(first.ok());
    PB_CHECK_EQ(first.value.uploaded, 0);
    PB_CHECK(!e.server.has("/Books/big.bin"));   // nothing is assembled before the last chunk is there
    cancel.flag = false;
    e.on_sender_step = nullptr;
    // the next sync leaves the file alone while its queue item is open (the item is the user's to send); sending it continues or restarts the upload
    const auto plan2 = e.runner->compare(e.pair, "tap", cancel);
    PB_CHECK(plan2.ok());
    PB_CHECK_EQ(e.store->sync_plan(plan2.value)->totals.n_upload, 0);
    PB_CHECK_EQ(e.run_ok(plan2.value).uploaded, 0);
    PB_CHECK_EQ(e.store->queue_count_open(), 1);
    const pbc::SendReport rep = e.sender->run(cancel);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const long after = vm_hwm_kb();
    PB_CHECK_EQ(rep.done, 1);
    PB_CHECK_EQ(rep.failed, 0);
    PB_CHECK_EQ(e.server.node("/Books/big.bin").size(), kSize);
    PB_CHECK_EQ(e.store->queue_count_open(), 0);
    PB_CHECK_EQ(e.store->sync_records(e.pair).size(), size_t(1));   // the queue wrote the record when the file was complete
    PB_CHECK_EQ(e.sync().uploaded, 0);                               // and a sync after it has nothing to do
    struct stat st;
    PB_CHECK(::stat((e.reader + "/big.bin").c_str(), &st) == 0);   // the source stays
    PB_CHECK_EQ(static_cast<long long>(st.st_size), kSize);
    std::printf("[budget] sync upload of 500 MB, stopped in the middle and sent again from the queue: %.1f s, peak memory %ld MB (was %ld MB), %.0f MB sent in all\n", seconds, after / 1024,
                before / 1024, static_cast<double>(e.server.bytes_uploaded()) / (1 << 20));
    PB_CHECK(kSanitized || after < 150 * 1024);
    PB_CHECK(kSanitized || after - before < 100 * 1024);
}

PB_TEST_MAIN()
