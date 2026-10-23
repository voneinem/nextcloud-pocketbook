// The additions the fake Nextcloud got for folder synchronization (specs/003-folder-sync, tasks T012/T013): checksums in listings, big
// generated trees, listings that fail in different ways, removals, content changes, unavailable entries, hostile names and a conditional
// GET. Test infrastructure: it proves the app against this harness, never against a real server (constitution Principle IX).
#include <string>

#include "fake_nextcloud_server.h"
#include "pbc/nc_listing.h"
#include "test_support.h"

PBC_FAKE_SERVER_TEST();

using pbtest::FakeNextcloud;

namespace {

pbc::Request propfind(const FakeNextcloud& s, const std::string& path, const std::string& depth = "1") {
    pbc::Request r;
    r.method = "PROPFIND";
    r.url = s.base_url() + "/remote.php/dav/files/" + s.user_id() + path;
    r.username = s.login_name();
    r.password = s.app_password();
    r.headers = {{"Depth", depth}, {"Content-Type", "application/xml"}};
    r.body = pbc::listing_request_body();
    return r;
}

pbc::Request get(const FakeNextcloud& s, const std::string& path, const std::string& if_match = std::string()) {
    pbc::Request r;
    r.method = "GET";
    r.url = s.base_url() + "/remote.php/dav/files/" + s.user_id() + path;
    r.username = s.login_name();
    r.password = s.app_password();
    if (!if_match.empty()) r.headers.push_back({"If-Match", if_match});
    return r;
}

pbc::Result<pbc::Listing> list(FakeNextcloud& s, const std::string& folder) {
    return pbc::parse_listing(s.perform(propfind(s, folder)), s.user_id(), folder);
}

const pbc::RemoteEntry* find(const pbc::Listing& l, const std::string& name) {
    for (const auto& e : l.entries)
        if (e.name == name) return &e;
    return nullptr;
}

}  // namespace

PB_TEST(checksums_are_listed_when_configured_and_omitted_otherwise) {
    FakeNextcloud s;
    s.add_file("/Books/a.epub", "alpha");
    s.add_file("/Books/b.epub", "beta");
    s.set_checksums("/Books/a.epub", "aa11", "bb22");
    auto l = list(s, "/Books");
    PB_CHECK(l.ok());
    PB_CHECK_EQ(find(l.value, "a.epub")->checksum, std::string("SHA256:aa11 SHA1:bb22"));
    PB_CHECK_EQ(find(l.value, "b.epub")->checksum, std::string(""));
    s.set_checksums("/Books/b.epub", "cc33");   // SHA-256 only
    l = list(s, "/Books");
    PB_CHECK_EQ(find(l.value, "b.epub")->checksum, std::string("SHA256:cc33"));
    // a server that computes the SHA-256 of every file by itself
    FakeNextcloud t;
    t.add_file("/Books/a.epub", "alpha");
    t.list_checksums(true);
    auto l2 = list(t, "/Books");
    PB_CHECK(find(l2.value, "a.epub")->checksum.compare(0, 7, "SHA256:") == 0);
    PB_CHECK_EQ(find(l2.value, "a.epub")->checksum.size(), size_t(7 + 64));
}

PB_TEST(a_generated_tree_of_five_thousand_entries_is_listed_folder_by_folder) {
    FakeNextcloud s;
    s.add_tree("/Big", 50, 99, 3);   // 50 folders with 99 files each, plus the 50 folders = 5,000 entries below /Big
    size_t total = 0;
    auto root = list(s, "/Big");
    PB_CHECK(root.ok());
    PB_CHECK_EQ(root.value.entries.size(), size_t(50));   // the root of the tree holds only the folders
    for (const auto& e : root.value.entries) {
        PB_CHECK(e.kind == pbc::Kind::Folder);
        auto sub = list(s, e.path);
        PB_CHECK(sub.ok());
        PB_CHECK_EQ(sub.value.entries.size(), size_t(99));
        total += sub.value.entries.size() + 1;
    }
    PB_CHECK_EQ(total, size_t(5000));
    PB_CHECK(s.has("/Big/d000/f000.bin"));
    PB_CHECK_EQ(s.node("/Big/d000/f000.bin").size(), int64_t(3));
}

