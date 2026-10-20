// The connection Test (research R-3, contracts/server-requests.md sections 1-4) against the fake Nextcloud: every outcome the user is
// meant to understand has its own key, nothing secret leaves the app, and the checks stop at the first failure.
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "fake_nextcloud_server.h"
#include "pbc/error_keys.h"
#include "pbc/log.h"
#include "pbc/nc_server.h"
#include "pbc/version.h"
#include "test_support.h"

PBC_FAKE_SERVER_TEST();

using pbtest::FakeNextcloud;

namespace {

struct FixedClock : pbc::Clock {
    int64_t t;
    explicit FixedClock(int64_t value) : t(value) {}
    int64_t now() const override { return t; }
};

struct Env {
    FakeNextcloud server;
    FixedClock clock{server.server_time()};
    pbc::Result<pbc::AccountCheck> check(const std::string& password = "app-pass-0001") {
        return pbc::check_account(server, server.base_url(), "demo", password, clock);
    }
};

pbc::Response ocs_401(const std::string& message) {
    pbc::Response r;
    r.status = 401;
    r.body = "{\"ocs\":{\"meta\":{\"status\":\"failure\",\"statuscode\":997,\"message\":\"" + message + "\"},\"data\":[]}}";
    return r;
}

std::function<std::optional<pbc::Response>(const pbc::Request&)> on_path(const std::string& part, pbc::Response answer) {
    return [part, answer](const pbc::Request& r) -> std::optional<pbc::Response> {
        if (r.url.find(part) != std::string::npos) return answer;
        return std::nullopt;
    };
}

}  // namespace

PB_TEST(a_good_account_shows_the_server_name_version_user_and_quota) {
    Env e;
    e.server.set_quota(1000, 9000);
    const auto r = e.check();
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.server.name, std::string("Example Cloud"));
    PB_CHECK_EQ(r.value.server.version, std::string("31.0.4"));
    PB_CHECK_EQ(r.value.server.major, 31);
    PB_CHECK_EQ(r.value.user_id, std::string("demo"));
    PB_CHECK_EQ(*r.value.quota.used, int64_t(1000));
    PB_CHECK_EQ(*r.value.quota.total, int64_t(10000));
    PB_CHECK(!r.value.quota.unlimited);
    PB_CHECK(r.value.server_time.has_value());
    PB_CHECK_EQ(r.value.clock_warning_minutes, 0);
    // four requests, in this order, and the credentials stay away from the one that needs none
    const auto& sent = e.server.transport().requests();
    PB_CHECK_EQ(sent.size(), size_t(4));
    PB_CHECK(sent[0].url.find("/status.php") != std::string::npos);
    PB_CHECK(sent[0].username.empty() && sent[0].password.empty());
    PB_CHECK(sent[1].url.find("/cloud/capabilities") != std::string::npos);
    PB_CHECK(sent[2].url.find("/cloud/user") != std::string::npos);
    PB_CHECK_EQ(sent[3].method, std::string("PROPFIND"));
    PB_CHECK_EQ(pbc::header_value(sent[3].headers, "Depth"), std::string("0"));
    for (const auto& q : sent) PB_CHECK(q.url.find("app-pass") == std::string::npos && q.url.find("demo:") == std::string::npos);   // never in the URL
}

PB_TEST(the_quota_of_an_unlimited_or_unknown_account) {
    Env e;
    auto r = e.check();   // the fake reports -3: unlimited
    PB_CHECK(r.ok());
    PB_CHECK(r.value.quota.unlimited);
    PB_CHECK(!r.value.quota.total.has_value());
    PB_CHECK(r.value.quota.used.has_value());
    for (int64_t code : {int64_t(-1), int64_t(-2)}) {   // other negative codes mean "unknown"
        e.server.set_quota(500, code);
        r = e.check();
        PB_CHECK(r.ok());
        PB_CHECK(!r.value.quota.unlimited);
        PB_CHECK(!r.value.quota.total.has_value());
    }
    e.server.set_quota(0, 0);   // a full quota is 0 bytes available, not unknown
    r = e.check();
    PB_CHECK_EQ(*r.value.quota.total, int64_t(0));
}

