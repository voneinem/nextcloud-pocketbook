// The write side of the fake Nextcloud (specs/002-manage-server-files, contracts/server-requests.md W3-W9): the harness implements the
// contract the app is tested against, so it is tested itself. What this proves is the harness, never a real server (PBC_FAKE_SERVER_TEST).
#include <fcntl.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "fake_nextcloud_server.h"
#include "test_support.h"
#include "tmp_dir.h"

PBC_FAKE_SERVER_TEST();

using pbtest::FakeNextcloud;

namespace {

struct Env {
    FakeNextcloud server;
    pbtest::TmpDir dir;
    std::string files = "/remote.php/dav/files/demo";
    std::string uploads = "/remote.php/dav/uploads/demo";

    pbc::Request req(const std::string& method, const std::string& path, pbc::Headers headers = {}, const std::string& body = std::string()) const {
        pbc::Request r;
        r.method = method;
        r.url = server.base_url() + path;
        r.headers = std::move(headers);
        r.body = body;
        r.username = "demo";
        r.password = "app-pass-0001";
        return r;
    }
    pbc::Response send(const pbc::Request& r) { return server.perform(r); }
    pbc::Response put(const std::string& path, const std::string& body, pbc::Headers headers = {}) {
        return send(req("PUT", files + path, std::move(headers), body));
    }
    pbc::Response put_file(const std::string& path, const std::string& src, long long offset, long long length, pbc::Headers headers = {}) {
        pbc::Request r = req("PUT", path, std::move(headers));
        r.body_file = src;
        r.body_offset = offset;
        r.body_length = length;
        return send(r);
    }
    std::string url(const std::string& path) const { return server.base_url() + files + path; }
    std::string content(const std::string& path) { return server.node(path).content; }
};

std::string header(const pbc::Response& r, const char* name) { return pbc::header_value(r.headers, name); }

}  // namespace

PB_TEST(put_creates_a_file_with_the_headers_of_the_contract_and_refuses_an_existing_name_with_if_none_match) {
    Env e;
    e.server.add_folder("/Docs");
    auto r = e.put("/Docs/a.txt", "hello", {{"If-None-Match", "*"}, {"X-OC-MTime", "1790000123"}});
    PB_CHECK_EQ(r.status, 201L);
    PB_CHECK(!header(r, "OC-Etag").empty());
    PB_CHECK(!header(r, "OC-FileId").empty());
    PB_CHECK_EQ(e.content("/Docs/a.txt"), std::string("hello"));
    PB_CHECK_EQ(e.server.node("/Docs/a.txt").mtime, 1790000123LL);
    PB_CHECK_EQ(header(r, "OC-Etag"), e.server.etag_of("/Docs/a.txt"));
    r = e.put("/Docs/a.txt", "other", {{"If-None-Match", "*"}});
    PB_CHECK_EQ(r.status, 412L);
    PB_CHECK_EQ(e.content("/Docs/a.txt"), std::string("hello"));   // nothing replaced
}

PB_TEST(put_replaces_only_with_a_matching_strong_etag_and_the_parent_must_exist) {
    Env e;
    e.server.add_file("/a.txt", "v1");
    const std::string etag = e.server.etag_of("/a.txt");
    PB_CHECK_EQ(e.put("/a.txt", "v2", {{"If-Match", "\"stale\""}}).status, 412L);
    PB_CHECK_EQ(e.content("/a.txt"), std::string("v1"));
    PB_CHECK_EQ(e.put("/a.txt", "v2", {{"If-Match", "W/" + etag}}).status, 412L);   // a weak validator never matches
    auto r = e.put("/a.txt", "v2", {{"If-Match", etag}});
    PB_CHECK_EQ(r.status, 204L);
    PB_CHECK_EQ(e.content("/a.txt"), std::string("v2"));
    PB_CHECK(e.server.etag_of("/a.txt") != etag);
    PB_CHECK_EQ(e.put("/missing/a.txt", "x", {{"If-None-Match", "*"}}).status, 409L);
    PB_CHECK_EQ(e.put("/gone.txt", "x", {{"If-Match", etag}}).status, 412L);   // If-Match on a file that does not exist
    // a PUT without any condition overwrites silently, as the real server does; the app never sends one
    PB_CHECK_EQ(e.put("/a.txt", "v3").status, 204L);
    PB_CHECK_EQ(e.content("/a.txt"), std::string("v3"));
}

