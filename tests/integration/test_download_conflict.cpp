// A local copy that the user changed is never overwritten (spec FR-021, SC-009; constitution Principle III): the download stops before
// it starts and asks; the user keeps the copy, keeps both, or replaces it after a second confirmation.
#include <string>

#include "download_env.h"
#include "pbc/local_files.h"

PBC_FAKE_SERVER_TEST();

using pbc::ConflictPolicy;
using pbc::JobState;
using pbtest::DlEnv;

namespace {

std::string pattern(size_t n, int seed) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 31 + static_cast<size_t>(seed)) & 0xff);
    return s;
}

// a downloaded file that the user then edited on the reader
void downloaded_and_edited(DlEnv& e, const std::string& edited = "my notes in the margin") {
    e.server.add_file("/Books/a.txt", pattern(1000, 1));
    PB_CHECK(e.run("/Books/a.txt").state == JobState::Done);
    e.write_local("Books/a.txt", edited);   // a different size: the edit shows
    e.server.set_file("/Books/a.txt", pattern(2000, 2), 1790009999);   // and the server has a newer version
    e.server.clear_log();
}

}  // namespace

PB_TEST(a_changed_copy_is_not_overwritten_and_the_job_stops_before_the_first_request) {
    DlEnv e;
    downloaded_and_edited(e);
    const auto out = e.run("/Books/a.txt");
    PB_CHECK(out.state == JobState::Blocked);
    PB_CHECK_EQ(out.error_key, std::string("local_changed"));
    PB_CHECK_EQ(e.read("Books/a.txt"), std::string("my notes in the margin"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(e.store->journal().empty());
    PB_CHECK(!e.any_part_file());
}

PB_TEST(keep_mine_changes_no_file_and_remembers_the_server_version_it_was_shown) {
    DlEnv e;
    downloaded_and_edited(e);
    PB_CHECK(pbc::keep_mine(*e.store, "/Books/a.txt", e.server.etag_of("/Books/a.txt")).ok());
    PB_CHECK_EQ(e.read("Books/a.txt"), std::string("my notes in the margin"));
    PB_CHECK_EQ(e.store->local_file("/Books/a.txt")->seen_etag, e.server.etag_of("/Books/a.txt"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(keep_both_saves_the_server_version_under_another_name_and_leaves_the_users_copy) {
    DlEnv e;
    downloaded_and_edited(e);
    const auto out = e.run("/Books/a.txt", ConflictPolicy::KeepBoth);
    PB_CHECK(out.state == JobState::Done);
    PB_CHECK_EQ(out.local_path, std::string("Books/a (server copy).txt"));
    PB_CHECK_EQ(e.read("Books/a.txt"), std::string("my notes in the margin"));
    PB_CHECK(e.read("Books/a (server copy).txt") == pattern(2000, 2));
    PB_CHECK_EQ(e.store->local_file("/Books/a.txt")->local_path, std::string("Books/a.txt"));   // the record still describes the user's copy
    PB_CHECK(!e.any_part_file());
    // a second time: another collision-safe name, nothing replaced
    const auto again = e.run("/Books/a.txt", ConflictPolicy::KeepBoth);
    PB_CHECK_EQ(again.local_path, std::string("Books/a (server copy) (2).txt"));
    PB_CHECK(e.read("Books/a (server copy).txt") == pattern(2000, 2));
}

PB_TEST(replace_overwrites_atomically_and_records_the_new_version) {
    DlEnv e;
    downloaded_and_edited(e);
    const auto out = e.run("/Books/a.txt", ConflictPolicy::Replace);
    PB_CHECK(out.state == JobState::Done);
    PB_CHECK(e.read("Books/a.txt") == pattern(2000, 2));
    const auto lf = e.store->local_file("/Books/a.txt");
    PB_CHECK_EQ(lf->remote_etag, e.server.etag_of("/Books/a.txt"));
    PB_CHECK_EQ(lf->seen_etag, std::string(""));
    PB_CHECK(!e.any_part_file());
    PB_CHECK_EQ(e.tree().size(), size_t(1));
}

PB_TEST(an_unchanged_copy_is_replaced_by_a_newer_version_without_asking) {
    DlEnv e;
    e.server.add_file("/a.txt", pattern(1000, 1));
    PB_CHECK(e.run("/a.txt").state == JobState::Done);
    e.server.set_file("/a.txt", pattern(1500, 2), 1790009999);
    const auto out = e.run("/a.txt");
    PB_CHECK(out.state == JobState::Done);
    PB_CHECK(e.read("a.txt") == pattern(1500, 2));
    PB_CHECK_EQ(e.tree().size(), size_t(1));
}

PB_TEST(a_file_on_the_card_that_the_app_never_recorded_is_treated_as_changed_not_as_free) {
    DlEnv e;
    e.server.add_file("/Books/a.txt", pattern(1000, 1));
    e.write_local("Books/a.txt", "the user put this here");
    const auto out = e.run("/Books/a.txt");
    // the name is taken by a file that is not ours: the download goes to a collision-safe name, never over it
    PB_CHECK(out.state == JobState::Done);
    PB_CHECK_EQ(e.read("Books/a.txt"), std::string("the user put this here"));
    PB_CHECK_EQ(out.local_path, std::string("Books/a (2).txt"));
}

PB_TEST(a_copy_that_is_gone_from_the_card_is_downloaded_again_to_the_same_place) {
    DlEnv e;
    e.server.add_file("/a.txt", pattern(1000, 1));
    PB_CHECK(e.run("/a.txt").state == JobState::Done);
    ::remove(e.abs("a.txt").c_str());
    const auto out = e.run("/a.txt");
    PB_CHECK(out.state == JobState::Done);
    PB_CHECK_EQ(out.local_path, std::string("a.txt"));
    PB_CHECK(e.read("a.txt") == pattern(1000, 1));
}

PB_TEST_MAIN()
