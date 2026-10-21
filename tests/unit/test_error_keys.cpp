// The stable error keys of the files client (specs/001-nextcloud-files-client/contracts/server-requests.md "Error keys"): every key
// exists once, belongs to one error class, and an unknown key is a protocol error rather than a guess.
#include <set>
#include <string>

#include "pbc/error_keys.h"
#include "test_support.h"

PB_TEST(every_key_of_the_contract_is_listed_exactly_once) {
    const char* contract[] = {
        "scheme_not_allowed", "address_invalid", "clock_implausible", "no_network", "server_unreachable", "timeout",
        "cert_untrusted", "cert_expired", "cert_host_mismatch", "not_nextcloud", "maintenance", "server_too_old",
        "auth_failed", "auth_app_password_required", "no_webdav", "forbidden", "not_found", "rate_limited", "server_error",
        "protocol_error", "malformed_xml", "listing_too_large", "redirect_other_host", "login_failed", "login_expired",
        "too_large_for_storage", "not_enough_space", "storage_readonly", "storage_full", "checksum_mismatch", "size_mismatch",
        "local_changed", "locked", "path_too_long", "cancelled",
        // parser-level keys documented next to the contract
        "foreign_href", "qr_too_long", "connection_lost", "path_unsafe", "write_failed",
        // the write side (specs/002-manage-server-files/contracts/server-requests.md "Error keys added to 001's list")
        "server_storage_full", "file_too_large_server", "type_not_accepted", "name_forbidden", "not_allowed", "changed_on_server",
        "name_exists", "target_gone", "source_gone", "partial_failure", "server_refused", "queue_full", "refresh_first", "change_pending",
        "source_unreadable", "source_changed", "source_not_allowed", "trash_unavailable", "outcome_unknown", "other_account",
        "no_connection",
    };
    const auto& all = pbc::all_error_keys();
    std::set<std::string> seen(all.begin(), all.end());
    PB_CHECK_EQ(seen.size(), all.size());   // no duplicate
    for (const char* k : contract) PB_CHECK(seen.count(k) == 1);
    PB_CHECK_EQ(all.size(), sizeof(contract) / sizeof(contract[0]));   // and nothing undocumented
}

PB_TEST(a_key_belongs_to_one_error_class_and_error_from_key_agrees) {
    for (const std::string& k : pbc::all_error_keys()) {
        const pbc::Error e = pbc::error_from_key(k);
        PB_CHECK(e.cls != pbc::ErrorClass::None);
        PB_CHECK(e.cls == pbc::class_of(k));
        PB_CHECK_EQ(e.key, k);
    }
    PB_CHECK(pbc::class_of("auth_failed") == pbc::ErrorClass::Auth);
    PB_CHECK(pbc::class_of("cert_untrusted") == pbc::ErrorClass::Tls);
    PB_CHECK(pbc::class_of("redirect_other_host") == pbc::ErrorClass::Tls);
    PB_CHECK(pbc::class_of("storage_full") == pbc::ErrorClass::Storage);
    PB_CHECK(pbc::class_of("no_network") == pbc::ErrorClass::Network);
    PB_CHECK(pbc::class_of("cancelled") == pbc::ErrorClass::Cancelled);
    PB_CHECK(pbc::class_of("malformed_xml") == pbc::ErrorClass::Protocol);
}

PB_TEST(the_write_side_keys_have_the_classes_of_the_contract) {
    for (const char* k : {"server_storage_full", "file_too_large_server", "type_not_accepted", "name_forbidden", "not_allowed", "changed_on_server",
                          "name_exists", "target_gone", "source_gone", "partial_failure", "server_refused"})
        PB_CHECK(pbc::class_of(k) == pbc::ErrorClass::Server);
    for (const char* k : {"queue_full", "source_unreadable", "source_changed", "source_not_allowed"}) PB_CHECK(pbc::class_of(k) == pbc::ErrorClass::Storage);
    for (const char* k : {"no_connection", "outcome_unknown"}) PB_CHECK(pbc::class_of(k) == pbc::ErrorClass::Network);
    for (const char* k : {"refresh_first", "trash_unavailable"}) PB_CHECK(pbc::class_of(k) == pbc::ErrorClass::Unsupported);
    PB_CHECK(pbc::class_of("other_account") == pbc::ErrorClass::Auth);
    // an unknown key still falls back to a protocol error
    PB_CHECK(pbc::error_from_key("name_exist").key == "protocol_error");
}

PB_TEST(an_unknown_key_is_a_protocol_error) {
    const pbc::Error e = pbc::error_from_key("something_new");
    PB_CHECK_EQ(e.key, std::string("protocol_error"));
    PB_CHECK(e.cls == pbc::ErrorClass::Protocol);
    PB_CHECK(!pbc::is_known_error_key("something_new"));
    PB_CHECK(pbc::is_known_error_key("locked"));
    PB_CHECK(pbc::class_of("something_new") == pbc::ErrorClass::Protocol);
}

PB_TEST_MAIN()
