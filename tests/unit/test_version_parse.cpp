// The tolerant parser of the version list of a file (specs/006-trash-and-versions, contracts/core-interfaces.md "trash_parse.h", research R-3):
// the collection itself is not a row, an id that is not 1 to 20 digits is skipped and counted, a missing date is kept as unknown, a future
// date is flagged, the order is newest first.
#include <string>
#include <vector>

#include "fixtures.h"
#include "pbc/clock_check.h"
#include "pbc/http_transport.h"
#include "pbc/trash_parse.h"
#include "test_support.h"

namespace {

const int64_t kNow = 1760100000;

std::string fx(const std::string& name) { return pbtest::fixture("nextcloud/trash/" + name); }

pbc::Response multistatus(const std::string& body) {
    pbc::Response r;
    r.status = 207;
    r.body = body;
    return r;
}

pbc::VersionList list_of(const std::string& body) {
    const auto r = pbc::parse_versions(multistatus(body), kNow);
    if (!r.ok()) PB_FAIL_AT("the list was refused: " + r.error.key + " " + r.error.detail);
    return r.value;
}

std::string many_versions(int n) {
    std::string b = "<?xml version=\"1.0\"?>\n<d:multistatus xmlns:d=\"DAV:\">"
                    "<d:response><d:href>/remote.php/dav/versions/demo/versions/4713/</d:href><d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype>"
                    "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
    for (int i = 0; i < n; ++i)
        b += "<d:response><d:href>/remote.php/dav/versions/demo/versions/4713/" + std::to_string(1600000000 + i) + "</d:href><d:propstat><d:prop>"
             "<d:getlastmodified>Thu, 09 Oct 2025 08:53:20 GMT</d:getlastmodified><d:getcontentlength>10</d:getcontentlength></d:prop>"
             "<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
    return b + "</d:multistatus>";
}

}  // namespace

PB_TEST(a_normal_list_gives_the_older_versions_newest_first) {
    const auto l = list_of(fx("versions_list_ok.unverified.xml"));
    PB_CHECK_EQ(l.rows.size(), size_t(3));   // the collection itself is not a row
    PB_CHECK_EQ(l.skipped, 0);
    PB_CHECK(!l.truncated);
    PB_CHECK_EQ(l.rows[0].version_id, std::string("1760000000"));
    PB_CHECK_EQ(l.rows[1].version_id, std::string("1750000000"));
    PB_CHECK_EQ(l.rows[2].version_id, std::string("1740000000"));
    PB_CHECK(l.rows[0].mtime == std::optional<int64_t>(1760000000));
    PB_CHECK(l.rows[1].mtime == std::optional<int64_t>(1750000000));
    PB_CHECK(l.rows[0].size == std::optional<int64_t>(1200));
    PB_CHECK(l.rows[2].size == std::optional<int64_t>(900));
    for (const auto& r : l.rows) PB_CHECK(!r.future);
    PB_CHECK(l.rows[0].mtime == pbc::parse_http_date("Thu, 09 Oct 2025 08:53:20 GMT"));
}

PB_TEST(a_collection_without_rows_gives_an_empty_list) {
    const std::string only_collection =
        "<d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>/remote.php/dav/versions/demo/versions/4713/</d:href><d:propstat><d:prop>"
        "<d:resourcetype><d:collection/></d:resourcetype></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>";
    const auto l = list_of(only_collection);
    PB_CHECK(l.rows.empty());
    PB_CHECK_EQ(l.skipped, 0);
}

PB_TEST(the_request_body_asks_for_date_size_and_type) {
    const std::string b = pbc::versions_request_body();
    for (const char* p : {"propfind", "getlastmodified", "getcontentlength", "getcontenttype"}) PB_CHECK(b.find(p) != std::string::npos);
}

