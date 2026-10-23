// Budget of the share screens (plan.md "Performance Goals" and "Constraints"; spec SC-007): a list of 200 shares is read and turned into rows quickly, at
// most 500 rows are kept of any answer, and 100 open/leave cycles do not grow the process. Measured on the HOST; the device figures stay UNVERIFIED until
// measured on the InkPad One (docs/device-test-checklist.md DT-L4).
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

#include "fake_nextcloud_server.h"
#include "pbc/dav_common.h"
#include "pbc/share_api.h"
#include "pbc/share_parse.h"
#include "test_support.h"

PBC_FAKE_SERVER_TEST();

namespace {

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

long vm_rss_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.compare(0, 6, "VmRSS:") == 0) return std::atol(line.c_str() + 6);
    return -1;
}

double seconds_since(const std::chrono::steady_clock::time_point& t0) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }

std::string body_of(int n) {
    std::string rows = "[";
    for (int i = 1; i <= n; ++i) {
        rows += (i > 1 ? "," : "");
        rows += "{\"id\":\"" + std::to_string(i) + "\",\"share_type\":3,\"permissions\":1,\"uid_owner\":\"demo\",\"item_type\":\"folder\",\"token\":\"TOK" + std::to_string(i) +
                "\",\"url\":\"https://cloud.example.org/s/TOK" + std::to_string(i) + "\",\"label\":\"Link " + std::to_string(i) + "\",\"password\":null,\"share_with\":null,"
                "\"expiration\":\"2026-12-31 00:00:00\",\"stime\":1790000000,\"mail_send\":0,\"hide_download\":0,\"path\":\"/Docs/Reports\"}";
    }
    return "{\"ocs\":{\"meta\":{\"statuscode\":200},\"data\":" + rows + "]}}";
}

}  // namespace

PB_TEST(two_hundred_shares_are_parsed_within_the_budget_and_the_501st_is_dropped) {
    const std::string body = body_of(200);
    const auto t0 = std::chrono::steady_clock::now();
    pbc::Result<pbc::ShareList> l;
    for (int i = 0; i < 20; ++i) l = pbc::parse_share_list(body, "demo", false, true);
    const double per_parse = seconds_since(t0) / 20;
    PB_CHECK(l.ok());
    PB_CHECK_EQ(l.value.rows.size(), size_t(200));
    std::printf("[budget] parse of a 200-share list (%zu bytes): %.1f ms (budget <= 100 ms on the host; <= 500 ms on the device, UNVERIFIED)\n", body.size(), per_parse * 1000);
    if (!kSanitized) PB_CHECK(per_parse <= 0.1);
    const auto big = pbc::parse_share_list(body_of(501), "demo", false, true);
    PB_CHECK(big.ok());
    PB_CHECK_EQ(big.value.rows.size(), size_t(500));
    PB_CHECK(big.value.truncated);
}

PB_TEST(a_hundred_open_and_leave_cycles_do_not_grow_the_process) {
    pbtest::FakeNextcloud server;
    server.add_folder("/Docs");
    server.add_folder("/Docs/Reports");
    server.add_generated_shares("/Docs/Reports", 200);
    pbc::SystemClock clock;
    pbc::DavSession session(server, server.base_url(), server.login_name(), server.app_password());
    pbc::ShareApi api(session, clock, server.user_id());
    // warm up: the allocator's first pages are not growth
    for (int i = 0; i < 10; ++i) PB_CHECK(api.list({"/Docs/Reports", true}).ok());
    const long before = vm_rss_kb();
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        const auto l = api.list({"/Docs/Reports", true});
        PB_CHECK(l.ok() && l.value.own.rows.size() == 200);
        PB_CHECK(api.detail(l.value.own.rows[0].id, true).ok());   // and a link is read and dropped each time
    }
    const long grown = vm_rss_kb() - before;
    std::printf("[budget] 100 open/leave cycles of a 200-share list: %.2f s through the fake, growth %ld KB (budget <= 1024 KB, UNVERIFIED on the device)\n", seconds_since(t0), grown);
    if (!kSanitized) PB_CHECK(grown <= 1024);
}

PB_TEST_MAIN()
