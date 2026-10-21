#include "pbc/queue_actions.h"

#include <algorithm>
#include <set>

#include "pbc/error_keys.h"
#include "pbc/new_name.h"
#include "pbc/remote_path.h"
#include "pbc/server_caps.h"
#include "pbc/upload_source.h"

namespace pbc {

namespace {

std::string base_name(const std::string& path) { return path.substr(path.rfind('/') + 1); }

bool strong_etag(const std::string& etag) { return !etag.empty() && etag.compare(0, 2, "W/") != 0 && etag.size() <= 256; }

QueueItem base_item(const AccountRow& a) {
    QueueItem q;
    q.server = a.base_url;
    q.user_id = a.user_id;
    return q;
}

}  // namespace

std::optional<NameAsk> QueueActions::ask_for(const std::string& source, const std::string& folder, const std::string& name, const ServerCaps& caps,
                                             const std::set<std::string>& also_taken) const {
    const NameUse use = store_->name_use(folder, name);
    const bool in_call = also_taken.count(name) == 1;
    if (!use.any() && !in_call) return std::nullopt;
    NameAsk a;
    a.source = source;
    a.target_folder = folder;
    a.name = name;
    a.in_queue = use.queued || in_call;
    const auto entry = store_->entry(join_remote(folder, name));
    if (entry) {
        a.existing_is_folder = entry->kind == Kind::Folder;
        a.existing_etag = entry->etag;
        a.existing_size = entry->size;
        a.existing_mtime = entry->mtime;
        if (entry->kind == Kind::File && entry->availability == Availability::Ok) {
            a.replace_possible = strong_etag(entry->etag) && !a.in_queue;
            a.refresh_needed = !strong_etag(entry->etag);
        }
    }
    a.suggested = keep_both_name(name, [&](std::string_view candidate) {
        const std::string n(candidate);
        return store_->name_use(folder, n).any() || also_taken.count(n) == 1;
    }, caps);
    return a;
}

int QueueActions::headroom() const { return std::max(0, kMaxQueueItems - store_->queue_page(0, 0).total); }

AddResult QueueActions::add_uploads(const std::string& target_folder, const std::vector<std::string>& picked_files) {
    AddResult out;
    if (!is_valid_remote_path(target_folder)) {
        out.error = make_error(ErrorClass::Protocol, "protocol_error", "the target folder is not a valid path");
        return out;
    }
    const auto account = store_->account();
    if (!account) {
        out.error = make_error(ErrorClass::Auth, "auth_failed", "there is no account");
        return out;
    }
    const ServerCaps caps = store_->server_caps().value_or(ServerCaps());
    std::vector<QueueItem> items;
    std::set<std::string> claimed;   // names the files of this call take
    for (const std::string& path : picked_files) {
        const auto identity = inspect_source(path, rules_);
        if (!identity.ok()) {
            out.refused.push_back({path, identity.error.key});
            continue;
        }
        const auto canonical = check_source_path(path, rules_);   // inspect_source checked it; this gives the canonical path
        const std::string name = base_name(path);
        if (!check_new_name(name, caps).ok) {
            out.refused.push_back({path, "name_forbidden"});
            continue;
        }
        if (auto ask = ask_for(canonical.ok() ? canonical.value : path, target_folder, name, caps, claimed)) {   // taken: asked, not queued
            out.asks.push_back(std::move(*ask));
            continue;
        }
        claimed.insert(name);
        QueueItem q = base_item(*account);
        q.depends_on = store_->queue_pending_mkdir(target_folder);   // an upload into a folder that is still queued waits for it
        q.kind = ItemKind::Upload;
        q.entry_kind = Kind::File;
        q.src_local = canonical.ok() ? canonical.value : path;
        q.src_size = identity.value.size;
        q.src_mtime = identity.value.mtime;
        q.dst_parent = target_folder;
        q.dst_name = name;
        items.push_back(std::move(q));
    }
    if (items.empty()) return out;
    if (static_cast<int>(items.size()) > headroom()) {
        out.error = make_error(ErrorClass::Storage, "queue_full", "the queue is full");
        return out;
    }
    if (items.size() == 1) {
        const auto id = store_->queue_add(items[0]);
        if (!id.ok()) {
            out.error = id.error;
            return out;
        }
        out.queued.push_back(id.value);
        return out;
    }
    const auto ids = store_->queue_add_group(items, std::vector<int>(items.size(), -1));
    if (!ids.ok()) {
        out.error = ids.error;
        return out;
    }
    out.queued = ids.value;
    return out;
}

FolderPick QueueActions::count_folder_pick(const std::string& target_folder, const std::vector<std::string>& picked_folders) const {
    FolderPick pick;
    pick.target = target_folder;
    if (!is_valid_remote_path(target_folder)) {
        pick.error = make_error(ErrorClass::Protocol, "protocol_error", "the target folder is not a valid path");
        return pick;
    }
    if (!store_->account()) {
        pick.error = make_error(ErrorClass::Auth, "auth_failed", "there is no account");
        return pick;
    }
    const ServerCaps caps = store_->server_caps().value_or(ServerCaps());
    for (const std::string& path : picked_folders) {
        const auto canonical = check_source_path(path, rules_);
        if (!canonical.ok()) {
            pick.refused.push_back({path, canonical.error.key});
            continue;
        }
        SourceWalk walk = walk_source(canonical.value, rules_);
        if (walk.entries.empty()) {   // not a folder
            pick.refused.push_back({path, "source_unreadable"});
            continue;
        }
        pick.skipped += walk.skipped;
        if (walk.over_limit) pick.over_limit = true;
        // the server's rules for names: an entry that fails is left out, and a folder that fails takes what is inside it along
        std::vector<SourceEntry> kept;
        std::set<std::string> dropped_folders;
        for (const SourceEntry& e : walk.entries) {
            bool inside_dropped = false;
            for (const std::string& d : dropped_folders)
                if (e.rel == d || (e.rel.size() > d.size() && e.rel.compare(0, d.size() + 1, d + "/") == 0)) inside_dropped = true;
            if (inside_dropped) continue;
            if (!check_new_name(e.name, caps).ok) {
                pick.refused.push_back({e.rel.empty() ? path : path + "/" + e.rel, "name_forbidden"});
                if (e.kind == SourceKind::Folder) dropped_folders.insert(e.rel);
                if (e.rel.empty()) {   // the picked folder itself: nothing of it can be sent
                    kept.clear();
                    break;
                }
                continue;
            }
            kept.push_back(e);
        }
        PickedFolder pf;
        pf.canonical = canonical.value;
        pf.walk.entries = std::move(kept);
        pf.walk.skipped = walk.skipped;
        pf.walk.over_limit = walk.over_limit;
        for (const SourceEntry& e : pf.walk.entries) {
            if (e.kind == SourceKind::Folder) ++pf.walk.folders;
            else {
                ++pf.walk.files;
                pf.walk.bytes += e.size;
            }
        }
        if (pf.walk.entries.empty()) continue;
        pick.folders_count += pf.walk.folders;
        pick.files += pf.walk.files;
        pick.bytes += pf.walk.bytes;
        pick.folders.push_back(std::move(pf));
    }
    return pick;
}

AddResult QueueActions::confirm_folder_pick(const FolderPick& pick, PickPolicy policy) {
    AddResult out;
    out.refused = pick.refused;
    if (!pick.error.ok()) {
        out.error = pick.error;
        return out;
    }
    if (pick.over_limit || pick.items() > headroom()) {
        out.error = make_error(ErrorClass::Storage, "queue_full", "the pick does not fit into the queue");
        return out;
    }
    if (pick.items() == 0) return out;
    const auto account = store_->account();
    if (!account) {
        out.error = make_error(ErrorClass::Auth, "auth_failed", "there is no account");
        return out;
    }
    const ServerCaps caps = store_->server_caps().value_or(ServerCaps());
    std::vector<QueueItem> items;
    std::vector<int> deps;
    for (const PickedFolder& pf : pick.folders) {
        // rel path of a folder of this pick -> the index of its creation in `items`, or -1 when the folder exists on the server already
        std::map<std::string, int> made;
        auto remote_of = [&](const std::string& rel) {
            const std::string top = base_name(pf.canonical);
            return join_remote(pick.target, rel.empty() ? top : top + "/" + rel);
        };
        for (const SourceEntry& e : pf.walk.entries) {
            const std::string parent_rel = e.rel.empty() ? std::string() : (e.rel.rfind('/') == std::string::npos ? std::string() : e.rel.substr(0, e.rel.rfind('/')));
            if (e.kind == SourceKind::Folder) {
                const std::string remote = remote_of(e.rel);
                const std::string remote_parent = parent_of(remote);
                const auto known = store_->entry(remote);
                int parent_dep = -1;
                if (!e.rel.empty()) {
                    const auto it = made.find(parent_rel);
                    if (it != made.end()) parent_dep = it->second;
                }
                if (known && known->kind == Kind::Folder) {   // the stored listing shows it on the server: it is used, not created
                    made[e.rel] = -1;
                    continue;
                }
                QueueItem q = base_item(*account);
                q.kind = ItemKind::Mkdir;
                q.entry_kind = Kind::Folder;
                q.dst_parent = remote_parent;
                q.dst_name = e.name;
                if (parent_dep < 0) q.depends_on = store_->queue_pending_mkdir(remote_parent);
                items.push_back(std::move(q));
                deps.push_back(parent_dep);
                made[e.rel] = static_cast<int>(items.size()) - 1;
            } else {
                int dep = -1;
                const auto it = made.find(parent_rel);
                if (it != made.end()) dep = it->second;
                QueueItem q = base_item(*account);
                q.kind = ItemKind::Upload;
                q.entry_kind = Kind::File;
                q.src_local = pf.canonical + "/" + e.rel;
                q.src_size = e.size;
                q.dst_parent = remote_of(parent_rel);
                q.dst_name = e.name;
                if (dep < 0) q.depends_on = store_->queue_pending_mkdir(q.dst_parent);
                // a name that is taken (only possible in a folder that exists on the server already): the question or the one answer for all
                if (const auto ask = dep < 0 ? ask_for(q.src_local, q.dst_parent, q.dst_name, caps, {}) : std::nullopt) {
                    if (policy == PickPolicy::Skip) continue;
                    if (policy == PickPolicy::KeepBoth && !ask->suggested.empty()) {
                        q.dst_name = ask->suggested;
                    } else if (policy == PickPolicy::Replace && ask->replace_possible) {
                        q.replace = true;
                        q.dst_etag = ask->existing_etag;
                    } else {
                        out.asks.push_back(*ask);
                        continue;
                    }
                }
                items.push_back(std::move(q));
                deps.push_back(dep);
            }
        }
    }
    if (items.empty()) return out;
    const auto ids = store_->queue_add_group(items, deps);
    if (!ids.ok()) {
        out.error = ids.error;
        return out;
    }
    out.queued = ids.value;
    return out;
}

AddResult QueueActions::answer_ask(const NameAsk& ask, AskChoice choice, const std::string& new_name) {
    AddResult out;
    if (choice == AskChoice::Skip) return out;
    const auto account = store_->account();
    if (!account) {
        out.error = make_error(ErrorClass::Auth, "auth_failed", "there is no account");
        return out;
    }
    const ServerCaps caps = store_->server_caps().value_or(ServerCaps());
    QueueItem q = base_item(*account);
    q.dst_parent = ask.target_folder;
    q.depends_on = store_->queue_pending_mkdir(ask.target_folder);
    bool source_is_file = true;
    if (ask.kind == AskKind::Upload) {
        const auto identity = inspect_source(ask.source, rules_);   // the file may have changed or gone while the question was open
        if (!identity.ok()) {
            out.refused.push_back({ask.source, identity.error.key});
            return out;
        }
        q.kind = ItemKind::Upload;
        q.entry_kind = Kind::File;
        q.src_local = ask.source;
        q.src_size = identity.value.size;
        q.src_mtime = identity.value.mtime;
    } else {   // a move or a copy of an entry of a cached listing: its version is read again
        const auto src = store_->entry(ask.source);
        if (!src || !strong_etag(src->etag)) {
            out.error = error_from_key("refresh_first");
            return out;
        }
        q.kind = ask.kind == AskKind::Move ? ItemKind::Move : ask.kind == AskKind::Rename ? ItemKind::Rename : ItemKind::Copy;
        q.entry_kind = src->kind;
        q.src_remote = ask.source;
        q.src_etag = src->etag;
        q.src_fileid = src->fileid;
        source_is_file = src->kind == Kind::File;
    }
    if (headroom() < 1) {
        out.error = make_error(ErrorClass::Storage, "queue_full", "the queue is full");
        return out;
    }
    if (choice == AskChoice::KeepBoth) {
        const std::string name = new_name.empty() ? ask.suggested : new_name;
        if (name.empty() || !check_new_name(name, caps).ok) {
            out.error = error_from_key("name_forbidden");
            return out;
        }
        if (auto again = ask_for(ask.source, ask.target_folder, name, caps, {})) {   // the new name is taken as well
            again->kind = ask.kind;
            out.asks.push_back(std::move(*again));
            return out;
        }
        q.dst_name = name;
    } else {   // replace
        const auto entry = store_->entry(join_remote(ask.target_folder, ask.name));
        if (!entry || entry->kind != Kind::File || entry->availability != Availability::Ok || !source_is_file) {
            out.error = error_from_key("not_allowed");   // a folder is never replaced, and what is not there cannot be
            return out;
        }
        if (!strong_etag(entry->etag)) {
            out.error = error_from_key("refresh_first");
            return out;
        }
        if (store_->name_use(ask.target_folder, ask.name).queued) {
            out.error = error_from_key("not_allowed");   // another open item already makes a change at this name
            return out;
        }
        q.dst_name = ask.name;
        q.replace = true;
        q.dst_etag = entry->etag;
    }
    const auto id = store_->queue_add(q);
    if (!id.ok()) {
        out.error = id.error;
        return out;
    }
    out.queued.push_back(id.value);
    return out;
}

AddResult QueueActions::move_or_copy(ItemKind kind, const std::vector<std::string>& remote_paths, const std::string& target_folder) {
    AddResult out;
    if ((kind != ItemKind::Move && kind != ItemKind::Copy) || !is_valid_remote_path(target_folder)) {
        out.error = make_error(ErrorClass::Protocol, "protocol_error", "a move or a copy needs a valid target folder");
        return out;
    }
    const auto account = store_->account();
    if (!account) {
        out.error = make_error(ErrorClass::Auth, "auth_failed", "there is no account");
        return out;
    }
    const ServerCaps caps = store_->server_caps().value_or(ServerCaps());
    std::vector<QueueItem> items;
    std::set<std::string> claimed;
    const auto inside_or_equal = [](const std::string& path, const std::string& folder) {
        return path == folder || (path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 && path[folder.size()] == '/');
    };
    for (const std::string& path : remote_paths) {
        const auto entry = store_->entry(path);
        if (!entry || !strong_etag(entry->etag)) {
            out.refused.push_back({path, "refresh_first"});
            continue;
        }
        if (!store_->queue_items_for_source(path).empty()) {
            out.refused.push_back({path, "change_pending"});
            continue;
        }
        if (entry->kind == Kind::Folder && inside_or_equal(target_folder, path)) {
            out.refused.push_back({path, "not_allowed"});
            continue;
        }
        if (kind == ItemKind::Move && parent_of(path) == target_folder) {
            out.refused.push_back({path, "name_exists"});
            continue;
        }
        if (auto ask = ask_for(path, target_folder, entry->name, caps, claimed)) {
            ask->kind = kind == ItemKind::Move ? AskKind::Move : AskKind::Copy;
            out.asks.push_back(std::move(*ask));
            continue;
        }
        claimed.insert(entry->name);
        QueueItem q = base_item(*account);
        q.kind = kind;
        q.entry_kind = entry->kind;
        q.src_remote = path;
        q.src_etag = entry->etag;
        q.src_fileid = entry->fileid;
        q.dst_parent = target_folder;
        q.dst_name = entry->name;
        q.depends_on = store_->queue_pending_mkdir(target_folder);
        items.push_back(std::move(q));
    }
    if (items.empty()) return out;
    if (static_cast<int>(items.size()) > headroom()) {
        out.error = make_error(ErrorClass::Storage, "queue_full", "the queue is full");
        return out;
    }
    if (items.size() == 1) {
        const auto id = store_->queue_add(items[0]);
        if (!id.ok()) {
            out.error = id.error;
            return out;
        }
        out.queued.push_back(id.value);
        return out;
    }
    const auto ids = store_->queue_add_group(items, std::vector<int>(items.size(), -1));
    if (!ids.ok()) {
        out.error = ids.error;
        return out;
    }
    out.queued = ids.value;
    return out;
}

DeletePreview QueueActions::delete_preview(const std::vector<std::string>& remote_paths) const {
    DeletePreview p;
    const auto caps = store_->server_caps();
    p.offered = caps && delete_offered(*caps);
    p.trash_known = caps && caps->trash.has_value();
    for (const std::string& path : remote_paths) {
        const auto entry = store_->entry(path);
        ++p.entries;
        if (p.names.size() < 5) p.names.push_back(entry ? entry->name : path.substr(path.rfind('/') + 1));
        if (entry && entry->kind == Kind::Folder) {
            ++p.folders;
            p.items_inside += static_cast<int>(store_->queue_items_below(path).size());
        }
    }
    return p;
}

AddResult QueueActions::delete_entries(const std::vector<std::string>& remote_paths) {
    AddResult out;
    const auto account = store_->account();
    if (!account) {
        out.error = make_error(ErrorClass::Auth, "auth_failed", "there is no account");
        return out;
    }
    if (!delete_offered(store_->server_caps().value_or(ServerCaps()))) {   // a delete here would be permanent: it is not offered (research R-9)
        out.error = error_from_key("trash_unavailable");
        return out;
    }
    std::vector<QueueItem> items;
    for (const std::string& path : remote_paths) {
        const auto entry = store_->entry(path);
        if (!entry || !strong_etag(entry->etag)) {
            out.refused.push_back({path, "refresh_first"});
            continue;
        }
        if (!store_->queue_items_for_source(path).empty()) {
            out.refused.push_back({path, "change_pending"});
            continue;
        }
        QueueItem q = base_item(*account);
        q.kind = ItemKind::Delete;
        q.entry_kind = entry->kind;
        q.src_remote = path;
        q.src_etag = entry->etag;
        q.src_fileid = entry->fileid;
        items.push_back(std::move(q));
    }
    if (items.empty()) return out;
    if (static_cast<int>(items.size()) > headroom()) {
        out.error = make_error(ErrorClass::Storage, "queue_full", "the queue is full");
        return out;
    }
    if (items.size() == 1) {
        const auto id = store_->queue_add(items[0]);
        if (!id.ok()) {
            out.error = id.error;
            return out;
        }
        out.queued.push_back(id.value);
        return out;
    }
    const auto ids = store_->queue_add_group(items, std::vector<int>(items.size(), -1));
    if (!ids.ok()) {
        out.error = ids.error;
        return out;
    }
    out.queued = ids.value;
    return out;
}

AddResult QueueActions::new_folder(const std::string& parent, const std::string& name) {
    AddResult out;
    if (!is_valid_remote_path(parent)) {
        out.error = make_error(ErrorClass::Protocol, "protocol_error", "the parent folder is not a valid path");
        return out;
    }
    const auto account = store_->account();
    if (!account) {
        out.error = make_error(ErrorClass::Auth, "auth_failed", "there is no account");
        return out;
    }
    const ServerCaps caps = store_->server_caps().value_or(ServerCaps());
    if (!check_new_name(name, caps).ok) {
        out.error = error_from_key("name_forbidden");
        return out;
    }
    if (store_->name_use(parent, name).any()) {
        out.error = error_from_key("name_exists");
        return out;
    }
    if (headroom() < 1) {
        out.error = make_error(ErrorClass::Storage, "queue_full", "the queue is full");
        return out;
    }
    QueueItem q = base_item(*account);
    q.kind = ItemKind::Mkdir;
    q.entry_kind = Kind::Folder;
    q.dst_parent = parent;
    q.dst_name = name;
    q.depends_on = store_->queue_pending_mkdir(parent);
    const auto id = store_->queue_add(q);
    if (!id.ok()) {
        out.error = id.error;
        return out;
    }
    out.queued.push_back(id.value);
    return out;
}

AddResult QueueActions::rename_entry(const std::string& remote_path, const std::string& new_name) {
    AddResult out;
    const auto account = store_->account();
    if (!account) {
        out.error = make_error(ErrorClass::Auth, "auth_failed", "there is no account");
        return out;
    }
    const ServerCaps caps = store_->server_caps().value_or(ServerCaps());
    if (!check_new_name(new_name, caps).ok) {
        out.error = error_from_key("name_forbidden");
        return out;
    }
    const auto entry = store_->entry(remote_path);
    if (!entry || !strong_etag(entry->etag)) {
        out.error = error_from_key("refresh_first");
        return out;
    }
    const std::string parent = parent_of(remote_path);
    if (new_name == entry->name) {
        out.error = error_from_key("name_exists");
        return out;
    }
    if (!store_->queue_items_for_source(remote_path).empty()) {
        out.error = error_from_key("change_pending");   // an earlier change of this entry is still waiting
        return out;
    }
    if (auto ask = ask_for(remote_path, parent, new_name, caps, {})) {   // the name is taken: the user decides before anything is queued
        ask->kind = AskKind::Rename;
        out.asks.push_back(std::move(*ask));
        return out;
    }
    if (headroom() < 1) {
        out.error = make_error(ErrorClass::Storage, "queue_full", "the queue is full");
        return out;
    }
    QueueItem q = base_item(*account);
    q.kind = ItemKind::Rename;
    q.entry_kind = entry->kind;
    q.src_remote = remote_path;
    q.src_etag = entry->etag;
    q.src_fileid = entry->fileid;
    q.dst_parent = parent;
    q.dst_name = new_name;
    const auto id = store_->queue_add(q);
    if (!id.ok()) {
        out.error = id.error;
        return out;
    }
    out.queued.push_back(id.value);
    return out;
}

Error QueueActions::remove_item(int64_t id) { return store_->queue_remove(id); }

}  // namespace pbc
