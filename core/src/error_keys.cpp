#include "pbc/error_keys.h"

#include <utility>

namespace pbc {

namespace {

struct Entry {
    const char* key;
    ErrorClass cls;
};

const Entry kEntries[] = {
    // address and local checks
    {"scheme_not_allowed", ErrorClass::Tls},
    {"address_invalid", ErrorClass::Unsupported},
    {"clock_implausible", ErrorClass::Unsupported},
    // network
    {"no_network", ErrorClass::Network},
    {"server_unreachable", ErrorClass::Network},
    {"timeout", ErrorClass::Network},
    {"connection_lost", ErrorClass::Network},
    // TLS
    {"cert_untrusted", ErrorClass::Tls},
    {"cert_expired", ErrorClass::Tls},
    {"cert_host_mismatch", ErrorClass::Tls},
    {"redirect_other_host", ErrorClass::Tls},
    // the server and the account
    {"not_nextcloud", ErrorClass::Unsupported},
    {"server_too_old", ErrorClass::Unsupported},
    {"no_webdav", ErrorClass::Unsupported},
    {"maintenance", ErrorClass::Server},
    {"auth_failed", ErrorClass::Auth},
    {"auth_app_password_required", ErrorClass::Auth},
    {"login_failed", ErrorClass::Auth},
    {"login_expired", ErrorClass::Auth},
    {"forbidden", ErrorClass::Server},
    {"not_found", ErrorClass::Server},
    {"rate_limited", ErrorClass::Server},
    {"server_error", ErrorClass::Server},
    {"locked", ErrorClass::Server},
    // answers that cannot be used
    {"protocol_error", ErrorClass::Protocol},
    {"malformed_xml", ErrorClass::Protocol},
    {"listing_too_large", ErrorClass::Protocol},
    {"foreign_href", ErrorClass::Protocol},
    {"checksum_mismatch", ErrorClass::Protocol},
    {"size_mismatch", ErrorClass::Protocol},
    {"qr_too_long", ErrorClass::Unsupported},
    // the reader's storage
    {"too_large_for_storage", ErrorClass::Storage},
    {"not_enough_space", ErrorClass::Storage},
    {"storage_readonly", ErrorClass::Storage},
    {"storage_full", ErrorClass::Storage},
    {"path_too_long", ErrorClass::Storage},
    {"path_unsafe", ErrorClass::Storage},
    {"write_failed", ErrorClass::Storage},
    {"local_changed", ErrorClass::Storage},
    // the write side (specs/002-manage-server-files)
    {"server_storage_full", ErrorClass::Server},
    {"file_too_large_server", ErrorClass::Server},
    {"type_not_accepted", ErrorClass::Server},
    {"name_forbidden", ErrorClass::Server},
    {"not_allowed", ErrorClass::Server},
    {"changed_on_server", ErrorClass::Server},
    {"name_exists", ErrorClass::Server},
    {"target_gone", ErrorClass::Server},
    {"source_gone", ErrorClass::Server},
    {"partial_failure", ErrorClass::Server},
    {"server_refused", ErrorClass::Server},
    {"trash_unavailable", ErrorClass::Unsupported},
    {"refresh_first", ErrorClass::Unsupported},
    {"change_pending", ErrorClass::Unsupported},
    {"outcome_unknown", ErrorClass::Network},
    {"no_connection", ErrorClass::Network},
    {"other_account", ErrorClass::Auth},
    {"queue_full", ErrorClass::Storage},
    {"source_unreadable", ErrorClass::Storage},
    {"source_changed", ErrorClass::Storage},
    {"source_not_allowed", ErrorClass::Storage},
    // folder synchronization (specs/003-folder-sync/data-model.md): refusals, skip reasons and the problems a pair can show
    {"pair_limit", ErrorClass::Unsupported},
    {"pair_overlap", ErrorClass::Unsupported},
    {"pair_folder_forbidden", ErrorClass::Unsupported},
    {"pair_limit_invalid", ErrorClass::Unsupported},
    {"confirmation_needed", ErrorClass::Unsupported},
    {"sync_busy", ErrorClass::Unsupported},
    {"plan_stale", ErrorClass::Unsupported},
    {"plan_not_approvable", ErrorClass::Unsupported},
    {"plan_too_large", ErrorClass::Unsupported},
    {"over_limit", ErrorClass::Unsupported},
    {"records_full", ErrorClass::Storage},
    {"changed_during_sync", ErrorClass::Storage},
    {"in_sync", ErrorClass::Unsupported},
    {"identical", ErrorClass::Unsupported},
    {"hidden_name", ErrorClass::Unsupported},
    {"direction", ErrorClass::Unsupported},
    {"kept_both", ErrorClass::Unsupported},
    {"by_user", ErrorClass::Unsupported},
    {"server_version_unknown", ErrorClass::Unsupported},
    {"unavailable", ErrorClass::Server},
    {"quota_exceeded", ErrorClass::Server},
    {"no_space", ErrorClass::Storage},
    {"over_reserve", ErrorClass::Storage},
    {"name_collision", ErrorClass::Storage},
    {"server_folder_missing", ErrorClass::Server},
    {"reader_folder_missing", ErrorClass::Storage},
    {"account_changed", ErrorClass::Auth},
    {"managed_by_sync", ErrorClass::Unsupported},
    // share links (specs/004-share-links/contracts/core-interfaces.md "Error keys"): refusals of the server, a lost answer, and client-side input errors
    {"share_not_offered", ErrorClass::Server},
    {"share_public_disabled", ErrorClass::Server},
    {"share_password_required", ErrorClass::Server},
    {"share_expiry_required", ErrorClass::Server},
    {"share_expiry_too_far", ErrorClass::Server},
    {"share_upload_not_allowed", ErrorClass::Server},
    {"share_item_not_shareable", ErrorClass::Server},
    {"share_item_gone", ErrorClass::Server},
    {"share_not_changeable", ErrorClass::Server},
    {"share_gone", ErrorClass::Server},
    {"share_changed_meanwhile", ErrorClass::Server},
    {"share_unknown_outcome", ErrorClass::Network},
    {"share_list_too_big", ErrorClass::Server},
    {"share_refused", ErrorClass::Server},
    {"expiry_invalid", ErrorClass::Protocol},
    {"expiry_in_past", ErrorClass::Protocol},
    {"password_invalid", ErrorClass::Protocol},
    {"permission_invalid", ErrorClass::Protocol},
    // the user
    {"cancelled", ErrorClass::Cancelled},
};

}  // namespace

const std::vector<std::string>& all_error_keys() {
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> v;
        for (const Entry& e : kEntries) v.emplace_back(e.key);
        return v;
    }();
    return keys;
}

bool is_known_error_key(const std::string& key) {
    for (const Entry& e : kEntries)
        if (key == e.key) return true;
    return false;
}

ErrorClass class_of(const std::string& key) {
    for (const Entry& e : kEntries)
        if (key == e.key) return e.cls;
    return ErrorClass::Protocol;
}

Error error_from_key(const std::string& key) {
    if (!is_known_error_key(key)) return make_error(ErrorClass::Protocol, "protocol_error");
    return make_error(class_of(key), key);
}

}  // namespace pbc
