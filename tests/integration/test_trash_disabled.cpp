// The explanation of a server without the trash app or the versions app (specs/006-trash-and-versions, research R-2, spec Story 5, FR-027 to FR-030),
// against the in-process fake Nextcloud: one capabilities request per check, each app judged on its own, an unclear answer is Unknown with a code
// and never Off. It proves the app follows the contract, never a real server (constitution Principle IX).
#include <string>

#include "fake_nextcloud_server.h"
#include "pbc/dav_common.h"
#include "pbc/trash_api.h"
#include "test_support.h"

PBC_FAKE_SERVER_TEST();

using pbc::FeatureState;
using pbtest::FakeNextcloud;

namespace {

struct Env {
    FakeNextcloud server;
    pbc::SystemClock clock;
    pbc::DavSession session;
    pbc::TrashApi api;
    Env() : session(server, server.base_url(), "demo", "pw"), api(session, "demo", clock) { server.set_account("demo", "demo", "pw"); }
};

}  // namespace

PB_TEST(both_apps_on_after_exactly_one_capabilities_request) {
    Env e;
    const auto r = e.api.fetch_availability();
    PB_CHECK(r.ok());
    PB_CHECK(r.value.trash == FeatureState::On && r.value.versions == FeatureState::On && r.value.code.empty());
    PB_CHECK_EQ(e.server.request_count(), size_t(1));
    PB_CHECK(e.server.log()[0].path.find("/cloud/capabilities") != std::string::npos);
    PB_CHECK_EQ(e.server.count("PROPFIND"), size_t(0));   // nothing is read from the apps themselves
}

PB_TEST(a_server_without_the_trash_app_says_off_and_the_versions_app_is_judged_on_its_own) {
    Env e;
    e.server.set_trash(false);
    auto r = e.api.fetch_availability();
    PB_CHECK(r.ok() && r.value.trash == FeatureState::Off && r.value.versions == FeatureState::On);
    e.server.set_trash(std::nullopt);   // the key is absent: the app does not announce itself
    r = e.api.fetch_availability();
    PB_CHECK(r.ok() && r.value.trash == FeatureState::Off && r.value.versions == FeatureState::On);
}

PB_TEST(a_server_without_the_versions_app_says_off_and_the_trash_app_stays_on) {
    Env e;
    e.server.set_versions(false);
    auto r = e.api.fetch_availability();
    PB_CHECK(r.ok() && r.value.versions == FeatureState::Off && r.value.trash == FeatureState::On);
    e.server.set_versions(std::nullopt);
    r = e.api.fetch_availability();
    PB_CHECK(r.ok() && r.value.versions == FeatureState::Off && r.value.trash == FeatureState::On);
    e.server.set_trash(false);
    r = e.api.fetch_availability();
    PB_CHECK(r.value.versions == FeatureState::Off && r.value.trash == FeatureState::Off);
}

PB_TEST(an_answer_without_the_files_object_is_unknown_with_a_code_never_off) {
    Env e;
    e.server.set_capabilities_without_files(true);
    const auto r = e.api.fetch_availability();
    PB_CHECK(r.value.trash == FeatureState::Unknown && r.value.versions == FeatureState::Unknown);
    PB_CHECK_EQ(r.value.code, std::string("T1-200"));
    PB_CHECK(r.ok());   // the request itself worked; it is the answer that says nothing
}

PB_TEST(a_failing_capabilities_request_is_unknown_with_its_code_and_the_error_key) {
    Env e;
    e.server.set_capabilities_failing(true);
    const auto r = e.api.fetch_availability();
    PB_CHECK(r.value.trash == FeatureState::Unknown && r.value.versions == FeatureState::Unknown);
    PB_CHECK_EQ(r.value.code, std::string("T1-500"));
    PB_CHECK_EQ(r.error.key, std::string("server_error"));
    PB_CHECK_EQ(e.server.request_count(), size_t(1));   // no second try
}

PB_TEST(a_wrong_password_is_an_auth_error_not_a_claim_about_the_apps) {
    FakeNextcloud server;
    server.set_account("demo", "demo", "pw");
    pbc::SystemClock clock;
    pbc::DavSession session(server, server.base_url(), "demo", "wrong");
    pbc::TrashApi api(session, "demo", clock);
    const auto r = api.fetch_availability();
    PB_CHECK_EQ(r.error.key, std::string("auth_failed"));
    PB_CHECK(r.value.trash == FeatureState::Unknown && r.value.versions == FeatureState::Unknown);
    PB_CHECK_EQ(r.value.code, std::string("T1-401"));
}

PB_TEST(an_unreachable_server_is_a_network_error_and_unknown) {
    Env e;
    e.server.set_offline(true);
    const auto r = e.api.fetch_availability();
    PB_CHECK(!r.ok());
    PB_CHECK(r.error.cls == pbc::ErrorClass::Network);
    PB_CHECK(r.value.trash == FeatureState::Unknown && r.value.versions == FeatureState::Unknown);
}

PB_TEST_MAIN()
