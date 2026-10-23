#include "pbc/share_date.h"

#include <cstdio>

#include "pbc/civil.h"

namespace pbc {

namespace {

bool digit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

std::optional<int64_t> parse_iso_day(std::string_view t) {
    if (t.size() != 10 || t[4] != '-' || t[7] != '-') return std::nullopt;
    for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u})
        if (!digit(t[i])) return std::nullopt;
    const int y = (t[0] - '0') * 1000 + (t[1] - '0') * 100 + (t[2] - '0') * 10 + (t[3] - '0');
    const int m = (t[5] - '0') * 10 + (t[6] - '0');
    const int d = (t[8] - '0') * 10 + (t[9] - '0');
    if (y < 1970 || !civil::valid_date(y, m, d)) return std::nullopt;
    return civil::days_from_civil(y, m, d);
}

std::string format_iso_day(int64_t days) {
    int y = 0, m = 0, d = 0;
    civil::civil_from_days(days, &y, &m, &d);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", y, m, d);
    return buf;
}

Error check_expiry(std::string_view text, int64_t today, std::optional<int> max_days) {
    const std::optional<int64_t> day = parse_iso_day(text);
    if (!day) return make_error(ErrorClass::Protocol, "expiry_invalid");
    if (*day < today) return make_error(ErrorClass::Protocol, "expiry_in_past");
    if (max_days && *day > today + *max_days) return make_error(ErrorClass::Protocol, "share_expiry_too_far");
    return Error();
}

}  // namespace pbc
