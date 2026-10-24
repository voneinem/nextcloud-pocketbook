// The stored copy of the last trash list (specs/006-trash-and-versions, research R-4, data-model.md "Stored table (schema 6)"): replaced as a whole in
// one transaction, bounded to kMaxTrashRows rows, bound to the account it was fetched for. It is a cache of server state: nothing here is user data.
#include <sqlite3.h>

#include <algorithm>

#include "pbc/store.h"
#include "store_sql.h"
#include "store_util.h"

namespace pbc {

using sql::Stmt;
using store_util::bind_opt;
using store_util::opt_col;
using store_util::storage_error;

namespace {

constexpr int kFlagUnreadable = 1;
constexpr int kFlagOriginalUnknown = 2;
constexpr int kAvailabilityShift = 2;

Error set_setting_locked(sqlite3* db, const char* key, const std::string& value) {
    Stmt st(db, "INSERT INTO settings(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    if (!st.ok()) return storage_error(db, "write_failed");
    st.bind_text(1, key);
    st.bind_text(2, value);
    if (st.step() != SQLITE_DONE) return storage_error(db, "write_failed");
    return {};
}

std::string setting_locked(sqlite3* db, const char* key) {
    Stmt st(db, "SELECT value FROM settings WHERE key = ?");
    if (!st.ok()) return std::string();
    st.bind_text(1, key);
    return st.step() == SQLITE_ROW ? st.text(0) : std::string();
}

int availability_from(int bits) {
    return bits >= static_cast<int>(Availability::Ok) && bits <= static_cast<int>(Availability::Unreadable) ? bits : static_cast<int>(Availability::Ok);
}

}  // namespace

Error Store::replace_trash(const TrashList& list, int64_t fetched_at, int64_t account_id) {
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    std::string err;
    if (!sql::exec(db_, "DELETE FROM trash_items", &err)) return storage_error(db_, "write_failed");
    const size_t keep = std::min(list.items.size(), kMaxTrashRows);
    {
        Stmt st(db_,
                "INSERT INTO trash_items(position, trash_name, name, original_dir, original_name, deleted_at, kind, size, fileid, flags)"
                " VALUES(?,?,?,?,?,?,?,?,?,?)");
        if (!st.ok()) return storage_error(db_, "write_failed");
        for (size_t i = 0; i < keep; ++i) {
            const TrashItem& it = list.items[i];
            sqlite3_reset(st.get());
            sqlite3_clear_bindings(st.get());
            st.bind_int64(1, static_cast<long long>(i));
            st.bind_text(2, it.trash_name);
            st.bind_text(3, it.name);
            store_util::bind_text_or_null(st, 4, it.original_dir);
            store_util::bind_text_or_null(st, 5, it.original_name);
            bind_opt(st, 6, it.deleted_at);
            st.bind_int64(7, it.kind == Kind::Folder ? 1 : 0);
            bind_opt(st, 8, it.size);
            bind_opt(st, 9, it.fileid);
            const int flags = (it.unreadable ? kFlagUnreadable : 0) | (it.original_known ? 0 : kFlagOriginalUnknown) |
                              (static_cast<int>(it.availability) << kAvailabilityShift);
            st.bind_int64(10, flags);
            if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        }
    }
    const bool truncated = list.truncated || list.items.size() > kMaxTrashRows;
    if (Error e = set_setting_locked(db_, "trash_fetched_at", std::to_string(fetched_at)); !e.ok()) return e;
    if (Error e = set_setting_locked(db_, "trash_truncated", truncated ? "1" : "0"); !e.ok()) return e;
    if (Error e = set_setting_locked(db_, "trash_account", std::to_string(account_id)); !e.ok()) return e;
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Result<TrashList> Store::load_trash(int64_t account_id) const {
    Result<TrashList> out;
    std::lock_guard<std::mutex> lock(mu_);
    if (setting_locked(db_, "trash_account") != std::to_string(account_id)) return out;   // nothing stored, or the list of another account
    const std::string fetched = setting_locked(db_, "trash_fetched_at");
    if (fetched.empty()) return out;
    TrashList& l = out.value;
    l.from_store = true;
    l.fetched_at = std::atoll(fetched.c_str());
    l.truncated = setting_locked(db_, "trash_truncated") == "1";
    Stmt st(db_, "SELECT trash_name, name, original_dir, original_name, deleted_at, kind, size, fileid, flags FROM trash_items ORDER BY position");
    if (!st.ok()) {
        out.error = storage_error(db_, "read_failed");
        return out;
    }
    while (st.step() == SQLITE_ROW) {
        TrashItem it;
        it.trash_name = st.text(0);
        it.name = st.text(1);
        it.original_dir = st.text(2);
        it.original_name = st.text(3);
        it.deleted_at = opt_col(st, 4);
        it.kind = st.int64(5) == 1 ? Kind::Folder : Kind::File;
        it.size = opt_col(st, 6);
        it.fileid = opt_col(st, 7);
        const int flags = static_cast<int>(st.int64(8));
        it.unreadable = (flags & kFlagUnreadable) != 0;
        it.original_known = (flags & kFlagOriginalUnknown) == 0;
        it.availability = static_cast<Availability>(availability_from((flags >> kAvailabilityShift) & 7));
        l.items.push_back(std::move(it));
    }
    l.total_seen = static_cast<int>(l.items.size());
    return out;
}

Error Store::remove_trash_item(std::string_view trash_name) {
    if (trash_name.empty()) return {};   // an unreadable item has no name that could identify it
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "DELETE FROM trash_items WHERE trash_name = ?");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_text(1, std::string(trash_name));
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return {};
}

Error Store::clear_trash() {
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    std::string err;
    if (!sql::exec(db_, "DELETE FROM trash_items; DELETE FROM settings WHERE key IN ('trash_fetched_at', 'trash_truncated', 'trash_account');", &err))
        return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

}  // namespace pbc
