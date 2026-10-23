// The exact requests of the sharing screens (specs/004-share-links, contracts/share-requests.md): method, URL, headers and body of every
// request, as the fake server and the recording transport saw them. Later stories add the write requests to this file.
#include <string>

#include "fake_nextcloud_server.h"
#include "pbc/dav_common.h"
#include "pbc/share_api.h"
#include "test_support.h"

PBC_FAKE_SERVER_TEST();

using pbtest::FakeNextcloud;

namespace {

struct Rig {
    FakeNextcloud server;
    pbc::SystemClock clock;
    pbc::DavSession session;
    pbc::ShareApi api;
    Rig() : session(server, server.base_url(), server.login_name(), server.app_password()), api(session, clock, server.user_id()) {
        server.add_folder("/Docs");
        server.add_folder("/Docs/A B \xc3\xa4 100%");
    }
    const std::vector<pbc::Request>& sent() { return server.transport().requests(); }
};

bool has(const pbc::Request& r, const std::string& name, const std::string& value) { return pbc::header_value(r.headers, name) == value; }

}  // namespace

PB_TEST(l1_the_capabilities_are_a_plain_get_with_the_ocs_headers) {
    Rig r;
    PB_CHECK(r.api.fetch_rules().ok());
    PB_CHECK_EQ(r.sent().size(), size_t(1));
    const auto& q = r.sent()[0];
    PB_CHECK_EQ(q.method, std::string("GET"));
    PB_CHECK_EQ(q.url, r.server.base_url() + "/ocs/v1.php/cloud/capabilities?format=json");
    PB_CHECK(has(q, "OCS-APIRequest", "true") && has(q, "Accept", "application/json"));
    PB_CHECK(pbc::header_value(q.headers, "User-Agent").compare(0, 8, "PBCloud/") == 0);
    PB_CHECK(q.body.empty() && q.body_file.empty());
}

PB_TEST(l2_and_l3_use_the_ocs_v2_prefix_and_the_path_as_a_query_value) {
    Rig r;
    r.server.add_folder("/Docs/A B \xc3\xa4 100%");
    PB_CHECK(r.api.list({"/Docs/A B \xc3\xa4 100%", true}).ok());
    PB_CHECK_EQ(r.sent().size(), size_t(2));
    const std::string api = r.server.base_url() + "/ocs/v2.php/apps/files_sharing/api/v1";
    const std::string enc = "%2FDocs%2FA%20B%20%C3%A4%20100%25";
    PB_CHECK_EQ(r.sent()[0].url, api + "/shares?path=" + enc + "&reshares=true&format=json");
    PB_CHECK_EQ(r.sent()[1].url, api + "/shares/inherited?path=" + enc + "&format=json");
    for (const auto& q : r.sent()) {
        PB_CHECK_EQ(q.method, std::string("GET"));
        PB_CHECK(has(q, "OCS-APIRequest", "true") && has(q, "Accept", "application/json"));
        PB_CHECK(q.body.empty());
    }
}

PB_TEST(credentials_go_through_the_transport_and_never_into_a_url) {
    Rig r;
    PB_CHECK(r.api.list({"/Docs", true}).ok());
    PB_CHECK(r.api.fetch_rules().ok());
    for (const auto& q : r.sent()) {
        PB_CHECK_EQ(q.username, r.server.login_name());
        PB_CHECK_EQ(q.password, r.server.app_password());
        PB_CHECK(q.url.find(r.server.app_password()) == std::string::npos && q.url.find('@') == std::string::npos);
    }
}

PB_TEST(a_redirect_to_another_host_is_not_followed_with_credentials_and_the_call_fails) {
    Rig r;
    r.server.set_interceptor([](const pbc::Request& q) -> std::optional<pbc::Response> {
        if (q.url.find("/shares?path") == std::string::npos) return std::nullopt;
        pbc::Response moved;
        moved.status = 302;
        moved.headers.push_back({"Location", "https://elsewhere.example.net/ocs"});
        return moved;
    });
    const auto l = r.api.list({"/Docs", true});
    PB_CHECK(!l.ok());
    for (const auto& q : r.sent()) PB_CHECK(q.url.find("elsewhere.example.net") == std::string::npos);
}

PB_TEST(nothing_is_sent_by_constructing_the_api_or_asking_for_a_message) {
    Rig r;
    (void)r.api.message();
    (void)r.api.current();
    (void)r.api.outcome_unknown();
    PB_CHECK_EQ(r.sent().size(), size_t(0));
}

