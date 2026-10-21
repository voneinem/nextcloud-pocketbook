// The files client's part of the store: the account row (data-model.md). Every public method is one short transaction.
#include <sqlite3.h>
#include <sys/stat.h>

#include <algorithm>
#include <set>

#include "pbc/log.h"
#include "pbc/nc_server.h"
#include "pbc/remote_path.h"
#include "pbc/utf8.h"
#include "pbc/store.h"
#include "store_sql.h"
#include "store_util.h"

namespace pbc {

using sql::Stmt;
using store_util::bind_opt;
using store_util::cap;
using store_util::opt_col;
using store_util::storage_error;

namespace {

Error bad(const char* key, const char* detail) { return make_error(ErrorClass::Protocol, key, detail); }

std::string clean(const std::string& s, size_t max) {
    std::string out;
    for (unsigned char c : s)
        if (c >= 0x20 && c != 0x7f) out.push_back(static_cast<char>(c));
    return cap(out, max);
}

bool has_control(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f) return true;
    return false;
}

}  // namespace

std::optional<AccountRow> Store::account() const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "SELECT base_url, login_name, user_id, server_name, server_version, quota_used, quota_total, checked_at, state, problem_key"
            " FROM account WHERE id = 1");
    if (!st.ok() || st.step() != SQLITE_ROW) return std::nullopt;
    AccountRow a;
    a.base_url = st.text(0);
    a.login_name = st.text(1);
    a.user_id = st.text(2);
    a.server_name = st.text(3);
    a.server_version = st.text(4);
    a.quota_used = opt_col(st, 5);
    a.quota_total = opt_col(st, 6);
    a.checked_at = opt_col(st, 7);
    a.state = st.text(8);
    a.problem_key = st.text(9);
    return a;
}

