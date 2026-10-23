// The downloads of a folder pair (specs/003-folder-sync, research R-7): a file goes below the pair's reader folder at the path of its remote name
// relative to the pair's server folder, is recorded in sync_files together with the end of its journal row, is requested only in the planned
// version when asked to, and never overwrites a reader file that is not exactly the recorded copy. Without a target root nothing changes (001).
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <string>

#include "download_env.h"
#include "pbc/sync_rules.h"
#include "pbc/sync_types.h"

PBC_FAKE_SERVER_TEST();

using pbc::ConflictPolicy;
using pbc::JobState;
using pbtest::DlEnv;

namespace {

std::string pattern(size_t n, int seed = 7) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 31 + static_cast<size_t>(seed)) & 0xff);
    return s;
}

struct PairEnv : DlEnv {
    std::string reader;   // the pair's reader folder
    int64_t pair = 0;
    PairEnv() {
        reader = dir.file("Reader");
        ::mkdir(reader.c_str(), 0700);
        pbc::SyncPair p = pbc::default_pair(server.base_url(), "demo");
        p.remote_folder = "/Books";
        p.local_folder = reader;
        auto r = store->sync_pair_add(p);
        if (!r.ok()) PB_FAIL_AT("cannot add the pair: " + r.error.key);
        pair = r.value;
    }
    pbc::DownloadRequest pair_request(const std::string& remote_path) {
        pbc::DownloadRequest r = request(remote_path);
        r.pair_id = pair;
        r.target_root = reader;
        r.strip_prefix = "/Books";
        return r;
    }
    pbc::DownloadOutcome go(pbc::DownloadRequest r) {
        NoCancelHolder c;
        return dl->run(r, c.c, [](const pbc::JobProgress&) {});
    }
    struct NoCancelHolder {
        pbtest::NoCancel c;
    };
    std::string rd(const std::string& rel) const {
        std::ifstream f(reader + "/" + rel, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    bool has(const std::string& rel) const {
        struct stat st;
        return ::lstat((reader + "/" + rel).c_str(), &st) == 0;
    }
    bool part_file_below(const std::string& folder) const {
        std::vector<std::string> out;
        pbtest::list_tree(folder, "", &out);
        for (const auto& f : out)
            if (f.find(".pbc-") != std::string::npos) return true;
        return false;
    }
};

}  // namespace

PB_TEST(a_pair_download_goes_below_the_reader_folder_at_the_path_relative_to_the_server_folder) {
    PairEnv e;
    e.server.add_file("/Books/Novels/Dracula.txt", pattern(5000, 3));
    auto out = e.go(e.pair_request("/Books/Novels/Dracula.txt"));
    PB_CHECK(out.state == JobState::Done);
    PB_CHECK_EQ(out.local_path, std::string("Novels/Dracula.txt"));
    PB_CHECK(e.rd("Novels/Dracula.txt") == pattern(5000, 3));
    PB_CHECK(!e.exists("Novels/Dracula.txt"));      // not below the download root
    PB_CHECK(!e.exists("Books/Novels/Dracula.txt"));
    PB_CHECK(!e.store->local_file("/Books/Novels/Dracula.txt").has_value());   // not a 001 copy
    PB_CHECK(e.store->journal().empty());                                       // the journal row ended with the record
    const auto rec = e.store->sync_record(e.pair, "Novels/Dracula.txt");
    PB_CHECK(rec.has_value());
    PB_CHECK_EQ(rec->local_rel, std::string("Novels/Dracula.txt"));
    PB_CHECK_EQ(rec->origin, std::string("download"));
    PB_CHECK_EQ(rec->server_etag, e.server.etag_of("/Books/Novels/Dracula.txt"));
    PB_CHECK_EQ(rec->server_size.value_or(-1), int64_t(5000));
    PB_CHECK_EQ(rec->local_size.value_or(-1), int64_t(5000));
    PB_CHECK(rec->local_mtime.has_value());
    PB_CHECK(!e.part_file_below(e.reader));
    // the same name again replaces the recorded copy in place (the server changed)
    e.server.touch_file("/Books/Novels/Dracula.txt", pattern(6000, 4), 1790009999);
    out = e.go(e.pair_request("/Books/Novels/Dracula.txt"));
    PB_CHECK_EQ(out.error_key, std::string(""));
    PB_CHECK(out.state == JobState::Done);
    PB_CHECK(e.rd("Novels/Dracula.txt") == pattern(6000, 4));
    PB_CHECK_EQ(e.store->sync_records(e.pair).size(), size_t(1));
    PB_CHECK_EQ(e.store->sync_record(e.pair, "Novels/Dracula.txt")->server_size.value_or(-1), int64_t(6000));
}

PB_TEST(without_a_target_root_the_download_is_the_one_of_feature_001) {
    PairEnv e;
    e.server.add_file("/Books/a.txt", pattern(1000));
    const auto out = e.run("/Books/a.txt");
    PB_CHECK(out.state == JobState::Done);
    PB_CHECK(e.exists("Books/a.txt"));
    PB_CHECK(e.store->local_file("/Books/a.txt").has_value());
    PB_CHECK(e.store->sync_records(e.pair).empty());
    PB_CHECK(!e.has("a.txt"));
}

PB_TEST(a_name_below_the_server_folder_is_required_and_the_reader_folder_must_be_a_real_folder) {
    PairEnv e;
    e.server.add_file("/Other/x.txt", "x");
    auto r = e.pair_request("/Other/x.txt");
    PB_CHECK_EQ(e.go(r).error_key, std::string("protocol_error"));   // not below /Books
    e.server.add_file("/Books/a.txt", "a");
    r = e.pair_request("/Books/a.txt");
    r.target_root = e.dir.file("nowhere");
    PB_CHECK_EQ(e.go(r).error_key, std::string("path_unsafe"));
    PB_CHECK(::symlink(e.reader.c_str(), e.dir.file("link").c_str()) == 0);
    r.target_root = e.dir.file("link");
    PB_CHECK_EQ(e.go(r).error_key, std::string("path_unsafe"));       // a link is not followed
    r.target_root = "relative/dir";
    PB_CHECK_EQ(e.go(r).error_key, std::string("path_unsafe"));
}

PB_TEST(a_reader_file_that_is_not_the_recorded_copy_is_never_overwritten) {
    PairEnv e;
    // an unrecorded file of the same name: the download gets another name and the file is untouched
    ::mkdir((e.reader + "/Novels").c_str(), 0700);
    std::ofstream(e.reader + "/Novels/Dracula.txt") << "mine";
    e.server.add_file("/Books/Novels/Dracula.txt", pattern(500));
    auto out = e.go(e.pair_request("/Books/Novels/Dracula.txt"));
    PB_CHECK(out.state == JobState::Done);
    PB_CHECK_EQ(out.local_path, std::string("Novels/Dracula (2).txt"));
    PB_CHECK_EQ(e.rd("Novels/Dracula.txt"), std::string("mine"));
    PB_CHECK_EQ(e.store->sync_record(e.pair, "Novels/Dracula.txt")->local_rel, std::string("Novels/Dracula (2).txt"));

    // a recorded file that the user changed afterwards: the download stops before the first request
    e.server.add_file("/Books/b.txt", pattern(800));
    PB_CHECK(e.go(e.pair_request("/Books/b.txt")).state == JobState::Done);
    std::ofstream(e.reader + "/b.txt", std::ios::trunc) << "edited by the user, longer than before";
    e.server.touch_file("/Books/b.txt", pattern(900, 2), 1790009999);
    e.server.clear_log();
    out = e.go(e.pair_request("/Books/b.txt"));
    PB_CHECK(out.state == JobState::Blocked);
    PB_CHECK_EQ(out.error_key, std::string("local_changed"));
    PB_CHECK_EQ(e.rd("b.txt"), std::string("edited by the user, longer than before"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(e.store->journal().empty());
}

PB_TEST(a_planned_version_is_required_with_if_match_and_a_changed_file_writes_nothing) {
    PairEnv e;
    e.server.add_file("/Books/a.txt", pattern(1000));
    auto r = e.pair_request("/Books/a.txt");
    r.strict_version = true;
    PB_CHECK(e.go(r).state == JobState::Done);
    bool saw_if_match = false;
    for (const auto& q : e.server.transport().requests())
        if (q.method == "GET" && pbc::header_value(q.headers, "If-Match") == e.server.etag_of("/Books/a.txt")) saw_if_match = true;
    PB_CHECK(saw_if_match);

    // the file changed between the comparison and the request: a 412, nothing is written
    e.server.add_file("/Books/c.txt", pattern(1000));
    r = e.pair_request("/Books/c.txt");   // the ETag of the planned version
    r.strict_version = true;
    e.server.touch_file("/Books/c.txt", pattern(1200, 5), 1790009999);
    auto out = e.go(r);
    PB_CHECK(out.state == JobState::Failed);
    PB_CHECK_EQ(out.error_key, std::string("changed_during_sync"));
    PB_CHECK(!e.has("c.txt"));
    PB_CHECK(!e.part_file_below(e.reader));
    PB_CHECK(!e.store->sync_record(e.pair, "c.txt").has_value());
    PB_CHECK(e.store->journal().empty());

    // a server that ignores If-Match: the answer's own ETag gives it away
    e.server.set_ignore_get_if_match(true);
    e.server.add_file("/Books/d.txt", pattern(1000));
    r = e.pair_request("/Books/d.txt");
    r.strict_version = true;
    e.server.touch_file("/Books/d.txt", pattern(1100, 6), 1790009999);
    out = e.go(r);
    PB_CHECK_EQ(out.error_key, std::string("changed_during_sync"));
    PB_CHECK(!e.has("d.txt"));
    PB_CHECK(!e.part_file_below(e.reader));
    PB_CHECK(e.store->journal().empty());

    // without the flag the new version is simply taken
    r = e.pair_request("/Books/d.txt");
    PB_CHECK(e.go(r).state == JobState::Done);
    PB_CHECK(e.rd("d.txt") == pattern(1100, 6));
}

PB_TEST(keep_both_writes_exactly_the_planned_name_beside_the_original_and_records_the_copy_as_one_sided) {
    PairEnv e;
    e.server.add_file("/Books/Novels/a.txt", pattern(700, 1));
    PB_CHECK(e.go(e.pair_request("/Books/Novels/a.txt")).state == JobState::Done);
    e.server.touch_file("/Books/Novels/a.txt", pattern(900, 2), 1790009999);
    auto r = e.pair_request("/Books/Novels/a.txt");
    r.policy = ConflictPolicy::KeepBoth;
    r.conflict_name = "a (server copy 2026-10-22).txt";
    r.copy_of = "Novels/a.txt";
    const auto before = e.store->sync_record(e.pair, "Novels/a.txt");
    const auto out = e.go(r);
    PB_CHECK(out.state == JobState::Done);
    PB_CHECK_EQ(out.local_path, std::string("Novels/a (server copy 2026-10-22).txt"));
    PB_CHECK(e.rd("Novels/a (server copy 2026-10-22).txt") == pattern(900, 2));
    PB_CHECK(e.rd("Novels/a.txt") == pattern(700, 1));                    // the original is untouched
    PB_CHECK_EQ(e.store->sync_records(e.pair).size(), size_t(2));         // the original's record is not touched; the copy has one of its own
    PB_CHECK_EQ(e.store->sync_record(e.pair, "Novels/a.txt")->server_etag, before->server_etag);
    PB_CHECK(!e.store->sync_record(e.pair, "Novels/a.txt")->diverged);    // marking the original is the runner's step, after every copy exists
    const auto copy = e.store->sync_record(e.pair, "Novels/a (server copy 2026-10-22).txt");
    PB_CHECK(copy.has_value());
    PB_CHECK_EQ(copy->origin, std::string("conflict_copy"));
    PB_CHECK_EQ(copy->one_sided, std::string("reader"));                  // it lives on the reader only and is never sent back
    PB_CHECK_EQ(copy->copy_of, std::string("Novels/a.txt"));
    PB_CHECK_EQ(copy->local_rel, std::string("Novels/a (server copy 2026-10-22).txt"));
    PB_CHECK(copy->server_etag.empty());
    PB_CHECK_EQ(copy->local_size.value_or(-1), int64_t(900));
    PB_CHECK(e.store->journal().empty());
    // the same name twice, a hostile name and no name are refused before anything is requested
    e.server.clear_log();
    PB_CHECK_EQ(e.go(r).error_key, std::string("name_exists"));
    for (const char* bad : {"", "../x", "a/b", ".hidden", "CON"}) {
        r.conflict_name = bad;
        PB_CHECK_EQ(e.go(r).error_key, std::string("name_forbidden"));
    }
    r.conflict_name = "b (server copy 2026-10-22).txt";
    r.copy_of.clear();
    PB_CHECK_EQ(e.go(r).error_key, std::string("protocol_error"));       // a copy must say which original it is for
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(a_verified_sha256_of_the_server_is_kept_in_the_record_and_nothing_is_hashed_for_it_otherwise) {
    PairEnv e;
    const std::string content = pattern(3000, 9);
    e.server.add_file("/Books/a.txt", content);
    e.server.list_checksums(true);
    auto r = e.pair_request("/Books/a.txt");
    r.checksum = "SHA256:" + std::string();   // filled from the server's own listing below
    // the listing text the app would pass
    {
        pbc::Request pr;
        pr.method = "PROPFIND";
        pr.url = e.server.base_url() + "/remote.php/dav/files/demo/Books";
        pr.username = e.server.login_name();
        pr.password = e.server.app_password();
        pr.headers = {{"Depth", "1"}};
        pr.body = "<d:propfind xmlns:d=\"DAV:\"/>";
        const auto resp = e.server.perform(pr);
        const size_t at = resp.body.find("SHA256:");
        PB_CHECK(at != std::string::npos);
        r.checksum = resp.body.substr(at, 7 + 64);
    }
    PB_CHECK(e.go(r).state == JobState::Done);
    const auto rec = e.store->sync_record(e.pair, "a.txt");
    PB_CHECK_EQ(rec->local_sha256.size(), size_t(64));
    PB_CHECK_EQ(rec->server_checksum, r.checksum);
    // a server that lists no checksum: nothing to verify against and nothing stored
    e.server.add_file("/Books/b.txt", pattern(100));
    auto r2 = e.pair_request("/Books/b.txt");
    PB_CHECK(e.go(r2).state == JobState::Done);
    PB_CHECK_EQ(e.store->sync_record(e.pair, "b.txt")->local_sha256, std::string(""));
    PB_CHECK_EQ(e.store->sync_record(e.pair, "b.txt")->server_checksum, std::string(""));
}

PB_TEST(a_cut_pair_download_resumes_into_the_pair_folder_and_is_recorded) {
    PairEnv e;
    const std::string content = pattern(100 * 1024, 5);
    e.server.add_file("/Books/Big/a.bin", content);
    e.server.transport().drop_next_download_after(30000);
    e.wifi = false;   // nothing returns: after the waits the job is paused
    auto out = e.go(e.pair_request("/Books/Big/a.bin"));
    PB_CHECK(out.state == JobState::Paused);
    const auto rows = e.store->journal();
    PB_CHECK_EQ(rows.size(), size_t(1));
    PB_CHECK_EQ(rows[0].pair_id, e.pair);
    PB_CHECK_EQ(rows[0].target_root, e.reader);
    PB_CHECK(e.part_file_below(e.reader + "/Big"));
    PB_CHECK(!e.store->sync_record(e.pair, "Big/a.bin").has_value());
    e.wifi = true;
    pbtest::NoCancel cancel;
    const auto done = e.dl->resume(out.job_id, cancel, [](const pbc::JobProgress&) {});
    PB_CHECK(done.state == JobState::Done);
    PB_CHECK(e.rd("Big/a.bin") == content);
    PB_CHECK(!e.part_file_below(e.reader));
    PB_CHECK(e.store->journal().empty());
    PB_CHECK_EQ(e.store->sync_record(e.pair, "Big/a.bin")->server_size.value_or(-1), int64_t(100 * 1024));
}

PB_TEST(recovery_at_start_handles_the_jobs_of_a_pair_like_those_of_feature_001) {
    PairEnv e;
    e.server.add_file("/Books/a.bin", pattern(100 * 1024, 5));
    e.server.transport().drop_next_download_after(30000);
    e.wifi = false;
    PB_CHECK(e.go(e.pair_request("/Books/a.bin")).state == JobState::Paused);
    // an orphan temporary file in the pair folder (no journal row knows it) goes away; the one with a journal row stays
    std::ofstream(e.reader + "/.pbc-777.part") << "orphan";
    e.dl->recover();
    PB_CHECK(!e.has(".pbc-777.part"));
    PB_CHECK_EQ(e.store->journal().size(), size_t(1));
    PB_CHECK(e.part_file_below(e.reader));
    // the temporary file is gone and the journal row stays: it is dropped; a placed file without a record is left for the next comparison
    std::vector<std::string> all;
    pbtest::list_tree(e.reader, "", &all);
    for (const auto& f : all) ::unlink((e.reader + "/" + f).c_str());
    std::ofstream(e.reader + "/a.bin") << "placed but never recorded";
    e.dl->recover();
    PB_CHECK(e.store->journal().empty());
    PB_CHECK(!e.store->sync_record(e.pair, "a.bin").has_value());
    PB_CHECK_EQ(e.rd("a.bin"), std::string("placed but never recorded"));
}

PB_TEST_MAIN()
