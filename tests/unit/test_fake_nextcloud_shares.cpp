// The shares side of the fake Nextcloud (specs/004-share-links, tasks T020/T021): it implements contracts/share-requests.md - list, inherited,
// one share, create, change, remove - and enforces the server's sharing rules with status codes that do not name the rule in text. Test
// infrastructure: it proves the app against this harness, never against a real server (constitution Principle IX).
#include <string>

#include "fake_nextcloud_server.h"
#include "pbc/json_min.h"
#include "pbc/share_parse.h"
#include "pbc/share_rules.h"
#include "test_support.h"

PBC_FAKE_SERVER_TEST();

using pbtest::FakeNextcloud;

namespace {

const char* kApi = "/ocs/v2.php/apps/files_sharing/api/v1";

pbc::Request ocs(const FakeNextcloud& s, const std::string& method, const std::string& route_and_query, const std::string& body = std::string(),
                 const char* prefix = nullptr) {
    pbc::Request r;
    r.method = method;
    r.url = s.base_url() + (prefix ? std::string(prefix) : std::string(kApi)) + route_and_query;
    r.username = s.login_name();
    r.password = s.app_password();
    r.headers = {{"OCS-APIRequest", "true"}, {"Accept", "application/json"}};
    if (!body.empty()) {
        r.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
        r.body = body;
    }
    return r;
}

pbc::Response send(FakeNextcloud& s, const std::string& method, const std::string& route_and_query, const std::string& body = std::string()) {
    return s.perform(ocs(s, method, route_and_query, body));
}

int statuscode(const pbc::Response& r) {
    const auto st = pbc::parse_ocs_status(r.body);
    return st.ok() ? st.value.statuscode : -1;
}

pbc::ShareRules rules_of(FakeNextcloud& s) {
    pbc::Request r = ocs(s, "GET", "", "", "/ocs/v1.php/cloud/capabilities?format=json");
    r.url = s.base_url() + "/ocs/v1.php/cloud/capabilities?format=json";
    const auto doc = pbc::parse_json(s.perform(r).body);
    if (!doc.ok()) PB_FAIL_AT("capabilities are not JSON");
    const pbc::JsonValue* v = &doc.value;
    for (const char* k : {"ocs", "data", "capabilities"}) {
        v = v->get(k);
        if (!v) PB_FAIL_AT("no capabilities");
    }
    return pbc::share_rules_from_capabilities(*v);
}

FakeNextcloud::Share link_share(const std::string& path, int perms = 1) {
    FakeNextcloud::Share sh;
    sh.type = 3;
    sh.path = path;
    sh.permissions = perms;
    return sh;
}

FakeNextcloud server() {
    FakeNextcloud s;
    s.add_folder("/Docs");
    s.add_file("/Docs/a.pdf", "alpha");
    s.add_folder("/Docs/Reports");
    return s;
}

std::string form(const std::string& fields) { return fields; }

}  // namespace

PB_TEST(the_capabilities_carry_a_files_sharing_object_built_from_the_switches) {
    FakeNextcloud s = server();
    auto r = rules_of(s);
    PB_CHECK(r.api_enabled == std::optional<bool>(true) && r.public_enabled == std::optional<bool>(true));
    PB_CHECK(!pbc::password_required(r, pbc::LinkPermission::ReadOnly) && !pbc::expiry_required(r) && pbc::upload_offered(r, true));
    s.set_password_enforced(true);
    s.set_expiry(true, true, 7);
    s.set_public_upload(false);
    r = rules_of(s);
    PB_CHECK(pbc::password_required(r, pbc::LinkPermission::ReadOnly));
    PB_CHECK(pbc::expiry_required(r) && pbc::expiry_max_days(r) == std::optional<int>(7));
    PB_CHECK(!pbc::upload_offered(r, true));
    s.set_password_enforced_for(false, true, false);
    r = rules_of(s);
    PB_CHECK(!pbc::password_required(r, pbc::LinkPermission::ReadOnly) && pbc::password_required(r, pbc::LinkPermission::ReadUpload));
    s.set_public_links_enabled(false);
    PB_CHECK(!pbc::link_creation_offered(rules_of(s)));
    s.set_sharing_enabled(false);
    PB_CHECK(!pbc::sharing_offered(rules_of(s)));
    FakeNextcloud quiet = server();
    quiet.set_sharing_rules_in_caps(false);
    PB_CHECK(!rules_of(quiet).api_enabled.has_value() && !rules_of(quiet).public_enabled.has_value());
}

