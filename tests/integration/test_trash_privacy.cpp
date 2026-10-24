// Names and places of the trash stay private (specs/006-trash-and-versions, spec FR-032, research R-4 and R-11, SC-006). The harness (tests/support/privacy_probe.h) captures
// the log and every file the app wrote; the full flows - list, store, probe, restore to the original and to a chosen place, delete, empty, list and restore a version - run
// with names and places that carry a marker, and the marker must not occur in the log, and not in any file once the stored list is gone. The stored list itself holds the names
// the user sees on the trash screen and nothing else holds them. It proves the app against the fake Nextcloud, never a real server (constitution Principle IV and IX).
#include <string>

#include "fake_nextcloud_server.h"
#include "pbc/credentials_file.h"
#include "pbc/dav_common.h"
#include "pbc/store.h"
#include "pbc/trash_api.h"
#include "pbc/trash_rules.h"
#include "privacy_probe.h"
#include "test_support.h"
#include "tmp_dir.h"

PBC_FAKE_SERVER_TEST();

namespace {

const std::string kMark = "Zeta";   // part of every name and place below

struct Env {
    pbtest::TmpDir dir;
    pbtest::FakeNextcloud server;
    pbc::SystemClock clock;
    std::unique_ptr<pbc::Store> store;
    pbc::DavSession session;
    pbc::TrashApi api;
    std::string data() const { return dir.file("data"); }
    static std::unique_ptr<pbc::Store> open_store(const std::string& path, pbc::Clock& clock) {
        auto r = pbc::Store::open(path, clock);
        if (!r.ok()) PB_FAIL_AT("cannot open the store");
        return std::move(r.value);
    }
    Env() : store(open_store(dir.file("data"), clock)), session(server, server.base_url(), "demo", "pw"), api(session, "demo", clock) {
        server.set_account("demo", "demo", "pw");
        server.set_server_time(1792497600);
        pbc::AccountRow a;
        a.base_url = server.base_url();
        a.login_name = "demo";
        a.user_id = "demo";
        a.state = "ok";
        PB_CHECK(store->save_account(a).ok());
    }
};

}  // namespace

PB_TEST(the_probe_sees_the_names_in_the_stored_list_so_the_test_can_fail) {
    Env e;
    pbtest::PrivacyProbe probe;
    e.server.add_trash_file("/Zeta-Private/Zeta-Diary.txt", "x", 1792497000);
    const auto l = e.api.list_trash();
    PB_CHECK(l.ok());
    PB_CHECK(e.store->replace_trash(l.value, 1792497600, 1).ok());
    PB_CHECK(pbtest::PrivacyProbe::tree_text(e.data()).find(kMark) != std::string::npos);   // the stored list is the one place that holds them
}

