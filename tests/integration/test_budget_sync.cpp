// Budgets of folder sync (specs/003-folder-sync plan.md "Performance Goals", SC-010, SC-011), measured on the HOST against the fake Nextcloud. Every number
// here is UNVERIFIED until it is measured on the InkPad One (docs/device-test-checklist.md DT-S6): a pass on the host says only that the code is not the
// bottleneck. The 500 MB transfer is covered by test_budget_download (a 1 GiB file streamed to disk through the real transport; memory does not grow with it).
#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

#include "sync_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::SyncEnv;
using pbtest::tree;

namespace {

long vm_hwm_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.compare(0, 6, "VmHWM:") == 0) return std::atol(line.c_str() + 6);
    return -1;
}
double since(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
long long file_size(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 ? static_cast<long long>(st.st_size) : -1;
}

}  // namespace

PB_TEST(a_1000_file_compare_is_fast_pages_turn_fast_and_memory_stays_small) {
    SyncEnv e;
    tree(e, 1000, 4);
    const long before = vm_hwm_kb();
    const auto t0 = std::chrono::steady_clock::now();
    const int64_t plan = e.compare_ok();
    const double compare_s = since(t0);
    const long after = vm_hwm_kb();
    PB_CHECK_EQ(e.plan(plan).totals.n_download, 1000);
    const auto t1 = std::chrono::steady_clock::now();
    double worst_page = 0;
    for (int offset = 0; offset < 1000; offset += 50) {
        const auto t = std::chrono::steady_clock::now();
        const auto page = e.store->sync_plan_page(plan, "transfers", offset, 50);
        PB_CHECK_EQ(page.rows.size(), size_t(50));
        worst_page = std::max(worst_page, since(t));
    }
    const double all_pages_s = since(t1);
    std::printf("[budget] compare of 1,000 files: %.2f s (budget 10 s, UNVERIFIED on the device); worst page turn %.3f s (budget 1 s); 20 pages %.2f s; "
                "VmHWM %ld -> %ld kB (budget 150 MB peak)\n", compare_s, worst_page, all_pages_s, before, after);
    if (compare_s > 10.0) PB_FAIL_AT("compare took " + std::to_string(compare_s) + " s: over the 10 s budget (UNVERIFIED until measured on the device, DT-S6)");
    if (worst_page > 1.0) PB_FAIL_AT("a page turn took over 1 s (UNVERIFIED until measured on the device, DT-S6)");
    if (after > 150 * 1024) PB_FAIL_AT("peak memory over 150 MB during the compare (UNVERIFIED until measured on the device, DT-S6)");
}

PB_TEST(the_database_at_its_bounds_stays_small_and_recovery_at_start_is_fast) {
    pbtest::TmpDir dir;
    pbtest::DlClock clock;
    auto opened = pbc::Store::open(dir.file("data"), clock);
    PB_CHECK(opened.ok());
    auto store = std::move(opened.value);
    pbc::AccountRow a;
    a.base_url = "https://cloud.example.org";
    a.login_name = a.user_id = "demo";
    a.state = "ok";
    PB_CHECK(store->save_account(a).ok());
    const long long base_size = file_size(dir.file("data") + "/pbcloud.db");
    pbc::SyncPair p = pbc::default_pair(a.base_url, "demo");
    p.remote_folder = "/Books";
    p.local_folder = dir.file("Reader");
    ::mkdir(p.local_folder.c_str(), 0700);
    const int64_t id = store->sync_pair_add(p).value;
    // the bounds: 20,000 records, a plan of 5,000 entries
    PB_CHECK(store->debug_exec("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < " + std::to_string(pbc::kSyncMaxRecords) +
                               ") INSERT INTO sync_files(pair_id, rel, local_rel, kind, origin, server_etag, server_size, local_size, local_mtime, synced_at) SELECT " +
                               std::to_string(id) + ", 'Folder' || (i % 50) || '/a-rather-long-file-name-number-' || i || '.epub', 'Folder' || (i % 50) || '/a-rather-long-file-name-number-' || i || '.epub',"
                               " 'file', 'download', '\"0123456789abcdef0123456789abcdef\"', 123456, 123456, 1790000000, 1790000000 FROM n").empty());
    pbc::SyncPlan plan;
    plan.pair_id = id;
    plan.created_at = clock.now();
    for (int i = 0; i < pbc::kSyncMaxPlanEntries; ++i) {
        pbc::PlanEntry e;
        e.rel = e.local_rel = "Folder" + std::to_string(i % 50) + "/entry-" + std::to_string(10000 + i) + ".epub";
        e.action = pbc::PlanAction::Download;
        e.size = 123456;
        e.plan_etag = "\"0123456789abcdef0123456789abcdef\"";
        plan.entries.push_back(e);
    }
    PB_CHECK(store->sync_plan_store(plan).ok());
    PB_CHECK(store->debug_exec("UPDATE sync_pairs SET state = 'running'").empty());
    const long long grown = file_size(dir.file("data") + "/pbcloud.db") - base_size;
    store.reset();
    auto again = pbc::Store::open(dir.file("data"), clock);
    PB_CHECK(again.ok());
    const auto t0 = std::chrono::steady_clock::now();
    const auto report = again.value->sync_recover();
    const double recover_s = since(t0);
    std::printf("[budget] database growth at the bounds: %lld kB (budget 10 MB); sync_recover with 20,000 records: %.1f ms (budget 100 ms, UNVERIFIED on the device)\n", grown / 1024, recover_s * 1000);
    PB_CHECK_EQ(report.pairs_interrupted, 1);
    if (grown > 10LL << 20) PB_FAIL_AT("the database grew by more than 10 MB at the bounds (UNVERIFIED until measured on the device, DT-S6)");
    if (recover_s > 0.1) PB_FAIL_AT("sync_recover took over 100 ms (UNVERIFIED until measured on the device, DT-S6)");
}

PB_TEST_MAIN()