PB_TEST(a_listing_can_fail_partly_be_cut_short_or_time_out) {
    FakeNextcloud s;
    for (int i = 0; i < 6; ++i) s.add_file("/Books/f" + std::to_string(i) + ".epub", "x");
    s.set_listing_failure("/Books", "partial");
    auto p = list(s, "/Books");
    PB_CHECK(p.ok());
    PB_CHECK(p.value.partial);   // some members carry a non-200 status
    PB_CHECK(p.value.entries.size() == 6);
    size_t unreadable = 0;
    for (const auto& e : p.value.entries)
        if (e.availability != pbc::Availability::Ok || e.etag.empty()) ++unreadable;
    PB_CHECK(unreadable >= 1 && unreadable < 6);

    s.set_listing_failure("/Books", "truncated");
    auto t = list(s, "/Books");
    PB_CHECK(!t.ok());
    PB_CHECK_EQ(t.error.key, std::string("malformed_xml"));

    s.set_listing_failure("/Books", "timeout");
    const auto timed = s.perform(propfind(s, "/Books"));
    PB_CHECK(timed.error != pbc::ErrorClass::None);
    PB_CHECK_EQ(timed.error_key, std::string("timeout"));

    s.set_listing_failure("/Books", "error");
    PB_CHECK_EQ(s.perform(propfind(s, "/Books")).status, 500L);

    s.set_listing_failure("/Books", "");   // cleared
    auto ok = list(s, "/Books");
    PB_CHECK(ok.ok() && !ok.value.partial);
    // other folders were never affected
    s.add_file("/Other/x.epub", "x");
    s.set_listing_failure("/Books", "timeout");
    PB_CHECK(list(s, "/Other").ok());
}

PB_TEST(remove_and_touch_change_the_tree_the_way_a_user_would) {
    FakeNextcloud s;
    s.add_file("/Books/a.epub", "alpha");
    s.add_file("/Books/Sub/b.epub", "beta");
    const std::string before = s.etag_of("/Books/a.epub");
    s.touch_file("/Books/a.epub", "alpha changed", 1790000999);
    PB_CHECK(s.etag_of("/Books/a.epub") != before);
    PB_CHECK_EQ(s.node("/Books/a.epub").size(), int64_t(13));
    auto l = list(s, "/Books");
    PB_CHECK_EQ(find(l.value, "a.epub")->size.value_or(-1), int64_t(13));
    PB_CHECK_EQ(find(l.value, "a.epub")->mtime.value_or(-1), int64_t(1790000999));
    PB_CHECK_EQ(s.perform(get(s, "/Books/a.epub")).body, std::string("alpha changed"));
    s.remove("/Books/Sub");
    PB_CHECK(!s.has("/Books/Sub/b.epub"));
    PB_CHECK_EQ(s.perform(get(s, "/Books/Sub/b.epub")).status, 404L);
    PB_CHECK_EQ(s.perform(propfind(s, "/Books/Sub")).status, 404L);
    s.remove("/Books/a.epub");
    PB_CHECK_EQ(s.perform(get(s, "/Books/a.epub")).status, 404L);
    PB_CHECK(list(s, "/Books").value.entries.empty());
}

PB_TEST(unavailable_entries_are_listed_as_such) {
    FakeNextcloud s;
    s.add_file("/Books/ok.epub", "x");
    s.add_file("/Books/secret.epub", "x");
    s.add_file("/Books/locked.epub", "x");
    s.add_file("/Books/odd.epub", "x");
    s.set_unavailable("/Books/secret.epub", "no_permission");
    s.set_unavailable("/Books/locked.epub", "encrypted");
    s.set_unavailable("/Books/odd.epub", "unsupported");
    auto l = list(s, "/Books");
    PB_CHECK(find(l.value, "ok.epub")->availability == pbc::Availability::Ok);
    PB_CHECK(find(l.value, "secret.epub")->availability == pbc::Availability::NoPermission);
    PB_CHECK(find(l.value, "locked.epub")->availability == pbc::Availability::Encrypted);
    PB_CHECK(find(l.value, "odd.epub")->availability == pbc::Availability::Unsupported);
}

