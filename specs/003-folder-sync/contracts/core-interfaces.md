# Contract: Core interfaces (`core/`, Qt-free)

**Feature**: [../spec.md](../spec.md) | **Rules**: [sync-rules.md](sync-rules.md) | **Data model**: [../data-model.md](../data-model.md) | **002**: [core-interfaces](../../002-manage-server-files/contracts/core-interfaces.md) | **001**: [core-interfaces](../../001-nextcloud-files-client/contracts/core-interfaces.md)

Declarations only; the code is written test-first (tasks.md). Namespace `pbc`, headers in `core/include/pbc/`, no Qt, no InkView,
no SDK header (constitution II). Existing seams (`Clock`, `Random`, `Cancel`, `HttpTransport`, `Result<T>`, `Error`,
`StorageInfo`, `DavSession`, `Store`, `SourceRules`, `ServerCaps`) are reused. New modules: `sync_types`, `sync_rules`,
`store_sync` (members of `Store`), `sync_scan`, `sync_planner`, `sync_runner`, `sync_scheduler`, `conflict_name`.
**No new module names a write verb** (rule N2); uploads and server deletions go through `queue_actions`/`queue_sender`.

## Changed existing seams (additive)

```cpp
// store.h: kSchemaVersion = 5; the sync members below live in store_sync.cpp.

// queue_types.h
struct QueueItem {
    // ...existing fields...
    std::optional<int64_t> pair_id;      // the pair whose plan created the item
    std::string sync_rel;                // the record the confirmation advances
    std::string sync_role;               // "", "file", "conflict_copy", "delete"
};

// queue_actions.h
struct SyncUploadSpec {                  // one upload of an approved plan
    std::string rel, local_abs;          // server-relative name, reader file (checked again by inspect_source)
    std::string dst_parent, dst_name;    // where it goes on the server (already validated by check_new_name)
    std::optional<std::string> replace_etag;   // set for a changed file: replace, conditional on this strong ETag
    std::string role = "file";           // or "conflict_copy"
    int64_t planned_size = 0, planned_mtime = 0;
};
// Writes ONE group in ONE transaction: mkdir items for folders not on the server yet (listing + open items looked at), then the uploads, each
// waiting for its mkdir. Nothing is sent. Errors: queue_full (whole call refused), not_allowed. Returns the group id and the item ids.
struct SyncGroup { int64_t group_id = 0; std::vector<int64_t> items; };
Result<SyncGroup> QueueActions::add_sync_uploads(int64_t pair_id, const std::string& remote_folder, const std::vector<SyncUploadSpec>&);
// One server deletion that the user confirmed in a proposal (sync_role "delete", conditional on the record's strong ETag).
Result<int64_t> QueueActions::add_sync_delete(int64_t pair_id, const std::string& remote_path, const std::string& etag);

// queue_sender.h
SendReport QueueSender::run(Cancel&, int64_t only = 0, int64_t only_group = 0);   // only_group != 0: items of that group, nothing else

// downloader.h
struct DownloadRequest {
    // ...existing fields...
    std::string target_root;             // "" = the download root of 001; else the pair's reader folder (absolute, checked)
    std::string strip_prefix;            // the pair's server folder: the local name is taken from the remote path below it
    std::optional<int64_t> pair_id;      // set: the result is recorded in sync_files, not local_files
    bool strict_version = false;         // the first answer must carry `etag`; otherwise JobState::Failed, error_key "changed_during_sync", nothing written
    std::string conflict_name;           // ConflictPolicy::KeepBoth under a sync: the exact (checked) local name to use
};

// local_files.h : the one new place that deletes a user's file for a sync (audited by N5)
// Removes the file only if: regular, inside `reader_folder`, no link on the way, and unchanged against `record` (size and mtime, or the
// stored hash). Error local_changed / not_found / path_unsafe otherwise; never touches the server. Removes empty parent folders up to (not
// including) reader_folder.
Error remove_pair_copy(const std::string& reader_folder, const std::string& local_rel, const SyncRecord& record);
// The only place that creates a folder for the folder picker ("New folder"); name through sanitize_component, parent inside the picker root
// without links, mkdir only, never over or into an existing entry (case-only clash refused). The bridge only calls it. Errors: name_invalid, exists, path_unsafe.
Result<std::string> create_picker_folder(const SourceRules& rules, const std::string& parent, const std::string& name);   // rules: picker root and the app's data folder
Result<std::string> resolve_local_file(const std::string& root, const std::string& local_relative);   // 001 function, root now a parameter
```

