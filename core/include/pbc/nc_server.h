#pragma once
// The server probe of the files client (specs/001-nextcloud-files-client, research R-3, contracts/server-requests.md sections 1-4):
// the address the user typed, status.php, the account check (capabilities, user, quota). Qt-free; every request goes through the
// HttpTransport seam, every secret through the transport's credential fields, never into a URL.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "pbc/http_transport.h"
#include "pbc/types.h"

namespace pbc {

// The oldest Nextcloud major version the app is written for (api-notes 3.1). Older servers get "server_too_old".
inline constexpr int kMinServerMajor = 30;

struct ServerInfo {
    std::string name = "Nextcloud";   // theming name or product name, sanitized, at most 64 bytes
    std::string version;              // digits and dots only, at most 32 characters
    int major = 0;
};

struct Quota {
    std::optional<int64_t> used;
    std::optional<int64_t> total;     // nullopt = unknown or unlimited
    bool unlimited = false;
};

struct AccountCheck {
    ServerInfo server;
    std::string user_id;
    Quota quota;
    std::optional<int64_t> server_time;            // from the HTTP Date header, if the server sent one
    int clock_warning_minutes = 0;                 // 0 = the clocks agree within ten minutes (or there is no Date header)
    std::optional<int64_t> retry_after_s;          // set on a failure with 429/503 that carries Retry-After (capped at 900)
};

// The address as the user typed it: https only, a bare host or host:port is completed with https://, one trailing slash is dropped,
// scheme and host are lower-cased. Refused: another scheme (scheme_not_allowed), user information, a query or fragment, a bad host
// or port, blanks and control characters (address_invalid).
Result<std::string> canonical_base_url(std::string_view typed);

// status.php (section 1). Ok for a usable Nextcloud; not_nextcloud, maintenance, server_too_old (with *out filled), server_error, or
// the transport's own key. *out gets the sanitized version and name whenever the document could be read.
Error classify_status_document(const Response& status_php, ServerInfo* out);

// Steps 0-4 of research R-3, stopping at the first failure. On a failure the value still carries what was learned (the version of a
// server that is too old, Retry-After). Nothing is stored.
Result<AccountCheck> check_account(HttpTransport& http, const std::string& base_url, const std::string& login_name,
                                   const std::string& app_password, const Clock& clock);

// The check at app start (research R-12): ONE authenticated request (the capabilities call) that confirms the login still works and
// refreshes the server name, version and the clock comparison. No status.php, no user id, no quota. Same error keys as check_account.
Result<AccountCheck> quick_check(HttpTransport& http, const std::string& base_url, const std::string& login_name,
                                 const std::string& app_password, const Clock& clock);

}  // namespace pbc
