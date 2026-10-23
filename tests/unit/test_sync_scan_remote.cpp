// The scan of a pair's server folder (specs/003-folder-sync, research R-4): the tree as the listings tell it, sorted, bounded, never cached, and honest
// about what it could not see. Against the fake Nextcloud and against fixtures of the answers a server could give (all unverified, Principle IX).
#include <string>
#include <vector>

#include "fake_nextcloud_server.h"
#include "fake_transport.h"
#include "fixtures.h"
#include "pbc/dav_common.h"
#include "pbc/sync_scan.h"
#include "pbc/sync_types.h"
#include "test_support.h"
#include "tmp_dir.h"

PBC_FAKE_SERVER_TEST();

using pbtest::FakeNextcloud;

namespace {

struct NoCancel : pbc::Cancel {
    bool requested() const override { return false; }
};
struct FlagCancel : pbc::Cancel {
    bool flag = false;
    bool requested() const override { return flag; }
};

pbc::SyncPair pair(const std::string& remote = "/Books") {
    pbc::SyncPair p;
    p.remote_folder = remote;
    p.local_folder = "/mnt/ext1/Books";
    return p;
}

struct Env {
    FakeNextcloud server;
    pbc::DavSession session{server, server.base_url(), "demo", server.app_password()};
    pbc::Scan scan(const std::string& remote = "/Books") {
        NoCancel c;
        return pbc::scan_server(session, "demo", pair(remote), c);
    }
};

const pbc::ScanEntry* find(const pbc::Scan& s, const std::string& rel) {
    for (const auto& e : s.entries)
        if (e.rel == rel) return &e;
    return nullptr;
}

std::vector<std::string> rels(const pbc::Scan& s) {
    std::vector<std::string> out;
    for (const auto& e : s.entries) out.push_back(e.rel);
    return out;
}

// the answer of a listing taken from a fixture, as a transport would give it
struct Scripted {
    pbtest::FakeTransport transport;
    pbc::DavSession session{transport, "https://cloud.example.org", "demo", "pw"};
    explicit Scripted(const std::string& fixture_name) {
        pbc::Response r = pbtest::FakeTransport::status(207, pbtest::fixture("nextcloud/sync/" + fixture_name));
        transport.set_handler([r](const pbc::Request&) { return r; });
    }
    pbc::Scan scan() {
        NoCancel c;
        return pbc::scan_server(session, "demo", pair(), c);
    }
};

}  // namespace

PB_TEST(the_tree_is_walked_with_depth_1_per_folder_and_comes_back_sorted_with_what_the_listings_said) {
    Env e;
    e.server.add_file("/Books/b.epub", "bb", 1790000002);
    e.server.add_file("/Books/a.epub", "aaa", 1790000001);
    e.server.add_file("/Books/Novels/Dracula.txt", "dracula", 1790000003);
    e.server.add_file("/Books/Novels/Deep/x.txt", "x", 1790000004);
    e.server.set_checksums("/Books/a.epub", std::string(64, 'a'));
    const pbc::Scan s = e.scan();
    PB_CHECK(!s.incomplete);
    PB_CHECK_EQ(s.stopped_by_key, std::string(""));
    PB_CHECK_EQ(s.folders_not_listed, 0);
    PB_CHECK((rels(s) == std::vector<std::string>{"Novels", "Novels/Deep", "Novels/Deep/x.txt", "Novels/Dracula.txt", "a.epub", "b.epub"}));
    const auto* a = find(s, "a.epub");
    PB_CHECK(a != nullptr);
    PB_CHECK(a->kind == pbc::Kind::File);
    PB_CHECK_EQ(a->size, int64_t(3));
    PB_CHECK_EQ(a->etag, e.server.etag_of("/Books/a.epub"));
    PB_CHECK_EQ(a->checksum, "SHA256:" + std::string(64, 'a'));
    PB_CHECK_EQ(a->mtime.value_or(0), int64_t(1790000001));
    PB_CHECK(a->availability == pbc::Availability::Ok);
    PB_CHECK(find(s, "Novels")->kind == pbc::Kind::Folder);
    // one Depth-1 request per folder: Books, Novels, Novels/Deep - never infinity
    PB_CHECK_EQ(e.server.count("PROPFIND"), size_t(3));
    for (const std::string& d : e.server.depths()) PB_CHECK_EQ(d, std::string("1"));
    PB_CHECK_EQ(e.server.count("GET"), size_t(0));
}