PB_TEST(put_streams_a_range_of_a_file_and_counts_the_bytes) {
    Env e;
    e.dir.write("src.bin", std::string(3 << 20, 'a') + std::string(1 << 20, 'b'));
    auto r = e.put_file(e.files + "/range.bin", e.dir.file("src.bin"), 3 << 20, 1 << 20, {{"If-None-Match", "*"}});
    PB_CHECK_EQ(r.status, 201L);
    PB_CHECK_EQ(e.content("/range.bin"), std::string(1 << 20, 'b'));
    PB_CHECK_EQ(e.server.bytes_uploaded(), 1LL << 20);
    PB_CHECK_EQ(e.server.sha256_of("/range.bin").size(), size_t(64));
}

PB_TEST(a_cancel_or_a_dropped_body_stores_nothing) {
    Env e;
    e.dir.write("src.bin", std::string(4 << 20, 'z'));
    pbc::Request cancel = e.req("PUT", e.files + "/c.bin", {{"If-None-Match", "*"}});
    cancel.body_file = e.dir.file("src.bin");
    cancel.body_length = 4 << 20;
    cancel.progress = [](long long done, long long) { return done < (1 << 20); };
    auto r = e.send(cancel);
    PB_CHECK(r.cancelled);
    PB_CHECK(r.delivery == pbc::Delivery::PartlySent);
    PB_CHECK(!e.server.has("/c.bin"));

    e.server.drop_after_bytes(1 << 20);
    r = e.put_file(e.files + "/d.bin", e.dir.file("src.bin"), 0, 4 << 20, {{"If-None-Match", "*"}});
    PB_CHECK_EQ(r.status, 0L);
    PB_CHECK(r.delivery == pbc::Delivery::PartlySent);
    PB_CHECK(!e.server.has("/d.bin"));
    // the cut applies once
    r = e.put_file(e.files + "/d.bin", e.dir.file("src.bin"), 0, 4 << 20, {{"If-None-Match", "*"}});
    PB_CHECK_EQ(r.status, 201L);
}

PB_TEST(mkcol_creates_a_folder_and_refuses_an_existing_name_or_a_missing_parent) {
    Env e;
    auto r = e.send(e.req("MKCOL", e.files + "/New/"));
    PB_CHECK_EQ(r.status, 201L);
    PB_CHECK(e.server.node("/New").folder);
    PB_CHECK(!header(r, "OC-Etag").empty());
    PB_CHECK_EQ(e.send(e.req("MKCOL", e.files + "/New/")).status, 405L);
    PB_CHECK_EQ(e.send(e.req("MKCOL", e.files + "/Nope/Sub/")).status, 409L);
}