PB_TEST(l5_create_is_a_form_post_with_the_fields_in_a_fixed_order) {
    Rig r;
    r.server.add_folder("/Docs/A B \xc3\xa4 100%");
    const auto rules = r.api.fetch_rules();
    PB_CHECK(rules.ok());
    r.server.transport().clear_requests();
    pbc::LinkRequest req;
    req.permission = pbc::LinkPermission::ReadUpload;
    req.password = pbc::SecretText{std::string("p&w=1 %")};
    req.expiry = std::string("2026-12-31");
    PB_CHECK(r.api.create({"/Docs/A B \xc3\xa4 100%", true}, rules.value, std::move(req)).ok());
    PB_CHECK_EQ(r.sent().size(), size_t(1));
    const auto& q = r.sent()[0];
    PB_CHECK_EQ(q.method, std::string("POST"));
    PB_CHECK_EQ(q.url, r.server.base_url() + "/ocs/v2.php/apps/files_sharing/api/v1/shares?format=json");
    PB_CHECK(has(q, "Content-Type", "application/x-www-form-urlencoded") && has(q, "OCS-APIRequest", "true") && has(q, "Accept", "application/json"));
    // the path is form-encoded (not URL-path encoded); the password is encoded byte by byte; nothing else is sent
    PB_CHECK_EQ(q.body, std::string("path=%2FDocs%2FA%20B%20%C3%A4%20100%25&shareType=3&permissions=15&password=p%26w%3D1%20%25&expireDate=2026-12-31"));
    PB_CHECK(q.url.find("p%26w") == std::string::npos);
    PB_CHECK_EQ(q.username, r.server.login_name());
}

PB_TEST(l5_create_without_a_password_or_expiry_leaves_the_fields_out) {
    Rig r;
    const auto rules = r.api.fetch_rules();
    r.server.transport().clear_requests();
    PB_CHECK(r.api.create({"/Docs", true}, rules.value, pbc::LinkRequest()).ok());
    PB_CHECK_EQ(r.sent()[0].body, std::string("path=%2FDocs&shareType=3&permissions=1"));
}

PB_TEST(nothing_is_sent_before_a_create_is_asked_for_even_with_a_screen_full_of_choices) {
    Rig r;
    const auto rules = r.api.fetch_rules();
    r.server.transport().clear_requests();
    pbc::LinkRequest req;
    req.permission = pbc::LinkPermission::ReadUpload;
    req.password = pbc::SecretText{std::string("pw")};
    // building a request and validating it makes no request; only create() sends
    PB_CHECK(pbc::validate_create(rules.value, {"/Docs", true}, req, 0).empty());
    PB_CHECK_EQ(r.sent().size(), size_t(0));
}

PB_TEST(l4_before_l5_the_share_is_read_then_one_put_with_one_field_per_action) {
    Rig r;
    r.server.add_folder("/Docs/A");
    pbtest::FakeNextcloud::Share link;
    link.path = "/Docs/A";
    const int64_t id = r.server.add_share(link);
    const auto rules = r.api.fetch_rules();
    const auto shown = pbc::snapshot_of(r.api.detail(id, true).value.row);
    r.server.transport().clear_requests();
    pbc::LinkChange c;
    c.action = pbc::LinkChange::Action::SetPassword;
    c.password = pbc::SecretText{std::string("a b&c")};
    PB_CHECK(r.api.change({"/Docs/A", true}, rules.value, id, shown, std::move(c)).ok());
    PB_CHECK_EQ(r.sent().size(), size_t(2));
    const std::string api = r.server.base_url() + "/ocs/v2.php/apps/files_sharing/api/v1/shares/" + std::to_string(id) + "?format=json";
    PB_CHECK_EQ(r.sent()[0].method, std::string("GET"));
    PB_CHECK_EQ(r.sent()[0].url, api);
    PB_CHECK_EQ(r.sent()[1].method, std::string("PUT"));
    PB_CHECK_EQ(r.sent()[1].url, api);
    PB_CHECK_EQ(r.sent()[1].body, std::string("password=a%20b%26c"));
    PB_CHECK(has(r.sent()[1], "Content-Type", "application/x-www-form-urlencoded") && has(r.sent()[1], "OCS-APIRequest", "true"));
    // the two clear actions send an empty value, the others one date or one permission value
    const auto after = pbc::snapshot_of(r.api.detail(id, true).value.row);
    r.server.transport().clear_requests();
    pbc::LinkChange clear;
    clear.action = pbc::LinkChange::Action::ClearPassword;
    PB_CHECK(r.api.change({"/Docs/A", true}, rules.value, id, after, std::move(clear)).ok());
    PB_CHECK_EQ(r.sent()[1].body, std::string("password="));
}

PB_TEST(l6_remove_is_a_delete_of_the_share_with_no_body) {
    Rig r;
    r.server.add_folder("/Docs/A");
    pbtest::FakeNextcloud::Share link;
    link.path = "/Docs/A";
    const int64_t id = r.server.add_share(link);
    const auto rules = r.api.fetch_rules();
    r.server.transport().clear_requests();
    PB_CHECK(r.api.remove_link(id, {"/Docs/A", true}, rules.value).ok());
    PB_CHECK_EQ(r.sent().size(), size_t(1));
    const auto& q = r.sent()[0];
    PB_CHECK_EQ(q.method, std::string("DELETE"));
    PB_CHECK_EQ(q.url, r.server.base_url() + "/ocs/v2.php/apps/files_sharing/api/v1/shares/" + std::to_string(id) + "?format=json");
    PB_CHECK(q.body.empty() && q.body_file.empty());
    PB_CHECK(has(q, "OCS-APIRequest", "true") && has(q, "Accept", "application/json"));
}

PB_TEST_MAIN()
