// The write side's part of the store (specs/002-manage-server-files/data-model.md): the server capabilities and, with the later tasks, the
// queue. Every public method is one short transaction under the store mutex.
#include <sqlite3.h>

#include <limits.h>
#include <stdlib.h>

#include <algorithm>

#include "pbc/error_keys.h"
#include "pbc/new_name.h"
#include "pbc/remote_path.h"
#include "pbc/store.h"
#include "store_sql.h"
#include "store_util.h"

namespace pbc {

using sql::Stmt;
using store_util::bind_opt;
using store_util::opt_col;
using store_util::storage_error;


namespace {

std::string join_lines(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) {
        if (!out.empty()) out += "\n";
        out += s;
    }
    return out;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size() && !s.empty()) {
        const size_t nl = s.find('\n', pos);
        const std::string item = s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!item.empty()) out.push_back(item);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

}  // namespace

std::optional<ServerCaps> Store::server_caps() const {
    std::lock_guard<std::mutex> lock(mu_);
    return server_caps_locked();
}

std::optional<ServerCaps> Store::server_caps_locked() const {
    Stmt st(db_,
            "SELECT trash, chunking, max_chunk, forbidden_chars, forbidden_names, forbidden_basenames, forbidden_extensions, checked_at"
            " FROM server_caps WHERE id = 1");
    if (!st.ok() || st.step() != SQLITE_ROW) return std::nullopt;
    ServerCaps c;
    if (!st.is_null(0)) c.trash = st.int64(0) != 0;
    if (!st.is_null(1)) c.chunking = st.int64(1) != 0;
    c.max_chunk = opt_col(st, 2);
    c.forbidden_chars = st.text(3);
    c.forbidden_names = split_lines(st.text(4));
    c.forbidden_basenames = split_lines(st.text(5));
    c.forbidden_extensions = split_lines(st.text(6));
    c.checked_at = opt_col(st, 7);
    return c;
}

Error Store::save_server_caps(const ServerCaps& caps) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "INSERT OR REPLACE INTO server_caps(id, trash, chunking, max_chunk, forbidden_chars, forbidden_names, forbidden_basenames,"
            " forbidden_extensions, checked_at) VALUES(1,?,?,?,?,?,?,?,?)");
    if (!st.ok()) return storage_error(db_, "write_failed");
    if (caps.trash) st.bind_int64(1, *caps.trash ? 1 : 0);
    else st.bind_null(1);
    if (caps.chunking) st.bind_int64(2, *caps.chunking ? 1 : 0);
    else st.bind_null(2);
    bind_opt(st, 3, caps.max_chunk);
    st.bind_text(4, caps.forbidden_chars);
    st.bind_text(5, join_lines(caps.forbidden_names));
    st.bind_text(6, join_lines(caps.forbidden_basenames));
    st.bind_text(7, join_lines(caps.forbidden_extensions));
    bind_opt(st, 8, caps.checked_at);
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return {};
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The queue
// ---------------------------------------------------------------------------------------------------------------------------------

const char* item_kind_text(ItemKind kind) {
    switch (kind) {
        case ItemKind::Upload: return "upload";
        case ItemKind::Mkdir: return "mkdir";
        case ItemKind::Rename: return "rename";
        case ItemKind::Move: return "move";
        case ItemKind::Copy: return "copy";
        case ItemKind::Delete: return "delete";
    }
    return "upload";
}

std::optional<ItemKind> parse_item_kind(std::string_view t) {
    for (ItemKind k : {ItemKind::Upload, ItemKind::Mkdir, ItemKind::Rename, ItemKind::Move, ItemKind::Copy, ItemKind::Delete})
        if (t == item_kind_text(k)) return k;
    return std::nullopt;
}

const char* item_state_text(ItemState state) {
    switch (state) {
        case ItemState::Queued: return "queued";
        case ItemState::Sending: return "sending";
        case ItemState::Done: return "done";
        case ItemState::Failed: return "failed";
        case ItemState::Conflict: return "conflict";
        case ItemState::Unknown: return "unknown";
    }
    return "queued";
}

std::optional<ItemState> parse_item_state(std::string_view t) {
    for (ItemState s : {ItemState::Queued, ItemState::Sending, ItemState::Done, ItemState::Failed, ItemState::Conflict, ItemState::Unknown})
        if (t == item_state_text(s)) return s;
    return std::nullopt;
}

bool is_held(ItemState state) { return state == ItemState::Failed || state == ItemState::Conflict || state == ItemState::Unknown; }

bool transition_allowed(ItemState from, ItemState to) {
    if (from == to) return false;
    switch (from) {
        case ItemState::Queued: return to == ItemState::Sending || to == ItemState::Failed;   // failed: refused before any request
        case ItemState::Sending: return true;                                                 // done, failed, conflict, unknown, or back to queued
        case ItemState::Failed:
        case ItemState::Conflict: return to == ItemState::Queued;
        case ItemState::Unknown: return to == ItemState::Queued || to == ItemState::Done;
        case ItemState::Done: return false;
    }
    return false;
}

