#include "pbc/trash_api.h"

#include "pbc/dav_write.h"
#include "pbc/error_keys.h"
#include "pbc/nc_listing.h"
#include "pbc/new_name.h"
#include "pbc/json_min.h"
#include "pbc/log.h"
#include "pbc/remote_path.h"
#include "pbc/trash_parse.h"
#include "pbc/version.h"

namespace pbc {

namespace {

Headers ocs_headers() { return {{"OCS-APIRequest", "true"}, {"Accept", "application/json"}, {"User-Agent", std::string("PBCloud/") + version()}}; }

std::string code_of(int op, long status) { return "T" + std::to_string(op) + "-" + std::to_string(status); }

}  // namespace

Error interpret_trash_answer(const Response& r, int op) {
    const WriteKind kind = op == 8 ? WriteKind::Delete : WriteKind::Move;
    if (r.status == 0) return interpret_write_answer(r, kind, false).error;   // no answer: unknown, no connection, a certificate, a cancel
    if (r.status >= 200 && r.status < 300) return r.status == 207 ? error_from_key("partial_failure") : Error();
    const std::string cls = sabre_exception_class(r.body);
    if (cls == "InsufficientStorage") return error_from_key("server_storage_full");
    if (cls == "FileLocked" && r.status != 401 && r.status != 429 && r.status != 503) return error_from_key("locked");
    if ((cls == "InvalidPath" || cls == "ForbiddenFileName") && (r.status == 400 || r.status == 403)) return error_from_key("name_forbidden");
    return error_from_key(refusal_for(op, r.status).key);
}

namespace {

// a segment of an address: percent-encoded like a path of one segment
std::string encode_segment(const std::string& s) { return encode_path("/" + s).substr(1); }

bool matches(const RemoteEntry& e, const TrashItem& item) {
    if (e.kind != item.kind) return false;
    if (item.kind == Kind::File && item.size && e.size) return *item.size == *e.size;
    return true;
}

}  // namespace

TrashApi::TrashApi(DavSession& session, std::string_view user_id, const Clock& clock) : session_(session), user_id_(user_id), clock_(clock) {}

std::string TrashApi::trash_base() const { return session_.base_url() + "/remote.php/dav/trashbin" + encode_path("/" + user_id_) + "/"; }

std::string TrashApi::versions_base() const { return session_.base_url() + "/remote.php/dav/versions" + encode_path("/" + user_id_) + "/"; }

int64_t TrashApi::now() const { return session_.last_server_time().value_or(clock_.now()); }

OperationResult TrashApi::delete_item(const TrashItem& item) {
    OperationResult out;
    if (!actionable(item) || !is_safe_segment(item.trash_name)) {
        out.error = error_from_key("trash_not_actionable");
        out.not_removed = 1;
        return out;
    }
    Headers headers = {{"User-Agent", std::string("PBCloud/") + version()}};
    const Response r = session_.send("DELETE", trash_base() + "trash/" + encode_segment(item.trash_name), std::string(), headers, 64 << 10, kTrashTotalTimeoutS);
    out.code = code_of(8, r.status);
    log::event("trash.delete", {log::status(r.status)});
    if (r.status == 404) {   // already gone: what was asked for has happened
        out.removed = 1;
        return out;
    }
    const Error e = interpret_trash_answer(r, 8);
    if (e.ok()) {
        out.removed = 1;
        return out;
    }
    out.error = e;
    if (e.key == "outcome_unknown") {
        out.outcome_unknown = true;
        out.unknown_trash_name = item.trash_name;
        return out;
    }
    log::event("trash.fail", {log::status(r.status), log::count("op", 8)});
    out.not_removed = 1;
    return out;
}

OperationResult TrashApi::empty_trash(const std::vector<TrashItem>& named, const std::function<bool(const EmptyProgress&)>& progress) {
    OperationResult out;
    EmptyProgress p;
    p.total = static_cast<int>(named.size());
    size_t handled = 0;
    for (const TrashItem& item : named) {
        const OperationResult r = delete_item(item);
        ++handled;
        ++p.done;
        p.removed += r.removed;
        p.not_removed += r.not_removed;
        out.removed += r.removed;
        out.not_removed += r.not_removed;
        out.code = r.code;
        if (r.removed) out.removed_trash_names.push_back(item.trash_name);
        const LoopStep step = loop_step_after(r);
        if (step != LoopStep::Continue) {   // the run stops here: the reason is the error of the request
            out.error = r.error;
            out.outcome_unknown = r.outcome_unknown;
            out.unknown_trash_name = r.unknown_trash_name;
            break;
        }
        if (progress && !progress(p)) {
            out.error = error_from_key("cancelled");
            break;
        }
    }
    out.not_tried = static_cast<int>(named.size() - handled);
    log::event("trash.empty", {log::count("removed", out.removed), log::count("refused", out.not_removed), log::count("not_tried", out.not_tried)});
    return out;
}

Result<RestorePlan> TrashApi::probe_place(std::string_view folder_view, std::string_view name_view, RestoreProbe* detail) {
    Result<RestorePlan> out;
    out.value = RestorePlan::OriginalFree;
    const std::string folder = normalize_remote_path(folder_view);
    const std::string name(name_view);
    RestoreProbe probe;
    probe.original_known = true;
    const auto entry = fetch_entry(session_, user_id_, join_remote(folder, name));
    if (!entry.ok()) {
        out.error = entry.error;
        return out;
    }
    if (entry.value) {
        probe.at_original = *entry.value;
        out.value = RestorePlan::NameTaken;
    } else if (folder != "/") {
        const auto parent = fetch_entry(session_, user_id_, folder);
        if (!parent.ok()) {
            out.error = parent.error;
            return out;
        }
        probe.parent_exists = parent.value.has_value() && parent.value->kind == Kind::Folder;
        if (!*probe.parent_exists) out.value = RestorePlan::ParentMissing;
    }
    if (detail) *detail = probe;
    return out;
}

Result<RestorePlan> TrashApi::probe_restore(const TrashItem& item, RestoreProbe* detail) {
    Result<RestorePlan> out;
    if (!actionable(item)) {
        out.value = RestorePlan::Unavailable;
        return out;
    }
    if (!item.original_known) {
        out.value = RestorePlan::ChoosePlace;
        return out;
    }
    RestoreProbe probe;
    const auto place = probe_place(item.original_dir, item.original_name, &probe);
    if (!place.ok()) {
        out.error = place.error;
        return out;
    }
    probe.original_known = item.original_known;
    out.value = plan_restore(item, probe);
    if (detail) *detail = probe;
    return out;
}

// One MOVE of a trash item with Overwrite: F, then the place it should have reached is probed (research R-5, R-6).
OperationResult TrashApi::move_item(const TrashItem& item, int op, const std::string& destination, const std::string& verify_folder, const std::string& verify_name) {
    OperationResult out;
    Headers headers = {{"Destination", destination}, {"Overwrite", "F"}, {"User-Agent", std::string("PBCloud/") + version()}};
    const Response r = session_.send("MOVE", trash_base() + "trash/" + encode_segment(item.trash_name), std::string(), headers, 64 << 10, kTrashTotalTimeoutS);
    out.code = code_of(op, r.status);
    if (op == 4) log::event("trash.restore", {log::status(r.status)});
    else log::event("trash.restore_chosen", {log::status(r.status)});
    Error e = interpret_trash_answer(r, op);
    if (e.key == "outcome_unknown") {
        out.error = e;
        out.outcome_unknown = true;
        return out;
    }
    if (!e.ok()) {
        if (op == 5 && e.key == "restore_place_unsupported") {   // the server says it cannot: unless the probe shows that it did
            const auto there = fetch_entry(session_, user_id_, join_remote(verify_folder, verify_name));
            if (there.ok() && there.value && matches(*there.value, item)) return out;
        }
        log::event("trash.fail", {log::status(r.status), log::count("op", op)});
        out.error = e;
        return out;
    }
    const auto there = fetch_entry(session_, user_id_, join_remote(verify_folder, verify_name));
    if (there.ok() && !(there.value && matches(*there.value, item))) out.landed_elsewhere = true;   // an unreadable probe is not a claim either way
    return out;
}

OperationResult TrashApi::restore_original(const TrashItem& item) {
    OperationResult out;
    if (!actionable(item) || !item.original_known || !is_safe_segment(item.trash_name)) {
        out.error = error_from_key("trash_not_actionable");
        return out;
    }
    return move_item(item, 4, trash_base() + "restore/" + encode_segment(item.trash_name), item.original_dir, item.original_name);
}

OperationResult TrashApi::restore_to(const TrashItem& item, const RestoreTarget& target) {
    OperationResult out;
    if (!actionable(item) || !is_safe_segment(item.trash_name)) {
        out.error = error_from_key("trash_not_actionable");
        return out;
    }
    if (!is_valid_remote_path(target.folder)) {
        out.error = error_from_key("restore_parent_missing");
        return out;
    }
    if (!check_new_name(target.name, ServerCaps()).ok) {
        out.error = error_from_key("name_forbidden");
        return out;
    }
    const std::string folder = normalize_remote_path(target.folder);
    return move_item(item, 5, files_url(session_, user_id_, join_remote(folder, target.name), item.kind == Kind::Folder), folder, target.name);
}

namespace {

bool is_digits(const std::string& s, size_t max) {
    if (s.empty() || s.size() > max) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

}  // namespace

Result<VersionList> TrashApi::list_versions(const RemoteEntry& file) {
    Result<VersionList> out;
    if (file.kind == Kind::Folder || file.availability != Availability::Ok) {   // a folder has no versions
        out.error = error_from_key("trash_not_actionable");
        return out;
    }
    if (!file.fileid || *file.fileid <= 0) {
        out.error = error_from_key("refresh_first");
        return out;
    }
    const auto stamp = fetch_entry(session_, user_id_, file.path);
    if (!stamp.ok()) {
        out.error = stamp.error;
        return out;
    }
    if (!stamp.value) {
        out.error = error_from_key("file_gone");
        return out;
    }
    if (stamp.value->kind == Kind::Folder) {
        out.error = error_from_key("trash_not_actionable");
        return out;
    }
    Headers headers = dav::dav_headers("1");
    headers.push_back({"User-Agent", std::string("PBCloud/") + version()});
    const Response r = session_.send("PROPFIND", versions_base() + "versions/" + std::to_string(*file.fileid) + "/", versions_request_body(), headers,
                                     static_cast<long long>(kMaxVersionBody), kTrashTotalTimeoutS);
    out.value.code = code_of(6, r.status);
    log::event("versions.list", {log::status(r.status)});
    const auto fail = [&](Error e) {
        log::event("trash.fail", {log::status(r.status), log::count("op", 6)});
        out.error = std::move(e);
        return out;
    };
    if (r.body_too_large) return fail(error_from_key("listing_too_large"));
    if (r.status == 404) {   // the file is there (the probe said so): the server has no versions of it
        out.value.file_etag = stamp.value->etag;
        out.value.file_id = *file.fileid;
        out.value.code.clear();
        return out;
    }
    if (r.status < 200 || r.status >= 300) return fail(r.status == 0 ? session_.status_error(r) : error_from_key(refusal_for(6, r.status).key));
    auto parsed = parse_versions(r, now());
    if (!parsed.ok()) return fail(parsed.error);
    out.value = std::move(parsed.value);
    out.value.file_etag = stamp.value->etag;
    out.value.file_id = *file.fileid;
    log::event("versions.list", {log::count("rows", static_cast<long>(out.value.rows.size()))});
    return out;
}

OperationResult TrashApi::restore_version(const RemoteEntry& file, const VersionList& shown, const VersionRow& row, VersionList* after) {
    OperationResult out;
    if (file.kind == Kind::Folder || file.availability != Availability::Ok || !is_digits(row.version_id, 20)) {
        out.error = error_from_key("trash_not_actionable");
        return out;
    }
    if (!file.fileid || *file.fileid <= 0) {
        out.error = error_from_key("refresh_first");
        return out;
    }
    // the stale check: the file must still be the one the list was read for
    const auto probe = fetch_entry(session_, user_id_, file.path);
    if (!probe.ok()) {
        out.error = probe.error;
        return out;
    }
    if (!probe.value) {
        out.error = error_from_key("file_gone");
        return out;
    }
    if (shown.file_etag.empty() || probe.value->etag != shown.file_etag) {
        out.error = error_from_key("version_changed_meanwhile");
        return out;
    }
    Headers headers = {{"Destination", versions_base() + "restore/target"}, {"Overwrite", "F"}, {"User-Agent", std::string("PBCloud/") + version()}};
    const Response r = session_.send("MOVE", versions_base() + "versions/" + std::to_string(*file.fileid) + "/" + row.version_id, std::string(), headers, 64 << 10, kTrashTotalTimeoutS);
    out.code = code_of(7, r.status);
    log::event("versions.restore", {log::status(r.status)});
    const Error e = interpret_trash_answer(r, 7);
    if (e.key == "outcome_unknown") {
        out.error = e;
        out.outcome_unknown = true;
        return out;
    }
    if (!e.ok()) {
        log::event("trash.fail", {log::status(r.status), log::count("op", 7)});
        out.error = e;
        return out;
    }
    // the result: the list again; the content that was current must be among the versions now (the server's rule, unverified)
    const auto again = list_versions(file);
    if (again.ok()) {
        if (probe.value->mtime) {
            const std::string previous = std::to_string(*probe.value->mtime);
            bool kept = false;
            for (const VersionRow& v : again.value.rows) kept = kept || v.version_id == previous;
            out.previous_not_kept = !kept;
        }
        if (after) *after = again.value;
    }
    return out;
}

Result<TrashList> TrashApi::list_trash() {
    Result<TrashList> out;
    Headers headers = dav::dav_headers("1");
    headers.push_back({"User-Agent", std::string("PBCloud/") + version()});
    const Response r = session_.send("PROPFIND", trash_base() + "trash/", trash_request_body(), headers, static_cast<long long>(kMaxTrashBody), kTrashTotalTimeoutS);
    out.value.code = code_of(2, r.status);
    log::event("trash.list", {log::status(r.status)});
    const auto fail = [&](Error e) {
        log::event("trash.fail", {log::status(r.status), log::count("op", 2)});
        out.error = std::move(e);
        return out;
    };
    if (r.body_too_large) return fail(error_from_key("listing_too_large"));
    if (r.status < 200 || r.status >= 300) return fail(r.status == 404 ? error_from_key("protocol_error") : session_.status_error(r));
    auto parsed = parse_trash(r, user_id_, now());
    if (!parsed.ok()) return fail(parsed.error);
    out.value = std::move(parsed.value);
    out.value.code.clear();
    log::event("trash.list", {log::count("items", out.value.total_seen)});
    return out;
}

Result<TrashAvailability> TrashApi::fetch_availability() {
    Result<TrashAvailability> out;
    const Response r = session_.send("GET", session_.base_url() + "/ocs/v1.php/cloud/capabilities?format=json", std::string(), ocs_headers(), 1 << 20, kTrashTotalTimeoutS);
    out.value.code = code_of(1, r.status);
    log::event("trash.open", {log::status(r.status)});
    if (r.status < 200 || r.status >= 300 || r.body_too_large) {
        log::event("trash.fail", {log::status(r.status), log::count("op", 1)});
        out.error = r.body_too_large ? error_from_key("protocol_error") : session_.status_error(r);
        return out;
    }
    const auto doc = parse_json(r.body);
    const JsonValue* caps = doc.ok() ? &doc.value : nullptr;
    for (const char* key : {"ocs", "data", "capabilities"}) caps = caps ? caps->get(key) : nullptr;
    if (!caps) return out;   // an answer that says nothing: both Unknown, the code stays
    const std::string code = out.value.code;
    out.value = availability_from_capabilities(*caps);
    if (out.value.trash == FeatureState::Unknown || out.value.versions == FeatureState::Unknown) out.value.code = code;
    return out;
}

}  // namespace pbc
