// The trash and versions side of the fake Nextcloud (specs/006-trash-and-versions, tasks T011): it implements contracts/trash-requests.md - the trashbin
// list, the restore to the original place and to a files path, the delete of one item, the version list and the version restore - and the
// capability switches. Test infrastructure: it proves the app against this harness, never against a real server (constitution Principle IX).
#include <string>

#include "fake_nextcloud_server.h"
#include "pbc/json_min.h"
#include "pbc/trash_parse.h"
#include "test_support.h"

PBC_FAKE_SERVER_TEST();

using pbtest::FakeNextcloud;

namespace {

const int64_t kNow = 1792497600;

pbc::Response send(FakeNextcloud& s, const std::string& method, const std::string& path, const std::string& depth = std::string(),
                   const pbc::Headers& extra = {}, const std::string& body = std::string()) {
    pbc::Request r;
    r.method = method;
    r.url = s.base_url() + path;
    r.username = s.login_name();
    r.password = s.app_password();
    if (!depth.empty()) r.headers.push_back({"Depth", depth});
    for (const auto& h : extra) r.headers.push_back(h);
    r.body = body;
    return s.perform(r);
}

const std::string kTrash = "/remote.php/dav/trashbin/demo/trash/";

pbc::TrashList trash_of(FakeNextcloud& s) {
    const auto r = pbc::parse_trash(send(s, "PROPFIND", kTrash, "1"), "demo", kNow);
    if (!r.ok()) PB_FAIL_AT("the trash was refused: " + r.error.key);
    return r.value;
}

pbc::Headers move_to(FakeNextcloud& s, const std::string& destination_path, const char* overwrite = "F") {
    return {{"Destination", s.base_url() + destination_path}, {"Overwrite", overwrite}};
}

}  // namespace

PB_TEST(the_trash_lists_what_was_added_in_the_form_the_parser_reads) {
    FakeNextcloud s;
    s.set_account("demo", "demo", "pw");
    const std::string a = s.add_trash_file("/Books/story.epub", "text", kNow - 100);
    s.add_trash_folder("/Documents/Old Notes", kNow - 200, {{"/a.txt", "a"}, {"/sub/b.txt", "bb"}});
    const auto l = trash_of(s);
    PB_CHECK_EQ(l.items.size(), size_t(2));
    PB_CHECK_EQ(l.items[0].trash_name, a);
    PB_CHECK_EQ(l.items[0].name, std::string("story.epub"));
    PB_CHECK_EQ(l.items[0].original_dir, std::string("/Books"));
    PB_CHECK(l.items[0].original_known && l.items[0].size == std::optional<int64_t>(4));
    PB_CHECK(l.items[1].kind == pbc::Kind::Folder);
    PB_CHECK_EQ(l.items[1].trash_name, std::string("Old Notes.d") + std::to_string(kNow - 200));
    // Depth 0 shows only the collection, a missing Depth is refused like the real server does
    PB_CHECK_EQ(pbc::parse_trash(send(s, "PROPFIND", kTrash, "0"), "demo", kNow).value.items.size(), size_t(0));
    PB_CHECK_EQ(send(s, "PROPFIND", kTrash).status, 403L);
}

PB_TEST(a_delete_through_the_files_endpoint_lands_in_the_trash_with_its_content) {
    FakeNextcloud s;
    s.set_account("demo", "demo", "pw");
    s.add_file("/Docs/a.txt", "hello");
    s.add_file("/Docs/sub/b.txt", "world");
    PB_CHECK_EQ(send(s, "DELETE", "/remote.php/dav/files/demo/Docs/", "").status, 204L);
    PB_CHECK(!s.has("/Docs"));
    const auto l = trash_of(s);
    PB_CHECK_EQ(l.items.size(), size_t(1));
    PB_CHECK(l.items[0].kind == pbc::Kind::Folder && l.items[0].original_dir == "/" && l.items[0].original_name == "Docs");
    PB_CHECK_EQ(send(s, "MOVE", kTrash + pbc::encode_path("/" + l.items[0].trash_name).substr(1), "",
                     {{"Destination", s.base_url() + "/remote.php/dav/trashbin/demo/restore/x"}}).status, 201L);
    PB_CHECK_EQ(s.content_of("/Docs/sub/b.txt"), std::string("world"));   // the whole subtree is back
    PB_CHECK_EQ(s.trashbin_count(), size_t(0));
}