PB_TEST(the_server_root_and_a_pair_folder_are_both_walked_by_the_same_rules) {
    Env e;
    e.server.add_file("/Books/a.epub", "a");
    e.server.add_file("/top.txt", "t");
    const pbc::Scan all = e.scan("/");
    PB_CHECK((rels(all) == std::vector<std::string>{"Books", "Books/a.epub", "top.txt"}));
    const pbc::Scan one = e.scan("/Books");
    PB_CHECK((rels(one) == std::vector<std::string>{"a.epub"}));
}

PB_TEST(a_scan_creates_no_cache_rows_and_no_record) {
    // the scan has no store at all: nothing it does can reach one (constitution V); here the proof is the signature plus the requests it made
    Env e;
    e.server.add_file("/Books/a.epub", "a");
    const size_t before = e.server.request_count();
    e.scan();
    PB_CHECK_EQ(e.server.request_count(), before + 1);
    PB_CHECK_EQ(e.server.count("PROPFIND"), e.server.request_count());
}

PB_TEST(a_partial_answer_a_failed_folder_and_a_timeout_make_the_scan_incomplete_and_keep_what_was_listed) {
    Env e;
    e.server.add_file("/Books/a.epub", "a");
    e.server.add_file("/Books/Bad/x.txt", "x");
    e.server.add_file("/Books/Good/y.txt", "y");
    e.server.add_file("/Books/Good/z.txt", "z");   // a "partial" answer fails every second member, so there must be two
    e.server.set_listing_failure("/Books/Bad", "timeout");
    pbc::Scan s = e.scan();
    PB_CHECK(s.incomplete);
    PB_CHECK_EQ(s.folders_not_listed, 1);
    PB_CHECK_EQ(s.stopped_by_key, std::string(""));
    PB_CHECK(find(s, "Good/y.txt") != nullptr);   // the rest of the tree is still there
    PB_CHECK(find(s, "a.epub") != nullptr);
    PB_CHECK(find(s, "Bad/x.txt") == nullptr);    // what is not listed is unknown, not claimed to be missing
    PB_CHECK(find(s, "Bad") != nullptr);          // the folder itself was listed by its parent

    e.server.set_listing_failure("/Books/Bad", "error");   // a 500 for one folder
    s = e.scan();
    PB_CHECK(s.incomplete);
    PB_CHECK_EQ(s.folders_not_listed, 1);

    e.server.set_listing_failure("/Books/Bad", "");
    e.server.set_listing_failure("/Books/Good", "partial");   // some members could not be read
    s = e.scan();
    PB_CHECK(s.incomplete);
    PB_CHECK(find(s, "Good/y.txt") != nullptr);

    e.server.set_listing_failure("/Books/Good", "truncated");   // an answer that is cut off
    s = e.scan();
    PB_CHECK(s.incomplete);
    PB_CHECK_EQ(s.folders_not_listed, 1);

    e.server.set_listing_failure("/Books/Good", "");
    s = e.scan();
    PB_CHECK(!s.incomplete);
}

PB_TEST(a_missing_pair_folder_and_a_general_problem_end_the_scan_with_their_key) {
    Env e;
    e.server.add_file("/Books/a.epub", "a");
    pbc::Scan s = e.scan("/Gone");
    PB_CHECK_EQ(s.stopped_by_key, std::string("server_folder_missing"));
    PB_CHECK(s.entries.empty());
    PB_CHECK(s.incomplete);

    FakeNextcloud wrong_password;
    wrong_password.add_file("/Books/a.epub", "a");
    pbc::DavSession bad(wrong_password, wrong_password.base_url(), "demo", "not-the-password");
    NoCancel c;
    s = pbc::scan_server(bad, "demo", pair(), c);
    PB_CHECK_EQ(s.stopped_by_key, std::string("auth_failed"));
    PB_CHECK(s.incomplete);

    e.server.set_maintenance(true);
    s = e.scan();
    PB_CHECK_EQ(s.stopped_by_key, std::string("maintenance"));

    e.server.set_maintenance(false);
    e.server.set_offline(true);
    s = e.scan();
    PB_CHECK_EQ(s.stopped_by_key, std::string("server_unreachable"));
    PB_CHECK(s.entries.empty());

    // a down server found in the middle of the tree ends it there, with what was listed before
    e.server.set_offline(false);
    e.server.add_file("/Books/Sub/b.txt", "b");
    e.server.fail_next(503, 1, "/Books/Sub");
    s = e.scan();
    PB_CHECK_EQ(s.stopped_by_key, std::string("maintenance"));
    PB_CHECK(s.incomplete);
    PB_CHECK(find(s, "a.epub") != nullptr);
}

