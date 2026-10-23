// ShareApi::remove against the fake Nextcloud (specs/004-share-links US5, contracts/share-requests.md L6): one DELETE of the share and nothing else; the
// shared item is never touched; a share that is already gone is share_gone, not a failure of data.
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
        server.add_file("/Docs/Reports/a.pdf", "alpha");
    }
    int64_t add(const std::string& owner = std::string()) {
        FakeNextcloud::Share s;
        s.path = "/Docs/Reports";
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
};

const pbc::ShareTarget kFolder{"/Docs/Reports", true};

}  // namespace

PB_TEST(one_delete_of_the_share_and_nothing_else_is_sent) {
    Rig r;
    const int64_t id = r.add();
    const auto rules = r.rules();
    const auto before = r.server.nodes();
    PB_CHECK(r.api.remove_link(id, kFolder, rules).ok());
    PB_CHECK_EQ(r.server.request_count(), size_t(1));
    PB_CHECK_EQ(r.server.share_log().size(), size_t(1));
    PB_CHECK_EQ(r.server.share_log()[0].method, std::string("DELETE"));
    PB_CHECK_EQ(r.server.share_log()[0].route, "/shares/" + std::to_string(id));
    PB_CHECK_EQ(r.server.share_log()[0].query, std::string("format=json"));
    PB_CHECK(r.server.share_log()[0].body.empty());
    PB_CHECK_EQ(r.server.share_count(), size_t(0));
    // the shared folder and the file in it are exactly as they were
    PB_CHECK_EQ(r.server.nodes().size(), before.size());
    for (const auto& [path, node] : before) {
        PB_CHECK(r.server.nodes().count(path) == 1);
        PB_CHECK_EQ(r.server.nodes().at(path).etag, node.etag);
        PB_CHECK_EQ(r.server.nodes().at(path).content, node.content);
    }
    PB_CHECK(!r.server.trash_has("/Docs/Reports") && !r.server.trash_has("/Docs/Reports/a.pdf"));
    PB_CHECK_EQ(r.server.count("PROPFIND") + r.server.count("PUT") + r.server.count("MOVE") + r.server.count("COPY"), size_t(0));
}

PB_TEST(a_share_that_is_already_gone_is_share_gone) {
    Rig r;
    const auto rules = r.rules();
    const auto e = r.api.remove_link(424242, kFolder, rules);
    PB_CHECK_EQ(e.key, std::string("share_gone"));
    PB_CHECK_EQ(r.api.message().key, std::string("share_gone"));
    const int64_t id = r.add();
    r.server.remove_share(id);
    PB_CHECK_EQ(r.api.remove_link(id, kFolder, rules).key, std::string("share_gone"));
}

PB_TEST(a_share_of_someone_else_is_not_changeable_and_stays_in_the_list) {
    Rig r;
    const int64_t id = r.add("carol");
    const auto rules = r.rules();
    // the caller knows the share is not the user's: nothing is sent at all
    PB_CHECK_EQ(r.api.remove_link(id, kFolder, rules, false).key, std::string("share_not_changeable"));
    PB_CHECK_EQ(r.server.request_count(), size_t(0));
    // and when it is asked anyway the server's 403 is explained the same way
    PB_CHECK_EQ(r.api.remove_link(id, kFolder, rules, true).key, std::string("share_not_changeable"));
    PB_CHECK_EQ(r.server.share_count(), size_t(1));
}

PB_TEST(other_refusals_keep_their_keys_and_the_link_stays) {
    Rig r;
    const int64_t id = r.add();
    const auto rules = r.rules();
    r.server.answer_next("DELETE", "/shares", 500);
    const auto e = r.api.remove_link(id, kFolder, rules);
    PB_CHECK_EQ(e.key, std::string("share_refused"));
    PB_CHECK_EQ(r.api.message().code, std::string("E500"));
    PB_CHECK_EQ(r.server.share_count(), size_t(1));
    r.server.answer_next("DELETE", "/shares", 429, 1, "30");
    PB_CHECK_EQ(r.api.remove_link(id, kFolder, rules).key, std::string("rate_limited"));
    pbc::DavSession bad(r.server, r.server.base_url(), "demo", "wrong");
    pbc::ShareApi api(bad, r.clock, "demo");
    PB_CHECK_EQ(api.remove_link(id, kFolder, rules).key, std::string("auth_failed"));
    r.server.set_offline(true);
    PB_CHECK_EQ(r.api.remove_link(id, kFolder, rules).key, std::string("server_unreachable"));
    PB_CHECK_EQ(r.server.share_count(), size_t(1));
}

PB_TEST(a_lost_answer_is_an_unknown_outcome_and_the_delete_is_not_repeated) {
    Rig r;
    const int64_t id = r.add();
    const auto rules = r.rules();
    r.server.lose_next_answer("DELETE", "/shares");
    const auto e = r.api.remove_link(id, kFolder, rules);
    PB_CHECK_EQ(e.key, std::string("share_unknown_outcome"));
    PB_CHECK(r.api.outcome_unknown());
    PB_CHECK_EQ(r.server.count("DELETE"), size_t(1));
    PB_CHECK_EQ(r.server.share_count(), size_t(0));   // the server did it
}

PB_TEST(a_request_that_never_left_is_a_plain_failure) {
    Rig r;
    const int64_t id = r.add();
    const auto rules = r.rules();
    r.server.drop_before_processing("DELETE", "/shares");
    PB_CHECK(!r.api.remove_link(id, kFolder, rules).ok());
    PB_CHECK(!r.api.outcome_unknown());
    PB_CHECK_EQ(r.server.share_count(), size_t(1));
}

PB_TEST_MAIN()