PB_TEST(move_and_copy_need_a_matching_source_etag_and_overwrite_f_protects_the_target) {
    Env e;
    e.server.add_file("/A/f.txt", "f");
    e.server.add_file("/B/g.txt", "g");
    const std::string etag = e.server.etag_of("/A/f.txt");
    const int64_t id = e.server.node("/A/f.txt").fileid;
    // the source changed
    PB_CHECK_EQ(e.send(e.req("MOVE", e.files + "/A/f.txt", {{"Destination", e.url("/B/f.txt")}, {"Overwrite", "F"}, {"If-Match", "\"stale\""}})).status, 412L);
    PB_CHECK(e.server.has("/A/f.txt"));
    // the target exists and Overwrite is F
    PB_CHECK_EQ(e.send(e.req("MOVE", e.files + "/A/f.txt", {{"Destination", e.url("/B/g.txt")}, {"Overwrite", "F"}, {"If-Match", etag}})).status, 412L);
    PB_CHECK_EQ(e.content("/B/g.txt"), std::string("g"));
    // a move keeps the file id; a copy gets a new one
    auto r = e.send(e.req("MOVE", e.files + "/A/f.txt", {{"Destination", e.url("/B/f.txt")}, {"Overwrite", "F"}, {"If-Match", etag}}));
    PB_CHECK_EQ(r.status, 201L);
    PB_CHECK(!e.server.has("/A/f.txt"));
    PB_CHECK_EQ(e.server.node("/B/f.txt").fileid, id);
    r = e.send(e.req("COPY", e.files + "/B/f.txt", {{"Destination", e.url("/B/copy.txt")}, {"Overwrite", "F"}}));
    PB_CHECK_EQ(r.status, 201L);
    PB_CHECK(e.server.node("/B/copy.txt").fileid != id);
    PB_CHECK(e.server.has("/B/f.txt"));
    // Overwrite T replaces the target
    r = e.send(e.req("COPY", e.files + "/B/f.txt", {{"Destination", e.url("/B/g.txt")}, {"Overwrite", "T"}}));
    PB_CHECK_EQ(r.status, 204L);
    PB_CHECK_EQ(e.content("/B/g.txt"), std::string("f"));
    PB_CHECK_EQ(e.server.overwrites(), 1);
    // the destination must be a full URL of the account
    PB_CHECK_EQ(e.send(e.req("MOVE", e.files + "/B/f.txt", {{"Destination", "/remote.php/dav/files/demo/x"}, {"Overwrite", "F"}})).status, 400L);
    PB_CHECK_EQ(e.send(e.req("MOVE", e.files + "/B/nope.txt", {{"Destination", e.url("/B/y")}, {"Overwrite", "F"}})).status, 404L);
    PB_CHECK_EQ(e.send(e.req("MOVE", e.files + "/B/f.txt", {{"Destination", e.url("/Missing/y")}, {"Overwrite", "F"}})).status, 409L);
}

PB_TEST(a_folder_moves_and_copies_with_its_content_and_never_into_itself) {
    Env e;
    e.server.add_file("/A/dir/one.txt", "1");
    e.server.add_file("/A/dir/sub/two.txt", "2");
    e.server.add_folder("/B");
    PB_CHECK_EQ(e.send(e.req("MOVE", e.files + "/A/dir/", {{"Destination", e.url("/A/dir/sub/dir")}, {"Overwrite", "F"}})).status, 403L);
    PB_CHECK_EQ(e.send(e.req("COPY", e.files + "/A/dir/", {{"Destination", e.url("/B/dir")}, {"Overwrite", "F"}})).status, 201L);
    PB_CHECK(e.server.has("/A/dir/sub/two.txt"));
    PB_CHECK_EQ(e.content("/B/dir/sub/two.txt"), std::string("2"));
    PB_CHECK_EQ(e.send(e.req("MOVE", e.files + "/A/dir/", {{"Destination", e.url("/B/moved")}, {"Overwrite", "F"}})).status, 201L);
    PB_CHECK(!e.server.has("/A/dir"));
    PB_CHECK_EQ(e.content("/B/moved/one.txt"), std::string("1"));
    // replacing a folder with Overwrite: T removes it with everything inside (the reason the app never offers it)
    PB_CHECK_EQ(e.send(e.req("MOVE", e.files + "/B/moved/", {{"Destination", e.url("/B/dir")}, {"Overwrite", "T"}})).status, 204L);
    PB_CHECK_EQ(e.content("/B/dir/one.txt"), std::string("1"));
}

PB_TEST(delete_needs_a_matching_etag_and_goes_to_the_trash_or_is_permanent_without_one) {
    Env e;
    e.server.add_file("/a.txt", "a");
    e.server.add_file("/dir/b.txt", "b");
    PB_CHECK_EQ(e.send(e.req("DELETE", e.files + "/a.txt", {{"If-Match", "\"stale\""}})).status, 412L);
    PB_CHECK(e.server.has("/a.txt"));
    PB_CHECK_EQ(e.send(e.req("DELETE", e.files + "/a.txt", {{"If-Match", e.server.etag_of("/a.txt")}})).status, 204L);
    PB_CHECK(!e.server.has("/a.txt"));
    PB_CHECK(e.server.trash_has("/a.txt"));
    PB_CHECK_EQ(e.send(e.req("DELETE", e.files + "/dir/")).status, 204L);   // a folder with its content
    PB_CHECK(e.server.trash_has("/dir") && e.server.trash_has("/dir/b.txt"));
    PB_CHECK_EQ(e.send(e.req("DELETE", e.files + "/a.txt")).status, 404L);
    PB_CHECK_EQ(e.server.permanent_deletes(), 0);
    e.server.set_trash(false);
    e.server.add_file("/c.txt", "c");
    PB_CHECK_EQ(e.send(e.req("DELETE", e.files + "/c.txt")).status, 204L);
    PB_CHECK_EQ(e.server.permanent_deletes(), 1);
}

