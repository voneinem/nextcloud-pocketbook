#pragma once
// The requests to the Files Sharing API (specs/004-share-links/contracts/share-requests.md, research R-1 to R-9): rules, list, one share,
// create, change, remove. Each method sends exactly the requests its row lists, never retries and never logs more than a code, a status and
// a count. A password lives in a SecretText and in Request::body only for the duration of one call and is wiped on every path. This is the
// only module that names PUT or DELETE for shares (rule N2) and the only one that calls SecretText::reveal() (rule N8). UNVERIFIED against a
// real server (docs/live-server-checklist.md LV-L*). Qt-free.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pbc/dav_common.h"
#include "pbc/share_diagnose.h"
#include "pbc/share_parse.h"
#include "pbc/share_rules.h"
#include "pbc/share_types.h"
#include "pbc/types.h"

namespace pbc {

// Timeouts of every sharing request (connect 10 s, total 30 s) and the OCS prefix of the share resources.
inline constexpr long kShareConnectTimeoutS = 10;
inline constexpr long kShareTotalTimeoutS = 30;
inline constexpr const char* kShareApiPath = "/ocs/v2.php/apps/files_sharing/api/v1";

// application/x-www-form-urlencoded: every byte except A-Z a-z 0-9 - . _ ~ is percent-encoded (upper-case hex), a space is %20, never "+".
// Names are encoded like values; the order of the fields is kept. The only place that turns a password into bytes.
std::string form_encode(const std::vector<std::pair<std::string, std::string>>& fields);

namespace share_detail {
// What an answer is, before anything is read from it (the classification is in share_api.cpp).
struct Answer {
    enum class Kind { Ok, Refused, Transport, TooBig, Bad };
    Kind kind = Kind::Ok;
    long status = 0;   // the effective status: the HTTP status, or the OCS statuscode of a 2xx answer that carries a failing one
    int ocs = 0;
};
}  // namespace share_detail

class ShareApi {
public:
    // `user_id`: the signed-in user's id (the WebDAV files root for the one existence check, and the owner comparison).
    ShareApi(DavSession& session, const Clock& clock, std::string user_id);

    struct Listing {
        ShareList own;
        ShareList inherited;
        bool inherited_failed = false;   // L3 failed for another reason than "not offered"; the own rows are still good
    };

    Result<ShareRules> fetch_rules();                                                                    // L1
    Result<Listing> list(const ShareTarget& target);                                                     // L2 + L3
    Result<ShareDetail> detail(int64_t id, bool item_is_folder);                                         // L4
    // `rules` are those the screen holds; a violation is reported before anything is sent. The password is wiped on every path.
    Result<ShareDetail> create(const ShareTarget& target, const ShareRules& rules, LinkRequest&& request);   // L5 POST
    // L4, compare with `shown`, then one L5 PUT. share_changed_meanwhile fills current(); nothing is sent then.
    Result<ShareDetail> change(const ShareTarget& target, const ShareRules& rules, int64_t id, const Snapshot& shown, LinkChange&& change);
    // `owned_by_me`: the screen knows the share is not the user's: nothing is sent and the answer is share_not_changeable.
    Error remove_link(int64_t id, const ShareTarget& target, const ShareRules& rules, bool owned_by_me = true);   // L6

    // About the last call that failed: the message (key, marked fields, limit, short code), the share as it is now (after
    // share_changed_meanwhile), and whether the outcome is unknown (R-8: the request may have been processed). Cleared by the next call.
    const ShareMessage& message() const { return message_; }
    const ShareRow& current() const { return current_; }
    bool outcome_unknown() const { return unknown_; }
    // The HTTP Date of the last answer, when there was one (the server's day for expiry dates).
    std::optional<int64_t> server_time() const { return session_.last_server_time(); }

private:
    int64_t today() const;                                   // the server's day (HTTP Date) when known, else the reader's, in days since 1970-01-01
    std::string api_url(const std::string& sub) const;       // <base>/ocs/v2.php/apps/files_sharing/api/v1<sub>
    std::string item_url(const ShareTarget& target) const;   // the WebDAV address of the item (for the one existence check)
    void reset();
    Error fail(const std::string& key);
    std::optional<bool> item_exists(const ShareTarget& target);
    Error refusal(const Response& r, const share_detail::Answer& a, ShareOp op, const ShareTarget& target, const LinkRequest* create, const LinkChange* change,
                  const ShareRow* current);

    DavSession& session_;
    const Clock& clock_;
    std::string user_id_, host_;
    ShareMessage message_;
    ShareRow current_;
    bool unknown_ = false;
};

}  // namespace pbc
