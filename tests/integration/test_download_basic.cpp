// A download from the fake Nextcloud (spec US3, FR-016..FR-019, FR-023; constitution Principle III): the file appears under its final name
// only when it is complete, nothing is left behind by a failure, and the journal row exists before the first request.
#include <string>

#include "download_env.h"

PBC_FAKE_SERVER_TEST();

using pbc::JobState;
using pbtest::DlEnv;

namespace {
std::string pattern(size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 31 + 7) & 0xff);
    return s;
}
}  // namespace

PB_TEST(a_complete_download_is_renamed_into_place_recorded_and_leaves_nothing_else) {
    DlEnv e;
    const std::string content = pattern(200 * 1024);
    e.server.add_file("/Books/story.epub", content, 1790000000);
    e.server.clear_log();
    const auto out = e.run("/Books/story.epub");
    PB_CHECK(out.state == JobState::Done);
    PB_CHECK(out.error_key.empty());
    PB_CHECK_EQ(out.local_path, std::string("Books/story.epub"));
    PB_CHECK(e.read("Books/story.epub") == content);
    PB_CHECK_EQ(e.tree().size(), size_t(1));   // no .part file, nothing else
    PB_CHECK(e.store->journal().empty());
    const auto lf = e.store->local_file("/Books/story.epub");
    PB_CHECK(lf.has_value());
    PB_CHECK_EQ(lf->local_path, std::string("Books/story.epub"));
    PB_CHECK_EQ(lf->size, static_cast<int64_t>(content.size()));
    PB_CHECK_EQ(lf->remote_etag, e.server.etag_of("/Books/story.epub"));
    PB_CHECK_EQ(*lf->remote_size, static_cast<int64_t>(content.size()));
    struct stat st;
    PB_CHECK(::stat(e.abs("Books/story.epub").c_str(), &st) == 0);
    PB_CHECK_EQ(lf->mtime, static_cast<int64_t>(st.st_mtime));   // taken from the file after the rename
    PB_CHECK(!e.progress.empty());
    PB_CHECK(e.progress.back().state == JobState::Done);
    PB_CHECK(e.server.depths().empty());
}

PB_TEST(the_journal_row_exists_before_the_first_request) {
    DlEnv e;
    e.server.add_file("/a.bin", pattern(1000));
    size_t rows_at_request = 99;
    e.server.set_interceptor([&](const pbc::Request& r) -> std::optional<pbc::Response> {
        if (r.method == "GET" && r.url.find("/a.bin") != std::string::npos) rows_at_request = e.store->journal().size();
        return std::nullopt;
    });
    PB_CHECK(e.run("/a.bin").state == JobState::Done);
    PB_CHECK_EQ(rows_at_request, size_t(1));
}

PB_TEST(the_file_is_asked_for_as_stored_with_credentials_in_the_transport_only) {
    DlEnv e;
    e.server.add_file("/a.bin", pattern(1000));
    e.server.clear_log();
    PB_CHECK(e.run("/a.bin").state == JobState::Done);
    const auto& sent = e.server.transport().requests();
    PB_CHECK(!sent.empty());
    PB_CHECK_EQ(sent[0].method, std::string("GET"));
    PB_CHECK(sent[0].identity_encoding);
    PB_CHECK(sent[0].save_append);
    PB_CHECK_EQ(sent[0].buffer_size, 65536L);
    PB_CHECK(!sent[0].username.empty());
    PB_CHECK(sent[0].url.find("demo:") == std::string::npos);
    PB_CHECK(pbc::header_value(sent[0].headers, "Range").empty());   // a fresh download asks for everything
}

PB_TEST(a_size_that_does_not_match_what_the_server_announced_is_refused_and_nothing_stays) {
    DlEnv e;
    e.server.add_file("/a.bin", pattern(1000));
    e.server.set_interceptor([](const pbc::Request& r) -> std::optional<pbc::Response> {
        if (r.method != "GET") return std::nullopt;
        pbc::Response lie;
        lie.status = 200;
        lie.body = std::string(60, 'x');
        lie.headers = {{"Content-Length", "100"}, {"ETag", "\"e\""}};
        return lie;
    });
    const auto out = e.run("/a.bin");
    PB_CHECK(out.state == JobState::Failed);
    PB_CHECK_EQ(out.error_key, std::string("size_mismatch"));
    PB_CHECK(e.tree().empty());
    PB_CHECK(e.store->journal().empty());
    PB_CHECK(!e.store->local_file("/a.bin").has_value());
}

PB_TEST(the_checksum_the_server_lists_is_checked_and_a_mismatch_leaves_no_file) {
    DlEnv e;
    e.server.add_file("/abc.txt", "abc");
    pbc::DownloadRequest good = e.request("/abc.txt");
    good.checksum = "SHA1:a9993e364706816aba3e25717850c26c9cd0d89d MD5:ffffffffffffffffffffffffffffffff";   // the SHA-1 is right; the weaker MD5 is not looked at
    pbtest::NoCancel c;
    PB_CHECK(e.dl->run(good, c, [](const pbc::JobProgress&) {}).state == JobState::Done);

    pbc::DownloadRequest bad = e.request("/abc.txt");
    bad.checksum = "SHA256:0000000000000000000000000000000000000000000000000000000000000000";
    e.server.add_file("/other.txt", "abc");
    bad.remote_path = "/other.txt";
    const auto out = e.dl->run(bad, c, [](const pbc::JobProgress&) {});
    PB_CHECK(out.state == JobState::Failed);
    PB_CHECK_EQ(out.error_key, std::string("checksum_mismatch"));
    PB_CHECK(!e.exists("other.txt"));
    PB_CHECK(!e.any_part_file());
    PB_CHECK(e.store->journal().empty());
}

