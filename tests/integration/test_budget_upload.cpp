// Budget of a large upload (plan.md "Constraints"; spec SC-004): a 1 GiB file goes to the server in chunks read from the file in small pieces, the
// process memory does not grow with the file, the checksum pass works in 64 KiB steps and the screen is told very little. Measured on the HOST with
// the in-process fake (the transport's own streaming is measured by test_upload_curl). The device budget (500 MB, 150 MB peak) stays UNVERIFIED until
// measured on the InkPad One (docs/device-test-checklist.md DT-W3, DT-W4, DT-W7).
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

#include "upload_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::UpEnv;

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

long vm_hwm_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.compare(0, 6, "VmHWM:") == 0) return std::atol(line.c_str() + 6);
    return -1;
}

constexpr long long kSize = 1ll << 30;   // 1 GiB

}  // namespace

PB_TEST(a_1_gib_file_is_sent_in_chunks_with_a_small_memory_footprint_and_few_progress_reports) {
    struct statvfs vfs;
    if (::statvfs("/tmp", &vfs) != 0) PB_SKIP("no /tmp");
    UpEnv e;
    e.server.set_keep_bytes_limit(1 << 20);   // the fake hashes and counts what it receives and keeps none of it
    const std::string src = e.root + "/Downloads/big.bin";
    {   // a sparse file: nothing of it is on the disk
        const int fd = ::open(src.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        PB_CHECK(fd >= 0);
        PB_CHECK(::ftruncate(fd, static_cast<off_t>(kSize)) == 0);
        ::close(fd);
    }
    e.queue({src});
    const long before = vm_hwm_kb();
    const auto t0 = std::chrono::steady_clock::now();
    const pbc::SendReport rep = e.run();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const long after = vm_hwm_kb();
    PB_CHECK_EQ(rep.done, 1);
    PB_CHECK_EQ(e.server.node("/Docs/big.bin").size(), kSize);
    PB_CHECK_EQ(e.server.bytes_uploaded(), kSize);
    // the memory budget: 150 MB peak for the whole process (plan.md), whatever the size of the file
    PB_CHECK(kSanitized || after < 150 * 1024);
    PB_CHECK(kSanitized || after - before < 100 * 1024);
    // the reports to the screen: every 64 KiB piece of the checksum pass and every piece of a chunk goes to the caller, who keeps it coarse; here only
    // the number of chunks and the steps of the pass are checked
    int chunk_reports = 0;
    for (const auto& p : e.progress)
        if (p.step == "chunks") ++chunk_reports;
    PB_CHECK(chunk_reports >= 103);   // 103 chunks of 10 MiB
    std::printf("BUDGET (host, unverified for the device): 1 GiB uploaded in %.1f s, peak memory %ld MB (was %ld MB), %zu progress callbacks of the sender\n", seconds,
                after / 1024, before / 1024, e.progress.size());
}

PB_TEST(a_file_is_hashed_in_steps_of_64_kib) {
    UpEnv e;
    const std::string src = e.source("Downloads/five.bin", 5 << 20, 3);
    pbtest::UpNoCancel cancel;
    int steps = 0;
    long long last = 0;
    const auto h = pbc::hash_source(src, cancel, [&](long long done, long long total) {
        ++steps;
        last = done;
        PB_CHECK_EQ(total, 5ll << 20);
    });
    PB_CHECK(h.ok());
    PB_CHECK_EQ(steps, 80);   // 5 MiB / 64 KiB
    PB_CHECK_EQ(last, 5ll << 20);
}

PB_TEST_MAIN()
