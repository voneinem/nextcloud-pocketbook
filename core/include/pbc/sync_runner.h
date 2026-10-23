#pragma once
// Comparing a folder pair and running an approved plan (specs/003-folder-sync, research R-5, R-12, R-14). A comparison scans both sides, plans and stores
// the plan: it changes no file, no record and queues nothing (FR-007). A run starts only from an approved plan; every transfer goes through the
// Downloader (downloads) or the queue of feature 002 (uploads), a record advances only when its file is complete (R-3), and a repeated run after an
// interruption computes a new plan from the records and ends as an uninterrupted one. Runs on the caller's (worker) thread; starts no thread, timer
// or sleep. Qt-free.

#include <cstdint>
#include <functional>
#include <set>
#include <string>

#include "pbc/downloader.h"
#include "pbc/http_transport.h"
#include "pbc/queue_sender.h"
#include "pbc/source_walk.h"
#include "pbc/storage_info.h"
#include "pbc/store.h"
#include "pbc/sync_planner.h"
#include "pbc/sync_types.h"
#include "pbc/types.h"

namespace pbc {

struct RunnerOptions {
    // Test seam for the kill harness, called at: plan_written, approved, before_download, downloaded, record_committed, result_written (and, for the
    // later stories, group_created, item_confirmed, proposals_applied).
    std::function<void(const char* step)> hook;
    // A coarse progress report after every finished entry (the counts so far).
    std::function<void(const SyncResult&)> on_progress;
    // The download in flight (the Downloader's own coarse progress), for the text of a long file.
    std::function<void(const JobProgress&)> on_file_progress;
};

class SyncRunner {
public:
    SyncRunner(Store& store, HttpTransport& http, Downloader& downloader, StorageInfo& storage, const Clock& clock, SourceRules rules, RunnerOptions options = {});

    // Locks the pair, scans both sides (the server with Depth-1 listings read transiently), plans and stores the plan; the pair then is plan_ready (or
    // idle with the reason). Errors: account_changed (a pair of another account: no lock, no request), sync_busy, auth_failed and the other general keys
    // of the server, server_folder_missing, reader_folder_missing, cancelled. `trigger` is tap, startup or auto.
    Result<int64_t> compare(int64_t pair_id, const std::string& trigger, Cancel& cancel);

    // The queue's sender, for the uploads of a plan (specs/003-folder-sync R-8). Without one an upload entry fails with protocol_error; the sender
    // outlives the run. Its options (Wi-Fi check, hooks) are the caller's.
    void attach_queue(QueueSender* sender) { sender_ = sender; }

    // Checks the approval (can_approve), then runs the plan: first the records of the identical first-sync matches and the dropped ones, then the downloads
    // in plan order, then the uploads as one group of the queue sent on its own. Blocks the worker. Errors before anything happens: account_changed, plan_not_approvable, plan_stale, plan_too_large, over_limit, sync_busy.
    // The result is also stored as the pair's last result.
    Result<SyncResult> run(int64_t plan_id, Cancel& cancel);

    // The user's choices for the proposals of a plan (specs/003-folder-sync R-10), a separate short run that is never part of run(): `transfer_again` copies the
    // file back to the side that lacks it through the normal paths, `leave` remembers that the user wants it left (the record is quiet until the present file
    // changes), `delete` removes the file on the side that still has it - on the server through a delete item of the queue (the trash, conditional on the
    // recorded version), on the reader through remove_pair_copy (only an unchanged copy). A plan that is ready or done and not older than an hour is
    // needed (plan_not_approvable, plan_stale). The caller has shown the confirmation for every delete; nothing here asks again, but every delete
    // re-checks its own conditions. Entries without a choice are untouched. The result is also stored as the pair's last result.
    Result<SyncResult> apply_choices(int64_t plan_id, Cancel& cancel);

    // What is known without any request (Story 2, scenario 8): the plan the stored listings of the folders the user has visited would give, flagged
    // stale and incomplete (state "preview", created_at = the age of the oldest listing used). It is in memory only: no plan row is written, no request
    // is made, and it cannot be approved (plan_not_approvable). A folder that was never listed counts among the folders not compared.
    Result<SyncPlan> preview_from_cache(int64_t pair_id, Cancel& cancel) const;

private:
    void run_uploads(const SyncPair& pair, int64_t plan_id, const std::vector<PlanEntry>& entries, const std::set<int>& reader_copy_done, Cancel& cancel, SyncResult* res);
    PlannerInput input_for(const SyncPair& pair, const AccountRow& account, Scan server, Scan reader, Cancel& cancel) const;
    Store* store_;
    HttpTransport* http_;
    Downloader* dl_;
    StorageInfo* storage_;
    const Clock* clock_;
    SourceRules rules_;
    RunnerOptions options_;
    QueueSender* sender_ = nullptr;
};

// FR-050: the pair belongs to the account that is signed in now (same server, same user, not marked account_changed).
bool pair_matches_account(const Store& store, const SyncPair& pair);

}  // namespace pbc
