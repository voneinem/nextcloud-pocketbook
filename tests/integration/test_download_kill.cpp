// The download is killed at every step (spec SC-006; constitution Principle III): after the journal row, in the middle of the body, before
// the verification, right after the rename, and after the commit. After the restart cleanup no partial file is orphaned, a final file is
// complete or absent, and a resume ends with the file identical to the server's.
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include "download_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::DlEnv;

namespace {

std::string pattern(size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 31 + 7) & 0xff);
    return s;
}

// Runs the download in a child that kills itself (SIGKILL: no destructors, no flush) when `step` is reached. Returns true if it was killed.
bool run_and_die_at(const std::string& data_dir, const std::string& root, const std::string& step, const std::string& content) {
    const pid_t pid = ::fork();
    if (pid < 0) PB_FAIL_AT("fork failed");
    if (pid == 0) {
        pbtest::FakeNextcloud server;
        server.add_file("/Books/a.bin", content);
        pbtest::DlClock clock;
        clock.t = server.server_time();
        auto opened = pbc::Store::open(data_dir, clock);
        if (!opened.ok()) ::_exit(3);
        auto store = std::move(opened.value);
        pbc::AccountRow a;
        a.base_url = server.base_url();
        a.login_name = "demo";
        a.user_id = "demo";
        a.state = "ok";
        store->save_account(a);
        pbc::Credentials c;
        c.password = server.app_password();
        pbc::save_credentials(store->directory(), c);
        pbtest::FakeStorage storage;
        pbc::DownloaderOptions o;
        o.hook = [&](const char* s) {
            if (step == s) ::kill(::getpid(), SIGKILL);
        };
        pbc::Downloader dl(*store, server, storage, clock, root, [] { return true; }, o);
        pbc::DownloadRequest r;
        r.remote_path = "/Books/a.bin";
        r.size = static_cast<int64_t>(content.size());
        pbtest::NoCancel cancel;
        dl.run(r, cancel, [](const pbc::JobProgress&) {});
        ::_exit(0);   // finished without reaching the step
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
}

}  // namespace

PB_TEST(whatever_step_the_process_dies_at_the_restart_leaves_a_consistent_state_and_a_resume_finishes_the_file) {
    const std::string content = pattern(300 * 1024);
    for (const std::string step : {"journal", "body", "verify", "rename", "committed"}) {
        pbtest::TmpDir dir;
        const std::string data_dir = dir.file("data");
        const std::string root = dir.file("PBCloud");
        ::mkdir(root.c_str(), 0700);
        PB_CHECK(run_and_die_at(data_dir, root, step, content));

        // the next start: open the store again, clean up, look at what is left
        pbtest::FakeNextcloud server;
        server.add_file("/Books/a.bin", content);
        pbtest::DlClock clock;
        clock.t = server.server_time();
        auto opened = pbc::Store::open(data_dir, clock);
        PB_CHECK(opened.ok());
        auto store = std::move(opened.value);
        pbtest::FakeStorage storage;
        pbc::Downloader dl(*store, server, storage, clock, root, [] { return true; }, pbc::DownloaderOptions());
        dl.recover();

        // 1. no partial file without a journal row, and no journal row that says "running"
        std::vector<std::string> files;
        pbtest::list_tree(root, "", &files);
        const auto journal = store->journal();
        for (const std::string& f : files) {
            const bool part = f.find(".pbc-") != std::string::npos;
            if (!part) continue;
            bool known = false;
            for (const auto& j : journal)
                if (f.find(".pbc-" + std::to_string(j.id) + ".part") != std::string::npos) known = true;
            PB_CHECK(known);
        }
        for (const auto& j : journal) PB_CHECK_EQ(j.state, std::string("paused"));

        // 2. a final file is complete: never half a file under the final name
        const std::string final_path = root + "/Books/a.bin";
        struct stat st;
        if (::stat(final_path.c_str(), &st) == 0) {
            std::ifstream in(final_path, std::ios::binary);
            std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            PB_CHECK(got == content);
        }

        // 3. the copy is recorded exactly when the file is there and complete
        if (step == "committed") PB_CHECK(store->local_file("/Books/a.bin").has_value());
        if (step == "rename") PB_CHECK(store->local_file("/Books/a.bin").has_value() || ::stat(final_path.c_str(), &st) == 0);

        // 4. a resume (or a new download when nothing is left) ends with the file identical to the server's
        pbtest::NoCancel cancel;
        if (!journal.empty()) {
            const auto out = dl.resume(journal[0].id, cancel, [](const pbc::JobProgress&) {});
            PB_CHECK(out.state == pbc::JobState::Done);
        } else if (!store->local_file("/Books/a.bin").has_value()) {
            pbc::DownloadRequest r;
            r.remote_path = "/Books/a.bin";
            r.size = static_cast<int64_t>(content.size());
            PB_CHECK(dl.run(r, cancel, [](const pbc::JobProgress&) {}).state == pbc::JobState::Done);
        }
        std::ifstream in(final_path, std::ios::binary);
        std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        PB_CHECK(got == content);
        PB_CHECK(store->journal().empty());
        files.clear();
        pbtest::list_tree(root, "", &files);
        PB_CHECK_EQ(files.size(), size_t(1));   // only the file, no .part
        PB_CHECK(store->local_file("/Books/a.bin").has_value());
    }
}

PB_TEST_MAIN()