PB_TEST(a_locked_file_answers_423_and_a_folder_with_a_locked_member_answers_207_and_changes_nothing) {
    Env e;
    e.server.add_file("/L/locked.txt", "l");
    e.server.add_file("/L/free.txt", "f");
    e.server.lock("/L/locked.txt");
    PB_CHECK_EQ(e.put("/L/locked.txt", "x", {{"If-Match", e.server.etag_of("/L/locked.txt")}}).status, 423L);
    PB_CHECK_EQ(e.send(e.req("DELETE", e.files + "/L/locked.txt")).status, 423L);
    PB_CHECK_EQ(e.send(e.req("MOVE", e.files + "/L/locked.txt", {{"Destination", e.url("/L/m.txt")}, {"Overwrite", "F"}})).status, 423L);
    auto r = e.send(e.req("DELETE", e.files + "/L/"));
    PB_CHECK_EQ(r.status, 207L);
    PB_CHECK(r.body.find("423") != std::string::npos);
    PB_CHECK(e.server.has("/L/free.txt"));   // nothing was deleted
    r = e.send(e.req("MOVE", e.files + "/L/", {{"Destination", e.url("/M")}, {"Overwrite", "F"}}));
    PB_CHECK_EQ(r.status, 207L);
    PB_CHECK(e.server.has("/L/free.txt") && !e.server.has("/M"));
}

PB_TEST(quota_size_type_and_name_limits_answer_with_their_statuses_and_a_sabre_body) {
    Env e;
    e.server.add_file("/used.bin", std::string(600, 'u'));
    e.server.set_quota_limit(1000);
    auto r = e.put("/big.bin", std::string(500, 'x'), {{"If-None-Match", "*"}});
    PB_CHECK_EQ(r.status, 507L);
    PB_CHECK(r.body.find("InsufficientStorage") != std::string::npos);
    PB_CHECK(!e.server.has("/big.bin"));
    PB_CHECK_EQ(e.put("/ok.bin", std::string(300, 'x'), {{"If-None-Match", "*"}}).status, 201L);
    e.server.set_quota_limit(std::nullopt);
    e.server.set_max_file_size(100);
    r = e.put("/huge.bin", std::string(101, 'x'), {{"If-None-Match", "*"}});
    PB_CHECK_EQ(r.status, 413L);
    PB_CHECK(r.body.find("EntityTooLarge") != std::string::npos);
    e.server.set_max_file_size(std::nullopt);
    e.server.set_blocked_extensions({".exe"});
    r = e.put("/run.exe", "x", {{"If-None-Match", "*"}});
    PB_CHECK_EQ(r.status, 415L);
    PB_CHECK(r.body.find("UnsupportedMediaType") != std::string::npos);
    e.server.set_forbidden("?|", {".htaccess"}, {"con"}, {"part"});
    for (const char* bad : {"/what%3F.txt", "/pipe%7C.txt", "/.htaccess", "/CON.txt", "/file.part"}) {
        r = e.put(bad, "x", {{"If-None-Match", "*"}});
        PB_CHECK_EQ(r.status, 400L);
        PB_CHECK(r.body.find("InvalidPath") != std::string::npos);
    }
    PB_CHECK_EQ(e.send(e.req("MKCOL", e.files + "/bad%3F/")).status, 400L);
}

