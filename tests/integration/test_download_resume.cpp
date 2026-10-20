// A download that is cut off and continued (spec FR-018, SC-006; research R-8 points 4 and 7): it resumes with a ranged request only when
// that is safe, otherwise it starts over; it waits for the connection without sending anything; after ten minutes or five tries it is
// paused and can be continued, also after a restart. Whatever happens the finished file equals the server's.
#include <string>

#include "download_env.h"

PBC_FAKE_SERVER_TEST();

using pbc::JobState;
using pbtest::DlEnv;

namespace {

std::string pattern(size_t n, int seed = 7) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 31 + static_cast<size_t>(seed)) & 0xff);
    return s;
}

std::vector<pbc::Request> gets(DlEnv& e) {
    std::vector<pbc::Request> out;
    for (const auto& r : e.server.transport().requests())
        if (r.method == "GET") out.push_back(r);
    return out;
}

}  // namespace

PB_TEST(after_a_drop_at_40_percent_the_download_continues_with_a_ranged_request_and_ends_identical) {
    DlEnv e;
    const std::string content = pattern(100 * 1024);
    e.server.add_file("/a.bin", content);
    e.server.transport().drop_next_download_after(40000);
    const auto out = e.run("/a.bin");
    PB_CHECK(out.state == JobState::Done);
    PB_CHECK(e.read("a.bin") == content);
    PB_CHECK(e.waits >= 1);
    const auto g = gets(e);
    PB_CHECK_EQ(g.size(), size_t(2));
    PB_CHECK_EQ(pbc::header_value(g[1].headers, "Range"), std::string("bytes=40000-"));
    PB_CHECK_EQ(pbc::header_value(g[1].headers, "If-Range"), e.server.etag_of("/a.bin"));
    PB_CHECK_EQ(e.store->local_file("/a.bin")->remote_etag, e.server.etag_of("/a.bin"));
    PB_CHECK(e.tree().size() == 1 && e.store->journal().empty());
}

PB_TEST(while_the_connection_is_down_nothing_is_sent_only_the_wifi_state_is_looked_at) {
    DlEnv e;
    e.server.add_file("/a.bin", pattern(100 * 1024));
    e.server.transport().drop_next_download_after(10000);
    e.wifi = false;
    e.on_wait = [&](int n) {
        if (n == 3) e.wifi = true;   // the connection returns after three waits
    };
    const auto out = e.run("/a.bin");
    PB_CHECK(out.state == JobState::Done);
    PB_CHECK_EQ(e.waits, 3);
    PB_CHECK_EQ(gets(e).size(), size_t(2));   // the first try and one retry; the waits sent nothing
}

PB_TEST(a_file_that_changed_in_the_meantime_is_downloaded_again_from_the_start_and_recorded_with_what_arrived) {
    DlEnv e;
    e.server.add_file("/a.bin", pattern(100 * 1024, 1));
    e.server.transport().drop_next_download_after(40000);
    const std::string newer = pattern(120 * 1024, 2);   // another size as well
    e.on_wait = [&](int) { e.server.set_file("/a.bin", newer, 1790009999); };
    const auto out = e.run("/a.bin");
    PB_CHECK(out.state == JobState::Done);
    PB_CHECK(e.read("a.bin") == newer);   // never the old head glued to the new tail
    const auto lf = e.store->local_file("/a.bin");
    PB_CHECK_EQ(lf->remote_etag, e.server.etag_of("/a.bin"));   // the version that was received, not the one the listing showed
    PB_CHECK_EQ(*lf->remote_size, static_cast<int64_t>(newer.size()));
}

PB_TEST(a_weak_etag_is_never_used_to_resume) {
    DlEnv e;
    const std::string content = pattern(100 * 1024);
    e.server.add_file("/a.bin", content);
    e.server.set_weak_etag("/a.bin", true);
    e.server.transport().drop_next_download_after(40000);
    const auto out = e.run("/a.bin");
    PB_CHECK(out.state == JobState::Done);
    PB_CHECK(e.read("a.bin") == content);
    const auto g = gets(e);
    PB_CHECK_EQ(g.size(), size_t(2));
    PB_CHECK(pbc::header_value(g[1].headers, "Range").empty());
    PB_CHECK(pbc::header_value(g[1].headers, "If-Range").empty());
}

PB_TEST(a_server_that_ignores_the_range_gives_a_full_answer_that_replaces_the_partial_file) {
    DlEnv e;
    const std::string content = pattern(100 * 1024);
    e.server.add_file("/a.bin", content);
    e.server.set_ignore_range(true);
    e.server.transport().drop_next_download_after(40000);
    const auto out = e.run("/a.bin");
    PB_CHECK(out.state == JobState::Done);
    PB_CHECK(e.read("a.bin") == content);   // not 40000 old bytes followed by everything
    PB_CHECK_EQ(gets(e).size(), size_t(2));
}

PB_TEST(a_416_restarts_once_without_a_range) {
    DlEnv e;
    const std::string content = pattern(100 * 1024);
    e.server.add_file("/a.bin", content);
    e.server.transport().drop_next_download_after(40000);
    int ranged = 0;
    e.server.set_interceptor([&](const pbc::Request& r) -> std::optional<pbc::Response> {
        if (r.method == "GET" && !pbc::header_value(r.headers, "Range").empty() && ++ranged == 1) {
            pbc::Response bad;
            bad.status = 416;
            bad.headers = {{"Content-Range", "bytes */100"}};
            return bad;
        }
        return std::nullopt;
    });
    const auto out = e.run("/a.bin");
    PB_CHECK(out.state == JobState::Done);
    PB_CHECK(e.read("a.bin") == content);
    const auto g = gets(e);
    PB_CHECK_EQ(g.size(), size_t(3));
    PB_CHECK(pbc::header_value(g[2].headers, "Range").empty());
}

