// The answer of a PROPFIND on an upload folder (specs/002-manage-server-files, research R-4, contracts/server-requests.md W4): which chunks the server
// holds, which are the wrong size, and what is ignored. Untrusted input: hostile names, a truncated answer, entries that are not chunks.
#include <string>

#include "fake_transport.h"
#include "fixtures.h"
#include "pbc/upload_session.h"
#include "test_support.h"

using pbtest::FakeTransport;

namespace {
constexpr int64_t kMiB = 1ll << 20;
pbc::ChunkPlan plan() { return *pbc::plan_chunks(35 * kMiB, 10 * kMiB); }   // four chunks: 10, 10, 10, 5 MiB
pbc::Response answer(const std::string& fixture, long status = 207) { return FakeTransport::status(status, pbtest::fixture("nextcloud/write/uploads/" + fixture)); }
}  // namespace

PB_TEST(a_chunk_is_good_only_when_its_name_is_in_range_and_its_size_is_the_expected_one) {
    const auto r = pbc::parse_upload_folder(answer("upload_folder_propfind.unverified.xml"), plan());
    PB_CHECK(r.ok());
    PB_CHECK(!r.value.gone);
    PB_CHECK((r.value.good == std::set<int>{1, 2, 4}));   // 3 has the wrong size
    PB_CHECK_EQ(r.value.bad, 1);                          // the chunk of the wrong size, counted once
}

PB_TEST(entries_that_are_not_chunks_hostile_names_and_other_users_folders_are_ignored) {
    const auto r = pbc::parse_upload_folder(answer("upload_folder_propfind.unverified.xml"), plan());
    PB_CHECK(r.ok());
    for (int n : r.value.good) PB_CHECK(n >= 1 && n <= 4);
    PB_CHECK(r.value.good.count(5) == 0);                 // out of range for this plan
    PB_CHECK_EQ(r.value.bad, 1);                          // ".file", "0004", "99999", "%2e%2e%2f00002", "0000a" are neither good nor bad
}

PB_TEST(a_chunk_that_appears_twice_with_different_sizes_is_not_trusted) {
    const auto r = pbc::parse_upload_folder(answer("upload_folder_duplicates.unverified.xml"), plan());
    PB_CHECK(r.ok());
    PB_CHECK((r.value.good == std::set<int>{2}));
    PB_CHECK_EQ(r.value.bad, 1);
}

PB_TEST(a_missing_folder_is_gone_and_other_statuses_are_errors) {
    auto r = pbc::parse_upload_folder(FakeTransport::status(404, ""), plan());
    PB_CHECK(r.ok());
    PB_CHECK(r.value.gone);
    PB_CHECK(r.value.good.empty());
    r = pbc::parse_upload_folder(FakeTransport::status(200, "<d:multistatus xmlns:d=\"DAV:\"/>"), plan());
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("protocol_error"));
    r = pbc::parse_upload_folder(FakeTransport::status(207, "this is not xml"), plan());
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("malformed_xml"));
    r = pbc::parse_upload_folder(FakeTransport::status(207, "<html/>"), plan());
    PB_CHECK(!r.ok());
}

PB_TEST(a_truncated_answer_is_an_error_not_a_guess) {
    const auto r = pbc::parse_upload_folder(answer("upload_folder_truncated.unverified.xml"), plan());
    PB_CHECK(!r.ok());   // the minimal reader refuses a document that is not well formed; the caller sends the chunks again
    PB_CHECK_EQ(r.error.key, std::string("malformed_xml"));
}

PB_TEST(an_empty_upload_folder_has_no_good_chunk) {
    const auto r = pbc::parse_upload_folder(FakeTransport::status(207, "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\"></d:multistatus>"), plan());
    PB_CHECK(r.ok());
    PB_CHECK(r.value.good.empty());
    PB_CHECK_EQ(r.value.bad, 0);
}

PB_TEST(thousands_of_entries_are_read_without_trouble_and_only_the_plans_chunks_count) {
    std::string xml = "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\">";
    for (int i = 0; i < 20000; ++i)
        xml += "<d:response><d:href>/remote.php/dav/uploads/demo/pbc-aa/x" + std::to_string(i) +
               "</d:href><d:propstat><d:prop><d:getcontentlength>1</d:getcontentlength></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
    xml += "</d:multistatus>";
    const auto r = pbc::parse_upload_folder(FakeTransport::status(207, xml), plan());
    PB_CHECK(r.ok());
    PB_CHECK(r.value.good.empty());
}

PB_TEST_MAIN()