namespace {

const char* kColumns =
    "id, group_id, kind, entry_kind, state, step, server, user_id, src_local, src_size, src_mtime, src_remote, src_etag, src_fileid, dst_parent,"
    " dst_name, \"replace\", dst_etag, depends_on, upload_id, chunk_size, checksum, result_etag, result_fileid, reason, created_at, updated_at";

QueueItem item_from(const Stmt& st) {
    QueueItem q;
    q.id = st.int64(0);
    q.group_id = opt_col(st, 1);
    q.kind = parse_item_kind(st.text(2)).value_or(ItemKind::Upload);
    q.entry_kind = st.text(3) == "folder" ? Kind::Folder : Kind::File;
    q.state = parse_item_state(st.text(4)).value_or(ItemState::Queued);
    q.step = st.text(5);
    q.server = st.text(6);
    q.user_id = st.text(7);
    q.src_local = st.text(8);
    q.src_size = opt_col(st, 9);
    q.src_mtime = opt_col(st, 10);
    q.src_remote = st.text(11);
    q.src_etag = st.text(12);
    q.src_fileid = opt_col(st, 13);
    q.dst_parent = st.text(14);
    q.dst_name = st.text(15);
    q.replace = st.int64(16) != 0;
    q.dst_etag = st.text(17);
    q.depends_on = opt_col(st, 18);
    q.upload_id = st.text(19);
    q.chunk_size = opt_col(st, 20);
    q.checksum = st.text(21);
    q.result_etag = st.text(22);
    q.result_fileid = opt_col(st, 23);
    q.reason = st.text(24);
    q.created_at = st.int64(25);
    q.updated_at = st.int64(26);
    return q;
}

Error refused(const char* key, const char* detail) { return make_error(class_of(key), key, detail); }

bool strong_etag(const std::string& etag) { return !etag.empty() && etag.compare(0, 2, "W/") != 0 && etag.size() <= 256; }

bool valid_folder_path(const std::string& p) { return is_valid_remote_path(p); }

bool valid_entry_path(const std::string& p) { return valid_folder_path(p) && p != "/"; }

std::string base_of(const std::string& path) { return path.substr(path.rfind('/') + 1); }

bool inside_or_equal(const std::string& path, const std::string& folder) {
    return path == folder || (path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 && path[folder.size()] == '/');
}

void bind_item(Stmt& st, const QueueItem& q, int64_t now, const std::optional<int64_t>& group) {
    int i = 1;
    bind_opt(st, i++, group);
    st.bind_text(i++, item_kind_text(q.kind));
    st.bind_text(i++, q.entry_kind == Kind::Folder ? "folder" : "file");
    st.bind_text(i++, item_state_text(q.state));
    st.bind_text(i++, q.step);
    st.bind_text(i++, q.server);
    st.bind_text(i++, q.user_id);
    st.bind_text(i++, q.src_local);
    bind_opt(st, i++, q.src_size);
    bind_opt(st, i++, q.src_mtime);
    st.bind_text(i++, q.src_remote);
    st.bind_text(i++, q.src_etag);
    bind_opt(st, i++, q.src_fileid);
    st.bind_text(i++, q.dst_parent);
    st.bind_text(i++, q.dst_name);
    st.bind_int64(i++, q.replace ? 1 : 0);
    st.bind_text(i++, q.dst_etag);
    bind_opt(st, i++, q.depends_on);
    st.bind_text(i++, q.upload_id);
    bind_opt(st, i++, q.chunk_size);
    st.bind_text(i++, q.checksum);
    st.bind_text(i++, q.result_etag);
    bind_opt(st, i++, q.result_fileid);
    st.bind_text(i++, q.reason);
    st.bind_int64(i++, now);
    st.bind_int64(i++, now);
}

int row_count(sqlite3* db) {
    Stmt st(db, "SELECT count(*) FROM queue_items");
    return st.ok() && st.step() == SQLITE_ROW ? static_cast<int>(st.int64(0)) : 0;
}

// "p" and everything below it as a range on a text column: [p + "/", p + "0")
void bind_below(Stmt& st, int first, const std::string& folder) {
    st.bind_text(first, folder);
    st.bind_text(first + 1, folder == "/" ? "/" : folder + "/");
    st.bind_text(first + 2, folder == "/" ? "0" : folder + "0");
}

}  // namespace

Error Store::validate_queue_item(const QueueItem& q, const ServerCaps& caps) const {
    if (q.server.empty() || q.user_id.empty()) return refused("protocol_error", "the item names no account");
    if (q.state != ItemState::Queued) return refused("protocol_error", "a new item starts queued");
    if (q.replace) {
        if (q.kind != ItemKind::Upload && q.kind != ItemKind::Move && q.kind != ItemKind::Copy && q.kind != ItemKind::Rename) return refused("protocol_error", "only a file can be replaced");
        if (q.entry_kind != Kind::File) return refused("not_allowed", "a folder is never replaced");
        if (!strong_etag(q.dst_etag)) return refused("refresh_first", "replacing needs the version of the file that is replaced");
    }
    if (q.kind != ItemKind::Delete) {
        if (!valid_folder_path(q.dst_parent)) return refused("protocol_error", "the target folder is not a valid path");
        if (!check_new_name(q.dst_name, caps).ok) return refused("name_forbidden", "the name is not allowed");
    }
    switch (q.kind) {
        case ItemKind::Upload:
            if (q.entry_kind != Kind::File) return refused("protocol_error", "an upload is a file");
            if (q.src_local.empty() || q.src_local[0] != '/') return refused("protocol_error", "an upload needs the absolute path of its source");
            break;
        case ItemKind::Mkdir:
            if (q.entry_kind != Kind::Folder) return refused("protocol_error", "a new folder is a folder");
            break;
        case ItemKind::Rename:
        case ItemKind::Move:
        case ItemKind::Copy:
        case ItemKind::Delete:
            if (!valid_entry_path(q.src_remote)) return refused("protocol_error", "the entry is not a valid path");
            if (!strong_etag(q.src_etag)) return refused("refresh_first", "the folder must be refreshed first: the version of the entry is not known");
            if (q.kind == ItemKind::Delete) break;
            if (q.kind == ItemKind::Rename && q.dst_parent != parent_of(q.src_remote)) return refused("protocol_error", "a rename stays in its folder");
            if (q.kind != ItemKind::Rename && q.entry_kind == Kind::Folder && inside_or_equal(q.dst_parent, q.src_remote))
                return refused("not_allowed", "a folder cannot go into itself");
            if (q.kind != ItemKind::Copy && q.dst_parent == parent_of(q.src_remote) && q.dst_name == base_of(q.src_remote)) return refused("name_exists", "the entry is already there");
            if (q.kind == ItemKind::Copy && q.dst_parent == parent_of(q.src_remote) && q.dst_name == base_of(q.src_remote)) return refused("name_exists", "the copy would have the name of the original");
            break;
    }
    return {};
}

