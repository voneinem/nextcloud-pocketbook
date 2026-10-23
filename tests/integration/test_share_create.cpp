// ShareApi::create against the fake Nextcloud (specs/004-share-links US1, contracts/share-requests.md L5): what is sent for the defaults and for the
// full set of choices, that nothing is sent when the rules or the input forbid it, that the link shown is the one of the answer, and that the password
// is gone from the request object after every outcome.
#include <string>

#include "fake_nextcloud_server.h"
#include "pbc/dav_common.h"
#include "pbc/share_api.h"
#include "test_support.h"

PBC_FAKE_SERVER_TEST();

using pbtest::FakeNextcloud;

namespace {

struct FixedClock : pbc::Clock {
    int64_t t;
    explicit FixedClock(int64_t v) : t(v) {}
    int64_t now() const override { return t; }
};

struct Rig {
    FakeNextcloud server;
    FixedClock clock{server.server_time()};
    pbc::DavSession session;
    pbc::ShareApi api;
    Rig() : session(server, server.base_url(), server.login_name(), server.app_password()), api(session, clock, server.user_id()) {
        server.add_folder("/Docs");
        server.add_file("/Docs/a.pdf", "alpha");
        server.add_folder("/Docs/Reports");
    }
    pbc::ShareRules rules() {
        const auto r = api.fetch_rules();
        PB_CHECK(r.ok());
        server.clear_log();
        server.transport().clear_requests();
        return r.value;
    }
    std::string last_body() { return server.share_log().empty() ? std::string() : server.share_log().back().body; }
};

pbc::LinkRequest request(pbc::LinkPermission p = pbc::LinkPermission::ReadOnly, const std::string& pw = "", std::optional<std::string> expiry = std::nullopt) {
    pbc::LinkRequest r;
    r.permission = p;
    if (!pw.empty()) r.password = pbc::SecretText{std::string(pw)};
    r.expiry = std::move(expiry);
    return r;
}

const pbc::ShareTarget kFolder{"/Docs/Reports", true};
const pbc::ShareTarget kFile{"/Docs/a.pdf", false};

}  // namespace

PB_TEST(the_defaults_send_the_path_the_type_and_read_only_and_nothing_else) {
    Rig r;
    const auto rules = r.rules();
    const auto d = r.api.create(kFolder, rules, request());
    PB_CHECK(d.ok());
    PB_CHECK_EQ(r.last_body(), std::string("path=%2FDocs%2FReports&shareType=3&permissions=1"));
    PB_CHECK_EQ(r.server.count("POST"), size_t(1));
    PB_CHECK_EQ(r.server.request_count(), size_t(1));
    PB_CHECK(d.value.row.permission == pbc::LinkPermission::ReadOnly && !d.value.row.has_password && !d.value.row.expires.has_value());
    PB_CHECK(d.value.row.owned_by_me && d.value.link_state == pbc::LinkState::Ok);
    PB_CHECK_EQ(r.server.share_count(), size_t(1));
}

PB_TEST(a_folder_with_upload_password_and_expiry_sends_all_three_choices) {
    Rig r;
    const auto rules = r.rules();
    const auto d = r.api.create(kFolder, rules, request(pbc::LinkPermission::ReadUpload, "Zx9-pw", std::string("2026-12-31")));
    PB_CHECK(d.ok());
    PB_CHECK_EQ(r.last_body(), std::string("path=%2FDocs%2FReports&shareType=3&permissions=15&password=Zx9-pw&expireDate=2026-12-31"));
    PB_CHECK(d.value.row.permission == pbc::LinkPermission::ReadUpload && d.value.row.has_password);
    PB_CHECK(d.value.row.expires == std::optional<std::string>("2026-12-31"));
}

PB_TEST(a_file_never_gets_upload_and_nothing_is_sent) {
    Rig r;
    const auto rules = r.rules();
    const auto d = r.api.create(kFile, rules, request(pbc::LinkPermission::ReadUpload));
    PB_CHECK(!d.ok());
    PB_CHECK_EQ(d.error.key, std::string("permission_invalid"));
    PB_CHECK_EQ(r.server.request_count(), size_t(0));
    const auto ok = r.api.create(kFile, rules, request());
    PB_CHECK(ok.ok());
    PB_CHECK_EQ(r.last_body(), std::string("path=%2FDocs%2Fa.pdf&shareType=3&permissions=1"));
}

