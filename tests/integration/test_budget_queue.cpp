// Budget of the queue (plan.md "Constraints"; spec SC-011): 2,000 queued items and a folder of 10,000 cached entries are read, paged and recovered
// within the budget, and the database does not grow beyond it. Measured on the HOST; the device figures stay UNVERIFIED until measured on the InkPad One
// (docs/device-test-checklist.md DT-W7 and the budget items).
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

#include "pbc/store.h"
#include "test_support.h"
#include "tmp_dir.h"

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

struct Clk : pbc::Clock {
    int64_t now() const override { return 1792497600; }
};

double seconds_since(const std::chrono::steady_clock::time_point& t0) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }

}  // namespace

PB_TEST(two_thousand_queued_items_and_ten_thousand_cached_entries_stay_within_the_budget) {
    pbtest::TmpDir dir;
    Clk clock;
    auto opened = pbc::Store::open(dir.file("data"), clock);
    PB_CHECK(opened.ok());
    auto store = std::move(opened.value);
    pbc::AccountRow a;
    a.base_url = "https://cloud.example.org";
    a.login_name = "demo";
    a.user_id = "demo";
    a.state = "ok";
    PB_CHECK(store->save_account(a).ok());

    pbc::Listing listing;
    for (int i = 0; i < 10000 - 1; ++i) {
        pbc::RemoteEntry e;
        e.name = "entry-" + std::to_string(100000 + i) + ".txt";
        e.path = "/Big/" + e.name;
        e.size = i;
        e.mtime = 1790000000 + i;
        e.etag = "\"e" + std::to_string(i) + "\"";
        listing.entries.push_back(e);
    }
    PB_CHECK(store->replace_listing("/Big", listing, clock.now()).ok());
    const long long db_before = store->database_bytes();

    std::vector<pbc::QueueItem> items;
    for (int i = 0; i < pbc::kMaxQueueItems; ++i) {
        pbc::QueueItem q;
        q.kind = pbc::ItemKind::Upload;
        q.server = a.base_url;
        q.user_id = a.user_id;
        q.src_local = "/mnt/ext1/Downloads/file-" + std::to_string(i) + ".bin";
        q.src_size = 1000 + i;
        q.src_mtime = 1790000000;
        q.dst_parent = "/Big";
        q.dst_name = "upload-" + std::to_string(i) + ".bin";
        items.push_back(q);
    }
    auto t0 = std::chrono::steady_clock::now();
    PB_CHECK(store->queue_add_group(items, std::vector<int>(items.size(), -1)).ok());
    const double add_s = seconds_since(t0);
    PB_CHECK_EQ(store->queue_count_open(), pbc::kMaxQueueItems);

    // the first page and a page turn of the queue screen
    t0 = std::chrono::steady_clock::now();
    const auto page0 = store->queue_page(0, 12);
    const double first_s = seconds_since(t0);
    t0 = std::chrono::steady_clock::now();
    const auto page5 = store->queue_page(60, 12);
    const double turn_s = seconds_since(t0);
    PB_CHECK_EQ(page0.rows.size(), size_t(12));
    PB_CHECK_EQ(page5.rows.size(), size_t(12));
    PB_CHECK(first_s <= 2.0);
    PB_CHECK(turn_s <= 1.0);

    // the folder listing with the queue merged in (paged in SQL)
    t0 = std::chrono::steady_clock::now();
    const auto overlay = store->entries_page_overlay("/Big", pbc::SortKey::Name, true, 0, 12);
    const double overlay_s = seconds_since(t0);
    PB_CHECK(overlay.rows.size() == 12);
    PB_CHECK(overlay_s <= 2.0);

    // recovery at start of 2,000 rows that were all being sent
    for (int i = 0; i < pbc::kMaxQueueItems; ++i) {
        auto it = store->queue_page(i, 1).rows[0];
        if (i % 50 != 0) continue;   // enough of them to make the recovery work; the rest are plain queued rows
        it.state = pbc::ItemState::Sending;
        it.step = i % 100 == 0 ? "chunks" : "single";
        PB_CHECK(store->queue_update(it).ok());
    }
    t0 = std::chrono::steady_clock::now();
    const pbc::RecoveryReport rec = store->queue_recover();
    const double recover_s = seconds_since(t0);
    PB_CHECK(rec.requeued + rec.unknown == 40);
    PB_CHECK(recover_s <= 0.1);

    const long long growth = store->database_bytes() - db_before;
    PB_CHECK(growth <= 2ll << 20);
    const long hwm = vm_hwm_kb();
    PB_CHECK(kSanitized || hwm < 150 * 1024);
    std::printf("BUDGET (host, unverified for the device): 2,000 items added in %.2f s; first page %.3f s, page turn %.3f s, listing with the queue merged %.3f s, "
                "recovery %.3f s; database grew %.2f MB; peak memory %ld MB\n",
                add_s, first_s, turn_s, overlay_s, recover_s, static_cast<double>(growth) / (1 << 20), hwm / 1024);
}

PB_TEST_MAIN()
