#include "pbc/trash_rules.h"

#include <algorithm>

#include "pbc/new_name.h"
#include "pbc/remote_path.h"
#include "pbc/utf8.h"

namespace pbc {

namespace {

// One app: Off when the key is missing or false, On when it is true, Unknown when it is something else.
FeatureState state_of(const JsonValue& files, const char* key) {
    const JsonValue* v = files.get(key);
    if (!v) return FeatureState::Off;
    if (v->type != JsonValue::Type::Bool) return FeatureState::Unknown;
    return v->boolean ? FeatureState::On : FeatureState::Off;
}

// at most `max` bytes, never cutting inside a character
std::string cut_on_boundary(const std::string& s, size_t max) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const Utf8Char ch = utf8_decode(s, i);
        const size_t len = ch.len ? ch.len : 1;
        if (out.size() + len > max) break;
        out.append(s, i, len);
        i += len;
    }
    return out;
}

std::string original_path_of(const TrashItem& item) { return join_remote(item.original_dir, item.original_name); }

}  // namespace

TrashAvailability availability_from_capabilities(const JsonValue& capabilities) {
    TrashAvailability a;   // Unknown for both
    if (capabilities.type != JsonValue::Type::Object) return a;
    const JsonValue* files = capabilities.get("files");
    if (!files || files->type != JsonValue::Type::Object) return a;
    a.trash = state_of(*files, "undelete");
    a.versions = state_of(*files, "versioning");
    return a;
}

bool actionable(const TrashItem& item) { return !item.unreadable && item.availability == Availability::Ok; }

RestorePlan plan_restore(const TrashItem& item, const RestoreProbe& probe) {
    if (!actionable(item)) return RestorePlan::Unavailable;
    if (!item.original_known || !probe.original_known) return RestorePlan::ChoosePlace;
    if (probe.at_original) return RestorePlan::NameTaken;
    if (probe.parent_exists && !*probe.parent_exists) return RestorePlan::ParentMissing;
    return RestorePlan::OriginalFree;
}

TargetCheck check_restore_target(const TrashItem& item, const RestoreTarget& target, const ServerCaps& caps) {
    TargetCheck c;
    if (!is_valid_remote_path(target.folder)) {
        c.error_key = "restore_parent_missing";
        return c;
    }
    if (!check_new_name(target.name, caps).ok) {
        c.error_key = "name_forbidden";
        return c;
    }
    if (item.kind == Kind::Folder && item.original_known && !target.is_original) {
        const std::string own = original_path_of(item);
        const std::string folder = normalize_remote_path(target.folder);
        if (folder == own || folder.compare(0, own.size() + 1, own + "/") == 0) {   // the folder would end up inside the place it came from
            c.error_key = "restore_into_self";
            return c;
        }
    }
    c.ok = true;
    return c;
}

int confirm_stages(ConfirmKind kind) { return kind == ConfirmKind::DeleteFolder || kind == ConfirmKind::EmptyTrash ? 2 : 1; }

ConfirmContent confirm_for_restore(const TrashItem& item, const RestoreTarget& target) {
    ConfirmContent c;
    c.kind = ConfirmKind::RestoreProposal;
    c.shown_names.push_back(cut_on_boundary(item.name, 40));
    c.items = 1;
    c.folders = item.kind == Kind::Folder ? 1 : 0;
    c.original_place = target.is_original;
    c.place = target.folder;
    return c;
}

ConfirmContent confirm_for_version(const std::string& file_name, const VersionRow& row) {
    ConfirmContent c;
    c.kind = ConfirmKind::RestoreVersion;
    c.shown_names.push_back(cut_on_boundary(file_name, 40));
    c.items = 1;
    c.version_mtime = row.mtime;
    c.version_size = row.size;
    return c;
}

ConfirmContent confirm_for_delete(const TrashItem& item, int stage) {
    ConfirmContent c;
    c.kind = item.kind == Kind::Folder ? ConfirmKind::DeleteFolder : ConfirmKind::DeleteFile;
    c.shown_names.push_back(cut_on_boundary(item.name, 40));
    c.items = 1;
    c.folders = item.kind == Kind::Folder ? 1 : 0;
    c.stage = std::max(1, std::min(stage, confirm_stages(c.kind)));
    c.destructive = true;
    c.default_cancel = true;
    return c;
}

ConfirmContent confirm_for_empty(const std::vector<TrashItem>& named, bool list_truncated, int stage) {
    ConfirmContent c;
    c.kind = ConfirmKind::EmptyTrash;
    c.items = static_cast<int>(named.size());
    for (const TrashItem& i : named) c.folders += i.kind == Kind::Folder ? 1 : 0;
    for (size_t k = 0; k < named.size() && k < 5; ++k) c.shown_names.push_back(cut_on_boundary(named[k].name, 40));
    c.truncated_notice = list_truncated;
    c.stage = std::max(1, std::min(stage, confirm_stages(c.kind)));
    c.destructive = true;
    c.default_cancel = true;
    return c;
}

EmptySet empty_set_of(const TrashList& list) {
    EmptySet s;
    for (const TrashItem& i : list.items) {
        if (actionable(i)) s.named.push_back(i);
        else ++s.not_actionable;
    }
    return s;
}

LoopStep loop_step_after(const OperationResult& r) {
    if (r.outcome_unknown) return LoopStep::StopUnknown;
    if (r.error.key == "auth_failed") return LoopStep::StopAuth;
    if (r.error.key == "rate_limited") return LoopStep::StopRateLimited;
    if (!r.error.ok() && (r.error.cls == ErrorClass::Network || r.error.cls == ErrorClass::Tls)) return LoopStep::StopConnection;
    return LoopStep::Continue;
}

Refusal refusal_for(int op, long status) {
    Refusal r;
    r.code = "T" + std::to_string(op) + "-" + std::to_string(status);
    const bool restore = op == 4 || op == 5;
    if (status == 401) r.key = "auth_failed";
    else if (status == 429) r.key = "rate_limited";
    else if (status == 507) r.key = "server_storage_full";
    else if (status == 423) r.key = "locked";
    else if (status == 207) r.key = "partial_failure";
    else if (status == 404) r.key = op == 7 ? "version_gone" : op == 6 ? "file_gone" : op == 1 || op == 2 ? "protocol_error" : "trash_item_gone";
    else if (status == 409) r.key = op == 5 ? "restore_parent_missing" : restore ? "restore_name_taken" : "protocol_error";
    else if (status == 412) r.key = restore ? "restore_name_taken" : op == 7 ? "version_changed_meanwhile" : "protocol_error";
    else if (op == 5 && (status == 403 || status == 405 || status == 501 || status == 502)) r.key = "restore_place_unsupported";
    else if (status == 403) r.key = "forbidden";
    else if (status >= 500) r.key = "server_error";
    else r.key = "protocol_error";
    return r;
}

}  // namespace pbc
