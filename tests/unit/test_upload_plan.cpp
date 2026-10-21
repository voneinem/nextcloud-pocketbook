// The arithmetic of a chunked upload (specs/002-manage-server-files, research R-4): how a file is cut, how the chunks are named, and the limits.
#include "pbc/upload_session.h"
#include "test_support.h"

namespace {
constexpr int64_t kMiB = 1ll << 20;
}

PB_TEST(a_file_is_cut_into_chunks_of_the_given_size_with_a_smaller_last_one) {
    const auto p = pbc::plan_chunks(35 * kMiB, 10 * kMiB);
    PB_CHECK(p.has_value());
    PB_CHECK_EQ(p->count, 4);
    PB_CHECK_EQ(p->size_of(1), 10 * kMiB);
    PB_CHECK_EQ(p->size_of(3), 10 * kMiB);
    PB_CHECK_EQ(p->size_of(4), 5 * kMiB);
    PB_CHECK_EQ(p->offset_of(1), 0LL);
    PB_CHECK_EQ(p->offset_of(2), 10 * kMiB);
    PB_CHECK_EQ(p->offset_of(4), 30 * kMiB);
    int64_t sum = 0;
    for (int n = 1; n <= p->count; ++n) sum += p->size_of(n);
    PB_CHECK_EQ(sum, 35 * kMiB);
}

PB_TEST(a_file_of_exactly_one_chunk_or_a_multiple_has_no_short_chunk) {
    PB_CHECK_EQ(pbc::plan_chunks(10 * kMiB, 10 * kMiB)->count, 1);
    PB_CHECK_EQ(pbc::plan_chunks(10 * kMiB, 10 * kMiB)->size_of(1), 10 * kMiB);
    const auto p = pbc::plan_chunks(30 * kMiB, 10 * kMiB);
    PB_CHECK_EQ(p->count, 3);
    PB_CHECK_EQ(p->size_of(3), 10 * kMiB);
    PB_CHECK_EQ(pbc::plan_chunks(10 * kMiB + 1, 10 * kMiB)->count, 2);
    PB_CHECK_EQ(pbc::plan_chunks(10 * kMiB + 1, 10 * kMiB)->size_of(2), 1LL);
}

PB_TEST(an_empty_file_a_zero_chunk_or_more_than_ten_thousand_chunks_have_no_plan) {
    PB_CHECK(!pbc::plan_chunks(0, 10 * kMiB).has_value());
    PB_CHECK(!pbc::plan_chunks(-5, 10 * kMiB).has_value());
    PB_CHECK(!pbc::plan_chunks(10 * kMiB, 0).has_value());
    PB_CHECK(pbc::plan_chunks(10000ll * 5 * kMiB, 5 * kMiB).has_value());
    PB_CHECK_EQ(pbc::plan_chunks(10000ll * 5 * kMiB, 5 * kMiB)->count, 10000);
    PB_CHECK(!pbc::plan_chunks(10000ll * 5 * kMiB + 1, 5 * kMiB).has_value());
    // the largest file the reader can hold (4 GiB - 1) in the smallest allowed chunks is far below the limit
    PB_CHECK(pbc::plan_chunks(4294967295ll, 5 * kMiB).has_value());
    PB_CHECK(pbc::plan_chunks(4294967295ll, 5 * kMiB)->count < 1000);
}

PB_TEST(chunk_names_are_five_digits_so_that_name_order_is_chunk_order) {
    PB_CHECK_EQ(pbc::chunk_name(1), std::string("00001"));
    PB_CHECK_EQ(pbc::chunk_name(42), std::string("00042"));
    PB_CHECK_EQ(pbc::chunk_name(10000), std::string("10000"));
    for (int n = 1; n < 10000; ++n) PB_CHECK(pbc::chunk_name(n) < pbc::chunk_name(n + 1));
}

PB_TEST_MAIN()
