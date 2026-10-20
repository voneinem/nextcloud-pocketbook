// Downloaded copies and the journal of transfers in progress (data-model.md "local_files", "downloads"): written so that a copy and the
// end of its journal row happen together or not at all (constitution Principle III).
#include <string>

#include "pbc/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

pbc::SystemClock g_clock;

std::unique_ptr<pbc::Store> open_store(const pbtest::TmpDir& d) {
    auto r = pbc::Store::open(d.file("data"), g_clock);
    if (!r.ok()) PB_FAIL_AT("cannot open the store: " + r.error.key);
    return std::move(r.value);
}

pbc::LocalFile copy(const std::string& remote, const std::string& local, const std::string& etag = "\"e1\"") {
    pbc::LocalFile f;
    f.remote_path = remote;
    f.local_path = local;
    f.size = 100;
    f.mtime = 200;
    f.remote_etag = etag;
    f.remote_size = 100;
    f.remote_mtime = 150;
    f.downloaded_at = 300;
    return f;
}

pbc::DownloadJournalRow job(const std::string& remote, const std::string& target) {
    pbc::DownloadJournalRow j;
    j.remote_path = remote;
    j.target_path = target;
    j.expected_etag = "\"e1\"";
    j.expected_size = 100;
    j.state = "running";
    j.started_at = 10;
    j.updated_at = 10;
    return j;
}

}  // namespace

PB_TEST(a_journal_row_is_written_updated_and_ended) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const auto a = s->journal_begin(job("/a.epub", "a.epub"));
    const auto b = s->journal_begin(job("/b.epub", "b.epub"));
    PB_CHECK(a.ok() && b.ok());
    PB_CHECK(b.value > a.value);
    PB_CHECK_EQ(s->journal().size(), size_t(2));
    PB_CHECK(s->journal_update(a.value, 50, "waiting").ok());
    const auto rows = s->journal();
    PB_CHECK_EQ(rows[0].received, int64_t(50));
    PB_CHECK_EQ(rows[0].state, std::string("waiting"));
    PB_CHECK_EQ(rows[0].remote_path, std::string("/a.epub"));
    PB_CHECK_EQ(rows[0].expected_etag, std::string("\"e1\""));
    PB_CHECK_EQ(*rows[0].expected_size, int64_t(100));
    PB_CHECK(!s->journal_update(a.value, 60, "done").ok());   // only running, waiting and paused exist
    PB_CHECK(s->journal_end(a.value).ok());
    PB_CHECK_EQ(s->journal().size(), size_t(1));
    PB_CHECK(s->journal_end(a.value).ok());   // ending twice is harmless
}

PB_TEST(a_journal_row_needs_a_known_state) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    pbc::DownloadJournalRow j = job("/a", "a");
    j.state = "done";
    PB_CHECK(!s->journal_begin(j).ok());
    PB_CHECK(s->journal().empty());
}

PB_TEST(a_commit_writes_the_copy_and_ends_the_journal_row_together) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const auto id = s->journal_begin(job("/Books/a.epub", "Books/a.epub"));
    PB_CHECK(id.ok());
    PB_CHECK(s->commit_download(copy("/Books/a.epub", "Books/a.epub"), id.value).ok());
    PB_CHECK(s->journal().empty());
    const auto f = s->local_file("/Books/a.epub");
    PB_CHECK(f.has_value());
    PB_CHECK_EQ(f->local_path, std::string("Books/a.epub"));
    PB_CHECK_EQ(f->size, int64_t(100));
    PB_CHECK_EQ(f->remote_etag, std::string("\"e1\""));
    PB_CHECK_EQ(*f->remote_size, int64_t(100));
    PB_CHECK(!f->server_missing);
    PB_CHECK_EQ(f->seen_etag, std::string(""));
    PB_CHECK(!s->local_file("/nope").has_value());
}

PB_TEST(a_commit_that_fails_changes_neither_the_copies_nor_the_journal) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK(s->commit_download(copy("/a", "same.epub"), 0).ok());
    const auto id = s->journal_begin(job("/b", "same.epub"));
    // the local path is already taken by another remote file
    const pbc::Error e = s->commit_download(copy("/b", "same.epub"), id.value);
    PB_CHECK(!e.ok());
    PB_CHECK(!s->local_file("/b").has_value());
    PB_CHECK_EQ(s->journal().size(), size_t(1));   // the job is still there to be cleaned up or resumed
    PB_CHECK_EQ(s->local_file("/a")->local_path, std::string("same.epub"));
}

PB_TEST(a_new_download_of_the_same_file_replaces_the_row_and_clears_the_marks) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK(s->commit_download(copy("/a", "a.epub", "\"old\""), 0).ok());
    PB_CHECK(s->set_server_missing("/a", true).ok());
    PB_CHECK(s->set_seen_etag("/a", "\"seen\"").ok());
    PB_CHECK(s->local_file("/a")->server_missing);
    PB_CHECK_EQ(s->local_file("/a")->seen_etag, std::string("\"seen\""));
    PB_CHECK(s->commit_download(copy("/a", "a.epub", "\"new\""), 0).ok());
    const auto f = s->local_file("/a");
    PB_CHECK_EQ(f->remote_etag, std::string("\"new\""));
    PB_CHECK(!f->server_missing);
    PB_CHECK_EQ(f->seen_etag, std::string(""));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM local_files"), std::string("1"));
}

PB_TEST(setting_a_mark_on_an_unknown_file_is_harmless) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK(s->set_server_missing("/nothing", true).ok());
    PB_CHECK(s->set_seen_etag("/nothing", "\"x\"").ok());
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM local_files"), std::string("0"));
}

PB_TEST(the_copies_are_listed_in_pages_by_their_local_name_and_forgetting_one_keeps_the_others) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    for (const char* n : {"c.epub", "A.epub", "b.epub"}) PB_CHECK(s->commit_download(copy(std::string("/") + n, n), 0).ok());
    PB_CHECK_EQ(s->local_file_count(), 3);
    auto page = s->local_files(0, 2);
    PB_CHECK_EQ(page.size(), size_t(2));
    PB_CHECK_EQ(page[0].local_path, std::string("A.epub"));   // case-folded order
    PB_CHECK_EQ(page[1].local_path, std::string("b.epub"));
    PB_CHECK_EQ(s->local_files(2, 2)[0].local_path, std::string("c.epub"));
    PB_CHECK(s->forget_local_file("/b.epub").ok());
    PB_CHECK_EQ(s->local_file_count(), 2);
    PB_CHECK(s->forget_local_file("/b.epub").ok());   // idempotent
    const auto pairs = s->local_path_pairs();
    PB_CHECK_EQ(pairs.size(), size_t(2));
}

PB_TEST_MAIN()
