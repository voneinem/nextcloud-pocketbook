#pragma once
// When a pair's Auto Sync may compare (specs/003-folder-sync, research R-13, constitution Principle VI): a quiet period (10 seconds, shared with feature
// 002) after the user's own change, once, and never otherwise - not at start, not when Wi-Fi comes up, not after a failure, not after a run. One
// AutoSyncScheduler of feature 002 per pair; the scheduler holds no timer and reads no clock (time is a parameter), so every rule is proven on the host
// without waiting. The one single-shot timer lives in the sync bridge. What it starts is a comparison, which only stores a plan: never a transfer. Qt-free.

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "pbc/auto_sync.h"
#include "pbc/sync_types.h"

namespace pbc {

struct SyncDue {
    std::vector<int64_t> pairs;   // the pairs to compare now, in id order
    int wait_ms = -1;             // the nearest quiet period that is still running; -1 = nothing is waiting
};

class SyncScheduler {
public:
    explicit SyncScheduler(int quiet_ms = 10000) : quiet_ms_(quiet_ms) {}

    // The user's own action touched this pair (queuing, removing, a choice on a held item, saving the pair): arms it and restarts its quiet period.
    void note_action(int64_t pair_id, int64_t now_ms);
    // The rules of the Auto Sync scheduler of 002 for every armed pair: off (global or the pair's own) disarms, a run in progress keeps it armed, the quiet
    // period waits, no Wi-Fi disarms (nothing retries), else the pair is due and disarmed.
    SyncDue next(int64_t now_ms, bool global_auto_sync, bool wifi_up, bool busy, const std::function<bool(int64_t)>& pair_auto_sync);
    void forget(int64_t pair_id);   // a removed pair
    bool armed(int64_t pair_id) const;

private:
    int quiet_ms_;
    std::map<int64_t, AutoSyncScheduler> pairs_;
};

// The pairs whose folder holds the path of an action of the user (the folder itself or anything below it, compared by whole components): server paths
// byte-exact, reader paths without regard to case (the storage ignores it). In id order.
std::vector<int64_t> pairs_for_server_path(const std::vector<SyncPair>& pairs, const std::string& remote_path);
std::vector<int64_t> pairs_for_reader_path(const std::vector<SyncPair>& pairs, const std::string& local_path);

}  // namespace pbc
