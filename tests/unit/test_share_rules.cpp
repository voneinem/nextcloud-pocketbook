// What the server says about sharing (specs/004-share-links, contracts/core-interfaces.md "share_rules.h", research R-3): the capabilities are
// untrusted and bounded; unknown stays unknown and is never read as "required" or as a limit.
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "fixtures.h"
#include "pbc/json_min.h"
#include "pbc/share_rules.h"
#include "test_support.h"

namespace {

pbc::ShareRules rules_of(const std::string& fixture_name) {
    const auto doc = pbc::parse_json(pbtest::fixture("nextcloud/shares/capabilities/" + fixture_name));
    if (!doc.ok()) PB_FAIL_AT("the fixture is not JSON: " + fixture_name);
    const pbc::JsonValue* v = &doc.value;
    for (const char* k : {"ocs", "data", "capabilities"}) {
        v = v->get(k);
        if (!v) PB_FAIL_AT("no capabilities in " + fixture_name);
    }
    return pbc::share_rules_from_capabilities(*v);
}

}  // namespace

PB_TEST(the_old_shape_is_read_and_enforced_for_is_unknown) {
    const auto r = rules_of("old.unverified.json");
    PB_CHECK(r.api_enabled == std::optional<bool>(true));
    PB_CHECK(r.public_enabled == std::optional<bool>(true));
    PB_CHECK(r.upload_allowed == std::optional<bool>(true));
    PB_CHECK(r.password_enforced == std::optional<bool>(false));
    PB_CHECK(!r.password_enforced_read_only.has_value());
    PB_CHECK(r.expiry_enabled == std::optional<bool>(true));
    PB_CHECK(r.expiry_enforced == std::optional<bool>(false));
    PB_CHECK(r.expiry_days == std::optional<int>(7));
    PB_CHECK(pbc::sharing_offered(r));
    PB_CHECK(pbc::link_creation_offered(r));
    PB_CHECK(!pbc::password_required(r, pbc::LinkPermission::ReadOnly));
    PB_CHECK(!pbc::password_required(r, pbc::LinkPermission::ReadUpload));
    PB_CHECK(!pbc::expiry_required(r));
    // days is the default distance when the expiry is not enforced, not a maximum
    PB_CHECK(!pbc::expiry_max_days(r).has_value());
    PB_CHECK(pbc::upload_offered(r, true));
}

PB_TEST(the_new_shape_has_per_permission_password_rules_and_a_forced_expiry) {
    const auto r = rules_of("new.unverified.json");
    PB_CHECK(r.password_enforced_read_only == std::optional<bool>(true));
    PB_CHECK(r.password_enforced_read_write == std::optional<bool>(true));
    PB_CHECK(r.password_enforced_upload_only == std::optional<bool>(false));
    PB_CHECK(pbc::password_required(r, pbc::LinkPermission::ReadOnly));
    PB_CHECK(pbc::password_required(r, pbc::LinkPermission::ReadUpload));
    PB_CHECK(!pbc::password_required(r, pbc::LinkPermission::UploadOnly));
    PB_CHECK(pbc::expiry_required(r));
    PB_CHECK(pbc::expiry_max_days(r) == std::optional<int>(30));
    PB_CHECK(r.upload_allowed == std::optional<bool>(false));
    PB_CHECK(!pbc::upload_offered(r, true));
}

PB_TEST(sharing_off_and_public_links_off_are_distinguished) {
    const auto off = rules_of("sharing-off.unverified.json");
    PB_CHECK(!pbc::sharing_offered(off));
    PB_CHECK(!pbc::link_creation_offered(off));
    const auto pub = rules_of("public-off.unverified.json");
    PB_CHECK(pbc::sharing_offered(pub));
    PB_CHECK(!pbc::link_creation_offered(pub));
}

PB_TEST(a_missing_object_gives_unknown_which_is_never_required_and_never_a_limit) {
    const auto r = rules_of("missing.unverified.json");
    PB_CHECK(!r.api_enabled.has_value() && !r.public_enabled.has_value() && !r.upload_allowed.has_value());
    PB_CHECK(!r.password_enforced.has_value() && !r.expiry_enforced.has_value() && !r.expiry_days.has_value());
    PB_CHECK(pbc::sharing_offered(r));              // unknown counts as offered: the refusal explains
    PB_CHECK(pbc::link_creation_offered(r));
    for (auto p : {pbc::LinkPermission::ReadOnly, pbc::LinkPermission::ReadUpload, pbc::LinkPermission::UploadOnly, pbc::LinkPermission::Other})
        PB_CHECK(!pbc::password_required(r, p));
    PB_CHECK(!pbc::expiry_required(r));
    PB_CHECK(!pbc::expiry_max_days(r).has_value());
    PB_CHECK(pbc::upload_offered(r, true));
}

PB_TEST(mistyped_and_contradictory_values_stay_unknown) {
    const auto r = rules_of("contradictory.unverified.json");
    PB_CHECK(!r.api_enabled.has_value());           // "yes" is not a bool
    PB_CHECK(!r.public_enabled.has_value());        // 1 is not a bool
    PB_CHECK(!r.password_enforced.has_value());     // "true" is not a bool
    PB_CHECK(!r.password_enforced_read_only.has_value());   // enforced_for is not an object
    PB_CHECK(!r.upload_allowed.has_value());        // null
    PB_CHECK(!r.resharing.has_value());             // an array
    PB_CHECK(r.expiry_enforced == std::optional<bool>(true));
    PB_CHECK(!r.expiry_days.has_value());           // "7" is a string
    PB_CHECK(pbc::expiry_required(r));
    PB_CHECK(!pbc::expiry_max_days(r).has_value());
    PB_CHECK(!pbc::password_required(r, pbc::LinkPermission::ReadOnly));
}

PB_TEST(days_is_known_only_between_1_and_3650) {
    PB_CHECK(!rules_of("days-out-of-range.unverified.json").expiry_days.has_value());
    PB_CHECK(!rules_of("days-negative.unverified.json").expiry_days.has_value());
    PB_CHECK(!pbc::expiry_max_days(rules_of("days-out-of-range.unverified.json")).has_value());
    // the boundaries themselves
    for (const auto& [text, expect] : std::vector<std::pair<std::string, std::optional<int>>>{
             {"1", 1}, {"3650", 3650}, {"0", std::nullopt}, {"3651", std::nullopt}}) {
        const auto doc = pbc::parse_json("{\"files_sharing\":{\"public\":{\"expire_date\":{\"days\":" + text + ",\"enforced\":true}}}}");
        PB_CHECK(doc.ok());
        PB_CHECK(pbc::share_rules_from_capabilities(doc.value).expiry_days == expect);
    }
}

PB_TEST(only_the_first_64_members_of_an_object_are_read) {
    const auto r = rules_of("many-members.unverified.json");
    PB_CHECK(!r.public_enabled.has_value());   // "enabled" comes after the 80 filler members
    PB_CHECK(pbc::link_creation_offered(r));
}

PB_TEST(a_file_never_offers_upload) {
    const auto r = rules_of("old.unverified.json");
    PB_CHECK(!pbc::upload_offered(r, false));
}

PB_TEST(a_capabilities_value_that_is_not_an_object_gives_unknown) {
    const auto doc = pbc::parse_json("[1,2,3]");
    PB_CHECK(doc.ok());
    const auto r = pbc::share_rules_from_capabilities(doc.value);
    PB_CHECK(!r.api_enabled.has_value() && !r.public_enabled.has_value());
}

PB_TEST_MAIN()
