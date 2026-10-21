// A 412 on MOVE or COPY has two causes (specs/002-manage-server-files, research R-3): the target exists, or the source changed. The caller
// reads target and source with two PROPFINDs (Depth 0) and this pure function decides which it was.
#include <optional>
#include <string>

#include "pbc/dav_write.h"
#include "test_support.h"

namespace {
pbc::RemoteEntry entry(const std::string& etag) {
    pbc::RemoteEntry e;
    e.name = "x";
    e.path = "/x";
    e.etag = etag;
    return e;
}
}  // namespace

PB_TEST(an_existing_target_is_a_name_collision) {
    PB_CHECK(pbc::disambiguate_412(entry("\"t\""), entry("\"s\""), "\"s\"") == pbc::Cause412::NameExists);
}

PB_TEST(an_existing_target_wins_even_when_the_source_also_changed_or_vanished) {
    PB_CHECK(pbc::disambiguate_412(entry("\"t\""), entry("\"other\""), "\"s\"") == pbc::Cause412::NameExists);
    PB_CHECK(pbc::disambiguate_412(entry("\"t\""), std::nullopt, "\"s\"") == pbc::Cause412::NameExists);
}

PB_TEST(a_vanished_source_is_gone) {
    PB_CHECK(pbc::disambiguate_412(std::nullopt, std::nullopt, "\"s\"") == pbc::Cause412::SourceGone);
}

PB_TEST(a_source_with_another_etag_has_changed) {
    PB_CHECK(pbc::disambiguate_412(std::nullopt, entry("\"newer\""), "\"s\"") == pbc::Cause412::ChangedOnServer);
}

PB_TEST(a_source_that_still_has_the_expected_etag_with_no_target_is_reported_as_changed) {
    // neither cause is visible any more (someone removed the colliding target meanwhile): the safe answer is to hold the item for the user
    PB_CHECK(pbc::disambiguate_412(std::nullopt, entry("\"s\""), "\"s\"") == pbc::Cause412::ChangedOnServer);
}

PB_TEST_MAIN()
