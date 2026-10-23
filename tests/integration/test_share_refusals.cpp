// The messages when the server limits sharing (specs/004-share-links US6, research R-4): every rule and every refusal has its own key, chosen from the
// rules and the status and never from the server's text; forced fields are marked; the entries are not lost.
#include <string>

#include "fake_nextcloud_server.h"
#include "pbc/dav_common.h"
#include "pbc/log.h"
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
};

pbc::LinkRequest request(pbc::LinkPermission p = pbc::LinkPermission::ReadOnly, const std::string& pw = "", std::optional<std::string> expiry = std::nullopt) {
    pbc::LinkRequest r;
    r.permission = p;
    if (!pw.empty()) r.password = pbc::SecretText{std::string(pw)};
    r.expiry = std::move(expiry);
    return r;
}

const pbc::ShareTarget kFolder{"/Docs/Reports", true};
const pbc::ShareRules kUnknown;   // the screen did not know the rule: only the server's refusal and a fresh read can explain it

}  // namespace

PB_TEST(each_rule_the_screen_did_not_know_is_explained_after_one_fresh_read) {
    struct Case { const char* what; std::function<void(FakeNextcloud&)> set; pbc::LinkRequest req; const char* key; };
    std::vector<Case> cases;
    cases.push_back({"public links off", [](FakeNextcloud& s) { s.set_public_links_enabled(false); }, request(), "share_public_disabled"});
    cases.push_back({"password forced", [](FakeNextcloud& s) { s.set_password_enforced(true); }, request(), "share_password_required"});
    cases.push_back({"expiry forced", [](FakeNextcloud& s) { s.set_expiry(true, true, 7); }, request(), "share_expiry_required"});
    cases.push_back({"expiry too far", [](FakeNextcloud& s) { s.set_expiry(true, true, 7); }, request(pbc::LinkPermission::ReadOnly, "", std::string("2026-12-31")), "share_expiry_too_far"});
    cases.push_back({"upload not allowed", [](FakeNextcloud& s) { s.set_public_upload(false); }, request(pbc::LinkPermission::ReadUpload), "share_upload_not_allowed"});
    cases.push_back({"sharing off", [](FakeNextcloud& s) { s.set_sharing_enabled(false); }, request(), "share_not_offered"});
    for (auto& c : cases) {
        Rig r;
        c.set(r.server);
        const auto d = r.api.create(kFolder, kUnknown, std::move(c.req));
        if (d.ok() || d.error.key != c.key) PB_FAIL_AT(std::string(c.what) + ": " + d.error.key);
        PB_CHECK_EQ(r.api.message().key, std::string(c.key));
        PB_CHECK_EQ(r.server.share_count(), size_t(0));
        PB_CHECK_EQ(r.server.count("POST"), size_t(1));
        PB_CHECK(r.server.request_count() <= size_t(3));   // the POST, the one fresh read of the rules, and for a 403/404 the one existence check
    }
}

PB_TEST(a_forced_expiry_names_the_maximum_and_marks_the_field) {
    Rig r;
    r.server.set_expiry(true, true, 7);
    const auto d = r.api.create(kFolder, kUnknown, request());
    PB_CHECK_EQ(d.error.key, std::string("share_expiry_required"));
    PB_CHECK(r.api.message().field == pbc::ShareMessage::Field::Expiry);
    PB_CHECK(r.api.message().limit_days == std::optional<int>(7));
    const auto far = r.api.create(kFolder, kUnknown, request(pbc::LinkPermission::ReadOnly, "", std::string("2026-12-31")));
    PB_CHECK_EQ(far.error.key, std::string("share_expiry_too_far"));
    PB_CHECK(r.api.message().limit_days == std::optional<int>(7));
}

PB_TEST(a_forced_password_and_a_forced_expiry_together_mark_both_fields_and_name_the_password_first) {
    Rig r;
    r.server.set_password_enforced(true);
    r.server.set_expiry(true, true, 14);
    const auto d = r.api.create(kFolder, kUnknown, request());
    PB_CHECK_EQ(d.error.key, std::string("share_password_required"));
    PB_CHECK_EQ(r.api.message().field_mask, unsigned(pbc::ShareMessage::kPassword | pbc::ShareMessage::kExpiry));
    PB_CHECK(r.api.message().limit_days == std::optional<int>(14));
    // the same through the rules the screen holds: nothing is sent at all
    const auto rules = r.api.fetch_rules().value;
    r.server.clear_log();
    const auto e = r.api.create(kFolder, rules, request());
    PB_CHECK_EQ(e.error.key, std::string("share_password_required"));
    PB_CHECK_EQ(r.api.message().field_mask, unsigned(pbc::ShareMessage::kPassword | pbc::ShareMessage::kExpiry));
    PB_CHECK_EQ(r.server.request_count(), size_t(0));
}

