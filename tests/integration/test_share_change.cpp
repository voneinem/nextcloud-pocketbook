// ShareApi::change against the fake Nextcloud (specs/004-share-links US4, contracts/share-requests.md L4 + L5 PUT): the share is read again and compared with
// what the screen showed before one PUT with exactly one field is sent; a link that changed meanwhile is never overwritten silently.
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
        server.add_folder("/Docs/Reports");
        server.add_file("/Docs/a.pdf", "alpha");
    }
    int64_t add(int perms = 1, const std::string& password = std::string(), const std::string& expiry = std::string(), const std::string& owner = std::string(),
                const std::string& path = "/Docs/Reports") {
        FakeNextcloud::Share s;
        s.path = path;
        s.permissions = perms;
        s.password = password;
        s.expiration = expiry;
        s.owner = owner;
        return server.add_share(s);
    }
    pbc::ShareRules rules() {
        const auto r = api.fetch_rules();
        PB_CHECK(r.ok());
        server.clear_log();
        server.transport().clear_requests();
        return r.value;
    }
    pbc::Snapshot shown(int64_t id) {
        const auto d = api.detail(id, true);
        PB_CHECK(d.ok());
        server.clear_log();
        server.transport().clear_requests();
        return pbc::snapshot_of(d.value.row);
    }
    std::string last_body() { return server.share_log().empty() ? std::string() : server.share_log().back().body; }
};

pbc::LinkChange set_password(const std::string& pw) {
    pbc::LinkChange c;
    c.action = pbc::LinkChange::Action::SetPassword;
    c.password = pbc::SecretText{std::string(pw)};
    return c;
}
pbc::LinkChange action(pbc::LinkChange::Action a) {
    pbc::LinkChange c;
    c.action = a;
    return c;
}
pbc::LinkChange set_expiry(const std::string& d) {
    pbc::LinkChange c;
    c.action = pbc::LinkChange::Action::SetExpiry;
    c.expiry = d;
    return c;
}
pbc::LinkChange set_permission(pbc::LinkPermission p) {
    pbc::LinkChange c;
    c.action = pbc::LinkChange::Action::SetPermission;
    c.permission = p;
    return c;
}

const pbc::ShareTarget kFolder{"/Docs/Reports", true};

}  // namespace

PB_TEST(each_action_reads_the_share_first_and_sends_one_put_with_one_field) {
    struct Case { const char* what; int perms; std::string pw, expiry; pbc::LinkChange change; std::string body; };
    std::vector<Case> cases;
    cases.push_back({"set a password", 1, "", "", set_password("Zx9-pw"), "password=Zx9-pw"});
    cases.push_back({"replace a password", 1, "old", "", set_password("new-pw"), "password=new-pw"});
    cases.push_back({"remove a password", 1, "old", "", action(pbc::LinkChange::Action::ClearPassword), "password="});
    cases.push_back({"set an expiry", 1, "", "", set_expiry("2026-12-31"), "expireDate=2026-12-31"});
    cases.push_back({"remove an expiry", 1, "", "2026-12-31", action(pbc::LinkChange::Action::ClearExpiry), "expireDate="});
    cases.push_back({"read and upload", 1, "", "", set_permission(pbc::LinkPermission::ReadUpload), "permissions=15"});
    cases.push_back({"back to read only", 15, "", "", set_permission(pbc::LinkPermission::ReadOnly), "permissions=1"});
    for (auto& c : cases) {
        Rig r;
        const int64_t id = r.add(c.perms, c.pw, c.expiry);
        const auto rules = r.rules();
        const auto shown = r.shown(id);
        const auto d = r.api.change(kFolder, rules, id, shown, std::move(c.change));
        if (!d.ok()) PB_FAIL_AT(std::string(c.what) + ": " + d.error.key);
        PB_CHECK_EQ(r.server.request_count(), size_t(2));
        const auto& log = r.server.log();
        PB_CHECK(log[0].method == "GET" && log[0].path.find("/shares/" + std::to_string(id)) != std::string::npos);
        PB_CHECK_EQ(r.server.share_log().back().method, std::string("PUT"));
        PB_CHECK_EQ(r.server.share_log().back().route, "/shares/" + std::to_string(id));
        PB_CHECK_EQ(r.server.share_log().back().query, std::string("format=json"));
        PB_CHECK_EQ(r.last_body(), c.body);
        PB_CHECK_EQ(r.server.share_log().back().content_type, std::string("application/x-www-form-urlencoded"));
    }
}

