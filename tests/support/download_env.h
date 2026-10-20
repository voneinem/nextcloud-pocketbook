#pragma once
// The pieces a download test needs: a fake Nextcloud, a store with a signed-in account, a fake card, a download folder and a Downloader
// whose waiting costs no time. Tests flip `server`/`wifi` from `on_wait` to play "the connection comes back".

#include <sys/stat.h>

#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "fake_nextcloud_server.h"
#include "fake_storage.h"
#include "pbc/credentials_file.h"
#include "pbc/downloader.h"
#include "pbc/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace pbtest {

struct DlClock : pbc::Clock {
    int64_t t = 1792497600;
    int64_t now() const override { return t; }
};

struct NoCancel : pbc::Cancel {
    bool requested() const override { return false; }
};

struct CancelFlag : pbc::Cancel {
    bool flag = false;
    bool requested() const override { return flag; }
};

struct DlEnv {
    TmpDir dir;
    FakeNextcloud server;
    DlClock clock;
    FakeStorage storage;
    std::unique_ptr<pbc::Store> store;
    std::string root;
    bool wifi = true;
    int waits = 0;                              // how often the downloader waited
    std::function<void(int)> on_wait;           // called with the number of the wait
    std::vector<pbc::JobProgress> progress;
    std::unique_ptr<pbc::Downloader> dl;

    DlEnv() {
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
        root = dir.file("PBCloud");
        ::mkdir(root.c_str(), 0700);
        pbc::DownloaderOptions o;
        o.delays_s = {1, 1, 1, 1, 1};   // five waits of one second, which cost no time here
        o.sleep_ms = [this](int) {
            ++waits;
            if (on_wait) on_wait(waits);
        };
        dl = std::make_unique<pbc::Downloader>(*store, server, storage, clock, root, [this] { return wifi; }, o);
        server.clear_log();
    }

    pbc::DownloadRequest request(const std::string& remote_path) {
        pbc::DownloadRequest r;
        r.remote_path = remote_path;
        const auto n = server.node(remote_path);
        r.size = n.size();
        r.etag = n.etag;
        r.remote_mtime = n.mtime;
        return r;
    }
    pbc::DownloadOutcome run(const std::string& remote_path, pbc::Cancel& cancel, pbc::ConflictPolicy policy = pbc::ConflictPolicy::Ask) {
        pbc::DownloadRequest r = request(remote_path);
        r.policy = policy;
        progress.clear();
        return dl->run(r, cancel, [this](const pbc::JobProgress& p) { progress.push_back(p); });
    }
    pbc::DownloadOutcome run(const std::string& remote_path) {
        NoCancel c;
        return run(remote_path, c);
    }
    pbc::DownloadOutcome run(const std::string& remote_path, pbc::ConflictPolicy policy) {
        NoCancel c;
        return run(remote_path, c, policy);
    }
    std::string abs(const std::string& rel) const { return root + "/" + rel; }
    std::string read(const std::string& rel) const { return dir.read("PBCloud/" + rel); }
    bool exists(const std::string& rel) const {
        struct stat st;
        return ::stat(abs(rel).c_str(), &st) == 0;
    }
    // every file below the download root, relative, sorted
    std::vector<std::string> tree() const;
    // a leftover partial file anywhere below the root
    bool any_part_file() const {
        for (const auto& f : tree())
            if (f.find(".pbc-") != std::string::npos && f.size() > 5 && f.compare(f.size() - 5, 5, ".part") == 0) return true;
        return false;
    }
    void write_local(const std::string& rel, const std::string& content) {
        const size_t slash = rel.rfind('/');
        if (slash != std::string::npos) mkdirs(root + "/" + rel.substr(0, slash));
        std::ofstream f(abs(rel), std::ios::binary | std::ios::trunc);
        f << content;
    }
    static void mkdirs(const std::string& path) {
        for (size_t i = 1; i <= path.size(); ++i)
            if (i == path.size() || path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0700);
    }
};

}  // namespace pbtest

#include <dirent.h>
#include <algorithm>

namespace pbtest {
inline void list_tree(const std::string& base, const std::string& rel, std::vector<std::string>* out) {
    DIR* d = opendir((rel.empty() ? base : base + "/" + rel).c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        const std::string r = rel.empty() ? n : rel + "/" + n;
        if (e->d_type == DT_DIR) list_tree(base, r, out);
        else out->push_back(r);
    }
    closedir(d);
}
inline std::vector<std::string> DlEnv::tree() const {
    std::vector<std::string> out;
    list_tree(root, "", &out);
    std::sort(out.begin(), out.end());
    return out;
}
}  // namespace pbtest
