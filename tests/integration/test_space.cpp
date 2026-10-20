// Managing space (spec US6, FR-031, FR-032, SC-013): a local copy is removed on the reader and never touches the server; a copy with
// changes is only removed after the user accepted the loss; nothing outside the download folder can be deleted; clearing the cache
// drops listings and nothing the user owns.
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "download_env.h"
#include "pbc/folder_service.h"
#include "pbc/local_files.h"

PBC_FAKE_SERVER_TEST();

using pbtest::DlEnv;

namespace {

std::string pattern(size_t n, int seed) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 31 + static_cast<size_t>(seed)) & 0xff);
    return s;
}

struct SEnv : DlEnv {
    SEnv() {
        server.add_file("/Books/a.epub", pattern(3000, 1), 1790000000);
        server.add_file("/Books/b.epub", pattern(2000, 2), 1790000000);
        server.add_file("/c.txt", "see", 1790000000);
        PB_CHECK(run("/Books/a.epub").state == pbc::JobState::Done);
        PB_CHECK(run("/Books/b.epub").state == pbc::JobState::Done);
        PB_CHECK(run("/c.txt").state == pbc::JobState::Done);
        server.clear_log();
    }
};

}  // namespace

PB_TEST(removing_a_copy_deletes_the_file_and_its_row_and_sends_no_request) {
    SEnv e;
    PB_CHECK(e.exists("Books/a.epub"));
    PB_CHECK(pbc::remove_local_copy(*e.store, e.root, "/Books/a.epub", false).ok());
    PB_CHECK(!e.exists("Books/a.epub"));
    PB_CHECK(!e.store->local_file("/Books/a.epub").has_value());
    PB_CHECK(e.exists("Books/b.epub"));                      // the others stay
    PB_CHECK(e.store->local_file("/Books/b.epub").has_value());
    PB_CHECK_EQ(e.server.request_count(), size_t(0));        // the server is not contacted
    PB_CHECK(e.server.has("/Books/a.epub"));                 // and has not changed
    PB_CHECK_EQ(e.server.node("/Books/a.epub").size(), int64_t(3000));
}