PB_TEST(the_answer_of_the_server_is_the_state_shown_and_the_link_is_unchanged) {
    Rig r;
    const int64_t id = r.add();
    const auto before = r.api.detail(id, true).value.link;
    const auto rules = r.rules();
    const auto shown = r.shown(id);
    const auto d = r.api.change(kFolder, rules, id, shown, set_password("Zx9-pw"));
    PB_CHECK(d.ok());
    PB_CHECK(d.value.row.has_password && d.value.row.id == id);
    PB_CHECK_EQ(d.value.link, before);
    const auto e = r.api.change(kFolder, rules, id, pbc::snapshot_of(d.value.row), set_expiry("2026-12-31"));
    PB_CHECK(e.ok() && e.value.row.expires == std::optional<std::string>("2026-12-31") && e.value.row.has_password);
    const auto f = r.api.change(kFolder, rules, id, pbc::snapshot_of(e.value.row), set_permission(pbc::LinkPermission::ReadUpload));
    PB_CHECK(f.ok() && f.value.row.permission == pbc::LinkPermission::ReadUpload);
}

PB_TEST(a_link_changed_meanwhile_sends_no_put_and_hands_back_the_current_state) {
    Rig q;
    const int64_t qid = q.add();
    const auto qrules = q.rules();
    const auto qshown = q.shown(qid);
    q.server.set_share_password(qid, "set-by-someone-else");   // someone else changes the link after the screen was opened
    const auto d = q.api.change(kFolder, qrules, qid, qshown, set_expiry("2026-12-31"));
    PB_CHECK(!d.ok());
    PB_CHECK_EQ(d.error.key, std::string("share_changed_meanwhile"));
    PB_CHECK_EQ(q.api.message().key, std::string("share_changed_meanwhile"));
    PB_CHECK(q.api.current().has_password && q.api.current().id == qid);   // the current state, for the screen to show
    PB_CHECK_EQ(q.server.count("PUT"), size_t(0));
    PB_CHECK(q.server.find_share(qid)->expiration.empty());
}

PB_TEST(a_permission_expiry_or_kind_that_differs_is_a_change_meanwhile_too) {
    for (int what = 0; what < 2; ++what) {
        Rig r;
        const int64_t id = r.add();
        const auto rules = r.rules();
        const auto shown = r.shown(id);
        if (what == 0) r.server.set_share_permissions(id, 15);
        else r.server.set_share_expiration(id, "2026-11-30");
        const auto d = r.api.change(kFolder, rules, id, shown, set_password("pw"));
        PB_CHECK_EQ(d.error.key, std::string("share_changed_meanwhile"));
        PB_CHECK_EQ(r.server.count("PUT"), size_t(0));
    }
}

PB_TEST(a_request_the_rules_forbid_is_refused_without_a_put) {
    struct Case { const char* what; std::function<void(FakeNextcloud&)> set; int perms; std::string pw; std::string expiry; pbc::LinkChange change; const char* key; };
    std::vector<Case> cases;
    cases.push_back({"remove a required password", [](FakeNextcloud& s) { s.set_password_enforced(true); }, 1, "old", "", action(pbc::LinkChange::Action::ClearPassword), "share_password_required"});
    cases.push_back({"remove a required expiry", [](FakeNextcloud& s) { s.set_expiry(true, true, 30); }, 1, "", "2026-11-01", action(pbc::LinkChange::Action::ClearExpiry), "share_expiry_required"});
    cases.push_back({"a date beyond the maximum", [](FakeNextcloud& s) { s.set_expiry(true, true, 7); }, 1, "", "", set_expiry("2026-12-31"), "share_expiry_too_far"});
    cases.push_back({"upload not allowed", [](FakeNextcloud& s) { s.set_public_upload(false); }, 1, "", "", set_permission(pbc::LinkPermission::ReadUpload), "share_upload_not_allowed"});
    cases.push_back({"an empty password", [](FakeNextcloud&) {}, 1, "", "", set_password(""), "password_invalid"});
    cases.push_back({"a date in the past", [](FakeNextcloud&) {}, 1, "", "", set_expiry("2026-10-19"), "expiry_in_past"});
    cases.push_back({"a malformed date", [](FakeNextcloud&) {}, 1, "", "", set_expiry("soon"), "expiry_invalid"});
    for (auto& c : cases) {
        Rig r;
        c.set(r.server);
        const int64_t id = r.add(c.perms, c.pw, c.expiry);
        const auto rules = r.rules();
        const auto shown = r.shown(id);
        const auto d = r.api.change(kFolder, rules, id, shown, std::move(c.change));
        if (d.ok() || d.error.key != c.key) PB_FAIL_AT(std::string(c.what) + ": " + d.error.key);
        PB_CHECK_EQ(r.server.count("PUT"), size_t(0));
    }
}

