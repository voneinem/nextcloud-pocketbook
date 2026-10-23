#pragma once
// The planner of a folder pair (specs/003-folder-sync, research R-5, contracts/sync-rules.md): a pure function from the pair, its records and the two scans to
// a plan. It decides nothing by clocks, sizes or names alone (the records and the version markers decide), never puts a deletion in a plan (they are
// proposals, a later story), and writes nothing: no file, no record, no queue item, no request. The only thing it reads is the file system, through
// choose_local_path, to give a new file a name the card does not hold yet. Qt-free.

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>

#include "pbc/server_caps.h"
#include "pbc/sync_scan.h"
#include "pbc/sync_types.h"

namespace pbc {

struct PlannerInput {
    SyncPair pair;
    std::vector<SyncRecord> records;
    Scan server, reader;
    // The names (rel) with an open queue item, of this pair's own uploads or of the user's actions (rule 1 of the table).
    std::set<std::string> open_rels;
    ServerCaps caps;                         // the names the server forbids (check_new_name)
    std::string reader_date;                 // "YYYY-MM-DD", for conflict names
    std::optional<int64_t> free_bytes, quota_free;
    int64_t volume_bytes = 0;
    int64_t held_files = 0, held_bytes = 0;  // what the reader folder holds now (the limits)
    // First-sync identity (R-3): called only for equal size and a checksum in the listing; the host hashes the file.
    std::function<bool(const ScanEntry& server, const ScanEntry& reader)> identical;
    // The change rule of R-3 for one recorded file: the host looks at the card (and may hash).
    std::function<bool(const std::string& local_abs, const SyncRecord&)> reader_changed;
};

// The plan, entries sorted by name (bytes). `created_at` and `trigger` are the caller's. A name that is in sync gets no entry and is only counted
// (totals.n_in_sync): a folder with nothing to do has an empty plan however many files it holds.
SyncPlan plan(const PlannerInput& input);

// Whether a plan may be approved (research R-12): pure.
struct Approval {
    bool ok = false;
    std::string error_key;   // plan_stale, plan_not_approvable, plan_too_large, over_limit
};
Approval can_approve(const SyncPlan& plan, int64_t now);

}  // namespace pbc