PB_TEST(a_list_returns_the_shares_of_the_path_and_reshares_widen_it) {
    FakeNextcloud s = server();
    s.add_share(link_share("/Docs/Reports", 1));
    FakeNextcloud::Share user;
    user.type = 0;
    user.path = "/Docs/Reports";
    user.with = "bob";
    user.with_display = "Bob Example";
    s.add_share(user);
    FakeNextcloud::Share foreign = link_share("/Docs/Reports", 1);
    foreign.owner = "carol";
    s.add_share(foreign);
    s.add_share(link_share("/Docs/a.pdf", 1));
    const auto own = send(s, "GET", "/shares?path=%2FDocs%2FReports&format=json");
    PB_CHECK_EQ(own.status, long(200));
    auto l = pbc::parse_share_list(own.body, s.user_id(), false, true);
    PB_CHECK(l.ok());
    PB_CHECK_EQ(l.value.rows.size(), size_t(2));
    const auto all = send(s, "GET", "/shares?path=%2FDocs%2FReports&reshares=true&format=json");
    l = pbc::parse_share_list(all.body, s.user_id(), false, true);
    PB_CHECK_EQ(l.value.rows.size(), size_t(3));
    int owned = 0;
    for (const auto& r : l.value.rows) owned += r.owned_by_me ? 1 : 0;
    PB_CHECK_EQ(owned, 2);
    PB_CHECK_EQ(pbc::parse_share_list(send(s, "GET", "/shares?path=%2FDocs%2Fa.pdf&format=json").body, s.user_id(), false, false).value.rows.size(), size_t(1));
    PB_CHECK_EQ(pbc::parse_share_list(send(s, "GET", "/shares?path=%2FDocs&format=json").body, s.user_id(), false, true).value.rows.size(), size_t(0));
}

PB_TEST(a_list_of_a_path_that_does_not_exist_is_a_404) {
    FakeNextcloud s = server();
    PB_CHECK_EQ(send(s, "GET", "/shares?path=%2FDocs%2FGone&format=json").status, long(404));
    s.remove("/Docs/Reports");
    PB_CHECK_EQ(send(s, "GET", "/shares?path=%2FDocs%2FReports&format=json").status, long(404));
}

PB_TEST(inherited_shares_are_listed_or_not_offered) {
    FakeNextcloud s = server();
    FakeNextcloud::Share parent = link_share("/Docs");
    s.set_inherited_shares("/Docs/Reports", {parent});
    auto l = pbc::parse_share_list(send(s, "GET", "/shares/inherited?path=%2FDocs%2FReports&format=json").body, s.user_id(), true, true);
    PB_CHECK(l.ok());
    PB_CHECK_EQ(l.value.rows.size(), size_t(1));
    PB_CHECK_EQ(send(s, "GET", "/shares/inherited?path=%2FDocs%2Fa.pdf&format=json").status, long(200));
    s.set_inherited_status(404);
    PB_CHECK_EQ(send(s, "GET", "/shares/inherited?path=%2FDocs%2FReports&format=json").status, long(404));
    s.set_inherited_status(501);
    PB_CHECK_EQ(send(s, "GET", "/shares/inherited?path=%2FDocs%2FReports&format=json").status, long(501));
}

PB_TEST(one_share_carries_its_link_and_the_link_can_be_set_by_the_test) {
    FakeNextcloud s = server();
    const int64_t id = s.add_share(link_share("/Docs/Reports", 1));
    auto d = pbc::parse_share_detail(send(s, "GET", "/shares/" + std::to_string(id) + "?format=json").body, s.user_id(), "cloud.example.org", true);
    PB_CHECK(d.ok());
    PB_CHECK(d.value.link_state == pbc::LinkState::Ok);
    PB_CHECK(d.value.link.compare(0, 31, "https://cloud.example.org/s/TOK") == 0);
    FakeNextcloud::Share other = link_share("/Docs/Reports", 1);
    other.url_override = "https://public.example.net/s/xyz";
    const int64_t id2 = s.add_share(other);
    d = pbc::parse_share_detail(send(s, "GET", "/shares/" + std::to_string(id2) + "?format=json").body, s.user_id(), "cloud.example.org", true);
    PB_CHECK(d.value.link_state == pbc::LinkState::DifferentAddress && d.value.link == "https://public.example.net/s/xyz");
    PB_CHECK_EQ(send(s, "GET", "/shares/99999?format=json").status, long(404));
}