Error Store::save_account(const AccountRow& row) {
    // validation first: nothing reaches the database unless it is usable
    const auto canonical = canonical_base_url(row.base_url);
    if (row.base_url.empty() || !canonical.ok() || canonical.value != row.base_url) return make_error(ErrorClass::Unsupported, "address_invalid", "the address is not a valid https address");
    if (row.user_id.empty() || row.user_id.size() > 256 || !is_valid_name(row.user_id)) return bad("protocol_error", "the user id is not usable");
    if (row.login_name.empty() || row.login_name.size() > 256 || has_control(row.login_name)) return bad("protocol_error", "the login name is not usable");
    if (row.state != "untested" && row.state != "ok" && row.state != "needs_signin" && row.state != "problem") return bad("protocol_error", "unknown account state");

    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    const std::string owner = row.base_url + "\n" + row.user_id;
    {
        Stmt st(db_, "SELECT value FROM settings WHERE key = 'cache_owner'");
        if (!st.ok()) return storage_error(db_, "write_failed");
        const std::string previous = st.step() == SQLITE_ROW ? st.text(0) : std::string();
        if (!previous.empty() && previous != owner) {   // the cached listings belong to another server or user
            std::string err;
            if (!sql::exec(db_, "DELETE FROM folders; DELETE FROM favorites; DELETE FROM server_caps; DELETE FROM settings WHERE key = 'favorites_fetched_at';", &err)) return storage_error(db_, "write_failed");
        }
    }
    {
        Stmt st(db_, "INSERT INTO settings(key, value) VALUES('cache_owner', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_text(1, owner);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    {
        Stmt st(db_,
                "INSERT OR REPLACE INTO account(id, base_url, login_name, user_id, server_name, server_version, quota_used, quota_total,"
                " checked_at, state, problem_key) VALUES(1,?,?,?,?,?,?,?,?,?,?)");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_text(1, row.base_url);
        st.bind_text(2, row.login_name);
        st.bind_text(3, row.user_id);
        st.bind_text(4, clean(row.server_name, 64));
        st.bind_text(5, clean(row.server_version, 32));
        bind_opt(st, 6, row.quota_used);
        bind_opt(st, 7, row.quota_total);
        bind_opt(st, 8, row.checked_at);
        st.bind_text(9, row.state);
        st.bind_text(10, clean(row.problem_key, 64));
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::clear_account() {
    std::lock_guard<std::mutex> lock(mu_);
    std::string err;
    if (!sql::exec(db_, "DELETE FROM account; DELETE FROM server_caps;", &err)) return storage_error(db_, "write_failed");
    return {};
}

Error Store::touch_account_checked() {
    const int64_t now = clock_->now();
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "UPDATE account SET checked_at = ? WHERE id = 1");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_int64(1, now);
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return {};
}

Error Store::set_account_state(const std::string& state, const std::string& problem_key) {
    if (state != "untested" && state != "ok" && state != "needs_signin" && state != "problem") return bad("protocol_error", "unknown account state");
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "UPDATE account SET state = ?, problem_key = ? WHERE id = 1");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_text(1, state);
    st.bind_text(2, clean(problem_key, 64));
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    if (sqlite3_changes(db_) == 0) return make_error(ErrorClass::Storage, "no_account", "there is no account");
    return {};
}

// ---------------------------------------------------------------------------------------------- cached listings

namespace {

const char* kind_text(Kind k) { return k == Kind::Folder ? "folder" : "file"; }
const char* availability_text(Availability a) {
    switch (a) {
        case Availability::NoPermission: return "no_permission";
        case Availability::Encrypted: return "encrypted";
        case Availability::Unsupported: return "unsupported";
        case Availability::Unreadable: return "unreadable";
        default: return "ok";
    }
}
Availability availability_from(const std::string& s) {
    if (s == "no_permission") return Availability::NoPermission;
    if (s == "encrypted") return Availability::Encrypted;
    if (s == "unsupported") return Availability::Unsupported;
    if (s == "unreadable") return Availability::Unreadable;
    return Availability::Ok;
}

RemoteEntry entry_from_row(sql::Stmt& st, const std::string& parent) {
    RemoteEntry e;
    e.name = st.text(0);
    e.path = join_remote(parent, e.name);
    e.kind = st.text(1) == "folder" ? Kind::Folder : Kind::File;
    e.size = store_util::opt_col(st, 2);
    e.mtime = store_util::opt_col(st, 3);
    e.etag = st.text(4);
    e.fileid = store_util::opt_col(st, 5);
    e.mime = st.text(6);
    e.permissions = st.text(7);
    e.favorite = st.int64(8) != 0;
    e.checksum = st.text(9);
    e.availability = availability_from(st.text(10));
    e.reason = st.text(11);
    return e;
}

const char* kEntryColumns = "name, kind, size, mtime, etag, fileid, mime, permissions, favorite, checksum, availability, reason";

}  // namespace

Error Store::replace_listing(const std::string& folder_path, const Listing& listing, int64_t fetched_at) {
    if (listing.entries.size() > static_cast<size_t>(kMaxEntriesPerFolder)) return make_error(ErrorClass::Protocol, "listing_too_large", "the folder has too many entries");
    const std::string folder = normalize_remote_path(folder_path);
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    const int64_t now = clock_->now();
    {
        Stmt st(db_,
                "INSERT INTO folders(path, etag, fileid, fetched_at, last_access_at, entry_count, gone) VALUES(?,?,?,?,?,0,0)"
                " ON CONFLICT(path) DO UPDATE SET etag = excluded.etag, fileid = excluded.fileid, fetched_at = excluded.fetched_at,"
                " last_access_at = excluded.last_access_at, gone = 0");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_text(1, folder);
        st.bind_text(2, cap(listing.folder_etag, 256));
        bind_opt(st, 3, listing.folder_fileid);
        st.bind_int64(4, fetched_at);
        st.bind_int64(5, now);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    {
        Stmt st(db_, "DELETE FROM entries WHERE parent = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_text(1, folder);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    int stored = 0;
    {
        Stmt st(db_,
                "INSERT INTO entries(parent, name, kind, size, mtime, etag, fileid, mime, permissions, favorite, checksum, availability, reason)"
                " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
        if (!st.ok()) return storage_error(db_, "write_failed");
        for (const RemoteEntry& e : listing.entries) {
            if (!is_valid_name(e.name)) continue;   // a name that cannot be one path segment is never stored
            sqlite3_reset(st.get());
            st.bind_text(1, folder);
            st.bind_text(2, e.name);
            st.bind_text(3, kind_text(e.kind));
            bind_opt(st, 4, e.size);
            bind_opt(st, 5, e.mtime);
            st.bind_text(6, cap(e.etag, 256));
            bind_opt(st, 7, e.fileid);
            st.bind_text(8, cap(e.mime, 100));
            st.bind_text(9, cap(e.permissions, 32));
            st.bind_int64(10, e.favorite ? 1 : 0);
            st.bind_text(11, cap(e.checksum, 256));
            st.bind_text(12, availability_text(e.availability));
            st.bind_text(13, cap(e.reason, 64));
            if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");   // for example two entries of one name
            ++stored;
        }
    }
    {
        Stmt st(db_, "UPDATE folders SET entry_count = ? WHERE path = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, stored);
        st.bind_text(2, folder);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    // the bounds of the cache: the oldest folders go, whole, never the root and never the folder just written
    for (;;) {
        long long entries = 0, folders = 0;
        {
            Stmt st(db_, "SELECT COALESCE(SUM(entry_count), 0), COUNT(*) FROM folders");
            if (!st.ok() || st.step() != SQLITE_ROW) return storage_error(db_, "write_failed");
            entries = st.int64(0);
            folders = st.int64(1);
        }
        if (entries <= max_entries_ && folders <= max_folders_) break;
        std::string victim;
        {
            Stmt st(db_, "SELECT path FROM folders WHERE path != '/' AND path != ? ORDER BY last_access_at ASC, rowid ASC LIMIT 1");
            if (!st.ok()) return storage_error(db_, "write_failed");
            st.bind_text(1, folder);
            if (st.step() != SQLITE_ROW) break;   // nothing more can be dropped
            victim = st.text(0);
        }
        Stmt del(db_, "DELETE FROM folders WHERE path = ?");
        if (!del.ok()) return storage_error(db_, "write_failed");
        del.bind_text(1, victim);
        if (del.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        log::event("cache.evict", {log::count("folders", 1)});
    }
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::mark_folder_gone(const std::string& folder_path) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "UPDATE folders SET gone = 1 WHERE path = ?");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_text(1, normalize_remote_path(folder_path));
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return {};
}

std::optional<FolderRow> Store::folder(const std::string& path) {
    const std::string p = normalize_remote_path(path);
    std::lock_guard<std::mutex> lock(mu_);
    {
        Stmt st(db_, "UPDATE folders SET last_access_at = ? WHERE path = ?");
        if (st.ok()) {
            st.bind_int64(1, clock_->now());
            st.bind_text(2, p);
            st.step();
        }
    }
    Stmt st(db_, "SELECT path, etag, fileid, fetched_at, last_access_at, entry_count, gone FROM folders WHERE path = ?");
    if (!st.ok()) return std::nullopt;
    st.bind_text(1, p);
    if (st.step() != SQLITE_ROW) return std::nullopt;
    FolderRow f;
    f.path = st.text(0);
    f.etag = st.text(1);
    f.fileid = opt_col(st, 2);
    f.fetched_at = opt_col(st, 3);
    f.last_access_at = st.int64(4);
    f.entry_count = static_cast<int>(st.int64(5));
    f.gone = st.int64(6) != 0;
    return f;
}

EntryPage Store::entries_page(const std::string& folder_path, SortKey key, bool ascending, int offset, int limit) const {
    EntryPage page;
    const std::string folder = normalize_remote_path(folder_path);
    std::lock_guard<std::mutex> lock(mu_);
    {
        Stmt st(db_, "SELECT COUNT(*) FROM entries WHERE parent = ?");
        if (!st.ok()) return page;
        st.bind_text(1, folder);
        if (st.step() == SQLITE_ROW) page.total = static_cast<int>(st.int64(0));
    }
    const char* dir = ascending ? "ASC" : "DESC";
    std::string order = "(kind = 'folder') DESC, ";   // folders first, whatever the direction
    switch (key) {
        case SortKey::Name: order += std::string("pbc_fold(name) ") + dir + ", name " + dir; break;
        case SortKey::Date: order += std::string("COALESCE(mtime, 0) ") + dir + ", pbc_fold(name) ASC, name ASC"; break;
        case SortKey::Size: order += std::string("COALESCE(size, -1) ") + dir + ", pbc_fold(name) ASC, name ASC"; break;
    }
    const std::string sql = std::string("SELECT ") + kEntryColumns + " FROM entries WHERE parent = ? ORDER BY " + order + " LIMIT ? OFFSET ?";
    Stmt st(db_, sql.c_str());
    if (!st.ok()) return page;
    st.bind_text(1, folder);
    st.bind_int64(2, limit < 0 ? 0 : limit);
    st.bind_int64(3, offset < 0 ? 0 : offset);
    while (st.step() == SQLITE_ROW) page.rows.push_back(entry_from_row(st, folder));
    return page;
}

std::optional<RemoteEntry> Store::entry(const std::string& path) const {
    const std::string p = normalize_remote_path(path);
    if (p == "/") return std::nullopt;
    const std::string parent = parent_of(p);
    const std::string name = p.substr(p.rfind('/') + 1);
    std::lock_guard<std::mutex> lock(mu_);
    {
        const std::string sql = std::string("SELECT ") + kEntryColumns + " FROM entries WHERE parent = ? AND name = ?";
        Stmt st(db_, sql.c_str());
        if (!st.ok()) return std::nullopt;
        st.bind_text(1, parent);
        st.bind_text(2, name);
        if (st.step() == SQLITE_ROW) return entry_from_row(st, parent);
    }
    // not in any cached listing: a favorite, or a result of the server search of this run
    {
        Stmt st(db_, "SELECT kind, size, mtime, etag, mime, availability, reason FROM favorites WHERE remote_path = ?");
        if (st.ok()) {
            st.bind_text(1, p);
            if (st.step() == SQLITE_ROW) {
                RemoteEntry e;
                e.path = p;
                e.name = name;
                e.kind = st.text(0) == "folder" ? Kind::Folder : Kind::File;
                e.size = opt_col(st, 1);
                e.mtime = opt_col(st, 2);
                e.etag = st.text(3);
                e.mime = st.text(4);
                e.availability = availability_from(st.text(5));
                e.reason = st.text(6);
                e.favorite = true;
                return e;
            }
        }
    }
    if (search_table_) {
        const std::string sql = std::string("SELECT ") + kEntryColumns + " FROM temp.search_results WHERE parent = ? AND name = ?";
        Stmt st(db_, sql.c_str());
        if (st.ok()) {
            st.bind_text(1, parent);
            st.bind_text(2, name);
            if (st.step() == SQLITE_ROW) return entry_from_row(st, parent);
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------- downloaded copies and the journal

namespace {

LocalFile local_from_row(sql::Stmt& st) {
    LocalFile f;
    f.remote_path = st.text(0);
    f.local_path = st.text(1);
    f.size = st.int64(2);
    f.mtime = st.int64(3);
    f.remote_etag = st.text(4);
    f.remote_size = store_util::opt_col(st, 5);
    f.remote_mtime = store_util::opt_col(st, 6);
    f.downloaded_at = st.int64(7);
    f.server_missing = st.int64(8) != 0;
    f.seen_etag = st.text(9);
    return f;
}

const char* kLocalColumns = "remote_path, local_path, size, mtime, remote_etag, remote_size, remote_mtime, downloaded_at, server_missing, seen_etag";

bool journal_state_ok(const std::string& s) { return s == "running" || s == "waiting" || s == "paused"; }

}  // namespace

Error Store::commit_download(const LocalFile& f, int64_t job_id) {
    if (f.remote_path.empty() || f.local_path.empty()) return make_error(ErrorClass::Protocol, "protocol_error", "a copy needs both paths");
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    {
        Stmt st(db_,
                "INSERT INTO local_files(remote_path, local_path, size, mtime, remote_etag, remote_size, remote_mtime, downloaded_at, server_missing, seen_etag)"
                " VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(remote_path) DO UPDATE SET local_path = excluded.local_path, size = excluded.size,"
                " mtime = excluded.mtime, remote_etag = excluded.remote_etag, remote_size = excluded.remote_size, remote_mtime = excluded.remote_mtime,"
                " downloaded_at = excluded.downloaded_at, server_missing = excluded.server_missing, seen_etag = excluded.seen_etag");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_text(1, f.remote_path);
        st.bind_text(2, f.local_path);
        st.bind_int64(3, f.size);
        st.bind_int64(4, f.mtime);
        st.bind_text(5, cap(f.remote_etag, 256));
        bind_opt(st, 6, f.remote_size);
        bind_opt(st, 7, f.remote_mtime);
        st.bind_int64(8, f.downloaded_at);
        st.bind_int64(9, f.server_missing ? 1 : 0);
        st.bind_text(10, cap(f.seen_etag, 256));
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");   // for example a local path that another copy uses
    }
    if (job_id > 0) {
        Stmt st(db_, "DELETE FROM downloads WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, job_id);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

std::optional<LocalFile> Store::local_file(const std::string& remote_path) const {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string sql = std::string("SELECT ") + kLocalColumns + " FROM local_files WHERE remote_path = ?";
    Stmt st(db_, sql.c_str());
    if (!st.ok()) return std::nullopt;
    st.bind_text(1, remote_path);
    if (st.step() != SQLITE_ROW) return std::nullopt;
    return local_from_row(st);
}

std::vector<LocalFile> Store::local_files(int offset, int limit) const {
    std::vector<LocalFile> out;
    std::lock_guard<std::mutex> lock(mu_);
    const std::string sql = std::string("SELECT ") + kLocalColumns + " FROM local_files ORDER BY pbc_fold(local_path), local_path LIMIT ? OFFSET ?";
    Stmt st(db_, sql.c_str());
    if (!st.ok()) return out;
    st.bind_int64(1, limit < 0 ? 0 : limit);
    st.bind_int64(2, offset < 0 ? 0 : offset);
    while (st.step() == SQLITE_ROW) out.push_back(local_from_row(st));
    return out;
}

int Store::local_file_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "SELECT COUNT(*) FROM local_files");
    return st.ok() && st.step() == SQLITE_ROW ? static_cast<int>(st.int64(0)) : 0;
}

std::vector<std::pair<std::string, std::string>> Store::local_path_pairs() const {
    std::vector<std::pair<std::string, std::string>> out;
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "SELECT remote_path, local_path FROM local_files");
    if (!st.ok()) return out;
    while (st.step() == SQLITE_ROW) out.emplace_back(st.text(0), st.text(1));
    return out;
}

Error Store::forget_local_file(const std::string& remote_path) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "DELETE FROM local_files WHERE remote_path = ?");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_text(1, remote_path);
    return st.step() == SQLITE_DONE ? Error() : storage_error(db_, "write_failed");
}

Error Store::set_server_missing(const std::string& remote_path, bool missing) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "UPDATE local_files SET server_missing = ? WHERE remote_path = ?");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_int64(1, missing ? 1 : 0);
    st.bind_text(2, remote_path);
    return st.step() == SQLITE_DONE ? Error() : storage_error(db_, "write_failed");
}

Error Store::set_seen_etag(const std::string& remote_path, const std::string& etag) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "UPDATE local_files SET seen_etag = ? WHERE remote_path = ?");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_text(1, cap(etag, 256));
    st.bind_text(2, remote_path);
    return st.step() == SQLITE_DONE ? Error() : storage_error(db_, "write_failed");
}

Result<int64_t> Store::journal_begin(const DownloadJournalRow& row) {
    Result<int64_t> out;
    if (!journal_state_ok(row.state) || row.remote_path.empty() || row.target_path.empty()) {
        out.error = make_error(ErrorClass::Protocol, "protocol_error", "a journal row needs a known state and both paths");
        return out;
    }
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "INSERT INTO downloads(remote_path, target_path, expected_etag, expected_size, received, state, started_at, updated_at) VALUES(?,?,?,?,?,?,?,?)");
    if (!st.ok()) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    const int64_t now = clock_->now();
    st.bind_text(1, row.remote_path);
    st.bind_text(2, row.target_path);
    st.bind_text(3, cap(row.expected_etag, 256));
    bind_opt(st, 4, row.expected_size);
    st.bind_int64(5, row.received);
    st.bind_text(6, row.state);
    st.bind_int64(7, row.started_at ? row.started_at : now);
    st.bind_int64(8, row.updated_at ? row.updated_at : now);
    if (st.step() != SQLITE_DONE) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    out.value = sqlite3_last_insert_rowid(db_);
    return out;
}

Error Store::journal_update(int64_t id, int64_t received, const std::string& state) {
    if (!journal_state_ok(state)) return make_error(ErrorClass::Protocol, "protocol_error", "unknown journal state");
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "UPDATE downloads SET received = ?, state = ?, updated_at = ? WHERE id = ?");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_int64(1, received);
    st.bind_text(2, state);
    st.bind_int64(3, clock_->now());
    st.bind_int64(4, id);
    return st.step() == SQLITE_DONE ? Error() : storage_error(db_, "write_failed");
}

Error Store::journal_set_expected(int64_t id, const std::string& etag, std::optional<int64_t> size) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "UPDATE downloads SET expected_etag = ?, expected_size = ?, updated_at = ? WHERE id = ?");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_text(1, cap(etag, 256));
    bind_opt(st, 2, size);
    st.bind_int64(3, clock_->now());
    st.bind_int64(4, id);
    return st.step() == SQLITE_DONE ? Error() : storage_error(db_, "write_failed");
}

