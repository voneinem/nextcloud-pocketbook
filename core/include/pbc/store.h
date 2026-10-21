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

#include "pbc/queue_types.h"
#include "pbc/server_caps.h"
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
    static constexpr int kSchemaVersion = 4;   // 1: settings and sync state; 2: the tables of the files client; 3: availability of favorites; 4: the queue

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

    // ---- what the server told the last account check (store_queue.cpp, specs/002-manage-server-files) ----
    // The row is replaced as a whole. save_account for another server or user, and clear_account, forget it.
    std::optional<ServerCaps> server_caps() const;
    Error save_server_caps(const ServerCaps& caps);

    // ---- the queue of changes (store_queue.cpp, specs/002-manage-server-files/data-model.md) ----
    // Written before any request. A row is validated (an account, a valid name that also passes the saved server lists, valid paths, a strong ETag
    // for everything that needs one, a replace only of a file) and the queue is bounded at kMaxQueueItems rows (queue_full; done rows count until
    // they are cleared). Errors: name_forbidden, refresh_first, queue_full, not_allowed, name_exists, protocol_error.
    Result<int64_t> queue_add(const QueueItem& item);
    // All rows or none. depends_on_index[i] is the index (smaller than i) of the item that item i waits for, or -1; the rows share one group id.
    Result<std::vector<int64_t>> queue_add_group(const std::vector<QueueItem>& items, const std::vector<int>& depends_on_index);
    std::optional<QueueItem> queue_item(int64_t id) const;
    QueuePage queue_page(int offset, int limit) const;   // creation order
    int queue_count_open() const;                        // every state except done
    // Queued rows of this account whose dependency (if any) is done: what a run can take.
    int queue_count_sendable(const std::string& server, const std::string& user_id) const;
    std::optional<QueueItem> queue_next(const std::string& server, const std::string& user_id) const;   // the oldest of them
    // Writes the fields a run or the user may change (state, step, the upload fields, reason, names, replace and ETags, depends_on, results). The
    // account, the kind and the creation time never change. A new name is validated. A row that is gone is an error.
    Error queue_update(const QueueItem& item);
    // Removes a row. A row that is being sent is refused; the queued dependants of the removed row become failed with the reason target_gone.
    // Idempotent for a row that is not there.
    Error queue_remove(int64_t id);
    Error queue_clear_done();
    // Is `name` taken in `folder`: by an entry of the cached listing, by the target of an open item of the current account (a new file or folder, the
    // target of a move, a copy or a rename). The server is not asked.
    NameUse name_use(const std::string& folder, const std::string& name) const;
    // Gives a failed or held upload, new folder, move or copy another target folder (its old one is gone, or the user chose again): the item is queued again,
    // waits for a queued folder of the new target, and what waits for a new folder that moves with it follows (by prefix). Errors: protocol_error, name_exists.
    Error queue_retarget(int64_t id, const std::string& new_parent);
    // The open item that is going to create this folder (an upload into it waits for that item), if there is one.
    std::optional<int64_t> queue_pending_mkdir(const std::string& folder_path) const;
    // One transaction: the item becomes done with the version the server reported, and the effects of research R-10 are applied to the cache (a new file or
    // folder enters the cached rows of its folder, a replaced one is updated) without touching the age of the listing. A folder that is not cached is not
    // patched: the next visit fetches. A rename or move takes the entry's downloaded copy, its offline mark and its favorite along (a folder: everything
    // below it by prefix); the file on the card is never moved. A delete removes the cached rows only. See research R-10.
    Error queue_confirm(int64_t id, const std::string& etag, std::optional<int64_t> fileid, const ConfirmContext& context);
    // At start, no network: Sending rows with the step prepare or chunks go back to queued, every other Sending row becomes unknown (no_answer).
    RecoveryReport queue_recover();
    // One page of a folder like entries_page, with the open queue items of the current account merged in: new files and folders and the targets of
    // moves and copies appear as pending rows (also in a folder that was never cached), a rename shows the new name under the old identity, a move,
    // a delete and a replace mark the existing row. Done items are not an overlay (their effects are in the cache). Sorting and paging are done in SQL.
    OverlayPage entries_page_overlay(const std::string& folder, SortKey key, bool ascending, int offset, int limit) const;
    // Open rows that name this entry as their source or their target, and open rows about anything below this folder (for the warning that a
    // delete of the folder makes them fail).
    std::vector<QueueItem> queue_items_for_source(const std::string& remote_path) const;
    std::vector<QueueItem> queue_items_below(const std::string& folder) const;

    // Test seams (the product code uses the typed methods). debug_exec returns the
    // SQLite error text ("" on success); debug_query returns the first column of the first row as text.
    std::string debug_exec(const std::string& sql);
    std::string debug_query(const std::string& sql) const;

private:
    Store(sqlite3* db, std::string dir, Clock& clock, int max_entries, int max_folders)
        : db_(db), dir_(std::move(dir)), clock_(&clock), max_entries_(max_entries), max_folders_(max_folders) {}

    // One SQLite connection is shared by the UI thread (reads) and the worker thread (sync). Every public method
    // holds this mutex for its whole transaction, so a reader never sees a half-applied batch.
    std::optional<ServerCaps> server_caps_locked() const;   // the mutex is already held
    Error validate_queue_item(const QueueItem& item, const ServerCaps& caps) const;
    mutable std::mutex mu_;
    sqlite3* db_ = nullptr;
    bool search_table_ = false;   // the temporary table of search results exists in this connection
    std::string dir_;
    Clock* clock_ = nullptr;
    int max_entries_ = kDefaultCachedEntries;
    int max_folders_ = kDefaultCachedFolders;
};

}  // namespace pbc