PB_TEST(a_206_that_does_not_start_where_we_asked_is_refused_and_leaves_nothing) {
    DlEnv e;
    const std::string content = pattern(100 * 1024);
    e.server.add_file("/a.bin", content);
    e.server.transport().drop_next_download_after(40000);
    e.server.set_interceptor([&](const pbc::Request& r) -> std::optional<pbc::Response> {
        if (r.method == "GET" && !pbc::header_value(r.headers, "Range").empty()) {
            pbc::Response wrong;
            wrong.status = 206;
            wrong.body = content.substr(0, 60000);
            wrong.headers = {{"Content-Range", "bytes 0-59999/102400"}, {"Content-Length", "60000"}, {"ETag", e.server.etag_of("/a.bin")}};
            return wrong;
        }
        return std::nullopt;
    });
    const auto out = e.run("/a.bin");
    PB_CHECK(out.state == JobState::Failed);
    PB_CHECK_EQ(out.error_key, std::string("protocol_error"));
    PB_CHECK(e.tree().empty());
    PB_CHECK(e.store->journal().empty());
}

PB_TEST(after_five_tries_the_job_is_paused_and_a_later_resume_finishes_it) {
    DlEnv e;
    const std::string content = pattern(100 * 1024);
    e.server.add_file("/a.bin", content);
    e.server.transport().drop_next_download_after(30000);
    e.server.set_offline(false);
    int attempts_after_drop = 0;
    e.server.set_interceptor([&](const pbc::Request& r) -> std::optional<pbc::Response> {
        if (r.method == "GET" && !pbc::header_value(r.headers, "Range").empty()) {   // every retry fails: the connection stays bad
            ++attempts_after_drop;
            pbc::Response down;
            down.error = pbc::ErrorClass::Network;
            down.error_key = "server_unreachable";
            return down;
        }
        return std::nullopt;
    });
    const auto out = e.run("/a.bin");
    PB_CHECK(out.state == JobState::Paused);
    PB_CHECK_EQ(out.error_key, std::string("server_unreachable"));
    PB_CHECK_EQ(attempts_after_drop, 5);
    const auto journal = e.store->journal();
    PB_CHECK_EQ(journal.size(), size_t(1));
    PB_CHECK_EQ(journal[0].state, std::string("paused"));
    PB_CHECK(e.any_part_file());   // the partial bytes are kept for the resume
    PB_CHECK(!e.exists("a.bin"));  // and the final name does not exist before the file is complete

    e.server.set_interceptor(nullptr);   // the connection is back
    pbtest::NoCancel c;
    const auto done = e.dl->resume(journal[0].id, c, [](const pbc::JobProgress&) {});
    PB_CHECK(done.state == JobState::Done);
    PB_CHECK(e.read("a.bin") == content);
    PB_CHECK(e.store->journal().empty());
    PB_CHECK(!e.any_part_file());
}

PB_TEST(a_restart_marks_running_jobs_as_paused_and_a_new_downloader_resumes_them) {
    DlEnv e;
    const std::string content = pattern(100 * 1024);
    e.server.add_file("/a.bin", content);
    // a job that was running when the app died: a partial file and a running row
    const auto id = e.store->journal_begin([&] {
        pbc::DownloadJournalRow j;
        j.remote_path = "/a.bin";
        j.target_path = "a.bin";
        j.expected_etag = e.server.etag_of("/a.bin");
        j.expected_size = static_cast<int64_t>(content.size());
        j.state = "running";
        j.started_at = 1;
        j.updated_at = 1;
        return j;
    }());
    e.write_local(".pbc-" + std::to_string(id.value) + ".part", content.substr(0, 25000));
    pbc::Downloader fresh(*e.store, e.server, e.storage, e.clock, e.root, [] { return true; }, pbc::DownloaderOptions());
    fresh.recover();
    PB_CHECK_EQ(e.store->journal()[0].state, std::string("paused"));
    pbtest::NoCancel c;
    const auto out = fresh.resume(id.value, c, [](const pbc::JobProgress&) {});
    PB_CHECK(out.state == JobState::Done);
    PB_CHECK(e.read("a.bin") == content);
    const auto g = gets(e);
    PB_CHECK_EQ(pbc::header_value(g[0].headers, "Range"), std::string("bytes=25000-"));
}

PB_TEST(a_cancel_while_waiting_ends_the_job_and_removes_its_files) {
    DlEnv e;
    e.server.add_file("/a.bin", pattern(100 * 1024));
    e.server.transport().drop_next_download_after(10000);
    e.server.set_offline(true);   // no way back
    pbtest::CancelFlag cancel;
    e.on_wait = [&](int n) { if (n == 2) cancel.flag = true; };
    const auto out = e.run("/a.bin", cancel);
    PB_CHECK(out.state == JobState::Cancelled);
    PB_CHECK(e.tree().empty());
    PB_CHECK(e.store->journal().empty());
}

PB_TEST_MAIN()
