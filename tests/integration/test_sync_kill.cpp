// A sync killed at every hook point (specs/003-folder-sync US8, SC-006, constitution Principle III): after the restart cleanup (Downloader::recover(),
// queue_recover(), Store::sync_recover()) a new comparison and run end with the reader tree, the records and the server exactly as an uninterrupted sync
// leaves them; no partial file is left, and a file that was recorded before the kill is not fetched or sent again. The pair syncs both ways: six
// downloads, three uploads (one into a folder the server lacks) and one name that both sides created with other content (kept in both versions). An upload whose outcome the kill left unknown is decided the way the user does in the
// Queue (Check the server, then Mark as done or Send again). The server outlives the client: the killed child saves its tree, the restart starts from it.
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "download_env.h"
#include "server_dump.h"
#include "pbc/sync_planner.h"
#include "pbc/sync_rules.h"
#include "pbc/sync_runner.h"
#include "sync_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::pattern;

namespace {

constexpr int kFiles = 6;     // downloads
constexpr int kUploads = 3;   // uploads
const char* const kUploadRels[kUploads] = {"u3.txt", "Up/u1.txt", "Up/Deep/u2.txt"};
constexpr int kConflictFiles = 2;   // the clashing name's two copies, one on each side

void fill(pbtest::FakeNextcloud& server) {
    for (int i = 0; i < kFiles; ++i)
        server.add_file("/Books/" + std::string(i % 2 ? "Odd" : "Even") + "/file" + std::to_string(i) + ".epub", pattern(2000 + static_cast<size_t>(i) * 331, i), 1790000000 + i);
    server.add_file("/Books/clash.epub", pattern(800, 50), 1790000050);
}

std::string upload_content(int i) { return pattern(700 + static_cast<size_t>(i) * 123, 40 + i); }

// What the child and the parent use: a store in `data_dir` with the signed-in account and the one pair; the server holds the same files in the same
// order, so that its ETags are the same in both processes.
struct Rig {
    pbtest::FakeNextcloud server;
    pbtest::DlClock clock;
    pbtest::FakeStorage storage;
    std::unique_ptr<pbc::Store> store;
    std::string data_dir, root, reader;
    int64_t pair = 0;
    pbc::SourceRules rules;
    std::unique_ptr<pbc::Downloader> dl;
    std::unique_ptr<pbc::SyncRunner> runner;
    std::unique_ptr<pbc::QueueSender> sender;
    pbtest::SyncRandom random;
    std::string dump_path;