PB_TEST(a_link_is_created_from_the_form_and_the_request_is_logged) {
    FakeNextcloud s = server();
    const auto r = send(s, "POST", "/shares?format=json", form("path=%2FDocs%2FReports&shareType=3&permissions=15&password=Zx9-pw&expireDate=2026-12-31"));
    PB_CHECK_EQ(r.status, long(200));
    PB_CHECK_EQ(statuscode(r), 200);
    PB_CHECK_EQ(s.share_count(), size_t(1));
    const auto d = pbc::parse_share_detail(r.body, s.user_id(), "cloud.example.org", true);
    PB_CHECK(d.ok());
    PB_CHECK(d.value.row.permission == pbc::LinkPermission::ReadUpload && d.value.row.has_password && d.value.row.owned_by_me);
    PB_CHECK(d.value.row.expires == std::optional<std::string>("2026-12-31"));
    PB_CHECK(!d.value.link.empty());
    const auto& log = s.share_log();
    PB_CHECK_EQ(log.size(), size_t(1));
    PB_CHECK_EQ(log[0].method, std::string("POST"));
    PB_CHECK_EQ(log[0].route, std::string("/shares"));
    PB_CHECK_EQ(log[0].content_type, std::string("application/x-www-form-urlencoded"));
    PB_CHECK(log[0].body.find("password=Zx9-pw") != std::string::npos);
    PB_CHECK(log[0].url.find("Zx9") == std::string::npos);
    PB_CHECK(log[0].ocs_header == "true" && log[0].accept == "application/json");
    // the defaults
    const auto plain = send(s, "POST", "/shares?format=json", form("path=%2FDocs%2Fa.pdf&shareType=3&permissions=1"));
    const auto p = pbc::parse_share_detail(plain.body, s.user_id(), "cloud.example.org", false);
    PB_CHECK(p.ok() && !p.value.row.has_password && !p.value.row.expires.has_value());
    PB_CHECK(p.value.row.id != d.value.row.id);
}

PB_TEST(the_rules_are_enforced_with_codes_that_do_not_name_the_rule_in_text) {
    FakeNextcloud s = server();
    const std::string base = "path=%2FDocs%2FReports&shareType=3&permissions=1";
    s.set_public_links_enabled(false);
    PB_CHECK_EQ(send(s, "POST", "/shares?format=json", base).status, long(403));
    s.set_public_links_enabled(true);
    s.set_password_enforced(true);
    const auto no_pw = send(s, "POST", "/shares?format=json", base);
    PB_CHECK_EQ(no_pw.status, long(400));
    PB_CHECK_EQ(send(s, "POST", "/shares?format=json", base + "&password=x").status, long(200));
    s.set_password_enforced(false);
    s.set_expiry(true, true, 7);
    PB_CHECK_EQ(send(s, "POST", "/shares?format=json", base).status, long(400));                       // required
    PB_CHECK_EQ(send(s, "POST", "/shares?format=json", base + "&expireDate=2026-10-27").status, long(200));   // server day 2026-10-20 + 7
}

PB_TEST(expiry_limits_use_the_servers_day) {
    FakeNextcloud s = server();   // server time: 2026-10-20 12:00 UTC
    s.set_expiry(true, true, 7);
    const std::string base = "path=%2FDocs%2FReports&shareType=3&permissions=1&expireDate=";
    PB_CHECK_EQ(send(s, "POST", "/shares?format=json", base + "2026-10-27").status, long(200));    // today + 7
    PB_CHECK_EQ(send(s, "POST", "/shares?format=json", base + "2026-10-28").status, long(400));    // today + 8
    PB_CHECK_EQ(send(s, "POST", "/shares?format=json", base + "2026-10-19").status, long(400));    // in the past
    PB_CHECK_EQ(send(s, "POST", "/shares?format=json", base + "tomorrow").status, long(400));
    s.set_expiry(true, false, 7);   // an optional expiry has no maximum
    PB_CHECK_EQ(send(s, "POST", "/shares?format=json", base + "2030-01-01").status, long(200));
}

PB_TEST(upload_the_item_and_the_share_type_are_checked) {
    FakeNextcloud s = server();
    s.set_public_upload(false);
    PB_CHECK_EQ(send(s, "POST", "/shares?format=json", "path=%2FDocs%2FReports&shareType=3&permissions=15").status, long(403));
    PB_CHECK_EQ(send(s, "POST", "/shares?format=json", "path=%2FDocs%2FReports&shareType=3&permissions=1").status, long(200));
    s.set_public_upload(true);
    s.set_unshareable("/Docs/Reports");
    PB_CHECK_EQ(send(s, "POST", "/shares?format=json", "path=%2FDocs%2FReports&shareType=3&permissions=1").status, long(403));
    PB_CHECK_EQ(send(s, "POST", "/shares?format=json", "path=%2FDocs%2FGone&shareType=3&permissions=1").status, long(404));
    PB_CHECK_EQ(send(s, "POST", "/shares?format=json", "path=%2FDocs%2Fa.pdf&shareType=0&shareWith=bob&permissions=1").status, long(400));
    s.set_sharing_enabled(false);
    PB_CHECK_EQ(send(s, "POST", "/shares?format=json", "path=%2FDocs%2Fa.pdf&shareType=3&permissions=1").status, long(404));
    PB_CHECK_EQ(send(s, "GET", "/shares?path=%2FDocs%2Fa.pdf&format=json").status, long(404));
}

