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