PB_TEST(a_restore_to_the_original_place_never_replaces_and_refuses_a_missing_folder) {
    FakeNextcloud s;
    s.set_account("demo", "demo", "pw");
    s.add_file("/Books/story.epub", "current");
    const std::string taken = s.add_trash_file("/Books/story.epub", "old", kNow - 50);
    const std::string gone = s.add_trash_file("/Nowhere/x.txt", "x", kNow - 60);
    const std::string free_one = s.add_trash_file("/Books/free.txt", "f", kNow - 70);
    const std::string restore = "/remote.php/dav/trashbin/demo/restore/x";
    PB_CHECK_EQ(send(s, "MOVE", kTrash + taken, "", {{"Destination", s.base_url() + restore}, {"Overwrite", "F"}}).status, 412L);
    PB_CHECK_EQ(s.content_of("/Books/story.epub"), std::string("current"));   // untouched
    PB_CHECK_EQ(send(s, "MOVE", kTrash + gone, "", {{"Destination", s.base_url() + restore}, {"Overwrite", "F"}}).status, 409L);
    PB_CHECK_EQ(send(s, "MOVE", kTrash + free_one, "", {{"Destination", s.base_url() + restore}, {"Overwrite", "F"}}).status, 201L);
    PB_CHECK_EQ(s.content_of("/Books/free.txt"), std::string("f"));
    PB_CHECK(s.trashbin_has(taken) && s.trashbin_has(gone) && !s.trashbin_has(free_one));
    PB_CHECK_EQ(send(s, "MOVE", kTrash + "nope.d1", "", {{"Destination", s.base_url() + restore}}).status, 404L);
}

PB_TEST(a_server_that_renames_on_a_collision_restores_under_another_name) {
    FakeNextcloud s;
    s.set_account("demo", "demo", "pw");
    s.add_file("/Books/story.epub", "current");
    const std::string t = s.add_trash_file("/Books/story.epub", "old", kNow - 50);
    s.set_restore_renames(true);
    PB_CHECK_EQ(send(s, "MOVE", kTrash + t, "", {{"Destination", s.base_url() + "/remote.php/dav/trashbin/demo/restore/x"}}).status, 201L);
    PB_CHECK_EQ(s.content_of("/Books/story.epub"), std::string("current"));
    PB_CHECK_EQ(s.content_of("/Books/story (restored).epub"), std::string("old"));
}

PB_TEST(a_restore_to_a_files_path_needs_a_free_name_and_can_be_unsupported) {
    FakeNextcloud s;
    s.set_account("demo", "demo", "pw");
    s.add_file("/Other/exists.txt", "e");
    const std::string t = s.add_trash_file("/Books/a.txt", "a", kNow - 50);
    PB_CHECK_EQ(send(s, "MOVE", kTrash + t, "", move_to(s, "/remote.php/dav/files/demo/Other/exists.txt")).status, 412L);
    PB_CHECK_EQ(send(s, "MOVE", kTrash + t, "", move_to(s, "/remote.php/dav/files/demo/Missing/a.txt")).status, 409L);
    s.set_restore_to_files_status(403);
    PB_CHECK_EQ(send(s, "MOVE", kTrash + t, "", move_to(s, "/remote.php/dav/files/demo/Other/a.txt")).status, 403L);
    PB_CHECK(!s.has("/Other/a.txt") && s.trashbin_has(t));
    s.set_restore_to_files_status(0);
    PB_CHECK_EQ(send(s, "MOVE", kTrash + t, "", move_to(s, "/remote.php/dav/files/demo/Other/a.txt")).status, 201L);
    PB_CHECK_EQ(s.content_of("/Other/a.txt"), std::string("a"));
}

PB_TEST(a_trash_item_is_deleted_by_its_name_a_locked_one_is_refused) {
    FakeNextcloud s;
    s.set_account("demo", "demo", "pw");
    const std::string a = s.add_trash_file("/a.txt", "a", kNow - 10);
    const std::string b = s.add_trash_file("/b.txt", "b", kNow - 20);
    s.lock_trash_item(b);
    PB_CHECK_EQ(send(s, "DELETE", kTrash + b).status, 423L);
    PB_CHECK_EQ(send(s, "DELETE", kTrash + a).status, 204L);
    PB_CHECK_EQ(send(s, "DELETE", kTrash + a).status, 404L);
    PB_CHECK_EQ(s.trashbin_count(), size_t(1));
}