PB_TEST(the_wording_of_the_message_changes_but_the_status_does_not) {
    FakeNextcloud s = server();
    s.set_password_enforced(true);
    const auto a = send(s, "POST", "/shares?format=json", "path=%2FDocs%2FReports&shareType=3&permissions=1");
    s.set_share_wording(1);
    const auto b = send(s, "POST", "/shares?format=json", "path=%2FDocs%2FReports&shareType=3&permissions=1");
    PB_CHECK_EQ(a.status, b.status);
    PB_CHECK(a.body != b.body);
}

PB_TEST(a_change_applies_one_field_and_checks_owner_and_rules) {
    FakeNextcloud s = server();
    const int64_t id = s.add_share(link_share("/Docs/Reports", 1));
    const std::string route = "/shares/" + std::to_string(id) + "?format=json";
    auto r = send(s, "PUT", route, "password=new-pw");
    PB_CHECK_EQ(r.status, long(200));
    PB_CHECK(s.find_share(id)->password == "new-pw");
    PB_CHECK(pbc::parse_share_detail(r.body, s.user_id(), "cloud.example.org", true).value.row.has_password);
    PB_CHECK_EQ(send(s, "PUT", route, "expireDate=2026-12-31").status, long(200));
    PB_CHECK(s.find_share(id)->expiration == "2026-12-31" && s.find_share(id)->password == "new-pw");
    PB_CHECK_EQ(send(s, "PUT", route, "permissions=15").status, long(200));
    PB_CHECK_EQ(s.find_share(id)->permissions, 15);
    PB_CHECK_EQ(send(s, "PUT", route, "password=").status, long(200));
    PB_CHECK(s.find_share(id)->password.empty());
    PB_CHECK_EQ(send(s, "PUT", route, "expireDate=").status, long(200));
    PB_CHECK(s.find_share(id)->expiration.empty());
    s.set_password_enforced(true);
    PB_CHECK_EQ(send(s, "PUT", route, "password=").status, long(400));
    s.set_password_enforced(false);
    s.set_public_upload(false);
    PB_CHECK_EQ(send(s, "PUT", route, "permissions=15").status, long(403));
    FakeNextcloud::Share foreign = link_share("/Docs/Reports", 1);
    foreign.owner = "carol";
    const int64_t fid = s.add_share(foreign);
    PB_CHECK_EQ(send(s, "PUT", "/shares/" + std::to_string(fid) + "?format=json", "password=x").status, long(403));
    PB_CHECK_EQ(send(s, "PUT", "/shares/424242?format=json", "password=x").status, long(404));
}

PB_TEST(a_remove_deletes_only_the_share) {
    FakeNextcloud s = server();
    const int64_t id = s.add_share(link_share("/Docs/Reports", 1));
    PB_CHECK_EQ(send(s, "DELETE", "/shares/" + std::to_string(id) + "?format=json").status, long(200));
    PB_CHECK_EQ(s.share_count(), size_t(0));
    PB_CHECK(s.nodes().count("/Docs/Reports") == 1);
    PB_CHECK_EQ(send(s, "DELETE", "/shares/" + std::to_string(id) + "?format=json").status, long(404));
    FakeNextcloud::Share foreign = link_share("/Docs/Reports", 1);
    foreign.owner = "carol";
    const int64_t fid = s.add_share(foreign);
    PB_CHECK_EQ(send(s, "DELETE", "/shares/" + std::to_string(fid) + "?format=json").status, long(403));
    PB_CHECK_EQ(s.share_count(), size_t(1));
}

