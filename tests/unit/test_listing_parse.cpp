// The folder listing (contracts/server-requests.md section 5): what a depth-1 PROPFIND answer becomes. The fixtures were written from the
// API notes and carry the .unverified marker until a live run produced them (docs/live-server-checklist.md LV-N5).
#include <fstream>
#include <sstream>
#include <string>

#include "fake_transport.h"
#include "pbc/clock_check.h"
#include "pbc/nc_listing.h"
#include "test_support.h"

using pbc::Availability;
using pbc::Kind;
using pbtest::FakeTransport;

namespace {

std::string fixture(const std::string& name) {
    std::ifstream f(std::string(PBC_FIXTURE_DIR) + "/nextcloud/propfind/" + name, std::ios::binary);
    if (!f) PB_FAIL_AT("missing fixture " + name);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

pbc::Result<pbc::Listing> parse(const std::string& body, const std::string& path = "/Books", long status = 207) {
    return pbc::parse_listing(FakeTransport::status(status, body), "demo", path);
}

const pbc::RemoteEntry* find(const pbc::Listing& l, const std::string& name) {
    for (const auto& e : l.entries)
        if (e.name == name) return &e;
    return nullptr;
}

std::string listing_xml(int entries, const std::string& extra_props = "") {
    std::string x = "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\">"
                    "<d:response><d:href>/remote.php/dav/files/demo/Big/</d:href><d:propstat><d:prop><d:getetag>\"f\"</d:getetag><d:resourcetype><d:collection/></d:resourcetype></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
    for (int i = 0; i < entries; ++i)
        x += "<d:response><d:href>/remote.php/dav/files/demo/Big/file" + std::to_string(i) + ".txt</d:href><d:propstat><d:prop><d:getetag>\"e" +
             std::to_string(i) + "\"</d:getetag><d:resourcetype/><d:getcontentlength>10</d:getcontentlength>" + extra_props + "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
    return x + "</d:multistatus>";
}

}  // namespace

PB_TEST(a_normal_answer_gives_the_children_and_leaves_out_the_folder_itself) {
    const auto r = parse(fixture("listing_basic.unverified.xml"));
    PB_CHECK(r.ok());
    const pbc::Listing& l = r.value;
    PB_CHECK_EQ(l.entries.size(), size_t(3));   // not four: the first response is the folder itself, found by its path and not by its place
    PB_CHECK_EQ(l.folder_etag, std::string("\"folder-etag-1\""));
    PB_CHECK_EQ(*l.folder_fileid, int64_t(4711));
    PB_CHECK_EQ(l.skipped_foreign, 0);
    PB_CHECK_EQ(l.skipped_hostile, 0);
    PB_CHECK(!l.partial);

    const auto* novels = find(l, "Novels");
    PB_CHECK(novels != nullptr);
    PB_CHECK(novels->kind == Kind::Folder);
    PB_CHECK_EQ(*novels->size, int64_t(3000));
    PB_CHECK_EQ(*novels->mtime, *pbc::parse_http_date("Mon, 19 Oct 2026 10:30:00 GMT"));
    PB_CHECK_EQ(novels->path, std::string("/Books/Novels"));

    const auto* story = find(l, "My Story \xC3\xA4.epub");   // decoded from %20 and %C3%A4
    PB_CHECK(story != nullptr);
    PB_CHECK(story->kind == Kind::File);
    PB_CHECK_EQ(*story->size, int64_t(1234));
    PB_CHECK_EQ(story->etag, std::string("W/\"weak-etag-3\""));   // stored as sent, weak marker and quotes included
    PB_CHECK_EQ(story->mime, std::string("application/epub+zip"));
    PB_CHECK(story->favorite);
    PB_CHECK_EQ(*story->fileid, int64_t(4713));
    PB_CHECK_EQ(story->permissions, std::string("RGDNVW"));
    PB_CHECK_EQ(story->checksum, std::string("SHA1:0123456789abcdef0123456789abcdef01234567 MD5:0123456789abcdef0123456789abcdef"));
    PB_CHECK(story->availability == Availability::Ok);   // a 404 for one property (nc:is-encrypted) is not an error
}

PB_TEST(size_comes_from_oc_size_else_from_the_content_length_and_a_bad_date_is_unknown) {
    const auto r = parse(fixture("listing_basic.unverified.xml"));
    const auto* notes = find(r.value, "notes.txt");
    PB_CHECK(notes != nullptr);
    PB_CHECK_EQ(*notes->size, int64_t(12));
    PB_CHECK(!notes->mtime.has_value());
    PB_CHECK(!notes->favorite);
}

PB_TEST(entries_the_user_cannot_use_are_marked_with_a_reason_and_the_rest_is_intact) {
    const auto r = parse(fixture("listing_markers.unverified.xml"), "/");
    PB_CHECK(r.ok());
    const pbc::Listing& l = r.value;
    PB_CHECK_EQ(l.entries.size(), size_t(5));
    PB_CHECK_EQ(l.folder_etag, std::string("\"root-etag\""));

    const auto* vault = find(l, "Vault");
    PB_CHECK(vault && vault->availability == Availability::Encrypted);
    PB_CHECK_EQ(vault->reason, std::string("end_to_end_encrypted"));
    PB_CHECK(vault->kind == Kind::Folder);

    const auto* shared = find(l, "Shared without reading");
    PB_CHECK(shared && shared->availability == Availability::NoPermission);   // "SRD" has no G
    PB_CHECK_EQ(shared->reason, std::string("no_read_permission"));

    const auto* broken = find(l, "Broken mount");
    PB_CHECK(broken && broken->availability == Availability::Unsupported);    // neither an ETag nor a resource type
    PB_CHECK_EQ(broken->reason, std::string("unsupported_state"));
    PB_CHECK(broken->kind == Kind::Folder);                                   // the address ends with "/"

    const auto* locked = find(l, "Locked away");
    PB_CHECK(locked && locked->availability == Availability::Unreadable);     // no 2xx propstat at all
    PB_CHECK_EQ(locked->reason, std::string("status_403"));

    const auto* fine = find(l, "fine.txt");
    PB_CHECK(fine && fine->availability == Availability::Ok);
    PB_CHECK(l.partial);   // something in the answer failed
}

PB_TEST(addresses_of_another_account_or_another_folder_are_counted_and_hostile_names_are_skipped) {
    const auto r = parse(fixture("listing_foreign_and_hostile.unverified.xml"));
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.entries.size(), size_t(2));
    PB_CHECK(find(r.value, "ok.epub") && find(r.value, "second.epub"));
    PB_CHECK_EQ(r.value.skipped_foreign, 3);   // another user, another prefix, another folder
    PB_CHECK_EQ(r.value.skipped_hostile, 4);   // an encoded slash, "..", NUL, a broken escape
    PB_CHECK(r.value.partial);
}

