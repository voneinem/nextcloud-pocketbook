#pragma once
// The SQLite store base. One connection; every method is a short transaction. This baseline holds the settings table and the
// sync state row; the tables of the product arrive with its migrations (store_migrations.cpp).
//
// Principle III: a database with a newer schema than this build knows is refused and never modified; upgrades are
// one-way, run in one transaction after a backup copy, and are followed by an integrity check.

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pbc/types.h"

struct sqlite3;

namespace pbc {

struct Migration {
    int from = 0;
    int to = 0;
    std::string sql;   // run inside the migration transaction
};

// The upgrade steps this build knows (store_migrations.cpp). Empty in the baseline: a fresh database is schema 1.
const std::vector<Migration>& builtin_migrations();

struct StoreOptions {
    int target_version = 0;                 // 0 = the version of this build (Store::kSchemaVersion)
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
    static constexpr int kSchemaVersion = 1;   // 1: settings and sync state

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

    // Test seams (the product code uses the typed methods). debug_exec returns the
    // SQLite error text ("" on success); debug_query returns the first column of the first row as text.
    std::string debug_exec(const std::string& sql);
    std::string debug_query(const std::string& sql) const;

private:
    Store(sqlite3* db, std::string dir, Clock& clock) : db_(db), dir_(std::move(dir)), clock_(&clock) {}

    // One SQLite connection is shared by the UI thread (reads) and the worker thread (sync). Every public method
    // holds this mutex for its whole transaction, so a reader never sees a half-applied batch.
    mutable std::mutex mu_;
    sqlite3* db_ = nullptr;
    std::string dir_;
    Clock* clock_ = nullptr;
};

}  // namespace pbc
