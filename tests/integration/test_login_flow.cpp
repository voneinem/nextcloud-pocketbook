// Login Flow v2 (research R-4, contracts/server-requests.md section 9) against the fake Nextcloud: no password is typed on the reader,
// the token lives in memory only, nothing is polled in the background, and no address on another origin is ever used.
#include <string>
#include <vector>

#include "fake_nextcloud_server.h"
#include "pbc/log.h"
#include "pbc/nc_login.h"
#include "test_support.h"

PBC_FAKE_SERVER_TEST();

using pbtest::FakeNextcloud;

namespace {

size_t polls(const FakeNextcloud& s) {
    size_t n = 0;
    for (const auto& l : s.log())
        if (l.method == "POST" && l.path.find("/login/v2/poll") != std::string::npos) ++n;
    return n;
}

}  // namespace

PB_TEST(starting_the_flow_returns_the_link_the_poll_address_and_the_token_without_credentials) {
    FakeNextcloud s;
    const auto r = pbc::login_start(s, s.base_url());
    PB_CHECK(r.ok());
    PB_CHECK(r.value.login_url.compare(0, s.base_url().size() + 1, s.base_url() + "/") == 0);
    PB_CHECK(r.value.poll_endpoint.compare(0, s.base_url().size() + 1, s.base_url() + "/") == 0);
    PB_CHECK(!r.value.token.empty());
    const auto& sent = s.transport().requests();
    PB_CHECK_EQ(sent.size(), size_t(1));
    PB_CHECK_EQ(sent[0].method, std::string("POST"));
    PB_CHECK(sent[0].username.empty() && sent[0].password.empty());
}

PB_TEST(a_link_or_poll_address_on_another_origin_is_refused) {
    for (const char* variant : {"link_other_host", "poll_other_host", "poll_other_port", "link_http", "poll_http"}) {
        FakeNextcloud s;
        s.set_login_variant(variant);
        const auto r = pbc::login_start(s, s.base_url());
        PB_CHECK_EQ(r.error.key, std::string("redirect_other_host"));
        PB_CHECK(r.value.token.empty());   // nothing is kept from a refused start
    }
}

PB_TEST(a_token_that_is_too_long_or_not_printable_is_a_protocol_error) {
    for (const char* variant : {"token_long", "token_control", "token_empty", "missing_poll", "not_json"}) {
        FakeNextcloud s;
        s.set_login_variant(variant);
        PB_CHECK_EQ(pbc::login_start(s, s.base_url()).error.key, std::string("protocol_error"));
    }
    FakeNextcloud ok;
    ok.set_login_variant("token_512");
    PB_CHECK(pbc::login_start(ok, ok.base_url()).ok());   // exactly 512 printable characters are fine
}

PB_TEST(check_sends_one_request_and_says_not_yet_until_the_user_approved) {
    FakeNextcloud s;
    auto start = pbc::login_start(s, s.base_url());
    PB_CHECK(start.ok());
    pbc::LoginStart flow = start.value;
    for (int i = 0; i < 3; ++i) {
        const auto r = pbc::login_check(s, s.base_url(), flow);
        PB_CHECK(r.ok());
        PB_CHECK(!r.value.has_value());   // 404: not approved yet
        PB_CHECK_EQ(polls(s), size_t(i + 1));   // exactly one request per call, no loop
    }
    PB_CHECK(!flow.token.empty());   // the flow is still open
    const auto& last = s.transport().requests().back();
    PB_CHECK(last.body.find("token=") == 0);
    PB_CHECK_EQ(pbc::header_value(last.headers, "Content-Type"), std::string("application/x-www-form-urlencoded"));
}

PB_TEST(after_the_approval_check_returns_the_credentials_once_and_the_token_is_gone) {
    FakeNextcloud s;
    auto start = pbc::login_start(s, s.base_url());
    pbc::LoginStart flow = start.value;
    s.approve_login("demo", "generated-app-password");
    const auto r = pbc::login_check(s, s.base_url(), flow);
    PB_CHECK(r.ok());
    PB_CHECK(r.value.has_value());
    PB_CHECK_EQ(r.value->server, s.base_url());
    PB_CHECK_EQ(r.value->login_name, std::string("demo"));
    PB_CHECK_EQ(r.value->app_password, std::string("generated-app-password"));
    PB_CHECK(flow.token.empty() && flow.poll_endpoint.empty() && flow.login_url.empty());   // discarded after use
    PB_CHECK_EQ(pbc::login_check(s, s.base_url(), flow).error.key, std::string("login_expired"));   // and cannot be checked again
}

PB_TEST(a_server_address_on_another_origin_in_the_answer_is_refused) {
    FakeNextcloud s;
    auto start = pbc::login_start(s, s.base_url());
    pbc::LoginStart flow = start.value;
    s.set_login_server_url("https://other.example.org");
    s.approve_login("demo", "pw");
    const auto r = pbc::login_check(s, s.base_url(), flow);
    PB_CHECK_EQ(r.error.key, std::string("redirect_other_host"));
    PB_CHECK(flow.token.empty());   // the flow ends, the password it returned is dropped
}

PB_TEST(any_other_status_ends_the_flow_as_failed) {
    for (long status : {400L, 401L, 403L, 500L}) {
        FakeNextcloud s;
        auto start = pbc::login_start(s, s.base_url());
        pbc::LoginStart flow = start.value;
        s.set_login_poll_status(status);
        PB_CHECK_EQ(pbc::login_check(s, s.base_url(), flow).error.key, std::string("login_failed"));
        PB_CHECK(flow.token.empty());
    }
    FakeNextcloud off;
    auto start = pbc::login_start(off, off.base_url());
    pbc::LoginStart flow = start.value;
    off.set_offline(true);
    const auto r = pbc::login_check(off, off.base_url(), flow);
    PB_CHECK_EQ(r.error.key, std::string("server_unreachable"));
    PB_CHECK(!flow.token.empty());   // a network failure does not end the flow: the user may tap Check again
}

PB_TEST(discard_overwrites_everything) {
    pbc::LoginStart flow;
    flow.token = std::string(40, 't');
    flow.poll_endpoint = "https://cloud.example.org/p";
    flow.login_url = "https://cloud.example.org/l";
    const char* where = flow.token.data();
    pbc::login_discard(flow);
    PB_CHECK(flow.token.empty() && flow.poll_endpoint.empty() && flow.login_url.empty());
    (void)where;
}

PB_TEST(neither_the_token_nor_the_password_reaches_the_log) {
    std::vector<std::string> lines;
    pbc::log::set_sink([&](const std::string& l) { lines.push_back(l); });
    FakeNextcloud s;
    auto start = pbc::login_start(s, s.base_url());
    pbc::LoginStart flow = start.value;
    const std::string token = flow.token;
    pbc::login_check(s, s.base_url(), flow);
    s.approve_login("demo", "generated-app-password");
    pbc::login_check(s, s.base_url(), flow);
    pbc::log::reset_sink();
    for (const auto& l : lines) {
        PB_CHECK(l.find(token) == std::string::npos);
        PB_CHECK(l.find("generated-app-password") == std::string::npos);
        PB_CHECK(l.find("cloud.example.org") == std::string::npos);
    }
}

PB_TEST_MAIN()