## Types

```cpp
// sync_types.h
enum class Direction { Down, Up, Both };
inline constexpr int kMaxSyncPairs = 8, kSyncMaxFolders = 500, kSyncMaxDepth = 20, kSyncMaxFiles = 5000,
                     kSyncMaxPlanEntries = 5000, kSyncMaxRecords = 20000, kSyncPlanMaxAgeS = 3600;
inline constexpr int64_t kSyncReserveMin = 256LL << 20; inline constexpr int kSyncReservePercent = 2;

struct SyncPair {
    int64_t id = 0; std::string server, user_id, remote_folder, local_folder;
    Direction direction = Direction::Down; int64_t max_files = 500, max_bytes = 2LL << 30;
    bool startup_sync = false, auto_sync = false;
    std::string state = "idle", problem_key, last_outcome = "never", last_result_json = "{}";
    std::optional<int64_t> last_compared_at, last_sync_at, held_files, held_bytes; int64_t created_at = 0;
};
struct SyncRecord {                       // data-model sync_files
    int64_t pair_id = 0; std::string rel, local_rel; Kind kind = Kind::File; std::string origin;
    std::string server_etag; std::optional<int64_t> server_size; std::string server_checksum;
    std::optional<int64_t> local_size, local_mtime; std::string local_sha256;
    bool diverged = false; std::string one_sided; bool quiet = false; int64_t synced_at = 0;
};
enum class PlanAction { Download, Upload, Skip, KeepBoth, ProposeDeleteServer, ProposeDeleteReader };
enum class Choice { None, TransferAgain, Leave, Delete };
struct PlanEntry {                        // data-model sync_plan_entries
    int seq = 0; std::string rel, local_rel; Kind kind = Kind::File; PlanAction action = PlanAction::Skip;
    std::string reason; std::optional<int64_t> size; std::string plan_etag; std::optional<int64_t> plan_lsize, plan_lmtime;
    std::string conflict_name, conflict_name_server; int records_below = 0;
    std::set<Choice> allowed; Choice choice = Choice::None; bool removed = false; std::string unapprovable, state = "planned", result_key;
};
struct SyncPlan {
    int64_t id = 0, pair_id = 0, created_at = 0; std::string trigger, state = "ready", over_limit;
    bool incomplete = false, truncated = false; int incomplete_folders = 0;
    std::optional<int64_t> free_bytes, quota_free; int64_t reserve_bytes = 0;
    struct Totals { int n_download = 0, n_upload = 0, n_skip = 0, n_keep_both = 0, n_prop_server = 0, n_prop_reader = 0, n_failed = 0, folders_new = 0;
                    int64_t b_download = 0, b_upload = 0, b_keep_both = 0; } totals;
    std::vector<PlanEntry> entries;       // in memory only while computing / small; the paged view reads the table
};
struct SyncedCopy { std::string root, local_rel, etag; int64_t size = 0, mtime = 0; };   // the 001 view of a synced file (R-15)
struct SyncResult { int downloaded = 0, uploaded = 0, skipped = 0, kept_both = 0, proposals_open = 0, failed = 0;
                    int64_t seconds = 0; std::vector<std::pair<std::string,int>> failure_keys; std::string stopped_by_key; };
```

## Rules of a pair

