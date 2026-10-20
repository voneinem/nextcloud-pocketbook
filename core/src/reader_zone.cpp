#include "pbc/reader_zone.h"

#include <cctype>
#include <cstdio>

namespace pbc {

std::string timezone_from_config_text(std::string_view config) {
    size_t pos = 0;
    while (pos <= config.size()) {
        size_t eol = config.find('\n', pos);
        if (eol == std::string_view::npos) eol = config.size();
        std::string_view line = config.substr(pos, eol - pos);
        pos = eol + 1;
        size_t b = line.find_first_not_of(" \t\r");
        if (b == std::string_view::npos) continue;
        line.remove_prefix(b);
        if (line.compare(0, 8, "timezone") != 0) continue;
        line.remove_prefix(8);
        size_t eq = line.find_first_not_of(" \t");
        if (eq == std::string_view::npos || line[eq] != '=') continue;   // "timezoneX=" or "timezone" alone
        line.remove_prefix(eq + 1);
        size_t v = line.find_first_not_of(" \t");
        if (v == std::string_view::npos) return std::string();
        line.remove_prefix(v);
        size_t end = line.find_first_of(" \t\r");
        return std::string(line.substr(0, end));
    }
    return std::string();
}

namespace {

std::string lower_of(std::string_view v) {
    std::string out(v);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The reader names its zone by the city only ("Berlin", seen on the InkPad One): the one bundled zone that ends in that city, or "" when
// there is none or more than one (a guess between two regions could show a wrong time).
std::string zone_of_city(const TzDatabase& db, const std::string& city) {
    const std::string want = lower_of(city);
    if (want.empty() || want.find('/') != std::string::npos) return std::string();
    std::string found;
    for (const std::string& name : db.zone_names()) {
        const size_t slash = name.rfind('/');
        if (slash == std::string::npos || lower_of(std::string_view(name).substr(slash + 1)) != want) continue;
        if (!found.empty()) return std::string();   // ambiguous
        found = name;
    }
    return found;
}

}  // namespace

ReaderZone reader_zone(const TzDatabase* db, std::string_view config_text, const ZoneEnvironment& env) {
    ReaderZone z;
    if (!db) return z;
    const std::string configured = timezone_from_config_text(config_text);
    if (!configured.empty()) {
        std::string canonical = db->canonical_name(configured);
        if (canonical.empty()) canonical = zone_of_city(*db, configured);
        if (!canonical.empty()) {
            z.name = canonical;
            z.utc_fallback = false;
            return z;
        }
    }
    const ZoneChoice choice = resolve_user_zone(std::nullopt, *db, env);
    if (choice.state == ZoneChoice::State::Resolved) {
        z.name = choice.name;
        z.utc_fallback = false;
    }
    return z;
}

namespace {
LocalDateTime local_of(int64_t epoch, const ReaderZone& zone, const TzDatabase* db) {
    if (db && !zone.utc_fallback) return db->utc_to_local(zone.name, epoch);
    int64_t days = epoch >= 0 ? epoch / 86400 : -((-epoch + 86399) / 86400);
    const int64_t rest = epoch - days * 86400;
    LocalDateTime t;
    civil::civil_from_days(days, &t.year, &t.month, &t.day);
    t.hour = static_cast<int>(rest / 3600);
    t.minute = static_cast<int>((rest % 3600) / 60);
    t.second = static_cast<int>(rest % 60);
    return t;
}
}  // namespace

std::string format_date(int64_t epoch, const ReaderZone& zone, const TzDatabase* db) {
    const LocalDateTime t = local_of(epoch, zone, db);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", t.year, t.month, t.day);
    return buf;
}

std::string format_date_time(int64_t epoch, const ReaderZone& zone, const TzDatabase* db) {
    const LocalDateTime t = local_of(epoch, zone, db);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", t.year, t.month, t.day, t.hour, t.minute);
    std::string s = buf;
    if (zone.utc_fallback || !db) s += " UTC";
    return s;
}

}  // namespace pbc
