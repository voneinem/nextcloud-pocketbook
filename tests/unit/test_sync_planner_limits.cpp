// The limits of a pair against a plan (specs/003-folder-sync R-11, FR-042 to FR-047): only what a plan adds to the reader is counted, the plan is refused as a
// whole when it goes over, and the planner never takes entries out or picks which ones fit.
#include <string>

#include "pbc/sync_planner.h"
#include "pbc/sync_rules.h"
#include <sys/stat.h>

#include "test_support.h"
#include "tmp_dir.h"

namespace {

std::string reader_folder() {   // a real, empty folder: a new name needs one to be chosen against
    static pbtest::TmpDir dir;
    static const std::string path = dir.file("Reader");
    ::mkdir(path.c_str(), 0700);
    return path;
}

pbc::PlannerInput input(pbc::Direction d, int64_t max_files, int64_t max_bytes, int64_t held_files = 0, int64_t held_bytes = 0) {
    pbc::PlannerInput in;
    in.pair = pbc::default_pair("https://cloud.example.org", "demo");
    in.pair.id = 1;
    in.pair.remote_folder = "/Books";
    in.pair.local_folder = reader_folder();
    in.pair.direction = d;
    in.pair.max_files = max_files;
    in.pair.max_bytes = max_bytes;
    in.reader_date = "2026-10-22";
    in.volume_bytes = 64LL << 30;
    in.free_bytes = 32LL << 30;
    in.held_files = held_files;
    in.held_bytes = held_bytes;
    return in;
}
void server(pbc::PlannerInput* in, const std::string& rel, int64_t size) {
    pbc::ScanEntry e;
    e.rel = rel;
    e.size = size;
    e.etag = "\"" + rel + "\"";
    in->server.entries.push_back(e);
}

}  // namespace

PB_TEST(a_plan_over_the_file_limit_the_size_limit_or_both_is_flagged_and_cannot_be_approved) {
    auto files = input(pbc::Direction::Down, 3, 1LL << 30, 1);
    for (const char* n : {"a", "b", "c"}) server(&files, n, 10);   // 1 held + 3 = 4 > 3
    auto p = pbc::plan(files);
    PB_CHECK_EQ(p.over_limit, std::string("files"));
    PB_CHECK(!pbc::can_approve(p, p.created_at).ok);
    PB_CHECK_EQ(pbc::can_approve(p, p.created_at).error_key, std::string("over_limit"));

    auto bytes = input(pbc::Direction::Down, 100, 100, 0, 50);
    server(&bytes, "big", 60);   // 50 + 60 > 100
    PB_CHECK_EQ(pbc::plan(bytes).over_limit, std::string("bytes"));

    auto both = input(pbc::Direction::Down, 1, 100, 1, 50);
    server(&both, "x", 60);
    PB_CHECK_EQ(pbc::plan(both).over_limit, std::string("both"));

    auto fits = input(pbc::Direction::Down, 4, 1LL << 30, 1);
    for (const char* n : {"a", "b", "c"}) server(&fits, n, 10);
    p = pbc::plan(fits);
    PB_CHECK(p.over_limit.empty());
    PB_CHECK(pbc::can_approve(p, p.created_at).ok);
}

PB_TEST(a_plan_that_adds_nothing_to_the_reader_is_never_over_the_limit_even_when_the_folder_holds_more) {
    auto up = input(pbc::Direction::Up, 1, 10, 500, 1LL << 30);   // lowered far below what it holds
    pbc::ScanEntry l;
    l.rel = "mine.epub";
    l.size = 5;
    up.reader.entries.push_back(l);
    PB_CHECK(pbc::plan(up).over_limit.empty());
    auto down = input(pbc::Direction::Down, 1, 10, 500, 1LL << 30);   // nothing on the server to download
    PB_CHECK(pbc::plan(down).over_limit.empty());
}

PB_TEST(the_planner_keeps_every_entry_and_chooses_none_when_over_the_limit) {
    auto in = input(pbc::Direction::Down, 1, 1LL << 30);
    for (const char* n : {"a", "b", "c"}) server(&in, n, 10);
    const auto p = pbc::plan(in);
    PB_CHECK_EQ(p.over_limit, std::string("files"));
    PB_CHECK_EQ(p.totals.n_download, 3);
    for (const auto& e : p.entries) PB_CHECK(!e.removed);
}

PB_TEST(a_newer_version_of_a_file_the_reader_holds_adds_no_file_only_the_difference_in_size) {
    auto in = input(pbc::Direction::Down, 2, 1000, 2, 900);
    server(&in, "a", 150);   // replaces a file of 100 bytes: +50 bytes, +0 files
    pbc::ScanEntry l;
    l.rel = "a";
    l.size = 100;
    in.reader.entries.push_back(l);
    pbc::SyncRecord r;
    r.pair_id = 1;
    r.rel = r.local_rel = "a";
    r.origin = "download";
    r.server_etag = "\"old\"";
    r.server_size = 100;
    r.local_size = 100;
    r.local_mtime = 1700000000;
    in.records.push_back(r);
    in.reader_changed = [](const std::string&, const pbc::SyncRecord&) { return false; };
    const auto p = pbc::plan(in);
    PB_CHECK_EQ(p.totals.n_download, 1);
    PB_CHECK_EQ(p.over_limit, std::string(""));   // 2 files held, 2 allowed; 950 bytes of 1000
    in.pair.max_bytes = 940;
    PB_CHECK_EQ(pbc::plan(in).over_limit, std::string("bytes"));
}

PB_TEST_MAIN()