PB_TEST(propfind_reports_the_quota_from_the_limit_and_the_capabilities_state_the_write_side) {
    Env e;
    e.server.add_file("/a.bin", std::string(250, 'a'));
    e.server.set_quota_limit(1000);
    auto r = e.send(e.req("PROPFIND", e.files + "/", {{"Depth", "0"}}, "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:quota-available-bytes/><d:quota-used-bytes/></d:prop></d:propfind>"));
    PB_CHECK_EQ(r.status, 207L);
    PB_CHECK(r.body.find("<d:quota-used-bytes>250<") != std::string::npos);
    PB_CHECK(r.body.find("<d:quota-available-bytes>750<") != std::string::npos);

    e.server.set_forbidden("?", {"x.txt"});
    e.server.set_max_chunk(7340032);
    r = e.send(e.req("GET", "/ocs/v1.php/cloud/capabilities?format=json"));
    PB_CHECK_EQ(r.status, 200L);
    PB_CHECK(r.body.find("\"undelete\":true") != std::string::npos);
    PB_CHECK(r.body.find("\"chunking\":\"1.0\"") != std::string::npos);
    PB_CHECK(r.body.find("\"max_size\":7340032") != std::string::npos);
    PB_CHECK(r.body.find("\"forbidden_filenames\":[\"x.txt\"]") != std::string::npos);
    e.server.set_trash(std::nullopt);
    e.server.set_chunking(false);
    r = e.send(e.req("GET", "/ocs/v1.php/cloud/capabilities?format=json"));
    PB_CHECK(r.body.find("undelete") == std::string::npos);
    PB_CHECK(r.body.find("\"chunking\"") == std::string::npos);
}

PB_TEST(a_chunked_upload_collects_the_chunks_in_the_upload_area_and_assembles_them_in_name_order) {
    Env e;
    e.server.add_folder("/Docs");
    const std::string id = "pbc-0123456789abcdef";
    const std::string dest = e.url("/Docs/big.bin");
    pbc::Headers h = {{"Destination", dest}, {"OC-Total-Length", "11"}};
    PB_CHECK_EQ(e.send(e.req("MKCOL", e.uploads + "/" + id, h)).status, 201L);
    PB_CHECK_EQ(e.send(e.req("MKCOL", e.uploads + "/" + id, h)).status, 405L);
    PB_CHECK_EQ(e.server.upload_sessions(), size_t(1));
    PB_CHECK_EQ(e.send(e.req("PUT", e.uploads + "/" + id + "/00002", h, "world")).status, 201L);
    PB_CHECK_EQ(e.send(e.req("PUT", e.uploads + "/" + id + "/00001", h, "hello ")).status, 201L);
    PB_CHECK_EQ(e.send(e.req("PUT", e.uploads + "/" + id + "/3", h, "x")).status, 400L);       // an unpadded name would sort wrongly
    PB_CHECK_EQ(e.send(e.req("PUT", e.uploads + "/" + id + "/00003", {{"Destination", e.url("/Docs/other")}}, "x")).status, 400L);
    PB_CHECK(!e.server.has("/Docs/big.bin"));                                                 // the chunks are never visible in the user's tree
    auto list = e.send(e.req("PROPFIND", e.uploads + "/" + id, {{"Depth", "1"}}));
    PB_CHECK_EQ(list.status, 207L);
    PB_CHECK(list.body.find("00001") != std::string::npos && list.body.find("<d:getcontentlength>5<") != std::string::npos);
    PB_CHECK_EQ(e.send(e.req("PROPFIND", e.uploads + "/" + id)).status, 403L);                // a Depth header is required
    auto r = e.send(e.req("MOVE", e.uploads + "/" + id + "/.file", {{"Destination", dest}, {"OC-Total-Length", "11"}, {"Overwrite", "F"}, {"X-OC-Mtime", "1790000500"}}));
    PB_CHECK_EQ(r.status, 201L);
    PB_CHECK_EQ(e.content("/Docs/big.bin"), std::string("hello world"));
    PB_CHECK_EQ(e.server.node("/Docs/big.bin").mtime, 1790000500LL);
    PB_CHECK_EQ(e.server.upload_sessions(), size_t(0));
}