PB_TEST(the_flows_log_codes_only_and_leave_no_name_in_any_file_once_the_stored_list_is_gone) {
    Env e;
    pbtest::PrivacyProbe probe;
    e.server.add_folder("/Zeta-Private");
    e.server.add_folder("/Zeta-Other");
    const std::string a = e.server.add_trash_file("/Zeta-Private/Zeta-Diary.txt", "diary", 1792497000);
    const std::string b = e.server.add_trash_file("/Zeta-Private/Zeta-Letter.txt", "letter", 1792496900);
    const std::string f = e.server.add_trash_folder("/Zeta-Private/Zeta-Notes", 1792496800, {{"/Zeta-in.txt", "i"}});
    e.server.add_trash_file("/Zeta-Private/Zeta-Gone.txt", "gone", 1792496700);
    e.server.add_trash_file("/Zeta-Private/Zeta-Later.txt", "later", 1792496600);
    e.server.add_file("/Zeta-Private/Zeta-Doc.txt", "current", 1760000300);
    e.server.add_version("/Zeta-Private/Zeta-Doc.txt", "older", 1760000100);
    // list, store, probe
    auto l = e.api.list_trash();
    PB_CHECK(l.ok());
    PB_CHECK(e.store->replace_trash(l.value, 1792497600, 1).ok());
    const auto item = [&](const std::string& name) {
        for (const auto& i : l.value.items)
            if (i.trash_name == name) return i;
        PB_FAIL_AT("not listed");
    };
    PB_CHECK(e.api.probe_restore(item(a)).ok());
    // restore to the original place and to a chosen place
    PB_CHECK(e.api.restore_original(item(a)).error.ok());
    pbc::RestoreTarget t;
    t.folder = "/Zeta-Other";
    t.name = "Zeta-Moved.txt";
    PB_CHECK(e.api.restore_to(item(b), t).error.ok());
    PB_CHECK(e.api.restore_original(item(f)).error.ok());
    // a refusal and a lost answer
    const std::string refused = e.server.add_trash_file("/Zeta-Private/Zeta-Refused.txt", "r", 1792496500);
    l = e.api.list_trash();
    e.server.answer_next("MOVE", "/trashbin/", 507);
    PB_CHECK(!e.api.restore_original(item(refused)).error.ok());
    e.server.lose_next_answer("MOVE", "/trashbin/");
    PB_CHECK(e.api.restore_original(item(refused)).outcome_unknown);
    // delete one, empty the rest
    l = e.api.list_trash();
    PB_CHECK(l.ok() && l.value.items.size() >= 2);
    PB_CHECK(e.api.delete_item(l.value.items[0]).error.ok());
    l = e.api.list_trash();
    PB_CHECK(e.api.empty_trash(pbc::empty_set_of(l.value).named, nullptr).error.ok());
    // the versions of a file
    pbc::RemoteEntry file;
    file.path = "/Zeta-Private/Zeta-Doc.txt";
    file.name = "Zeta-Doc.txt";
    file.kind = pbc::Kind::File;
    file.fileid = e.server.nodes().at(file.path).fileid;
    const auto vl = e.api.list_versions(file);
    PB_CHECK(vl.ok() && !vl.value.rows.empty());
    PB_CHECK(e.api.restore_version(file, vl.value, vl.value.rows[0]).error.ok());
    // the log holds codes, statuses and counts: no name, no place
    probe.expect_absent_in(kMark, probe.log_text(), "the log of the trash and versions flows");
    PB_CHECK(!probe.log_lines().empty());   // and it did log something
    // the stored list is gone with the account's trash: no file holds a name any more
    PB_CHECK(e.store->clear_trash().ok());
    probe.expect_absent(kMark, e.data(), "the data folder after the stored list was cleared");
}

PB_TEST(signing_out_removes_the_stored_list_and_with_it_every_name) {
    Env e;
    pbtest::PrivacyProbe probe;
    e.server.add_trash_file("/Zeta-Private/Zeta-Diary.txt", "x", 1792497000);
    const auto l = e.api.list_trash();
    PB_CHECK(e.store->replace_trash(l.value, 1792497600, 1).ok());
    PB_CHECK(pbtest::PrivacyProbe::tree_text(e.data()).find(kMark) != std::string::npos);
    PB_CHECK(e.store->clear_account().ok());
    probe.expect_absent(kMark, e.data(), "the data folder after signing out");
}

PB_TEST(a_failed_list_or_a_failed_request_logs_no_name_and_no_server_text) {
    Env e;
    pbtest::PrivacyProbe probe;
    e.server.add_trash_file("/Zeta-Private/Zeta-Diary.txt", "x", 1792497000);
    e.server.set_interceptor([](const pbc::Request&) -> std::optional<pbc::Response> {
        pbc::Response r;
        r.status = 500;
        r.body = "<s:message>cannot read /Zeta-Private/Zeta-Diary.txt for demo</s:message>";
        return r;
    });
    PB_CHECK(!e.api.list_trash().ok());
    PB_CHECK(!e.api.fetch_availability().ok());
    PB_CHECK(!e.api.delete_item([] { pbc::TrashItem i; i.trash_name = "x.d1"; i.name = "x"; return i; }()).error.ok());
    probe.expect_absent_in(kMark, probe.log_text(), "the log of failed requests");
    probe.expect_absent_in("Diary", probe.log_text(), "the log of failed requests");
}

PB_TEST_MAIN()