Error Store::journal_end(int64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "DELETE FROM downloads WHERE id = ?");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_int64(1, id);
    return st.step() == SQLITE_DONE ? Error() : storage_error(db_, "write_failed");
}

std::vector<DownloadJournalRow> Store::journal() const {
    std::vector<DownloadJournalRow> out;
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "SELECT id, remote_path, target_path, expected_etag, expected_size, received, state, started_at, updated_at FROM downloads ORDER BY id");
    if (!st.ok()) return out;
    while (st.step() == SQLITE_ROW) {
        DownloadJournalRow j;
        j.id = st.int64(0);
        j.remote_path = st.text(1);
        j.target_path = st.text(2);
        j.expected_etag = st.text(3);
        j.expected_size = store_util::opt_col(st, 4);
        j.received = st.int64(5);
        j.state = st.text(6);
        j.started_at = st.int64(7);
        j.updated_at = st.int64(8);
        out.push_back(std::move(j));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------- "keep offline" marks

Error Store::set_mark(const std::string& remote_path, Kind kind, bool on) {
    const std::string p = normalize_remote_path(remote_path);
    if (on && p == "/" && kind == Kind::File) return make_error(ErrorClass::Protocol, "protocol_error", "the root is not a file");
    if (on && remote_path.empty() && kind == Kind::File) return make_error(ErrorClass::Protocol, "protocol_error", "a mark needs a path");
    std::lock_guard<std::mutex> lock(mu_);
    if (on) {
        Stmt st(db_, "INSERT INTO offline_marks(remote_path, kind, marked_at) VALUES(?,?,?) ON CONFLICT(remote_path) DO UPDATE SET kind = excluded.kind");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_text(1, p);
        st.bind_text(2, kind_text(kind));
        st.bind_int64(3, clock_->now());
        return st.step() == SQLITE_DONE ? Error() : storage_error(db_, "write_failed");
    }
    Stmt st(db_, "DELETE FROM offline_marks WHERE remote_path = ?");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_text(1, p);
    return st.step() == SQLITE_DONE ? Error() : storage_error(db_, "write_failed");
}

std::vector<MarkRow> Store::marks() const {
    std::vector<MarkRow> out;
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "SELECT remote_path, kind, marked_at FROM offline_marks ORDER BY remote_path");
    if (!st.ok()) return out;
    while (st.step() == SQLITE_ROW) {
        MarkRow m;
        m.remote_path = st.text(0);
        m.kind = st.text(1) == "folder" ? Kind::Folder : Kind::File;
        m.marked_at = st.int64(2);
        out.push_back(std::move(m));
    }
    return out;
}

