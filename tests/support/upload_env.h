#pragma once
// The pieces an upload test needs (specs/002-manage-server-files): a fake Nextcloud with a folder to upload into, a store with a signed-in account and
// the capabilities of a stock server, a picker root with files made from a pattern, the queue actions to queue them, and a sender whose steps tests can
// watch and interrupt. Nothing here sleeps.

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "fake_nextcloud_server.h"
#include "pbc/credentials_file.h"
#include "pbc/folder_service.h"
#include "pbc/queue_actions.h"
#include "pbc/queue_sender.h"
#include "pbc/store.h"
#include "pbc/upload_source.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace pbtest {

struct UpClock : pbc::Clock {
    int64_t t = 1792497600;
    int64_t now() const override { return t; }
};

struct UpNoCancel : pbc::Cancel {
    bool requested() const override { return false; }
};

struct UpCancelFlag : pbc::Cancel {
    bool flag = false;
    bool requested() const override { return flag; }
};

// a deterministic Random: the same sequence in every run, different for every call
struct UpRandom : pbc::Random {
    uint64_t n = 0;
    void bytes(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>((++n * 131 + 17) & 0xff);
    }
};

inline void up_mkdirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0700);
}

// byte i of a file made from `seed`
inline char up_byte(uint8_t seed, long long i) { return static_cast<char>((i * 31 + seed) & 0xff); }

struct UpEnv {
    TmpDir dir;
    FakeNextcloud server;
    UpClock clock;
    UpRandom random;
    std::unique_ptr<pbc::Store> store;
    std::string root;                  // the reader's storage: root/Downloads, root/Notes, root/PBCloud (the download folder), root/system/pbcloud (denied)
    std::string download_root;
    pbc::SourceRules rules;
    bool wifi = true;
    std::vector<pbc::SendProgress> progress;
    std::function<void(const char*)> on_hook;   // called at every step of the sender (after the recording below)
    std::vector<std::string> hooks;

    explicit UpEnv(bool chunking = true) {
        clock.t = server.server_time();
        auto r = pbc::Store::open(dir.file("data"), clock);
        if (!r.ok()) PB_FAIL_AT("cannot open the store: " + r.error.key);
        store = std::move(r.value);
        pbc::AccountRow a;
        a.base_url = server.base_url();
        a.login_name = "demo";
        a.user_id = "demo";
        a.state = "ok";
        PB_CHECK(store->save_account(a).ok());
        pbc::Credentials c;
        c.password = server.app_password();
        PB_CHECK(pbc::save_credentials(store->directory(), c).ok());
        pbc::ServerCaps caps;
        caps.trash = true;
        caps.chunking = chunking;
        PB_CHECK(store->save_server_caps(caps).ok());
        server.set_chunking(chunking);
        root = dir.file("root");
        up_mkdirs(root + "/Downloads");
        up_mkdirs(root + "/Notes");
        up_mkdirs(root + "/system/pbcloud");
        download_root = root + "/PBCloud";
        up_mkdirs(download_root);
        rules.root = root;
        rules.deny = {root + "/system/pbcloud"};
        server.add_folder("/Docs");
        server.clear_log();
    }

    // A file of `size` bytes below the picker root; returns its path.
    std::string source(const std::string& rel, long long size, uint8_t seed = 1) {
        const std::string path = root + "/" + rel;
        up_mkdirs(path.substr(0, path.rfind('/')));
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        std::string block(1 << 20, '\0');
        for (long long off = 0; off < size; off += static_cast<long long>(block.size())) {
            const long long n = std::min<long long>(static_cast<long long>(block.size()), size - off);
            for (long long i = 0; i < n; ++i) block[static_cast<size_t>(i)] = up_byte(seed, off + i);
            f.write(block.data(), n);
        }
        return path;
    }
    // The same file rewritten with other bytes (the user changed it): new mtime too.
    void rewrite(const std::string& path, long long size, uint8_t seed) {
        source(path.substr(root.size() + 1), size, seed);
        struct timespec ts[2] = {{clock.t + 5, 0}, {clock.t + 5, 0}};
        ::utimensat(AT_FDCWD, path.c_str(), ts, 0);
    }

    // The process died and started again: the same data folder opened anew (what was committed is all that is left), then recovery at start.
    pbc::RecoveryReport restart() {
        store.reset();
        auto r = pbc::Store::open(dir.file("data"), clock);
        if (!r.ok()) PB_FAIL_AT("cannot reopen the store: " + r.error.key);
        store = std::move(r.value);
        return store->queue_recover();
    }

    // The reader fetches the listing of a folder (what the user saw when they queued something).
    void cache(const std::string& folder) {
        pbc::FolderService fs(*store, server, clock);
        const pbc::FolderResult r = fs.load(folder, pbc::Fetch::Always);
        if (!r.error.ok()) PB_FAIL_AT("cannot cache " + folder + ": " + r.error.key);
        server.clear_log();
    }

    pbc::QueueActions actions() { return pbc::QueueActions(*store, rules); }
    // Queues the files for the folder; returns the ids.
    std::vector<int64_t> queue(const std::vector<std::string>& paths, const std::string& target = "/Docs") {
        const auto r = actions().add_uploads(target, paths);
        if (!r.error.ok()) PB_FAIL_AT("add_uploads failed: " + r.error.key);
        return r.queued;
    }

    pbc::SenderOptions options() {
        pbc::SenderOptions o;
        o.wifi_up = [this] { return wifi; };
        o.on_progress = [this](const pbc::SendProgress& p) { progress.push_back(p); };
        o.hook = [this](const char* step) {
            hooks.push_back(step);
            if (on_hook) on_hook(step);
        };
        return o;
    }
    std::unique_ptr<pbc::QueueSender> sender() {
        return std::make_unique<pbc::QueueSender>(*store, server, clock, random, rules, download_root, options());
    }
    pbc::SendReport run(int64_t only = 0) {
        UpNoCancel cancel;
        return sender()->run(cancel, only);
    }
    pbc::QueueItem item(int64_t id) {
        const auto it = store->queue_item(id);
        if (!it) PB_FAIL_AT("the item " + std::to_string(id) + " is gone");
        return *it;
    }
    std::string sha_of(const std::string& path) {
        UpNoCancel cancel;
        const auto h = pbc::hash_source(path, cancel, nullptr);
        if (!h.ok()) PB_FAIL_AT("cannot hash " + path);
        return h.value;
    }
    std::string etag(const std::string& path) const { return server.etag_of(path); }
    size_t count(const std::string& method) const { return server.count(method); }
    // the logged write requests of one method whose path contains `part`
    std::vector<FakeNextcloud::Logged> logged(const std::string& method, const std::string& part = std::string()) const {
        std::vector<FakeNextcloud::Logged> out;
        for (const auto& l : server.log())
            if (l.method == method && (part.empty() || l.path.find(part) != std::string::npos)) out.push_back(l);
        return out;
    }
};

}  // namespace pbtest
