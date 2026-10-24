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
        // folder synchronization (specs/003-folder-sync/data-model.md "Reason and key vocabulary")
        "pair_limit", "pair_overlap", "pair_folder_forbidden", "pair_limit_invalid", "sync_busy", "plan_stale", "plan_not_approvable",
        "plan_too_large", "over_limit", "records_full", "changed_during_sync", "confirmation_needed",
        "in_sync", "identical", "hidden_name", "direction", "kept_both", "unavailable", "by_user", "server_version_unknown", "quota_exceeded", "no_space", "over_reserve",
        "name_collision", "server_folder_missing", "reader_folder_missing", "account_changed", "managed_by_sync",
        // share links (specs/004-share-links/contracts/core-interfaces.md "Error keys")
        "share_not_offered", "share_public_disabled", "share_password_required", "share_expiry_required", "share_expiry_too_far",
        "share_upload_not_allowed", "share_item_not_shareable", "share_item_gone", "share_not_changeable", "share_gone",
        "share_changed_meanwhile", "share_unknown_outcome", "share_list_too_big", "share_refused",
        "expiry_invalid", "expiry_in_past", "password_invalid", "permission_invalid",
        // trash and versions (specs/006-trash-and-versions/contracts/core-interfaces.md "error_keys additions"); trash_unavailable, refresh_first
        // and outcome_unknown exist from feature 002
        "versions_unavailable", "restore_name_taken", "restore_parent_missing", "restore_place_unsupported", "restore_into_self", "trash_item_gone",
        "version_changed_meanwhile", "version_gone", "file_gone", "no_older_versions", "trash_not_actionable",
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

PB_TEST(the_trash_and_version_keys_have_their_classes) {
    for (const char* k : {"versions_unavailable", "restore_place_unsupported", "trash_unavailable", "refresh_first"})
        PB_CHECK(pbc::class_of(k) == pbc::ErrorClass::Unsupported);
    for (const char* k : {"restore_name_taken", "restore_parent_missing", "trash_item_gone", "version_changed_meanwhile", "version_gone", "file_gone", "no_older_versions"})
        PB_CHECK(pbc::class_of(k) == pbc::ErrorClass::Server);
    for (const char* k : {"restore_into_self", "trash_not_actionable"}) PB_CHECK(pbc::class_of(k) == pbc::ErrorClass::Protocol);
    PB_CHECK(pbc::class_of("outcome_unknown") == pbc::ErrorClass::Network);
}

PB_TEST(the_sync_keys_have_the_classes_of_the_data_model) {
    for (const char* k : {"pair_limit", "pair_overlap", "pair_folder_forbidden", "pair_limit_invalid", "sync_busy", "plan_stale", "plan_not_approvable",
                          "plan_too_large", "over_limit", "confirmation_needed", "in_sync", "identical", "hidden_name", "direction", "kept_both", "by_user", "server_version_unknown", "managed_by_sync"})
        PB_CHECK(pbc::class_of(k) == pbc::ErrorClass::Unsupported);
    for (const char* k : {"records_full", "changed_during_sync", "no_space", "over_reserve", "name_collision", "reader_folder_missing"})
        PB_CHECK(pbc::class_of(k) == pbc::ErrorClass::Storage);
    for (const char* k : {"unavailable", "quota_exceeded", "server_folder_missing"}) PB_CHECK(pbc::class_of(k) == pbc::ErrorClass::Server);
    PB_CHECK(pbc::class_of("account_changed") == pbc::ErrorClass::Auth);
    // keys that already existed keep their class and meaning
    for (const char* k : {"name_forbidden", "name_exists"}) PB_CHECK(pbc::class_of(k) == pbc::ErrorClass::Server);
    for (const char* k : {"path_unsafe", "path_too_long", "source_unreadable", "too_large_for_storage", "storage_full", "storage_readonly", "queue_full", "local_changed"})
        PB_CHECK(pbc::class_of(k) == pbc::ErrorClass::Storage);
    for (const char* k : {"trash_unavailable", "refresh_first"}) PB_CHECK(pbc::class_of(k) == pbc::ErrorClass::Unsupported);
    PB_CHECK(pbc::is_known_error_key("change_pending"));
}

PB_TEST(the_share_keys_have_the_classes_of_the_contract) {
    for (const char* k : {"share_not_offered", "share_public_disabled", "share_password_required", "share_expiry_required", "share_expiry_too_far",
                          "share_upload_not_allowed", "share_item_not_shareable", "share_item_gone", "share_not_changeable", "share_gone",
                          "share_changed_meanwhile", "share_list_too_big", "share_refused"})
        PB_CHECK(pbc::class_of(k) == pbc::ErrorClass::Server);
    PB_CHECK(pbc::class_of("share_unknown_outcome") == pbc::ErrorClass::Network);
    // client-side input errors never come from the network; they are Protocol by the contract
    for (const char* k : {"expiry_invalid", "expiry_in_past", "password_invalid", "permission_invalid"})
        PB_CHECK(pbc::class_of(k) == pbc::ErrorClass::Protocol && pbc::is_known_error_key(k));
    // keys the share code reuses keep their class
    PB_CHECK(pbc::class_of("auth_failed") == pbc::ErrorClass::Auth);
    PB_CHECK(pbc::class_of("no_network") == pbc::ErrorClass::Network);
    PB_CHECK(pbc::class_of("rate_limited") == pbc::ErrorClass::Server);
    PB_CHECK(pbc::class_of("maintenance") == pbc::ErrorClass::Server);
    PB_CHECK(pbc::class_of("protocol_error") == pbc::ErrorClass::Protocol);
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