PB_TEST(the_link_shown_is_the_one_of_the_answer_and_not_one_the_app_derived) {
    Rig r;
    const auto rules = r.rules();
    r.server.set_interceptor([](const pbc::Request& q) -> std::optional<pbc::Response> {
        if (q.method != "POST") return std::nullopt;
        pbc::Response a;
        a.status = 200;
        a.body = "{\"ocs\":{\"meta\":{\"statuscode\":200},\"data\":{\"id\":\"77\",\"share_type\":3,\"permissions\":1,\"uid_owner\":\"demo\","
                 "\"item_type\":\"folder\",\"url\":\"https://cloud.example.org/s/ANSWERONLYtoken\",\"token\":\"x\"}}}";
        return a;
    });
    const auto d = r.api.create(kFolder, rules, request());
    PB_CHECK(d.ok());
    PB_CHECK_EQ(d.value.link, std::string("https://cloud.example.org/s/ANSWERONLYtoken"));
    PB_CHECK_EQ(d.value.row.id, int64_t(77));
}

PB_TEST(a_request_the_rules_forbid_is_refused_before_anything_is_sent) {
    struct Case { const char* what; std::function<void(FakeNextcloud&)> set; pbc::LinkRequest req; const char* key; };
    std::vector<Case> cases;
    cases.push_back({"public links off", [](FakeNextcloud& s) { s.set_public_links_enabled(false); }, request(), "share_public_disabled"});
    cases.push_back({"password required", [](FakeNextcloud& s) { s.set_password_enforced(true); }, request(), "share_password_required"});
    cases.push_back({"expiry required", [](FakeNextcloud& s) { s.set_expiry(true, true, 7); }, request(), "share_expiry_required"});
    cases.push_back({"expiry too far", [](FakeNextcloud& s) { s.set_expiry(true, true, 7); }, request(pbc::LinkPermission::ReadOnly, "", std::string("2026-12-31")), "share_expiry_too_far"});
    cases.push_back({"upload not allowed", [](FakeNextcloud& s) { s.set_public_upload(false); }, request(pbc::LinkPermission::ReadUpload), "share_upload_not_allowed"});
    cases.push_back({"sharing off", [](FakeNextcloud& s) { s.set_sharing_enabled(false); }, request(), "share_not_offered"});
    cases.push_back({"date in the past", [](FakeNextcloud&) {}, request(pbc::LinkPermission::ReadOnly, "", std::string("2026-10-19")), "expiry_in_past"});
    cases.push_back({"date malformed", [](FakeNextcloud&) {}, request(pbc::LinkPermission::ReadOnly, "", std::string("31.12.2026")), "expiry_invalid"});
    cases.push_back({"password too long", [](FakeNextcloud&) {}, request(pbc::LinkPermission::ReadOnly, std::string(257, 'a')), "password_invalid"});
    cases.push_back({"password with a control character", [](FakeNextcloud&) {}, request(pbc::LinkPermission::ReadOnly, "a\nb"), "password_invalid"});
    for (auto& c : cases) {
        Rig r;
        c.set(r.server);
        const auto rules = r.rules();
        const auto d = r.api.create(kFolder, rules, std::move(c.req));
        if (d.ok() || d.error.key != c.key) PB_FAIL_AT(std::string(c.what) + ": " + d.error.key);
        PB_CHECK_EQ(r.api.message().key, std::string(c.key));
        PB_CHECK_EQ(r.server.request_count(), size_t(0));   // not even the capabilities: the rules the screen holds were enough
        PB_CHECK_EQ(r.server.share_count(), size_t(0));
    }
}

PB_TEST(the_password_is_gone_from_the_request_after_every_outcome) {
    // success
    {
        Rig r;
        const auto rules = r.rules();
        auto req = request(pbc::LinkPermission::ReadOnly, "Zx9-pw");
        PB_CHECK(r.api.create(kFolder, rules, std::move(req)).ok());
        PB_CHECK(req.password.empty());
    }
    // an HTTP refusal
    {
        Rig r;
        const auto rules = r.rules();
        r.server.answer_next("POST", "/shares", 500);
        auto req = request(pbc::LinkPermission::ReadOnly, "Zx9-pw");
        PB_CHECK(!r.api.create(kFolder, rules, std::move(req)).ok());
        PB_CHECK(req.password.empty());
    }
    // no connection
    {
        Rig r;
        const auto rules = r.rules();
        r.server.set_offline(true);
        auto req = request(pbc::LinkPermission::ReadOnly, "Zx9-pw");
        PB_CHECK(!r.api.create(kFolder, rules, std::move(req)).ok());
        PB_CHECK(req.password.empty());
    }
    // refused before sending
    {
        Rig r;
        const auto rules = r.rules();
        auto req = request(pbc::LinkPermission::ReadUpload, "Zx9-pw");
        PB_CHECK(!r.api.create(kFile, rules, std::move(req)).ok());
        PB_CHECK(req.password.empty());
    }
}

