// Dates in the reader's own time zone (research R-14). The reader's zone is read from its configuration; without one the dates are
// shown in UTC and labelled so, never silently in a wrong zone.
#include <memory>
#include <string>

#include "pbc/reader_zone.h"
#include "pbc/tz.h"
#include "test_support.h"

namespace {

std::unique_ptr<pbc::TzDatabase> db() {
    auto d = pbc::TzDatabase::open(std::string(PBC_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
    if (!d) PB_FAIL_AT("the bundled zone data cannot be opened");
    return d;
}

// 2026-07-15 12:00:00 UTC and 2026-01-15 12:00:00 UTC
constexpr int64_t kSummer = 1784116800;
constexpr int64_t kWinter = 1768478400;

}  // namespace

PB_TEST(the_timezone_key_of_the_configuration_is_read) {
    using pbc::timezone_from_config_text;
    PB_CHECK_EQ(timezone_from_config_text("a=1\ntimezone=Europe/Berlin\nb=2\n"), std::string("Europe/Berlin"));
    PB_CHECK_EQ(timezone_from_config_text("timezone = America/New_York \r\n"), std::string("America/New_York"));
    PB_CHECK_EQ(timezone_from_config_text("timezone=Europe/Berlin (CEST)\n"), std::string("Europe/Berlin"));
    PB_CHECK_EQ(timezone_from_config_text("timezone=First/Zone\ntimezone=Second/Zone\n"), std::string("First/Zone"));
    PB_CHECK_EQ(timezone_from_config_text("timezone=\n"), std::string(""));
    PB_CHECK_EQ(timezone_from_config_text("other=1\n"), std::string(""));
    PB_CHECK_EQ(timezone_from_config_text("mytimezone=Europe/Berlin\n"), std::string(""));
    PB_CHECK_EQ(timezone_from_config_text(""), std::string(""));
}

PB_TEST(the_zone_comes_from_the_configuration_then_the_environment_and_otherwise_utc_is_labelled) {
    auto d = db();
    auto z = pbc::reader_zone(d.get(), "timezone=Europe/Berlin\n", {"", ""});
    PB_CHECK_EQ(z.name, std::string("Europe/Berlin"));
    PB_CHECK(!z.utc_fallback);
    z = pbc::reader_zone(d.get(), "timezone=Mars/Olympus\n", {"Asia/Tokyo", ""});   // not a bundled zone: the next source
    PB_CHECK_EQ(z.name, std::string("Asia/Tokyo"));
    z = pbc::reader_zone(d.get(), "", {"", ""});
    PB_CHECK_EQ(z.name, std::string("UTC"));
    PB_CHECK(z.utc_fallback);
    z = pbc::reader_zone(nullptr, "timezone=Europe/Berlin\n", {"", ""});   // no zone data at all
    PB_CHECK(z.utc_fallback);
}

PB_TEST(dates_follow_summer_and_winter_time) {
    auto d = db();
    const auto berlin = pbc::reader_zone(d.get(), "timezone=Europe/Berlin\n", {"", ""});
    PB_CHECK_EQ(pbc::format_date_time(kSummer, berlin, d.get()), std::string("2026-07-15 14:00"));
    PB_CHECK_EQ(pbc::format_date_time(kWinter, berlin, d.get()), std::string("2026-01-15 13:00"));
    const auto ny = pbc::reader_zone(d.get(), "timezone=America/New_York\n", {"", ""});
    PB_CHECK_EQ(pbc::format_date_time(kSummer, ny, d.get()), std::string("2026-07-15 08:00"));
    PB_CHECK_EQ(pbc::format_date_time(kWinter, ny, d.get()), std::string("2026-01-15 07:00"));
    PB_CHECK_EQ(pbc::format_date(kSummer, berlin, d.get()), std::string("2026-07-15"));
}

PB_TEST(the_day_changes_with_the_zone) {
    auto d = db();
    const auto berlin = pbc::reader_zone(d.get(), "timezone=Europe/Berlin\n", {"", ""});
    const int64_t late = 1792497600 + 11 * 3600 + 30 * 60;   // 2026-10-20 23:30 UTC: already the 21st in Berlin
    PB_CHECK_EQ(pbc::format_date(late, berlin, d.get()), std::string("2026-10-21"));
    const auto utc = pbc::reader_zone(d.get(), "", {"", ""});
    PB_CHECK_EQ(pbc::format_date(late, utc, d.get()), std::string("2026-10-20"));
}

PB_TEST(the_utc_fallback_is_labelled_where_the_time_is_shown_and_the_list_date_stays_short) {
    auto d = db();
    const auto utc = pbc::reader_zone(d.get(), "", {"", ""});
    PB_CHECK_EQ(pbc::format_date_time(kSummer, utc, d.get()), std::string("2026-07-15 12:00 UTC"));
    PB_CHECK_EQ(pbc::format_date(kSummer, utc, d.get()), std::string("2026-07-15"));
    PB_CHECK_EQ(pbc::format_date_time(kSummer, utc, nullptr), std::string("2026-07-15 12:00 UTC"));   // even without zone data
}

PB_TEST_MAIN()
