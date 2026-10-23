// The expiry date of a share link (specs/004-share-links, contracts/core-interfaces.md "share_date.h", research R-12): only "YYYY-MM-DD", a
// real calendar date, checked against the server's day.
#include <optional>
#include <string>

#include "pbc/share_date.h"
#include "test_support.h"

namespace {

int64_t day(const char* iso) {
    const auto d = pbc::parse_iso_day(iso);
    if (!d) PB_FAIL_AT(std::string("a good date was refused: ") + iso);
    return *d;
}

}  // namespace

PB_TEST(real_calendar_dates_are_accepted) {
    PB_CHECK_EQ(day("1970-01-01"), int64_t(0));
    PB_CHECK_EQ(day("1970-01-02"), int64_t(1));
    PB_CHECK(pbc::parse_iso_day("2028-02-29").has_value());     // a leap day
    PB_CHECK(pbc::parse_iso_day("2000-02-29").has_value());     // divisible by 400
    PB_CHECK(pbc::parse_iso_day("9999-12-31").has_value());
    PB_CHECK_EQ(day("2026-10-22") - day("2026-10-21"), int64_t(1));
}

PB_TEST(impossible_and_malformed_dates_are_refused) {
    for (const char* bad : {"2027-02-29", "1900-02-29", "2026-13-01", "2026-00-10", "2026-04-31", "2026-01-00", "2026-01-32",
                            "1969-12-31", "10000-01-01", "0999-01-01"})
        PB_CHECK(!pbc::parse_iso_day(bad).has_value());
}

PB_TEST(nothing_but_year_month_day_is_accepted) {
    for (const char* bad : {"", "2026-10-22 00:00:00", "2026-10-22T00:00:00Z", "2026-10-22Z", "2026-10-22+02:00", " 2026-10-22", "2026-10-22 ",
                            "2026-1-2", "26-10-22", "2026/10/22", "20261022", "+2026-10-22", "-2026-10-22", "2026-10-2x", "2026.10.22",
                            "\xd9\xa2\xd9\xa0\xd9\xa2\xd9\xa6-10-22" /* Arabic-Indic digits */, "2026-10-22\n"})
        PB_CHECK(!pbc::parse_iso_day(std::string_view(bad, std::char_traits<char>::length(bad))).has_value());
    // a NUL inside the text is refused, not read as the end
    PB_CHECK(!pbc::parse_iso_day(std::string_view("2026-10-22\0x", 12)).has_value());
}

PB_TEST(format_and_parse_round_trip) {
    for (const char* d : {"1970-01-01", "2026-10-22", "2028-02-29", "9999-12-31", "2000-02-29"}) PB_CHECK_EQ(pbc::format_iso_day(day(d)), std::string(d));
}

PB_TEST(today_is_allowed_yesterday_is_in_the_past) {
    const int64_t today = day("2026-10-22");
    PB_CHECK(pbc::check_expiry("2026-10-22", today, std::nullopt).ok());
    const pbc::Error past = pbc::check_expiry("2026-10-21", today, std::nullopt);
    PB_CHECK_EQ(past.key, std::string("expiry_in_past"));
    PB_CHECK(past.cls == pbc::ErrorClass::Protocol);
}

PB_TEST(the_maximum_distance_is_inclusive) {
    const int64_t today = day("2026-10-22");
    PB_CHECK(pbc::check_expiry("2026-10-29", today, 7).ok());                                              // today + 7
    PB_CHECK_EQ(pbc::check_expiry("2026-10-30", today, 7).key, std::string("share_expiry_too_far"));      // today + 8
    PB_CHECK(pbc::check_expiry("2036-10-22", today, std::nullopt).ok());                                   // no limit
    PB_CHECK(pbc::check_expiry("2026-10-22", today, 1).ok());
}

PB_TEST(a_malformed_text_gives_expiry_invalid_before_any_range_check) {
    const int64_t today = day("2026-10-22");
    for (const char* bad : {"", "tomorrow", "2026-02-30", "2026-10-22 12:00"}) {
        const pbc::Error e = pbc::check_expiry(bad, today, 7);
        PB_CHECK_EQ(e.key, std::string("expiry_invalid"));
        PB_CHECK(e.cls == pbc::ErrorClass::Protocol);
    }
}

PB_TEST_MAIN()
