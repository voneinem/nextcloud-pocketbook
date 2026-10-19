// Store: the settings table and the sync state row. Every public method is one short transaction; a failing statement
// leaves the complete before-state.
#include <sqlite3.h>

#include "pbc/store.h"
#include "store_sql.h"
#include "store_util.h"

namespace pbc {

using sql::Stmt;
using store_util::bind_opt;
using store_util::cap;
using store_util::opt_col;
using store_util::storage_error;

std::string Store::setting(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "SELECT value FROM settings WHERE key = ?");
    if (!st.ok()) return std::string();
    st.bind_text(1, key);
    return st.step() == SQLITE_ROW ? st.text(0) : std::string();
}

Error Store::set_setting(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "INSERT INTO settings(key, value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_text(1, cap(key, 128));
    st.bind_text(2, cap(value, 4096));
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return {};
}

// ---------------------------------------------------------------------------------------------- sync state

Error Store::set_clock_skew(std::optional<int64_t> skew) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "INSERT INTO sync_state(id, clock_skew_s) VALUES(1, ?) ON CONFLICT(id) DO UPDATE SET clock_skew_s = excluded.clock_skew_s");
    if (!st.ok()) return storage_error(db_, "write_failed");
    bind_opt(st, 1, skew);
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return {};
}

SyncState Store::sync_state() const {
    std::lock_guard<std::mutex> lock(mu_);
    SyncState s;
    Stmt st(db_,
            "SELECT last_started, last_finished, last_success, outcome, problem_key, problem_area, counts_json, clock_skew_s"
            " FROM sync_state WHERE id = 1");
    if (!st.ok() || st.step() != SQLITE_ROW) return s;
    s.last_started = opt_col(st, 0);
    s.last_finished = opt_col(st, 1);
    s.last_success = opt_col(st, 2);
    s.outcome = st.text(3);
    s.problem_key = st.text(4);
    s.problem_area = st.text(5);
    s.counts_json = st.text(6);
    s.clock_skew_s = opt_col(st, 7);
    return s;
}

Error Store::write_sync_state(const SyncState& s) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "INSERT INTO sync_state(id, last_started, last_finished, last_success, outcome, problem_key, problem_area, counts_json,"
            " clock_skew_s) VALUES(1,?,?,?,?,?,?,?,?)"
            " ON CONFLICT(id) DO UPDATE SET last_started=excluded.last_started, last_finished=excluded.last_finished,"
            " last_success=excluded.last_success, outcome=excluded.outcome, problem_key=excluded.problem_key,"
            " problem_area=excluded.problem_area, counts_json=excluded.counts_json, clock_skew_s=excluded.clock_skew_s");
    if (!st.ok()) return storage_error(db_, "write_failed");
    bind_opt(st, 1, s.last_started);
    bind_opt(st, 2, s.last_finished);
    bind_opt(st, 3, s.last_success);
    st.bind_text(4, s.outcome);
    st.bind_text(5, s.problem_key);
    st.bind_text(6, s.problem_area);
    st.bind_text(7, s.counts_json);
    bind_opt(st, 8, s.clock_skew_s);
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return {};
}

}  // namespace pbc