PB_TEST(hostile_names_are_served_inside_a_listing) {
    FakeNextcloud s;
    s.add_file("/Books/fine.epub", "x");
    // names the tree itself can hold
    s.add_file("/Books/control\x01name.epub", "x");
    s.add_file("/Books/" + std::string(300, 'n') + ".epub", "x");
    s.add_file("/Books/Case.epub", "x");
    s.add_file("/Books/case.epub", "y");
    s.add_file("/Books/caf\xC3\xA9.epub", "x");        // NFC
    s.add_file("/Books/cafe\xCC\x81.epub", "y");       // NFD
    s.add_file("/Books/slash\xE2\x88\x95name.epub", "x");   // U+2215 DIVISION SLASH, not a separator
    // members only a raw answer can carry: a dot-dot segment, an encoded separator, a foreign address
    s.add_raw_member("/Books", "..");
    s.add_raw_member("/Books", "a%2Fb.epub");
    s.add_raw_member("/Books", "%2E%2E");
    const auto r = s.perform(propfind(s, "/Books"));
    PB_CHECK_EQ(r.status, 207L);
    PB_CHECK(r.body.find("a%2Fb.epub") != std::string::npos);
    PB_CHECK(r.body.find("%2E%2E") != std::string::npos);
    auto l = pbc::parse_listing(r, s.user_id(), "/Books");
    PB_CHECK(l.ok());
    PB_CHECK(l.value.skipped_hostile >= 3);   // the raw members never become entries
    PB_CHECK(find(l.value, "fine.epub") != nullptr);
    PB_CHECK(find(l.value, "Case.epub") != nullptr && find(l.value, "case.epub") != nullptr);
}

PB_TEST(a_get_with_a_stale_if_match_answers_412) {
    FakeNextcloud s;
    s.add_file("/Books/a.epub", "alpha");
    const std::string etag = s.etag_of("/Books/a.epub");
    PB_CHECK_EQ(s.perform(get(s, "/Books/a.epub", etag)).status, 200L);
    PB_CHECK_EQ(s.perform(get(s, "/Books/a.epub", "\"0000000000000000\"")).status, 412L);
    s.touch_file("/Books/a.epub", "beta", 1790000001);
    PB_CHECK_EQ(s.perform(get(s, "/Books/a.epub", etag)).status, 412L);   // the version planned against is gone
    PB_CHECK_EQ(s.perform(get(s, "/Books/a.epub")).status, 200L);        // no condition: as before
    PB_CHECK_EQ(s.perform(get(s, "/Books/a.epub", "*")).status, 200L);
}

PB_TEST(the_request_log_counts_requests_per_method) {
    FakeNextcloud s;
    s.add_file("/Books/a.epub", "alpha");
    s.perform(propfind(s, "/Books"));
    s.perform(propfind(s, "/Books"));
    s.perform(get(s, "/Books/a.epub"));
    PB_CHECK_EQ(s.count("PROPFIND"), size_t(2));
    PB_CHECK_EQ(s.count("GET"), size_t(1));
    PB_CHECK_EQ(s.count("PUT"), size_t(0));
    PB_CHECK_EQ(s.count_matching(std::vector<std::string>{"PUT", "MKCOL", "MOVE", "COPY", "DELETE"}), size_t(0));   // "no write request at all"
    PB_CHECK_EQ(s.count_matching(std::vector<std::string>{"GET", "PROPFIND"}), size_t(3));
    s.clear_log();
    PB_CHECK_EQ(s.request_count(), size_t(0));
}

PB_TEST_MAIN()