bool Store::is_marked(const std::string& remote_path) const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "SELECT 1 FROM offline_marks WHERE remote_path = ?");
    if (!st.ok()) return false;
    st.bind_text(1, normalize_remote_path(remote_path));
    return st.step() == SQLITE_ROW;
}

bool Store::in_scope(const std::string& remote_path) const {
    const std::string p = normalize_remote_path(remote_path);
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "SELECT remote_path, kind FROM offline_marks");
    if (!st.ok()) return false;
    while (st.step() == SQLITE_ROW) {
        const std::string m = st.text(0);
        if (m == p) return true;
        if (st.text(1) != "folder") continue;
        const std::string prefix = m == "/" ? std::string("/") : m + "/";
        if (p.size() > prefix.size() && p.compare(0, prefix.size(), prefix) == 0) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------- favorites and search

Error Store::replace_favorites(const std::vector<RemoteEntry>& entries, int64_t fetched_at) {
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    {
        Stmt st(db_, "DELETE FROM favorites");
        if (!st.ok() || st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    {
        Stmt st(db_, "INSERT INTO favorites(remote_path, kind, size, mtime, etag, mime, availability, reason, position) VALUES(?,?,?,?,?,?,?,?,?)");
        if (!st.ok()) return storage_error(db_, "write_failed");
        int position = 0;
        for (const RemoteEntry& e : entries) {
            if (!is_valid_name(e.name) || e.path.empty() || e.path[0] != '/') continue;
            sqlite3_reset(st.get());
            st.bind_text(1, e.path);
            st.bind_text(2, kind_text(e.kind));
            bind_opt(st, 3, e.size);
            bind_opt(st, 4, e.mtime);
            st.bind_text(5, cap(e.etag, 256));
            st.bind_text(6, cap(e.mime, 100));
            st.bind_text(7, availability_text(e.availability));
            st.bind_text(8, cap(e.reason, 64));
            st.bind_int64(9, position++);
            if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");   // for example the same path twice
        }
    }
    {
        Stmt st(db_, "INSERT INTO settings(key, value) VALUES('favorites_fetched_at', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_text(1, std::to_string(fetched_at));
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

std::vector<RemoteEntry> Store::favorites() const {
    std::vector<RemoteEntry> out;
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "SELECT remote_path, kind, size, mtime, etag, mime, availability, reason FROM favorites ORDER BY position");
    if (!st.ok()) return out;
    while (st.step() == SQLITE_ROW) {
        RemoteEntry e;
        e.path = st.text(0);
        e.name = e.path.substr(e.path.rfind('/') + 1);
        e.kind = st.text(1) == "folder" ? Kind::Folder : Kind::File;
        e.size = opt_col(st, 2);
        e.mtime = opt_col(st, 3);
        e.etag = st.text(4);
        e.mime = st.text(5);
        e.availability = availability_from(st.text(6));
        e.reason = st.text(7);
        e.favorite = true;
        out.push_back(std::move(e));
    }
    return out;
}

std::optional<int64_t> Store::favorites_fetched_at() const {
    const std::string v = setting("favorites_fetched_at");
    if (v.empty()) return std::nullopt;
    return std::atoll(v.c_str());
}

namespace {

std::string like_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '%' || c == '_' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

}  // namespace

std::vector<SearchHit> Store::search_cached(const std::string& raw_term, int limit) const {
    std::vector<SearchHit> out;
    size_t a = 0, b = raw_term.size();
    while (a < b && (raw_term[a] == ' ' || raw_term[a] == '\t' || raw_term[a] == '\n')) ++a;
    while (b > a && (raw_term[b - 1] == ' ' || raw_term[b - 1] == '\t' || raw_term[b - 1] == '\n')) --b;
    if (a == b || limit <= 0) return out;
    const std::string folded = sort_key(raw_term.substr(a, b - a));
    const std::string pattern = "%" + like_escape(folded) + "%";
    std::set<std::string> seen;
    std::lock_guard<std::mutex> lock(mu_);
    {
        const std::string sql = std::string("SELECT parent, ") + kEntryColumns + " FROM entries WHERE pbc_fold(name) LIKE ? ESCAPE '\\' ORDER BY pbc_fold(name), parent LIMIT ?";
        Stmt st(db_, sql.c_str());
        if (st.ok()) {
            st.bind_text(1, pattern);
            st.bind_int64(2, limit);
            while (st.step() == SQLITE_ROW) {
                // column 0 is the parent; entry_from_row reads from column 0, so the row is read by hand
                RemoteEntry e;
                const std::string parent = st.text(0);
                e.name = st.text(1);
                e.path = join_remote(parent, e.name);
                e.kind = st.text(2) == "folder" ? Kind::Folder : Kind::File;
                e.size = opt_col(st, 3);
                e.mtime = opt_col(st, 4);
                e.etag = st.text(5);
                e.fileid = opt_col(st, 6);
                e.mime = st.text(7);
                e.permissions = st.text(8);
                e.favorite = st.int64(9) != 0;
                e.checksum = st.text(10);
                e.availability = availability_from(st.text(11));
                e.reason = st.text(12);
                if (seen.insert(e.path).second) out.push_back({std::move(e), "reader"});
            }
        }
    }
    auto matches = [&](const std::string& path) { return sort_key(path.substr(path.rfind('/') + 1)).find(folded) != std::string::npos; };
    {
        Stmt st(db_, "SELECT remote_path, kind, size, mtime, etag, mime, availability, reason FROM favorites ORDER BY position");
        while (st.ok() && st.step() == SQLITE_ROW) {
            const std::string path = st.text(0);
            if (!matches(path) || seen.count(path)) continue;
            RemoteEntry e;
            e.path = path;
            e.name = path.substr(path.rfind('/') + 1);
            e.kind = st.text(1) == "folder" ? Kind::Folder : Kind::File;
            e.size = opt_col(st, 2);
            e.mtime = opt_col(st, 3);
            e.etag = st.text(4);
            e.mime = st.text(5);
            e.availability = availability_from(st.text(6));
            e.reason = st.text(7);
            e.favorite = true;
            seen.insert(path);
            out.push_back({std::move(e), "favorites"});
        }
    }
    {
        Stmt st(db_, "SELECT remote_path, size, mtime, remote_etag FROM local_files ORDER BY pbc_fold(local_path)");
        while (st.ok() && st.step() == SQLITE_ROW) {
            const std::string path = st.text(0);
            if (!matches(path) || seen.count(path)) continue;
            RemoteEntry e;
            e.path = path;
            e.name = path.substr(path.rfind('/') + 1);
            e.kind = Kind::File;
            e.size = st.int64(1);
            e.mtime = st.int64(2);
            e.etag = st.text(3);
            seen.insert(path);
            out.push_back({std::move(e), "download"});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const SearchHit& x, const SearchHit& y) {
        if ((x.entry.kind == Kind::Folder) != (y.entry.kind == Kind::Folder)) return x.entry.kind == Kind::Folder;
        return sort_key(x.entry.name) < sort_key(y.entry.name);
    });
    if (out.size() > static_cast<size_t>(limit)) out.resize(static_cast<size_t>(limit));
    return out;
}

void Store::set_search_results(const std::vector<RemoteEntry>& entries) {
    std::lock_guard<std::mutex> lock(mu_);
    std::string err;
    if (!search_table_) {
        // TEMP: lives in this connection only and is never part of the database file
        if (!sql::exec(db_,
                       "CREATE TEMP TABLE IF NOT EXISTS search_results(parent TEXT NOT NULL, name TEXT NOT NULL, kind TEXT NOT NULL, size INTEGER, mtime INTEGER,"
                       " etag TEXT NOT NULL DEFAULT '', fileid INTEGER, mime TEXT NOT NULL DEFAULT '', permissions TEXT NOT NULL DEFAULT '',"
                       " favorite INTEGER NOT NULL DEFAULT 0, checksum TEXT NOT NULL DEFAULT '', availability TEXT NOT NULL DEFAULT 'ok',"
                       " reason TEXT NOT NULL DEFAULT '', PRIMARY KEY(parent, name))",
                       &err))
            return;
        search_table_ = true;
    }
    sql::exec(db_, "DELETE FROM temp.search_results", &err);
    Stmt st(db_,
            "INSERT OR REPLACE INTO temp.search_results(parent, name, kind, size, mtime, etag, fileid, mime, permissions, favorite, checksum, availability, reason)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!st.ok()) return;
    for (const RemoteEntry& e : entries) {
        if (!is_valid_name(e.name) || e.path.empty() || e.path[0] != '/') continue;
        sqlite3_reset(st.get());
        st.bind_text(1, parent_of(e.path));
        st.bind_text(2, e.name);
        st.bind_text(3, kind_text(e.kind));
        bind_opt(st, 4, e.size);
        bind_opt(st, 5, e.mtime);
        st.bind_text(6, cap(e.etag, 256));
        bind_opt(st, 7, e.fileid);
        st.bind_text(8, cap(e.mime, 100));
        st.bind_text(9, cap(e.permissions, 32));
        st.bind_int64(10, e.favorite ? 1 : 0);
        st.bind_text(11, cap(e.checksum, 256));
        st.bind_text(12, availability_text(e.availability));
        st.bind_text(13, cap(e.reason, 64));
        st.step();
    }
}

// ---------------------------------------------------------------------------------------------- space

Error Store::clear_cache() {
    std::lock_guard<std::mutex> lock(mu_);
    std::string err;
    {
        store_util::Transaction tx(db_);
        if (!tx.open) return storage_error(db_, "write_failed");
        if (!sql::exec(db_, "DELETE FROM entries", &err) || !sql::exec(db_, "DELETE FROM folders", &err)) return storage_error(db_, "write_failed");
        if (!tx.commit()) return storage_error(db_, "write_failed");
    }
    if (search_table_) sql::exec(db_, "DELETE FROM temp.search_results", &err);
    sql::exec(db_, "VACUUM", &err);   // the file shrinks; a failure here only leaves it larger
    log::event("cache.cleared", {});
    return {};
}

int64_t Store::local_files_bytes() const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "SELECT COALESCE(SUM(size), 0) FROM local_files");
    return st.ok() && st.step() == SQLITE_ROW ? st.int64(0) : 0;
}

int64_t Store::database_bytes() const {
    struct stat sb;
    return ::stat((dir_ + "/pbcloud.db").c_str(), &sb) == 0 ? static_cast<int64_t>(sb.st_size) : 0;
}

}  // namespace pbc
