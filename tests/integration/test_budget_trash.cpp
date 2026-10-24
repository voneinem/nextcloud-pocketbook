// Budget of the trash and versions screens (specs/006-trash-and-versions plan.md "Performance Goals" and "Constraints"; spec SC-007): a trash of 1,000 items is read, kept and
// turned into a list quickly, at most 1,000 rows are kept and stored, a list of 200 versions is parsed quickly, and 100 open and leave cycles do not grow the process.
// Measured on the HOST; the device figures stay UNVERIFIED until measured on the InkPad One (docs/device-test-checklist.md DT-T3).
#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

#include "fake_nextcloud_server.h"
#include "pbc/dav_common.h"
#include "pbc/store.h"
#include "pbc/trash_api.h"
#include "pbc/trash_parse.h"
#include "test_support.h"
#include "tmp_dir.h"

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

std::string trash_body(int n) {
    std::string b = "<?xml version=\"1.0\"?>\n<d:multistatus xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\" xmlns:nc=\"http://nextcloud.org/ns\">"
                    "<d:response><d:href>/remote.php/dav/trashbin/demo/trash/</d:href><d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop>"
                    "<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
    for (int i = 0; i < n; ++i) {
        const std::string id = std::to_string(i);
        b += "<d:response><d:href>/remote.php/dav/trashbin/demo/trash/Document%20number%20" + id + ".txt.d" + std::to_string(1700000000 + i) + "</d:href><d:propstat><d:prop>"
             "<nc:trashbin-filename>Document number " + id + ".txt</nc:trashbin-filename><nc:trashbin-original-location>Projects/2026/Reports/Document number " + id +
             ".txt</nc:trashbin-original-location><nc:trashbin-deletion-time>" + std::to_string(1700000000 + i) + "</nc:trashbin-deletion-time>"
             "<d:getcontentlength>12345</d:getcontentlength><d:resourcetype/><oc:fileid>" + std::to_string(5000 + i) + "</oc:fileid><oc:permissions>GD</oc:permissions>"
             "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
    }
    return b + "</d:multistatus>";
}

std::string versions_body(int n) {
    std::string b = "<?xml version=\"1.0\"?>\n<d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>/remote.php/dav/versions/demo/versions/4713/</d:href><d:propstat><d:prop>"
                    "<d:resourcetype><d:collection/></d:resourcetype></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
    for (int i = 0; i < n; ++i)
        b += "<d:response><d:href>/remote.php/dav/versions/demo/versions/4713/" + std::to_string(1600000000 + i * 10) + "</d:href><d:propstat><d:prop>"
             "<d:getlastmodified>Thu, 09 Oct 2025 08:53:20 GMT</d:getlastmodified><d:getcontentlength>2048</d:getcontentlength><d:getcontenttype>text/plain</d:getcontenttype>"
             "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
    return b + "</d:multistatus>";
}

pbc::Response multistatus(const std::string& body) {
    pbc::Response r;
    r.status = 207;
    r.body = body;
    return r;
}

}  // namespace

PB_TEST(a_thousand_items_are_parsed_stored_and_loaded_within_the_budget) {
    const std::string body = trash_body(1000);
    const pbc::Response r = multistatus(body);
    pbtest::TmpDir dir;
    pbc::SystemClock clock;
    auto opened = pbc::Store::open(dir.file("data"), clock);
    PB_CHECK(opened.ok());
    const auto t0 = std::chrono::steady_clock::now();
    pbc::Result<pbc::TrashList> l;
    for (int i = 0; i < 5; ++i) {
        l = pbc::parse_trash(r, "demo", 1790000000 + 100000000);
        PB_CHECK(l.ok());
        PB_CHECK(opened.value->replace_trash(l.value, 1790000000, 1).ok());
        PB_CHECK(opened.value->load_trash(1).ok());
    }
    const double per_round = seconds_since(t0) / 5;
    PB_CHECK_EQ(l.value.items.size(), size_t(1000));
    std::printf("[budget] parse + store + load of a 1,000-item trash (%zu bytes): %.1f ms (budget <= 150 ms on the host; <= 700 ms on the device, UNVERIFIED)\n", body.size(), per_round * 1000);
    if (!kSanitized) PB_CHECK(per_round <= 0.15);
    // the stored list is bounded: 1,000 rows of this size are well below 1 MB
    struct stat st;
    PB_CHECK(::stat(dir.file("data/pbcloud.db").c_str(), &st) == 0);
    std::printf("[budget] the database with the stored trash list: %lld bytes (budget: the list <= 1 MB, UNVERIFIED on the device)\n", static_cast<long long>(st.st_size));
    PB_CHECK(st.st_size <= 2 * 1024 * 1024);
}

PB_TEST(the_1001st_item_is_cut_and_the_answer_limit_holds) {
    const auto big = pbc::parse_trash(multistatus(trash_body(1200)), "demo", 1790000000 + 100000000);
    PB_CHECK(big.ok());
    PB_CHECK_EQ(big.value.items.size(), pbc::kMaxTrashRows);
    PB_CHECK(big.value.truncated);
    PB_CHECK_EQ(big.value.total_seen, 1200);
}

PB_TEST(two_hundred_versions_are_parsed_within_the_budget_and_the_201st_is_cut) {
    const std::string body = versions_body(200);
    const pbc::Response r = multistatus(body);
    const auto t0 = std::chrono::steady_clock::now();
    pbc::Result<pbc::VersionList> l;
    for (int i = 0; i < 20; ++i) l = pbc::parse_versions(r, 1790000000 + 100000000);
    const double per_parse = seconds_since(t0) / 20;
    PB_CHECK(l.ok());
    PB_CHECK_EQ(l.value.rows.size(), size_t(200));
    std::printf("[budget] parse of a 200-version list (%zu bytes): %.1f ms (budget <= 50 ms on the host; <= 300 ms on the device, UNVERIFIED)\n", body.size(), per_parse * 1000);
    if (!kSanitized) PB_CHECK(per_parse <= 0.05);
    const auto big = pbc::parse_versions(multistatus(versions_body(201)), 1790000000 + 100000000);
    PB_CHECK(big.ok());
    PB_CHECK_EQ(big.value.rows.size(), size_t(200));
    PB_CHECK(big.value.truncated);
}

PB_TEST(a_hundred_open_and_leave_cycles_do_not_grow_the_process) {
    pbtest::FakeNextcloud server;
    server.set_account("demo", "demo", "pw");
    for (int i = 0; i < 300; ++i) server.add_trash_file("/F/f" + std::to_string(i) + ".txt", "x", 1792497000 - i);
    pbc::SystemClock clock;
    pbc::DavSession session(server, server.base_url(), "demo", "pw");
    pbc::TrashApi api(session, "demo", clock);
    for (int i = 0; i < 10; ++i) PB_CHECK(api.list_trash().ok());   // warm up: the allocator's first pages are not growth
    const long before = vm_rss_kb();
    for (int i = 0; i < 100; ++i) PB_CHECK(api.list_trash().ok());
    const long after = vm_rss_kb();
    std::printf("[budget] 100 trash reads: RSS %ld KB -> %ld KB (budget: the state <= 1.5 MB, UNVERIFIED on the device)\n", before, after);
    if (!kSanitized && before > 0 && after > 0) PB_CHECK(after - before <= 4096);
}

PB_TEST_MAIN()
