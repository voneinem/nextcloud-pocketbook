#pragma once
// The SQLite store. One connection; every method is a short transaction. Schema 1 holds the settings table and the sync state
// row; schema 2 adds the tables of the files client (store_migrations.cpp, data-model.md of feature 001).
//
// Principle III: a database with a newer schema than this build knows is refused and never modified; upgrades are
// one-way, run in one transaction after a backup copy, and are followed by an integrity check.

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pbc/store_files.h"
#include "pbc/types.h"

struct sqlite3;

namespace pbc {

struct Migration {
    int from = 0;
    int to = 0;
    std::string sql;   // run inside the migration transaction
};

// The upgrade steps this build knows (store_migrations.cpp). A fresh database is created as schema 1 and then migrated.
const std::vector<Migration>& builtin_migrations();

struct StoreOptions {
    int target_version = 0;                 // 0 = the version of this build (Store::kSchemaVersion)
    int max_cached_entries = kDefaultCachedEntries;   // bounds of the listing cache; tests use small ones
    int max_cached_folders = kDefaultCachedFolders;
    std::vector<Migration> extra_migrations;  // steps after the built-in ones (tests add their own)
};

// The last synchronisation, one row.
struct SyncState {
    std::optional<int64_t> last_started, last_finished, last_success;
    std::string outcome = "never";   // never, running, success, partial, failed, offline
    std::string problem_key;         // first problem, "" = none
    std::string problem_area;        // "" = none; the product names its areas
    std::string counts_json = "{}";
    std::optional<int64_t> clock_skew_s;
};

class Store {
public:
    static constexpr int kSchemaVersion = 3;   // 1: settings and sync state; 2: the tables of the files client; 3: availability of favorites

    // Creates `dir` (mode 0700) if needed and opens `dir/pbcloud.db` (0600).
    // Errors: "schema_too_new" (the file is left untouched), "migration_failed" (rolled back, backup kept),
    // "open_failed", "integrity_failed".
    static Result<std::unique_ptr<Store>> open(const std::string& dir, Clock& clock, const StoreOptions& options = {});

    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    int schema_version() const;
    Error check_integrity() const;
    const std::string& directory() const { return dir_; }

    // ---- settings (table settings) ----
    std::string setting(const std::string& key) const;   // "" if not set
    Error set_setting(const std::string& key, const std::string& value);

    // ---- sync state (one row) ----
    SyncState sync_state() const;
    // The device clock minus a server's time, measured during a sync (clock_check.h); nullopt forgets it.
    Error set_clock_skew(std::optional<int64_t> skew_seconds);
    Error write_sync_state(const SyncState& state);

    // ---- the account (store_files.cpp) ----
    std::optional<AccountRow> account() const;
    // Validates the row (https address, a usable user id, bounded texts). Saving an account for a different server or user than the cached
    // listings belong to drops those listings and the favorites (downloaded copies, marks and the journal stay).
    Error save_account(const AccountRow& row);
    // Signing out: removes the account row only. The cache, the downloaded copies and the marks stay.
    Error clear_account();
    // The server accepted the account's credentials just now: the time shown as "Last checked". Nothing else of the row changes.
    Error touch_account_checked();
    Error set_account_state(const std::string& state, const std::string& problem_key);

    // ---- cached listings (store_files.cpp) ----
    // Replaces the rows of one folder as a whole in one transaction, then evicts the oldest folders while the cache is over its bounds
    // (never the root, never this folder; downloads, marks, favorites and the journal are not touched). A failure leaves the old rows.
    // Error listing_too_large above kMaxEntriesPerFolder entries.
    Error replace_listing(const std::string& folder, const Listing& listing, int64_t fetched_at);
    // After a 404 on refresh: the cached rows stay readable. Nothing is created for a folder that was never cached.
    Error mark_folder_gone(const std::string& folder);
    // A cached folder, or nullopt. Opening it renews its last_access_at (the eviction order).
    std::optional<FolderRow> folder(const std::string& path);
    // One page of a folder, folders first, then by the key (names compare case-folded). Unknown dates and sizes sort as the smallest.
    EntryPage entries_page(const std::string& folder, SortKey key, bool ascending, int offset, int limit) const;
    std::optional<RemoteEntry> entry(const std::string& path) const;

