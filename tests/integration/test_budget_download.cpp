// Budget of a large download (plan.md "Constraints"; spec SC-007): a 1 GiB file streams through the real transport with a small fixed
// buffer, the process memory does not grow with the file, and the screen is told very little. Measured on the HOST. The device budget
// (500 MB, 150 MB peak) stays UNVERIFIED until measured on the InkPad One (docs/device-test-checklist.md DT-NC-7, DT-NC-10). Needs `openssl`.
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

#include "download_env.h"
#include "pbc/curl_transport.h"
#include "tls_test_server.h"

namespace {

long vm_hwm_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.compare(0, 6, "VmHWM:") == 0) return std::atol(line.c_str() + 6);
    return -1;
}

constexpr long long kSize = 1ll << 30;   // 1 GiB

}  // namespace

PB_TEST(a_1_gib_file_streams_to_disk_with_a_small_memory_footprint_and_few_progress_reports) {
    pbtest::TmpDir dir;
    pbtest::TestPki pki(dir);
    if (!pki.available()) PB_SKIP("openssl CLI not available");
    struct statvfs vfs;
    if (::statvfs("/tmp", &vfs) != 0 || static_cast<long long>(vfs.f_bavail) * static_cast<long long>(vfs.f_frsize) < 3ll << 30) PB_SKIP("less than 3 GiB free in /tmp");
    const auto leaf = pki.issue("good", "DNS:localhost,IP:127.0.0.1");
    PB_CHECK(leaf.ok);

    // the server generates the bytes while it sends them: nothing of the file is held in memory
    pbtest::TlsTestServer server(leaf.cert, leaf.key, [](const std::string&, const std::function<bool(const char*, size_t)>& write) {
        const std::string head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(kSize) + "\r\nETag: \"big\"\r\nConnection: close\r\n\r\n";
        if (!write(head.data(), head.size())) return;
        std::string chunk(1 << 20, '\0');
        for (long long sent = 0; sent < kSize; sent += static_cast<long long>(chunk.size())) {
            for (size_t i = 0; i < chunk.size(); ++i) chunk[i] = static_cast<char>(((static_cast<size_t>(sent) + i) * 31 + 7) & 0xff);
            if (!write(chunk.data(), chunk.size())) return;
        }
    });
    PB_CHECK(server.running());
    pbc::CurlTransport transport(pki.ca_cert());

    pbtest::TmpDir data;
    pbtest::DlClock clock;
    auto opened = pbc::Store::open(data.file("data"), clock);
    PB_CHECK(opened.ok());
    auto store = std::move(opened.value);
    pbc::AccountRow a;
    a.base_url = "https://localhost:" + std::to_string(server.port());
    a.login_name = "demo";
    a.user_id = "demo";
    a.state = "ok";
    PB_CHECK(store->save_account(a).ok());
    pbc::Credentials c;
    c.password = "app-pass";
    PB_CHECK(pbc::save_credentials(store->directory(), c).ok());
    const std::string root = data.file("PBCloud");
    ::mkdir(root.c_str(), 0700);
    pbtest::FakeStorage storage;
    pbc::Downloader dl(*store, transport, storage, clock, root, [] { return true; }, pbc::DownloaderOptions());

    const long before = vm_hwm_kb();
    int reports = 0;
    const auto t0 = std::chrono::steady_clock::now();
    pbc::DownloadRequest r;
    r.remote_path = "/big.bin";
    r.size = kSize;
    pbtest::NoCancel cancel;
    const auto out = dl.run(r, cancel, [&](const pbc::JobProgress&) { ++reports; });
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const long after = vm_hwm_kb();

    PB_CHECK(out.state == pbc::JobState::Done);
    struct stat st;
    PB_CHECK(::stat((root + "/big.bin").c_str(), &st) == 0);
    PB_CHECK_EQ(static_cast<long long>(st.st_size), kSize);
    // the first and the last bytes are the generator's
    std::ifstream in(root + "/big.bin", std::ios::binary);
    char buf[16];
    in.read(buf, 16);
    PB_CHECK_EQ(static_cast<unsigned char>(buf[1]), static_cast<unsigned char>((1 * 31 + 7) & 0xff));
    in.seekg(kSize - 16);
    in.read(buf, 16);
    PB_CHECK_EQ(static_cast<unsigned char>(buf[15]), static_cast<unsigned char>(((static_cast<size_t>(kSize) - 1) * 31 + 7) & 0xff));

    std::printf("BUDGET (host, unverified for the device): 1 GiB in %.1f s, peak memory %ld MB (was %ld MB), %d progress reports\n", seconds, after / 1024,
                before / 1024, reports);
    PB_CHECK(after / 1024 <= 150);                 // the whole process stays within the budget
    PB_CHECK((after - before) / 1024 <= 100);      // and it did not grow with the file
    PB_CHECK(reports <= 40);                       // at most one report per 5 % (about 21), plus the start and the end
}

PB_TEST_MAIN()