PB_TEST(ten_thousand_entries_are_the_limit) {
    PB_CHECK(parse(listing_xml(10000), "/Big").ok());
    PB_CHECK_EQ(parse(listing_xml(10000), "/Big").value.entries.size(), size_t(10000));
    const auto r = parse(listing_xml(10001), "/Big");
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("listing_too_large"));
}

PB_TEST(long_values_are_cut_to_their_bounds) {
    const auto r = parse(listing_xml(1, "<d:getcontenttype>" + std::string(300, 'a') + "</d:getcontenttype><oc:checksums><oc:checksum>" + std::string(600, 'b') + "</oc:checksum></oc:checksums>"), "/Big");
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.entries[0].mime.size(), size_t(100));
    PB_CHECK_EQ(r.value.entries[0].checksum.size(), size_t(256));
}

PB_TEST(a_name_of_1024_bytes_is_accepted_and_one_more_is_not) {
    auto one = [](size_t n) {
        std::string x = "<d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>/remote.php/dav/files/demo/F/" + std::string(n, 'n') +
                        "</d:href><d:propstat><d:prop><d:getetag>\"e\"</d:getetag><d:resourcetype/></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>";
        return parse(x, "/F");
    };
    PB_CHECK_EQ(one(1024).value.entries.size(), size_t(1));
    PB_CHECK_EQ(one(1025).value.entries.size(), size_t(0));
    PB_CHECK_EQ(one(1025).value.skipped_hostile, 1);
}

PB_TEST(a_body_over_the_cap_a_wrong_root_and_a_wrong_status_are_errors) {
    pbc::Response big = FakeTransport::status(207, "");
    big.body_too_large = true;
    PB_CHECK_EQ(pbc::parse_listing(big, "demo", "/Books").error.key, std::string("listing_too_large"));
    PB_CHECK_EQ(parse("<html/>").error.key, std::string("malformed_xml"));
    PB_CHECK_EQ(parse(fixture("listing_basic.unverified.xml"), "/Books", 200).error.key, std::string("protocol_error"));
}

PB_TEST_MAIN()