PB_TEST(every_final_error_of_the_server_ends_the_job_with_its_key_and_leaves_nothing) {
    struct Case { long status; const char* key; };
    for (const Case c : {Case{404, "not_found"}, Case{403, "forbidden"}, Case{401, "auth_failed"}, Case{423, "locked"}, Case{429, "rate_limited"}, Case{503, "maintenance"}, Case{500, "server_error"}}) {
        DlEnv e;
        e.server.add_file("/a.bin", pattern(1000));
        e.server.fail_next(c.status, 1, "/a.bin");
        const auto out = e.run("/a.bin");
        PB_CHECK(out.state == JobState::Failed);
        PB_CHECK_EQ(out.error_key, std::string(c.key));
        PB_CHECK(e.tree().empty());
        PB_CHECK(e.store->journal().empty());
        PB_CHECK_EQ(e.waits, 0);   // a final answer is not waited for
    }
}

PB_TEST(a_file_that_vanished_on_the_server_marks_the_recorded_copy_as_missing_and_keeps_it) {
    DlEnv e;
    e.server.add_file("/a.bin", pattern(1000));
    PB_CHECK(e.run("/a.bin").state == JobState::Done);
    e.server.remove("/a.bin");
    pbc::DownloadRequest r;
    r.remote_path = "/a.bin";
    pbtest::NoCancel c;
    PB_CHECK_EQ(e.dl->run(r, c, [](const pbc::JobProgress&) {}).error_key, std::string("not_found"));
    PB_CHECK(e.store->local_file("/a.bin")->server_missing);
    PB_CHECK(e.exists("a.bin"));   // the copy on the reader stays
}

PB_TEST(a_full_card_and_an_io_error_end_the_job_cleanly) {
    {
        DlEnv e;
        e.server.add_file("/a.bin", pattern(5000));
        e.server.transport().simulate_storage_full = true;
        const auto out = e.run("/a.bin");
        PB_CHECK(out.state == JobState::Failed);
        PB_CHECK_EQ(out.error_key, std::string("storage_full"));
        PB_CHECK(e.tree().empty());
        PB_CHECK(e.store->journal().empty());
    }
    {
        DlEnv e;
        e.server.add_file("/a.bin", pattern(5000));
        e.server.transport().simulate_io_error = true;   // the card was pulled
        const auto out = e.run("/a.bin");
        PB_CHECK(out.state == JobState::Failed);
        PB_CHECK(out.error_key == "write_failed" || out.error_key == "storage_full" || out.error_key == "storage_readonly");
        PB_CHECK(e.tree().empty());
        PB_CHECK(e.store->journal().empty());
    }
}

PB_TEST(a_cancel_in_the_middle_removes_the_partial_file_and_the_journal_row) {
    DlEnv e;
    e.server.add_file("/big.bin", pattern(2 * 1024 * 1024));
    struct CancelAfterPolls : pbc::Cancel {
        mutable int polls = 0;
        bool requested() const override { return ++polls > 8; }
    } cancel;
    const auto out = e.run("/big.bin", cancel);
    PB_CHECK(out.state == JobState::Cancelled);
    PB_CHECK_EQ(out.error_key, std::string("cancelled"));
    PB_CHECK(e.tree().empty());
    PB_CHECK(e.store->journal().empty());
    PB_CHECK(!e.store->local_file("/big.bin").has_value());
    PB_CHECK(e.progress.back().state == JobState::Cancelled);
}

PB_TEST(a_file_that_is_too_big_or_does_not_fit_is_refused_before_any_request) {
    DlEnv e;
    e.server.add_file("/a.bin", pattern(1000));
    pbc::DownloadRequest r = e.request("/a.bin");
    pbtest::NoCancel c;
    r.size = 5ll << 30;   // the listing says 5 GiB
    auto out = e.dl->run(r, c, [](const pbc::JobProgress&) {});
    PB_CHECK(out.state == JobState::Failed);
    PB_CHECK_EQ(out.error_key, std::string("too_large_for_storage"));
    r.size = 100ll << 20;
    e.storage.free = 20ll << 20;
    out = e.dl->run(r, c, [](const pbc::JobProgress&) {});
    PB_CHECK_EQ(out.error_key, std::string("not_enough_space"));
    e.storage.free = 1ll << 40;
    e.storage.read_only = true;
    out = e.dl->run(r, c, [](const pbc::JobProgress&) {});
    PB_CHECK_EQ(out.error_key, std::string("storage_readonly"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(e.store->journal().empty());
    PB_CHECK(e.tree().empty());
}

PB_TEST(the_name_on_the_card_is_a_safe_version_of_the_name_on_the_server) {
    DlEnv e;
    e.server.add_file("/Books/a:b*c.epub", pattern(100));
    e.server.add_file("/CON/nul.txt", pattern(100));
    PB_CHECK_EQ(e.run("/Books/a:b*c.epub").local_path, std::string("Books/a_b_c.epub"));
    PB_CHECK_EQ(e.run("/CON/nul.txt").local_path, std::string("_CON/_nul.txt"));
    PB_CHECK_EQ(e.tree().size(), size_t(2));
}

PB_TEST_MAIN()
