// The planner (specs/003-folder-sync, contracts/sync-rules.md): the decision table row by row, the direction wall, determinism, name safety and the totals.
// Rows that belong to a later story are present as pending cases (SKIPPED_UNTIL_STORY) so that the whole table stays in one file.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "pbc/sync_planner.h"
#include "pbc/sync_rules.h"
#include "pbc/sync_scan.h"
#include "pbc/sync_types.h"
#include "test_support.h"
#include "tmp_dir.h"

// A case of a later story: it is listed and compiled but not run until that story enables it (tasks T080, T085, T091, T106, T115).
#define SKIPPED_UNTIL_STORY(name, story) PB_TEST(name) { PB_SKIP(story); }

namespace {

using pbc::Direction;
using pbc::PlanAction;

struct Env {
    pbtest::TmpDir tmp;
    std::string reader;
    pbc::PlannerInput in;
    std::map<std::string, bool> changed_reader;   // local rel -> what reader_changed answers (default: unchanged)
    std::map<std::string, bool> same;             // server rel -> what identical answers (default: not the same)
    Env(Direction d = Direction::Down) {
        reader = tmp.file("Reader");
        ::mkdir(reader.c_str(), 0700);
        in.pair = pbc::default_pair("https://cloud.example.org", "demo");
        in.pair.id = 1;
        in.pair.remote_folder = "/Books";
        in.pair.local_folder = reader;
        in.pair.direction = d;
        in.reader_date = "2026-10-22";
        in.volume_bytes = 64LL << 30;
        in.free_bytes = 32LL << 30;
        in.identical = [this](const pbc::ScanEntry& s, const pbc::ScanEntry&) { return same.count(s.rel) && same[s.rel]; };
        in.reader_changed = [this](const std::string& abs, const pbc::SyncRecord& r) {
            return changed_reader.count(r.local_rel) && changed_reader[r.local_rel];
        };
    }
    void server(const std::string& rel, int64_t size, const std::string& etag, const std::string& checksum = "", const char* skip = "") {
        pbc::ScanEntry e;
        e.rel = rel;
        e.size = size;
        e.etag = etag;
        e.checksum = checksum;
        e.skip_reason = skip;
        if (*skip) e.availability = pbc::Availability::NoPermission;
        in.server.entries.push_back(e);
    }
    void server_folder(const std::string& rel) {
        pbc::ScanEntry e;
        e.rel = rel;
        e.kind = pbc::Kind::Folder;
        in.server.entries.push_back(e);
    }
    void local(const std::string& rel, int64_t size, int64_t mtime = 1700000000, const char* skip = "") {
        pbc::ScanEntry e;
        e.rel = rel;
        e.size = size;
        e.mtime = mtime;
        e.skip_reason = skip;
        in.reader.entries.push_back(e);
    }
    void local_folder(const std::string& rel) {
        pbc::ScanEntry e;
        e.rel = rel;
        e.kind = pbc::Kind::Folder;
        in.reader.entries.push_back(e);
    }
    void record(const std::string& rel, const std::string& etag, int64_t size, const std::string& local_rel = "") {
        pbc::SyncRecord r;
        r.pair_id = 1;
        r.rel = rel;
        r.local_rel = local_rel.empty() ? rel : local_rel;
        r.origin = "download";
        r.server_etag = etag;
        r.server_size = size;
        r.local_size = size;
        r.local_mtime = 1700000000;
        in.records.push_back(r);
    }
    pbc::SyncPlan plan() { return pbc::plan(in); }
};

const pbc::PlanEntry* find(const pbc::SyncPlan& p, const std::string& rel) {
    for (const auto& e : p.entries)
        if (e.rel == rel) return &e;
    return nullptr;
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------------------- the table, direction down

PB_TEST(row_1_an_open_queue_item_for_a_name_skips_it_whatever_else_is_true) {
    Env e;
    e.server("new.epub", 10, "\"n\"");
    e.server("synced.epub", 10, "\"s2\"");
    e.local("synced.epub", 10);
    e.record("synced.epub", "\"s1\"", 10);   // the server changed: it would be a download
    e.in.open_rels = {"new.epub", "synced.epub"};
    const auto p = e.plan();
    PB_CHECK(find(p, "new.epub")->action == PlanAction::Skip);
    PB_CHECK_EQ(find(p, "new.epub")->reason, std::string("change_pending"));
    PB_CHECK_EQ(find(p, "synced.epub")->reason, std::string("change_pending"));
    PB_CHECK_EQ(p.totals.n_download, 0);
}

PB_TEST(row_2_an_entry_the_server_does_not_let_the_user_use_is_skipped_as_unavailable) {
    Env e;
    e.server("secret.epub", 10, "\"x\"", "", "unavailable");
    const auto p = e.plan();
    PB_CHECK(find(p, "secret.epub")->action == PlanAction::Skip);
    PB_CHECK_EQ(find(p, "secret.epub")->reason, std::string("unavailable"));
    PB_CHECK_EQ(p.totals.n_skip, 1);
}

PB_TEST(row_3_a_reader_entry_that_is_not_a_regular_file_is_skipped_where_it_matters) {
    Env e;
    e.server("a.epub", 10, "\"a2\"");
    e.local("a.epub", 0, 1700000000, "not_regular");   // a link where the recorded file was
    e.record("a.epub", "\"a1\"", 10);
    e.local("stray-link", 0, 1700000000, "not_regular");   // a link nobody asked about
    e.local("stray-locked", 5, 1700000000, "source_unreadable");
    const auto p = e.plan();
    PB_CHECK(find(p, "a.epub")->action == PlanAction::Skip);
    PB_CHECK_EQ(find(p, "a.epub")->reason, std::string("not_regular"));
    PB_CHECK(find(p, "stray-link") == nullptr);   // direction down: nothing would be sent, so nothing is reported
    PB_CHECK(find(p, "stray-locked") == nullptr);
}

PB_TEST(row_4_a_name_that_cannot_be_made_safe_is_skipped_with_its_reason) {
    {   // a link where a folder is needed
        Env e;
        PB_CHECK(::symlink("/tmp", (e.reader + "/Sub").c_str()) == 0);
        e.local("Sub", 0, 1700000000, "not_regular");
        e.server("Sub/a.epub", 10, "\"a\"");
        const auto p = e.plan();
        PB_CHECK(find(p, "Sub/a.epub")->action == PlanAction::Skip);
        PB_CHECK_EQ(find(p, "Sub/a.epub")->reason, std::string("path_unsafe"));
    }
    {   // a path that is too long for the reader
        Env e;
        std::string rel;
        for (int i = 0; i < 40; ++i) rel += std::string(110, 'x') + "/";
        e.server(rel + "a.epub", 10, "\"a\"");
        const auto p = e.plan();
        PB_CHECK_EQ(p.entries.size(), size_t(1));
        PB_CHECK(p.entries[0].action == PlanAction::Skip);
        PB_CHECK(p.entries[0].reason == "path_too_long" || p.entries[0].reason == "path_unsafe");
    }
    {   // two reader names that differ only by case: the second is a collision, whatever the server says
        Env e;
        e.local("Book.epub", 1);
        e.local("book.epub", 2, 1700000000, "name_collision");
        e.record("book.epub", "\"b1\"", 2);
        e.server("book.epub", 2, "\"b1\"");
        const auto p = e.plan();
        PB_CHECK(find(p, "book.epub")->action == PlanAction::Skip);
        PB_CHECK_EQ(find(p, "book.epub")->reason, std::string("name_collision"));
    }
}

PB_TEST(row_5_a_file_the_card_cannot_hold_is_skipped) {
    Env e;
    e.server("huge.bin", 5LL << 30, "\"h\"");
    e.server("just-fits.bin", (4LL << 30) - 1, "\"j\"");
    const auto p = e.plan();
    PB_CHECK_EQ(find(p, "huge.bin")->reason, std::string("too_large_for_storage"));
    PB_CHECK(find(p, "just-fits.bin")->action == PlanAction::Download);
}

PB_TEST(row_6_a_new_server_file_is_downloaded_under_a_safe_name_below_the_pair_folder) {
    Env e;
    e.server("a.epub", 100, "\"a\"", "SHA256:" + std::string(64, 'a'));
    e.server("Novels/b:c.epub", 200, "\"b\"");
    const auto p = e.plan();
    PB_CHECK_EQ(p.entries.size(), size_t(2));
    const auto* a = find(p, "a.epub");
    PB_CHECK(a->action == PlanAction::Download);
    PB_CHECK_EQ(a->local_rel, std::string("a.epub"));
    PB_CHECK_EQ(a->size.value_or(-1), int64_t(100));
    PB_CHECK_EQ(a->plan_etag, std::string("\"a\""));
    PB_CHECK(a->unapprovable.empty());
    const auto* b = find(p, "Novels/b:c.epub");
    PB_CHECK_EQ(b->local_rel, std::string("Novels/b_c.epub"));   // the colon is not allowed on the card
    PB_CHECK_EQ(p.totals.n_download, 2);
    PB_CHECK_EQ(p.totals.b_download, int64_t(300));
    PB_CHECK_EQ(p.totals.folders_new, 1);   // Novels
}

PB_TEST(row_6_two_server_names_that_become_the_same_reader_name_get_different_ones) {
    Env e;
    e.server("a:b.epub", 1, "\"1\"");
    e.server("a_b.epub", 2, "\"2\"");
    e.server("A_B.epub", 3, "\"3\"");   // the card cannot tell it from the others
    const auto p = e.plan();
    std::set<std::string> folded;
    for (const auto& en : p.entries) {
        PB_CHECK(en.action == PlanAction::Download);
        std::string f = en.local_rel;
        for (char& c : f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        PB_CHECK(folded.insert(f).second);   // no two entries claim the same name
    }
    PB_CHECK_EQ(folded.size(), size_t(3));
}

PB_TEST(row_7_a_new_reader_file_is_not_listed_and_never_offered_for_deletion_when_the_direction_is_down) {
    Env e;
    e.local("mine.epub", 50);
    e.local("Folder", 0);
    e.in.reader.entries.back().kind = pbc::Kind::Folder;
    const auto p = e.plan();
    PB_CHECK(p.entries.empty());
    PB_CHECK_EQ(p.totals.n_upload, 0);
    PB_CHECK_EQ(p.totals.n_prop_server + p.totals.n_prop_reader, 0);
}

PB_TEST(row_8_a_file_that_is_the_same_on_both_sides_at_the_first_sync_is_remembered_not_transferred) {
    Env e;
    e.server("a.epub", 100, "\"a\"", "SHA256:" + std::string(64, 'a'));
    e.local("a.epub", 100, 1700000555);
    e.same["a.epub"] = true;
    const auto p = e.plan();
    PB_CHECK_EQ(p.entries.size(), size_t(1));
    const auto& en = p.entries[0];
    PB_CHECK(en.action == PlanAction::Skip);
    PB_CHECK_EQ(en.reason, std::string("identical"));
    PB_CHECK_EQ(en.plan_etag, std::string("\"a\""));       // what the record will say
    PB_CHECK_EQ(en.plan_lsize.value_or(-1), int64_t(100));
    PB_CHECK_EQ(en.plan_lmtime.value_or(-1), int64_t(1700000555));
    PB_CHECK_EQ(en.size.value_or(-1), int64_t(100));
    PB_CHECK_EQ(p.totals.n_download, 0);
}

PB_TEST(row_9_a_name_on_both_sides_without_a_record_that_is_not_the_same_is_never_overwritten) {
    Env e;
    e.server("a.epub", 100, "\"a\"", "SHA256:" + std::string(64, 'a'));
    e.local("a.epub", 100);
    e.server("b.epub", 100, "\"b\"");   // equal size without any checksum is not enough
    e.local("b.epub", 100);
    const auto p = e.plan();
    for (const char* n : {"a.epub", "b.epub"}) {
        PB_CHECK(find(p, n)->action != PlanAction::Download);
        PB_CHECK(find(p, n)->action != PlanAction::Upload);
    }
    PB_CHECK_EQ(p.totals.n_download, 0);
}

PB_TEST(row_11_a_file_that_is_in_sync_gets_no_entry_and_is_only_counted) {
    Env e;
    e.server("a.epub", 100, "\"a\"");
    e.local("a.epub", 100);
    e.record("a.epub", "\"a\"", 100);
    const auto p = e.plan();
    PB_CHECK(p.entries.empty());
    PB_CHECK_EQ(p.totals.n_in_sync, 1);
    PB_CHECK_EQ(p.totals.n_download, 0);
}

PB_TEST(row_12_a_file_the_server_changed_is_downloaded_over_the_recorded_copy) {
    Env e;
    e.local_folder("Novels");
    e.server("Novels/a.epub", 150, "\"a2\"");
    e.local("Novels/a (2).epub", 100);
    e.record("Novels/a.epub", "\"a1\"", 100, "Novels/a (2).epub");
    const auto p = e.plan();
    PB_CHECK_EQ(p.entries.size(), size_t(1));
    const auto& en = p.entries[0];
    PB_CHECK(en.action == PlanAction::Download);
    PB_CHECK_EQ(en.rel, std::string("Novels/a.epub"));
    PB_CHECK_EQ(en.local_rel, std::string("Novels/a (2).epub"));   // the recorded name stays
    PB_CHECK_EQ(en.plan_etag, std::string("\"a2\""));
    PB_CHECK_EQ(en.size.value_or(-1), int64_t(150));
    PB_CHECK_EQ(p.totals.folders_new, 0);
}

PB_TEST(row_12_a_server_entry_without_an_etag_counts_as_changed) {
    Env e;
    e.server("a.epub", 100, "");
    e.local("a.epub", 100);
    e.record("a.epub", "\"a1\"", 100);
    const auto p = e.plan();
    PB_CHECK_EQ(p.entries.size(), size_t(1));
    PB_CHECK(p.entries[0].action == PlanAction::Download);
}

PB_TEST(row_13_a_file_the_reader_changed_is_not_sent_when_the_direction_is_down) {
    Env e;
    e.server("a.epub", 100, "\"a1\"");
    e.local("a.epub", 130);
    e.record("a.epub", "\"a1\"", 100);
    e.changed_reader["a.epub"] = true;
    const auto p = e.plan();
    PB_CHECK_EQ(p.entries.size(), size_t(1));
    PB_CHECK(p.entries[0].action == PlanAction::Skip);
    PB_CHECK_EQ(p.entries[0].reason, std::string("direction"));
    PB_CHECK_EQ(p.totals.n_upload, 0);
}

PB_TEST(row_19_a_record_whose_file_is_gone_on_both_sides_is_dropped_on_approval_and_not_shown) {
    Env e;
    e.record("gone.epub", "\"g\"", 10);
    auto p = e.plan();
    const auto* en = find(p, "gone.epub");
    PB_CHECK(en != nullptr);
    PB_CHECK(en->action == PlanAction::Skip);
    PB_CHECK_EQ(en->reason, std::string("gone_both"));
    PB_CHECK_EQ(p.totals.n_skip, 0);   // hidden: not counted among the skips the user sees
    // an incomplete scan of either side never drops a record: unknown is not missing
    Env a;
    a.record("gone.epub", "\"g\"", 10);
    a.in.server.incomplete = true;
    PB_CHECK(a.plan().entries.empty());
    Env b;
    b.record("gone.epub", "\"g\"", 10);
    b.in.reader.incomplete = true;
    PB_CHECK(b.plan().entries.empty());
}

PB_TEST(row_22_everything_else_is_left_alone) {
    Env e;
    e.server("a.epub", 100, "\"a\"");
    e.local("a.epub", 100);
    e.record("a.epub", "\"a\"", 100);
    e.record("other.epub", "\"o\"", 5);   // only on the reader's record, files exist neither side but scans are incomplete
    e.in.server.incomplete = true;
    const auto p = e.plan();
    PB_CHECK(p.entries.empty());
}

// ------------------------------------------------------------------------------------------------------------------------- the table, direction up and both (US4)

PB_TEST(row_7_a_new_reader_file_is_an_upload_when_the_direction_allows_the_server) {
    for (const Direction d : {Direction::Up, Direction::Both}) {
        Env e(d);
        e.local("note.txt", 50, 1700000123);
        e.local_folder("Trips");
        e.local("Trips/day1.txt", 70, 1700000456);
        const auto p = e.plan();
        PB_CHECK_EQ(p.totals.n_upload, 2);
        PB_CHECK_EQ(p.totals.b_upload, int64_t(120));
        PB_CHECK_EQ(p.totals.folders_new, 1);   // Trips has to be made on the server
        const auto* n = find(p, "note.txt");
        PB_CHECK(n != nullptr && n->action == PlanAction::Upload);
        PB_CHECK_EQ(n->plan_lsize.value_or(-1), int64_t(50));
        PB_CHECK_EQ(n->plan_lmtime.value_or(-1), int64_t(1700000123));
        PB_CHECK(n->plan_etag.empty());   // a new file replaces nothing
        PB_CHECK(find(p, "Trips/day1.txt") != nullptr);
    }
}

PB_TEST(row_13_a_file_the_reader_changed_is_a_replace_conditional_on_the_recorded_strong_version) {
    for (const Direction d : {Direction::Up, Direction::Both}) {
        Env e(d);
        e.server("a.epub", 100, "\"a1\"");
        e.local("a.epub", 130);
        e.record("a.epub", "\"a1\"", 100);
        e.changed_reader["a.epub"] = true;
        const auto p = e.plan();
        PB_CHECK_EQ(p.totals.n_upload, 1);
        const auto* a = find(p, "a.epub");
        PB_CHECK(a != nullptr && a->action == PlanAction::Upload);
        PB_CHECK_EQ(a->plan_etag, std::string("\"a1\""));   // the replace is conditional on it
    }
    Env weak(Direction::Up);
    weak.server("w.epub", 100, "W/\"w1\"");
    weak.local("w.epub", 130);
    weak.record("w.epub", "W/\"w1\"", 100);
    weak.changed_reader["w.epub"] = true;
    const auto p = weak.plan();
    PB_CHECK_EQ(p.totals.n_upload, 0);
    PB_CHECK_EQ(find(p, "w.epub")->reason, std::string("server_version_unknown"));
}

PB_TEST(a_file_the_server_has_changed_is_not_downloaded_in_an_upload_only_pair_and_a_clean_file_is_not_sent) {
    Env e(Direction::Up);
    e.server("a.epub", 150, "\"a2\"");
    e.local("a.epub", 100);
    e.record("a.epub", "\"a1\"", 100);
    e.server("s.epub", 10, "\"s1\"");   // only on the server
    e.server("b.epub", 100, "\"b1\"");
    e.local("b.epub", 100);
    e.record("b.epub", "\"b1\"", 100);
    const auto p = e.plan();
    PB_CHECK_EQ(p.totals.n_download, 0);
    PB_CHECK_EQ(p.totals.n_upload, 0);
    PB_CHECK_EQ(find(p, "a.epub")->reason, std::string("direction"));
    PB_CHECK(find(p, "s.epub") == nullptr);   // the direction wall: not even listed
    PB_CHECK_EQ(p.totals.n_in_sync, 1);
}

PB_TEST(a_file_larger_than_the_servers_free_quota_is_skipped_as_quota_exceeded) {
    Env e(Direction::Up);
    e.in.quota_free = 1000;
    e.local("big.bin", 5000);
    e.local("small.bin", 900);
    const auto p = e.plan();
    PB_CHECK_EQ(find(p, "big.bin")->reason, std::string("quota_exceeded"));
    PB_CHECK(find(p, "small.bin")->action == PlanAction::Upload);
}

PB_TEST(files_that_cannot_be_sent_are_skips_with_their_reason) {
    Env e(Direction::Up);
    e.local("empty.txt", 0);
    e.local("huge.bin", (4LL << 30) + 1);
    e.local("ends-with-dot.", 5);
    e.local("link.txt", 5, 1700000000, "not_regular");
    e.local("fine.txt", 5);
    const auto p = e.plan();
    PB_CHECK_EQ(find(p, "empty.txt")->reason, std::string("source_unreadable"));
    PB_CHECK_EQ(find(p, "huge.bin")->reason, std::string("too_large_for_storage"));
    PB_CHECK_EQ(find(p, "ends-with-dot.")->reason, std::string("name_forbidden"));
    PB_CHECK_EQ(find(p, "link.txt")->reason, std::string("not_regular"));
    PB_CHECK(find(p, "fine.txt")->action == PlanAction::Upload);
    PB_CHECK_EQ(p.totals.n_upload, 1);
}

PB_TEST(both_directions_together_download_the_new_server_file_and_upload_the_new_reader_file_and_the_changed_one_each_way) {
    Env e(Direction::Both);
    e.server("from-server.epub", 10, "\"s1\"");
    e.local("from-reader.epub", 20);
    e.server("sc.epub", 30, "\"c2\"");
    e.local("sc.epub", 25);
    e.record("sc.epub", "\"c1\"", 25);
    e.server("rc.epub", 40, "\"r1\"");
    e.local("rc.epub", 45);
    e.record("rc.epub", "\"r1\"", 40);
    e.changed_reader["rc.epub"] = true;
    const auto p = e.plan();
    PB_CHECK(find(p, "from-server.epub")->action == PlanAction::Download);
    PB_CHECK(find(p, "from-reader.epub")->action == PlanAction::Upload);
    PB_CHECK(find(p, "sc.epub")->action == PlanAction::Download);
    PB_CHECK(find(p, "rc.epub")->action == PlanAction::Upload);
    PB_CHECK_EQ(p.totals.n_download, 2);
    PB_CHECK_EQ(p.totals.n_upload, 2);
}

// ------------------------------------------------------------------------------------------------------------------------- keep both versions (US6)

namespace {
// a record that is diverged / a conflict copy that lives on one side only
void mark_diverged(Env& e, const std::string& rel) {
    for (auto& r : e.in.records)
        if (r.rel == rel) r.diverged = true;
}
void add_copy_record(Env& e, const std::string& rel, const char* one_sided, const std::string& copy_of) {
    pbc::SyncRecord r;
    r.pair_id = 1;
    r.rel = r.local_rel = rel;
    r.origin = "conflict_copy";
    r.one_sided = one_sided;
    r.copy_of = copy_of;
    r.server_size = r.local_size = 10;
    r.local_mtime = 1700000000;
    e.in.records.push_back(r);
}
}  // namespace

PB_TEST(rows_9_and_14_a_clash_at_the_first_sync_and_a_change_on_both_sides_keep_both_versions_named_by_direction) {
    for (const Direction d : {Direction::Down, Direction::Up, Direction::Both}) {
        Env e(d);
        e.server("clash.epub", 100, "\"c1\"");   // row 9: the same name on both sides, no record, different content
        e.local("clash.epub", 120);
        e.server("Sub/both.epub", 200, "\"b2\"");   // row 14: changed on both sides since the record
        e.local("Sub/both.epub", 230);
        e.record("Sub/both.epub", "\"b1\"", 180);
        e.changed_reader["Sub/both.epub"] = true;
        e.server_folder("Sub");
        e.local_folder("Sub");
        const auto p = e.plan();
        PB_CHECK_EQ(p.totals.n_keep_both, 2);
        PB_CHECK_EQ(p.totals.n_download + p.totals.n_upload, 0);
        const auto* c = find(p, "clash.epub");
        const auto* b = find(p, "Sub/both.epub");
        PB_CHECK(c != nullptr && c->action == PlanAction::KeepBoth);
        PB_CHECK(b != nullptr && b->action == PlanAction::KeepBoth);
        const bool down = d != Direction::Up, up = d != Direction::Down;
        PB_CHECK_EQ(c->conflict_name, down ? std::string("clash (server copy 2026-10-22).epub") : std::string());
        PB_CHECK_EQ(c->conflict_name_server, up ? std::string("clash (reader copy 2026-10-22).epub") : std::string());
        PB_CHECK_EQ(b->conflict_name, down ? std::string("Sub/both (server copy 2026-10-22).epub") : std::string());
        PB_CHECK_EQ(b->conflict_name_server, up ? std::string("Sub/both (reader copy 2026-10-22).epub") : std::string());
        PB_CHECK_EQ(c->plan_etag, std::string("\"c1\""));   // the server version a copy is made from
        PB_CHECK_EQ(c->size.value_or(-1), int64_t(100));
        PB_CHECK_EQ(c->plan_lsize.value_or(-1), int64_t(120));   // the reader version a copy is made from
        PB_CHECK_EQ(p.totals.b_keep_both, down ? int64_t(300) : int64_t(300));
        // the wall: a down pair writes nothing to the server, an up pair nothing to the reader
        for (const auto& en : p.entries) {
            if (d == Direction::Down) PB_CHECK(en.conflict_name_server.empty());
            if (d == Direction::Up) PB_CHECK(en.conflict_name.empty());
        }
    }
}

PB_TEST(a_conflict_name_is_free_on_the_receiving_side_in_the_listing_the_records_and_the_plan) {
    Env e(Direction::Both);
    e.server("a.epub", 100, "\"a1\"");
    e.local("a.epub", 120);
    e.server("a (server copy 2026-10-22).epub", 5, "\"x\"");          // the server holds that name already (not the reader)
    e.local("a (reader copy 2026-10-22).epub", 6);                       // the reader holds that name already (not the server)
    e.local("a (server copy 2026-10-22).epub", 7);                       // and the reader this one: the reader copy name is taken there
    e.record("a (server copy 2026-10-22).epub", "\"x\"", 5);
    e.server("z.epub", 100, "\"z1\"");
    e.local("z.epub", 120);
    e.server("y.epub", 100, "\"y1\"");
    e.local("y.epub", 120);
    const auto p = e.plan();
    const auto* a = find(p, "a.epub");
    PB_CHECK(a != nullptr && a->action == PlanAction::KeepBoth);
    PB_CHECK_EQ(a->conflict_name, std::string("a (server copy 2026-10-22) (2).epub"));       // taken on the reader (a file and a record)
    PB_CHECK_EQ(a->conflict_name_server, std::string("a (reader copy 2026-10-22).epub"));     // free on the server
    // different names of one folder never collide with each other
    PB_CHECK_EQ(find(p, "z.epub")->conflict_name, std::string("z (server copy 2026-10-22).epub"));
    PB_CHECK_EQ(find(p, "y.epub")->conflict_name, std::string("y (server copy 2026-10-22).epub"));
    // equal input, equal names
    const auto again = e.plan();
    PB_CHECK_EQ(find(again, "a.epub")->conflict_name, a->conflict_name);
}

PB_TEST(row_10_a_diverged_file_is_never_transferred_again_and_is_listed_as_kept_both) {
    Env e(Direction::Both);
    e.server("a.epub", 100, "\"a2\"");
    e.local("a.epub", 120);
    e.record("a.epub", "\"a1\"", 90);
    mark_diverged(e, "a.epub");
    e.changed_reader["a.epub"] = true;   // both sides keep changing: still no transfer
    add_copy_record(e, "a (server copy 2026-10-20).epub", "reader", "a.epub");
    add_copy_record(e, "a (reader copy 2026-10-20).epub", "server", "a.epub");
    e.local("a (server copy 2026-10-20).epub", 100);
    e.server("a (reader copy 2026-10-20).epub", 120, "\"r\"");
    const auto p = e.plan();
    PB_CHECK_EQ(p.totals.n_download + p.totals.n_upload + p.totals.n_keep_both, 0);
    const auto* a = find(p, "a.epub");
    PB_CHECK(a != nullptr && a->action == PlanAction::Skip);
    PB_CHECK_EQ(a->reason, std::string("kept_both"));
    PB_CHECK(a->plan_etag.empty());   // nothing to remember: it is remembered already
    // row 21: the copies, each on its one side, are neither transferred nor proposed
    const auto* r = find(p, "a (server copy 2026-10-20).epub");
    const auto* s = find(p, "a (reader copy 2026-10-20).epub");
    PB_CHECK(r != nullptr && r->action == PlanAction::Skip && r->reason == "kept_both");
    PB_CHECK(s != nullptr && s->action == PlanAction::Skip && s->reason == "kept_both");
    for (const auto& en : p.entries) PB_CHECK(en.action == PlanAction::Skip);
}

PB_TEST(a_diverged_file_whose_copy_for_an_allowed_side_is_missing_gets_only_that_copy) {
    Env e(Direction::Both);
    e.server("a.epub", 100, "\"a2\"");
    e.local("a.epub", 120);
    e.record("a.epub", "\"a1\"", 90);
    mark_diverged(e, "a.epub");
    add_copy_record(e, "a (server copy 2026-10-20).epub", "reader", "a.epub");   // the reader copy was made, the server copy was not
    e.local("a (server copy 2026-10-20).epub", 100);
    const auto p = e.plan();
    const auto* a = find(p, "a.epub");
    PB_CHECK(a != nullptr && a->action == PlanAction::KeepBoth);
    PB_CHECK(a->conflict_name.empty());
    PB_CHECK_EQ(a->conflict_name_server, std::string("a (reader copy 2026-10-22).epub"));
}

PB_TEST(a_conflict_whose_copies_exist_but_whose_original_is_not_yet_marked_is_marked_on_approval) {
    Env e(Direction::Down);
    e.server("a.epub", 100, "\"a2\"");
    e.local("a.epub", 120, 1700000555);
    e.record("a.epub", "\"a1\"", 90);
    e.changed_reader["a.epub"] = true;
    add_copy_record(e, "a (server copy 2026-10-20).epub", "reader", "a.epub");
    e.local("a (server copy 2026-10-20).epub", 100);
    const auto p = e.plan();
    const auto* a = find(p, "a.epub");
    PB_CHECK(a != nullptr && a->action == PlanAction::Skip);
    PB_CHECK_EQ(a->reason, std::string("kept_both"));
    PB_CHECK_EQ(a->plan_etag, std::string("\"a2\""));        // the divergence is recorded with the versions seen
    PB_CHECK_EQ(a->plan_lsize.value_or(-1), int64_t(120));
    PB_CHECK_EQ(a->plan_lmtime.value_or(-1), int64_t(1700000555));
    PB_CHECK_EQ(p.totals.n_keep_both, 0);
}

PB_TEST(row_21_a_one_sided_copy_that_the_user_deleted_is_dropped_on_approval_and_one_that_both_sides_hold_is_ordinary) {
    Env e(Direction::Both);
    add_copy_record(e, "gone (server copy 2026-10-20).epub", "reader", "gone.epub");   // nobody has it any more
    add_copy_record(e, "both (reader copy 2026-10-20).epub", "server", "both.epub");
    e.server("both (reader copy 2026-10-20).epub", 10, "\"s\"", "SHA256:" + std::string(64, 'b'));
    e.local("both (reader copy 2026-10-20).epub", 10);   // the user copied it over: both sides have it, the same
    e.same["both (reader copy 2026-10-20).epub"] = true;
    const auto p = e.plan();
    const auto* g = find(p, "gone (server copy 2026-10-20).epub");
    PB_CHECK(g != nullptr && g->action == PlanAction::Skip && g->reason == "gone_both");
    const auto* b = find(p, "both (reader copy 2026-10-20).epub");   // an ordinary name seen for the first time: identical, remembered on approval
    PB_CHECK(b != nullptr && b->action == PlanAction::Skip && b->reason == "identical");
    PB_CHECK_EQ(p.totals.n_download + p.totals.n_upload, 0);
}

PB_TEST(invariant_7_after_a_keep_both_ran_the_next_plan_transfers_neither_the_originals_nor_the_copies) {
    for (const Direction d : {Direction::Down, Direction::Up, Direction::Both}) {
        Env e(d);
        e.server("a.epub", 100, "\"a2\"");
        e.local("a.epub", 120);
        e.record("a.epub", "\"a1\"", 90);
        mark_diverged(e, "a.epub");
        if (d != Direction::Up) {
            add_copy_record(e, "a (server copy 2026-10-20).epub", "reader", "a.epub");
            e.local("a (server copy 2026-10-20).epub", 100);
        }
        if (d != Direction::Down) {
            add_copy_record(e, "a (reader copy 2026-10-20).epub", "server", "a.epub");
            e.server("a (reader copy 2026-10-20).epub", 120, "\"r\"");
        }
        const auto p = e.plan();
        for (const auto& en : p.entries) {
            PB_CHECK(en.action == PlanAction::Skip);
            PB_CHECK_EQ(en.reason, std::string("kept_both"));
        }
        PB_CHECK_EQ(p.totals.n_download + p.totals.n_upload + p.totals.n_keep_both + p.totals.n_prop_server + p.totals.n_prop_reader, 0);
    }
}

PB_TEST(a_conflict_copy_that_cannot_be_made_is_a_skip_with_its_reason) {
    Env e(Direction::Both);
    e.server("big.bin", (4LL << 30) + 5, "\"b1\"");
    e.local("big.bin", 100);
    e.server("empty.bin", 100, "\"e1\"");
    e.local("empty.bin", 0);
    const auto p = e.plan();
    PB_CHECK_EQ(find(p, "big.bin")->reason, std::string("too_large_for_storage"));
    PB_CHECK_EQ(find(p, "empty.bin")->reason, std::string("source_unreadable"));
    PB_CHECK_EQ(p.totals.n_keep_both, 0);
}

// ------------------------------------------------------------------------------------------------------------------------- deletions are proposals (US7)

namespace {
using pbc::Choice;
std::set<Choice> choices(std::initializer_list<Choice> c) { return std::set<Choice>(c); }
void trash(Env& e, bool on) { e.in.caps.trash = on; }
void mark_quiet(Env& e, const std::string& rel) {
    for (auto& r : e.in.records)
        if (r.rel == rel) r.quiet = true;
}
}  // namespace

PB_TEST(rows_15_and_16_a_file_deleted_on_the_server_is_a_proposal_with_the_choices_the_direction_and_the_reader_file_allow) {
    struct Case {
        Direction d;
        bool reader_changed;
        std::set<Choice> allowed;
    };
    const Case cases[] = {
        {Direction::Down, false, choices({Choice::Leave, Choice::Delete})},                                    // never an upload in a down pair
        {Direction::Down, true, choices({Choice::Leave})},                                                      // a changed reader file is never offered for deletion
        {Direction::Both, false, choices({Choice::Leave, Choice::Delete, Choice::TransferAgain})},
        {Direction::Both, true, choices({Choice::Leave, Choice::TransferAgain})},
        {Direction::Up, false, choices({Choice::Leave, Choice::TransferAgain})},                                // an up pair never writes the reader
        {Direction::Up, true, choices({Choice::Leave, Choice::TransferAgain})},
    };
    for (const Case& c : cases) {
        Env e(c.d);
        e.local("a.epub", 100);
        e.record("a.epub", "\"a1\"", 100);
        e.changed_reader["a.epub"] = c.reader_changed;
        const auto p = e.plan();
        const auto* a = find(p, "a.epub");
        PB_CHECK(a != nullptr && a->action == PlanAction::ProposeDeleteReader);
        PB_CHECK(a->allowed == c.allowed);
        PB_CHECK(a->choice == Choice::None);
        PB_CHECK_EQ(p.totals.n_prop_reader, 1);
        PB_CHECK_EQ(p.totals.n_download + p.totals.n_upload, 0);   // nothing is planned to be transferred or removed
    }
}

PB_TEST(rows_17_and_18_a_file_deleted_on_the_reader_is_a_proposal_with_the_choices_the_direction_the_trash_and_the_server_file_allow) {
    struct Case {
        Direction d;
        bool trash_on;
        bool server_changed;
        std::set<Choice> allowed;
    };
    const Case cases[] = {
        {Direction::Down, true, false, choices({Choice::Leave, Choice::TransferAgain})},                       // a book removed to free space: copy it back, or leave it
        {Direction::Both, true, false, choices({Choice::Leave, Choice::TransferAgain, Choice::Delete})},
        {Direction::Both, false, false, choices({Choice::Leave, Choice::TransferAgain})},                      // no trash: no deletion on the server
        {Direction::Both, true, true, choices({Choice::Leave, Choice::TransferAgain})},                        // the server file changed: it is kept
        {Direction::Up, true, false, choices({Choice::Leave, Choice::Delete})},                                // never a copy to the reader in an up pair
        {Direction::Up, true, true, choices({Choice::Leave})},
    };
    for (const Case& c : cases) {
        Env e(c.d);
        trash(e, c.trash_on);
        e.server("a.epub", 100, c.server_changed ? "\"a2\"" : "\"a1\"");
        e.record("a.epub", "\"a1\"", 100);
        const auto p = e.plan();
        const auto* a = find(p, "a.epub");
        PB_CHECK(a != nullptr && a->action == PlanAction::ProposeDeleteServer);
        PB_CHECK(a->allowed == c.allowed);
        PB_CHECK_EQ(p.totals.n_prop_server, 1);
        PB_CHECK_EQ(p.totals.n_download + p.totals.n_upload, 0);
    }
}

PB_TEST(row_20_a_quiet_record_is_proposed_again_only_when_the_present_file_changes) {
    Env e(Direction::Both);
    trash(e, true);
    e.server("s.epub", 100, "\"s1\"");     // gone from the reader, left alone by the user
    e.record("s.epub", "\"s1\"", 100);
    mark_quiet(e, "s.epub");
    e.local("r.epub", 100);                  // gone from the server, left alone
    e.record("r.epub", "\"r1\"", 100);
    mark_quiet(e, "r.epub");
    auto p = e.plan();
    PB_CHECK(p.entries.empty());
    PB_CHECK_EQ(p.totals.n_in_sync, 2);
    // the file that is still there changes: it is proposed again, and never for deletion
    Env f(Direction::Both);
    trash(f, true);
    f.server("s.epub", 150, "\"s2\"");
    f.record("s.epub", "\"s1\"", 100);
    mark_quiet(f, "s.epub");
    f.local("r.epub", 130);
    f.record("r.epub", "\"r1\"", 100);
    mark_quiet(f, "r.epub");
    f.changed_reader["r.epub"] = true;
    p = f.plan();
    PB_CHECK(find(p, "s.epub")->action == PlanAction::ProposeDeleteServer);
    PB_CHECK(find(p, "s.epub")->allowed.count(Choice::Delete) == 0);
    PB_CHECK(find(p, "r.epub")->action == PlanAction::ProposeDeleteReader);
    PB_CHECK(find(p, "r.epub")->allowed.count(Choice::Delete) == 0);
}

PB_TEST(a_folder_that_is_gone_from_one_side_as_a_whole_is_one_proposal_with_the_number_of_synced_files) {
    Env e(Direction::Both);
    trash(e, true);
    // the reader lost the folder Novels (two synced files in it, one deeper); the server still has it
    e.server_folder("Novels");
    e.server_folder("Novels/Deep");
    e.server("Novels/a.epub", 10, "\"n1\"");
    e.server("Novels/b.epub", 20, "\"n2\"");
    e.server("Novels/Deep/c.epub", 30, "\"n3\"");
    e.record("Novels/a.epub", "\"n1\"", 10);
    e.record("Novels/b.epub", "\"n2\"", 20);
    e.record("Novels/Deep/c.epub", "\"n3\"", 30);
    e.server_folder("Novels");
    e.in.server.entries.back().etag = "\"nf\"";
    // the server lost the folder Poems (one synced file in it); the reader still has it
    e.local_folder("Poems");
    e.local("Poems/p.epub", 5);
    e.record("Poems/p.epub", "\"p1\"", 5);
    // an ordinary file that is gone from the reader stays a file proposal
    e.server("single.epub", 7, "\"x1\"");
    e.record("single.epub", "\"x1\"", 7);
    const auto p = e.plan();
    const auto* n = find(p, "Novels");
    PB_CHECK(n != nullptr && n->action == PlanAction::ProposeDeleteServer && n->kind == pbc::Kind::Folder);
    PB_CHECK_EQ(n->records_below, 3);
    PB_CHECK(n->allowed == choices({Choice::Leave, Choice::Delete, Choice::TransferAgain}));   // the folder's own choices
    PB_CHECK_EQ(n->plan_etag, std::string("\"nf\""));
    PB_CHECK(find(p, "Novels/a.epub") == nullptr && find(p, "Novels/Deep/c.epub") == nullptr);   // not listed on their own
    const auto* po = find(p, "Poems");
    PB_CHECK(po != nullptr && po->action == PlanAction::ProposeDeleteReader && po->records_below == 1);
    PB_CHECK(po->allowed == choices({Choice::Leave, Choice::Delete, Choice::TransferAgain}));
    PB_CHECK(find(p, "single.epub")->action == PlanAction::ProposeDeleteServer);
    PB_CHECK_EQ(p.totals.n_prop_server, 2);
    PB_CHECK_EQ(p.totals.n_prop_reader, 1);
}

PB_TEST(invariant_8_an_incomplete_side_gives_no_proposal_and_no_record_drop) {
    Env e(Direction::Both);
    trash(e, true);
    e.in.server.incomplete = true;
    e.local("a.epub", 100);                  // looks gone from the server: unknown, not missing
    e.record("a.epub", "\"a1\"", 100);
    e.record("gone.epub", "\"g1\"", 10);   // looks gone from both: not dropped either
    auto p = e.plan();
    PB_CHECK(p.entries.empty());
    PB_CHECK_EQ(p.totals.n_prop_server + p.totals.n_prop_reader, 0);
    Env f(Direction::Both);
    trash(f, true);
    f.in.reader.incomplete = true;
    f.server("b.epub", 100, "\"b1\"");
    f.record("b.epub", "\"b1\"", 100);
    f.server_folder("Dir");
    f.server("Dir/x.epub", 5, "\"x\"");
    f.record("Dir/x.epub", "\"x\"", 5);
    p = f.plan();
    PB_CHECK(p.entries.empty());
}

PB_TEST(invariant_4_no_row_produces_an_action_that_removes_a_file) {
    Env e(Direction::Both);
    trash(e, true);
    e.server("s.epub", 100, "\"s1\"");
    e.record("s.epub", "\"s1\"", 100);
    e.local("r.epub", 100);
    e.record("r.epub", "\"r1\"", 100);
    const auto p = e.plan();
    for (const auto& en : p.entries) {
        PB_CHECK(en.action != PlanAction::Download);   // a deleted file is not downloaded again by itself
        PB_CHECK(en.action != PlanAction::Upload);
        PB_CHECK(en.choice == Choice::None);            // and nothing is chosen for the user
    }
    PB_CHECK_EQ(p.totals.n_prop_server + p.totals.n_prop_reader, 2);
}

// ------------------------------------------------------------------------------------------------------------------------- the rest of the table (later stories)


// ------------------------------------------------------------------------------------------------------------------------- invariants

PB_TEST(invariant_1_equal_input_gives_an_equal_plan_in_the_same_order) {
    Env e;
    for (int i = 0; i < 40; ++i) e.server("f" + std::to_string(100 - i) + ".epub", 10 + i, "\"e" + std::to_string(i) + "\"");
    e.server("Z/last.epub", 1, "\"z\"");
    e.server("A/first.epub", 1, "\"a\"");
    const auto a = e.plan();
    const auto b = e.plan();
    PB_CHECK_EQ(a.entries.size(), b.entries.size());
    for (size_t i = 0; i < a.entries.size(); ++i) {
        PB_CHECK_EQ(a.entries[i].rel, b.entries[i].rel);
        PB_CHECK_EQ(a.entries[i].local_rel, b.entries[i].local_rel);
        PB_CHECK_EQ(a.entries[i].seq, b.entries[i].seq);
    }
    // sorted by name, bytes order, with their sequence numbers from 1
    for (size_t i = 1; i < a.entries.size(); ++i) PB_CHECK(a.entries[i - 1].rel < a.entries[i].rel);
    for (size_t i = 0; i < a.entries.size(); ++i) PB_CHECK_EQ(a.entries[i].seq, static_cast<int>(i) + 1);
    // the order of the input does not matter
    std::reverse(e.in.server.entries.begin(), e.in.server.entries.end());
    const auto c = e.plan();
    PB_CHECK_EQ(c.entries.size(), a.entries.size());
    for (size_t i = 0; i < a.entries.size(); ++i) PB_CHECK_EQ(a.entries[i].rel, c.entries[i].rel);
}

PB_TEST(invariant_3_direction_down_is_a_wall_no_upload_whatever_the_reader_holds) {
    Env e;
    e.server("a.epub", 100, "\"a1\"");
    e.local("a.epub", 130);
    e.record("a.epub", "\"a1\"", 100);
    e.changed_reader["a.epub"] = true;
    e.local("new-on-reader.epub", 5);
    e.local("changed-too.epub", 9);
    e.server("changed-too.epub", 9, "\"c1\"");
    e.record("changed-too.epub", "\"c1\"", 9);
    e.changed_reader["changed-too.epub"] = true;
    const auto p = e.plan();
    for (const auto& en : p.entries) {
        PB_CHECK(en.action != PlanAction::Upload);
        PB_CHECK(en.action != PlanAction::ProposeDeleteServer);
    }
    PB_CHECK_EQ(p.totals.n_upload, 0);
    PB_CHECK_EQ(p.totals.b_upload, int64_t(0));
}

PB_TEST(invariant_9_a_hostile_server_name_becomes_a_safe_name_or_a_skip_never_a_path_outside) {
    Env e;
    for (const char* n : {"..", "a/../../x.epub", "CON.epub", "con", "tab\there.epub", "caf\xC3\xA9.epub", "cafe\xCC\x81.epub", ".pbc-9.part", "trailing.", "nul.txt"}) {
        const std::string rel = n;
        if (rel.find("..") != std::string::npos && rel.find('/') != std::string::npos) continue;   // the scan never hands such a name over
        e.server(rel, 5, "\"h\"");
    }
    const auto p = e.plan();
    for (const auto& en : p.entries) {
        if (en.action == PlanAction::Skip) continue;
        PB_CHECK(en.local_rel.find("..") == std::string::npos);
        PB_CHECK(en.local_rel.find('\t') == std::string::npos);
        PB_CHECK(en.local_rel[0] != '/');
        PB_CHECK(en.local_rel.compare(0, 5, ".pbc-") != 0);
        PB_CHECK(!pbc::join_path_checked(e.reader, en.local_rel).empty());
    }
    // the two spellings of é stay distinct names
    const auto* nfc = find(p, "caf\xC3\xA9.epub");
    const auto* nfd = find(p, "cafe\xCC\x81.epub");
    PB_CHECK(nfc != nullptr && nfd != nullptr);
    PB_CHECK(nfc->local_rel != nfd->local_rel);
}

PB_TEST(invariant_10_more_than_5000_entries_make_the_plan_truncated_and_keep_the_totals) {
    Env e;
    for (int i = 0; i < pbc::kSyncMaxPlanEntries + 1; ++i) e.server("f" + std::to_string(100000 + i) + ".epub", 1, "\"e" + std::to_string(i) + "\"");
    const auto p = e.plan();
    PB_CHECK(p.truncated);
    PB_CHECK(p.entries.empty());
    PB_CHECK_EQ(p.totals.n_download, pbc::kSyncMaxPlanEntries + 1);
    PB_CHECK_EQ(p.totals.b_download, int64_t(pbc::kSyncMaxPlanEntries + 1));
    // exactly the bound is fine
    Env f;
    for (int i = 0; i < pbc::kSyncMaxPlanEntries; ++i) f.server("f" + std::to_string(100000 + i) + ".epub", 1, "\"e" + std::to_string(i) + "\"");
    const auto q = f.plan();
    PB_CHECK(!q.truncated);
    PB_CHECK_EQ(q.entries.size(), size_t(pbc::kSyncMaxPlanEntries));
}

PB_TEST(a_folder_with_thousands_of_files_in_sync_has_an_empty_plan_that_can_be_approved) {
    Env e;
    for (int i = 0; i < 4000; ++i) {
        const std::string n = "f" + std::to_string(10000 + i) + ".epub";
        e.server(n, 5, "\"v\"");
        e.local(n, 5);
        e.record(n, "\"v\"", 5);
    }
    const auto p = e.plan();
    PB_CHECK(p.entries.empty());
    PB_CHECK_EQ(p.totals.n_in_sync, 4000);
    PB_CHECK(!p.truncated);
}

// ------------------------------------------------------------------------------------------------------------------------- totals, scans, space

PB_TEST(the_incomplete_flag_and_the_number_of_folders_not_compared_come_from_the_scans) {
    Env e;
    e.in.server.incomplete = true;
    e.in.server.folders_not_listed = 2;
    e.in.reader.incomplete = true;
    e.in.reader.folders_not_listed = 1;
    const auto p = e.plan();
    PB_CHECK(p.incomplete);
    PB_CHECK_EQ(p.incomplete_folders, 3);
    Env ok;
    PB_CHECK(!ok.plan().incomplete);
    PB_CHECK_EQ(ok.plan().incomplete_folders, 0);
}

PB_TEST(a_download_that_would_take_the_free_space_below_the_reserve_is_marked_no_space_in_plan_order) {
    Env e;
    e.in.volume_bytes = 10LL << 30;                  // the reserve is 256 MiB
    e.in.free_bytes = (256LL << 20) + 1000;          // 1000 bytes more than the reserve
    e.server("a.epub", 600, "\"a\"");
    e.server("b.epub", 600, "\"b\"");                // does not fit after a
    e.server("c.epub", 300, "\"c\"");                // fits after a
    const auto p = e.plan();
    PB_CHECK(find(p, "a.epub")->unapprovable.empty());
    PB_CHECK_EQ(find(p, "b.epub")->unapprovable, std::string("no_space"));
    PB_CHECK(find(p, "c.epub")->unapprovable.empty());
    PB_CHECK_EQ(p.reserve_bytes, int64_t(256) << 20);
    PB_CHECK_EQ(p.free_bytes.value_or(-1), (256LL << 20) + 1000);
    PB_CHECK(find(p, "b.epub")->action == PlanAction::Download);   // listed, but it cannot be approved as it is
    // unknown free space: nothing is marked
    Env u;
    u.in.free_bytes.reset();
    u.server("a.epub", 5LL << 30, "\"a\"");
    PB_CHECK(u.plan().entries[0].unapprovable.empty());
}

PB_TEST(sizes_folders_and_counts_are_right) {
    Env e;
    e.local_folder("Existing");
    e.server("Existing/x.epub", 10, "\"1\"");
    e.server("New/y.epub", 20, "\"2\"");
    e.server("New/Deeper/z.epub", 30, "\"3\"");
    e.server("secret.epub", 40, "\"4\"", "", "unavailable");
    e.local("keep.epub", 5);
    e.server("keep.epub", 5, "\"5\"");
    e.record("keep.epub", "\"5\"", 5);
    const auto p = e.plan();
    PB_CHECK_EQ(p.totals.n_download, 3);
    PB_CHECK_EQ(p.totals.b_download, int64_t(60));
    PB_CHECK_EQ(p.totals.n_skip, 1);
    PB_CHECK_EQ(p.totals.n_in_sync, 1);
    PB_CHECK_EQ(p.totals.folders_new, 2);   // New and New/Deeper; Existing is there
    PB_CHECK_EQ(p.free_bytes.value_or(-1), int64_t(32LL << 30));
    PB_CHECK_EQ(p.state, std::string("ready"));
    PB_CHECK(p.over_limit.empty());
}

PB_TEST(a_server_folder_and_a_reader_file_of_one_name_do_not_produce_a_transfer) {
    Env e;
    e.server_folder("Clash");
    e.local("Clash", 5);
    const auto p = e.plan();
    PB_CHECK(p.entries.empty() || p.entries[0].action == PlanAction::Skip);
    Env f;
    f.server("Clash", 5, "\"c\"");
    f.local_folder("Clash");
    const auto q = f.plan();
    PB_CHECK(!q.entries.empty());
    PB_CHECK(q.entries[0].action == PlanAction::Skip);   // a file cannot go where the reader has a folder
}

PB_TEST_MAIN()
