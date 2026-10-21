#include "pbc/update.h"

#include <deque>
#include <map>
#include <set>

#include "pbc/auth_guard.h"
#include "pbc/credentials_file.h"
#include "pbc/entry_state.h"
#include "pbc/local_files.h"
#include "pbc/log.h"
#include "pbc/nc_listing.h"
#include "pbc/remote_path.h"
#include "pbc/secure_wipe.h"

namespace pbc {

namespace {

// A problem that makes every further request pointless: the batch stops. Everything else belongs to one file or one folder.
bool general_problem(const std::string& key) {
    static const char* const keys[] = {"not_enough_space", "storage_readonly", "storage_full", "write_failed", "auth_failed", "maintenance", "rate_limited",
                                       "server_unreachable", "no_network", "timeout", "connection_lost", "cert_untrusted", "cert_expired", "cert_host_mismatch",
                                       "redirect_other_host", "tls_failed", "scheme_not_allowed", "server_error"};
    for (const char* k : keys)
        if (key == k) return true;
    return false;
}

bool under(const std::string& path, const std::string& folder) {
    const std::string prefix = folder == "/" ? std::string("/") : folder + "/";
    return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0;
}

std::string json(const UpdateReport& r) {
    return "{\"folders\":" + std::to_string(r.folders) + ",\"files\":" + std::to_string(r.files) + ",\"updated\":" + std::to_string(r.updated) +
           ",\"added\":" + std::to_string(r.added) + ",\"skipped\":" + std::to_string(r.skipped) + ",\"decision\":" + std::to_string(r.need_decision) +
           ",\"removed\":" + std::to_string(r.removed) + ",\"failed\":" + std::to_string(r.failed) +
           ",\"unchecked_folders\":" + std::to_string(r.unchecked_folders) + "}";
}

}  // namespace

UpdatePlan Updater::plan(Cancel& cancel) {
    UpdatePlan plan;
    plan.started_at = clock_->now();
    UpdateReport& rep = plan.report;
    const auto marks = store_->marks();
    if (marks.empty()) return plan;

    const auto account = store_->account();
    AuthGuard guard(*store_);
    if (!account || guard.blocked()) {
        rep.stopped_by_key = "auth_failed";
        return plan;
    }
    auto creds = load_credentials(store_->directory());
    if (!creds.ok() || creds.value.password.empty()) {
        rep.stopped_by_key = "auth_failed";
        return plan;
    }
    DavSession session(*http_, account->base_url, account->login_name, creds.value.password);
    secure_wipe(creds.value.password);
    const std::string root = dl_->root();

    struct Item { std::string path; int depth; };
    std::deque<Item> queue;
    for (const MarkRow& m : marks) queue.push_back({m.kind == Kind::Folder ? m.remote_path : parent_of(m.remote_path), 0});

    std::set<std::string> listed;                              // folders that were listed
    std::set<std::string> seen_files;
    std::map<std::string, std::set<std::string>> children;     // the names found in each listed folder
    const auto pairs = store_->local_path_pairs();

    while (!queue.empty()) {
        if (cancel.requested()) {
            rep.stopped_by_key = "cancelled";
            return plan;
        }
        const Item item = queue.front();
        queue.pop_front();
        if (listed.count(item.path)) continue;
        if (item.depth > kUpdateMaxDepth || rep.folders >= kUpdateMaxFolders) {
            ++rep.unchecked_folders;
            continue;
        }
        listed.insert(item.path);   // also when it fails: it is not tried twice
        auto fetched = fetch_listing(session, account->user_id, item.path);
        guard.record(fetched.error);
        if (!fetched.ok()) {
            const std::string& key = fetched.error.key;
            if (key == "not_found") {   // a kept folder that is gone on the server: its copies are flagged, never deleted
                for (const auto& pr : pairs) {
                    if (!under(pr.first, item.path) || !store_->in_scope(pr.first)) continue;
                    const auto lf = store_->local_file(pr.first);
                    if (lf && !lf->server_missing) {
                        store_->set_server_missing(pr.first, true);
                        ++rep.removed;
                        rep.removed_paths.push_back(pr.first);
                    }
                }
                continue;
            }
            if (general_problem(key)) {
                rep.stopped_by_key = key;
                return plan;
            }
            ++rep.failed;   // forbidden, too large, unusable answer: this folder only
            rep.failed_paths.push_back(item.path);
            continue;
        }
        ++rep.folders;
        const Listing& listing = fetched.value;
        if (store_->folder(item.path)) store_->replace_listing(item.path, listing, clock_->now());   // a folder the user opened stays current
        auto& names = children[item.path];
        for (const RemoteEntry& e : listing.entries) names.insert(e.name);

        for (const RemoteEntry& e : listing.entries) {
            if (!store_->in_scope(e.path)) continue;
            if (e.kind == Kind::Folder) {
                if (e.availability == Availability::Ok) queue.push_back({e.path, item.depth + 1});
                continue;
            }
            if (!seen_files.insert(e.path).second) continue;
            ++rep.files;
            if (e.availability != Availability::Ok) {
                ++rep.skipped;
                continue;
            }
            auto local = store_->local_file(e.path);
            if (local && local->server_missing) {   // it is back
                store_->set_server_missing(e.path, false);
                local->server_missing = false;
            }
            const FileStat st = local ? stat_file(root + "/" + local->local_path) : FileStat();
            switch (compute_state(&e, local ? &*local : nullptr, st)) {
                case EntryState::NotDownloaded:
                case EntryState::MissingLocally:
                    plan.downloads.push_back(e);
                    plan.is_update.push_back(false);
                    plan.bytes += e.size.value_or(0);
                    break;
                case EntryState::Outdated:
                    plan.downloads.push_back(e);
                    plan.is_update.push_back(true);
                    plan.bytes += e.size.value_or(0);
                    break;
                case EntryState::Conflict:
                    ++rep.need_decision;
                    rep.decision_paths.push_back(e.path);
                    break;
                default:
                    ++rep.skipped;
                    break;
            }
        }
    }

    // copies in scope that the listings no longer show (the nearest listed folder above them does not contain them)
    for (const auto& pr : pairs) {
        if (!store_->in_scope(pr.first)) continue;
        std::string parent = "/";
        bool vanished = false;
        size_t pos = 1;
        while (pos < pr.first.size()) {
            size_t next = pr.first.find('/', pos);
            if (next == std::string::npos) next = pr.first.size();
            const std::string name = pr.first.substr(pos, next - pos);
            const auto it = children.find(parent);
            if (it != children.end() && !it->second.count(name)) {   // a level that was not listed tells nothing: go on below it
                vanished = true;
                break;
            }
            parent = parent == "/" ? "/" + name : parent + "/" + name;
            pos = next + 1;
        }
        if (!vanished) continue;
        const auto lf = store_->local_file(pr.first);
        if (lf && !lf->server_missing) {
            store_->set_server_missing(pr.first, true);
            ++rep.removed;
            rep.removed_paths.push_back(pr.first);
        }
    }
    return plan;
}

UpdateReport Updater::execute(UpdatePlan& plan, Cancel& cancel, const std::function<void(const UpdateReport&)>& on_progress) {
    UpdateReport& rep = plan.report;
    if (rep.stopped_by_key.empty()) {
        for (size_t i = 0; i < plan.downloads.size(); ++i) {
            if (cancel.requested()) {
                rep.stopped_by_key = "cancelled";
                break;
            }
            const RemoteEntry& e = plan.downloads[i];
            DownloadRequest req;
            req.remote_path = e.path;
            req.size = e.size;
            req.etag = e.etag;
            req.checksum = e.checksum;
            req.remote_mtime = e.mtime;
            const DownloadOutcome out = dl_->run(req, cancel, [](const JobProgress&) {});
            switch (out.state) {
                case JobState::Done:
                    if (plan.is_update[i]) ++rep.updated;
                    else ++rep.added;
                    break;
                case JobState::Blocked:
                    ++rep.need_decision;
                    rep.decision_paths.push_back(e.path);
                    break;
                case JobState::Cancelled:
                    rep.stopped_by_key = "cancelled";
                    break;
                case JobState::Paused:
                    rep.stopped_by_key = out.error_key.empty() ? "connection_lost" : out.error_key;
                    break;
                default:   // Failed
                    ++rep.failed;
                    rep.failed_paths.push_back(e.path);
                    if (general_problem(out.error_key)) rep.stopped_by_key = out.error_key;
                    break;
            }
            if (on_progress) on_progress(rep);
            if (!rep.stopped_by_key.empty()) break;
        }
    }
    // the result, for the home page
    SyncState st = store_->sync_state();
    st.last_started = plan.started_at;
    st.last_finished = clock_->now();
    const bool stopped = !rep.stopped_by_key.empty();
    const bool did_something = rep.added + rep.updated > 0;
    st.outcome = stopped ? (did_something ? "partial" : (rep.stopped_by_key == "cancelled" ? "partial" : "failed")) : (rep.failed > 0 ? "partial" : "success");
    if (st.outcome == "success") st.last_success = st.last_finished;
    st.problem_key = stopped ? rep.stopped_by_key : (rep.failed > 0 ? "failed_files" : "");
    st.problem_area = st.problem_key.empty() ? "" : "update";
    st.counts_json = json(rep);
    store_->write_sync_state(st);
    log::event("update.run", {log::count("files", rep.files), log::count("added", rep.added), log::count("updated", rep.updated), log::count("failed", rep.failed), log::text("stopped", rep.stopped_by_key)});
    return rep;
}

UpdateReport Updater::run(Cancel& cancel, const std::function<void(const UpdateReport&)>& on_progress) {
    UpdatePlan p = plan(cancel);
    return execute(p, cancel, on_progress);
}

}  // namespace pbc
