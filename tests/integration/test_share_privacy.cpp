// Links and passwords stay private (specs/004-share-links US7, research R-9). The harness (tests/support/privacy_probe.h) captures the log and
// every file the app wrote and searches it for a known password, token and link; the meta-tests prove that it finds a planted secret, and the
// cases of the flows are added by the story that makes the flow exist (a case that cannot run yet is a skipped case, not a missing one).
#include <string>

#include "fake_nextcloud_server.h"
#include "pbc/credentials_file.h"
#include "pbc/dav_common.h"
#include "pbc/share_api.h"
#include "pbc/log.h"
#include "pbc/store.h"
#include "privacy_probe.h"
#include "test_support.h"
#include "tmp_dir.h"

PBC_FAKE_SERVER_TEST();

namespace {

const std::string kPassword = "Zx9-secret-pw";
const std::string kToken = "TOKENqwertyuiop123456";

struct Env {
    pbtest::TmpDir dir;
    pbtest::FakeNextcloud server;
    pbc::SystemClock clock;
    std::unique_ptr<pbc::Store> store;
    std::string data() const { return dir.file("data"); }
    Env() {
        auto r = pbc::Store::open(data(), clock);
        if (!r.ok()) PB_FAIL_AT("cannot open the store");
        store = std::move(r.value);
        pbc::AccountRow a;
        a.base_url = server.base_url();
        a.login_name = "demo";
        a.user_id = "demo";
        a.state = "ok";
        PB_CHECK(store->save_account(a).ok());
        pbc::Credentials cr;
        cr.password = server.app_password();
        PB_CHECK(pbc::save_credentials(store->directory(), cr).ok());
    }
};

bool finds(const std::function<void()>& check) {
    try {
        check();
    } catch (const pbtest::Failure&) {
        return true;
    }
    return false;
}

}  // namespace

PB_TEST(the_probe_finds_a_secret_planted_in_the_log) {
    Env e;
    pbtest::PrivacyProbe probe;
    pbc::log::event("share.open", {pbc::log::text("result", "x " + kPassword)});
    PB_CHECK(finds([&] { probe.expect_absent(kPassword, e.data(), "planted"); }));
}

PB_TEST(the_probe_finds_a_secret_planted_in_a_file) {
    Env e;
    pbtest::PrivacyProbe probe;
    e.dir.write("data/notes.txt", "before " + kToken + " after");
    PB_CHECK(finds([&] { probe.expect_absent(kToken, e.data(), "planted"); }));
}

PB_TEST(the_probe_finds_a_secret_planted_in_the_database) {
    Env e;
    pbtest::PrivacyProbe probe;
    PB_CHECK(e.store->set_setting("auto_sync", kPassword).ok());
    PB_CHECK(finds([&] { probe.expect_absent(kPassword, e.data(), "planted"); }));
}

PB_TEST(the_probe_passes_a_clean_run_and_reports_text_collected_elsewhere) {
    Env e;
    pbtest::PrivacyProbe probe;
    PB_CHECK(e.store->set_setting("auto_sync", "on").ok());
    pbc::log::event("share.open", {pbc::log::status(200), pbc::log::count("rows", 3)});
    probe.expect_absent(kPassword, e.data(), "clean");
    probe.expect_absent(kToken, e.data(), "clean");
    PB_CHECK(finds([&] { pbtest::PrivacyProbe::expect_absent_in(kToken, "a message with " + kToken, "planted"); }));
    probe.expect_only_in(kToken, "the link page shows " + kToken, e.data(), "allowed where the screen asked for it");
    PB_CHECK(finds([&] { probe.expect_only_in(kToken, "no link here", e.data(), "must be present"); }));
    PB_CHECK_EQ(e.server.link_hits(), size_t(0));
}

PB_TEST(the_probe_sees_the_whole_log_and_every_file_name) {
    Env e;
    pbtest::PrivacyProbe probe;
    pbc::log::event("share.fail", {pbc::log::status(403)});
    PB_CHECK_EQ(probe.log_lines().size(), size_t(1));
    e.dir.write("data/a-" + kToken + ".tmp", "x");   // a secret in a file NAME is found too
    PB_CHECK(finds([&] { probe.expect_absent(kToken, e.data(), "planted"); }));
}

// ---- cases of the flows: enabled by the story that makes the flow exist ----