Result<int64_t> Store::queue_add(const QueueItem& item) {
    Result<int64_t> out;
    std::lock_guard<std::mutex> lock(mu_);
    const ServerCaps caps = server_caps_locked().value_or(ServerCaps());
    out.error = validate_queue_item(item, caps);
    if (!out.ok()) return out;
    if (row_count(db_) >= kMaxQueueItems) {
        out.error = refused("queue_full", "the queue is full");
        return out;
    }
    if (item.depends_on) {
        Stmt st(db_, "SELECT 1 FROM queue_items WHERE id = ?");
        if (!st.ok()) {
            out.error = storage_error(db_, "write_failed");
            return out;
        }
        st.bind_int64(1, *item.depends_on);
        if (st.step() != SQLITE_ROW) {
            out.error = refused("protocol_error", "the item it waits for does not exist");
            return out;
        }
    }
    Stmt st(db_, (std::string("INSERT INTO queue_items(group_id, kind, entry_kind, state, step, server, user_id, src_local, src_size, src_mtime, src_remote,"
                              " src_etag, src_fileid, dst_parent, dst_name, \"replace\", dst_etag, depends_on, upload_id, chunk_size, checksum,"
                              " result_etag, result_fileid, reason, created_at, updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)")).c_str());
    if (!st.ok()) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    bind_item(st, item, clock_->now(), item.group_id);
    if (st.step() != SQLITE_DONE) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    out.value = sqlite3_last_insert_rowid(db_);
    return out;
}

Result<std::vector<int64_t>> Store::queue_add_group(const std::vector<QueueItem>& items, const std::vector<int>& depends_on_index) {
    Result<std::vector<int64_t>> out;
    if (items.empty() || depends_on_index.size() != items.size()) {
        out.error = refused("protocol_error", "a group needs its items and one dependency entry for each");
        return out;
    }
    std::lock_guard<std::mutex> lock(mu_);
    const ServerCaps caps = server_caps_locked().value_or(ServerCaps());
    for (size_t i = 0; i < items.size(); ++i) {
        out.error = validate_queue_item(items[i], caps);
        if (!out.ok()) return out;
        const int d = depends_on_index[i];
        if (d < -1 || d >= static_cast<int>(i)) {
            out.error = refused("protocol_error", "a dependency must point to an earlier item of the group");
            return out;
        }
    }
    if (row_count(db_) + static_cast<int>(items.size()) > kMaxQueueItems) {
        out.error = refused("queue_full", "the group does not fit into the queue");
        return out;
    }
    store_util::Transaction tx(db_);
    if (!tx.open) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    int64_t group = 1;
    {
        Stmt st(db_, "SELECT COALESCE(MAX(group_id), 0) + 1 FROM queue_items");
        if (st.ok() && st.step() == SQLITE_ROW) group = st.int64(0);
    }
    std::vector<int64_t> ids;
    for (size_t i = 0; i < items.size(); ++i) {
        QueueItem q = items[i];
        if (depends_on_index[i] >= 0) q.depends_on = ids[static_cast<size_t>(depends_on_index[i])];
        Stmt st(db_, "INSERT INTO queue_items(group_id, kind, entry_kind, state, step, server, user_id, src_local, src_size, src_mtime, src_remote,"
                     " src_etag, src_fileid, dst_parent, dst_name, \"replace\", dst_etag, depends_on, upload_id, chunk_size, checksum,"
                     " result_etag, result_fileid, reason, created_at, updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        if (!st.ok()) {
            out.error = storage_error(db_, "write_failed");
            return out;
        }
        bind_item(st, q, clock_->now(), group);
        if (st.step() != SQLITE_DONE) {
            out.error = storage_error(db_, "write_failed");
            return out;
        }
        ids.push_back(sqlite3_last_insert_rowid(db_));
    }
    if (!tx.commit()) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    out.value = std::move(ids);
    return out;
}

std::optional<QueueItem> Store::queue_item(int64_t id) const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, (std::string("SELECT ") + kColumns + " FROM queue_items WHERE id = ?").c_str());
    if (!st.ok()) return std::nullopt;
    st.bind_int64(1, id);
    if (st.step() != SQLITE_ROW) return std::nullopt;
    return item_from(st);
}

QueuePage Store::queue_page(int offset, int limit) const {
    QueuePage page;
    std::lock_guard<std::mutex> lock(mu_);
    page.total = row_count(db_);
    Stmt st(db_, (std::string("SELECT ") + kColumns + " FROM queue_items ORDER BY id LIMIT ? OFFSET ?").c_str());
    if (!st.ok()) return page;
    st.bind_int64(1, std::max(0, limit));
    st.bind_int64(2, std::max(0, offset));
    while (st.step() == SQLITE_ROW) page.rows.push_back(item_from(st));
    return page;
}

int Store::queue_count_open() const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "SELECT count(*) FROM queue_items WHERE state <> 'done'");
    return st.ok() && st.step() == SQLITE_ROW ? static_cast<int>(st.int64(0)) : 0;
}

namespace {
const char* kSendable =
    " FROM queue_items q WHERE q.state = 'queued' AND q.server = ? AND q.user_id = ?"
    " AND (q.depends_on IS NULL OR NOT EXISTS (SELECT 1 FROM queue_items d WHERE d.id = q.depends_on AND d.state <> 'done'))";
}

int Store::queue_count_sendable(const std::string& server, const std::string& user_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, (std::string("SELECT count(*)") + kSendable).c_str());
    if (!st.ok()) return 0;
    st.bind_text(1, server);
    st.bind_text(2, user_id);
    return st.step() == SQLITE_ROW ? static_cast<int>(st.int64(0)) : 0;
}

