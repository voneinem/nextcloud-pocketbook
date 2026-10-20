// "Keep offline" marks (data-model.md "offline_marks"): files and whole folders; what is in scope of an Update.
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

}  // namespace

PB_TEST(a_mark_is_set_listed_and_removed) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK(s->marks().empty());
    PB_CHECK(s->set_mark("/Books", pbc::Kind::Folder, true).ok());
    PB_CHECK(s->set_mark("/readme.txt", pbc::Kind::File, true).ok());
    PB_CHECK(s->set_mark("/Books", pbc::Kind::Folder, true).ok());   // twice: still one
    auto marks = s->marks();
    PB_CHECK_EQ(marks.size(), size_t(2));
    PB_CHECK_EQ(marks[0].remote_path, std::string("/Books"));
    PB_CHECK(marks[0].kind == pbc::Kind::Folder);
    PB_CHECK(marks[0].marked_at > 0);
    PB_CHECK(s->is_marked("/Books"));
    PB_CHECK(s->set_mark("/Books", pbc::Kind::Folder, false).ok());
    PB_CHECK(!s->is_marked("/Books"));
    PB_CHECK_EQ(s->marks().size(), size_t(1));
    PB_CHECK(s->set_mark("/never-marked", pbc::Kind::File, false).ok());   // removing what is not there is harmless
}

PB_TEST(marks_survive_a_restart) {
    pbtest::TmpDir d;
    {
        auto s = open_store(d);
        PB_CHECK(s->set_mark("/Books", pbc::Kind::Folder, true).ok());
    }
    auto s = open_store(d);
    PB_CHECK(s->is_marked("/Books"));
}

PB_TEST(a_more_specific_mark_stays_when_the_mark_above_it_is_removed) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK(s->set_mark("/Books", pbc::Kind::Folder, true).ok());
    PB_CHECK(s->set_mark("/Books/Novels/story.epub", pbc::Kind::File, true).ok());
    PB_CHECK(s->in_scope("/Books/Novels/story.epub"));
    PB_CHECK(s->set_mark("/Books", pbc::Kind::Folder, false).ok());
    PB_CHECK(s->in_scope("/Books/Novels/story.epub"));    // its own mark is still there
    PB_CHECK(!s->in_scope("/Books/Novels/other.epub"));
}

PB_TEST(scope_covers_a_marked_file_and_everything_below_a_marked_folder_at_any_depth) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK(s->set_mark("/Books", pbc::Kind::Folder, true).ok());
    PB_CHECK(s->set_mark("/readme.txt", pbc::Kind::File, true).ok());
    PB_CHECK(s->in_scope("/Books"));
    PB_CHECK(s->in_scope("/Books/a.epub"));
    PB_CHECK(s->in_scope("/Books/x/y/z/deep.epub"));
    PB_CHECK(s->in_scope("/readme.txt"));
    PB_CHECK(!s->in_scope("/Books2/a.epub"));        // a similar prefix is another folder
    PB_CHECK(!s->in_scope("/BooksExtra"));
    PB_CHECK(!s->in_scope("/other/readme.txt"));
    PB_CHECK(!s->in_scope("/"));
    PB_CHECK(!s->in_scope("/readme.txt.bak"));
}

PB_TEST(a_mark_on_the_root_covers_everything) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK(s->set_mark("/", pbc::Kind::Folder, true).ok());
    PB_CHECK(s->in_scope("/a/b/c.epub"));
    PB_CHECK(s->in_scope("/x.txt"));
}

PB_TEST(removing_a_mark_never_touches_the_downloaded_copies) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK(s->debug_exec("INSERT INTO local_files(remote_path, local_path, size, mtime, remote_etag, downloaded_at) VALUES('/Books/a', 'Books/a', 1, 2, '\"e\"', 3)").empty());
    PB_CHECK(s->set_mark("/Books", pbc::Kind::Folder, true).ok());
    PB_CHECK(s->set_mark("/Books", pbc::Kind::Folder, false).ok());
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM local_files"), std::string("1"));
}

PB_TEST(paths_are_normalized_and_names_that_are_not_paths_are_refused) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK(s->set_mark("Books/", pbc::Kind::Folder, true).ok());
    PB_CHECK(s->is_marked("/Books"));
    PB_CHECK(!s->set_mark("", pbc::Kind::File, true).ok());   // the root is not a file
}

PB_TEST_MAIN()
