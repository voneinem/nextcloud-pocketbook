// ShareApi::detail against the fake Nextcloud (specs/004-share-links US3, contracts/share-requests.md L4): the only read whose answer may hold a link,
// with the checks a link must pass before it is shown, and no cache between two calls.
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
        server.add_file("/Docs/a.pdf", "alpha");
    }
    int64_t add(const std::string& url = std::string(), int perms = 1) {
        FakeNextcloud::Share s;
        s.path = "/Docs";
        s.permissions = perms;
        s.url_override = url;
        return server.add_share(s);
    }
};

}  // namespace

PB_TEST(a_link_of_the_answer_is_returned_in_full_and_checked_ok) {
    Rig r;
    const int64_t id = r.add();
    const auto d = r.api.detail(id, true);
    PB_CHECK(d.ok());
    PB_CHECK(d.value.link_state == pbc::LinkState::Ok);
    PB_CHECK_EQ(d.value.link, r.server.base_url() + "/s/" + r.server.find_share(id)->token);
    PB_CHECK_EQ(d.value.row.id, id);
    PB_CHECK(d.value.row.owned_by_me && d.value.row.permission == pbc::LinkPermission::ReadOnly);
    PB_CHECK_EQ(r.server.request_count(), size_t(1));
    const auto& log = r.server.share_log();
    PB_CHECK_EQ(log[0].method, std::string("GET"));
    PB_CHECK_EQ(log[0].route, "/shares/" + std::to_string(id));
    PB_CHECK_EQ(log[0].query, std::string("format=json"));
}

PB_TEST(a_link_of_120_characters_survives_unchanged) {
    Rig r;
    const std::string prefix = "https://cloud.example.org/s/";
    const std::string url = prefix + std::string(120 - prefix.size(), 'k');
    PB_CHECK_EQ(url.size(), size_t(120));
    const int64_t id = r.add(url);
    const auto d = r.api.detail(id, true);
    PB_CHECK(d.ok());
    PB_CHECK_EQ(d.value.link, url);
}

PB_TEST(another_host_a_plain_address_and_unusable_addresses_are_classified_not_hidden_or_trusted) {
    Rig r;
    const auto other = r.api.detail(r.add("https://public.example.net/s/abc"), true);
    PB_CHECK(other.ok() && other.value.link_state == pbc::LinkState::DifferentAddress && other.value.link == "https://public.example.net/s/abc");
    const auto plain = r.api.detail(r.add(std::string("http") + "://cloud.example.org/s/abc"), true);
    PB_CHECK(plain.ok() && plain.value.link_state == pbc::LinkState::NotSecure);
    for (const std::string& bad : {std::string("https://cloud.example.org/s/a b"), std::string("https://cloud.example.org/s/a\x01" "b"),
                                   std::string("https://cloud.example.org/s/\xc3\xa4"), "https://cloud.example.org/s/" + std::string(600, 'a'),
                                   std::string("javascript:alert(1)")}) {
        const auto d = r.api.detail(r.add(bad), true);
        if (!d.ok() || d.value.link_state != pbc::LinkState::Unreadable || !d.value.link.empty()) PB_FAIL_AT("shown although unusable: " + bad.substr(0, 40));
    }
}

PB_TEST(a_share_that_is_gone_is_share_gone) {
    Rig r;
    const auto d = r.api.detail(424242, true);
    PB_CHECK(!d.ok());
    PB_CHECK_EQ(d.error.key, std::string("share_gone"));
    PB_CHECK_EQ(r.api.message().key, std::string("share_gone"));
    PB_CHECK_EQ(r.server.request_count(), size_t(1));   // no extra read of the rules for a plain read
}

PB_TEST(every_call_is_a_new_request_nothing_is_cached) {
    Rig r;
    const int64_t id = r.add();
    PB_CHECK(r.api.detail(id, true).ok());
    PB_CHECK(r.api.detail(id, true).ok());
    PB_CHECK_EQ(r.server.request_count(), size_t(2));
    // a change made behind the app's back shows at once
    r.server.find_share(id);
    r.server.remove_share(id);
    PB_CHECK_EQ(r.api.detail(id, true).error.key, std::string("share_gone"));
}

PB_TEST(the_other_failures_keep_their_keys) {
    Rig r;
    const int64_t id = r.add();
    pbc::DavSession bad(r.server, r.server.base_url(), "demo", "wrong");
    pbc::ShareApi api(bad, r.clock, "demo");
    PB_CHECK_EQ(api.detail(id, true).error.key, std::string("auth_failed"));
    r.server.answer_next("GET", "/shares/", 500);
    PB_CHECK_EQ(r.api.detail(id, true).error.key, std::string("share_refused"));
    PB_CHECK_EQ(r.api.message().code, std::string("E500"));
    r.server.set_offline(true);
    PB_CHECK_EQ(r.api.detail(id, true).error.key, std::string("server_unreachable"));
    r.server.set_offline(false);
    r.server.set_interceptor([](const pbc::Request&) -> std::optional<pbc::Response> {
        pbc::Response html;
        html.status = 200;
        html.body = "<html>nope</html>";
        return html;
    });
    PB_CHECK_EQ(r.api.detail(id, true).error.key, std::string("protocol_error"));
}

PB_TEST(a_non_link_share_has_no_link_in_its_detail) {
    Rig r;
    FakeNextcloud::Share user;
    user.type = 0;
    user.path = "/Docs";
    user.with = "bob";
    const int64_t id = r.server.add_share(user);
    const auto d = r.api.detail(id, true);
    PB_CHECK(d.ok());
    PB_CHECK(d.value.link.empty() && d.value.link_state == pbc::LinkState::Unreadable);
}

PB_TEST_MAIN()
