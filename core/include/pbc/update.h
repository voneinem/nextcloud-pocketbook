#pragma once
// Update (specs/001-nextcloud-files-client US4, research R-10): started only by the user. It walks what is kept offline, one listing per
// folder (Depth 1, at most 500 folders and 20 levels per run), downloads what is new or changed through the Downloader, leaves a conflict
// and every change of the user alone, and flags what has vanished on the server (its copy stays). The listings are read transiently: a
// folder the user never opened gets no cache row (Principle V); a folder that was cached is refreshed like an explicit refresh. A repeated
// run after an interruption ends as an uninterrupted one and fetches nothing twice. Qt-free.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "pbc/downloader.h"
#include "pbc/http_transport.h"
#include "pbc/storage_info.h"
#include "pbc/store.h"
#include "pbc/types.h"

namespace pbc {

inline constexpr int kUpdateMaxFolders = 500;
inline constexpr int kUpdateMaxDepth = 20;

struct UpdateReport {
    int folders = 0;             // listed
    int files = 0;               // examined
    int updated = 0;             // downloaded again because the server version changed
    int added = 0;               // downloaded for the first time (or the copy was gone from the card)
    int skipped = 0;             // unchanged, or changed by the user while the server stayed the same
    int need_decision = 0;       // a copy changed on both sides: the user decides
    int removed = 0;             // gone on the server (the copy stays)
    int failed = 0;
    int unchecked_folders = 0;   // beyond the limits of this run
    std::string stopped_by_key;  // "" = the run reached its end; otherwise the general problem (or "cancelled") that stopped it
    std::vector<std::string> decision_paths, removed_paths, failed_paths;
};

struct UpdatePlan {
    UpdateReport report;                 // what the walk found
    std::vector<RemoteEntry> downloads;  // the files to fetch, in walk order
    std::vector<bool> is_update;         // parallel to downloads: true = a newer version of a copy, false = new
    int64_t bytes = 0;                   // their sizes together
    int64_t started_at = 0;
};

class Updater {
public:
    Updater(Store& store, HttpTransport& http, Downloader& downloader, StorageInfo& storage, const Clock& clock)
        : store_(&store), http_(&http), dl_(&downloader), storage_(&storage), clock_(&clock) {}

    // The walk: lists the folders in scope and decides per file; downloads nothing. A general problem (login refused, server down, ...)
    // ends it early with report.stopped_by_key.
    UpdatePlan plan(Cancel& cancel);
    // The downloads of a plan. Records the result in the sync state.
    UpdateReport execute(UpdatePlan& plan, Cancel& cancel, const std::function<void(const UpdateReport&)>& on_progress);
    // plan() and execute() in one go.
    UpdateReport run(Cancel& cancel, const std::function<void(const UpdateReport&)>& on_progress);

private:
    Store* store_;
    HttpTransport* http_;
    Downloader* dl_;
    StorageInfo* storage_;
    const Clock* clock_;
};

}  // namespace pbc
