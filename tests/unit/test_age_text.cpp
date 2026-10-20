// The age of a listing (FR-012, FR-035): what the page says about how old the shown folder is.
#include "pbc/age_text.h"
#include "test_support.h"

using pbc::AgeKind;
using pbc::listing_age;

namespace {
constexpr int64_t kNow = 1792497600;
}

PB_TEST(a_listing_that_was_never_confirmed_is_not_synced) {
    const auto a = listing_age(kNow, std::nullopt, false);
    PB_CHECK(a.kind == AgeKind::NotSynced);
}

PB_TEST(a_folder_that_is_gone_says_so_whatever_its_age) {
    PB_CHECK(listing_age(kNow, kNow - 5, true).kind == AgeKind::Gone);
    PB_CHECK(listing_age(kNow, std::nullopt, true).kind == AgeKind::Gone);
}

PB_TEST(the_age_is_given_in_the_largest_whole_unit) {
    auto age = [](int64_t seconds) { return listing_age(kNow, kNow - seconds, false); };
    PB_CHECK(age(0).kind == AgeKind::JustNow);
    PB_CHECK(age(59).kind == AgeKind::JustNow);
    PB_CHECK(age(60).kind == AgeKind::Minutes);
    PB_CHECK_EQ(age(60).n, 1);
    PB_CHECK_EQ(age(179).n, 2);
    PB_CHECK(age(3599).kind == AgeKind::Minutes);
    PB_CHECK_EQ(age(3599).n, 59);
    PB_CHECK(age(3600).kind == AgeKind::Hours);
    PB_CHECK_EQ(age(3 * 3600 + 100).n, 3);
    PB_CHECK_EQ(age(23 * 3600 + 3599).n, 23);
    PB_CHECK(age(86400).kind == AgeKind::Days);
    PB_CHECK_EQ(age(2 * 86400 + 5).n, 2);
    PB_CHECK_EQ(age(400 * 86400).n, 400);
}

PB_TEST(a_time_in_the_future_means_the_clock_is_wrong_and_reads_as_just_now) {
    const auto a = listing_age(kNow, kNow + 3600, false);
    PB_CHECK(a.kind == AgeKind::JustNow);
    PB_CHECK(a.clock_suspect);
    PB_CHECK(!listing_age(kNow, kNow - 5, false).clock_suspect);
}

PB_TEST_MAIN()
