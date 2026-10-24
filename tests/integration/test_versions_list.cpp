// Listing the older versions of a file (specs/006-trash-and-versions, spec Story 3, FR-015, research R-3 and R-9, contracts/trash-requests.md T3 and T6), against the
// in-process fake Nextcloud: date and size, newest first, the current content is not a row, the stamp for the stale check. It proves the app follows the contract, never
// a real server (constitution Principle IX).
#include <string>

#include "fixtures.h"

#include "test_support.h"
#include "trash_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::TrashEnv;

namespace {

pbc::RemoteEntry entry_of(TrashEnv& e, const std::string& path) {
    const auto n = e.server.nodes().at(path);
    pbc::RemoteEntry r;
    r.path = path;
    r.name = path.substr(path.rfind('/') + 1);
    r.kind = n.folder ? pbc::Kind::Folder : pbc::Kind::File;
    r.fileid = n.fileid;
    r.etag = n.etag;
    r.mtime = n.mtime;
    r.size = n.size();
    return r;
}

}  // namespace

PB_TEST(the_older_versions_are_listed_newest_first_with_date_and_size_and_the_current_content_is_not_a_row) {
    TrashEnv e;
    e.server.add_file("/Books/a.txt", "current!", 1760000300);
    e.server.add_version("/Books/a.txt", "v1", 1760000100);
    e.server.add_version("/Books/a.txt", "version two", 1760000200);
    const auto file = entry_of(e, "/Books/a.txt");
    e.server.clear_log();
    const auto r = e.api.list_versions(file);
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.rows.size(), size_t(2));
    PB_CHECK_EQ(r.value.rows[0].version_id, std::string("1760000200"));
    PB_CHECK(r.value.rows[0].mtime == std::optional<int64_t>(1760000200));
    PB_CHECK(r.value.rows[0].size == std::optional<int64_t>(11));
    PB_CHECK_EQ(r.value.rows[1].version_id, std::string("1760000100"));
    PB_CHECK(!r.value.truncated && r.value.skipped == 0);
    // the stamp of the stale check: the file's ETag and id as the server has them now
    PB_CHECK_EQ(r.value.file_etag, e.server.nodes().at("/Books/a.txt").etag);
    PB_CHECK_EQ(r.value.file_id, *file.fileid);
    // two requests: the file (Depth 0) and its versions (Depth 1)
    PB_CHECK_EQ(e.server.request_count(), size_t(2));
    PB_CHECK_EQ(e.server.log()[0].depth, std::string("0"));
    PB_CHECK(e.server.log()[0].path.find("/dav/files/demo/Books/a.txt") != std::string::npos);
    PB_CHECK_EQ(e.server.log()[1].depth, std::string("1"));
    PB_CHECK(e.server.log()[1].path.find("/dav/versions/demo/versions/" + std::to_string(*file.fileid) + "/") != std::string::npos);
}

PB_TEST(a_file_without_older_versions_gives_an_empty_list_not_an_error) {
    TrashEnv e;
    e.server.add_file("/Books/a.txt", "current");
    const auto r = e.api.list_versions(entry_of(e, "/Books/a.txt"));
    PB_CHECK(r.ok() && r.value.rows.empty());
    PB_CHECK(!r.value.file_etag.empty());
}

PB_TEST(an_endpoint_that_knows_no_versions_of_the_file_is_the_same_empty_list) {
    TrashEnv e;
    e.server.add_file("/Books/a.txt", "current");
    const auto file = entry_of(e, "/Books/a.txt");
    e.server.answer_next("PROPFIND", "/versions/", 404);   // the file is there (the probe said so) and the versions endpoint has nothing
    const auto r = e.api.list_versions(file);
    PB_CHECK(r.ok() && r.value.rows.empty());
}

PB_TEST(a_folder_a_file_without_an_id_and_a_vanished_file_send_nothing_or_nothing_more) {
    TrashEnv e;
    e.server.add_folder("/Books");
    e.server.add_file("/Books/a.txt", "current");
    e.server.clear_log();
    const auto folder = e.api.list_versions(entry_of(e, "/Books"));
    PB_CHECK_EQ(folder.error.key, std::string("trash_not_actionable"));
    auto no_id = entry_of(e, "/Books/a.txt");
    no_id.fileid.reset();
    PB_CHECK_EQ(e.api.list_versions(no_id).error.key, std::string("refresh_first"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    auto gone = entry_of(e, "/Books/a.txt");
    e.server.remove("/Books/a.txt");
    PB_CHECK_EQ(e.api.list_versions(gone).error.key, std::string("file_gone"));
    PB_CHECK_EQ(e.server.count("PROPFIND"), size_t(1));   // the probe of the file, nothing more
}

PB_TEST(failures_give_their_key_and_a_code) {
    TrashEnv e;
    e.server.add_file("/Books/a.txt", "current");
    e.server.add_version("/Books/a.txt", "old", 1760000100);
    const auto file = entry_of(e, "/Books/a.txt");
    e.server.answer_next("PROPFIND", "/versions/", 500);
    auto r = e.api.list_versions(file);
    PB_CHECK_EQ(r.error.key, std::string("server_error"));
    PB_CHECK_EQ(r.value.code, std::string("T6-500"));
    e.server.answer_next("PROPFIND", "/versions/", 401);
    PB_CHECK_EQ(e.api.list_versions(file).error.key, std::string("auth_failed"));
    e.server.answer_next("PROPFIND", "/versions/", 403);
    PB_CHECK_EQ(e.api.list_versions(file).error.key, std::string("forbidden"));
    e.server.set_offline(true);
    PB_CHECK(e.api.list_versions(file).error.cls == pbc::ErrorClass::Network);
}

PB_TEST(a_hostile_answer_is_bounded_and_skipped_rows_are_counted) {
    TrashEnv e;
    e.server.add_file("/Books/a.txt", "current");
    const auto file = entry_of(e, "/Books/a.txt");
    e.server.set_interceptor([](const pbc::Request& r) -> std::optional<pbc::Response> {
        if (r.method != "PROPFIND" || r.url.find("/versions/") == std::string::npos) return std::nullopt;
        pbc::Response resp;
        resp.status = 207;
        resp.body = pbtest::fixture("nextcloud/trash/versions_list_hostile.unverified.xml");
        return resp;
    });
    const auto r = e.api.list_versions(file);
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.rows.size(), size_t(4));
    PB_CHECK_EQ(r.value.skipped, 3);
}

PB_TEST_MAIN()
