// A request whose answer is lost is an unknown outcome, never repeated by the app (specs/004-share-links US6, FR-020, research R-8): create, change and
// remove each. A request that never left is a plain failure. Nothing is persisted.
#include <string>

#include "fake_nextcloud_server.h"
#include "pbc/dav_common.h"
#include "pbc/share_api.h"
#include "privacy_probe.h"
#include "test_support.h"
#include "tmp_dir.h"

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
    }
    int64_t add() {
        FakeNextcloud::Share s;
        s.path = "/Docs/Reports";
        return server.add_share(s);
    }
};

const pbc::ShareTarget kFolder{"/Docs/Reports", true};

}  // namespace

PB_TEST(a_lost_create_is_unknown_not_repeated_and_a_second_create_by_the_user_makes_a_second_link) {
    Rig r;
    const auto rules = r.api.fetch_rules().value;
    r.server.lose_next_answer("POST", "/shares");
    const auto d = r.api.create(kFolder, rules, pbc::LinkRequest());
    PB_CHECK_EQ(d.error.key, std::string("share_unknown_outcome"));
    PB_CHECK(r.api.outcome_unknown());
    PB_CHECK_EQ(r.server.count("POST"), size_t(1));
    PB_CHECK_EQ(r.server.share_count(), size_t(1));
    // the user taps Create again on purpose: that is a new, deliberate action and makes a second link (the message said so)
    PB_CHECK(r.api.create(kFolder, rules, pbc::LinkRequest()).ok());
    PB_CHECK(!r.api.outcome_unknown());
    PB_CHECK_EQ(r.server.share_count(), size_t(2));
}

PB_TEST(a_lost_change_is_unknown_and_not_repeated) {
    Rig r;
    const int64_t id = r.add();
    const auto rules = r.api.fetch_rules().value;
    const auto shown = pbc::snapshot_of(r.api.detail(id, true).value.row);
    r.server.lose_next_answer("PUT", "/shares");
    pbc::LinkChange c;
    c.action = pbc::LinkChange::Action::SetExpiry;
    c.expiry = "2026-12-31";
    const auto d = r.api.change(kFolder, rules, id, shown, std::move(c));
    PB_CHECK_EQ(d.error.key, std::string("share_unknown_outcome"));
    PB_CHECK(r.api.outcome_unknown());
    PB_CHECK_EQ(r.server.count("PUT"), size_t(1));
    PB_CHECK(r.server.find_share(id)->expiration == "2026-12-31");
}

PB_TEST(a_lost_removal_is_unknown_and_not_repeated) {
    Rig r;
    const int64_t id = r.add();
    const auto rules = r.api.fetch_rules().value;
    r.server.lose_next_answer("DELETE", "/shares");
    PB_CHECK_EQ(r.api.remove_link(id, kFolder, rules).key, std::string("share_unknown_outcome"));
    PB_CHECK(r.api.outcome_unknown());
    PB_CHECK_EQ(r.server.count("DELETE"), size_t(1));
}

PB_TEST(a_request_that_never_left_is_not_unknown_and_creates_nothing) {
    Rig r;
    const auto rules = r.api.fetch_rules().value;
    r.server.drop_before_processing("POST", "/shares");
    const auto d = r.api.create(kFolder, rules, pbc::LinkRequest());
    PB_CHECK(!d.ok());
    PB_CHECK(!r.api.outcome_unknown());
    PB_CHECK_EQ(d.error.key, std::string("server_unreachable"));
    PB_CHECK_EQ(r.server.share_count(), size_t(0));
}

PB_TEST(reads_have_no_unknown_outcome_a_lost_answer_is_a_plain_failure) {
    Rig r;
    const int64_t id = r.add();
    r.server.lose_next_answer("GET", "/shares/");
    const auto d = r.api.detail(id, true);
    PB_CHECK(!d.ok());
    PB_CHECK(!r.api.outcome_unknown());
    PB_CHECK(d.error.key != "share_unknown_outcome");
}

PB_TEST(nothing_is_written_to_disk_by_a_failure_or_an_unknown_outcome) {
    pbtest::TmpDir dir;
    dir.write("data/marker", "x");
    const std::string before = pbtest::PrivacyProbe::tree_text(dir.file("data"));
    Rig r;
    const auto rules = r.api.fetch_rules().value;
    r.server.lose_next_answer("POST", "/shares");
    (void)r.api.create(kFolder, rules, pbc::LinkRequest());
    r.server.drop_before_processing("POST", "/shares");
    (void)r.api.create(kFolder, rules, pbc::LinkRequest());
    PB_CHECK_EQ(pbtest::PrivacyProbe::tree_text(dir.file("data")), before);   // the core writes no file: a record of an unknown request would need one
}

PB_TEST_MAIN()
