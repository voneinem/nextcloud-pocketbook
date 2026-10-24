// A file that changed after its versions were listed is not restored over (specs/006-trash-and-versions, spec Story 3 scenario 5, FR-018, research R-9), against the
// in-process fake Nextcloud: the stale check reads the file's ETag again immediately before the request; a different one refuses, sends nothing and keeps the newer content.
// It proves the app follows the contract, never a real server (constitution Principle IX).
#include <string>

#include "test_support.h"
#include "trash_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::TrashEnv;

namespace {

struct Setup {
    TrashEnv e;
    pbc::RemoteEntry file;
    pbc::VersionList shown;
    Setup() {
        e.server.add_file("/Books/a.txt", "current", 1760000300);
        e.server.add_version("/Books/a.txt", "v1", 1760000100);
        const auto n = e.server.nodes().at("/Books/a.txt");
        file.path = "/Books/a.txt";
        file.name = "a.txt";
        file.kind = pbc::Kind::File;
        file.fileid = n.fileid;
        file.etag = n.etag;
        file.mtime = n.mtime;
        shown = e.api.list_versions(file).value;
    }
};

}  // namespace

PB_TEST(a_file_changed_after_the_list_was_read_is_refused_and_nothing_is_sent) {
    Setup s;
    s.e.server.set_file("/Books/a.txt", "changed on another device", 1760000400);
    s.e.server.clear_log();
    const auto r = s.e.api.restore_version(s.file, s.shown, s.shown.rows[0]);
    PB_CHECK_EQ(r.error.key, std::string("version_changed_meanwhile"));
    PB_CHECK_EQ(s.e.moves(), size_t(0));
    PB_CHECK_EQ(s.e.server.request_count(), size_t(1));   // only the stale check
    PB_CHECK_EQ(s.e.server.content_of("/Books/a.txt"), std::string("changed on another device"));
    PB_CHECK_EQ(s.e.server.versions_of("/Books/a.txt").size(), size_t(1));
}

PB_TEST(a_change_that_happens_between_the_check_and_the_request_is_the_residual_race_and_loses_nothing) {
    Setup s;
    // the file changes right after the stale check, before the MOVE: the server keeps the content that was current as a version either way
    s.e.server.before_next("MOVE", "/versions/", [&] { s.e.server.set_file("/Books/a.txt", "the newest", 1760000500); });
    const auto r = s.e.api.restore_version(s.file, s.shown, s.shown.rows[0]);
    PB_CHECK(r.error.ok());
    // the content that was current at the moment of the restore is a version: the newer edit is not lost
    bool kept = false;
    for (const auto& v : s.e.server.versions_of("/Books/a.txt")) kept = kept || v.content == "the newest";
    PB_CHECK(kept);
}

PB_TEST(a_file_that_vanished_after_the_list_was_read_gives_file_gone_and_sends_nothing) {
    Setup s;
    s.e.server.remove("/Books/a.txt");
    s.e.server.clear_log();
    const auto r = s.e.api.restore_version(s.file, s.shown, s.shown.rows[0]);
    PB_CHECK_EQ(r.error.key, std::string("file_gone"));
    PB_CHECK_EQ(s.e.moves(), size_t(0));
}

PB_TEST(a_list_without_a_stamp_cannot_be_restored_from) {
    Setup s;
    pbc::VersionList no_stamp = s.shown;
    no_stamp.file_etag.clear();
    s.e.server.clear_log();
    PB_CHECK_EQ(s.e.api.restore_version(s.file, no_stamp, s.shown.rows[0]).error.key, std::string("version_changed_meanwhile"));
    PB_CHECK_EQ(s.e.moves(), size_t(0));
}

PB_TEST(listing_again_after_a_refusal_gives_a_fresh_stamp_and_the_restore_then_works) {
    Setup s;
    s.e.server.set_file("/Books/a.txt", "changed", 1760000400);
    PB_CHECK(!s.e.api.restore_version(s.file, s.shown, s.shown.rows[0]).error.ok());
    auto fresh = s.file;
    fresh.etag = s.e.server.nodes().at("/Books/a.txt").etag;
    const auto again = s.e.api.list_versions(fresh);
    PB_CHECK(again.ok());
    PB_CHECK(s.e.api.restore_version(fresh, again.value, again.value.rows[0]).error.ok());
    PB_CHECK_EQ(s.e.server.content_of("/Books/a.txt"), std::string("v1"));
}

PB_TEST_MAIN()
