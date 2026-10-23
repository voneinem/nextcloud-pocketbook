#pragma once
// The expiry date of a share link (specs/004-share-links, research R-12): typed and sent as "YYYY-MM-DD" and nothing else. Pure; "today" is a
// parameter (the server's day from the HTTP Date header when known, else the reader's clock), so tests need no clock. Qt-free.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "pbc/types.h"

namespace pbc {

// Days since 1970-01-01 of a text of exactly four digits, '-', two digits, '-', two digits that is a real calendar date in 1970..9999.
// Anything else (a time part, an offset, blanks, a sign, non-ASCII digits, a NUL, an empty text) gives nullopt.
std::optional<int64_t> parse_iso_day(std::string_view text);
std::string format_iso_day(int64_t days);

// Ok, or: expiry_invalid (not a date), expiry_in_past (before `today`), share_expiry_too_far (after today + max_days; max_days nullopt = no limit).
// All errors are of class Protocol (client-side input errors).
Error check_expiry(std::string_view text, int64_t today, std::optional<int> max_days);

}  // namespace pbc
