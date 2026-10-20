#include "pbc/reader_zone.h"

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

ReaderZone reader_zone(const TzDatabase* db, std::string_view config_text, const ZoneEnvironment& env) {
    ReaderZone z;
    if (!db) return z;
    const std::string configured = timezone_from_config_text(config_text);
    if (!configured.empty()) {
        const std::string canonical = db->canonical_name(configured);
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