PB_TEST(the_final_move_checks_the_conditions_the_length_and_the_quota) {
    Env e;
    e.server.add_file("/f.bin", "old");
    const std::string id = "pbc-aa";
    const std::string dest = e.url("/f.bin");
    auto open = [&](const std::string& session, long long total) {
        pbc::Headers h = {{"Destination", dest}, {"OC-Total-Length", std::to_string(total)}};
        PB_CHECK_EQ(e.send(e.req("MKCOL", e.uploads + "/" + session, h)).status, 201L);
        PB_CHECK_EQ(e.send(e.req("PUT", e.uploads + "/" + session + "/00001", h, std::string(static_cast<size_t>(total), 'n'))).status, 201L);
    };
    open(id, 6);
    // a new file onto an existing name: Overwrite F is refused
    PB_CHECK_EQ(e.send(e.req("MOVE", e.uploads + "/" + id + "/.file", {{"Destination", dest}, {"OC-Total-Length", "6"}, {"Overwrite", "F"}})).status, 412L);
    // a replace with a stale If-Match is refused, with the right one it goes through
    PB_CHECK_EQ(e.send(e.req("MOVE", e.uploads + "/" + id + "/.file", {{"Destination", dest}, {"OC-Total-Length", "6"}, {"Overwrite", "T"}, {"If-Match", "\"stale\""}})).status, 412L);
    PB_CHECK_EQ(e.content("/f.bin"), std::string("old"));
    // the declared length must equal what arrived
    PB_CHECK_EQ(e.send(e.req("MOVE", e.uploads + "/" + id + "/.file", {{"Destination", dest}, {"OC-Total-Length", "7"}, {"Overwrite", "T"}, {"If-Match", e.server.etag_of("/f.bin")}})).status, 400L);
    PB_CHECK_EQ(e.send(e.req("MOVE", e.uploads + "/" + id + "/.file", {{"Destination", dest}, {"OC-Total-Length", "6"}, {"Overwrite", "T"}, {"If-Match", e.server.etag_of("/f.bin")}})).status, 204L);
    PB_CHECK_EQ(e.content("/f.bin"), std::string("nnnnnn"));
    // an early 507 at the first chunk when the declared total does not fit
    e.server.set_quota_limit(20);
    PB_CHECK_EQ(e.send(e.req("MKCOL", e.uploads + "/pbc-bb", {{"Destination", e.url("/g.bin")}, {"OC-Total-Length", "100"}})).status, 201L);
    PB_CHECK_EQ(e.send(e.req("PUT", e.uploads + "/pbc-bb/00001", {{"Destination", e.url("/g.bin")}, {"OC-Total-Length", "100"}}, "x")).status, 507L);
}

PB_TEST(a_server_that_ignores_the_conditions_of_the_final_move_overwrites_and_can_be_caught_by_a_pre_check) {
    Env e;
    e.server.add_file("/f.bin", "old");
    pbc::Headers h = {{"Destination", e.url("/f.bin")}, {"OC-Total-Length", "3"}};
    PB_CHECK_EQ(e.send(e.req("MKCOL", e.uploads + "/pbc-cc", h)).status, 201L);
    PB_CHECK_EQ(e.send(e.req("PUT", e.uploads + "/pbc-cc/00001", h, "new")).status, 201L);
    e.server.ignore_conditions_on_assemble(true);
    PB_CHECK_EQ(e.send(e.req("MOVE", e.uploads + "/pbc-cc/.file", {{"Destination", e.url("/f.bin")}, {"OC-Total-Length", "3"}, {"Overwrite", "F"}})).status, 204L);
    PB_CHECK_EQ(e.content("/f.bin"), std::string("new"));   // the harness lets the test prove that the app checks first
}

PB_TEST(abort_deletes_the_upload_folder_and_an_expired_one_is_gone) {
    Env e;
    pbc::Headers h = {{"Destination", e.url("/x.bin")}};
    PB_CHECK_EQ(e.send(e.req("MKCOL", e.uploads + "/pbc-dd", h)).status, 201L);
    PB_CHECK_EQ(e.send(e.req("DELETE", e.uploads + "/pbc-dd")).status, 204L);
    PB_CHECK_EQ(e.send(e.req("DELETE", e.uploads + "/pbc-dd")).status, 404L);
    PB_CHECK_EQ(e.send(e.req("MKCOL", e.uploads + "/pbc-ee", h)).status, 201L);
    e.server.expire_uploads();
    PB_CHECK_EQ(e.send(e.req("PROPFIND", e.uploads + "/pbc-ee", {{"Depth", "1"}})).status, 404L);
    PB_CHECK_EQ(e.send(e.req("PUT", e.uploads + "/pbc-ee/00001", h, "x")).status, 404L);
}

