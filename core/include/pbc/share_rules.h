#pragma once
// What the server says about sharing (specs/004-share-links, contracts/core-interfaces.md, research R-3). The capabilities are untrusted and
// bounded: a missing or unusable value is "unknown", never a guess. Unknown is never "required" and never a limit. JSON paths are UNVERIFIED
// (docs/live-server-checklist.md LV-L1). Qt-free.

#include <optional>

#include "pbc/json_min.h"
#include "pbc/share_types.h"

namespace pbc {

struct ShareRules {
    std::optional<bool> api_enabled, public_enabled, upload_allowed, resharing;
    std::optional<bool> password_enforced, password_enforced_read_only, password_enforced_read_write, password_enforced_upload_only;
    std::optional<bool> expiry_enabled, expiry_enforced;
    std::optional<int> expiry_days;   // 1..3650, else unknown
};

// Reads the "files_sharing" object of the "capabilities" object of the OCS answer. Total: anything else gives unknown fields. At most
// 64 members of an object are looked at and no string is read.
ShareRules share_rules_from_capabilities(const JsonValue& capabilities);

bool sharing_offered(const ShareRules& rules);                          // api_enabled != false
bool link_creation_offered(const ShareRules& rules);                    // sharing_offered && public_enabled != false
bool password_required(const ShareRules& rules, LinkPermission p);      // the per-permission rule where present, else password_enforced == true
bool expiry_required(const ShareRules& rules);                          // expiry_enforced == true
// The maximum distance in days: known only when the expiry is enforced and `days` is usable (otherwise `days` is just a default).
std::optional<int> expiry_max_days(const ShareRules& rules);
bool upload_offered(const ShareRules& rules, bool is_folder);           // is_folder && upload_allowed != false

}  // namespace pbc