PB_TEST(the_v1_prefix_answers_200_with_the_failure_in_the_envelope_and_v2_mirrors_it) {
    FakeNextcloud s = server();
    s.set_password_enforced(true);
    const auto v1 = s.perform(ocs(s, "POST", "/shares?format=json", "path=%2FDocs%2FReports&shareType=3&permissions=1", "/ocs/v1.php/apps/files_sharing/api/v1"));
    PB_CHECK_EQ(v1.status, long(200));
    PB_CHECK_EQ(statuscode(v1), 400);
    const auto v2 = send(s, "POST", "/shares?format=json", "path=%2FDocs%2FReports&shareType=3&permissions=1");
    PB_CHECK_EQ(v2.status, long(400));
    PB_CHECK_EQ(statuscode(v2), 400);
}

PB_TEST(a_request_without_the_ocs_header_or_with_wrong_credentials_is_refused) {
    FakeNextcloud s = server();
    pbc::Request r = ocs(s, "GET", "/shares?path=%2FDocs&format=json");
    r.headers.clear();
    PB_CHECK_EQ(s.perform(r).status, long(401));
    pbc::Request bad = ocs(s, "GET", "/shares?path=%2FDocs&format=json");
    bad.password = "wrong";
    PB_CHECK_EQ(s.perform(bad).status, long(401));
}

PB_TEST(six_hundred_generated_shares_and_hostile_links) {
    FakeNextcloud s = server();
    s.add_generated_shares("/Docs/Reports", 600);
    PB_CHECK_EQ(s.share_count(), size_t(600));
    const auto l = pbc::parse_share_list(send(s, "GET", "/shares?path=%2FDocs%2FReports&format=json").body, s.user_id(), false, true);
    PB_CHECK(l.ok());
    PB_CHECK_EQ(l.value.rows.size(), size_t(500));
    PB_CHECK(l.value.truncated);
    FakeNextcloud h = server();
    FakeNextcloud::Share bad = link_share("/Docs/Reports");
    bad.url_override = "https://cloud.example.org/s/a\x01" "b";
    const int64_t id = h.add_share(bad);
    PB_CHECK(pbc::parse_share_detail(send(h, "GET", "/shares/" + std::to_string(id) + "?format=json").body, h.user_id(), "cloud.example.org", true)
                 .value.link_state == pbc::LinkState::Unreadable);
}

PB_TEST(a_lost_answer_still_creates_the_share_and_a_dropped_request_does_not) {
    FakeNextcloud s = server();
    s.lose_next_answer("POST", "/shares");
    const auto lost = send(s, "POST", "/shares?format=json", "path=%2FDocs%2FReports&shareType=3&permissions=1");
    PB_CHECK_EQ(lost.status, long(0));
    PB_CHECK(lost.delivery == pbc::Delivery::FullySent);
    PB_CHECK_EQ(s.share_count(), size_t(1));
    s.drop_before_processing("POST", "/shares");
    const auto dropped = send(s, "POST", "/shares?format=json", "path=%2FDocs%2FReports&shareType=3&permissions=1");
    PB_CHECK_EQ(dropped.status, long(0));
    PB_CHECK(dropped.delivery == pbc::Delivery::NotSent);
    PB_CHECK_EQ(s.share_count(), size_t(1));
}

PB_TEST(requests_to_a_link_address_are_counted) {
    FakeNextcloud s = server();
    const int64_t id = s.add_share(link_share("/Docs/Reports"));
    const auto d = pbc::parse_share_detail(send(s, "GET", "/shares/" + std::to_string(id) + "?format=json").body, s.user_id(), "cloud.example.org", true);
    PB_CHECK_EQ(s.link_hits(), size_t(0));
    pbc::Request visit;
    visit.method = "GET";
    visit.url = d.value.link;
    s.perform(visit);
    PB_CHECK_EQ(s.link_hits(), size_t(1));
    FakeNextcloud::Share other = link_share("/Docs/Reports");
    other.url_override = "https://public.example.net/s/xyz";
    s.add_share(other);
    pbc::Request away;
    away.method = "GET";
    away.url = "https://public.example.net/s/xyz";
    s.perform(away);
    PB_CHECK_EQ(s.link_hits(), size_t(2));
}

PB_TEST(the_password_state_appears_in_the_configured_member) {
    FakeNextcloud s = server();
    FakeNextcloud::Share sh = link_share("/Docs/Reports");
    sh.password = "pw";
    sh.password_style = 1;   // only "password"
    const int64_t a = s.add_share(sh);
    sh.password_style = 2;   // only "share_with"
    const int64_t b = s.add_share(sh);
    for (int64_t id : {a, b}) {
        const auto d = pbc::parse_share_detail(send(s, "GET", "/shares/" + std::to_string(id) + "?format=json").body, s.user_id(), "cloud.example.org", true);
        PB_CHECK(d.ok() && d.value.row.has_password);
    }
}

PB_TEST_MAIN()