```cpp
// sync_rules.h : pure
struct PairCheck { bool ok; std::string error_key; };   // pair_overlap, pair_folder_forbidden, source_not_allowed, pair_limit_invalid, pair_limit
PairCheck check_pair(const SyncPair& candidate, const std::vector<SyncPair>& existing, const SourceRules&, const std::string& download_root);
bool needs_send_confirmation(Direction from, Direction to);          // true when the new direction can send reader files to the server
bool direction_allows_to_reader(Direction); bool direction_allows_to_server(Direction);
SyncPair default_pair(const std::string& server, const std::string& user_id);   // down, 500 files, 2 GiB, both switches off
int64_t reserve_for(int64_t volume_bytes);                           // max(kSyncReserveMin, 2 % of the volume)
```

## The store (members of `Store`, `store_sync.cpp`)

```cpp
Result<int64_t> sync_pair_add(const SyncPair&);                       // validates (check_pair) and the bound, in one transaction
Error sync_pair_update(const SyncPair&);                              // direction, limits, switches, folders; revalidated
Error sync_pair_remove(int64_t id);                                   // rows only; files and queue items untouched
// FR-050: marks every pair whose server/user_id differ from the given account with problem_key = account_changed (no row deleted). Called in the
// same transaction by save_account (other server or user) and clear_account. SyncRunner and the bridge also compare pair and current account.
Error sync_pairs_mark_account(const std::string& server, const std::string& user_id);
std::vector<SyncPair> sync_pairs() const;  std::optional<SyncPair> sync_pair(int64_t id) const;
// The lock of FR-034: sets state comparing|running if it is idle|plan_ready|interrupted, else error sync_busy. All in one transaction.
Error sync_pair_lock(int64_t id, const std::string& new_state);  Error sync_pair_unlock(int64_t id, const std::string& state, const std::string& problem_key);
Error sync_set_result(int64_t id, const std::string& outcome, const std::string& result_json, int64_t at);

std::optional<SyncRecord> sync_record(int64_t pair_id, const std::string& rel) const;
std::vector<SyncRecord> sync_records(int64_t pair_id) const;          // sorted by rel; bounded by the scan limits (<= 20,000 in total)
int64_t sync_record_count() const;
Error sync_record_put(const SyncRecord&);                              // identical / conflict copy / quiet / drop use these inside their own transaction
Error sync_record_drop(int64_t pair_id, const std::string& rel);
// One transaction: ends the Downloader's journal row and writes/advances the record (R-3, R-7).
Error sync_commit_download(int64_t pair_id, const SyncRecord&, int64_t job_id);
std::optional<SyncedCopy> synced_copy(const std::string& remote_path) const;   // R-15: root, local_rel, etag, size, mtime for a path inside a pair
std::set<std::string> queue_sync_open_rels(int64_t pair_id) const;    // names with an open item (rule 1 of the table)

Result<int64_t> sync_plan_store(const SyncPlan&);                      // supersedes a ready plan of the pair, in one transaction; entries bounded
std::optional<SyncPlan> sync_plan(int64_t id) const;                   // header and totals, no entries
struct PlanPage { std::vector<PlanEntry> rows; int total = 0; };
PlanPage sync_plan_page(int64_t plan_id, const std::string& section /* "transfers" | "skips" | "proposals" | "all" */, int offset, int limit) const;
Error sync_plan_remove_entry(int64_t plan_id, int seq, bool removed);  // only download/upload/keep_both, only while ready
Error sync_plan_set_choice(int64_t plan_id, int seq, Choice);          // only a proposal, only an allowed choice
Error sync_plan_state(int64_t plan_id, const std::string& state);      // enforces the state machine
Error sync_entry_result(int64_t plan_id, int seq, const std::string& state, const std::string& result_key);   // updates counts
struct SyncRecoveryReport { int pairs_interrupted = 0, plans_interrupted = 0; };
SyncRecoveryReport sync_recover();                                     // at start, no network: comparing|running -> interrupted
```

## Scanning

