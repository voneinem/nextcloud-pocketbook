// What the capabilities say about the trash app and the versions app (specs/006-trash-and-versions, research R-2, spec FR-027 to FR-030): On, Off or
// Unknown, judged separately; Unknown is never Off. Pure.
#include <string>

#include "fixtures.h"
#include "pbc/json_min.h"
#include "pbc/trash_rules.h"
#include "test_support.h"

namespace {

using pbc::FeatureState;

pbc::JsonValue caps_of_answer(const std::string& json) {
    const auto doc = pbc::parse_json(json);
    if (!doc.ok()) PB_FAIL_AT("the fixture is not JSON");
    const pbc::JsonValue* v = &doc.value;
    for (const char* key : {"ocs", "data", "capabilities"}) {
        v = v ? v->get(key) : nullptr;
        if (!v) PB_FAIL_AT(std::string("no ") + key);
    }
    return *v;
}

pbc::TrashAvailability from_fixture(const char* name) {
    return pbc::availability_from_capabilities(caps_of_answer(pbtest::fixture(std::string("nextcloud/trash/") + name)));
}

pbc::TrashAvailability from_json(const std::string& caps_object) {
    const auto doc = pbc::parse_json(caps_object);
    if (!doc.ok()) PB_FAIL_AT("not JSON: " + caps_object);
    return pbc::availability_from_capabilities(doc.value);
}

}  // namespace

PB_TEST(both_apps_on_when_both_keys_are_true) {
    const auto a = from_fixture("caps_trash_versions_on.unverified.json");
    PB_CHECK(a.trash == FeatureState::On && a.versions == FeatureState::On);
    PB_CHECK(a.code.empty());
}

PB_TEST(a_missing_key_in_a_files_object_means_off_for_that_app_only) {
    const auto t = from_fixture("caps_trash_off.unverified.json");   // versioning only
    PB_CHECK(t.trash == FeatureState::Off && t.versions == FeatureState::On);
    const auto v = from_fixture("caps_versions_off.unverified.json");   // undelete only
    PB_CHECK(v.trash == FeatureState::On && v.versions == FeatureState::Off);
    const auto none = from_json("{\"files\":{}}");
    PB_CHECK(none.trash == FeatureState::Off && none.versions == FeatureState::Off);
}

PB_TEST(a_key_that_is_false_means_off) {
    const auto a = from_json("{\"files\":{\"undelete\":false,\"versioning\":false}}");
    PB_CHECK(a.trash == FeatureState::Off && a.versions == FeatureState::Off);
    const auto b = from_json("{\"files\":{\"undelete\":true,\"versioning\":false}}");
    PB_CHECK(b.trash == FeatureState::On && b.versions == FeatureState::Off);
}

PB_TEST(no_files_object_means_unknown_for_both) {
    const auto a = from_fixture("caps_no_files.unverified.json");
    PB_CHECK(a.trash == FeatureState::Unknown && a.versions == FeatureState::Unknown);
    PB_CHECK(from_json("{}").trash == FeatureState::Unknown);
    PB_CHECK(from_json("{\"files\":null}").versions == FeatureState::Unknown);
    PB_CHECK(from_json("{\"files\":[1]}").trash == FeatureState::Unknown);
    PB_CHECK(from_json("{\"files\":\"yes\"}").trash == FeatureState::Unknown);
}

PB_TEST(anything_that_is_not_an_object_gives_unknown) {
    for (const char* text : {"null", "[]", "\"x\"", "5", "true"}) {
        const auto a = from_json(text);
        PB_CHECK(a.trash == FeatureState::Unknown && a.versions == FeatureState::Unknown);
    }
    const auto a = pbc::availability_from_capabilities(pbc::JsonValue());   // a default value is no object either
    PB_CHECK(a.trash == FeatureState::Unknown && a.versions == FeatureState::Unknown);
}

PB_TEST(a_key_that_is_not_a_boolean_is_unknown_for_that_app_only) {
    for (const char* bad : {"\"yes\"", "1", "null", "[true]", "{\"a\":1}", "\"true\""}) {
        const auto a = from_json(std::string("{\"files\":{\"undelete\":") + bad + ",\"versioning\":true}}");
        PB_CHECK(a.trash == FeatureState::Unknown);
        PB_CHECK(a.versions == FeatureState::On);   // the other app is judged on its own
        const auto b = from_json(std::string("{\"files\":{\"undelete\":true,\"versioning\":") + bad + "}}");
        PB_CHECK(b.trash == FeatureState::On && b.versions == FeatureState::Unknown);
    }
}

PB_TEST(unknown_is_never_off) {
    // the rule of spec FR-030: only a conclusive answer says Off
    for (const char* text : {"{}", "{\"files\":1}", "{\"files\":{\"undelete\":\"no\"}}"}) {
        const auto a = from_json(text);
        PB_CHECK(a.trash != FeatureState::Off || std::string(text) == "{\"files\":{}}");
    }
}

PB_TEST_MAIN()
