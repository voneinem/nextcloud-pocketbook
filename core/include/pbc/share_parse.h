#pragma once
// The tolerant parsers of the share answers (specs/004-share-links, contracts/core-interfaces.md, research R-5, R-6, R-14). Total and bounded
// on hostile input: any body gives a result or `protocol_error` (the detail names the kind of problem, never the content). The list parser has
// no code path that reads `url` or `token`; only parse_share_detail reads the link. Password hashes are never copied out. Qt-free.

#include <string>
#include <string_view>
#include <vector>

#include "pbc/share_types.h"
#include "pbc/types.h"

namespace pbc {

struct ShareList {
    std::vector<ShareRow> rows;
    int dropped = 0;          // rows without a positive integer id, rows that are not objects, duplicate ids
    bool truncated = false;   // more than kMaxShareRows rows came back; the first kMaxShareRows are kept
};

// L2/L3: `data` must be an array. `item_is_folder` is used for a row that does not say its item_type. `inherited` marks every row.
Result<ShareList> parse_share_list(std::string_view body, const std::string& my_user_id, bool inherited, bool item_is_folder);

// L4/L5: the first (or only) share object of `data`. The link of a public link share is checked per data-model "ShareDetail": an
// `https://` address of at most 512 printable ASCII characters without blanks and user information is Ok, or DifferentAddress when its host
// is not `connected_host`; `http://` is NotSecure (shown); anything else is Unreadable and the link is empty.
Result<ShareDetail> parse_share_detail(std::string_view body, const std::string& my_user_id, std::string_view connected_host, bool item_is_folder);

// The OCS envelope: `ok` when statuscode is 100 or 200; otherwise the integer. `message` is never read. protocol_error for a body without an
// integer statuscode.
struct OcsStatus {
    bool ok = false;
    int statuscode = 0;
};
Result<OcsStatus> parse_ocs_status(std::string_view body);

}  // namespace pbc
