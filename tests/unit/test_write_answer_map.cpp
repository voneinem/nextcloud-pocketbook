// How the answer to a write request becomes a stable key (specs/002-manage-server-files, contracts/server-requests.md W10, research R-7 and
// R-12): by transport state and delivery first, then by status, then by the whitelisted exception class of a sabre error body. Pure: a
// scripted Response, no server involved.
#include <string>

#include "fake_transport.h"
#include "fixtures.h"
#include "pbc/dav_write.h"
#include "pbc/error_keys.h"
#include "test_support.h"

using pbc::Delivery;
using pbc::WriteKind;
using pbtest::FakeTransport;

namespace {

pbc::WriteAnswer answer(long status, WriteKind kind, bool replacing = false, const std::string& body = std::string()) {
    return pbc::interpret_write_answer(FakeTransport::status(status, body), kind, replacing);
}

pbc::Response no_answer(Delivery d, const std::string& error_key = "timeout") {
    pbc::Response r;
    r.status = 0;
    r.delivery = d;
    r.error = pbc::ErrorClass::Network;
    r.error_key = error_key;
    return r;
}

std::string sabre(const std::string& cls) {
    return "<?xml version=\"1.0\"?><d:error xmlns:d=\"DAV:\" xmlns:s=\"http://sabredav.org/ns\"><s:exception>" + cls +
           "</s:exception><s:message>x</s:message></d:error>";
}

}  // namespace

PB_TEST(success_statuses_are_ok_and_carry_the_new_etag_and_file_id) {
    pbc::Response r = FakeTransport::status(201, "", {{"OC-Etag", "\"abc\""}, {"OC-FileId", "00000042ocxyz"}, {"ETag", "\"zzz\""}});
    pbc::WriteAnswer a = pbc::interpret_write_answer(r, WriteKind::Put, false);
    PB_CHECK(a.error.ok());
    PB_CHECK_EQ(a.etag, std::string("\"abc\""));   // OC-Etag first
    PB_CHECK(!a.fileid.has_value());               // 00000042ocxyz is not a number: never parsed from the opaque id
    r = FakeTransport::status(204, "", {{"ETag", "\"zzz\""}, {"OC-FileId", "42"}});
    a = pbc::interpret_write_answer(r, WriteKind::Put, true);
    PB_CHECK(a.error.ok());
    PB_CHECK_EQ(a.etag, std::string("\"zzz\""));   // else ETag
    PB_CHECK_EQ(a.fileid.value_or(-1), 42LL);
    for (WriteKind k : {WriteKind::Mkcol, WriteKind::Move, WriteKind::Copy, WriteKind::Delete, WriteKind::Chunk})
        for (long s : {200L, 201L, 204L}) PB_CHECK(answer(s, k).error.ok());
}

PB_TEST(a_412_means_the_name_exists_for_a_new_file_and_a_changed_file_for_a_replace) {
    PB_CHECK_EQ(answer(412, WriteKind::Put, false).error.key, std::string("name_exists"));
    PB_CHECK_EQ(answer(412, WriteKind::Put, true).error.key, std::string("changed_on_server"));
    PB_CHECK_EQ(answer(412, WriteKind::Delete).error.key, std::string("changed_on_server"));
    // MOVE and COPY carry two conditions: the key is only a placeholder, the cause is decided by the caller
    for (WriteKind k : {WriteKind::Move, WriteKind::Copy}) {
        const auto a = answer(412, k);
        PB_CHECK(a.ambiguous_412);
        PB_CHECK_EQ(a.error.key, std::string("changed_on_server"));
    }
    PB_CHECK(!answer(412, WriteKind::Put).ambiguous_412);
}

PB_TEST(mkcol_405_means_the_name_exists_and_other_methods_do_not) {
    PB_CHECK_EQ(answer(405, WriteKind::Mkcol).error.key, std::string("name_exists"));
    for (WriteKind k : {WriteKind::Put, WriteKind::Move, WriteKind::Copy, WriteKind::Delete}) PB_CHECK_EQ(answer(405, k).error.key, std::string("server_refused"));
}

PB_TEST(parent_and_source_statuses_are_told_apart) {
    PB_CHECK_EQ(answer(409, WriteKind::Put).error.key, std::string("target_gone"));
    PB_CHECK_EQ(answer(409, WriteKind::Mkcol).error.key, std::string("target_gone"));
    PB_CHECK_EQ(answer(409, WriteKind::Move).error.key, std::string("target_gone"));
    PB_CHECK_EQ(answer(404, WriteKind::Put, true).error.key, std::string("source_gone"));
    PB_CHECK_EQ(answer(404, WriteKind::Put, false).error.key, std::string("target_gone"));
    PB_CHECK_EQ(answer(404, WriteKind::Mkcol).error.key, std::string("target_gone"));
    PB_CHECK_EQ(answer(404, WriteKind::Move).error.key, std::string("source_gone"));
    PB_CHECK_EQ(answer(404, WriteKind::Copy).error.key, std::string("source_gone"));
    PB_CHECK(answer(404, WriteKind::Delete).error.ok());   // already gone
}