PB_TEST(a_wrong_password_is_auth_failed_and_the_check_stops_there) {
    Env e;
    const auto r = e.check("wrong");
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("auth_failed"));
    PB_CHECK(r.error.cls == pbc::ErrorClass::Auth);
    PB_CHECK_EQ(e.server.request_count(), size_t(2));   // status.php and capabilities, nothing more
}

PB_TEST(a_marker_for_an_app_password_gives_the_distinct_key) {
    Env e;
    e.server.set_interceptor(on_path("/cloud/capabilities", ocs_401("Logging in with your password is disabled, create an app password in your security settings")));
    PB_CHECK_EQ(e.check().error.key, std::string("auth_app_password_required"));
    e.server.set_interceptor(on_path("/cloud/capabilities", ocs_401("Invalid credentials")));
    PB_CHECK_EQ(e.check().error.key, std::string("auth_failed"));
    e.server.set_interceptor(on_path("/cloud/capabilities", ocs_401(std::string(5000, 'x') + " app password")));
    PB_CHECK_EQ(e.check().error.key, std::string("auth_failed"));   // only the first 512 bytes are read
}

PB_TEST(maintenance_mode_is_reported_before_any_login) {
    Env e;
    e.server.set_maintenance(true);
    const auto r = e.check();
    PB_CHECK_EQ(r.error.key, std::string("maintenance"));
    PB_CHECK_EQ(e.server.request_count(), size_t(1));   // only status.php
}

PB_TEST(a_503_on_a_later_call_is_maintenance_too) {
    Env e;
    pbc::Response r503;
    r503.status = 503;
    e.server.set_interceptor(on_path("/cloud/user", r503));
    PB_CHECK_EQ(e.check().error.key, std::string("maintenance"));
}

PB_TEST(a_server_without_file_access_is_no_webdav) {
    Env e;
    for (long status : {404L, 405L, 501L}) {
        pbc::Response answer;
        answer.status = status;
        e.server.set_interceptor(on_path("/remote.php/dav/", answer));
        PB_CHECK_EQ(e.check().error.key, std::string("no_webdav"));
    }
    pbc::Response forbidden;
    forbidden.status = 403;
    e.server.set_interceptor(on_path("/remote.php/dav/", forbidden));
    PB_CHECK_EQ(e.check().error.key, std::string("forbidden"));
}

PB_TEST(a_server_too_old_and_an_address_that_is_not_a_nextcloud) {
    Env e;
    e.server.set_version("27.1.11.1");
    auto r = e.check();
    PB_CHECK_EQ(r.error.key, std::string("server_too_old"));
    PB_CHECK_EQ(r.value.server.version, std::string("27.1.11.1"));   // the message can name it
    Env other;
    pbc::Response page;
    page.status = 200;
    page.body = "<html><body>It works</body></html>";
    other.server.set_interceptor(on_path("/status.php", page));
    PB_CHECK_EQ(other.check().error.key, std::string("not_nextcloud"));
}

PB_TEST(transport_failures_keep_their_keys) {
    for (const char* key : {"cert_untrusted", "cert_expired", "cert_host_mismatch", "server_unreachable", "no_network", "timeout", "scheme_not_allowed"}) {
        Env e;
        pbc::Response down;
        down.error = pbc::class_of(key);
        down.error_key = key;
        e.server.set_interceptor([down](const pbc::Request&) -> std::optional<pbc::Response> { return down; });
        PB_CHECK_EQ(e.check().error.key, std::string(key));
    }
}