```cpp
// sync_scan.h
struct ScanEntry { std::string rel; Kind kind = Kind::File; int64_t size = 0; std::string etag, checksum; std::optional<int64_t> mtime;
                   Availability availability = Availability::Ok; std::string skip_reason; };   // sorted by rel (bytes)
struct Scan { std::vector<ScanEntry> entries; bool incomplete = false; int folders_not_listed = 0; std::string stopped_by_key; };
// Transient: creates no cache rows. Bounded by kSyncMax*. A failed or partial folder sets incomplete; a general problem (login refused, server
// down) ends the scan with stopped_by_key. A missing pair folder is stopped_by_key "server_folder_missing".
Scan scan_server(DavSession&, std::string_view user_id, const SyncPair&, Cancel&);
// No links followed (AT_SYMLINK_NOFOLLOW), hidden names (".") skipped, bounded; the pair folder missing is "reader_folder_missing".
Scan scan_reader(const SyncPair&, const SourceRules&, Cancel&);
// First-sync identity (R-3): streams hash_file over candidates with equal name and size; only called by the planner's host (the runner) with a Cancel.
bool identical_by_checksum(const std::string& reader_file, const ScanEntry& server_entry, Cancel&);
```

## The planner (pure) and conflict names

```cpp
// sync_planner.h
struct PlannerInput {
    SyncPair pair; std::vector<SyncRecord> records; Scan server, reader;
    std::set<std::string> open_rels; ServerCaps caps; std::string reader_date;     // "YYYY-MM-DD", for conflict names
    std::optional<int64_t> free_bytes, quota_free; int64_t volume_bytes = 0; int64_t held_files = 0, held_bytes = 0;
    std::function<bool(const ScanEntry&, const ScanEntry&)> identical;            // first-sync identity (R-3); the host supplies hash_file
    std::function<bool(const std::string& local_abs, const SyncRecord&)> reader_changed;   // R-3 change rule; the host supplies stat/hash
};
SyncPlan plan(const PlannerInput&);                                    // sync-rules.md; deterministic; no I/O of its own
// Pure approvability: state ready, !incomplete-blocking, !over_limit, !truncated, no unapprovable entry left (after removals), age <= kSyncPlanMaxAgeS.
struct Approval { bool ok; std::string error_key; };                   // plan_stale, plan_not_approvable, plan_too_large, over_limit
Approval can_approve(const SyncPlan&, int64_t now);
// conflict_name.h : "<stem> (server copy YYYY-MM-DD)<ext>" / "(reader copy ...)", " (2)", " (3)" while taken; extension and 255-byte limit kept.
std::string conflict_name(std::string_view name, bool server_copy, std::string_view date, const std::function<bool(std::string_view)>& taken);
```

## The runner and the scheduler

```cpp
// sync_runner.h : starts no thread, timer or sleep; names no write verb (queue/downloader do the I/O); builds paths only via choose_local_path / scans
struct RunnerOptions { std::function<bool()> wifi_up; std::function<void(const char* step)> hook;   // kill points: plan_written, approved, before_download,
                       std::function<void(const SyncResult&)> on_progress; };                         // downloaded, record_committed, group_created, item_confirmed, proposals_applied, result_written
class SyncRunner {
public:
    SyncRunner(Store&, HttpTransport&, Downloader&, QueueSender&, QueueActions&, StorageInfo&, const Clock&, SourceRules, RunnerOptions = {});
    // All three entry points first refuse a pair of another account (server/user_id differ from the current account row): error account_changed,
    // no lock, no request, no queue item, no file change (FR-050).
    // Compare: locks the pair, scans both sides (network), plans, stores the plan, unlocks as plan_ready (or idle with problem_key). No file changes.
    Result<int64_t> compare(int64_t pair_id, const std::string& trigger, Cancel&);
    // Approve + run: re-checks can_approve, then R-14 steps 1-5. Blocks the worker. Returns the result (also stored as last_result_json).
    Result<SyncResult> run(int64_t plan_id, Cancel&);
    // Apply choices of the proposals (R-10): each Delete re-checks its conditions; transfer_again becomes a download/upload through the same paths.
    Result<SyncResult> apply_choices(int64_t plan_id, Cancel&);
    // The offline preview: the planner over stored listings; flagged stale/incomplete; writes nothing.
    SyncPlan preview_from_cache(int64_t pair_id) const;
};

// sync_scheduler.h : time passed in; one AutoSyncScheduler per pair; no timers
struct SyncDue { std::vector<int64_t> pairs; int wait_ms = -1; };
class SyncScheduler {
public:
    explicit SyncScheduler(int quiet_ms = 10000);
    void note_action(int64_t pair_id, int64_t now_ms);
    SyncDue next(int64_t now_ms, bool global_auto_sync, bool wifi_up, bool busy, const std::function<bool(int64_t)>& pair_auto_sync);
    void forget(int64_t pair_id);
};
// Pairs touched by a path of the user's own action (prefix match, component-wise): server paths and reader paths.
std::vector<int64_t> pairs_for_server_path(const std::vector<SyncPair>&, const std::string& remote_path);
std::vector<int64_t> pairs_for_reader_path(const std::vector<SyncPair>&, const std::string& local_path);
```

