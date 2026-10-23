// The sync part of the store (specs/003-folder-sync/data-model.md): pairs and their lock, the records, the plans and their paged view,
// the plan state machine and the recovery at start. Every public method is one short transaction; a failing statement leaves the
// complete before-state. The user's files are never copied here: only identities (versions, sizes, times, hashes).
#include <sqlite3.h>

#include <algorithm>

#include "pbc/error_keys.h"
#include "pbc/log.h"
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

Error refuse(const char* key) { return error_from_key(key); }

const char* kPairColumns =
    "id, server, user_id, remote_folder, local_folder, direction, max_files, max_bytes, startup_sync, auto_sync, state, problem_key,"
    " last_compared_at, last_sync_at, last_outcome, last_result_json, held_files, held_bytes, created_at";

SyncPair read_pair(const Stmt& st) {
    SyncPair p;
    p.id = st.int64(0);
    p.server = st.text(1);
    p.user_id = st.text(2);
    p.remote_folder = st.text(3);
    p.local_folder = st.text(4);
    p.direction = parse_direction(st.text(5)).value_or(Direction::Down);
    p.max_files = st.int64(6);
    p.max_bytes = st.int64(7);
    p.startup_sync = st.int64(8) != 0;
    p.auto_sync = st.int64(9) != 0;
    p.state = st.text(10);
    p.problem_key = st.text(11);
    p.last_compared_at = opt_col(st, 12);
    p.last_sync_at = opt_col(st, 13);
    p.last_outcome = st.text(14);
    p.last_result_json = st.text(15);
    p.held_files = opt_col(st, 16);
    p.held_bytes = opt_col(st, 17);
    p.created_at = st.int64(18);
    return p;
}

bool has_control(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f) return true;
    return false;
}

// The checks the store itself makes for every pair that is written; the folder rules (overlap, forbidden folders) are those of sync_rules.h.
Error validate_pair(const SyncPair& p) {
    if (p.max_files < kSyncMinFiles || p.max_files > kSyncMaxFilesLimit || p.max_bytes < kSyncMinBytes || p.max_bytes > kSyncMaxBytesLimit)
        return refuse("pair_limit_invalid");
    if (p.remote_folder.empty() || p.remote_folder[0] != '/' || p.remote_folder.size() > 4096 || has_control(p.remote_folder))
        return make_error(ErrorClass::Protocol, "protocol_error", "the server folder is not usable");
    if (p.local_folder.empty() || p.local_folder[0] != '/' || p.local_folder.size() > 4096 || has_control(p.local_folder))
        return make_error(ErrorClass::Protocol, "protocol_error", "the reader folder is not usable");
    if (p.server.empty() || p.user_id.empty()) return make_error(ErrorClass::Protocol, "protocol_error", "the pair has no account");
    return {};
}

int64_t scalar(sqlite3* db, const char* sql, int64_t fallback = 0) {
    Stmt st(db, sql);
    return st.ok() && st.step() == SQLITE_ROW ? st.int64(0) : fallback;
}

bool pair_exists(sqlite3* db, int64_t id) {
    Stmt st(db, "SELECT 1 FROM sync_pairs WHERE id = ?");
    if (!st.ok()) return false;
    st.bind_int64(1, id);
    return st.step() == SQLITE_ROW;
}

std::string join_choices(const std::set<Choice>& allowed) {
    std::string out;
    for (Choice c : {Choice::TransferAgain, Choice::Leave, Choice::Delete})
        if (allowed.count(c)) out += (out.empty() ? "" : ",") + std::string(choice_text(c));
    return out;
}

std::set<Choice> split_choices(const std::string& text) {
    std::set<Choice> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t comma = text.find(',', pos);
        const std::string part = text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (auto c = parse_choice(part); c && *c != Choice::None) out.insert(*c);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}

const char* kFinal = "('done','interrupted','cancelled','superseded')";

// Keeps the newest kSyncFinalPlansKept finished plans of a pair for the result screen and removes the older ones with their entries.
bool prune_final_plans(sqlite3* db, int64_t pair_id) {
    const std::string keep = std::string("SELECT id FROM sync_plans WHERE pair_id = ?1 AND state IN ") + kFinal + " ORDER BY id DESC LIMIT " +
                             std::to_string(kSyncFinalPlansKept);
    const std::string old = std::string("SELECT id FROM sync_plans WHERE pair_id = ?1 AND state IN ") + kFinal + " AND id NOT IN (" + keep + ")";
    for (const std::string& sql : {"DELETE FROM sync_plan_entries WHERE plan_id IN (" + old + ")", "DELETE FROM sync_plans WHERE id IN (" + old + ")"}) {
        Stmt st(db, sql.c_str());
        if (!st.ok()) return false;
        st.bind_int64(1, pair_id);
        if (st.step() != SQLITE_DONE) return false;
    }
    return true;
}

PlanEntry read_entry(const Stmt& st) {
    PlanEntry e;
    e.seq = static_cast<int>(st.int64(0));
    e.rel = st.text(1);
    e.local_rel = st.text(2);
    e.kind = st.text(3) == "folder" ? Kind::Folder : Kind::File;
    e.action = parse_plan_action(st.text(4)).value_or(PlanAction::Skip);
    e.reason = st.text(5);
    e.size = opt_col(st, 6);
    e.plan_etag = st.text(7);
    e.plan_lsize = opt_col(st, 8);
    e.plan_lmtime = opt_col(st, 9);
    e.conflict_name = st.text(10);
    e.conflict_name_server = st.text(11);
    e.records_below = static_cast<int>(st.int64(12));
    e.allowed = split_choices(st.text(13));
    e.choice = parse_choice(st.text(14)).value_or(Choice::None);
    e.removed = st.int64(15) != 0;
    e.unapprovable = st.text(16);
    e.state = st.text(17);
    e.result_key = st.text(18);
    e.plan_checksum = st.text(19);
    return e;
}

