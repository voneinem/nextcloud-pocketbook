#pragma once
// Folder synchronization: the model of a pair, its records and its plans (specs/003-folder-sync/data-model.md, contracts/core-interfaces.md
// "Types"). Plain values; the rules are in sync_rules.h and sync_planner.h, the rows in the store (store_sync.cpp). No Qt.

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pbc/store_files.h"

namespace pbc {

inline constexpr int kMaxSyncPairs = 8;                  // pair_limit beyond
inline constexpr int kSyncMaxFolders = 500;              // bounds of one scan (research R-4)
inline constexpr int kSyncMaxDepth = 20;
inline constexpr int kSyncMaxFiles = 5000;
inline constexpr int kSyncMaxPlanEntries = 5000;         // more: the plan is `truncated` and cannot be approved (plan_too_large)
inline constexpr int kSyncMaxRecords = 20000;            // records_full beyond
inline constexpr int kSyncPlanMaxAgeS = 3600;            // a plan older than this cannot be approved (plan_stale)
inline constexpr int64_t kSyncReserveMin = 256LL << 20;  // working values (research R-11)
inline constexpr int kSyncReservePercent = 2;
inline constexpr int kSyncMinFiles = 1, kSyncMaxFilesLimit = 5000;                       // what a pair may be set to
inline constexpr int64_t kSyncMinBytes = 1LL << 20, kSyncMaxBytesLimit = 64LL << 30;
inline constexpr int kSyncFinalPlansKept = 2;            // finished plans per pair that stay for the result

enum class Direction { Down, Up, Both };
const char* direction_text(Direction d);                 // "down" | "up" | "both"
std::optional<Direction> parse_direction(std::string_view text);

struct SyncPair {
    int64_t id = 0;
    std::string server, user_id, remote_folder, local_folder;
    Direction direction = Direction::Down;
    int64_t max_files = 500, max_bytes = 2LL << 30;
    bool startup_sync = false, auto_sync = false;
    std::string state = "idle";      // idle | comparing | plan_ready | running | interrupted: the lock of FR-034
    std::string problem_key;         // "" or server_folder_missing, reader_folder_missing, auth_failed, account_changed, no_network
    std::string last_outcome = "never", last_result_json = "{}";
    std::optional<int64_t> last_compared_at, last_sync_at, held_files, held_bytes;
    int64_t created_at = 0;
};

// One name a pair has brought into agreement or deliberately left as it is (sync_files).
struct SyncRecord {
    int64_t pair_id = 0;
    std::string rel, local_rel;
    Kind kind = Kind::File;
    std::string origin;              // download | upload | identical | conflict_copy
    std::string server_etag;
    std::optional<int64_t> server_size;
    std::string server_checksum;
    std::optional<int64_t> local_size, local_mtime;
    std::string local_sha256;        // only when a transfer already had it
    bool diverged = false;
    std::string one_sided;           // "" | reader | server
    bool quiet = false;
    std::string copy_of;             // a conflict copy: the rel of the original it was made for
    int64_t synced_at = 0;
};

enum class PlanAction { Download, Upload, Skip, KeepBoth, ProposeDeleteServer, ProposeDeleteReader };
const char* plan_action_text(PlanAction a);
std::optional<PlanAction> parse_plan_action(std::string_view text);

enum class Choice { None, TransferAgain, Leave, Delete };
const char* choice_text(Choice c);                       // "" | transfer_again | leave | delete
std::optional<Choice> parse_choice(std::string_view text);

struct PlanEntry {
    int seq = 0;
    std::string rel, local_rel;
    Kind kind = Kind::File;
    PlanAction action = PlanAction::Skip;
    std::string reason;
    std::optional<int64_t> size;
    std::string plan_etag, plan_checksum;   // the server version planned against; the listing's checksum text (a download is verified against it)
    std::optional<int64_t> plan_lsize, plan_lmtime;
    std::string conflict_name, conflict_name_server;
    int records_below = 0;
    std::set<Choice> allowed;
    Choice choice = Choice::None;
    bool removed = false;
    std::string unapprovable;
    std::string state = "planned";   // planned | done | failed | skipped
    std::string result_key;
};

struct SyncPlan {
    int64_t id = 0, pair_id = 0, created_at = 0;
    std::optional<int64_t> approved_at, finished_at;
    std::string trigger = "tap";     // tap | startup | auto
    std::string state = "ready";     // ready | approved | running | done | interrupted | cancelled | superseded
    std::string over_limit;          // "" | files | bytes | both
    bool incomplete = false, truncated = false;
    int incomplete_folders = 0;      // the number of folders that were not compared (never names)
    std::optional<int64_t> free_bytes, quota_free;
    int64_t reserve_bytes = 0;
    struct Totals {
        int n_download = 0, n_upload = 0, n_skip = 0, n_keep_both = 0, n_prop_server = 0, n_prop_reader = 0, n_failed = 0, folders_new = 0;
        int n_in_sync = 0;               // names that are in sync: counted, not listed
        int64_t b_download = 0, b_upload = 0, b_keep_both = 0;
    } totals;
    int unapprovable = 0;            // entries (not removed) that cannot be approved as they are (no_space, ...)
    std::vector<PlanEntry> entries;  // in memory while computing and when stored; the paged view reads the table
};

// The 001 view of a file a pair has synchronized (research R-15).
struct SyncedCopy {
    int64_t pair_id = 0;
    std::string root, local_rel, etag;   // root: the pair's reader folder; local_rel is relative to it
    int64_t size = 0, mtime = 0;
};

struct SyncResult {
    int downloaded = 0, uploaded = 0, skipped = 0, kept_both = 0, proposals_open = 0, failed = 0;
    int deleted = 0;                 // files the user's confirmed choices removed (the server's trash, or the reader)
    int64_t seconds = 0;
    std::vector<std::pair<std::string, int>> failure_keys;
    std::string stopped_by_key;
};

struct PlanPage {
    std::vector<PlanEntry> rows;
    int total = 0;
};

struct SyncRecoveryReport {
    int pairs_interrupted = 0, plans_interrupted = 0;
};

// `parent_rel` + "/" + `name` as a relative path of a pair ("" parent: just the name); "" when `name` is empty, ".", "..", or holds a "/", a backslash or a NUL.
// Pure string work for the scans; a path on the card is built by join_path_checked (sync_scan.h).
std::string rel_join(std::string_view parent_rel, std::string_view name);

// "/" for the server root, else the folder with a trailing "/": the prefix every remote path below the folder starts with.
std::string remote_prefix(const std::string& folder);

// The legal plan transitions of data-model.md "State machines".
bool plan_transition_allowed(const std::string& from, const std::string& to);

}  // namespace pbc