PB_TEST(an_item_that_cannot_be_shared_and_an_item_that_is_gone_are_told_apart) {
    Rig r;
    r.server.set_unshareable("/Docs/Reports");
    const auto a = r.api.create(kFolder, kUnknown, request());
    PB_CHECK_EQ(a.error.key, std::string("share_item_not_shareable"));
    Rig q;
    q.server.remove("/Docs/Reports");
    const auto b = q.api.create(kFolder, kUnknown, request());
    PB_CHECK_EQ(b.error.key, std::string("share_item_gone"));
    PB_CHECK_EQ(q.server.share_count(), size_t(0));
}

PB_TEST(a_refusal_nothing_can_explain_has_a_short_code_and_no_server_text) {
    Rig r;
    r.server.answer_next("POST", "/shares", 500);
    const auto d = r.api.create(kFolder, kUnknown, request());
    PB_CHECK_EQ(d.error.key, std::string("share_refused"));
    PB_CHECK_EQ(r.api.message().code, std::string("E500"));
    Rig q;
    q.server.set_interceptor([](const pbc::Request& x) -> std::optional<pbc::Response> {
        if (x.method != "POST") return std::nullopt;
        pbc::Response a;
        a.status = 400;
        a.body = "{\"ocs\":{\"meta\":{\"statuscode\":400,\"message\":\"Secret server text https://cloud.example.org/s/LEAK\"},\"data\":[]}}";
        return a;
    });
    const auto e = q.api.create(kFolder, kUnknown, request());
    PB_CHECK_EQ(e.error.key, std::string("share_refused"));
    const auto& m = q.api.message();
    for (const std::string* text : {&m.key, &m.code, &e.error.detail}) PB_CHECK(text->find("Secret") == std::string::npos && text->find("LEAK") == std::string::npos);
}

PB_TEST(the_wording_and_language_of_the_servers_message_do_not_change_the_key) {
    for (int wording = 0; wording < 2; ++wording) {
        Rig r;
        r.server.set_share_wording(wording);
        r.server.set_password_enforced(true);
        const auto d = r.api.create(kFolder, kUnknown, request());
        PB_CHECK_EQ(d.error.key, std::string("share_password_required"));
    }
}

PB_TEST(login_and_rate_limit_statuses_keep_their_keys_for_every_request) {
    Rig r;
    const int64_t id = r.server.add_share([] { FakeNextcloud::Share s; s.path = "/Docs/Reports"; return s; }());
    pbc::DavSession bad(r.server, r.server.base_url(), "demo", "wrong");
    pbc::ShareApi api(bad, r.clock, "demo");
    PB_CHECK_EQ(api.create(kFolder, kUnknown, request()).error.key, std::string("auth_failed"));
    PB_CHECK_EQ(api.remove_link(id, kFolder, kUnknown).key, std::string("auth_failed"));
    r.server.answer_next("POST", "/shares", 429, 1, "20");
    PB_CHECK_EQ(r.api.create(kFolder, kUnknown, request()).error.key, std::string("rate_limited"));
    r.server.answer_next("POST", "/shares", 503);
    PB_CHECK_EQ(r.api.create(kFolder, kUnknown, request()).error.key, std::string("maintenance"));
}

PB_TEST(no_message_key_code_or_log_line_of_a_refusal_holds_a_link_a_token_a_password_or_an_address) {
    Rig r;
    r.server.set_password_enforced(true);
    std::vector<std::string> lines;
    pbc::log::set_sink([&](const std::string& l) { lines.push_back(l); });
    (void)r.api.create(kFolder, kUnknown, request(pbc::LinkPermission::ReadOnly, "Zx9-secret-pw"));
    r.server.set_unshareable("/Docs/Reports");
    r.server.set_password_enforced(false);
    (void)r.api.create(kFolder, kUnknown, request(pbc::LinkPermission::ReadOnly, "Zx9-secret-pw"));
    pbc::log::reset_sink();
    PB_CHECK(!lines.empty());
    for (const auto& l : lines)
        PB_CHECK(l.find("Zx9") == std::string::npos && l.find("Reports") == std::string::npos && l.find("cloud.example.org") == std::string::npos && l.find("/s/") == std::string::npos);
    const auto& m = r.api.message();
    PB_CHECK(m.key.find("Zx9") == std::string::npos && m.code.find("Zx9") == std::string::npos);
}

PB_TEST_MAIN()