const char* kEntryColumns =
    "seq, rel, local_rel, kind, action, reason, size, plan_etag, plan_lsize, plan_lmtime, conflict_name, conflict_name_server, records_below,"
    " allowed, choice, removed, unapprovable, state, result_key, plan_checksum";

bool known_plan_state(const std::string& s) {
    for (const char* k : {"ready", "approved", "running", "done", "interrupted", "cancelled", "superseded"})
        if (s == k) return true;
    return false;
}

}  // namespace

namespace store_util {

// FR-050, called by Store::save_account and clear_account inside their own transaction (the mutex is held by the caller).
bool sync_mark_account_rows(sqlite3* db, const std::string& server, const std::string& user_id) {
    {
        Stmt st(db, "UPDATE sync_pairs SET problem_key = 'account_changed' WHERE (server != ?1 OR user_id != ?2) AND problem_key != 'account_changed'");
        if (!st.ok()) return false;
        st.bind_text(1, server);
        st.bind_text(2, user_id);
        if (st.step() != SQLITE_DONE) return false;
    }
    Stmt st(db, "UPDATE sync_pairs SET problem_key = '' WHERE server = ?1 AND user_id = ?2 AND problem_key = 'account_changed'");
    if (!st.ok()) return false;
    st.bind_text(1, server);
    st.bind_text(2, user_id);
    return st.step() == SQLITE_DONE;
}

}  // namespace store_util

// ----------------------------------------------------------------------------------------------------------------------------- pairs

void Store::set_pair_validator(PairValidator validator) {
    std::lock_guard<std::mutex> lock(mu_);
    pair_validator_ = std::move(validator);
}

std::vector<SyncPair> Store::pairs_locked() const {
    std::vector<SyncPair> out;
    Stmt st(db_, (std::string("SELECT ") + kPairColumns + " FROM sync_pairs ORDER BY id").c_str());
    if (!st.ok()) return out;
    while (st.step() == SQLITE_ROW) out.push_back(read_pair(st));
    return out;
}

Result<int64_t> Store::sync_pair_add(const SyncPair& given) {
    Result<int64_t> r;
    SyncPair pair = given;
    pair.id = 0;
    if (const Error e = validate_pair(pair); !e.ok()) {
        r.error = e;
        return r;
    }
    const int64_t now = clock_->now();
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) {
        r.error = storage_error(db_, "write_failed");
        return r;
    }
    if (scalar(db_, "SELECT count(*) FROM sync_pairs") >= kMaxSyncPairs) {
        r.error = refuse("pair_limit");
        return r;
    }
    if (pair_validator_) {
        if (const Error e = pair_validator_(pair, pairs_locked()); !e.ok()) {
            r.error = e;
            return r;
        }
    }
    {
        Stmt st(db_,
                "INSERT INTO sync_pairs(server, user_id, remote_folder, local_folder, direction, max_files, max_bytes, startup_sync, auto_sync,"
                " created_at) VALUES(?,?,?,?,?,?,?,?,?,?)");
        if (!st.ok()) {
            r.error = storage_error(db_, "write_failed");
            return r;
        }
        st.bind_text(1, pair.server);
        st.bind_text(2, pair.user_id);
        st.bind_text(3, pair.remote_folder);
        st.bind_text(4, pair.local_folder);
        st.bind_text(5, direction_text(pair.direction));
        st.bind_int64(6, pair.max_files);
        st.bind_int64(7, pair.max_bytes);
        st.bind_int64(8, pair.startup_sync ? 1 : 0);
        st.bind_int64(9, pair.auto_sync ? 1 : 0);
        st.bind_int64(10, now);
        if (st.step() != SQLITE_DONE) {
            r.error = storage_error(db_, "write_failed");
            return r;
        }
    }
    r.value = sqlite3_last_insert_rowid(db_);
    if (!tx.commit()) {
        r.error = storage_error(db_, "write_failed");
        r.value = 0;
    }
    return r;
}