PB_TEST(an_id_that_is_not_one_to_twenty_digits_is_skipped_and_counted) {
    const auto l = list_of(fx("versions_list_hostile.unverified.xml"));
    PB_CHECK_EQ(l.skipped, 3);   // "abc", 21 digits, an encoded "/"
    PB_CHECK_EQ(l.rows.size(), size_t(4));
    for (const auto& r : l.rows) {
        PB_CHECK(!r.version_id.empty() && r.version_id.size() <= 20);
        for (char c : r.version_id) PB_CHECK(c >= '0' && c <= '9');
    }
}

PB_TEST(a_missing_or_unreadable_date_is_kept_as_unknown_and_a_future_date_is_flagged) {
    const auto l = list_of(fx("versions_list_hostile.unverified.xml"));
    const auto row = [&](const char* id) -> const pbc::VersionRow& {
        for (const auto& r : l.rows)
            if (r.version_id == id) return r;
        PB_FAIL_AT(std::string("no row ") + id);
    };
    PB_CHECK(!row("1730000000").mtime.has_value());   // no date at all
    PB_CHECK(!row("1700000000").mtime.has_value());   // "garbage date"
    PB_CHECK(row("1720000000").mtime.has_value() && row("1720000000").future);
    PB_CHECK(row("1710000000").mtime.has_value() && !row("1710000000").future);
    PB_CHECK(!row("1710000000").size.has_value());    // "-4" is not a size
    PB_CHECK(row("1730000000").size == std::optional<int64_t>(500));
    // newest first by date; rows without a date last, by id descending
    PB_CHECK_EQ(l.rows[0].version_id, std::string("1720000000"));
    PB_CHECK_EQ(l.rows[1].version_id, std::string("1710000000"));
    PB_CHECK_EQ(l.rows[2].version_id, std::string("1730000000"));
    PB_CHECK_EQ(l.rows[3].version_id, std::string("1700000000"));
}

PB_TEST(more_than_the_limit_keeps_the_newest_and_says_so) {
    const auto l = list_of(many_versions(250));
    PB_CHECK_EQ(l.rows.size(), pbc::kMaxVersionRows);
    PB_CHECK(l.truncated);
    const auto exact = list_of(many_versions(200));
    PB_CHECK_EQ(exact.rows.size(), size_t(200));
    PB_CHECK(!exact.truncated);
    // equal dates: the order is by id descending
    PB_CHECK_EQ(exact.rows[0].version_id, std::string("1600000199"));
}

PB_TEST(an_answer_above_the_limit_is_refused) {
    pbc::Response r;
    r.status = 207;
    r.body_too_large = true;
    PB_CHECK_EQ(pbc::parse_versions(r, kNow).error.key, std::string("listing_too_large"));
    PB_CHECK_EQ(pbc::parse_versions(multistatus(std::string(pbc::kMaxVersionBody + 1, ' ')), kNow).error.key, std::string("listing_too_large"));
}

PB_TEST(a_broken_answer_gives_a_key_and_never_a_crash) {
    PB_CHECK_EQ(pbc::parse_versions(multistatus("<d:multistatus xmlns:d=\"DAV:\"><d:response>"), kNow).error.key, std::string("malformed_xml"));
    PB_CHECK_EQ(pbc::parse_versions(multistatus(""), kNow).error.key, std::string("malformed_xml"));
    pbc::Response not207;
    not207.status = 404;
    not207.body = fx("versions_list_ok.unverified.xml");
    PB_CHECK_EQ(pbc::parse_versions(not207, kNow).error.key, std::string("protocol_error"));
    for (const char* name : {"versions_list_ok.unverified.xml", "versions_list_hostile.unverified.xml"}) {
        const std::string good = fx(name);
        for (size_t cut = 1; cut < good.size(); cut += 29) {
            const auto r = pbc::parse_versions(multistatus(good.substr(0, cut)), kNow);
            if (r.ok()) PB_CHECK(r.value.rows.size() <= pbc::kMaxVersionRows);
        }
        for (size_t at = 0; at < good.size(); at += 41) {
            std::string bad = good;
            bad[at] = static_cast<char>(bad[at] ^ 0x5a);
            (void)pbc::parse_versions(multistatus(bad), kNow);
        }
    }
}

PB_TEST_MAIN()
