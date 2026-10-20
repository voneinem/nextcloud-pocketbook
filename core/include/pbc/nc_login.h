#pragma once
// Login Flow v2 (specs/001-nextcloud-files-client, research R-4, contracts/server-requests.md section 9). No password is typed on the
// reader: the user opens a link on another device, approves there, and taps Check on the reader. The poll token lives in memory only;
// there is no timer - every call of login_check sends exactly one request. Addresses of another origin are refused. Qt-free.

#include <optional>
#include <string>

#include "pbc/http_transport.h"
#include "pbc/types.h"

namespace pbc {

struct LoginStart {
    std::string login_url;       // for the other device
    std::string poll_endpoint;
    std::string token;           // a secret: never stored, never logged
};

struct LoginResult {
    std::string server;          // the canonical address the server reports
    std::string login_name;
    std::string app_password;    // a secret
};

// POST <base>/index.php/login/v2. Errors: redirect_other_host (the link or the poll address is not on the server's own https origin),
// protocol_error (no usable token or address), or the transport/status keys.
Result<LoginStart> login_start(HttpTransport& http, const std::string& base_url);

// One poll. Ok with no value: not approved yet (the flow stays open). Ok with a value: approved; the flow is discarded and the answer
// cannot be fetched again. Errors: login_expired (nothing to check), redirect_other_host (the flow ends), login_failed (the flow ends),
// or a network key (the flow stays open, the user may tap Check again).
Result<std::optional<LoginResult>> login_check(HttpTransport& http, const std::string& base_url, LoginStart& flow);

// Overwrites the token and clears the flow.
void login_discard(LoginStart& flow);

}  // namespace pbc