## Rules that every implementation must keep (checked by tests)

1. `compare` changes no file on either side and queues nothing; it writes `sync_pairs`/`sync_plans`/`sync_plan_entries` only.
2. No function approves, runs or applies anything on its own: `run`/`apply_choices` are called only by the bridge on a user tap
   (`test_no_background_network`, `test_sync_triggers`).
3. A record changes only in `sync_commit_download`, `queue_confirm`, and the explicit record operations of R-3/R-10, each inside the
   transaction of the work it records.
4. `remove_pair_copy` is the only code that deletes a user's file for a sync, and only after the conditions of R-10.2.
5. Nothing in `sync_*.cpp` calls `unlink`, `rename`, `remove`, `rmdir`, `truncate` or opens a file for writing (N5, N6).
6. Hostile server names never produce a path outside the pair's reader folder (`choose_local_path` is the only constructor).
7. `sync_recover` and every pure function touch no network; recovery touches no file.
8. Logs carry codes, counts and ids only (N4).

## As built (deviations from the declarations above; the code is authoritative)

- `SyncUploadSpec` has `rel, local_abs, dst_parent, dst_name, replace_etag, role` and no planned size or time: `SyncRunner` re-inspects each source against the plan before it builds the specs.
- `SyncGroup { group_id, items, refused }`: `items` runs parallel to the specs (0 for a refused one); `refused` is a list of `PickRefusal` whose `path` is the spec's `rel`.
- `QueueActions::add_sync_uploads(pair_id, remote_folder, specs, existing_folders = {})`: folders in `existing_folders` get no folder item even when the stored listings do not show them (the comparison reads listings transiently, so the runner asks the server first).
- `QueueActions::add_sync_delete(pair_id, remote_path, etag, entry_kind = Kind::File)` returns `Result<int64_t>`; a folder deletion drops the records below it on confirmation.
- `QueueSender::run(cancel, only = 0, only_group = 0)`; `Store::queue_next(server, user, only_group = 0)`.
- `SyncRunner::attach_queue(QueueSender*)` gives the runner the sender for uploads and server deletions; `SyncRunner::apply_choices(plan_id, cancel)` is the separate run for proposals; `RunnerOptions::hook` also reports `group_created`, `item_confirmed`, `copy_committed`, `proposal_applied`, `proposals_applied`.
- `DownloadRequest::copy_of` (with `ConflictPolicy::KeepBoth` under a pair): the copy is recorded as a conflict copy with `one_sided = reader`.
- `SyncRecord::copy_of`; `SyncResult::deleted`; `Store::sync_interrupted_plan(pair_id)`.
- `Downloader::recover()` also records a pair file that was renamed into place but not yet recorded (planned size, strong version, not the old recorded copy), and `Downloader::run` replaces an earlier journal row of the same pair file.
- `Store::sync_plan_remove_entry` recomputes `sync_plans.over_limit`.
- `remove_pair_copy` uses the `reader_changed` rule of `sync_scan.h`.