PB_TEST(a_copy_with_changes_is_refused_unless_the_loss_is_confirmed) {
    SEnv e;
    e.write_local("Books/a.epub", "my notes");
    const auto refused = pbc::remove_local_copy(*e.store, e.root, "/Books/a.epub", false);
    PB_CHECK_EQ(refused.key, std::string("local_changed"));
    PB_CHECK(e.exists("Books/a.epub"));
    PB_CHECK_EQ(e.read("Books/a.epub"), std::string("my notes"));
    PB_CHECK(e.store->local_file("/Books/a.epub").has_value());
    PB_CHECK(pbc::remove_local_copy(*e.store, e.root, "/Books/a.epub", true).ok());
    PB_CHECK(!e.exists("Books/a.epub"));
    PB_CHECK(!e.store->local_file("/Books/a.epub").has_value());
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(a_conflict_copy_needs_the_confirmation_as_well) {
    SEnv e;
    e.write_local("Books/b.epub", "mine");
    e.server.set_file("/Books/b.epub", pattern(2100, 9), 1790009000);
    PB_CHECK_EQ(pbc::remove_local_copy(*e.store, e.root, "/Books/b.epub", false).key, std::string("local_changed"));
    PB_CHECK(e.exists("Books/b.epub"));
    PB_CHECK(pbc::remove_local_copy(*e.store, e.root, "/Books/b.epub", true).ok());
}

PB_TEST(a_file_that_is_already_gone_from_the_card_only_loses_its_row) {
    SEnv e;
    ::unlink(e.abs("c.txt").c_str());
    PB_CHECK(pbc::remove_local_copy(*e.store, e.root, "/c.txt", false).ok());
    PB_CHECK(!e.store->local_file("/c.txt").has_value());
    PB_CHECK(pbc::remove_local_copy(*e.store, e.root, "/c.txt", false).ok());   // and a second call is harmless
}

PB_TEST(a_row_that_points_outside_the_download_folder_never_deletes_anything) {
    SEnv e;
    const std::string outside = e.dir.file("outside.txt");
    e.dir.write("outside.txt", "keep me");
    pbc::LocalFile bad;
    bad.size = 7;
    bad.mtime = 1;
    bad.remote_etag = "\"e\"";
    bad.downloaded_at = 1;
    bad.remote_path = "/h1";
    bad.local_path = "../outside.txt";
    PB_CHECK(e.store->commit_download(bad, 0).ok());
    PB_CHECK(!pbc::remove_local_copy(*e.store, e.root, "/h1", true).ok());
    PB_CHECK_EQ(e.dir.read("outside.txt"), std::string("keep me"));
    bad.remote_path = "/h2";
    bad.local_path = "link.txt";
    PB_CHECK(::symlink(outside.c_str(), e.abs("link.txt").c_str()) == 0);
    PB_CHECK(e.store->commit_download(bad, 0).ok());
    PB_CHECK(!pbc::remove_local_copy(*e.store, e.root, "/h2", true).ok());
    PB_CHECK_EQ(e.dir.read("outside.txt"), std::string("keep me"));
    PB_CHECK(e.store->local_file("/h2").has_value());   // the row stays: nothing was proven safe
}

PB_TEST(clearing_the_cache_drops_the_listings_and_nothing_the_user_owns) {
    SEnv e;
    pbc::FolderService svc(*e.store, e.server, e.clock);
    e.server.set_favorite("/c.txt", true);
    PB_CHECK(svc.load("/", pbc::Fetch::Always).error.ok());
    PB_CHECK(svc.load("/Books", pbc::Fetch::Always).error.ok());
    PB_CHECK(svc.refresh_favorites().error.ok());
    PB_CHECK(e.store->set_mark("/Books", pbc::Kind::Folder, true).ok());
    pbc::DownloadJournalRow j;
    j.remote_path = "/Books/a.epub";
    j.target_path = "Books/a.epub";
    j.state = "paused";
    j.started_at = 1;
    j.updated_at = 1;
    PB_CHECK(e.store->journal_begin(j).ok());
    e.server.clear_log();

    PB_CHECK(e.store->folder("/Books").has_value());
    PB_CHECK(e.store->clear_cache().ok());
    PB_CHECK(!e.store->folder("/").has_value());
    PB_CHECK(!e.store->folder("/Books").has_value());
    PB_CHECK(e.store->entries_page("/Books", pbc::SortKey::Name, true, 0, 10).rows.empty());
    PB_CHECK_EQ(e.store->local_file_count(), 3);                    // downloads stay
    PB_CHECK_EQ(e.store->marks().size(), size_t(1));                // marks stay
    PB_CHECK(!e.store->favorites().empty());                        // favorites stay
    PB_CHECK_EQ(e.store->journal().size(), size_t(1));              // the journal stays
    PB_CHECK(e.exists("Books/a.epub"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(e.store->clear_cache().ok());                          // twice is harmless
}

PB_TEST(the_numbers_for_the_space_screen) {
    SEnv e;
    PB_CHECK_EQ(e.store->local_files_bytes(), int64_t(3000 + 2000 + 3));
    PB_CHECK(e.store->database_bytes() > 0);
    PB_CHECK(pbc::remove_local_copy(*e.store, e.root, "/Books/a.epub", false).ok());
    PB_CHECK_EQ(e.store->local_files_bytes(), int64_t(2000 + 3));
    pbc::FolderService svc(*e.store, e.server, e.clock);
    PB_CHECK(svc.load("/", pbc::Fetch::Always).error.ok());
    const int64_t with_listing = e.store->database_bytes();
    PB_CHECK(e.store->clear_cache().ok());
    PB_CHECK(e.store->database_bytes() <= with_listing);
}

PB_TEST_MAIN()