PB_TEST(the_refusals_of_the_server_have_their_own_keys) {
    PB_CHECK_EQ(answer(507, WriteKind::Put).error.key, std::string("server_storage_full"));
    PB_CHECK_EQ(answer(413, WriteKind::Put).error.key, std::string("file_too_large_server"));
    PB_CHECK_EQ(answer(415, WriteKind::Put).error.key, std::string("type_not_accepted"));
    PB_CHECK_EQ(answer(423, WriteKind::Move).error.key, std::string("locked"));
    PB_CHECK_EQ(answer(403, WriteKind::Put).error.key, std::string("not_allowed"));
    PB_CHECK_EQ(answer(400, WriteKind::Put).error.key, std::string("protocol_error"));
    PB_CHECK_EQ(answer(401, WriteKind::Put).error.key, std::string("auth_failed"));
    PB_CHECK_EQ(answer(429, WriteKind::Put).error.key, std::string("rate_limited"));
    PB_CHECK_EQ(answer(503, WriteKind::Put).error.key, std::string("maintenance"));
    PB_CHECK_EQ(answer(207, WriteKind::Delete).error.key, std::string("partial_failure"));
    PB_CHECK_EQ(answer(207, WriteKind::Move).error.key, std::string("partial_failure"));
    PB_CHECK_EQ(answer(207, WriteKind::Copy).error.key, std::string("partial_failure"));
    PB_CHECK_EQ(answer(207, WriteKind::Put).error.key, std::string("protocol_error"));
}

PB_TEST(a_known_exception_class_refines_the_status) {
    PB_CHECK_EQ(answer(403, WriteKind::Put, false, sabre("OCA\\DAV\\Connector\\Sabre\\Exception\\InvalidPath")).error.key, std::string("name_forbidden"));
    PB_CHECK_EQ(answer(400, WriteKind::Put, false, sabre("OCA\\DAV\\Connector\\Sabre\\Exception\\InvalidPath")).error.key, std::string("name_forbidden"));
    PB_CHECK_EQ(answer(403, WriteKind::Mkcol, false, sabre("OCA\\DAV\\Connector\\Sabre\\Exception\\ForbiddenFileName")).error.key, std::string("name_forbidden"));
    PB_CHECK_EQ(answer(403, WriteKind::Put, false, sabre("Sabre\\DAV\\Exception\\Forbidden")).error.key, std::string("not_allowed"));
    PB_CHECK_EQ(answer(403, WriteKind::Put, false, sabre("OCA\\DAV\\Connector\\Sabre\\Exception\\FileLocked")).error.key, std::string("locked"));
    PB_CHECK_EQ(answer(500, WriteKind::Put, false, sabre("Sabre\\DAV\\Exception\\InsufficientStorage")).error.key, std::string("server_storage_full"));
    PB_CHECK_EQ(answer(400, WriteKind::Put, false, sabre("Sabre\\DAV\\Exception\\EntityTooLarge")).error.key, std::string("file_too_large_server"));
    PB_CHECK_EQ(answer(400, WriteKind::Put, false, sabre("Sabre\\DAV\\Exception\\UnsupportedMediaType")).error.key, std::string("type_not_accepted"));
    // an unknown class changes nothing
    PB_CHECK_EQ(answer(403, WriteKind::Put, false, sabre("Vendor\\Odd\\Failure")).error.key, std::string("not_allowed"));
    PB_CHECK_EQ(answer(418, WriteKind::Put, false, sabre("Vendor\\Odd\\Failure")).error.key, std::string("server_refused"));
    PB_CHECK_EQ(answer(418, WriteKind::Put).error.detail, std::string("status 418"));   // the numeric status is the whole detail
}

PB_TEST(a_failure_before_anything_was_sent_is_not_unknown) {
    const auto a = pbc::interpret_write_answer(no_answer(Delivery::NotSent, "server_unreachable"), WriteKind::Put, false);
    PB_CHECK_EQ(a.error.key, std::string("no_connection"));
    PB_CHECK(!a.outcome_unknown);
    PB_CHECK(a.error.cls == pbc::ErrorClass::Network);
    // a certificate problem keeps its own key (the user must be told the real reason)
    pbc::Response tls = no_answer(Delivery::NotSent, "cert_untrusted");
    tls.error = pbc::ErrorClass::Tls;
    PB_CHECK_EQ(pbc::interpret_write_answer(tls, WriteKind::Move, false).error.key, std::string("cert_untrusted"));
    pbc::Response redirect = no_answer(Delivery::NotSent, "redirect_other_host");
    redirect.error = pbc::ErrorClass::Tls;
    PB_CHECK_EQ(pbc::interpret_write_answer(redirect, WriteKind::Put, false).error.key, std::string("redirect_other_host"));
    PB_CHECK(!pbc::interpret_write_answer(redirect, WriteKind::Put, false).outcome_unknown);
}