    // ---- downloaded copies and the journal (store_files.cpp) ----
    // The row of a finished download and the end of its journal row (job_id > 0), in one transaction: both or neither. A new download of
    // the same remote path replaces the row and clears server_missing and seen_etag. A local path already used by another remote path is refused.
    Error commit_download(const LocalFile& file, int64_t job_id);
    std::optional<LocalFile> local_file(const std::string& remote_path) const;
    std::vector<LocalFile> local_files(int offset, int limit) const;   // in the order of their local names, case-folded
    int local_file_count() const;
    std::vector<std::pair<std::string, std::string>> local_path_pairs() const;   // (remote path, local path) of every copy
    Error forget_local_file(const std::string& remote_path);          // the Space screen only; idempotent
    Error set_server_missing(const std::string& remote_path, bool missing);
    Error set_seen_etag(const std::string& remote_path, const std::string& etag);
    Result<int64_t> journal_begin(const DownloadJournalRow& row);
    Error journal_update(int64_t id, int64_t received, const std::string& state);
    Error journal_end(int64_t id);                                    // idempotent
    // What the first answer of the server said about the file: the strong ETag usable for If-Range ("" if weak or none) and the total size.
    Error journal_set_expected(int64_t id, const std::string& etag, std::optional<int64_t> size);
    std::vector<DownloadJournalRow> journal() const;

    // ---- "keep offline" marks (store_files.cpp) ----
    // Adds or removes a mark. A folder mark covers every file below it at any depth; a more specific mark stays when the one above it goes.
    // Removing a mark never touches a downloaded copy. The root can only be marked as a folder.
    Error set_mark(const std::string& remote_path, Kind kind, bool on);
    std::vector<MarkRow> marks() const;
    bool is_marked(const std::string& remote_path) const;   // a mark exactly on this path
    bool in_scope(const std::string& remote_path) const;    // this path, or a folder above it, is marked

    // ---- space (store_files.cpp) ----
    // Drops every cached listing (folders and entries) and gives the space back to the file system. Downloads, marks, favorites, the
    // journal, the account and the settings are not touched.
    Error clear_cache();
    int64_t local_files_bytes() const;   // the sizes of the downloaded copies together, as recorded
    int64_t database_bytes() const;      // the size of the database file: what the metadata cache and the rest of the records use

    // ---- favorites and search (store_files.cpp) ----
    // All rows of the favorites list are replaced in one transaction together with the time of the fetch; a failure keeps the old rows and
    // the old time. The order received is kept.
    Error replace_favorites(const std::vector<RemoteEntry>& entries, int64_t fetched_at);
    std::vector<RemoteEntry> favorites() const;
    std::optional<int64_t> favorites_fetched_at() const;
    // A case- and accent-folded substring match on the names of what the reader knows: cached listings ("reader"), favorites ("favorites")
    // and downloaded copies ("download"), each path once, by name. An empty term finds nothing. Results of a server search are not included.
    std::vector<SearchHit> search_cached(const std::string& term, int limit) const;
    // The answer of a server search, held in memory only (a temporary table of this connection, never written to the database file) so that a
    // tap on a result can show it and download it. Replaced by every search; gone with the process.
    void set_search_results(const std::vector<RemoteEntry>& entries);

    // Test seams (the product code uses the typed methods). debug_exec returns the
    // SQLite error text ("" on success); debug_query returns the first column of the first row as text.
    std::string debug_exec(const std::string& sql);
    std::string debug_query(const std::string& sql) const;

private:
    Store(sqlite3* db, std::string dir, Clock& clock, int max_entries, int max_folders)
        : db_(db), dir_(std::move(dir)), clock_(&clock), max_entries_(max_entries), max_folders_(max_folders) {}

    // One SQLite connection is shared by the UI thread (reads) and the worker thread (sync). Every public method
    // holds this mutex for its whole transaction, so a reader never sees a half-applied batch.
    mutable std::mutex mu_;
    sqlite3* db_ = nullptr;
    bool search_table_ = false;   // the temporary table of search results exists in this connection
    std::string dir_;
    Clock* clock_ = nullptr;
    int max_entries_ = kDefaultCachedEntries;
    int max_folders_ = kDefaultCachedFolders;
};

}  // namespace pbc