    Rig(const std::string& base, bool first, const std::function<void(const char*)>& runner_hook, const std::function<void(const char*)>& dl_hook,
        const std::function<void(const char*)>& sender_hook = nullptr) {
        dump_path = base + "/server.dump";
        if (first || !pbtest::load_server(server, dump_path)) fill(server);   // a restart starts from what the killed process left on its server
        clock.t = server.server_time();
        data_dir = base + "/data";
        root = base + "/PBCloud";
        reader = base + "/Reader";
        auto opened = pbc::Store::open(data_dir, clock);
        if (!opened.ok()) PB_FAIL_AT("cannot open the store: " + opened.error.key);
        store = std::move(opened.value);
        if (first) {
            ::mkdir(root.c_str(), 0700);
            ::mkdir(reader.c_str(), 0700);
            pbc::AccountRow a;
            a.base_url = server.base_url();
            a.login_name = "demo";
            a.user_id = "demo";
            a.state = "ok";
            store->save_account(a);
            pbc::Credentials c;
            c.password = server.app_password();
            pbc::save_credentials(store->directory(), c);
            pbc::ServerCaps caps;   // a trash: a deletion on the server is offered
            caps.trash = true;
            store->save_server_caps(caps);
            pbc::SyncPair p = pbc::default_pair(server.base_url(), "demo");
            p.remote_folder = "/Books";
            p.local_folder = reader;
            p.direction = pbc::Direction::Both;
            pair = store->sync_pair_add(p).value;
            for (int i = 0; i < kUploads; ++i) {
                const std::string path = reader + "/" + kUploadRels[i];
                std::string dirs = path.substr(0, path.rfind('/'));
                for (size_t k = reader.size() + 1; k <= dirs.size(); ++k)
                    if (k == dirs.size() || dirs[k] == '/') ::mkdir(dirs.substr(0, k).c_str(), 0700);
                std::ofstream(path, std::ios::binary) << upload_content(i);
            }
            std::ofstream(reader + "/clash.epub", std::ios::binary) << pattern(900, 51);
        } else {
            pair = store->sync_pairs().at(0).id;
        }
        rules.root = base;
        rules.deny = {data_dir};
        pbc::DownloaderOptions o;
        o.delays_s = {1};
        o.sleep_ms = [](int) {};
        o.hook = dl_hook;
        dl = std::make_unique<pbc::Downloader>(*store, server, storage, clock, root, [] { return true; }, o);
        pbc::RunnerOptions ro;
        ro.hook = runner_hook;
        runner = std::make_unique<pbc::SyncRunner>(*store, server, *dl, storage, clock, rules, ro);
        pbc::SenderOptions so;
        so.hook = sender_hook;
        sender = std::make_unique<pbc::QueueSender>(*store, server, clock, random, rules, root, so);
        runner->attach_queue(sender.get());
    }
    // the work of main.cpp at start: before anything else, no network
    void recover() {
        dl->recover();
        store->queue_recover();
        store->sync_recover();
    }
    // What the user does in the Queue with an item whose outcome the kill left unknown, and then Send for what is still queued.
    void decide_queue() {
        pbtest::NoCancel c;
        for (const auto& it : store->queue_page(0, 100).rows) {
            if (it.state != pbc::ItemState::Unknown) continue;
            if (sender->check_unknown(it.id) == pbc::Found::Arrived) sender->mark_unknown_done(it.id);
            else sender->send_again(it.id);
        }
        sender->run(c);
    }
    // The user's deletions (specs/003-folder-sync US7): a compare, "Delete" chosen for every proposal that offers it, then the choices are applied.
    bool apply_deletes() {
        pbtest::NoCancel c;
        auto plan = runner->compare(pair, "tap", c);
        if (!plan.ok()) return false;
        for (const pbc::PlanEntry& e : store->sync_plan_page(plan.value, "proposals", 0, 1000).rows)
            if (e.allowed.count(pbc::Choice::Delete)) store->sync_plan_set_choice(plan.value, e.seq, pbc::Choice::Delete);
        return runner->apply_choices(plan.value, c).ok();
    }
    // Something removed on each side after a complete sync: the reader's copy of a file the server lost, the server's copy of a file the reader lost.
    void remove_on_both_sides() {
        server.remove("/Books/Even/file0.epub");
        ::unlink((reader + "/Odd/file1.epub").c_str());
    }
    bool sync() {
        pbtest::NoCancel c;
        auto plan = runner->compare(pair, "tap", c);
        if (!plan.ok()) return false;
        auto ready = store->sync_plan(plan.value);
        if (ready->state != "ready") return true;   // nothing to do
        return runner->run(plan.value, c).ok();
    }
};

struct KillPoint {
    std::string where;   // "runner", "downloader" or "sender"
    std::string step;
    int nth;             // the nth time the step is reached
};

// Runs the sync in a child that kills itself (SIGKILL: no destructors, no flush) at the kill point, after saving what its server holds. True if it was
// killed there.
bool run_and_die_at(const std::string& base, const KillPoint& kp, bool proposals = false) {
    const pid_t pid = ::fork();
    if (pid < 0) PB_FAIL_AT("fork failed");
    if (pid == 0) {
        int seen = 0;
        bool armed = !proposals;   // the proposal scenario first syncs completely, then arms the kill point for the application of the choices
        Rig* self = nullptr;
        auto hook = [&](const std::string& where) {
            return [&, where](const char* s) {
                if (!armed || kp.where != where || kp.step != s || ++seen != kp.nth) return;
                pbtest::save_server(self->server, self->dump_path);   // the server lives on without the client
                ::kill(::getpid(), SIGKILL);
            };
        };
        Rig rig(base, true, hook("runner"), hook("downloader"), hook("sender"));
        self = &rig;
        rig.sync();
        if (proposals) {
            rig.remove_on_both_sides();
            armed = true;
            rig.apply_deletes();
        }
        ::_exit(0);   // finished without reaching the point
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
}

struct State {
    std::map<std::string, std::string> files;     // the reader tree with its content
    std::map<std::string, std::string> records;   // rel -> origin:size (and the version, for what was downloaded: it is the server's own)
    std::map<std::string, std::string> server;    // the server's files below /Books with their content
    bool operator==(const State& o) const { return files == o.files && records == o.records && server == o.server; }
};

State state_of(Rig& r) {
    State s;
    std::vector<std::string> tree;
    pbtest::list_tree(r.reader, "", &tree);
    for (const std::string& f : tree) {
        std::ifstream in(r.reader + "/" + f, std::ios::binary);
        s.files[f] = std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    for (const auto& rec : r.store->sync_records(r.pair))
        s.records[rec.rel] = rec.origin + ":" + std::to_string(rec.server_size.value_or(-1)) + (rec.origin == "download" ? ":" + rec.server_etag : std::string());
    for (const auto& kv : r.server.nodes())
        if (!kv.second.folder && kv.first.compare(0, 7, "/Books/") == 0) s.server[kv.first] = kv.second.content;
    return s;
}

}  // namespace

PB_TEST(a_kill_at_any_point_of_a_sync_and_a_repeat_give_the_state_of_an_uninterrupted_run) {
    pbtest::TmpDir clean_dir;
    State expected;
    {
        Rig rig(clean_dir.path(), true, nullptr, nullptr);
        PB_CHECK(rig.sync());
        expected = state_of(rig);
        PB_CHECK_EQ(expected.files.size(), size_t(kFiles + kUploads + 1 + kConflictFiles / 2));   // the clash's original and the server's copy beside it
        PB_CHECK_EQ(expected.records.size(), size_t(kFiles + kUploads + 1 + kConflictFiles));    // the original (diverged) and both copies
        PB_CHECK_EQ(expected.server.size(), size_t(kFiles + kUploads + 1 + kConflictFiles / 2)); // the clash's original and the reader's copy beside it
    }
    std::vector<KillPoint> points = {{"runner", "plan_written", 1}, {"runner", "approved", 1}, {"runner", "result_written", 1}, {"runner", "group_created", 1}};
    for (int n : {1, 3, kFiles}) {
        points.push_back({"runner", "before_download", n});
        points.push_back({"runner", "downloaded", n});
        points.push_back({"runner", "record_committed", n});
    }
    for (int n : {1, 2, kUploads}) points.push_back({"runner", "item_confirmed", n});
    points.push_back({"runner", "copy_committed", 1});
    for (const char* step : {"journal", "body", "verify", "rename", "committed"}) points.push_back({"downloader", step, 2});
    for (int n : {1, 2, 4, 5}) {   // the folders count as items: Up, Up/Deep, then the three files in some order
        points.push_back({"sender", "sending", n});
        points.push_back({"sender", "request", n});
        points.push_back({"sender", "confirm", n});
    }
    int killed = 0;
    for (const KillPoint& kp : points) {
        pbtest::TmpDir dir;
        if (!run_and_die_at(dir.path(), kp)) continue;   // a point this build never reaches
        ++killed;
        Rig rig(dir.path(), false, nullptr, nullptr);
        rig.recover();
        const size_t recorded = rig.store->sync_records(rig.pair).size();
        const size_t on_server = state_of(rig).server.size();
        // 1. the pair shows that it was interrupted when the kill came during a comparison or a run
        const std::string pair_state = rig.store->sync_pair(rig.pair)->state;
        PB_CHECK(pair_state == "interrupted" || pair_state == "idle" || pair_state == "plan_ready");
        // 2. no partial file without a journal row
        const auto journal = rig.store->journal();
        for (const auto& kv : state_of(rig).files) {
            const std::string& f = kv.first;
            if (f.find(".pbc-") == std::string::npos) continue;
            bool known = false;
            for (const auto& j : journal)
                if (f.find(".pbc-" + std::to_string(j.id) + ".part") != std::string::npos) known = true;
            PB_CHECK(known);
        }
        // 3. the user settles what the kill left in the Queue, and syncing again gives the uninterrupted result
        rig.server.clear_log();
        rig.decide_queue();
        PB_CHECK(rig.sync());
        rig.decide_queue();   // anything a repeated run queued again
        const State got = state_of(rig);
        if (!(got == expected)) {
            std::fprintf(stderr, "DIFF at %s/%s/%d: files %zu vs %zu, records %zu vs %zu, server %zu vs %zu\n", kp.where.c_str(), kp.step.c_str(), kp.nth, got.files.size(),
                         expected.files.size(), got.records.size(), expected.records.size(), got.server.size(), expected.server.size());
            for (const auto& kv : got.files)
                if (!expected.files.count(kv.first) || expected.files.at(kv.first) != kv.second) std::fprintf(stderr, "  extra/different file %s\n", kv.first.c_str());
            for (const auto& kv : expected.files)
                if (!got.files.count(kv.first)) std::fprintf(stderr, "  missing file %s\n", kv.first.c_str());
            for (const auto& kv : got.records)
                if (!expected.records.count(kv.first) || expected.records.at(kv.first) != kv.second) std::fprintf(stderr, "  different record %s %s\n", kv.first.c_str(), kv.second.c_str());
            for (const auto& kv : expected.records)
                if (!got.records.count(kv.first)) std::fprintf(stderr, "  missing record %s\n", kv.first.c_str());
            for (const auto& kv : got.server)
                if (!expected.server.count(kv.first) || expected.server.at(kv.first) != kv.second) std::fprintf(stderr, "  extra/different server file %s\n", kv.first.c_str());
            for (const auto& kv : expected.server)
                if (!got.server.count(kv.first)) std::fprintf(stderr, "  missing server file %s\n", kv.first.c_str());
        }
        PB_CHECK(got == expected);
        PB_CHECK(rig.store->journal().empty());
        PB_CHECK_EQ(rig.store->queue_count_open(), 0);
        PB_CHECK_EQ(rig.store->sync_pair(rig.pair)->state, std::string("idle"));
        // 4. what was recorded before the kill is not fetched again, and nothing is sent twice
        size_t content_gets = 0;
        for (const auto& r : rig.server.log())
            if (r.method == "GET" && r.path.find("/dav/files/") != std::string::npos) ++content_gets;
        PB_CHECK(content_gets <= static_cast<size_t>(kFiles) - std::min<size_t>(recorded, kFiles) + 1);   // +1: a half file may be fetched again
        PB_CHECK(rig.server.count("PUT") <= static_cast<size_t>(kFiles + kUploads) - on_server + static_cast<size_t>(kFiles));
        for (const char* m : {"MOVE", "COPY", "DELETE"}) PB_CHECK_EQ(rig.server.count(m), size_t(0));
        // 5. and one more time changes nothing
        rig.server.clear_log();
        PB_CHECK(rig.sync());
        PB_CHECK(state_of(rig) == expected);
        for (const char* m : {"PUT", "MKCOL", "MOVE", "COPY", "DELETE"}) PB_CHECK_EQ(rig.server.count(m), size_t(0));
        PB_CHECK_EQ(rig.server.count("PUT"), size_t(0));
    }
    PB_CHECK(killed >= 21);   // the points of the runner, the downloader and the sender are reached
}

PB_TEST(a_kill_while_the_choices_for_proposals_are_applied_and_a_repeat_give_the_state_of_an_uninterrupted_application) {
    pbtest::TmpDir clean_dir;
    State expected;
    {
        Rig rig(clean_dir.path(), true, nullptr, nullptr);
        PB_CHECK(rig.sync());
        rig.remove_on_both_sides();
        PB_CHECK(rig.apply_deletes());
        expected = state_of(rig);
        PB_CHECK(!expected.files.count("Even/file0.epub") && !expected.files.count("Odd/file1.epub"));
        PB_CHECK(!expected.records.count("Even/file0.epub") && !expected.records.count("Odd/file1.epub"));
        PB_CHECK(!expected.server.count("/Books/Odd/file1.epub"));
    }
    std::vector<KillPoint> points = {{"runner", "proposal_applied", 1}, {"runner", "proposal_applied", 2}, {"runner", "proposals_applied", 1}};
    for (int n : {1, 2, 3, 4}) {   // the sender's steps of the server deletion
        points.push_back({"sender", "request", n});
        points.push_back({"sender", "confirm", n});
    }
    int killed = 0;
    for (const KillPoint& kp : points) {
        pbtest::TmpDir dir;
        if (!run_and_die_at(dir.path(), kp, true)) continue;
        ++killed;
        Rig rig(dir.path(), false, nullptr, nullptr);
        rig.recover();
        rig.server.clear_log();
        rig.decide_queue();   // what the kill left unknown in the Queue is decided the way the user does
        PB_CHECK(rig.apply_deletes());
        rig.decide_queue();
        const State got = state_of(rig);
        if (!(got == expected))
            std::fprintf(stderr, "DIFF at %s/%s/%d: files %zu vs %zu, records %zu vs %zu, server %zu vs %zu\n", kp.where.c_str(), kp.step.c_str(), kp.nth, got.files.size(),
                         expected.files.size(), got.records.size(), expected.records.size(), got.server.size(), expected.server.size());
        PB_CHECK(got == expected);
        PB_CHECK_EQ(rig.store->queue_count_open(), 0);
        // once more: nothing is proposed, nothing is deleted again
        rig.server.clear_log();
        PB_CHECK(rig.sync());
        PB_CHECK(state_of(rig) == expected);
        PB_CHECK_EQ(rig.server.count("DELETE"), size_t(0));
        PB_CHECK_EQ(rig.store->sync_pair(rig.pair)->state, std::string("idle"));
    }
    PB_CHECK(killed >= 3);
}

PB_TEST(recovery_touches_no_file_and_sends_no_request) {
    pbtest::TmpDir dir;
    PB_CHECK(run_and_die_at(dir.path(), {"runner", "downloaded", 2}));
    Rig rig(dir.path(), false, nullptr, nullptr);
    const State before = state_of(rig);
    rig.server.clear_log();
    rig.recover();
    PB_CHECK(state_of(rig) == before);
    PB_CHECK_EQ(rig.server.request_count(), size_t(0));
    PB_CHECK_EQ(rig.store->sync_pair(rig.pair)->state, std::string("interrupted"));
    PB_CHECK_EQ(rig.store->sync_plan(1)->state, std::string("interrupted"));   // the only plan of this store
}

PB_TEST_MAIN()
