// ShareApi::fetch_rules and ShareApi::list against the fake Nextcloud (specs/004-share-links US2, contracts/share-requests.md L1-L3): the three
// reads of an open, the rows with kind, permission, expiry and owner, the optional inherited list, bounds, refusals and the existence check.
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
        server.add_file("/Docs/a.pdf", "alpha");
        server.add_folder("/Docs/Reports");
    }
};

FakeNextcloud::Share link(const std::string& path, int perms = 1) {
    FakeNextcloud::Share s;
    s.path = path;
    s.permissions = perms;
    return s;
}

const pbc::ShareTarget kReports{"/Docs/Reports", true};

}  // namespace

PB_TEST(an_open_is_three_reads_in_a_fixed_order_and_nothing_else) {
    Rig r;
    r.server.add_share(link("/Docs/Reports"));
    const auto rules = r.api.fetch_rules();
    PB_CHECK(rules.ok());
    const auto l = r.api.list(kReports);
    PB_CHECK(l.ok());
    PB_CHECK_EQ(r.server.request_count(), size_t(3));
    const auto& log = r.server.log();
    PB_CHECK_EQ(log[0].method, std::string("GET"));
    PB_CHECK(log[0].path.find("/ocs/v1.php/cloud/capabilities") == 0);
    PB_CHECK(log[1].path.find("/shares?path=") != std::string::npos && log[1].path.find("/shares/inherited") == std::string::npos);
    PB_CHECK(log[2].path.find("/shares/inherited?path=") != std::string::npos);
    for (const auto& e : log) PB_CHECK(e.depth.empty() && e.body_bytes == 0 && e.method == "GET");
}

PB_TEST(five_mixed_rows_come_back_with_kind_permission_expiry_and_owner) {
    Rig r;
    auto reports = link("/Docs/Reports", 15);
    reports.expiration = "2026-12-31";
    reports.password = "pw";
    r.server.add_share(reports);
    FakeNextcloud::Share user;
    user.type = 0;
    user.path = "/Docs/Reports";
    user.with = "bob";
    user.with_display = "Bob Example";
    r.server.add_share(user);
    FakeNextcloud::Share group = user;
    group.type = 1;
    group.with = "team";
    group.with_display = "Team Example";
    r.server.add_share(group);
    FakeNextcloud::Share mail = user;
    mail.type = 4;
    r.server.add_share(mail);
    auto foreign = link("/Docs/Reports");
    foreign.owner = "carol";
    r.server.add_share(foreign);
    const auto l = r.api.list(kReports);
    PB_CHECK(l.ok());
    const auto& rows = l.value.own.rows;
    PB_CHECK_EQ(rows.size(), size_t(5));
    PB_CHECK(rows[0].kind == pbc::ShareKind::PublicLink && rows[0].permission == pbc::LinkPermission::ReadUpload && rows[0].has_password);
    PB_CHECK(rows[0].expires == std::optional<std::string>("2026-12-31") && rows[0].owned_by_me);
    PB_CHECK(rows[1].kind == pbc::ShareKind::User && rows[1].with_name == "Bob Example");
    PB_CHECK(rows[2].kind == pbc::ShareKind::Group && rows[2].with_name == "Team Example");
    PB_CHECK(rows[3].kind == pbc::ShareKind::Other);
    PB_CHECK(rows[4].kind == pbc::ShareKind::PublicLink && !rows[4].owned_by_me);
    for (const auto& row : rows) PB_CHECK(!row.inherited);
}

PB_TEST(inherited_rows_are_marked_and_a_missing_inherited_list_is_silent) {
    Rig r;
    r.server.set_inherited_shares("/Docs/Reports", {link("/Docs")});
    auto l = r.api.list(kReports);
    PB_CHECK(l.ok() && !l.value.inherited_failed);
    PB_CHECK_EQ(l.value.inherited.rows.size(), size_t(1));
    PB_CHECK(l.value.inherited.rows[0].inherited);
    for (long status : {404L, 405L, 501L}) {
        r.server.set_inherited_status(status);
        l = r.api.list(kReports);
        PB_CHECK(l.ok());
        PB_CHECK(!l.value.inherited_failed && l.value.inherited.rows.empty());
    }
}

PB_TEST(another_failure_of_the_inherited_list_keeps_the_own_rows_and_says_so) {
    Rig r;
    r.server.add_share(link("/Docs/Reports"));
    r.server.answer_next("GET", "/shares/inherited", 500);
    const auto l = r.api.list(kReports);
    PB_CHECK(l.ok());
    PB_CHECK(l.value.inherited_failed);
    PB_CHECK_EQ(l.value.own.rows.size(), size_t(1));
}

PB_TEST(an_item_without_shares_gives_an_empty_list) {
    Rig r;
    const auto l = r.api.list(pbc::ShareTarget{"/Docs/a.pdf", false});
    PB_CHECK(l.ok());
    PB_CHECK(l.value.own.rows.empty() && !l.value.own.truncated);
}

