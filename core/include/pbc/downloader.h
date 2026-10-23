#pragma once
// The download engine (specs/001-nextcloud-files-client US3, research R-8, constitution Principle III): a file is written to a temporary
// file in its target folder, checked, and renamed into place only when it is complete; an interrupted download resumes with a ranged
// request when that is safe and otherwise starts over; a connection that drops is waited for without sending anything; nothing the user
// changed is overwritten. One job at a time, on the caller's (worker) thread. Qt-free.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "pbc/http_transport.h"
#include "pbc/storage_info.h"
#include "pbc/store.h"
#include "pbc/types.h"

namespace pbc {

enum class JobState { Running, Waiting, Paused, Done, Cancelled, Failed, Blocked };

// What to do when a local copy of the file exists.
enum class ConflictPolicy {
    Ask,        // a copy the user changed (or a missing record) stops the job before it starts: Blocked, local_changed
    Replace,    // the user confirmed twice: the copy is replaced (atomically, only when the new file is complete)
    KeepBoth,   // the server version is saved next to the copy as "<name> (server copy)<ext>" and not recorded; the copy stays
};

struct DownloadRequest {
    std::string remote_path;
    std::optional<int64_t> size;             // from the listing; used for the checks before the first byte
    std::string etag;                        // from the listing
    std::string checksum;                    // the oc:checksums text of the listing ("" = none)
    std::optional<int64_t> remote_mtime;
    ConflictPolicy policy = ConflictPolicy::Ask;
    // The downloads of a folder pair (specs/003-folder-sync, research R-7). Without `pair_id` none of this has any effect.
    int64_t pair_id = 0;                     // > 0: the result is recorded in sync_files (Store::sync_commit_download), not in local_files
    std::string target_root;                 // absolute: the pair's reader folder; the file goes below it instead of below the download root
    std::string strip_prefix;                // the pair's server folder: the remote path below it is the path below target_root
    bool strict_version = false;             // the answer must be the version in `etag` (strong): If-Match; a 412 or another ETag ends the job
                                             // Failed with changed_during_sync and nothing is written
    std::string conflict_name;               // with KeepBoth: the exact name of the copy beside the original (already a valid, free name; checked again)
    std::string copy_of;                     // with KeepBoth under a pair: the rel of the original; the copy is recorded as a conflict copy that lives on the reader only
};

struct JobProgress {
    JobState state = JobState::Running;
    int64_t done = 0;
    int64_t total = 0;                       // 0 = unknown
    int attempt = 0;                         // retries after a dropped connection
};

struct DownloadOutcome {
    JobState state = JobState::Failed;
    std::string error_key;                   // "" when Done
    std::string local_path;                  // relative to the download root, when Done
    int64_t job_id = 0;                      // of a Paused job: what resume() takes
};

// The checks before the first byte is written (FR-019, FR-023). Refused: too_large_for_storage (more than the file system holds: 4 GiB or
// more on FAT), storage_readonly, not_enough_space (free < size + max(8 MiB, 1 %)); NeedsConfirmation from `warn_bytes` on.
struct Preflight {
    enum class Verdict { Ok, NeedsConfirmation, Refused } verdict = Verdict::Ok;
    std::string error_key;
    int64_t size = 0, free = 0, needed = 0;
};
Preflight preflight_download(const StorageInfo& storage, const std::string& target_dir, std::optional<int64_t> size, int64_t warn_bytes);

struct DownloaderOptions {
    std::vector<int> delays_s = {5, 15, 45, 60, 60};   // the waits after a dropped connection; after the last one the job is paused
    int max_wait_s = 600;
    std::function<void(int ms)> sleep_ms;              // default: sleeps; tests make waiting free
    std::function<void(const char* step)> hook;        // test seam, called at: journal, body, verify, rename, committed
};

class Downloader {
public:
    Downloader(Store& store, HttpTransport& http, StorageInfo& storage, const Clock& clock, std::string download_root,
               std::function<bool()> wifi_up, DownloaderOptions options = {});

    // Blocks until the job ends. `on_progress` is called when the state changes and then about every 5 % or 2 seconds.
    DownloadOutcome run(const DownloadRequest& request, Cancel& cancel, const std::function<void(const JobProgress&)>& on_progress);
    // Continues a paused journal row (also one left by an earlier run of the app).
    DownloadOutcome resume(int64_t job_id, Cancel& cancel, const std::function<void(const JobProgress&)>& on_progress);
    // At app start, without network: running jobs become paused, journal rows without a partial file are dropped (or completed when the
    // rename had happened), partial files without a journal row are removed.
    void recover();
    const std::string& root() const { return root_; }

private:
    struct Job;
    DownloadOutcome transfer(Job& job, Cancel& cancel, const std::function<void(const JobProgress&)>& on_progress);
    void sleep(int ms);

    Store* store_;
    HttpTransport* http_;
    StorageInfo* storage_;
    const Clock* clock_;
    std::string root_;
    std::function<bool()> wifi_up_;
    DownloaderOptions options_;
};

}  // namespace pbc