std::optional<QueueItem> Store::queue_next(const std::string& server, const std::string& user_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, (std::string("SELECT q.id, q.group_id, q.kind, q.entry_kind, q.state, q.step, q.server, q.user_id, q.src_local, q.src_size, q.src_mtime,"
                              " q.src_remote, q.src_etag, q.src_fileid, q.dst_parent, q.dst_name, q.\"replace\", q.dst_etag, q.depends_on, q.upload_id,"
                              " q.chunk_size, q.checksum, q.result_etag, q.result_fileid, q.reason, q.created_at, q.updated_at") + kSendable +
                  " ORDER BY q.id LIMIT 1").c_str());
    if (!st.ok()) return std::nullopt;
    st.bind_text(1, server);
    st.bind_text(2, user_id);
    if (st.step() != SQLITE_ROW) return std::nullopt;
    return item_from(st);
}

Error Store::queue_update(const QueueItem& item) {
    std::lock_guard<std::mutex> lock(mu_);
    {
        Stmt st(db_, "SELECT kind FROM queue_items WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, item.id);
        if (st.step() != SQLITE_ROW) return refused("protocol_error", "the item does not exist");
        const auto kind = parse_item_kind(st.text(0)).value_or(ItemKind::Upload);
        if (kind != ItemKind::Delete) {
            const ServerCaps caps = server_caps_locked().value_or(ServerCaps());
            if (!check_new_name(item.dst_name, caps).ok) return refused("name_forbidden", "the name is not allowed");
            if (!valid_folder_path(item.dst_parent)) return refused("protocol_error", "the target folder is not a valid path");
        }
    }
    Stmt st(db_,
            "UPDATE queue_items SET state = ?, step = ?, src_size = ?, src_mtime = ?, src_etag = ?, src_fileid = ?, dst_parent = ?, dst_name = ?,"
            " \"replace\" = ?, dst_etag = ?, depends_on = ?, upload_id = ?, chunk_size = ?, checksum = ?, result_etag = ?, result_fileid = ?, reason = ?,"
            " updated_at = ? WHERE id = ?");
    if (!st.ok()) return storage_error(db_, "write_failed");
    int i = 1;
    st.bind_text(i++, item_state_text(item.state));
    st.bind_text(i++, item.step);
    bind_opt(st, i++, item.src_size);
    bind_opt(st, i++, item.src_mtime);
    st.bind_text(i++, item.src_etag);
    bind_opt(st, i++, item.src_fileid);
    st.bind_text(i++, item.dst_parent);
    st.bind_text(i++, item.dst_name);
    st.bind_int64(i++, item.replace ? 1 : 0);
    st.bind_text(i++, item.dst_etag);
    bind_opt(st, i++, item.depends_on);
    st.bind_text(i++, item.upload_id);
    bind_opt(st, i++, item.chunk_size);
    st.bind_text(i++, item.checksum);
    st.bind_text(i++, item.result_etag);
    bind_opt(st, i++, item.result_fileid);
    st.bind_text(i++, item.reason);
    st.bind_int64(i++, clock_->now());
    st.bind_int64(i++, item.id);
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return {};
}

Error Store::queue_remove(int64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    {
        Stmt st(db_, "SELECT state FROM queue_items WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, id);
        if (st.step() != SQLITE_ROW) return {};   // already gone
        if (st.text(0) == "sending") return refused("protocol_error", "an item that is being sent cannot be removed");
    }
    {
        Stmt st(db_, "UPDATE queue_items SET state = 'failed', reason = 'target_gone', updated_at = ? WHERE depends_on = ? AND state = 'queued'");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, clock_->now());
        st.bind_int64(2, id);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    {
        Stmt st(db_, "DELETE FROM queue_items WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, id);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::queue_clear_done() {
    std::lock_guard<std::mutex> lock(mu_);
    std::string err;
    if (!sql::exec(db_, "DELETE FROM queue_items WHERE state = 'done'", &err)) return storage_error(db_, "write_failed");
    return {};
}

NameUse Store::name_use(const std::string& folder, const std::string& name) const {
    NameUse use;
    std::lock_guard<std::mutex> lock(mu_);
    {
        Stmt st(db_, "SELECT 1 FROM entries WHERE parent = ? AND name = ?");
        if (st.ok()) {
            st.bind_text(1, folder);
            st.bind_text(2, name);
            use.cached = st.step() == SQLITE_ROW;
        }
    }
    {
        Stmt st(db_,
                "SELECT 1 FROM queue_items q, account a WHERE a.id = 1 AND q.server = a.base_url AND q.user_id = a.user_id AND q.state <> 'done' AND"
                " q.dst_parent = ? AND q.dst_name = ? AND q.kind IN ('upload', 'mkdir', 'move', 'copy', 'rename') LIMIT 1");
        if (st.ok()) {
            st.bind_text(1, folder);
            st.bind_text(2, name);
            use.queued = st.step() == SQLITE_ROW;
        }
    }
    return use;
}

std::optional<int64_t> Store::queue_pending_mkdir(const std::string& folder_path) const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "SELECT q.id FROM queue_items q, account a WHERE a.id = 1 AND q.server = a.base_url AND q.user_id = a.user_id AND q.kind = 'mkdir' AND"
            " q.state <> 'done' AND (CASE WHEN q.dst_parent = '/' THEN '/' || q.dst_name ELSE q.dst_parent || '/' || q.dst_name END) = ? ORDER BY q.id DESC LIMIT 1");
    if (!st.ok()) return std::nullopt;
    st.bind_text(1, folder_path);
    if (st.step() != SQLITE_ROW) return std::nullopt;
    return st.int64(0);
}

namespace {

// One statement with text parameters (every parameter of the effects below is a path or a name).
bool run_text(sqlite3* db, const char* sql, const std::vector<std::string>& params) {
    Stmt st(db, sql);
    if (!st.ok()) return false;
    for (size_t i = 0; i < params.size(); ++i) st.bind_text(static_cast<int>(i) + 1, params[i]);
    return st.step() == SQLITE_DONE;
}

std::string join_path(const std::string& folder, const std::string& name) { return folder == "/" ? "/" + name : folder + "/" + name; }
std::string parent_path(const std::string& path) {
    const size_t cut = path.rfind('/');
    return cut == 0 ? std::string("/") : path.substr(0, cut);
}
std::string last_segment(const std::string& path) { return path.substr(path.rfind('/') + 1); }

bool refresh_count(sqlite3* db, const std::string& folder) {
    return run_text(db, "UPDATE folders SET entry_count = (SELECT COUNT(*) FROM entries WHERE parent = ?1) WHERE path = ?1", {folder});
}

bool folder_cached(sqlite3* db, const std::string& folder) {
    Stmt st(db, "SELECT 1 FROM folders WHERE path = ?");
    if (!st.ok()) return false;
    st.bind_text(1, folder);
    return st.step() == SQLITE_ROW;
}

// The cached listings of a folder and of everything below it are dropped (they are a cache; the next visit fetches).
bool drop_listings_below(sqlite3* db, const std::string& path) {
    return run_text(db, "DELETE FROM entries WHERE parent = ?1 OR (parent >= ?1 || '/' AND parent < ?1 || '0')", {path}) &&
           run_text(db, "DELETE FROM folders WHERE path = ?1 OR (path >= ?1 || '/' AND path < ?1 || '0')", {path});
}

// A change inside a folder changes the version (ETag) of that folder and of every folder above it on the server. The reader cannot know the new versions
// without a listing, so the cached ones are forgotten (set to ""): a later move, copy, rename or delete of such a folder asks for a refresh first instead
// of holding a false "changed on the server" that the reader's own change caused.
bool forget_versions_up(sqlite3* db, std::string folder) {
    while (folder != "/") {
        if (!run_text(db, "UPDATE entries SET etag = '' WHERE parent = ?1 AND name = ?2", {parent_path(folder), last_segment(folder)}) ||
            !run_text(db, "UPDATE folders SET etag = '' WHERE path = ?1", {folder}))
            return false;
        folder = parent_path(folder);
    }
    return run_text(db, "UPDATE folders SET etag = '' WHERE path = '/'", {});
}

// The records of the entry and of everything below it follow a rename or a move: the downloaded copies and the offline marks (the files on the card
// keep their names) and the favorite rows.
bool rewrite_records(sqlite3* db, const std::string& from, const std::string& to) {
    for (const char* table : {"local_files", "offline_marks", "favorites"}) {
        const std::string sql = std::string("UPDATE OR REPLACE ") + table + " SET remote_path = ?2 || substr(remote_path, length(?1) + 1)"
                                " WHERE remote_path = ?1 OR (remote_path >= ?1 || '/' AND remote_path < ?1 || '0')";
        if (!run_text(db, sql.c_str(), {from, to})) return false;
    }
    return true;
}

}  // namespace

Error Store::queue_retarget(int64_t id, const std::string& new_parent) {
    if (!valid_folder_path(new_parent)) return refused("protocol_error", "the target folder is not a valid path");
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    std::string kind, state, parent, name;
    {
        Stmt st(db_, "SELECT kind, state, dst_parent, dst_name FROM queue_items WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, id);
        if (st.step() != SQLITE_ROW) return refused("protocol_error", "the item does not exist");
        kind = st.text(0);
        state = st.text(1);
        parent = st.text(2);
        name = st.text(3);
    }
    if ((state != "failed" && state != "conflict") || (kind != "upload" && kind != "mkdir" && kind != "move" && kind != "copy"))
        return refused("protocol_error", "the item cannot be given another target");
    if (kind == "move" || kind == "copy") {
        std::string src;
        Stmt st(db_, "SELECT src_remote FROM queue_items WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, id);
        if (st.step() == SQLITE_ROW) src = st.text(0);
        if (inside_or_equal(new_parent, src)) return refused("not_allowed", "an entry cannot go into itself");
    }
    {   // the name must be free at the new place
        Stmt st(db_, "SELECT 1 FROM entries WHERE parent = ?1 AND name = ?2 UNION ALL SELECT 1 FROM queue_items WHERE state <> 'done' AND dst_parent = ?1 AND dst_name = ?2 AND id <> ?3");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_text(1, new_parent);
        st.bind_text(2, name);
        st.bind_int64(3, id);
        if (st.step() == SQLITE_ROW) return refused("name_exists", "the name is taken at the new target");
    }
    int64_t depends = 0;
    {   // a folder of the new target that is still queued: the item waits for it
        Stmt st(db_,
                "SELECT q.id FROM queue_items q, account a WHERE a.id = 1 AND q.server = a.base_url AND q.user_id = a.user_id AND q.kind = 'mkdir' AND q.state <> 'done' AND q.id <> ?2"
                " AND (CASE WHEN q.dst_parent = '/' THEN '/' || q.dst_name ELSE q.dst_parent || '/' || q.dst_name END) = ?1 ORDER BY q.id DESC LIMIT 1");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_text(1, new_parent);
        st.bind_int64(2, id);
        if (st.step() == SQLITE_ROW) depends = st.int64(0);
    }
    if (kind == "mkdir") {   // what waits for this new folder follows it
        const std::string old_path = join_path(parent, name), new_path = join_path(new_parent, name);
        if (!run_text(db_,
                      "UPDATE queue_items SET dst_parent = ?2 || substr(dst_parent, length(?1) + 1) WHERE id <> ?3 AND state <> 'done' AND (dst_parent = ?1 OR (dst_parent >= ?1 || '/' AND dst_parent < ?1 || '0'))",
                      {old_path, new_path, std::to_string(id)}))
            return storage_error(db_, "write_failed");
    }
    {
        Stmt st(db_, "UPDATE queue_items SET dst_parent = ?1, depends_on = ?2, state = 'queued', step = '', reason = '', updated_at = ?3 WHERE id = ?4");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_text(1, new_parent);
        if (depends != 0) st.bind_int64(2, depends);
        else st.bind_null(2);
        st.bind_int64(3, clock_->now());
        st.bind_int64(4, id);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    if (!tx.commit()) return storage_error(db_, "write_failed");
    return {};
}

Error Store::queue_confirm(int64_t id, const std::string& etag, std::optional<int64_t> fileid, const ConfirmContext& context) {
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    ItemKind kind = ItemKind::Upload;
    Kind entry_kind = Kind::File;
    std::string parent, name, src_local, src_remote;
    std::optional<int64_t> size, mtime;
    bool replace = false;
    {
        Stmt st(db_, "SELECT kind, entry_kind, dst_parent, dst_name, src_size, src_mtime, src_local, \"replace\", src_remote FROM queue_items WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, id);
        if (st.step() != SQLITE_ROW) return refused("protocol_error", "the item does not exist");
        kind = parse_item_kind(st.text(0)).value_or(ItemKind::Upload);
        entry_kind = st.text(1) == "folder" ? Kind::Folder : Kind::File;
        parent = st.text(2);
        name = st.text(3);
        size = opt_col(st, 4);
        mtime = opt_col(st, 5);
        src_local = st.text(6);
        replace = st.int64(7) != 0;
        src_remote = st.text(8);
    }
    {
        Stmt st(db_, "UPDATE queue_items SET state = 'done', step = '', reason = '', result_etag = ?, result_fileid = ?, updated_at = ? WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_text(1, etag);
        bind_opt(st, 2, fileid);
        st.bind_int64(3, clock_->now());
        st.bind_int64(4, id);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    if (kind == ItemKind::Upload || kind == ItemKind::Mkdir) {
        bool cached = false;
        {
            Stmt st(db_, "SELECT 1 FROM folders WHERE path = ?");
            if (!st.ok()) return storage_error(db_, "write_failed");
            st.bind_text(1, parent);
            cached = st.step() == SQLITE_ROW;
        }
        if (cached) {
            {
                Stmt st(db_,
                        "INSERT INTO entries(parent, name, kind, size, mtime, etag, fileid, mime, permissions, favorite, checksum, availability, reason)"
                        " VALUES(?,?,?,?,?,?,?,'','',0,'','ok','')"
                        " ON CONFLICT(parent, name) DO UPDATE SET kind = excluded.kind, size = excluded.size, mtime = excluded.mtime, etag = excluded.etag,"
                        " fileid = excluded.fileid, checksum = '', availability = 'ok', reason = ''");
                if (!st.ok()) return storage_error(db_, "write_failed");
                st.bind_text(1, parent);
                st.bind_text(2, name);
                st.bind_text(3, entry_kind == Kind::Folder ? "folder" : "file");
                if (entry_kind == Kind::File) {
                    bind_opt(st, 4, size);
                    bind_opt(st, 5, mtime);
                } else {
                    st.bind_null(4);
                    st.bind_null(5);
                }
                st.bind_text(6, etag.substr(0, 256));
                bind_opt(st, 7, fileid);
                if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
            }
            Stmt st(db_, "UPDATE folders SET entry_count = (SELECT COUNT(*) FROM entries WHERE parent = ?1) WHERE path = ?1");
            if (!st.ok()) return storage_error(db_, "write_failed");
            st.bind_text(1, parent);
            if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        }
    }
    if (kind == ItemKind::Rename || kind == ItemKind::Move || kind == ItemKind::Copy) {
        const std::string to = join_path(parent, name);
        const std::string from_parent = parent_path(src_remote), from_name = last_segment(src_remote);
        const bool dest_cached = folder_cached(db_, parent);
        // the stale rows at the destination go first, then the entry's own row is carried over under its new place
        if (dest_cached) {
            if (!run_text(db_, "DELETE FROM entries WHERE parent = ?1 AND name = ?2", {parent, name})) return storage_error(db_, "write_failed");
            if (!drop_listings_below(db_, to)) return storage_error(db_, "write_failed");
            Stmt st(db_,
                    "INSERT INTO entries(parent, name, kind, size, mtime, etag, fileid, mime, permissions, favorite, checksum, availability, reason)"
                    " SELECT ?1, ?2, kind, size, mtime, etag, fileid, mime, permissions, ?5, checksum, availability, reason FROM entries WHERE parent = ?3 AND name = ?4");
            if (!st.ok()) return storage_error(db_, "write_failed");
            st.bind_text(1, parent);
            st.bind_text(2, name);
            st.bind_text(3, from_parent);
            st.bind_text(4, from_name);
            st.bind_int64(5, 0);
            if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
            Stmt up(db_, "UPDATE entries SET etag = CASE WHEN ?3 = '' THEN etag ELSE ?3 END, fileid = COALESCE(?4, fileid) WHERE parent = ?1 AND name = ?2");
            if (!up.ok()) return storage_error(db_, "write_failed");
            up.bind_text(1, parent);
            up.bind_text(2, name);
            up.bind_text(3, etag);
            bind_opt(up, 4, kind == ItemKind::Copy ? fileid : std::optional<int64_t>());   // a move keeps the file id; a copy has its own
            if (up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
            if (kind == ItemKind::Copy && fileid) {
                Stmt cp(db_, "UPDATE entries SET fileid = ?3 WHERE parent = ?1 AND name = ?2");
                if (!cp.ok()) return storage_error(db_, "write_failed");
                cp.bind_text(1, parent);
                cp.bind_text(2, name);
                cp.bind_int64(3, *fileid);
                if (cp.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
            }
            if (!refresh_count(db_, parent)) return storage_error(db_, "write_failed");
        }
        if (kind != ItemKind::Copy) {
            if (!run_text(db_, "DELETE FROM entries WHERE parent = ?1 AND name = ?2", {from_parent, from_name}) || !drop_listings_below(db_, src_remote) ||
                !refresh_count(db_, from_parent) || !rewrite_records(db_, src_remote, to))
                return storage_error(db_, "write_failed");
        }
    }
    if (kind == ItemKind::Delete) {   // the cached rows go; the downloaded copy, its mark and the file on the card are not touched
        if (!run_text(db_, "DELETE FROM entries WHERE parent = ?1 AND name = ?2", {parent_path(src_remote), last_segment(src_remote)}) ||
            !drop_listings_below(db_, src_remote) || !refresh_count(db_, parent_path(src_remote)) ||
            !run_text(db_, "DELETE FROM favorites WHERE remote_path = ?1 OR (remote_path >= ?1 || '/' AND remote_path < ?1 || '0')", {src_remote}))
            return storage_error(db_, "write_failed");
    }
    if (kind == ItemKind::Delete) {
        if (!forget_versions_up(db_, parent_path(src_remote))) return storage_error(db_, "write_failed");
    } else {
        if (!forget_versions_up(db_, parent)) return storage_error(db_, "write_failed");
        if ((kind == ItemKind::Rename || kind == ItemKind::Move) && !forget_versions_up(db_, parent_path(src_remote))) return storage_error(db_, "write_failed");
    }
    if (kind == ItemKind::Upload && replace) {
        // The link (research R-10, FR-029): an upload that replaced the entry's file from the entry's own downloaded copy, with the copy unchanged since
        // the upload started, makes the copy the file the server has now. Any other source never touches the record of the copy.
        const std::string remote = parent == "/" ? "/" + name : parent + "/" + name;
        std::string local_path;
        {
            Stmt st(db_, "SELECT local_path FROM local_files WHERE remote_path = ?");
            if (st.ok()) {
                st.bind_text(1, remote);
                if (st.step() == SQLITE_ROW) local_path = st.text(0);
            }
        }
        const bool unchanged = size && mtime && context.source_size && context.source_mtime && *size == *context.source_size && *mtime == *context.source_mtime;
        if (!local_path.empty() && unchanged && !context.download_root.empty() && !src_local.empty()) {
            char resolved[PATH_MAX];
            const std::string absolute = context.download_root + "/" + local_path;
            if (::realpath(absolute.c_str(), resolved) && src_local == resolved) {
                Stmt st(db_,
                        "UPDATE local_files SET remote_etag = ?, seen_etag = '', size = ?, mtime = ?, remote_size = ?, remote_mtime = ?, server_missing = 0"
                        " WHERE remote_path = ?");
                if (!st.ok()) return storage_error(db_, "write_failed");
                st.bind_text(1, etag);
                st.bind_int64(2, *size);
                st.bind_int64(3, *mtime);
                st.bind_int64(4, *size);
                st.bind_int64(5, *mtime);
                st.bind_text(6, remote);
                if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
            }
        }
    }
    if (!tx.commit()) return storage_error(db_, "write_failed");
    return {};
}

RecoveryReport Store::queue_recover() {
    RecoveryReport rep;
    std::lock_guard<std::mutex> lock(mu_);
    store_util::Transaction tx(db_);
    if (!tx.open) return rep;
    const int64_t now = clock_->now();
    {
        Stmt st(db_, "UPDATE queue_items SET state = 'queued', step = '', updated_at = ? WHERE state = 'sending' AND step IN ('prepare', 'chunks')");
        if (!st.ok()) return rep;
        st.bind_int64(1, now);
        if (st.step() != SQLITE_DONE) return rep;
        rep.requeued = sqlite3_changes(db_);
    }
    {
        Stmt st(db_, "UPDATE queue_items SET state = 'unknown', step = '', reason = 'no_answer', updated_at = ? WHERE state = 'sending'");
        if (!st.ok()) return rep;
        st.bind_int64(1, now);
        if (st.step() != SQLITE_DONE) return rep;
        rep.unknown = sqlite3_changes(db_);
    }
    if (!tx.commit()) return RecoveryReport();
    return rep;
}

namespace {

Availability availability_of(const std::string& t) {
    if (t == "no_permission") return Availability::NoPermission;
    if (t == "encrypted") return Availability::Encrypted;
    if (t == "unsupported") return Availability::Unsupported;
    if (t == "unreadable") return Availability::Unreadable;
    return Availability::Ok;
}

// The cached rows of ?1 and the open items of the current account that touch it, as one set (see Store::entries_page_overlay). ?2 and ?3 are
// the range of the paths below ?1. The items that decorate an existing row are looked up once in a small materialized set, not per row.
const char* kOverlay =
    "WITH acct AS (SELECT base_url AS server, user_id AS uid FROM account WHERE id = 1),"
    " deco AS MATERIALIZED (SELECT path, MAX(id) AS id FROM ("
    "  SELECT q.src_remote AS path, q.id AS id FROM queue_items q, acct a WHERE q.state <> 'done' AND q.server = a.server AND q.user_id = a.uid AND"
    "   q.kind IN ('rename', 'move', 'delete') AND q.src_remote >= ?2 AND q.src_remote < ?3"
    "  UNION ALL"
    "  SELECT (CASE WHEN q.dst_parent = '/' THEN '/' || q.dst_name ELSE q.dst_parent || '/' || q.dst_name END), q.id"
    "   FROM queue_items q, acct a WHERE q.state <> 'done' AND q.server = a.server AND q.user_id = a.uid AND q.kind = 'upload' AND q.\"replace\" = 1 AND"
    "   q.dst_parent = ?1) GROUP BY path),"
    " base AS (SELECT e.name AS name, e.kind AS kind, e.size AS size, e.mtime AS mtime, e.etag AS etag, e.fileid AS fileid, e.mime AS mime,"
    "  e.permissions AS permissions, e.favorite AS favorite, e.checksum AS checksum, e.availability AS availability, e.reason AS reason, d.id AS item_id"
    "  FROM entries e LEFT JOIN deco d ON d.path = (CASE WHEN ?1 = '/' THEN '/' || e.name ELSE ?1 || '/' || e.name END) WHERE e.parent = ?1),"
    " eff AS (SELECT CASE WHEN q.kind = 'rename' THEN q.dst_name ELSE b.name END AS name, b.name AS identity, b.kind AS kind, b.size AS size,"
    "  b.mtime AS mtime, b.etag AS etag, b.fileid AS fileid, b.mime AS mime, b.permissions AS permissions, b.favorite AS favorite,"
    "  b.checksum AS checksum, b.availability AS availability, b.reason AS reason,"
    "  CASE q.kind WHEN 'rename' THEN 'renamed' WHEN 'move' THEN 'moving' WHEN 'delete' THEN 'deleting' WHEN 'upload' THEN 'replacing' ELSE '' END AS pending,"
    "  COALESCE(q.state, '') AS sync_state, COALESCE(q.id, 0) AS item_id, 0 AS virt"
    "  FROM base b LEFT JOIN queue_items q ON q.id = b.item_id"
    "  UNION ALL"
    "  SELECT q.dst_name, q.dst_name, q.entry_kind, CASE WHEN q.kind = 'upload' THEN q.src_size END, NULL, '', NULL, '', '', 0, '', 'ok', '',"
    "   CASE q.kind WHEN 'move' THEN 'moving' WHEN 'copy' THEN 'copying' ELSE 'new' END, q.state, q.id, 1"
    "  FROM queue_items q, acct a WHERE q.state <> 'done' AND q.server = a.server AND q.user_id = a.uid AND q.dst_parent = ?1 AND"
    "   ((q.kind = 'upload' AND q.\"replace\" = 0) OR q.kind IN ('mkdir', 'move', 'copy')))";

}  // namespace

OverlayPage Store::entries_page_overlay(const std::string& folder_path, SortKey key, bool ascending, int offset, int limit) const {
    OverlayPage page;
    const std::string folder = normalize_remote_path(folder_path);
    std::lock_guard<std::mutex> lock(mu_);
    {
        Stmt st(db_, (std::string(kOverlay) + " SELECT COUNT(*) FROM eff").c_str());
        if (!st.ok()) return page;
        bind_below(st, 1, folder);
        if (st.step() == SQLITE_ROW) page.total = static_cast<int>(st.int64(0));
    }
    const char* dir = ascending ? "ASC" : "DESC";
    std::string order = "(kind = 'folder') DESC, ";   // folders first, whatever the direction
    switch (key) {
        case SortKey::Name: order += std::string("pbc_fold(name) ") + dir + ", name " + dir; break;
        case SortKey::Date: order += std::string("COALESCE(mtime, 0) ") + dir + ", pbc_fold(name) ASC, name ASC"; break;
        case SortKey::Size: order += std::string("COALESCE(size, -1) ") + dir + ", pbc_fold(name) ASC, name ASC"; break;
    }
    const std::string sql = std::string(kOverlay) +
                            " SELECT name, identity, kind, size, mtime, etag, fileid, mime, permissions, favorite, checksum, availability, reason, pending,"
                            " sync_state, item_id, virt FROM eff ORDER BY " + order + " LIMIT ?4 OFFSET ?5";
    Stmt st(db_, sql.c_str());
    if (!st.ok()) return page;
    bind_below(st, 1, folder);
    st.bind_int64(4, limit < 0 ? 0 : limit);
    st.bind_int64(5, offset < 0 ? 0 : offset);
    while (st.step() == SQLITE_ROW) {
        OverlayRow r;
        r.entry.name = st.text(0);
        r.entry.path = join_remote(folder, st.text(1));
        r.entry.kind = st.text(2) == "folder" ? Kind::Folder : Kind::File;
        r.entry.size = opt_col(st, 3);
        r.entry.mtime = opt_col(st, 4);
        r.entry.etag = st.text(5);
        r.entry.fileid = opt_col(st, 6);
        r.entry.mime = st.text(7);
        r.entry.permissions = st.text(8);
        r.entry.favorite = st.int64(9) != 0;
        r.entry.checksum = st.text(10);
        r.entry.availability = availability_of(st.text(11));
        r.entry.reason = st.text(12);
        r.pending = st.text(13);
        r.sync_state = st.text(14);
        r.item_id = st.int64(15);
        r.pending_row = st.int64(16) != 0;
        page.rows.push_back(std::move(r));
    }
    return page;
}

std::vector<QueueItem> Store::queue_items_for_source(const std::string& remote_path) const {
    std::vector<QueueItem> out;
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, (std::string("SELECT ") + kColumns + " FROM queue_items WHERE state <> 'done' AND (src_remote = ?1 OR (dst_parent = ?2 AND dst_name = ?3)) ORDER BY id").c_str());
    if (!st.ok()) return out;
    st.bind_text(1, remote_path);
    st.bind_text(2, parent_of(remote_path));
    st.bind_text(3, remote_path.substr(remote_path.rfind('/') + 1));
    while (st.step() == SQLITE_ROW) out.push_back(item_from(st));
    return out;
}

std::vector<QueueItem> Store::queue_items_below(const std::string& folder) const {
    std::vector<QueueItem> out;
    const std::string f = normalize_remote_path(folder);
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, (std::string("SELECT ") + kColumns + " FROM queue_items WHERE state <> 'done' AND"
                  " (dst_parent = ?1 OR (dst_parent >= ?2 AND dst_parent < ?3) OR (src_remote >= ?2 AND src_remote < ?3)) ORDER BY id").c_str());
    if (!st.ok()) return out;
    bind_below(st, 1, f);
    while (st.step() == SQLITE_ROW) out.push_back(item_from(st));
    return out;
}

}  // namespace pbc