PB_TEST(no_answer_after_the_request_went_out_is_an_unknown_outcome) {
    for (WriteKind k : {WriteKind::Put, WriteKind::Mkcol, WriteKind::Move, WriteKind::Copy, WriteKind::Delete}) {
        for (Delivery d : {Delivery::PartlySent, Delivery::FullySent}) {
            const auto a = pbc::interpret_write_answer(no_answer(d), k, false);
            PB_CHECK(a.outcome_unknown);
            PB_CHECK_EQ(a.error.key, std::string("outcome_unknown"));
        }
    }
    // a chunk of the app's own upload session is idempotent: the next send lists the upload folder, nothing is unknown
    const auto c = pbc::interpret_write_answer(no_answer(Delivery::FullySent), WriteKind::Chunk, false);
    PB_CHECK(!c.outcome_unknown);
    PB_CHECK_EQ(c.error.key, std::string("connection_lost"));
}

PB_TEST(a_cancel_is_a_cancel_and_not_an_unknown_outcome) {
    pbc::Response r = no_answer(Delivery::PartlySent, "cancelled");
    r.cancelled = true;
    r.error = pbc::ErrorClass::Cancelled;
    const auto a = pbc::interpret_write_answer(r, WriteKind::Put, false);
    PB_CHECK_EQ(a.error.key, std::string("cancelled"));
    PB_CHECK(!a.outcome_unknown);
}

PB_TEST(a_proxy_error_after_a_write_is_unknown_but_a_busy_server_is_not) {
    for (long s : {500L, 502L, 504L}) {
        for (WriteKind k : {WriteKind::Put, WriteKind::Mkcol, WriteKind::Move, WriteKind::Copy, WriteKind::Delete}) {
            const auto a = answer(s, k);
            PB_CHECK(a.outcome_unknown);
            PB_CHECK_EQ(a.error.key, std::string("outcome_unknown"));
        }
        PB_CHECK(!answer(s, WriteKind::Chunk).outcome_unknown);   // the chunk is verified by listing
        PB_CHECK_EQ(answer(s, WriteKind::Chunk).error.key, std::string("server_error"));
    }
    PB_CHECK(!answer(503, WriteKind::Put).outcome_unknown);   // the server states that it did not execute it
    PB_CHECK(!answer(429, WriteKind::Put).outcome_unknown);
    PB_CHECK(!answer(501, WriteKind::Put).outcome_unknown);
    PB_CHECK_EQ(answer(501, WriteKind::Put).error.key, std::string("server_error"));
}

PB_TEST(every_key_the_mapper_can_return_is_a_known_error_key) {
    for (long s : {200L, 201L, 204L, 207L, 400L, 401L, 403L, 404L, 405L, 409L, 412L, 413L, 415L, 418L, 423L, 429L, 500L, 501L, 502L, 503L, 504L, 507L})
        for (WriteKind k : {WriteKind::Put, WriteKind::Mkcol, WriteKind::Move, WriteKind::Copy, WriteKind::Delete, WriteKind::Chunk})
            for (bool replacing : {false, true}) {
                const auto a = answer(s, k, replacing);
                if (a.error.ok()) continue;
                PB_CHECK(pbc::is_known_error_key(a.error.key));
            }
    for (Delivery d : {Delivery::NotSent, Delivery::PartlySent, Delivery::FullySent})
        PB_CHECK(pbc::is_known_error_key(pbc::interpret_write_answer(no_answer(d), WriteKind::Put, false).error.key));
}

PB_TEST(the_recorded_shapes_of_write_answers_map_to_their_keys) {
    // 207 with members that failed (a locked file inside a folder that is moved, copied or deleted): partial_failure, nothing patched
    for (WriteKind k : {WriteKind::Move, WriteKind::Copy, WriteKind::Delete})
        PB_CHECK_EQ(answer(207, k, false, pbtest::fixture("nextcloud/write/dav/move_207_partial.unverified.xml")).error.key, std::string("partial_failure"));
    PB_CHECK_EQ(answer(207, WriteKind::Delete, false, pbtest::fixture("nextcloud/write/dav/delete_207_partial.unverified.xml")).error.key, std::string("partial_failure"));
    // MKCOL of a name that exists: 405
    PB_CHECK_EQ(answer(405, WriteKind::Mkcol, false, pbtest::fixture("nextcloud/write/dav/mkcol_405_exists.unverified.xml")).error.key, std::string("name_exists"));
    // PUT with a condition that failed: 412 is the name for a new file and the version for a replace
    const std::string pre = pbtest::fixture("nextcloud/write/put/put_412_precondition.unverified.xml");
    PB_CHECK_EQ(answer(412, WriteKind::Put, false, pre).error.key, std::string("name_exists"));
    PB_CHECK_EQ(answer(412, WriteKind::Put, true, pre).error.key, std::string("changed_on_server"));
    PB_CHECK_EQ(answer(413, WriteKind::Put, false, pbtest::fixture("nextcloud/write/put/put_413_too_large.unverified.xml")).error.key, std::string("file_too_large_server"));
}

PB_TEST_MAIN()
