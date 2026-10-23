#pragma once
// The pieces a sync test needs on top of a download test (tests/support/download_env.h): a pair below a reader folder, a Downloader that can be watched,
// the SyncRunner with a hook for every step, and small helpers for the reader folder. Fake-server tests only (constitution Principle IX).

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "download_env.h"
#include "pbc/queue_sender.h"
#include "pbc/sync_planner.h"
#include "pbc/sync_rules.h"
#include "pbc/sync_runner.h"
#include "pbc/sync_types.h"

namespace pbtest {

inline std::string pattern(size_t n, int seed = 7) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 31 + static_cast<size_t>(seed)) & 0xff);
    return s;
}

// the same bytes in every run, different for every call
struct SyncRandom : pbc::Random {
    uint64_t n = 0;
    void bytes(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>((++n * 131 + 17) & 0xff);
    }
};

struct SyncEnv : DlEnv {
    std::string reader;
    int64_t pair = 0;
    std::vector<std::string> steps;                // the hooks of the runner, in order
    std::function<void(const char*)> on_step;      // a test acts at a step
    std::unique_ptr<pbc::Downloader> sync_dl;
    std::unique_ptr<pbc::SyncRunner> runner;
    std::unique_ptr<pbc::QueueSender> sender;      // the queue's sender the runner sends its uploads with
    SyncRandom random;
    std::vector<std::string> sender_steps;          // the sender's hooks, in order
    std::function<void(const char*)> on_sender_step;
    pbc::SourceRules rules;
    bool target_visible_before_rename = false;
    bool watch_partial = false;                     // check, at every verification, that the file in flight has no final name

    SyncEnv() {
        reader = dir.file("Reader");
        ::mkdir(reader.c_str(), 0700);
        rules.root = dir.path();
        rules.deny = {dir.file("data")};
        pbc::SyncPair p = pbc::default_pair(server.base_url(), "demo");
        p.remote_folder = "/Books";
        p.local_folder = reader;
        auto r = store->sync_pair_add(p);
        if (!r.ok()) PB_FAIL_AT("cannot add the pair: " + r.error.key);
        pair = r.value;
        pbc::DownloaderOptions o;
        o.delays_s = {1, 1};
        o.sleep_ms = [](int) {};
        o.hook = [this](const char* step) {
            // while a file is being checked it has no final name yet: the card holds exactly the files that are recorded
            if (std::string(step) == "verify" && watch_partial) {
                size_t finals = 0;
                for (const auto& f : reader_tree())
                    if (f.find(".pbc-") == std::string::npos) ++finals;
                if (finals != store->sync_records(pair).size()) target_visible_before_rename = true;
            }
        };
        sync_dl = std::make_unique<pbc::Downloader>(*store, server, storage, clock, root, [this] { return wifi; }, o);
        build_runner();
    }
    void build_runner() {
        pbc::RunnerOptions ro;
        ro.hook = [this](const char* step) {
            steps.push_back(step);
            if (on_step) on_step(step);
        };
        runner = std::make_unique<pbc::SyncRunner>(*store, server, *sync_dl, storage, clock, rules, ro);
        pbc::SenderOptions so;
        so.wifi_up = [this] { return wifi; };
        so.hook = [this](const char* step) {
            sender_steps.push_back(step);
            if (on_sender_step) on_sender_step(step);
        };
        sender = std::make_unique<pbc::QueueSender>(*store, server, clock, random, rules, root, so);
        runner->attach_queue(sender.get());
    }
    void direction(pbc::Direction d) {
        auto p = store->sync_pair(pair);
        p->direction = d;
        if (!store->sync_pair_update(*p).ok()) PB_FAIL_AT("cannot change the direction");
    }
    pbc::Result<int64_t> compare(const std::string& trigger = "tap") {
        NoCancel c;
        return runner->compare(pair, trigger, c);
    }
    pbc::Result<pbc::SyncResult> run(int64_t plan, pbc::Cancel& c) { return runner->run(plan, c); }
    pbc::Result<pbc::SyncResult> run(int64_t plan) {
        NoCancel c;
        return runner->run(plan, c);
    }
    int64_t compare_ok() {
        auto r = compare();
        if (!r.ok()) PB_FAIL_AT("compare failed: " + r.error.key);
        return r.value;
    }
    pbc::SyncResult run_ok(int64_t plan) {
        auto r = run(plan);
        if (!r.ok()) PB_FAIL_AT("run failed: " + r.error.key);
        return r.value;
    }
    pbc::SyncResult sync() { return run_ok(compare_ok()); }
    std::string rd(const std::string& rel) const {
        std::ifstream f(reader + "/" + rel, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    bool has(const std::string& rel) const {
        struct stat st;
        return ::lstat((reader + "/" + rel).c_str(), &st) == 0;
    }
    void write_reader(const std::string& rel, const std::string& content) {
        const std::string p = reader + "/" + rel;
        mkdirs(p.substr(0, p.rfind('/')));
        std::ofstream(p, std::ios::binary | std::ios::trunc) << content;
    }
    std::vector<std::string> reader_tree() const {
        std::vector<std::string> out;
        list_tree(reader, "", &out);
        std::sort(out.begin(), out.end());
        return out;
    }
    bool part_file() const {
        for (const auto& f : reader_tree())
            if (f.find(".pbc-") != std::string::npos) return true;
        return false;
    }
    pbc::SyncPlan plan(int64_t id) const { return *store->sync_plan(id); }
    std::vector<pbc::PlanEntry> entries(int64_t id) const { return store->sync_plan_page(id, "all", 0, 10000).rows; }
    size_t requests(const std::string& method) const { return server.count(method); }
};

inline const pbc::PlanEntry* find(const std::vector<pbc::PlanEntry>& v, const std::string& rel) {
    for (const auto& e : v)
        if (e.rel == rel) return &e;
    return nullptr;
}

// a tree of `files` files over `levels` nested folders, below /Books
inline void tree(SyncEnv& e, int files, int levels) {
    for (int i = 0; i < files; ++i) {
        std::string path = "/Books";
        for (int l = 0; l < levels; ++l) path += "/L" + std::to_string(l) + "_" + std::to_string(i % (l + 2));
        e.server.add_file(path + "/file" + std::to_string(1000 + i) + ".txt", pattern(100 + static_cast<size_t>(i), i), 1790000000 + i);
    }
}

}  // namespace pbtest
