#pragma once
// Validation before a share request is sent, and the diagnosis of a refusal (specs/004-share-links, research R-4, data-model "Validation
// rules"). The message is chosen from the rules and the status only; no member can hold text of the server. Pure. Qt-free.

#include <cstdint>
#include <optional>
#include <string>

#include "pbc/share_rules.h"
#include "pbc/share_types.h"

namespace pbc {

struct ShareMessage {
    enum class Field { None, Password, Expiry, Permission };
    static constexpr unsigned kPassword = 1, kExpiry = 2, kPermission = 4;

    std::string key;                   // empty = nothing to report
    Field field = Field::None;         // the control the first message is about
    unsigned field_mask = 0;           // every control a violated rule is about (a forced password and a forced expiry give both bits)
    std::optional<int> limit_days;     // the server's maximum, when an expiry rule is violated and the maximum is known
    std::string code;                  // "E403" for share_refused, else empty

    bool empty() const { return key.empty(); }
};

enum class ShareOp { List, Create, Change, Remove, Detail };   // Detail: the read of one share (404 -> share_gone; no rules are needed)

// Checks a request against the rules BEFORE sending. Format errors first (permission_invalid, password_invalid, expiry_invalid,
// expiry_in_past), then the rules: sharing off, public links off, password, expiry, upload. An empty key means the request may be sent.
// `today` is the server's day (or the reader's) in days since 1970-01-01.
ShareMessage validate_create(const ShareRules& rules, const ShareTarget& target, const LinkRequest& request, int64_t today);
// `current` is the share as the screen shows it; a share that is not an own public link gives share_not_changeable.
ShareMessage validate_change(const ShareRules& rules, const ShareTarget& target, const ShareRow& current, const LinkChange& change, int64_t today);

struct RefusalContext {
    const ShareRules& fresh;           // the rules re-read after the refusal
    ShareTarget target;
    ShareOp op;
    const LinkRequest* create;         // the request that was refused (Create)
    const LinkChange* change;          // (Change)
    const ShareRow* current;           // the share as shown (Change)
    long http_status;                  // the HTTP status of the refused request
    int ocs_status;                    // the OCS statuscode (0 = none); for a 2xx answer with a statuscode >= 400 that code counts as the status
    std::optional<bool> item_exists;   // the answer of the one Depth-0 PROPFIND made on a 403/404 of the path; nullopt = not made or failed
    int64_t today;
};
// R-4: login/rate/maintenance statuses keep their keys; otherwise the first violated rule of the fresh rules; else for Create/List
// share_item_gone (item_exists == false) or share_item_not_shareable (403/404), for Change/Remove share_not_changeable (403) or share_gone
// (404); anything else share_refused with code "E<status>".
ShareMessage diagnose_refusal(const RefusalContext& context);

}  // namespace pbc