PB_TEST(the_bounds_of_a_scan_make_it_incomplete_and_say_how_many_folders_were_left) {
    {   // 500 folders
        Env e;
        for (int i = 0; i < 520; ++i) e.server.add_file("/Books/d" + std::to_string(1000 + i) + "/f.txt", "x");
        const pbc::Scan s = e.scan();
        PB_CHECK(s.incomplete);
        PB_CHECK(s.folders_not_listed >= 20);   // 521 folders in all, 500 listed
        PB_CHECK_EQ(e.server.count("PROPFIND"), size_t(pbc::kSyncMaxFolders));
    }
    {   // depth 20
        Env e;
        std::string path = "/Books";
        for (int i = 0; i < 23; ++i) path += "/l" + std::to_string(i);
        e.server.add_file(path + "/deep.txt", "x");
        const pbc::Scan s = e.scan();
        PB_CHECK(s.incomplete);
        PB_CHECK(s.folders_not_listed >= 1);
        const std::string l19 = "l0/l1/l2/l3/l4/l5/l6/l7/l8/l9/l10/l11/l12/l13/l14/l15/l16/l17/l18/l19";
        PB_CHECK(find(s, l19) != nullptr);
        PB_CHECK(find(s, l19 + "/l20") != nullptr);          // known by its parent
        PB_CHECK(find(s, l19 + "/l20/l21") == nullptr);      // but not looked into: the bound is 20 levels
    }
    {   // 5,000 files
        Env e;
        e.server.add_tree("/Books", 51, 100, 1);   // 51 folders of 100 files: 5,100 files
        const pbc::Scan s = e.scan();
        PB_CHECK(s.incomplete);
        size_t files = 0;
        for (const auto& en : s.entries)
            if (en.kind == pbc::Kind::File) ++files;
        PB_CHECK_EQ(files, size_t(pbc::kSyncMaxFiles));
    }
    {   // exactly at the bound: complete
        Env e;
        e.server.add_tree("/Books", 50, 100, 1);
        const pbc::Scan s = e.scan();
        PB_CHECK(!s.incomplete);
    }
}

PB_TEST(entries_without_permission_or_encrypted_are_marked_unavailable_and_not_entered) {
    Env e;
    e.server.add_file("/Books/ok.epub", "x");
    e.server.add_file("/Books/secret.epub", "x");
    e.server.add_file("/Books/Vault/inside.txt", "x");
    e.server.set_unavailable("/Books/secret.epub", "no_permission");
    e.server.set_unavailable("/Books/Vault", "encrypted");
    const pbc::Scan s = e.scan();
    PB_CHECK(!s.incomplete);
    PB_CHECK(find(s, "ok.epub")->skip_reason.empty());
    PB_CHECK_EQ(find(s, "secret.epub")->skip_reason, std::string("unavailable"));
    PB_CHECK(find(s, "secret.epub")->availability == pbc::Availability::NoPermission);
    PB_CHECK_EQ(find(s, "Vault")->skip_reason, std::string("unavailable"));
    PB_CHECK(find(s, "Vault/inside.txt") == nullptr);   // an encrypted folder is not entered
    PB_CHECK_EQ(e.server.count("PROPFIND"), size_t(1));
}

