#include "pbc/sync_scheduler.h"

#include <algorithm>

#include "pbc/utf8.h"

namespace pbc {

void SyncScheduler::note_action(int64_t pair_id, int64_t now_ms) {
    auto it = pairs_.find(pair_id);
    if (it == pairs_.end()) it = pairs_.emplace(pair_id, AutoSyncScheduler(quiet_ms_)).first;
    it->second.note_action(now_ms);
}

SyncDue SyncScheduler::next(int64_t now_ms, bool global_auto_sync, bool wifi_up, bool busy, const std::function<bool(int64_t)>& pair_auto_sync) {
    SyncDue due;
    for (auto& kv : pairs_) {
        AutoSyncInputs in;
        in.enabled = global_auto_sync && (!pair_auto_sync || pair_auto_sync(kv.first));
        in.has_account = true;
        in.send_running = busy;
        in.sendable_items = true;   // a comparison is always worth making after a change
        in.wifi_up = wifi_up;
        const AutoSyncDecision d = kv.second.next(now_ms, in);
        if (d.step == AutoSyncStep::Start) {
            due.pairs.push_back(kv.first);
            kv.second.send_started(now_ms);   // it is going to be compared now: what the user did so far is covered
        } else if (d.step == AutoSyncStep::Wait) {
            if (due.wait_ms < 0 || d.wait_ms < due.wait_ms) due.wait_ms = d.wait_ms;
        }
    }
    return due;
}

void SyncScheduler::forget(int64_t pair_id) { pairs_.erase(pair_id); }

bool SyncScheduler::armed(int64_t pair_id) const {
    const auto it = pairs_.find(pair_id);
    return it != pairs_.end() && it->second.armed();
}

namespace {

std::vector<std::string> components(const std::string& path, bool fold) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < path.size()) {
        while (pos < path.size() && path[pos] == '/') ++pos;
        if (pos >= path.size()) break;
        const size_t slash = path.find('/', pos);
        const std::string part = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        out.push_back(fold ? case_fold(part) : part);
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return out;
}

// the folder equals the path or holds it, by whole components
bool holds(const std::string& folder, const std::string& path, bool fold) {
    if (folder.empty() || path.empty()) return false;
    const auto f = components(folder, fold), p = components(path, fold);
    if (f.size() > p.size()) return false;
    return std::equal(f.begin(), f.end(), p.begin());
}

}  // namespace

std::vector<int64_t> pairs_for_server_path(const std::vector<SyncPair>& pairs, const std::string& remote_path) {
    std::vector<int64_t> out;
    for (const SyncPair& p : pairs)
        if (holds(p.remote_folder, remote_path, false)) out.push_back(p.id);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<int64_t> pairs_for_reader_path(const std::vector<SyncPair>& pairs, const std::string& local_path) {
    std::vector<int64_t> out;
    for (const SyncPair& p : pairs)
        if (holds(p.local_folder, local_path, true)) out.push_back(p.id);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace pbc