PB_TEST(six_hundred_shares_give_five_hundred_rows_and_the_truncation_flag) {
    Rig r;
    r.server.add_generated_shares("/Docs/Reports", 600);
    const auto l = r.api.list(kReports);
    PB_CHECK(l.ok());
    PB_CHECK_EQ(l.value.own.rows.size(), size_t(500));
    PB_CHECK(l.value.own.truncated);
}

PB_TEST(the_rules_are_read_from_the_capabilities) {
    Rig r;
    r.server.set_password_enforced(true);
    r.server.set_expiry(true, true, 14);
    r.server.set_public_upload(false);
    const auto rules = r.api.fetch_rules();
    PB_CHECK(rules.ok());
    PB_CHECK(pbc::password_required(rules.value, pbc::LinkPermission::ReadOnly));
    PB_CHECK(pbc::expiry_max_days(rules.value) == std::optional<int>(14));
    PB_CHECK(!pbc::upload_offered(rules.value, true));
    r.server.set_sharing_enabled(false);
    PB_CHECK(!pbc::sharing_offered(r.api.fetch_rules().value));
}

PB_TEST(sharing_switched_off_on_the_server_gives_share_not_offered) {
    Rig r;
    r.server.set_sharing_enabled(false);
    const auto l = r.api.list(kReports);
    PB_CHECK(!l.ok());
    PB_CHECK_EQ(l.error.key, std::string("share_not_offered"));
    PB_CHECK_EQ(r.api.message().key, std::string("share_not_offered"));
}

PB_TEST(an_item_deleted_since_the_listing_gives_share_item_gone) {
    Rig r;
    r.server.remove("/Docs/Reports");
    const auto l = r.api.list(kReports);
    PB_CHECK(!l.ok());
    PB_CHECK_EQ(l.error.key, std::string("share_item_gone"));
    PB_CHECK(r.server.count("PROPFIND") == 1);   // the one Depth-0 check of research R-4
    PB_CHECK_EQ(r.server.depths().at(0), std::string("0"));
}

PB_TEST(a_refused_login_is_auth_failed_and_other_statuses_keep_their_keys) {
    Rig r;
    pbc::DavSession bad(r.server, r.server.base_url(), "demo", "wrong");
    pbc::ShareApi api(bad, r.clock, "demo");
    PB_CHECK_EQ(api.fetch_rules().error.key, std::string("auth_failed"));
    PB_CHECK_EQ(api.list(kReports).error.key, std::string("auth_failed"));
    r.server.answer_next("GET", "/shares?path", 429, 1, "30");
    PB_CHECK_EQ(r.api.list(kReports).error.key, std::string("rate_limited"));
    r.server.answer_next("GET", "/shares?path", 503);
    PB_CHECK_EQ(r.api.list(kReports).error.key, std::string("maintenance"));
    r.server.set_offline(true);
    PB_CHECK_EQ(r.api.list(kReports).error.key, std::string("server_unreachable"));
}

PB_TEST(an_answer_over_two_mebibytes_or_not_json_is_a_protocol_error_and_shows_nothing) {
    Rig r;
    r.server.set_interceptor([](const pbc::Request& q) -> std::optional<pbc::Response> {
        if (q.url.find("/shares?path") == std::string::npos) return std::nullopt;
        pbc::Response big;
        big.status = 200;
        big.body = std::string(3u << 20, ' ') + "{}";
        return big;
    });
    const auto l = r.api.list(kReports);
    PB_CHECK(!l.ok());
    PB_CHECK_EQ(l.error.key, std::string("protocol_error"));
    r.server.set_interceptor([](const pbc::Request& q) -> std::optional<pbc::Response> {
        if (q.url.find("/shares?path") == std::string::npos) return std::nullopt;
        pbc::Response huge;
        huge.status = 200;
        huge.body_too_large = true;
        return huge;
    });
    PB_CHECK_EQ(r.api.list(kReports).error.key, std::string("protocol_error"));
    r.server.set_interceptor([](const pbc::Request& q) -> std::optional<pbc::Response> {
        if (q.url.find("/shares?path") == std::string::npos) return std::nullopt;
        pbc::Response html;
        html.status = 200;
        html.body = "<html>maintenance page</html>";
        return html;
    });
    PB_CHECK_EQ(r.api.list(kReports).error.key, std::string("protocol_error"));
}

PB_TEST(the_log_of_an_open_holds_counts_and_codes_only) {
    Rig r;
    r.server.add_generated_shares("/Docs/Reports", 3);
    std::vector<std::string> lines;
    pbc::log::set_sink([&](const std::string& l) { lines.push_back(l); });
    PB_CHECK(r.api.list(kReports).ok());
    pbc::log::reset_sink();
    bool opened = false;
    for (const auto& l : lines) {
        if (l.find("share.open") != std::string::npos) opened = true;
        PB_CHECK(l.find("Docs") == std::string::npos && l.find("TOK") == std::string::npos && l.find("cloud.example.org") == std::string::npos);
    }
    PB_CHECK(opened);
}

PB_TEST_MAIN()