Error Store::sync_pair_update(const SyncPair& given) {
    SyncPair pair = given;
    if (const Error e = validate_pair(pair); !e.ok()) return e;
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    if (!pair_exists(db_, pair.id)) return refuse("not_found");
    if (pair_validator_) {
        std::vector<SyncPair> others;
        for (SyncPair& o : pairs_locked())
            if (o.id != pair.id) others.push_back(std::move(o));
        if (const Error e = pair_validator_(pair, others); !e.ok()) return e;
    }
    {
        Stmt st(db_,
                "UPDATE sync_pairs SET remote_folder = ?, local_folder = ?, direction = ?, max_files = ?, max_bytes = ?, startup_sync = ?,"
                " auto_sync = ? WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_text(1, pair.remote_folder);
        st.bind_text(2, pair.local_folder);
        st.bind_text(3, direction_text(pair.direction));
        st.bind_int64(4, pair.max_files);
        st.bind_int64(5, pair.max_bytes);
        st.bind_int64(6, pair.startup_sync ? 1 : 0);
        st.bind_int64(7, pair.auto_sync ? 1 : 0);
        st.bind_int64(8, pair.id);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::sync_pair_remove(int64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    {
        Stmt st(db_, "SELECT state FROM sync_pairs WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, id);
        if (st.step() != SQLITE_ROW) return {};   // already gone
        const std::string state = st.text(0);
        if (state == "comparing" || state == "running") return refuse("sync_busy");
    }
    for (const char* sql : {"DELETE FROM sync_plan_entries WHERE plan_id IN (SELECT id FROM sync_plans WHERE pair_id = ?)",
                            "DELETE FROM sync_plans WHERE pair_id = ?", "DELETE FROM sync_files WHERE pair_id = ?", "DELETE FROM sync_pairs WHERE id = ?"}) {
        Stmt st(db_, sql);
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, id);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

std::vector<SyncPair> Store::sync_pairs() const {
    std::lock_guard<std::mutex> lock(mu_);
    return pairs_locked();
}

std::optional<SyncPair> Store::sync_pair(int64_t id) const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, (std::string("SELECT ") + kPairColumns + " FROM sync_pairs WHERE id = ?").c_str());
    if (!st.ok()) return std::nullopt;
    st.bind_int64(1, id);
    if (st.step() != SQLITE_ROW) return std::nullopt;
    return read_pair(st);
}

Error Store::sync_pair_lock(int64_t id, const std::string& new_state) {
    if (new_state != "comparing" && new_state != "running") return make_error(ErrorClass::Protocol, "protocol_error", "a pair is locked as comparing or running");
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    {
        Stmt st(db_, "SELECT state FROM sync_pairs WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, id);
        if (st.step() != SQLITE_ROW) return refuse("not_found");
        const std::string state = st.text(0);
        if (state != "idle" && state != "plan_ready" && state != "interrupted") return refuse("sync_busy");
    }
    {
        Stmt st(db_, "UPDATE sync_pairs SET state = ? WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_text(1, new_state);
        st.bind_int64(2, id);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::sync_pair_unlock(int64_t id, const std::string& state, const std::string& problem_key) {
    if (state != "idle" && state != "plan_ready" && state != "interrupted") return make_error(ErrorClass::Protocol, "protocol_error", "a pair is unlocked as idle, plan_ready or interrupted");
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    {
        Stmt st(db_, "SELECT state FROM sync_pairs WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, id);
        if (st.step() != SQLITE_ROW) return refuse("not_found");
        const std::string now = st.text(0);
        if (now != "comparing" && now != "running") return make_error(ErrorClass::Protocol, "protocol_error", "the pair is not locked");
    }
    {
        Stmt st(db_, "UPDATE sync_pairs SET state = ?, problem_key = ? WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_text(1, state);
        st.bind_text(2, cap(problem_key, 64));
        st.bind_int64(3, id);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::sync_set_result(int64_t id, const std::string& outcome, const std::string& result_json, int64_t at) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "UPDATE sync_pairs SET last_outcome = ?, last_result_json = ?, last_sync_at = ? WHERE id = ?");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_text(1, cap(outcome, 32));
    st.bind_text(2, cap(result_json, 4096));
    st.bind_int64(3, at);
    st.bind_int64(4, id);
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return sqlite3_changes(db_) == 1 ? Error() : refuse("not_found");
}

Error Store::sync_pair_compared(int64_t id, int64_t at, std::optional<int64_t> held_files, std::optional<int64_t> held_bytes) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "UPDATE sync_pairs SET last_compared_at = ?, held_files = ?, held_bytes = ? WHERE id = ?");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_int64(1, at);
    bind_opt(st, 2, held_files);
    bind_opt(st, 3, held_bytes);
    st.bind_int64(4, id);
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return sqlite3_changes(db_) == 1 ? Error() : refuse("not_found");
}

Error Store::sync_pairs_mark_account(const std::string& server, const std::string& user_id) {
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    if (!store_util::sync_mark_account_rows(db_, server, user_id)) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

// -------------------------------------------------------------------------------------------------------------------------- records

namespace {

const char* kRecordColumns =
    "pair_id, rel, local_rel, kind, origin, server_etag, server_size, server_checksum, local_size, local_mtime, local_sha256, diverged,"
    " one_sided, quiet, synced_at, copy_of";

SyncRecord read_record(const Stmt& st) {
    SyncRecord r;
    r.pair_id = st.int64(0);
    r.rel = st.text(1);
    r.local_rel = st.text(2);
    r.kind = st.text(3) == "folder" ? Kind::Folder : Kind::File;
    r.origin = st.text(4);
    r.server_etag = st.text(5);
    r.server_size = opt_col(st, 6);
    r.server_checksum = st.text(7);
    r.local_size = opt_col(st, 8);
    r.local_mtime = opt_col(st, 9);
    r.local_sha256 = st.text(10);
    r.diverged = st.int64(11) != 0;
    r.one_sided = st.text(12);
    r.quiet = st.int64(13) != 0;
    r.synced_at = st.int64(14);
    r.copy_of = st.text(15);
    return r;
}

// A relative path as the records hold it: not empty, not absolute, no empty or dot component, no control character.
bool safe_relative(const std::string& rel) {
    if (rel.empty() || rel.size() > 4096 || rel[0] == '/' || rel.back() == '/' || has_control(rel)) return false;
    size_t pos = 0;
    while (pos <= rel.size()) {
        const size_t slash = rel.find('/', pos);
        const std::string part = rel.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return true;
}

}  // namespace

std::optional<SyncRecord> Store::sync_record(int64_t pair_id, const std::string& rel) const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, (std::string("SELECT ") + kRecordColumns + " FROM sync_files WHERE pair_id = ? AND rel = ?").c_str());
    if (!st.ok()) return std::nullopt;
    st.bind_int64(1, pair_id);
    st.bind_text(2, rel);
    if (st.step() != SQLITE_ROW) return std::nullopt;
    return read_record(st);
}

std::vector<SyncRecord> Store::sync_records(int64_t pair_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<SyncRecord> out;
    Stmt st(db_, (std::string("SELECT ") + kRecordColumns + " FROM sync_files WHERE pair_id = ? ORDER BY rel").c_str());
    if (!st.ok()) return out;
    st.bind_int64(1, pair_id);
    while (st.step() == SQLITE_ROW) out.push_back(read_record(st));
    return out;
}

int64_t Store::sync_record_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return scalar(db_, "SELECT count(*) FROM sync_files");
}

namespace {

// The write of one record inside a transaction the caller holds (the mutex is held too).
Error put_record_in_tx(sqlite3* db, const SyncRecord& r) {
    if (!safe_relative(r.rel) || !safe_relative(r.local_rel)) return refuse("path_unsafe");
    if (r.origin != "download" && r.origin != "upload" && r.origin != "identical" && r.origin != "conflict_copy")
        return make_error(ErrorClass::Protocol, "protocol_error", "unknown record origin");
    if (r.one_sided != "" && r.one_sided != "reader" && r.one_sided != "server") return make_error(ErrorClass::Protocol, "protocol_error", "unknown one-sided marker");
    if (!pair_exists(db, r.pair_id)) return refuse("not_found");
    {
        Stmt st(db, "SELECT 1 FROM sync_files WHERE pair_id = ? AND rel = ?");
        if (!st.ok()) return storage_error(db, "write_failed");
        st.bind_int64(1, r.pair_id);
        st.bind_text(2, r.rel);
        const bool exists = st.step() == SQLITE_ROW;
        if (!exists && scalar(db, "SELECT count(*) FROM sync_files") >= kSyncMaxRecords) return refuse("records_full");
    }
    Stmt st(db,
            "INSERT INTO sync_files(pair_id, rel, local_rel, kind, origin, server_etag, server_size, server_checksum, local_size, local_mtime,"
            " local_sha256, diverged, one_sided, quiet, synced_at, copy_of) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
            " ON CONFLICT(pair_id, rel) DO UPDATE SET local_rel = excluded.local_rel, kind = excluded.kind, origin = excluded.origin,"
            " server_etag = excluded.server_etag, server_size = excluded.server_size, server_checksum = excluded.server_checksum,"
            " local_size = excluded.local_size, local_mtime = excluded.local_mtime, local_sha256 = excluded.local_sha256,"
            " diverged = excluded.diverged, one_sided = excluded.one_sided, quiet = excluded.quiet, synced_at = excluded.synced_at, copy_of = excluded.copy_of");
    if (!st.ok()) return storage_error(db, "write_failed");
    st.bind_int64(1, r.pair_id);
    st.bind_text(2, r.rel);
    st.bind_text(3, r.local_rel);
    st.bind_text(4, r.kind == Kind::Folder ? "folder" : "file");
    st.bind_text(5, r.origin);
    st.bind_text(6, cap(r.server_etag, 256));
    bind_opt(st, 7, r.server_size);
    st.bind_text(8, cap(r.server_checksum, 256));
    bind_opt(st, 9, r.local_size);
    bind_opt(st, 10, r.local_mtime);
    st.bind_text(11, cap(r.local_sha256, 128));
    st.bind_int64(12, r.diverged ? 1 : 0);
    st.bind_text(13, r.one_sided);
    st.bind_int64(14, r.quiet ? 1 : 0);
    st.bind_int64(15, r.synced_at);
    st.bind_text(16, cap(r.copy_of, 4096));
    if (st.step() != SQLITE_DONE) {
        if (sqlite3_errcode(db) == SQLITE_CONSTRAINT) return refuse("name_collision");
        return storage_error(db, "write_failed");
    }
    return {};
}

}  // namespace

namespace store_util {

Error sync_record_write(sqlite3* db, const SyncRecord& record) { return put_record_in_tx(db, record); }

bool sync_record_erase(sqlite3* db, int64_t pair_id, const std::string& rel) {
    Stmt st(db, "DELETE FROM sync_files WHERE pair_id = ? AND rel = ?");
    if (!st.ok()) return false;
    st.bind_int64(1, pair_id);
    st.bind_text(2, rel);
    return st.step() == SQLITE_DONE;
}

}  // namespace store_util

Error Store::sync_record_put(const SyncRecord& r) {
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    if (const Error e = put_record_in_tx(db_, r); !e.ok()) return e;
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

// One transaction: the record of a finished download and the end of its journal row (research R-3, R-7): both or neither.
Error Store::sync_commit_download(int64_t pair_id, const SyncRecord& record, int64_t job_id) {
    SyncRecord r = record;
    r.pair_id = pair_id;
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    if (const Error e = put_record_in_tx(db_, r); !e.ok()) return e;
    if (job_id > 0) {
        Stmt st(db_, "DELETE FROM downloads WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, job_id);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

// The 001 view of a file a pair has synchronized (research R-15): the pair whose server folder holds the path, and the record of that name.
std::optional<SyncedCopy> Store::synced_copy(const std::string& remote_path) const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt acc(db_, "SELECT base_url, user_id FROM account WHERE id = 1");
    if (!acc.ok() || acc.step() != SQLITE_ROW) return std::nullopt;
    const std::string server = acc.text(0), user = acc.text(1);
    const std::string path = remote_path;
    std::optional<SyncedCopy> best;
    size_t best_len = 0;
    for (const SyncPair& p : pairs_locked()) {
        if (p.server != server || p.user_id != user || p.problem_key == "account_changed") continue;
        const std::string prefix = p.remote_folder == "/" ? std::string("/") : p.remote_folder + "/";
        if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0) continue;
        if (best && prefix.size() <= best_len) continue;
        Stmt st(db_, "SELECT local_rel, server_etag, local_size, local_mtime FROM sync_files WHERE pair_id = ? AND rel = ? AND kind = 'file' AND one_sided != 'reader'");
        if (!st.ok()) continue;
        st.bind_int64(1, p.id);
        st.bind_text(2, path.substr(prefix.size()));
        if (st.step() != SQLITE_ROW) continue;
        SyncedCopy c;
        c.pair_id = p.id;
        c.root = p.local_folder;
        c.local_rel = st.text(0);
        c.etag = st.text(1);
        c.size = st.is_null(2) ? 0 : st.int64(2);
        c.mtime = st.is_null(3) ? 0 : st.int64(3);
        best = c;
        best_len = prefix.size();
    }
    return best;
}

Error Store::sync_record_drop(int64_t pair_id, const std::string& rel) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "DELETE FROM sync_files WHERE pair_id = ? AND rel = ?");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_int64(1, pair_id);
    st.bind_text(2, rel);
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return {};
}

std::set<std::string> Store::queue_sync_open_rels(int64_t pair_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::set<std::string> out;
    Stmt st(db_, "SELECT DISTINCT sync_rel FROM queue_items WHERE pair_id = ? AND state != 'done' AND sync_rel != ''");
    if (!st.ok()) return out;
    st.bind_int64(1, pair_id);
    while (st.step() == SQLITE_ROW) out.insert(st.text(0));
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------------- plans

Result<int64_t> Store::sync_plan_store(const SyncPlan& plan) {
    Result<int64_t> r;
    const bool too_many = plan.truncated || plan.entries.size() > static_cast<size_t>(kSyncMaxPlanEntries);   // a plan that is too large keeps its totals, no entries
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) {
        r.error = storage_error(db_, "write_failed");
        return r;
    }
    if (!pair_exists(db_, plan.pair_id)) {
        r.error = refuse("not_found");
        return r;
    }
    {
        Stmt st(db_, "UPDATE sync_plans SET state = 'superseded' WHERE pair_id = ? AND state IN ('ready','interrupted')");
        if (!st.ok()) {
            r.error = storage_error(db_, "write_failed");
            return r;
        }
        st.bind_int64(1, plan.pair_id);
        if (st.step() != SQLITE_DONE) {
            r.error = storage_error(db_, "write_failed");
            return r;
        }
    }
    {
        Stmt st(db_,
                "INSERT INTO sync_plans(pair_id, trigger, state, created_at, incomplete, incomplete_note, over_limit, free_bytes, reserve_bytes, quota_free,"
                " n_download, n_upload, n_skip, n_keep_both, n_prop_server, n_prop_reader, n_failed, n_in_sync, b_download, b_upload, b_keep_both, folders_new,"
                " truncated) VALUES(?, ?, 'ready', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        if (!st.ok()) {
            r.error = storage_error(db_, "write_failed");
            return r;
        }
        const SyncPlan::Totals& t = plan.totals;
        st.bind_int64(1, plan.pair_id);
        st.bind_text(2, plan.trigger);
        st.bind_int64(3, plan.created_at);
        st.bind_int64(4, plan.incomplete ? 1 : 0);
        st.bind_text(5, plan.incomplete_folders > 0 ? std::to_string(plan.incomplete_folders) : std::string());
        st.bind_text(6, plan.over_limit);
        bind_opt(st, 7, plan.free_bytes);
        st.bind_int64(8, plan.reserve_bytes);
        bind_opt(st, 9, plan.quota_free);
        st.bind_int64(10, t.n_download);
        st.bind_int64(11, t.n_upload);
        st.bind_int64(12, t.n_skip);
        st.bind_int64(13, t.n_keep_both);
        st.bind_int64(14, t.n_prop_server);
        st.bind_int64(15, t.n_prop_reader);
        st.bind_int64(16, t.n_failed);
        st.bind_int64(17, t.n_in_sync);
        st.bind_int64(18, t.b_download);
        st.bind_int64(19, t.b_upload);
        st.bind_int64(20, t.b_keep_both);
        st.bind_int64(21, t.folders_new);
        st.bind_int64(22, (plan.truncated || too_many) ? 1 : 0);
        if (st.step() != SQLITE_DONE) {
            r.error = storage_error(db_, "write_failed");
            return r;
        }
    }
    const int64_t id = sqlite3_last_insert_rowid(db_);
    if (!too_many) {
        Stmt st(db_,
                "INSERT INTO sync_plan_entries(plan_id, seq, rel, local_rel, kind, action, reason, size, plan_etag, plan_lsize, plan_lmtime, conflict_name,"
                " conflict_name_server, records_below, allowed, choice, removed, unapprovable, state, result_key, plan_checksum)"
                " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        if (!st.ok()) {
            r.error = storage_error(db_, "write_failed");
            return r;
        }
        int next = 1;
        for (const PlanEntry& e : plan.entries) {
            sqlite3_reset(st.get());
            sqlite3_clear_bindings(st.get());
            st.bind_int64(1, id);
            st.bind_int64(2, e.seq > 0 ? e.seq : next);
            st.bind_text(3, e.rel);
            st.bind_text(4, e.local_rel);
            st.bind_text(5, e.kind == Kind::Folder ? "folder" : "file");
            st.bind_text(6, plan_action_text(e.action));
            st.bind_text(7, cap(e.reason, 64));
            bind_opt(st, 8, e.size);
            st.bind_text(9, cap(e.plan_etag, 256));
            bind_opt(st, 10, e.plan_lsize);
            bind_opt(st, 11, e.plan_lmtime);
            st.bind_text(12, cap(e.conflict_name, 512));
            st.bind_text(13, cap(e.conflict_name_server, 512));
            st.bind_int64(14, e.records_below);
            st.bind_text(15, join_choices(e.allowed));
            st.bind_text(16, choice_text(e.choice));
            st.bind_int64(17, e.removed ? 1 : 0);
            st.bind_text(18, cap(e.unapprovable, 64));
            st.bind_text(19, e.state.empty() ? "planned" : e.state);
            st.bind_text(20, cap(e.result_key, 64));
            st.bind_text(21, cap(e.plan_checksum, 256));
            if (st.step() != SQLITE_DONE) {
                r.error = storage_error(db_, "write_failed");
                return r;
            }
            ++next;
        }
    }
    if (!prune_final_plans(db_, plan.pair_id)) {
        r.error = storage_error(db_, "write_failed");
        return r;
    }
    if (!tx.commit()) {
        r.error = storage_error(db_, "write_failed");
        return r;
    }
    r.value = id;
    return r;
}

std::optional<int64_t> Store::sync_ready_plan(int64_t pair_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "SELECT id FROM sync_plans WHERE pair_id = ? AND state = 'ready' ORDER BY id DESC LIMIT 1");
    if (!st.ok()) return std::nullopt;
    st.bind_int64(1, pair_id);
    if (st.step() != SQLITE_ROW) return std::nullopt;
    return st.int64(0);
}

std::optional<int64_t> Store::sync_interrupted_plan(int64_t pair_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "SELECT id FROM sync_plans WHERE pair_id = ? ORDER BY id DESC LIMIT 1");
    if (!st.ok()) return std::nullopt;
    st.bind_int64(1, pair_id);
    if (st.step() != SQLITE_ROW) return std::nullopt;
    const int64_t newest = st.int64(0);
    Stmt state(db_, "SELECT state FROM sync_plans WHERE id = ?");
    if (!state.ok()) return std::nullopt;
    state.bind_int64(1, newest);
    if (state.step() != SQLITE_ROW || state.text(0) != "interrupted") return std::nullopt;
    return newest;
}

std::optional<SyncPlan> Store::sync_plan(int64_t id) const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "SELECT id, pair_id, trigger, state, created_at, approved_at, finished_at, incomplete, incomplete_note, over_limit, free_bytes, reserve_bytes,"
            " quota_free, n_download, n_upload, n_skip, n_keep_both, n_prop_server, n_prop_reader, n_failed, b_download, b_upload, b_keep_both,"
            " folders_new, truncated, n_in_sync FROM sync_plans WHERE id = ?");
    if (!st.ok()) return std::nullopt;
    st.bind_int64(1, id);
    if (st.step() != SQLITE_ROW) return std::nullopt;
    SyncPlan p;
    p.id = st.int64(0);
    p.pair_id = st.int64(1);
    p.trigger = st.text(2);
    p.state = st.text(3);
    p.created_at = st.int64(4);
    p.approved_at = opt_col(st, 5);
    p.finished_at = opt_col(st, 6);
    p.incomplete = st.int64(7) != 0;
    p.incomplete_folders = std::atoi(st.text(8).c_str());
    p.over_limit = st.text(9);
    p.free_bytes = opt_col(st, 10);
    p.reserve_bytes = st.is_null(11) ? 0 : st.int64(11);
    p.quota_free = opt_col(st, 12);
    p.totals.n_download = static_cast<int>(st.int64(13));
    p.totals.n_upload = static_cast<int>(st.int64(14));
    p.totals.n_skip = static_cast<int>(st.int64(15));
    p.totals.n_keep_both = static_cast<int>(st.int64(16));
    p.totals.n_prop_server = static_cast<int>(st.int64(17));
    p.totals.n_prop_reader = static_cast<int>(st.int64(18));
    p.totals.n_failed = static_cast<int>(st.int64(19));
    p.totals.b_download = st.int64(20);
    p.totals.b_upload = st.int64(21);
    p.totals.b_keep_both = st.int64(22);
    p.totals.folders_new = static_cast<int>(st.int64(23));
    p.truncated = st.int64(24) != 0;
    p.totals.n_in_sync = static_cast<int>(st.int64(25));
    Stmt un(db_, "SELECT count(*) FROM sync_plan_entries WHERE plan_id = ? AND unapprovable != '' AND removed = 0");
    if (un.ok()) {
        un.bind_int64(1, id);
        if (un.step() == SQLITE_ROW) p.unapprovable = static_cast<int>(un.int64(0));
    }
    return p;
}

PlanPage Store::sync_plan_page(int64_t plan_id, const std::string& section, int offset, int limit) const {
    PlanPage page;
    std::string where;
    if (section == "transfers") where = " AND action IN ('download','upload','keep_both')";
    else if (section == "skips") where = " AND action = 'skip' AND reason != 'gone_both'";   // gone_both: a record dropped on approval, not shown
    else if (section == "proposals") where = " AND action IN ('propose_delete_server','propose_delete_reader')";
    else if (section != "all") return page;
    if (offset < 0) offset = 0;
    if (limit < 0) limit = 0;
    std::lock_guard<std::mutex> lock(mu_);
    {
        Stmt st(db_, ("SELECT count(*) FROM sync_plan_entries WHERE plan_id = ?" + where).c_str());
        if (!st.ok()) return page;
        st.bind_int64(1, plan_id);
        if (st.step() == SQLITE_ROW) page.total = static_cast<int>(st.int64(0));
    }
    Stmt st(db_, (std::string("SELECT ") + kEntryColumns + " FROM sync_plan_entries WHERE plan_id = ?" + where + " ORDER BY seq LIMIT ? OFFSET ?").c_str());
    if (!st.ok()) return page;
    st.bind_int64(1, plan_id);
    st.bind_int64(2, limit);
    st.bind_int64(3, offset);
    while (st.step() == SQLITE_ROW) page.rows.push_back(read_entry(st));
    return page;
}

namespace {

// The counts and sizes of a plan from its entries: a removed entry no longer counts as a transfer and counts as skipped (it is reported as
// skipped by the user and listed again by the next comparison).
bool recount_plan(sqlite3* db, int64_t plan_id) {
    Stmt st(db,
            "UPDATE sync_plans SET"
            " n_download = (SELECT count(*) FROM sync_plan_entries WHERE plan_id = ?1 AND action = 'download' AND removed = 0),"
            " b_download = (SELECT coalesce(sum(size), 0) FROM sync_plan_entries WHERE plan_id = ?1 AND action = 'download' AND removed = 0),"
            " n_upload = (SELECT count(*) FROM sync_plan_entries WHERE plan_id = ?1 AND action = 'upload' AND removed = 0),"
            " b_upload = (SELECT coalesce(sum(size), 0) FROM sync_plan_entries WHERE plan_id = ?1 AND action = 'upload' AND removed = 0),"
            " n_keep_both = (SELECT count(*) FROM sync_plan_entries WHERE plan_id = ?1 AND action = 'keep_both' AND removed = 0),"
            " b_keep_both = (SELECT coalesce(sum(size), 0) FROM sync_plan_entries WHERE plan_id = ?1 AND action = 'keep_both' AND removed = 0),"
            " n_skip = (SELECT count(*) FROM sync_plan_entries WHERE plan_id = ?1 AND ((action = 'skip' AND reason != 'gone_both') OR removed = 1)),"
            " n_failed = (SELECT count(*) FROM sync_plan_entries WHERE plan_id = ?1 AND state = 'failed')"
            " WHERE id = ?1");
    if (!st.ok()) return false;
    st.bind_int64(1, plan_id);
    return st.step() == SQLITE_DONE;
}

}  // namespace

Error Store::sync_plan_remove_entry(int64_t plan_id, int seq, bool removed) {
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    {
        Stmt st(db_, "SELECT state FROM sync_plans WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, plan_id);
        if (st.step() != SQLITE_ROW) return refuse("not_found");
        if (st.text(0) != "ready") return refuse("plan_not_approvable");
    }
    {
        Stmt st(db_, "SELECT action FROM sync_plan_entries WHERE plan_id = ? AND seq = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, plan_id);
        st.bind_int64(2, seq);
        if (st.step() != SQLITE_ROW) return refuse("not_found");
        const std::string action = st.text(0);
        if (action != "download" && action != "upload" && action != "keep_both") return refuse("not_allowed");
    }
    {
        Stmt st(db_, "UPDATE sync_plan_entries SET removed = ? WHERE plan_id = ? AND seq = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, removed ? 1 : 0);
        st.bind_int64(2, plan_id);
        st.bind_int64(3, seq);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    if (!recount_plan(db_, plan_id)) return storage_error(db_, "write_failed");
    // what is left of the plan against the pair's limits: taking entries out is how a plan is made to fit (the system never picks them)
    {
        Stmt st(db_,
                "UPDATE sync_plans SET over_limit = COALESCE((SELECT CASE"
                " WHEN n = 0 THEN '' WHEN f > p.max_files AND b > p.max_bytes THEN 'both' WHEN f > p.max_files THEN 'files' WHEN b > p.max_bytes THEN 'bytes' ELSE '' END"
                " FROM sync_pairs p, (SELECT COUNT(*) AS n, SUM(CASE WHEN e.action = 'download' AND e.plan_lsize IS NOT NULL THEN 0 ELSE 1 END) + COALESCE(p2.held_files, 0) AS f,"
                "  COALESCE(SUM(e.size - CASE WHEN e.action = 'download' THEN COALESCE(e.plan_lsize, 0) ELSE 0 END), 0) + COALESCE(p2.held_bytes, 0) AS b"
                "  FROM sync_plan_entries e, sync_pairs p2 WHERE e.plan_id = sync_plans.id AND p2.id = sync_plans.pair_id AND e.removed = 0"
                "  AND (e.action = 'download' OR (e.action = 'keep_both' AND e.conflict_name != ''))) WHERE p.id = sync_plans.pair_id), '')"
                " WHERE id = ? AND over_limit != ''");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, plan_id);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::sync_plan_set_choice(int64_t plan_id, int seq, Choice choice) {
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    {
        Stmt st(db_, "SELECT state FROM sync_plans WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, plan_id);
        if (st.step() != SQLITE_ROW) return refuse("not_found");
        const std::string state = st.text(0);
        if (state == "cancelled" || state == "superseded") return refuse("plan_not_approvable");
    }
    {
        Stmt st(db_, "SELECT action, allowed FROM sync_plan_entries WHERE plan_id = ? AND seq = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, plan_id);
        st.bind_int64(2, seq);
        if (st.step() != SQLITE_ROW) return refuse("not_found");
        const std::string action = st.text(0);
        if (action != "propose_delete_server" && action != "propose_delete_reader") return refuse("not_allowed");
        if (choice != Choice::None && split_choices(st.text(1)).count(choice) == 0) return refuse("not_allowed");
    }
    {
        Stmt st(db_, "UPDATE sync_plan_entries SET choice = ? WHERE plan_id = ? AND seq = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_text(1, choice_text(choice));
        st.bind_int64(2, plan_id);
        st.bind_int64(3, seq);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::sync_plan_state(int64_t plan_id, const std::string& state) {
    if (!known_plan_state(state)) return make_error(ErrorClass::Protocol, "protocol_error", "unknown plan state");
    const int64_t now = clock_->now();
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    int64_t pair_id = 0;
    {
        Stmt st(db_, "SELECT state, pair_id FROM sync_plans WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, plan_id);
        if (st.step() != SQLITE_ROW) return refuse("not_found");
        if (!plan_transition_allowed(st.text(0), state)) return make_error(ErrorClass::Protocol, "protocol_error", "illegal plan transition");
        pair_id = st.int64(1);
    }
    {
        Stmt st(db_,
                "UPDATE sync_plans SET state = ?1, approved_at = CASE WHEN ?1 = 'approved' THEN ?2 ELSE approved_at END,"
                " finished_at = CASE WHEN ?1 IN ('done','interrupted','cancelled') THEN ?2 ELSE finished_at END WHERE id = ?3");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_text(1, state);
        st.bind_int64(2, now);
        st.bind_int64(3, plan_id);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    if (!prune_final_plans(db_, pair_id)) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::sync_plan_cancel(int64_t plan_id) {
    const int64_t now = clock_->now();
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    int64_t pair_id = 0;
    {
        Stmt st(db_, "SELECT state, pair_id FROM sync_plans WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, plan_id);
        if (st.step() != SQLITE_ROW) return refuse("not_found");
        if (!plan_transition_allowed(st.text(0), "cancelled")) return refuse("plan_not_approvable");
        pair_id = st.int64(1);
    }
    {
        Stmt st(db_, "UPDATE sync_plans SET state = 'cancelled', finished_at = ? WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, now);
        st.bind_int64(2, plan_id);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    {
        Stmt st(db_, "UPDATE sync_pairs SET state = 'idle' WHERE id = ? AND state = 'plan_ready'");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, pair_id);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    if (!prune_final_plans(db_, pair_id)) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::sync_entry_result(int64_t plan_id, int seq, const std::string& state, const std::string& result_key) {
    if (state != "planned" && state != "done" && state != "failed" && state != "skipped") return make_error(ErrorClass::Protocol, "protocol_error", "unknown entry state");
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    {
        Stmt st(db_, "UPDATE sync_plan_entries SET state = ?, result_key = ? WHERE plan_id = ? AND seq = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_text(1, state);
        st.bind_text(2, cap(result_key, 64));
        st.bind_int64(3, plan_id);
        st.bind_int64(4, seq);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        if (sqlite3_changes(db_) != 1) return refuse("not_found");
    }
    if (!recount_plan(db_, plan_id)) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

SyncRecoveryReport Store::sync_recover() {
    SyncRecoveryReport report;
    const int64_t now = clock_->now();
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return report;
    {
        Stmt st(db_, "UPDATE sync_pairs SET state = 'interrupted' WHERE state IN ('comparing','running')");
        if (!st.ok() || st.step() != SQLITE_DONE) return report;
        report.pairs_interrupted = sqlite3_changes(db_);
    }
    {
        Stmt st(db_, "UPDATE sync_plans SET state = 'interrupted', finished_at = ? WHERE state IN ('approved','running')");
        if (!st.ok()) return SyncRecoveryReport();
        st.bind_int64(1, now);
        if (st.step() != SQLITE_DONE) return SyncRecoveryReport();
        report.plans_interrupted = sqlite3_changes(db_);
    }
    if (!tx.commit()) return SyncRecoveryReport();
    if (report.pairs_interrupted > 0 || report.plans_interrupted > 0)
        log::event("sync.recover", {log::count("pairs", report.pairs_interrupted), log::count("plans", report.plans_interrupted)});
    return report;
}

}  // namespace pbc