PB_TEST(the_password_is_in_the_body_only_never_in_a_url_or_the_request_log_paths) {
    Rig r;
    const auto rules = r.rules();
    PB_CHECK(r.api.create(kFolder, rules, request(pbc::LinkPermission::ReadOnly, "Zx9-pw")).ok());
    PB_CHECK(r.last_body().find("password=Zx9-pw") != std::string::npos);
    for (const auto& q : r.server.transport().requests()) PB_CHECK(q.url.find("Zx9") == std::string::npos);
    for (const auto& l : r.server.log()) PB_CHECK(l.path.find("Zx9") == std::string::npos);
    for (const auto& l : r.server.share_log()) PB_CHECK(l.url.find("Zx9") == std::string::npos && l.query.find("Zx9") == std::string::npos);
}

PB_TEST(the_server_day_decides_whether_an_expiry_is_in_the_past) {
    // the fake's server time is 2026-10-20 12:00 UTC; a reader whose clock is two days ahead must not call the 20th "in the past"
    Rig ahead;
    ahead.clock.t = ahead.server.server_time() + 2 * 86400;
    const auto rules = ahead.rules();   // one answer with a Date header: the session now knows the server's day
    PB_CHECK(ahead.api.create(kFolder, rules, request(pbc::LinkPermission::ReadOnly, "", std::string("2026-10-20"))).ok());
    // and a reader whose clock is behind must not accept the day before the server's today
    Rig behind;
    behind.clock.t = behind.server.server_time() - 3 * 86400;
    const auto rules2 = behind.rules();
    const auto d = behind.api.create(kFolder, rules2, request(pbc::LinkPermission::ReadOnly, "", std::string("2026-10-19")));
    PB_CHECK(!d.ok());
    PB_CHECK_EQ(d.error.key, std::string("expiry_in_past"));
}

PB_TEST(a_rule_learned_from_the_refusal_is_explained_after_one_fresh_read_of_the_rules) {
    Rig r;
    r.server.set_password_enforced(true);   // the screen's rules (empty) do not know it
    const pbc::ShareRules unknown;
    const auto d = r.api.create(kFolder, unknown, request());
    PB_CHECK(!d.ok());
    PB_CHECK_EQ(d.error.key, std::string("share_password_required"));
    PB_CHECK(r.api.message().field == pbc::ShareMessage::Field::Password);
    PB_CHECK_EQ(r.server.count("POST"), size_t(1));
    PB_CHECK_EQ(r.server.request_count(), size_t(2));   // the POST and the one fresh read of the capabilities
    PB_CHECK_EQ(r.server.share_count(), size_t(0));
}

PB_TEST(a_deleted_item_is_item_gone_and_nothing_is_created_elsewhere) {
    Rig r;
    const auto rules = r.rules();
    r.server.remove("/Docs/Reports");
    const auto d = r.api.create(kFolder, rules, request());
    PB_CHECK(!d.ok());
    PB_CHECK_EQ(d.error.key, std::string("share_item_gone"));
    PB_CHECK_EQ(r.server.share_count(), size_t(0));
    PB_CHECK_EQ(r.server.count("PROPFIND"), size_t(1));
}

PB_TEST(only_public_links_are_ever_created) {
    Rig r;
    const auto rules = r.rules();
    for (auto p : {pbc::LinkPermission::ReadOnly, pbc::LinkPermission::ReadUpload}) (void)r.api.create(kFolder, rules, request(p, "pw", std::string("2026-12-31")));
    (void)r.api.create(kFile, rules, request());
    for (const auto& l : r.server.share_log()) {
        PB_CHECK(l.body.find("shareType=3") != std::string::npos);
        for (const char* other : {"shareType=0", "shareType=1", "shareType=4", "shareType=6", "shareType=7", "shareType=10", "shareWith", "publicUpload", "note=", "label=", "sendPasswordByTalk"})
            PB_CHECK(l.body.find(other) == std::string::npos);
    }
}

PB_TEST(a_lost_answer_is_an_unknown_outcome_and_nothing_is_repeated) {
    Rig r;
    const auto rules = r.rules();
    r.server.lose_next_answer("POST", "/shares");
    auto req = request(pbc::LinkPermission::ReadOnly, "Zx9-pw");
    const auto d = r.api.create(kFolder, rules, std::move(req));
    PB_CHECK(!d.ok());
    PB_CHECK_EQ(d.error.key, std::string("share_unknown_outcome"));
    PB_CHECK(r.api.outcome_unknown());
    PB_CHECK_EQ(r.server.count("POST"), size_t(1));      // not repeated
    PB_CHECK_EQ(r.server.share_count(), size_t(1));      // the server did it
    PB_CHECK(req.password.empty());
    // a request that never reached the server is a plain failure
    Rig q;
    const auto qrules = q.rules();
    q.server.drop_before_processing("POST", "/shares");
    const auto e = q.api.create(kFolder, qrules, request());
    PB_CHECK(!e.ok());
    PB_CHECK(!q.api.outcome_unknown());
    PB_CHECK_EQ(q.server.share_count(), size_t(0));
}

PB_TEST_MAIN()