PB_TEST(the_trash_endpoint_is_absent_when_the_trash_app_is_off) {
    FakeNextcloud s;
    s.set_account("demo", "demo", "pw");
    s.add_trash_file("/a.txt", "a", kNow - 10);
    s.set_trash(false);
    PB_CHECK_EQ(send(s, "PROPFIND", kTrash, "1").status, 404L);
    s.set_trash(true);
    PB_CHECK_EQ(send(s, "PROPFIND", kTrash, "1").status, 207L);
    pbc::Request r;   // another user's trash and a wrong password are refused
    r.method = "PROPFIND";
    r.url = s.base_url() + "/remote.php/dav/trashbin/other/trash/";
    r.username = s.login_name();
    r.password = s.app_password();
    r.headers = {{"Depth", "1"}};
    PB_CHECK_EQ(s.perform(r).status, 404L);
    r.url = s.base_url() + kTrash;
    r.password = "wrong";
    PB_CHECK_EQ(s.perform(r).status, 401L);
}

PB_TEST(the_capabilities_state_each_app_and_can_fail_or_lack_the_files_object) {
    FakeNextcloud s;
    s.set_account("demo", "demo", "pw");
    const auto caps = [&](bool* ok) {
        pbc::Request r;
        r.method = "GET";
        r.url = s.base_url() + "/ocs/v1.php/cloud/capabilities?format=json";
        r.username = "demo";
        r.password = "pw";
        const auto resp = s.perform(r);
        *ok = resp.status == 200;
        return resp.body;
    };
    bool ok = false;
    std::string body = caps(&ok);
    PB_CHECK(ok && body.find("\"undelete\":true") != std::string::npos && body.find("\"versioning\":true") != std::string::npos);
    s.set_trash(false);
    s.set_versions(std::nullopt);
    body = caps(&ok);
    PB_CHECK(body.find("\"undelete\":false") != std::string::npos && body.find("versioning") == std::string::npos);
    s.set_capabilities_without_files(true);
    body = caps(&ok);
    PB_CHECK(ok && body.find("\"files\"") == std::string::npos);
    s.set_capabilities_failing(true);
    caps(&ok);
    PB_CHECK(!ok);
}

PB_TEST(the_version_list_shows_the_older_versions_and_a_restore_keeps_the_current_content) {
    FakeNextcloud s;
    s.set_account("demo", "demo", "pw");
    s.add_file("/Books/a.txt", "v3", 1760000300);
    s.add_version("/Books/a.txt", "v1", 1760000100);
    s.add_version("/Books/a.txt", "v2", 1760000200);
    const int64_t id = s.nodes().at("/Books/a.txt").fileid;
    const std::string base = "/remote.php/dav/versions/demo/versions/" + std::to_string(id) + "/";
    const auto l = pbc::parse_versions(send(s, "PROPFIND", base, "1"), kNow);
    PB_CHECK(l.ok());
    PB_CHECK_EQ(l.value.rows.size(), size_t(2));   // the current content is not a row
    PB_CHECK_EQ(l.value.rows[0].version_id, std::string("1760000200"));
    PB_CHECK(l.value.rows[0].size == std::optional<int64_t>(2));
    const std::string etag_before = s.nodes().at("/Books/a.txt").etag;
    const auto restore = send(s, "MOVE", base + "1760000100", "", {{"Destination", s.base_url() + "/remote.php/dav/versions/demo/restore/target"}});
    PB_CHECK_EQ(restore.status, 201L);
    PB_CHECK_EQ(s.content_of("/Books/a.txt"), std::string("v1"));
    PB_CHECK(s.nodes().at("/Books/a.txt").etag != etag_before);
    PB_CHECK_EQ(s.versions_of("/Books/a.txt").size(), size_t(3));   // v3 was added as a version
    PB_CHECK_EQ(s.versions_of("/Books/a.txt").back().content, std::string("v3"));
    PB_CHECK_EQ(send(s, "MOVE", base + "999", "", {{"Destination", s.base_url() + "/remote.php/dav/versions/demo/restore/target"}}).status, 404L);
    s.set_versions_keep_current(false);
    PB_CHECK_EQ(send(s, "MOVE", base + "1760000200", "", {{"Destination", s.base_url() + "/remote.php/dav/versions/demo/restore/target"}}).status, 201L);
    PB_CHECK_EQ(s.versions_of("/Books/a.txt").size(), size_t(3));   // nothing was added this time
    s.set_versions(false);
    PB_CHECK_EQ(send(s, "PROPFIND", base, "1").status, 404L);
}

PB_TEST_MAIN()
