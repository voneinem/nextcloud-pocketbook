#include "pbc/clock_check.h"

#include <cctype>
#include <cstring>
#include <string>

#include "pbc/civil.h"

namespace pbc {

std::optional<int64_t> parse_http_date(std::string_view text) {
    std::string s(text);
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    s = s.substr(b, e - b);
    // "Wed, 14 Oct 2026 12:00:00 GMT": 29 characters
    if (s.size() != 29 || s[3] != ',' || s[4] != ' ' || s[7] != ' ' || s[11] != ' ' || s[16] != ' ' || s[19] != ':' || s[22] != ':' || s.compare(25, 4, " GMT") != 0) return std::nullopt;
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int month = 0;
    for (int i = 0; i < 12; ++i)
        if (s.compare(8, 3, months[i]) == 0) month = i + 1;
    if (month == 0) return std::nullopt;
    auto num = [&](size_t pos, size_t len, int* out) {
        int v = 0;
        for (size_t i = pos; i < pos + len; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
            v = v * 10 + (s[i] - '0');
        }
        *out = v;
        return true;
    };
    int day, year, hour, minute, second;
    if (!num(5, 2, &day) || !num(12, 4, &year) || !num(17, 2, &hour) || !num(20, 2, &minute) || !num(23, 2, &second)) return std::nullopt;
    if (year < 1970 || !civil::valid_date(year, month, day) || hour > 23 || minute > 59 || second > 60) return std::nullopt;
    return civil::days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + (second > 59 ? 59 : second);
}

}  // namespace pbc