PB_TEST(a_share_of_someone_else_is_not_changeable_and_nothing_at_all_is_sent) {
    Rig r;
    const int64_t id = r.add(1, "", "", "carol");
    const auto rules = r.rules();
    const auto shown = r.shown(id);
    r.server.clear_log();
    r.server.transport().clear_requests();
    const auto d = r.api.change(kFolder, rules, id, shown, set_password("pw"));
    PB_CHECK_EQ(d.error.key, std::string("share_not_changeable"));
    PB_CHECK_EQ(r.server.request_count(), size_t(0));
    // a share with a user is not a link
    FakeNextcloud::Share user;
    user.type = 0;
    user.path = "/Docs/Reports";
    user.with = "bob";
    const int64_t uid = r.server.add_share(user);
    const auto ushown = r.shown(uid);
    PB_CHECK_EQ(r.api.change(kFolder, rules, uid, ushown, set_password("pw")).error.key, std::string("share_not_changeable"));
    PB_CHECK_EQ(r.server.count("PUT"), size_t(0));
}

PB_TEST(the_password_is_wiped_after_every_outcome_and_never_in_a_url) {
    // success
    {
        Rig r;
        const int64_t id = r.add();
        const auto rules = r.rules();
        const auto shown = r.shown(id);
        auto c = set_password("Zx9-pw");
        PB_CHECK(r.api.change(kFolder, rules, id, shown, std::move(c)).ok());
        PB_CHECK(c.password.empty());
        for (const auto& q : r.server.transport().requests()) PB_CHECK(q.url.find("Zx9") == std::string::npos);
    }
    // changed meanwhile
    {
        Rig r;
        const int64_t id = r.add();
        const auto rules = r.rules();
        const auto shown = r.shown(id);
        r.server.set_share_permissions(id, 15);
        auto c = set_password("Zx9-pw");
        PB_CHECK(!r.api.change(kFolder, rules, id, shown, std::move(c)).ok());
        PB_CHECK(c.password.empty());
    }
    // HTTP refusal and dropped connection
    {
        Rig r;
        const int64_t id = r.add();
        const auto rules = r.rules();
        const auto shown = r.shown(id);
        r.server.answer_next("PUT", "/shares", 500);
        auto c = set_password("Zx9-pw");
        PB_CHECK(!r.api.change(kFolder, rules, id, shown, std::move(c)).ok());
        PB_CHECK(c.password.empty());
        r.server.set_offline(true);
        auto d = set_password("Zx9-pw");
        PB_CHECK(!r.api.change(kFolder, rules, id, shown, std::move(d)).ok());
        PB_CHECK(d.password.empty());
    }
}

PB_TEST(a_lost_answer_is_an_unknown_outcome_and_the_put_is_not_repeated) {
    Rig r;
    const int64_t id = r.add();
    const auto rules = r.rules();
    const auto shown = r.shown(id);
    r.server.lose_next_answer("PUT", "/shares");
    const auto d = r.api.change(kFolder, rules, id, shown, set_password("pw"));
    PB_CHECK_EQ(d.error.key, std::string("share_unknown_outcome"));
    PB_CHECK(r.api.outcome_unknown());
    PB_CHECK_EQ(r.server.count("PUT"), size_t(1));
    PB_CHECK(r.server.find_share(id)->password == "pw");   // the server did it
}

PB_TEST(a_share_removed_meanwhile_is_share_gone_without_a_put) {
    Rig r;
    const int64_t id = r.add();
    const auto rules = r.rules();
    const auto shown = r.shown(id);
    r.server.remove_share(id);
    const auto d = r.api.change(kFolder, rules, id, shown, set_password("pw"));
    PB_CHECK_EQ(d.error.key, std::string("share_gone"));
    PB_CHECK_EQ(r.server.count("PUT"), size_t(0));
}

PB_TEST(a_refusal_after_the_put_is_explained_from_fresh_rules) {
    Rig r;
    const int64_t id = r.add(1, "old");
    const pbc::ShareRules unknown;   // the screen's rules do not know that a password is enforced
    const auto shown = r.shown(id);
    r.server.set_password_enforced(true);
    const auto d = r.api.change(kFolder, unknown, id, shown, action(pbc::LinkChange::Action::ClearPassword));
    PB_CHECK(!d.ok());
    PB_CHECK_EQ(d.error.key, std::string("share_password_required"));
    PB_CHECK(r.api.message().field == pbc::ShareMessage::Field::Password);
}

PB_TEST_MAIN()