PB_TEST(the_request_log_records_the_conditions_of_every_write) {
    Env e;
    e.server.add_file("/a.txt", "a");
    e.put("/b.txt", "b", {{"If-None-Match", "*"}});
    e.send(e.req("MOVE", e.files + "/a.txt", {{"Destination", e.url("/c.txt")}, {"Overwrite", "F"}, {"If-Match", e.server.etag_of("/a.txt")}}));
    const auto& log = e.server.log();
    PB_CHECK_EQ(log.size(), size_t(2));
    PB_CHECK_EQ(log[0].if_none_match, std::string("*"));
    PB_CHECK_EQ(log[1].overwrite, std::string("F"));
    PB_CHECK(!log[1].if_match.empty());
    PB_CHECK(log[1].destination.find("/c.txt") != std::string::npos);
}

PB_TEST(a_lost_answer_is_processed_by_the_server_and_a_forced_status_is_not) {
    Env e;
    e.server.lose_next_answer("PUT", "/lost.txt");
    auto r = e.put("/lost.txt", "x", {{"If-None-Match", "*"}});
    PB_CHECK_EQ(r.status, 0L);
    PB_CHECK(r.delivery == pbc::Delivery::FullySent);
    PB_CHECK_EQ(r.error_key, std::string("timeout"));
    PB_CHECK(e.server.has("/lost.txt"));   // the server did it
    e.server.answer_next("PUT", "/busy.txt", 503, 1, "30");
    r = e.put("/busy.txt", "x", {{"If-None-Match", "*"}});
    PB_CHECK_EQ(r.status, 503L);
    PB_CHECK_EQ(header(r, "Retry-After"), std::string("30"));
    PB_CHECK(!e.server.has("/busy.txt"));
    e.server.before_next("PUT", "/hook.txt", [&] { e.server.add_file("/hook.txt", "appeared first"); });
    r = e.put("/hook.txt", "x", {{"If-None-Match", "*"}});
    PB_CHECK_EQ(r.status, 412L);   // the name appeared between the listing and the send
}

PB_TEST(opaque_file_ids_and_missing_etag_headers_can_be_played) {
    Env e;
    e.server.set_opaque_file_ids(true);
    auto r = e.put("/a.txt", "a", {{"If-None-Match", "*"}});
    PB_CHECK(header(r, "OC-FileId").find("ocabc") != std::string::npos);
    e.server.set_answer_etag_headers(false);
    r = e.put("/b.txt", "b", {{"If-None-Match", "*"}});
    PB_CHECK_EQ(r.status, 201L);
    PB_CHECK(header(r, "OC-Etag").empty() && header(r, "ETag").empty());
}

PB_TEST(a_chunk_cut_in_the_middle_can_stay_behind_as_a_partial_chunk) {
    Env e;
    e.dir.write("src.bin", std::string(3 << 20, 'q'));
    pbc::Headers h = {{"Destination", e.url("/p.bin")}, {"OC-Total-Length", std::to_string(3 << 20)}};
    PB_CHECK_EQ(e.send(e.req("MKCOL", e.uploads + "/pbc-ff", h)).status, 201L);
    e.server.set_partial_chunks_visible(true);
    e.server.drop_after_bytes(1 << 20);
    pbc::Request r = e.req("PUT", e.uploads + "/pbc-ff/00001", h);
    r.body_file = e.dir.file("src.bin");
    r.body_length = 3 << 20;
    PB_CHECK_EQ(e.send(r).status, 0L);
    PB_CHECK_EQ(e.server.chunk_size_of("pbc-ff", "00001"), 1LL << 20);   // visible with the wrong size
    e.server.plant_partial_chunk("pbc-ff", "00002", 123);
    PB_CHECK((e.server.chunk_names("pbc-ff") == std::vector<std::string>{"00001", "00002"}));
}

PB_TEST_MAIN()
