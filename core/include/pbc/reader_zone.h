#pragma once
// Dates in the reader's own time zone (research R-14). The zone is read from the reader's configuration (the `timezone` key; unverified
// on the device, DT-NC-6), then from the environment; without one the dates are shown in UTC and the details are labelled "UTC", never
// silently in a wrong zone. The zone data is the bundled IANA database (tz.h), never the device's own. No Qt.

#include <cstdint>
#include <string>
#include <string_view>

#include "pbc/tz.h"

namespace pbc {

struct ReaderZone {
    std::string name = "UTC";
    bool utc_fallback = true;   // no zone could be determined
};

// The value of the `timezone` key of a key=value configuration text ("" if absent or empty): first occurrence, blanks around the equals
// sign and the line end tolerated, a trailing remark after a blank ("Europe/Berlin (CEST)") dropped.
std::string timezone_from_config_text(std::string_view config);

// The configuration first, then TZ and the target of /etc/localtime (tz.h), else UTC with utc_fallback. `db` may be null.
ReaderZone reader_zone(const TzDatabase* db, std::string_view config_text, const ZoneEnvironment& env);

// "2026-07-15" and "2026-07-15 14:00" (with " UTC" appended when the zone is the fallback). `db` may be null (then UTC).
std::string format_date(int64_t epoch, const ReaderZone& zone, const TzDatabase* db);
std::string format_date_time(int64_t epoch, const ReaderZone& zone, const TzDatabase* db);

}  // namespace pbc