PB_TEST(names_that_fail_the_rules_of_feature_001_are_dropped_and_counted) {
    Env e;
    e.server.add_file("/Books/fine.epub", "x");
    e.server.add_raw_member("/Books", "..");
    e.server.add_raw_member("/Books", "a%2Fb.epub");
    e.server.add_raw_member("/Books", "%2E%2E");
    const pbc::Scan s = e.scan();
    PB_CHECK((rels(s) == std::vector<std::string>{"fine.epub"}));
    PB_CHECK(s.dropped_names >= 3);
    PB_CHECK(s.incomplete);   // an unusable member is something the scan could not read
}

PB_TEST(a_scan_honors_cancel_between_folders) {
    Env e;
    e.server.add_file("/Books/A/a.txt", "a");
    e.server.add_file("/Books/B/b.txt", "b");
    FlagCancel cancel;
    e.server.before_next("PROPFIND", "/Books/A", [&] { cancel.flag = true; });
    const pbc::Scan s = pbc::scan_server(e.session, "demo", pair(), cancel);
    PB_CHECK_EQ(s.stopped_by_key, std::string("cancelled"));
    PB_CHECK(s.incomplete);
    PB_CHECK(e.server.count("PROPFIND") <= size_t(2));
    FlagCancel before;
    before.flag = true;
    const size_t requests = e.server.request_count();
    const pbc::Scan none = pbc::scan_server(e.session, "demo", pair(), before);
    PB_CHECK_EQ(none.stopped_by_key, std::string("cancelled"));
    PB_CHECK_EQ(e.server.request_count(), requests);   // not even the first request
}

PB_TEST(the_fixture_with_checksums_gives_the_checksum_text_the_listing_offered) {
    Scripted s("listings/books_with_checksums.unverified.xml");
    const pbc::Scan scan = s.scan();
    // the scripted transport answers every folder with the same listing; the first call is the pair folder itself
    PB_CHECK(find(scan, "a.epub") != nullptr);
    PB_CHECK_EQ(find(scan, "a.epub")->checksum, "SHA256:" + [] { std::string h; for (int i = 0; i < 32; ++i) h += "ab"; return h; }() + " SHA1:" + [] { std::string h; for (int i = 0; i < 20; ++i) h += "cd"; return h; }());
    PB_CHECK(find(scan, "b.epub")->checksum.compare(0, 4, "MD5:") == 0);
    PB_CHECK_EQ(find(scan, "c.epub")->checksum, std::string(""));
    PB_CHECK_EQ(find(scan, "a.epub")->size, int64_t(1000));
    PB_CHECK_EQ(find(scan, "a.epub")->etag, std::string("\"a-1\""));
}

PB_TEST(the_fixture_of_a_multistatus_with_a_failed_member_makes_the_scan_incomplete) {
    Scripted s("partial/books_members_failed.unverified.xml");
    const pbc::Scan scan = s.scan();
    PB_CHECK(scan.incomplete);
    PB_CHECK(find(scan, "ok.epub") != nullptr);
    PB_CHECK(find(scan, "broken.epub") != nullptr);   // listed, but its details are unknown: it cannot be used
    PB_CHECK(!find(scan, "broken.epub")->skip_reason.empty());
}

PB_TEST(the_fixture_with_hostile_names_never_yields_an_entry_for_them) {
    Scripted s("hostile/books_hostile_names.unverified.xml");
    const pbc::Scan scan = s.scan();
    PB_CHECK(find(scan, "fine.epub") != nullptr);
    PB_CHECK_EQ(scan.entries.size(), size_t(1));
    PB_CHECK(scan.dropped_names >= 3);
}

PB_TEST(the_fixture_with_unavailable_entries_marks_them_and_stays_on_one_request) {
    Scripted s("unavailable/books_no_permission_encrypted.unverified.xml");
    const pbc::Scan scan = s.scan();
    PB_CHECK(find(scan, "ok.epub")->skip_reason.empty());
    PB_CHECK_EQ(find(scan, "secret.epub")->skip_reason, std::string("unavailable"));
    PB_CHECK_EQ(find(scan, "Vault")->skip_reason, std::string("unavailable"));
    PB_CHECK_EQ(s.transport.requests().size(), size_t(1));   // the encrypted folder is not listed
    PB_CHECK(!scan.incomplete);
}

PB_TEST_MAIN()
