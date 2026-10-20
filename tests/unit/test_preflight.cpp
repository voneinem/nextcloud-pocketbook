// The checks before a download writes its first byte (spec FR-019, FR-023, SC-008): the FAT limit, free space with a margin, a warning
// for very large files, a card that cannot be written.
#include <string>

#include "fake_storage.h"
#include "pbc/downloader.h"
#include "test_support.h"

using pbc::Preflight;
using pbc::preflight_download;

namespace {
constexpr int64_t MiB = 1024 * 1024;
constexpr int64_t GiB = 1024 * MiB;
constexpr int64_t kWarn = 100 * MiB;

Preflight run(const pbtest::FakeStorage& s, std::optional<int64_t> size, int64_t warn = kWarn) { return preflight_download(s, "/card/PBCloud/Books", size, warn); }
}  // namespace

PB_TEST(a_small_file_on_a_card_with_room_is_ok) {
    pbtest::FakeStorage s;
    const auto p = run(s, 5 * MiB);
    PB_CHECK(p.verdict == Preflight::Verdict::Ok);
    PB_CHECK(p.error_key.empty());
}

PB_TEST(a_file_of_4_gib_or_more_is_refused_before_anything_else) {
    pbtest::FakeStorage s;
    s.free = 100ll * GiB;
    auto p = run(s, 4 * GiB);
    PB_CHECK(p.verdict == Preflight::Verdict::Refused);
    PB_CHECK_EQ(p.error_key, std::string("too_large_for_storage"));
    p = run(s, 4 * GiB - 1);   // the largest file a FAT card can hold
    PB_CHECK(p.verdict != Preflight::Verdict::Refused);
    p = run(s, 4 * GiB + 1);
    PB_CHECK_EQ(p.error_key, std::string("too_large_for_storage"));
    p = run(s, 40ll * GiB);
    PB_CHECK_EQ(p.error_key, std::string("too_large_for_storage"));
}

PB_TEST(free_space_must_cover_the_file_plus_a_margin_of_8_mib_or_1_percent) {
    pbtest::FakeStorage s;
    // 10 MiB: margin 8 MiB (more than 1 %) -> needs 18 MiB
    s.free = 18 * MiB - 1;
    auto p = run(s, 10 * MiB);
    PB_CHECK(p.verdict == Preflight::Verdict::Refused);
    PB_CHECK_EQ(p.error_key, std::string("not_enough_space"));
    PB_CHECK_EQ(p.size, 10 * MiB);
    PB_CHECK_EQ(p.free, 18 * MiB - 1);
    PB_CHECK_EQ(p.needed, 18 * MiB);
    s.free = 18 * MiB;
    PB_CHECK(run(s, 10 * MiB).verdict == Preflight::Verdict::Ok);
    // 2 GiB: the margin is 1 % = 20.48 MiB (rounded down to whole bytes)
    const int64_t size = 2 * GiB;
    s.free = size + size / 100 - 1;
    p = run(s, size, 4 * GiB);   // no warning at this size
    PB_CHECK_EQ(p.error_key, std::string("not_enough_space"));
    PB_CHECK_EQ(p.needed, size + size / 100);
    s.free = size + size / 100;
    PB_CHECK(run(s, size, 4 * GiB).verdict == Preflight::Verdict::Ok);
}

PB_TEST(a_file_at_or_above_the_warning_size_needs_the_users_confirmation) {
    pbtest::FakeStorage s;
    auto p = run(s, 100 * MiB - 1);
    PB_CHECK(p.verdict == Preflight::Verdict::Ok);
    p = run(s, 100 * MiB);
    PB_CHECK(p.verdict == Preflight::Verdict::NeedsConfirmation);
    p = run(s, 1 * GiB);
    PB_CHECK(p.verdict == Preflight::Verdict::NeedsConfirmation);
    p = run(s, 150 * MiB, 200 * MiB);   // a raised warning size
    PB_CHECK(p.verdict == Preflight::Verdict::Ok);
    // a large file that does not fit is refused, not merely warned about
    s.free = 50 * MiB;
    PB_CHECK_EQ(run(s, 1 * GiB).error_key, std::string("not_enough_space"));
}

PB_TEST(a_card_that_cannot_be_written_is_refused_with_its_own_reason) {
    pbtest::FakeStorage s;
    s.read_only = true;
    const auto p = run(s, 1 * MiB);
    PB_CHECK(p.verdict == Preflight::Verdict::Refused);
    PB_CHECK_EQ(p.error_key, std::string("storage_readonly"));
}

PB_TEST(an_unknown_size_passes_and_an_unknown_free_space_does_not_block) {
    pbtest::FakeStorage s;
    auto p = run(s, std::nullopt);
    PB_CHECK(p.verdict == Preflight::Verdict::Ok);   // the stream is checked against the space while it is written
    s.free = std::nullopt;
    p = run(s, 5 * MiB);
    PB_CHECK(p.verdict == Preflight::Verdict::Ok);
}

PB_TEST(the_numbers_are_returned_for_the_message) {
    pbtest::FakeStorage s;
    s.free = 3 * MiB;
    const auto p = run(s, 700 * MiB);
    PB_CHECK_EQ(p.size, 700 * MiB);
    PB_CHECK_EQ(p.free, 3 * MiB);
    PB_CHECK(p.needed > p.size);
}

PB_TEST_MAIN()
