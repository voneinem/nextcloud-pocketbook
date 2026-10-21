// The exception class of a sabre error body is read from untrusted input (specs/002-manage-server-files, contracts/server-requests.md W10,
// constitution Principles I and IV): only a fixed list of class names is accepted, nothing of the body ever reaches a message, a log or an
// Error::detail, and no input makes the reader fail in any way but by returning "".
#include <string>

#include "fake_transport.h"
#include "fixtures.h"
#include "pbc/dav_write.h"
#include "test_support.h"

using pbtest::FakeTransport;

namespace {
std::string sabre(const std::string& cls) {
    return "<?xml version=\"1.0\"?><d:error xmlns:d=\"DAV:\" xmlns:s=\"http://sabredav.org/ns\"><s:exception>" + cls + "</s:exception></d:error>";
}
}  // namespace

PB_TEST(the_known_classes_are_read_by_their_last_name_segment) {
    PB_CHECK_EQ(pbc::sabre_exception_class(pbtest::fixture("nextcloud/write/errors/sabre_invalid_path.unverified.xml")), std::string("InvalidPath"));
    PB_CHECK_EQ(pbc::sabre_exception_class(pbtest::fixture("nextcloud/write/errors/sabre_forbidden_name.unverified.xml")), std::string("Forbidden"));
    PB_CHECK_EQ(pbc::sabre_exception_class(pbtest::fixture("nextcloud/write/errors/sabre_file_locked.unverified.xml")), std::string("FileLocked"));
    PB_CHECK_EQ(pbc::sabre_exception_class(pbtest::fixture("nextcloud/write/errors/sabre_insufficient_storage.unverified.xml")), std::string("InsufficientStorage"));
    PB_CHECK_EQ(pbc::sabre_exception_class(sabre("OCP\\Files\\ForbiddenException")), std::string("Forbidden"));   // a trailing "Exception" is ignored
    PB_CHECK_EQ(pbc::sabre_exception_class(sabre("Sabre\\DAV\\Exception\\Locked")), std::string("FileLocked"));
    PB_CHECK_EQ(pbc::sabre_exception_class(sabre("  Sabre\\DAV\\Exception\\NotFound \n")), std::string("NotFound"));
}

PB_TEST(an_unknown_hostile_or_broken_class_gives_nothing) {
    PB_CHECK_EQ(pbc::sabre_exception_class(pbtest::fixture("nextcloud/write/errors/sabre_unknown_class.unverified.xml")), std::string());
    PB_CHECK_EQ(pbc::sabre_exception_class(pbtest::fixture("nextcloud/write/errors/sabre_error_hostile.unverified.xml")), std::string());   // malformed
    PB_CHECK_EQ(pbc::sabre_exception_class(pbtest::fixture("nextcloud/write/errors/sabre_entity_bomb.unverified.xml")), std::string());      // DOCTYPE refused
    for (const std::string& cls : {std::string("Forbidden<script>"), std::string("../../Forbidden"), std::string("Sabre\\DAV\\Exception\\Forbid\x01"),
                                   std::string("Sabre\\DAV\\Exception\\forbidden"), std::string(""), std::string("\\"), std::string("Exception"),
                                   std::string(10000, 'A'), std::string("Sabre\\DAV\\Exception\\") + std::string(10000, 'B')})
        PB_CHECK_EQ(pbc::sabre_exception_class(sabre(cls)), std::string());
}

PB_TEST(not_xml_truncated_or_oversized_bodies_are_ignored) {
    PB_CHECK_EQ(pbc::sabre_exception_class(""), std::string());
    PB_CHECK_EQ(pbc::sabre_exception_class("plain text, not xml"), std::string());
    PB_CHECK_EQ(pbc::sabre_exception_class("{\"error\":\"Forbidden\"}"), std::string());
    const std::string good = sabre("Sabre\\DAV\\Exception\\Forbidden");
    PB_CHECK_EQ(pbc::sabre_exception_class(good.substr(0, good.size() / 2)), std::string());   // truncated
    // a body above 64 KiB is not read at all, even when the class is valid and comes first
    std::string big = sabre("Sabre\\DAV\\Exception\\Forbidden");
    big.insert(big.find("</d:error>"), std::string(70000, ' '));
    PB_CHECK_EQ(pbc::sabre_exception_class(big), std::string());
    // deep nesting and many elements are refused by the reader's limits, not followed
    std::string deep = "<a>";
    for (int i = 0; i < 5000; ++i) deep += "<b>";
    PB_CHECK_EQ(pbc::sabre_exception_class(deep), std::string());
    // the exception element in a foreign namespace is not the sabre one
    PB_CHECK_EQ(pbc::sabre_exception_class("<d:error xmlns:d=\"DAV:\" xmlns:s=\"http://evil.example.org/ns\"><s:exception>Sabre\\DAV\\Exception\\Forbidden</s:exception></d:error>"),
                std::string());
}

PB_TEST(nothing_of_the_body_reaches_the_error_detail) {
    const std::string body = pbtest::fixture("nextcloud/write/errors/sabre_error_hostile.unverified.xml");
    for (long s : {400L, 403L, 409L, 412L, 413L, 415L, 423L, 500L, 507L}) {
        for (pbc::WriteKind k : {pbc::WriteKind::Put, pbc::WriteKind::Move, pbc::WriteKind::Delete}) {
            const auto a = pbc::interpret_write_answer(FakeTransport::status(s, body + "SECRET-PATH-private.txt"), k, false);
            PB_CHECK(a.error.detail.find("SECRET") == std::string::npos);
            PB_CHECK(a.error.detail.find("script") == std::string::npos);
            PB_CHECK(a.error.detail.find("passwd") == std::string::npos);
            PB_CHECK(a.error.detail.size() < 100);
            PB_CHECK(a.exception_class.empty());
        }
    }
}

PB_TEST_MAIN()