PB_TEST(a_clock_before_the_floor_is_reported_before_anything_is_sent) {
    Env e;
    e.clock.t = pbc::kClockFloorEpoch - 1;
    const auto r = e.check();
    PB_CHECK_EQ(r.error.key, std::string("clock_implausible"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    e.clock.t = pbc::kClockFloorEpoch;
    e.server.set_server_time(pbc::kClockFloorEpoch);
    PB_CHECK(e.check().ok());
}

PB_TEST(a_difference_to_the_server_time_above_ten_minutes_raises_the_warning) {
    Env e;
    e.server.set_server_time(e.clock.t - 3600);   // the reader is an hour ahead
    auto r = e.check();
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.clock_warning_minutes, 60);
    e.server.set_server_time(e.clock.t + 300);    // five minutes: fine
    PB_CHECK_EQ(e.check().value.clock_warning_minutes, 0);
    e.server.set_server_time(e.clock.t + 601);
    PB_CHECK_EQ(e.check().value.clock_warning_minutes, 11);
    e.server.omit_date_header(true);
    r = e.check();
    PB_CHECK(r.ok());
    PB_CHECK(!r.value.server_time.has_value());
    PB_CHECK_EQ(r.value.clock_warning_minutes, 0);   // no Date header: no claim about the clock
}

PB_TEST(a_user_id_that_could_build_a_hostile_path_is_a_protocol_error) {
    for (const char* bad : {"a/b", "..", ".", "", "x\\u0001y"}) {
        Env e;
        std::string json = std::string("{\"ocs\":{\"meta\":{\"status\":\"ok\",\"statuscode\":100},\"data\":{\"id\":\"") + bad + "\"}}}";
        pbc::Response answer;
        answer.status = 200;
        answer.body = json;
        e.server.set_interceptor(on_path("/cloud/user", answer));
        PB_CHECK_EQ(e.check().error.key, std::string("protocol_error"));
    }
}

PB_TEST(a_429_carries_the_pause_the_server_asks_for) {
    Env e;
    pbc::Response slow;
    slow.status = 429;
    slow.headers = {{"Retry-After", "120"}};
    e.server.set_interceptor(on_path("/cloud/capabilities", slow));
    const auto r = e.check();
    PB_CHECK_EQ(r.error.key, std::string("rate_limited"));
    PB_CHECK_EQ(*r.value.retry_after_s, int64_t(120));
    slow.headers = {{"Retry-After", "100000"}};
    e.server.set_interceptor(on_path("/cloud/capabilities", slow));
    PB_CHECK_EQ(*e.check().value.retry_after_s, int64_t(900));   // capped at fifteen minutes
}

PB_TEST(the_check_at_start_is_one_authenticated_request) {
    Env e;
    const auto r = pbc::quick_check(e.server, e.server.base_url(), "demo", "app-pass-0001", e.clock);
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.server.name, std::string("Example Cloud"));
    PB_CHECK_EQ(r.value.server.version, std::string("31.0.4"));
    PB_CHECK_EQ(e.server.request_count(), size_t(1));
    PB_CHECK(e.server.log()[0].path.find("/cloud/capabilities") != std::string::npos);
    PB_CHECK_EQ(r.value.clock_warning_minutes, 0);
    PB_CHECK(r.value.server_time.has_value());

    e.server.clear_log();
    e.server.set_server_time(e.clock.t - 7200);
    PB_CHECK_EQ(pbc::quick_check(e.server, e.server.base_url(), "demo", "app-pass-0001", e.clock).value.clock_warning_minutes, 120);
    PB_CHECK_EQ(pbc::quick_check(e.server, e.server.base_url(), "demo", "wrong", e.clock).error.key, std::string("auth_failed"));
    e.server.set_maintenance(true);
    PB_CHECK_EQ(pbc::quick_check(e.server, e.server.base_url(), "demo", "app-pass-0001", e.clock).error.key, std::string("maintenance"));
    e.server.set_maintenance(false);
    e.clock.t = pbc::kClockFloorEpoch - 1;
    e.server.clear_log();
    PB_CHECK_EQ(pbc::quick_check(e.server, e.server.base_url(), "demo", "app-pass-0001", e.clock).error.key, std::string("clock_implausible"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(the_app_password_never_reaches_the_log) {
    std::vector<std::string> lines;
    pbc::log::set_sink([&](const std::string& l) { lines.push_back(l); });
    Env e;
    e.check("app-pass-0001");
    e.check("wrong-secret-xyz");
    e.server.set_maintenance(true);
    e.check("app-pass-0001");
    pbc::log::reset_sink();
    for (const auto& l : lines) {
        PB_CHECK(l.find("app-pass") == std::string::npos);
        PB_CHECK(l.find("wrong-secret") == std::string::npos);
        PB_CHECK(l.find("cloud.example.org") == std::string::npos);
        PB_CHECK(l.find("demo") == std::string::npos);
    }
}

PB_TEST_MAIN()
