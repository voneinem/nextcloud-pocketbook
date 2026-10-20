// Budgets of the folder cache (plan.md "Performance Goals"; spec SC-004, SC-005, SC-014): opening a cached folder of 1,000 entries,
// turning a page, the cost of parsing and storing a 1,000-entry answer, and the size of the database at the cache limits.
// The figures are measured on the HOST. They are budgets for the InkPad One and stay UNVERIFIED until measured there (docs/device-test-checklist.md DT-NC-7).
#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <string>

#include "fake_nextcloud_server.h"
#include "pbc/credentials_file.h"
#include "pbc/folder_service.h"
#include "pbc/store.h"
#include "test_support.h"
#include "tmp_dir.h"

PBC_FAKE_SERVER_TEST();

namespace {

struct FixedClock : pbc::Clock {
    int64_t t;
    explicit FixedClock(int64_t v) : t(v) {}
    int64_t now() const override { return t; }
};

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

PB_TEST(a_cached_folder_of_1000_entries_opens_within_2_seconds_and_a_page_turn_within_1_second) {
    pbtest::TmpDir dir;
    pbtest::FakeNextcloud server;
    FixedClock clock(server.server_time());
    for (int i = 0; i < 1000; ++i) server.add_file("/Many/book " + std::to_string(i) + ".epub", "x", 1790000000 + i);
    auto opened = pbc::Store::open(dir.file("data"), clock);
    PB_CHECK(opened.ok());
    auto store = std::move(opened.value);
    pbc::AccountRow a;
    a.base_url = server.base_url();
    a.login_name = "demo";
    a.user_id = "demo";
    a.state = "ok";
    PB_CHECK(store->save_account(a).ok());
    pbc::Credentials c;
    c.password = server.app_password();
    PB_CHECK(pbc::save_credentials(store->directory(), c).ok());
    pbc::FolderService svc(*store, server, clock);

    // SC-005 (host part): parsing and storing a 1,000-entry answer
    auto t0 = std::chrono::steady_clock::now();
    PB_CHECK(svc.load("/Many", pbc::Fetch::IfMissing).error.ok());
    const double fetch_s = seconds_since(t0);

    // SC-004: opening the cached folder is a read of the first page; turning a page is another one
    t0 = std::chrono::steady_clock::now();
    PB_CHECK(store->folder("/Many").has_value());
    auto page = store->entries_page("/Many", pbc::SortKey::Name, true, 0, 12);
    const double open_s = seconds_since(t0);
    t0 = std::chrono::steady_clock::now();
    page = store->entries_page("/Many", pbc::SortKey::Date, false, 600, 12);
    const double turn_s = seconds_since(t0);
    PB_CHECK_EQ(page.total, 1000);
    PB_CHECK_EQ(page.rows.size(), size_t(12));
    std::printf("BUDGET (host, unverified for the device): parse+store 1000 entries %.3f s, open cached folder %.4f s, page turn %.4f s\n", fetch_s, open_s, turn_s);
    PB_CHECK(fetch_s <= 10.0);
    PB_CHECK(open_s <= 2.0);
    PB_CHECK(turn_s <= 1.0);
}

PB_TEST(the_database_stays_within_50_mb_at_the_cache_limits) {
    pbtest::TmpDir dir;
    FixedClock clock(1792497600);
    auto opened = pbc::Store::open(dir.file("data"), clock);
    PB_CHECK(opened.ok());
    auto store = std::move(opened.value);
    // 2,000 folders of 25 entries = 50,000 entries, with realistic names and an ETag, a type and a checksum each
    for (int f = 0; f < 2000; ++f) {
        const std::string folder = "/Library/Author " + std::to_string(f);
        pbc::Listing l;
        l.folder_etag = "\"0123456789abcdef0123456789abcdef\"";
        for (int i = 0; i < 25; ++i) {
            pbc::RemoteEntry e;
            e.name = "A long enough title of a book number " + std::to_string(i) + ".epub";
            e.path = pbc::join_remote(folder, e.name);
            e.size = 1234567 + i;
            e.mtime = 1790000000 + i;
            e.etag = "\"0123456789abcdef0123456789abcdef\"";
            e.mime = "application/epub+zip";
            e.permissions = "RGDNVW";
            e.checksum = "SHA1:0123456789abcdef0123456789abcdef01234567";
            e.fileid = 100000 + f * 25 + i;
            l.entries.push_back(e);
        }
        PB_CHECK(store->replace_listing(folder, l, 1790000000).ok());
    }
    PB_CHECK_EQ(store->debug_query("SELECT count(*) FROM entries"), std::string("50000"));
    struct stat st;
    PB_CHECK(::stat(dir.file("data/pbcloud.db").c_str(), &st) == 0);
    std::printf("BUDGET (host, unverified for the device): database with 50,000 entries %.1f MB\n", static_cast<double>(st.st_size) / (1 << 20));
    PB_CHECK(st.st_size <= 50ll * 1024 * 1024);
}

PB_TEST_MAIN()