PB_TEST(list_case_no_link_token_or_password_after_opening_and_leaving_a_share_screen) {
    Env e;
    e.server.add_folder("/Docs");
    e.server.add_folder("/Docs/Reports");
    pbtest::FakeNextcloud::Share link;
    link.path = "/Docs/Reports";
    link.token = kToken;
    link.password = kPassword;   // the fake keeps the clear text; the list answer carries a hash, never the password
    e.server.add_share(link);
    e.server.add_generated_shares("/Docs/Reports", 5);
    pbtest::PrivacyProbe probe;
    pbc::DavSession session(e.server, e.server.base_url(), e.server.login_name(), e.server.app_password());
    pbc::ShareApi api(session, e.clock, e.server.user_id());
    PB_CHECK(api.fetch_rules().ok());
    const auto l = api.list({"/Docs/Reports", true});
    PB_CHECK(l.ok());
    PB_CHECK_EQ(l.value.own.rows.size(), size_t(6));
    // what the list holds: not the token, the address, the password or its hash
    std::string held;
    for (const auto& r : l.value.own.rows) held += r.label + "|" + r.with_name + "|" + r.expires.value_or("") + "|";
    pbtest::PrivacyProbe::expect_absent_in(kToken, held, "the rows of the list");
    pbtest::PrivacyProbe::expect_absent_in("https://", held, "the rows of the list");
    pbtest::PrivacyProbe::expect_absent_in("$2y$", held, "the rows of the list");
    // and what the app wrote: nothing
    probe.expect_absent(kToken, e.data(), "the token after opening Share");
    probe.expect_absent(kPassword, e.data(), "the password after opening Share");
    probe.expect_absent("$2y$", e.data(), "the password hash after opening Share");
    probe.expect_absent("/s/", e.data(), "a link address after opening Share");
    PB_CHECK_EQ(e.server.link_hits(), size_t(0));
    for (const auto& line : probe.log_lines()) PB_CHECK(line.find("Reports") == std::string::npos);
}
PB_TEST(create_case_the_password_is_found_nowhere_also_after_a_failed_create) {
    Env e;
    e.server.add_folder("/Docs");
    e.server.add_folder("/Docs/Reports");
    pbtest::PrivacyProbe probe;
    pbc::DavSession session(e.server, e.server.base_url(), e.server.login_name(), e.server.app_password());
    pbc::ShareApi api(session, e.clock, e.server.user_id());
    const auto rules = api.fetch_rules();
    PB_CHECK(rules.ok());
    auto make = [] {
        pbc::LinkRequest r;
        r.permission = pbc::LinkPermission::ReadUpload;
        r.password = pbc::SecretText{std::string(kPassword)};
        r.expiry = std::string("2026-12-31");
        return r;
    };
    // a success, an HTTP refusal, a request that never left, and one whose answer was lost
    PB_CHECK(api.create({"/Docs/Reports", true}, rules.value, make()).ok());
    e.server.answer_next("POST", "/shares", 500);
    PB_CHECK(!api.create({"/Docs/Reports", true}, rules.value, make()).ok());
    e.server.drop_before_processing("POST", "/shares");
    PB_CHECK(!api.create({"/Docs/Reports", true}, rules.value, make()).ok());
    e.server.lose_next_answer("POST", "/shares");
    PB_CHECK(!api.create({"/Docs/Reports", true}, rules.value, make()).ok());
    probe.expect_absent(kPassword, e.data(), "the password after create (success and three failures)");
    for (const auto& q : e.server.transport().requests()) pbtest::PrivacyProbe::expect_absent_in(kPassword, q.url, "a request URL");
    for (const auto& l : e.server.share_log()) pbtest::PrivacyProbe::expect_absent_in(kPassword, l.url + l.query, "a share request URL");
    // the log of create is codes, a status and counts
    for (const auto& line : probe.log_lines()) {
        if (line.find("share.") == std::string::npos) continue;
        PB_CHECK(line.find("share.open") != std::string::npos || line.find("share.create") != std::string::npos || line.find("share.fail") != std::string::npos);
        PB_CHECK(line.find("Reports") == std::string::npos && line.find("2026-12-31") == std::string::npos);
    }
    PB_CHECK_EQ(e.server.link_hits(), size_t(0));
}
PB_TEST(show_case_the_link_is_only_where_the_screen_asked_for_it_and_never_requested) {
    Env e;
    e.server.add_folder("/Docs");
    pbtest::FakeNextcloud::Share link;
    link.path = "/Docs";
    link.token = kToken;
    const int64_t id = e.server.add_share(link);
    pbtest::PrivacyProbe probe;
    pbc::DavSession session(e.server, e.server.base_url(), e.server.login_name(), e.server.app_password());
    pbc::ShareApi api(session, e.clock, e.server.user_id());
    const auto d = api.detail(id, true);
    PB_CHECK(d.ok());
    probe.expect_only_in(kToken, d.value.link, e.data(), "the link of a share");
    PB_CHECK_EQ(e.server.link_hits(), size_t(0));
    // the list of the same item shows no link at all
    const auto l = api.list({"/Docs", true});
    PB_CHECK(l.ok());
    std::string held;
    for (const auto& r : l.value.own.rows) held += r.label + r.with_name + r.expires.value_or("");
    pbtest::PrivacyProbe::expect_absent_in(kToken, held, "the rows of the list");
    for (const auto& line : probe.log_lines()) PB_CHECK(line.find(kToken) == std::string::npos && line.find("/s/") == std::string::npos);
}
PB_TEST(change_case_set_replace_and_remove_a_password_leaves_no_trace) {
    Env e;
    e.server.add_folder("/Docs");
    e.server.add_folder("/Docs/Reports");
    pbtest::FakeNextcloud::Share link;
    link.path = "/Docs/Reports";
    link.token = kToken;
    const int64_t id = e.server.add_share(link);
    pbtest::PrivacyProbe probe;
    pbc::DavSession session(e.server, e.server.base_url(), e.server.login_name(), e.server.app_password());
    pbc::ShareApi api(session, e.clock, e.server.user_id());
    const auto rules = api.fetch_rules();
    PB_CHECK(rules.ok());
    const pbc::ShareTarget target{"/Docs/Reports", true};
    auto shown = pbc::snapshot_of(api.detail(id, true).value.row);
    pbc::LinkChange set;
    set.action = pbc::LinkChange::Action::SetPassword;
    set.password = pbc::SecretText{std::string(kPassword)};
    auto d = api.change(target, rules.value, id, shown, std::move(set));
    PB_CHECK(d.ok());
    shown = pbc::snapshot_of(d.value.row);
    pbc::LinkChange replace;
    replace.action = pbc::LinkChange::Action::SetPassword;
    replace.password = pbc::SecretText{std::string(kPassword) + "-2"};
    d = api.change(target, rules.value, id, shown, std::move(replace));
    PB_CHECK(d.ok());
    shown = pbc::snapshot_of(d.value.row);
    pbc::LinkChange clear;
    clear.action = pbc::LinkChange::Action::ClearPassword;
    PB_CHECK(api.change(target, rules.value, id, shown, std::move(clear)).ok());
    // a failed change
    e.server.answer_next("PUT", "/shares", 500);
    pbc::LinkChange failed;
    failed.action = pbc::LinkChange::Action::SetPassword;
    failed.password = pbc::SecretText{std::string(kPassword)};
    PB_CHECK(!api.change(target, rules.value, id, pbc::snapshot_of(api.detail(id, true).value.row), std::move(failed)).ok());
    probe.expect_absent(kPassword, e.data(), "the password after set, replace, remove and a failed change");
    probe.expect_absent(kToken, e.data(), "the token after the changes");
    for (const auto& q : e.server.transport().requests()) pbtest::PrivacyProbe::expect_absent_in(kPassword, q.url, "a request URL");
    for (const auto& line : probe.log_lines()) PB_CHECK(line.find("Reports") == std::string::npos && line.find(kToken) == std::string::npos);
    PB_CHECK_EQ(e.server.link_hits(), size_t(0));
}
PB_TEST(remove_case_the_link_of_a_removed_share_is_found_nowhere_after_the_page_is_left) {
    Env e;
    e.server.add_folder("/Docs");
    e.server.add_folder("/Docs/Reports");
    pbtest::FakeNextcloud::Share link;
    link.path = "/Docs/Reports";
    link.token = kToken;
    const int64_t id = e.server.add_share(link);
    pbtest::PrivacyProbe probe;
    pbc::DavSession session(e.server, e.server.base_url(), e.server.login_name(), e.server.app_password());
    pbc::ShareApi api(session, e.clock, e.server.user_id());
    const auto rules = api.fetch_rules();
    PB_CHECK(rules.ok());
    PB_CHECK(api.detail(id, true).ok());
    PB_CHECK(api.remove_link(id, {"/Docs/Reports", true}, rules.value).ok());
    PB_CHECK(!api.remove_link(id, {"/Docs/Reports", true}, rules.value).ok());   // already gone
    probe.expect_absent(kToken, e.data(), "the token of a removed share");
    for (const auto& line : probe.log_lines()) PB_CHECK(line.find("Reports") == std::string::npos && line.find(kToken) == std::string::npos);
    PB_CHECK_EQ(e.server.link_hits(), size_t(0));
}

PB_TEST_MAIN()
