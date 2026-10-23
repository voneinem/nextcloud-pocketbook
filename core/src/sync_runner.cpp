#include "pbc/sync_runner.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>

#include "pbc/auth_guard.h"
#include "pbc/civil.h"
#include "pbc/credentials_file.h"
#include "pbc/dav_common.h"
#include "pbc/error_keys.h"
#include "pbc/hash.h"
#include "pbc/local_files.h"
#include "pbc/nc_listing.h"
#include "pbc/queue_actions.h"
#include "pbc/log.h"
#include "pbc/remote_path.h"
#include "pbc/secure_wipe.h"
#include "pbc/sync_planner.h"
#include "pbc/sync_rules.h"
#include "pbc/sync_scan.h"
#include "pbc/upload_source.h"

namespace pbc {

namespace {

// A problem that makes every further request pointless: the run or the scan ends there.
bool general_problem(const std::string& key) {
    static const char* const keys[] = {"auth_failed", "maintenance", "rate_limited", "server_unreachable", "no_network", "no_connection", "connection_lost",
                                       "cert_untrusted", "cert_expired", "cert_host_mismatch", "redirect_other_host", "tls_failed", "scheme_not_allowed",
                                       "storage_full", "storage_readonly", "write_failed"};
    for (const char* k : keys)
        if (key == k) return true;
    return false;
}

// What the pair shows while the reader cannot reach the server: not a fault of the pair.
bool offline_key(const std::string& key) {
    return key == "no_network" || key == "server_unreachable" || key == "connection_lost" || key == "no_connection" || key == "timeout";
}

std::string date_of(int64_t now) {
    int y = 1970, m = 1, d = 1;
    civil::civil_from_days(civil::floor_div(now, 86400), &y, &m, &d);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", y, m, d);
    return buf;
}

std::string json_of(const SyncResult& r) {
    std::string j = "{\"downloaded\":" + std::to_string(r.downloaded) + ",\"uploaded\":" + std::to_string(r.uploaded) + ",\"skipped\":" + std::to_string(r.skipped) +
                    ",\"kept_both\":" + std::to_string(r.kept_both) + ",\"proposals_open\":" + std::to_string(r.proposals_open) + ",\"deleted\":" + std::to_string(r.deleted) +
                    ",\"failed\":" + std::to_string(r.failed) +
                    ",\"seconds\":" + std::to_string(r.seconds) + ",\"stopped_by\":\"" + r.stopped_by_key + "\",\"failures\":[";
    for (size_t i = 0; i < r.failure_keys.size(); ++i) j += (i ? "," : "") + std::string("{\"key\":\"") + r.failure_keys[i].first + "\",\"count\":" + std::to_string(r.failure_keys[i].second) + "}";
    return j + "]}";
}

void count_failure(SyncResult* r, const std::string& key) {
    ++r->failed;
    for (auto& f : r->failure_keys)
        if (f.first == key) {
            ++f.second;
            return;
        }
    r->failure_keys.push_back({key, 1});
}

// Holds the lock of a pair (FR-034) and gives it back, whatever way a function is left.
struct PairLock {
    Store& store;
    int64_t id;
    std::string end_state = "idle";
    std::string problem;
    bool held = false;
    ~PairLock() {
        if (held) store.sync_pair_unlock(id, end_state, problem);
    }
};

// The strongest checksum of a listing's text as the record keeps it ("SHA256:<hex>")
std::string strongest_text(const std::string& oc_checksums) {
    const auto sum = strongest(parse_checksums(oc_checksums));
    if (!sum) return std::string();
    return std::string(sum->algo == HashAlgo::Sha256 ? "SHA256:" : sum->algo == HashAlgo::Sha1 ? "SHA1:" : "MD5:") + sum->hex;
}

// The two versions of a name were kept apart (research R-9): the original's record remembers the versions that were seen and is marked diverged, so that no
// later plan transfers it again. Written after the copies exist; a run that dies before this leaves copies with records, and the next plan only marks it.
Error mark_diverged(Store& store, const SyncPair& pair, const PlanEntry& e, int64_t now) {
    SyncRecord r;
    if (const auto old = store.sync_record(pair.id, e.rel)) r = *old;
    r.pair_id = pair.id;
    r.rel = e.rel;
    if (r.local_rel.empty()) r.local_rel = e.local_rel;
    r.kind = Kind::File;
    if (r.origin.empty()) r.origin = "identical";
    r.server_etag = e.plan_etag;
    r.server_size = e.size;
    r.server_checksum = strongest_text(e.plan_checksum);
    r.local_size = e.plan_lsize;
    r.local_mtime = e.plan_lmtime;
    r.local_sha256.clear();
    r.diverged = true;
    r.synced_at = now;
    return store.sync_record_put(r);
}

}  // namespace

bool pair_matches_account(const Store& store, const SyncPair& pair) {
    const auto account = store.account();
    return account && account->base_url == pair.server && account->user_id == pair.user_id && pair.problem_key != "account_changed";
}

SyncRunner::SyncRunner(Store& store, HttpTransport& http, Downloader& downloader, StorageInfo& storage, const Clock& clock, SourceRules rules, RunnerOptions options)
    : store_(&store), http_(&http), dl_(&downloader), storage_(&storage), clock_(&clock), rules_(std::move(rules)), options_(std::move(options)) {}

// ------------------------------------------------------------------------------------------------------------------------------ compare

PlannerInput SyncRunner::input_for(const SyncPair& pair, const AccountRow& account, Scan server, Scan reader, Cancel& cancel) const {
    PlannerInput in;
    in.pair = pair;
    in.records = store_->sync_records(pair.id);
    in.open_rels = store_->queue_sync_open_rels(pair.id);
    {   // the names the user's own queued changes name below the pair's server folder (rule 1 of the table)
        const std::string base = normalize_remote_path(pair.remote_folder);
        const std::string prefix = remote_prefix(base);
        auto add = [&](const std::string& path) {
            if (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0) in.open_rels.insert(path.substr(prefix.size()));
        };
        for (const QueueItem& item : store_->queue_items_below(base)) {
            add(item.src_remote);
            if (!item.dst_name.empty()) add(join_remote(item.dst_parent, item.dst_name));
        }
    }
    in.caps = store_->server_caps().value_or(ServerCaps());
    in.reader_date = date_of(clock_->now());
    in.free_bytes = storage_->free_bytes(pair.local_folder);
    in.volume_bytes = storage_->total_bytes(pair.local_folder).value_or(0);
    if (account.quota_total && *account.quota_total > 0 && account.quota_used) in.quota_free = std::max<int64_t>(0, *account.quota_total - *account.quota_used);
    for (const ScanEntry& e : reader.entries)
        if (e.kind == Kind::File && e.skip_reason.empty()) {
            ++in.held_files;
            in.held_bytes += e.size;
        }
    const std::string folder = pair.local_folder;
    in.identical = [folder, &cancel](const ScanEntry& s, const ScanEntry& l) {
        const std::string abs = join_path_checked(folder, l.rel);
        return !abs.empty() && identical_by_checksum(abs, s, cancel);
    };
    in.reader_changed = [&cancel](const std::string& abs, const SyncRecord& r) { return pbc::reader_changed(abs, r, &cancel); };
    in.server = std::move(server);
    in.reader = std::move(reader);
    return in;
}

Result<SyncPlan> SyncRunner::preview_from_cache(int64_t pair_id, Cancel& cancel) const {
    Result<SyncPlan> out;
    auto fail = [&](const std::string& key) {
        out.error = error_from_key(key);
        return out;
    };
    const auto pair = store_->sync_pair(pair_id);
    if (!pair) return fail("not_found");
    if (!pair_matches_account(*store_, *pair)) return fail("account_changed");
    const auto account = store_->account();
    Scan reader = scan_reader(*pair, rules_, cancel);
    if (reader.stopped_by_key == "cancelled") return fail("cancelled");
    if (!reader.stopped_by_key.empty()) return fail(reader.stopped_by_key);

    // the stored listings of the folders the user has opened: only what is already in the store is looked at
    Scan server;
    server.incomplete = true;   // whatever was listed once is not current: what is absent from it is unknown
    int64_t oldest = 0;
    struct Item {
        std::string rel;
        int depth;
    };
    std::deque<Item> queue;
    queue.push_back({std::string(), 0});
    int listed = 0, files = 0;
    const std::string base = normalize_remote_path(pair->remote_folder);
    while (!queue.empty()) {
        const Item item = queue.front();
        queue.pop_front();
        if (item.depth > kSyncMaxDepth || listed >= kSyncMaxFolders) {
            ++server.folders_not_listed;
            continue;
        }
        const std::string path = item.rel.empty() ? base : join_remote(base, item.rel);
        const auto folder = store_->folder(path);
        if (!folder || !folder->fetched_at) {   // never listed: nothing is known about what is in it
            ++server.folders_not_listed;
            continue;
        }
        ++listed;
        oldest = oldest == 0 ? *folder->fetched_at : std::min(oldest, *folder->fetched_at);
        for (int offset = 0;; offset += 500) {
            const EntryPage page = store_->entries_page(path, SortKey::Name, true, offset, 500);
            for (const RemoteEntry& e : page.rows) {
                const std::string rel = rel_join(item.rel, e.name);
                if (rel.empty()) continue;
                if (e.kind == Kind::File && files >= kSyncMaxFiles) continue;
                ScanEntry se;
                se.rel = rel;
                se.kind = e.kind;
                se.size = e.kind == Kind::File ? e.size.value_or(0) : 0;
                se.etag = e.etag;
                se.checksum = e.checksum;
                se.mtime = e.mtime;
                se.availability = e.availability;
                if (e.availability != Availability::Ok) se.skip_reason = "unavailable";
                if (e.kind == Kind::File) ++files;
                else if (e.availability == Availability::Ok) queue.push_back({rel, item.depth + 1});
                server.entries.push_back(std::move(se));
            }
            if (static_cast<int>(page.rows.size()) < 500) break;
        }
    }
    std::sort(server.entries.begin(), server.entries.end(), [](const ScanEntry& a, const ScanEntry& b) { return a.rel < b.rel; });
    PlannerInput in = input_for(*pair, *account, std::move(server), std::move(reader), cancel);
    SyncPlan plan = pbc::plan(in);
    plan.state = "preview";   // not "ready": it cannot be approved
    plan.incomplete = true;
    plan.created_at = oldest != 0 ? oldest : clock_->now();
    plan.trigger = "tap";
    out.value = std::move(plan);
    return out;
}


Result<int64_t> SyncRunner::compare(int64_t pair_id, const std::string& trigger, Cancel& cancel) {
    Result<int64_t> out;
    auto fail = [&](const std::string& key) {
        out.error = error_from_key(key);
        return out;
    };
    const auto pair = store_->sync_pair(pair_id);
    if (!pair) return fail("not_found");
    if (!pair_matches_account(*store_, *pair)) return fail("account_changed");   // FR-050: before any lock, request or file access
    if (trigger != "tap" && trigger != "startup" && trigger != "auto") return fail("protocol_error");
    if (const Error e = store_->sync_pair_lock(pair_id, "comparing"); !e.ok()) {
        out.error = e;
        return out;
    }
    PairLock lock{*store_, pair_id};
    lock.held = true;
    log::event("sync.compare.start", {log::count("pair", static_cast<long>(pair_id))});

    const auto account = store_->account();
    AuthGuard guard(*store_);
    if (guard.blocked()) {
        lock.problem = "auth_failed";
        return fail("auth_failed");
    }
    auto creds = load_credentials(store_->directory());
    if (!creds.ok() || creds.value.password.empty()) {
        lock.problem = "auth_failed";
        return fail("auth_failed");
    }
    DavSession session(*http_, account->base_url, account->login_name, creds.value.password);
    secure_wipe(creds.value.password);

    // the reader first: it is local, and a folder that is not there needs no request at all
    Scan reader = scan_reader(*pair, rules_, cancel);
    if (reader.stopped_by_key == "cancelled") return fail("cancelled");
    if (!reader.stopped_by_key.empty()) {
        lock.problem = reader.stopped_by_key;
        return fail(reader.stopped_by_key);
    }
    Scan server = scan_server(session, account->user_id, *pair, cancel);
    if (server.stopped_by_key == "auth_failed") guard.record(make_error(ErrorClass::Auth, "auth_failed"));
    else if (server.stopped_by_key.empty() || server.stopped_by_key == "server_folder_missing") guard.record(Error());
    if (server.stopped_by_key == "cancelled") return fail("cancelled");
    if (!server.stopped_by_key.empty()) {
        if (server.stopped_by_key == "server_folder_missing" || server.stopped_by_key == "auth_failed") lock.problem = server.stopped_by_key;
        else if (offline_key(server.stopped_by_key)) lock.problem = "no_network";
        return fail(server.stopped_by_key);
    }

    // what the planner needs besides the scans
    int64_t held_files = 0, held_bytes = 0;
    for (const ScanEntry& e : reader.entries)
        if (e.kind == Kind::File && e.skip_reason.empty()) {
            ++held_files;
            held_bytes += e.size;
        }
    PlannerInput in = input_for(*pair, *account, std::move(server), std::move(reader), cancel);
    if (cancel.requested()) return fail("cancelled");

    SyncPlan plan = pbc::plan(in);
    plan.created_at = clock_->now();
    plan.trigger = trigger;
    const Result<int64_t> stored = store_->sync_plan_store(plan);
    if (!stored.ok()) {
        out.error = stored.error;
        return out;
    }
    if (!plan.over_limit.empty()) log::event("sync.limit", {log::text("which", plan.over_limit)});
    if (options_.hook) options_.hook("plan_written");
    store_->sync_pair_compared(pair_id, clock_->now(), held_files, held_bytes);

    // something to approve, or nothing to do
    bool needs_run = plan.truncated || plan.unapprovable > 0;
    for (const PlanEntry& e : plan.entries)
        if (e.action != PlanAction::Skip || e.reason == "identical" || e.reason == "gone_both") needs_run = true;
    if (needs_run) {
        lock.end_state = "plan_ready";
    } else {
        store_->sync_set_result(pair_id, "in_sync", "{}", clock_->now());
    }
    log::event("sync.compare.end", {log::count("pair", static_cast<long>(pair_id)), log::count("plan", static_cast<long>(stored.value)),
                                    log::count("downloads", plan.totals.n_download), log::count("uploads", plan.totals.n_upload),
                                    log::count("skipped", plan.totals.n_skip), log::count("in_sync", plan.totals.n_in_sync)});
    out.value = stored.value;
    return out;
}

// ------------------------------------------------------------------------------------------------------------------------------ run

// The upload entries of an approved plan (R-8): every source is looked at again against the plan, the rest becomes ONE group of queue items, and the sender
// sends that group and nothing else. An item that does not end done stays in the queue for the user (Retry, a choice, Check the server); here it only
// counts as failed with its reason, and the pair's record is not touched (the confirmation of an item writes it, in the same transaction as its done state).
void SyncRunner::run_uploads(const SyncPair& pair, int64_t plan_id, const std::vector<PlanEntry>& entries, const std::set<int>& reader_copy_done, Cancel& cancel, SyncResult* res) {
    // the entries that send something: uploads, and the reader's version of a conflicting name as a copy on the server
    std::vector<const PlanEntry*> todo;
    for (const PlanEntry& e : entries) {
        if (e.state != "planned") continue;
        if (e.action == PlanAction::Upload) todo.push_back(&e);
        if (e.action == PlanAction::KeepBoth && !e.conflict_name_server.empty()) todo.push_back(&e);
    }
    if (todo.empty()) return;
    auto finish_entry = [&](const PlanEntry& e, const char* state, const std::string& key) { store_->sync_entry_result(plan_id, e.seq, state, key); };
    auto progress = [&]() {
        if (options_.on_progress) options_.on_progress(*res);
    };
    auto fail_all = [&](const std::vector<const PlanEntry*>& list, const std::string& key) {
        for (const PlanEntry* e : list) {
            finish_entry(*e, "failed", key);
            count_failure(res, key);
        }
        progress();
    };

    std::vector<SyncUploadSpec> specs;
    std::vector<const PlanEntry*> sent;   // parallel to `specs`
    for (const PlanEntry* e : todo) {
        if (e->removed) {   // taken out of the plan by the user: skipped, and it appears again in the next plan
            if (e->action == PlanAction::Upload) {   // (a removed keep-both entry was settled before the downloads of its copies)
                finish_entry(*e, "skipped", "by_user");
                ++res->skipped;
            }
            continue;
        }
        const bool copy = e->action == PlanAction::KeepBoth;
        if (copy && !e->conflict_name.empty() && !reader_copy_done.count(e->seq)) continue;   // its reader copy failed: the server copy waits for the next plan
        const std::string abs = join_path_checked(pair.local_folder, e->local_rel);
        const auto identity = abs.empty() ? Result<SourceIdentity>() : inspect_source(abs, rules_);
        std::string key;
        if (abs.empty()) key = "path_unsafe";
        else if (!identity.ok()) key = identity.error.key == "source_not_allowed" ? std::string("path_unsafe") : identity.error.key;
        else if (identity.value.size != e->plan_lsize.value_or(-1) || identity.value.mtime != e->plan_lmtime.value_or(-1)) key = "changed_during_sync";
        if (!key.empty()) {   // the file is not the one the plan showed: left for the next plan, nothing is sent
            finish_entry(*e, "skipped", key);
            ++res->skipped;
            continue;
        }
        SyncUploadSpec sp;
        sp.local_abs = abs;
        const std::string& target = copy ? e->conflict_name_server : e->rel;
        sp.rel = target;
        const size_t slash = target.rfind('/');
        sp.dst_parent = slash == std::string::npos ? pair.remote_folder : join_remote(pair.remote_folder, target.substr(0, slash));
        sp.dst_name = target.substr(slash == std::string::npos ? 0 : slash + 1);
        if (copy) sp.role = "conflict_copy";
        else if (!e->plan_etag.empty()) sp.replace_etag = e->plan_etag;
        specs.push_back(std::move(sp));
        sent.push_back(e);
    }
    progress();
    if (specs.empty()) return;
    if (!sender_) {
        fail_all(sent, "protocol_error");
        return;
    }
    if (cancel.requested()) {
        res->stopped_by_key = "cancelled";
        return;
    }
    // The folders the uploads go into that the server has: asked now (the comparison read the listings transiently), top down, so that a folder below a
    // missing one is not asked about. What is not there gets a folder item in the group.
    std::set<std::string> existing;
    {
        const auto account = store_->account();
        auto creds = load_credentials(store_->directory());
        if (account && creds.ok() && !creds.value.password.empty()) {
            DavSession session(*http_, account->base_url, account->login_name, creds.value.password);
            secure_wipe(creds.value.password);
            std::set<std::string> wanted, missing;
            for (const SyncUploadSpec& sp : specs) {
                for (std::string folder = sp.dst_parent; folder.size() > pair.remote_folder.size(); folder = parent_of(folder)) wanted.insert(folder);
            }
            for (const std::string& folder : wanted) {   // shallow before deep: a std::set orders a parent before what it holds
                if (cancel.requested()) break;
                bool below_missing = false;
                for (const std::string& m : missing) {
                    const std::string prefix = remote_prefix(m);
                    if (folder.size() > prefix.size() && folder.compare(0, prefix.size(), prefix) == 0) below_missing = true;
                }
                if (below_missing) continue;
                const auto seen = fetch_entry(session, account->user_id, folder);
                if (seen.ok() && seen.value && seen.value->kind == Kind::Folder) existing.insert(folder);
                else if (seen.ok()) missing.insert(folder);
            }
        }
    }
    QueueActions actions(*store_, rules_);
    const Result<SyncGroup> group = actions.add_sync_uploads(pair.id, pair.remote_folder, specs, existing);
    if (!group.ok()) {
        fail_all(sent, group.error.key);
        return;
    }
    for (const PickRefusal& r : group.value.refused) {   // a source that went between the look above and the queue
        for (size_t i = 0; i < sent.size(); ++i)
            if (specs[i].rel == r.path) {
                finish_entry(*sent[i], "skipped", r.key == "source_not_allowed" ? std::string("path_unsafe") : r.key);
                ++res->skipped;
            }
    }
    if (options_.hook) options_.hook("group_created");
    const SendReport report = sender_->run(cancel, 0, group.value.group_id);
    if (report.stopped_by_key == "cancelled") res->stopped_by_key = "cancelled";
    else if (!report.stopped_by_key.empty()) res->stopped_by_key = report.stopped_by_key;
    for (size_t i = 0; i < sent.size(); ++i) {
        const int64_t id = group.value.items[i];
        if (id == 0) continue;   // refused above
        const PlanEntry& entry = *sent[i];
        const auto item = store_->queue_item(id);
        if (!item) {
            finish_entry(entry, "failed", "protocol_error");
            count_failure(res, "protocol_error");
            continue;
        }
        switch (item->state) {
            case ItemState::Done:
                if (entry.action == PlanAction::KeepBoth) {   // both copies exist now: the original remembers that its two versions are apart
                    if (!mark_diverged(*store_, pair, entry, clock_->now()).ok()) {
                        finish_entry(entry, "failed", "write_failed");
                        count_failure(res, "write_failed");
                        break;
                    }
                    ++res->kept_both;
                } else {
                    ++res->uploaded;
                }
                finish_entry(entry, "done", "");
                log::event("sync.item.done", {log::count("plan", static_cast<long>(plan_id)), log::count("seq", entry.seq)});
                if (options_.hook) options_.hook("item_confirmed");
                break;
            case ItemState::Queued:
            case ItemState::Sending:
                if (res->stopped_by_key.empty()) {   // its folder could not be made, or it waits for something that failed: the Queue shows it
                    finish_entry(entry, "failed", item->reason.empty() ? std::string("target_gone") : item->reason);
                    count_failure(res, item->reason.empty() ? std::string("target_gone") : item->reason);
                }   // else the run stopped before it: the entry stays planned and the item stays queued
                break;
            default: {   // failed, conflict or unknown: the item stays for the user; the file is not synced until they decide
                const std::string key = item->reason.empty() ? std::string(item->state == ItemState::Unknown ? "no_answer" : "write_failed") : item->reason;
                finish_entry(entry, "failed", key);
                count_failure(res, key);
                log::event("sync.item.fail", {log::count("plan", static_cast<long>(plan_id)), log::count("seq", entry.seq), log::text("reason", key)});
                break;
            }
        }
    }
    progress();
}

Result<SyncResult> SyncRunner::run(int64_t plan_id, Cancel& cancel) {
    Result<SyncResult> out;
    auto fail = [&](const std::string& key) {
        out.error = error_from_key(key);
        return out;
    };
    const auto plan = store_->sync_plan(plan_id);
    if (!plan) return fail("not_found");
    const auto pair = store_->sync_pair(plan->pair_id);
    if (!pair) return fail("not_found");
    if (!pair_matches_account(*store_, *pair)) return fail("account_changed");
    if (pair->state == "comparing" || pair->state == "running") return fail("sync_busy");   // another trigger holds the pair: that is the answer, not the plan's state
    if (const Approval a = can_approve(*plan, clock_->now()); !a.ok) return fail(a.error_key);
    if (const Error e = store_->sync_pair_lock(pair->id, "running"); !e.ok()) {
        out.error = e;
        return out;
    }
    PairLock lock{*store_, pair->id};
    lock.held = true;
    if (const Error e = store_->sync_plan_state(plan_id, "approved"); !e.ok()) {
        out.error = e;
        return out;
    }
    if (options_.hook) options_.hook("approved");
    if (const Error e = store_->sync_plan_state(plan_id, "running"); !e.ok()) {
        out.error = e;
        return out;
    }
    const int64_t started = clock_->now();
    log::event("sync.run.start", {log::count("pair", static_cast<long>(pair->id)), log::count("plan", static_cast<long>(plan_id)),
                                  log::count("downloads", plan->totals.n_download)});

    SyncResult res;
    const std::vector<PlanEntry> entries = store_->sync_plan_page(plan_id, "all", 0, kSyncMaxPlanEntries).rows;
    for (const PlanEntry& e : entries) {
        if (e.action == PlanAction::Skip && e.reason != "identical" && e.reason != "gone_both" && !e.removed) ++res.skipped;
        if ((e.action == PlanAction::ProposeDeleteServer || e.action == PlanAction::ProposeDeleteReader) && e.choice == Choice::None) ++res.proposals_open;   // never touched by a run
    }
    auto progress = [&]() {
        if (options_.on_progress) options_.on_progress(res);
    };
    auto finish_entry = [&](const PlanEntry& e, const char* state, const std::string& key) { store_->sync_entry_result(plan_id, e.seq, state, key); };

    // 1. what needs no transfer: the identical files of a first sync are remembered, the records of files gone on both sides are dropped
    for (const PlanEntry& e : entries) {
        if (e.removed || e.state != "planned" || e.action != PlanAction::Skip) continue;
        if (e.reason == "identical") {
            SyncRecord r;
            r.pair_id = pair->id;
            r.rel = e.rel;
            r.local_rel = e.local_rel;
            r.kind = Kind::File;
            r.origin = "identical";
            r.server_etag = e.plan_etag;
            r.server_size = e.size;
            r.server_checksum = strongest_text(e.plan_checksum);
            r.local_size = e.plan_lsize;
            r.local_mtime = e.plan_lmtime;
            r.synced_at = clock_->now();
            if (store_->sync_record_put(r).ok()) finish_entry(e, "done", "");
            else finish_entry(e, "failed", "write_failed");
        } else if (e.reason == "gone_both") {
            store_->sync_record_drop(pair->id, e.rel);
            finish_entry(e, "done", "");
        } else if (e.reason == "kept_both" && !e.plan_etag.empty()) {   // the copies exist already: only the divergence is left to remember
            if (mark_diverged(*store_, *pair, e, clock_->now()).ok()) finish_entry(e, "done", "");
            else finish_entry(e, "failed", "write_failed");
        }
    }

    // What a download that did not end done means for its entry and for the run: 0 = the entry ended and the run goes on, 1 = the run stops here.
    auto settle_failure = [&](const PlanEntry& e, const DownloadOutcome& outcome) {
        if (outcome.state == JobState::Cancelled || cancel.requested()) {
            res.stopped_by_key = "cancelled";
            return 1;
        }
        std::string key = outcome.error_key;
        if (outcome.state == JobState::Paused) key = key.empty() ? "connection_lost" : key;
        if (outcome.state == JobState::Blocked) key = "changed_during_sync";   // the reader file is not the recorded copy any more
        if (key == "not_found") key = "changed_during_sync";                   // gone since the comparison
        if (key == "not_enough_space") key = "no_space";
        log::event("sync.item.fail", {log::count("plan", static_cast<long>(plan_id)), log::count("seq", e.seq), log::text("reason", key)});
        if (general_problem(key)) {   // the card is full or read-only, or the server cannot be reached: nothing after this could work either
            finish_entry(e, "failed", key);
            count_failure(&res, key);
            res.stopped_by_key = key;
            return 1;
        }
        static const char* const kPerFile[] = {"changed_during_sync", "no_space", "too_large_for_storage", "path_unsafe", "path_too_long", "name_forbidden", "name_exists"};
        bool per_file = false;
        for (const char* k : kPerFile)
            if (key == k) per_file = true;
        if (per_file) {
            finish_entry(e, "skipped", key);
            ++res.skipped;
        } else {
            finish_entry(e, "failed", key);
            count_failure(&res, key);
        }
        progress();
        return 0;
    };
    auto request_for = [&](const PlanEntry& e) {
        DownloadRequest req;
        req.remote_path = join_remote(pair->remote_folder, e.rel);
        req.size = e.size;
        req.etag = e.plan_etag;
        req.checksum = e.plan_checksum;
        req.policy = ConflictPolicy::Ask;
        req.pair_id = pair->id;
        req.target_root = pair->local_folder;
        req.strip_prefix = pair->remote_folder;
        req.strict_version = true;
        return req;
    };

    // 2. the downloads, in plan order, one at a time
    for (const PlanEntry& e : entries) {
        if (e.action != PlanAction::Download || e.state != "planned") continue;
        if (e.removed) {   // taken out of the plan by the user: skipped, and it appears again in the next plan
            finish_entry(e, "skipped", "by_user");
            ++res.skipped;
            continue;
        }
        if (cancel.requested()) {
            res.stopped_by_key = "cancelled";
            break;
        }
        {   // the reserve again, now: what the card holds may have changed since the plan, and an earlier download of this run took its share
            const auto free_now = storage_->free_bytes(pair->local_folder);
            if (free_now && *free_now - reserve_for(storage_->total_bytes(pair->local_folder).value_or(0)) < e.size.value_or(0)) {
                finish_entry(e, "skipped", "no_space");
                ++res.skipped;
                progress();
                continue;
            }
        }
        if (options_.hook) options_.hook("before_download");
        const DownloadOutcome outcome = dl_->run(request_for(e), cancel, [this](const JobProgress& p) {
            if (options_.on_file_progress) options_.on_file_progress(p);
        });
        if (outcome.state == JobState::Done) {
            if (options_.hook) options_.hook("downloaded");
            finish_entry(e, "done", "");
            ++res.downloaded;
            log::event("sync.item.done", {log::count("plan", static_cast<long>(plan_id)), log::count("seq", e.seq)});
            if (options_.hook) options_.hook("record_committed");
            progress();
            continue;
        }
        if (settle_failure(e, outcome) != 0) break;
    }

    // 2b. the server's version of a conflicting name, added beside the reader's file under the name the plan chose (research R-9). The copy and its record
    // are one transaction of the downloader; the original is marked only when every copy this entry needs exists.
    std::set<int> reader_copy_done;
    for (const PlanEntry& e : entries) {
        if (!res.stopped_by_key.empty()) break;
        if (e.action != PlanAction::KeepBoth || e.state != "planned") continue;
        if (e.removed) {
            finish_entry(e, "skipped", "by_user");
            ++res.skipped;
            continue;
        }
        if (e.conflict_name.empty()) continue;   // only the reader's version goes to the server: the uploads below
        if (cancel.requested()) {
            res.stopped_by_key = "cancelled";
            break;
        }
        if (options_.hook) options_.hook("before_download");
        DownloadRequest req = request_for(e);
        req.policy = ConflictPolicy::KeepBoth;
        const size_t slash = e.conflict_name.rfind('/');
        req.conflict_name = e.conflict_name.substr(slash == std::string::npos ? 0 : slash + 1);
        req.copy_of = e.rel;
        const DownloadOutcome outcome = dl_->run(req, cancel, [this](const JobProgress& p) {
            if (options_.on_file_progress) options_.on_file_progress(p);
        });
        if (outcome.state != JobState::Done) {
            if (settle_failure(e, outcome) != 0) break;
            continue;
        }
        if (options_.hook) options_.hook("copy_committed");
        reader_copy_done.insert(e.seq);
        if (e.conflict_name_server.empty()) {   // nothing goes to the server: the entry is complete
            if (mark_diverged(*store_, *pair, e, clock_->now()).ok()) {
                finish_entry(e, "done", "");
                ++res.kept_both;
            } else {
                finish_entry(e, "failed", "write_failed");
                count_failure(&res, "write_failed");
            }
            progress();
        }
    }

    // 3. the uploads: one group of the queue, sent alone
    if (res.stopped_by_key.empty()) run_uploads(*pair, plan_id, entries, reader_copy_done, cancel, &res);

    // 4. the result
    res.seconds = std::max<int64_t>(0, clock_->now() - started);
    const bool stopped = !res.stopped_by_key.empty();
    const char* outcome = stopped ? "interrupted" : (res.failed > 0 ? "partial" : "done");
    store_->sync_set_result(pair->id, outcome, json_of(res), clock_->now());
    store_->sync_plan_state(plan_id, stopped ? "interrupted" : "done");
    lock.end_state = stopped ? "interrupted" : "idle";
    if (options_.hook) options_.hook("result_written");
    log::event("sync.run.end", {log::count("pair", static_cast<long>(pair->id)), log::count("plan", static_cast<long>(plan_id)), log::count("downloaded", res.downloaded),
                                log::count("skipped", res.skipped), log::count("failed", res.failed), log::text("reason", res.stopped_by_key)});
    out.value = res;
    return out;
}

// ------------------------------------------------------------------------------------------------------------------------------ the choices for proposals

Result<SyncResult> SyncRunner::apply_choices(int64_t plan_id, Cancel& cancel) {
    Result<SyncResult> out;
    auto fail = [&](const std::string& key) {
        out.error = error_from_key(key);
        return out;
    };
    const auto plan = store_->sync_plan(plan_id);
    if (!plan) return fail("not_found");
    const auto pair = store_->sync_pair(plan->pair_id);
    if (!pair) return fail("not_found");
    if (!pair_matches_account(*store_, *pair)) return fail("account_changed");   // before any lock, request or file access
    if (pair->state == "comparing" || pair->state == "running") return fail("sync_busy");
    if (plan->state != "ready" && plan->state != "done") return fail("plan_not_approvable");
    if (clock_->now() - plan->created_at > kSyncPlanMaxAgeS) return fail("plan_stale");
    if (const Error e = store_->sync_pair_lock(pair->id, "running"); !e.ok()) {
        out.error = e;
        return out;
    }
    PairLock lock{*store_, pair->id};
    lock.held = true;
    lock.end_state = pair->state == "plan_ready" || pair->state == "interrupted" ? pair->state : "idle";
    const int64_t started = clock_->now();

    SyncResult res;
    const std::vector<PlanEntry> entries = store_->sync_plan_page(plan_id, "proposals", 0, kSyncMaxPlanEntries).rows;
    auto finish_entry = [&](const PlanEntry& e, const char* state, const std::string& key) { store_->sync_entry_result(plan_id, e.seq, state, key); };
    auto progress = [&]() {
        if (options_.on_progress) options_.on_progress(res);
    };
    auto strong = [](const std::string& etag) { return !etag.empty() && etag.compare(0, 2, "W/") != 0; };
    std::vector<PlanEntry> uploads;   // the files to send again: sent together as one group at the end
    std::set<int> folder_uploads;     // the folder proposals whose files are among them: their entry ends with the group

    for (const PlanEntry& e : entries) {
        if (e.state != "planned") continue;
        if (e.choice == Choice::None) {
            ++res.proposals_open;
            continue;
        }
        if (!res.stopped_by_key.empty()) break;
        if (cancel.requested()) {
            res.stopped_by_key = "cancelled";
            break;
        }
        const bool server_side = e.action == PlanAction::ProposeDeleteServer;   // the server still has it; the reader lacks it
        const bool folder = e.kind == Kind::Folder;
        const std::string remote = join_remote(pair->remote_folder, e.rel);
        if (e.choice == Choice::Leave) {
            // remembered: the records stay and are quiet, with the identity of the file that is still there as it is now
            bool ok = true;
            for (const SyncRecord& rec : store_->sync_records(pair->id)) {
                if (!(rec.rel == e.rel || (folder && rec.rel.size() > e.rel.size() && rec.rel.compare(0, e.rel.size() + 1, remote_prefix(e.rel)) == 0))) continue;
                SyncRecord r = rec;
                r.quiet = true;
                if (!folder && server_side) {
                    r.server_etag = e.plan_etag;
                    r.server_size = e.size;
                    r.server_checksum = strongest_text(e.plan_checksum);
                } else if (!folder) {
                    r.local_size = e.plan_lsize;
                    r.local_mtime = e.plan_lmtime;
                    r.local_sha256.clear();
                }
                r.synced_at = clock_->now();
                ok = store_->sync_record_put(r).ok() && ok;
            }
            finish_entry(e, ok ? "done" : "failed", ok ? std::string() : std::string("write_failed"));
            if (!ok) count_failure(&res, "write_failed");
        } else if (e.choice == Choice::TransferAgain && folder) {
            // a folder: every synced file below it goes back, as its record knows it (the same version, the same reader copy); what changed since is skipped
            const std::vector<SyncRecord> records = store_->sync_records(pair->id);
            const std::string prefix = remote_prefix(e.rel);
            int skipped_here = 0, failed_here = 0;
            for (const SyncRecord& rec : records) {
                if (rec.rel.size() <= prefix.size() || rec.rel.compare(0, prefix.size(), prefix) != 0) continue;
                if (cancel.requested()) {
                    res.stopped_by_key = "cancelled";
                    break;
                }
                if (server_side) {   // the reader lost the folder: download each file
                    DownloadRequest req;
                    req.remote_path = join_remote(pair->remote_folder, rec.rel);
                    req.size = rec.server_size;
                    req.etag = rec.server_etag;
                    req.checksum = rec.server_checksum;
                    req.policy = ConflictPolicy::Ask;
                    req.pair_id = pair->id;
                    req.target_root = pair->local_folder;
                    req.strip_prefix = pair->remote_folder;
                    req.strict_version = true;
                    const DownloadOutcome outcome = dl_->run(req, cancel, [this](const JobProgress& p) {
                        if (options_.on_file_progress) options_.on_file_progress(p);
                    });
                    if (outcome.state == JobState::Done) {
                        ++res.downloaded;
                    } else if (outcome.state == JobState::Cancelled || cancel.requested()) {
                        res.stopped_by_key = "cancelled";
                        break;
                    } else if (general_problem(outcome.error_key)) {
                        res.stopped_by_key = outcome.error_key;
                        ++failed_here;
                        break;
                    } else {
                        ++skipped_here;   // changed on the server since, or a name the reader cannot take: left for the next plan
                    }
                } else {   // the server lost the folder: send each file again, if the reader's copy is the one the record knows
                    PlanEntry up;
                    up.seq = e.seq;
                    up.rel = rec.rel;
                    up.local_rel = rec.local_rel;
                    up.action = PlanAction::Upload;
                    up.plan_lsize = rec.local_size;
                    up.plan_lmtime = rec.local_mtime;
                    up.state = "planned";
                    uploads.push_back(std::move(up));
                    folder_uploads.insert(e.seq);
                }
            }
            if (server_side) {
                res.skipped += skipped_here;
                const bool stopped_by_user = res.stopped_by_key == "cancelled";   // the entry stays planned: the rest can be chosen again
                if (!stopped_by_user) {
                    finish_entry(e, failed_here > 0 ? "failed" : "done", failed_here > 0 ? res.stopped_by_key : std::string());
                    if (failed_here > 0) count_failure(&res, res.stopped_by_key);
                }
            }
        } else if (e.choice == Choice::TransferAgain && !folder) {
            if (server_side) {   // the server still has it: copy it back to the reader
                const auto rec = store_->sync_record(pair->id, e.rel);
                if (!rec) {
                    finish_entry(e, "failed", "not_found");
                    count_failure(&res, "not_found");
                    continue;
                }
                if (options_.hook) options_.hook("before_download");
                DownloadRequest req;
                req.remote_path = remote;
                req.size = e.size;
                req.etag = e.plan_etag;
                req.checksum = e.plan_checksum;
                req.policy = ConflictPolicy::Ask;
                req.pair_id = pair->id;
                req.target_root = pair->local_folder;
                req.strip_prefix = pair->remote_folder;
                req.strict_version = true;
                const DownloadOutcome outcome = dl_->run(req, cancel, [this](const JobProgress& p) {
                    if (options_.on_file_progress) options_.on_file_progress(p);
                });
                if (outcome.state == JobState::Done) {
                    finish_entry(e, "done", "");
                    ++res.downloaded;
                } else if (outcome.state == JobState::Cancelled || cancel.requested()) {
                    res.stopped_by_key = "cancelled";
                } else {
                    std::string key = outcome.error_key.empty() ? std::string("connection_lost") : outcome.error_key;
                    if (outcome.state == JobState::Blocked || key == "not_found") key = "changed_during_sync";
                    finish_entry(e, general_problem(key) ? "failed" : "skipped", key);
                    if (general_problem(key)) {
                        count_failure(&res, key);
                        res.stopped_by_key = key;
                    } else {
                        ++res.skipped;
                    }
                }
            } else {   // the reader still has it: send it to the server again, as an upload of the one file
                PlanEntry up = e;
                up.action = PlanAction::Upload;
                up.plan_etag.clear();   // the server lacks the file: a plain upload, nothing to replace
                up.state = "planned";
                uploads.push_back(std::move(up));
            }
        } else if (e.choice == Choice::Delete) {
            if (server_side) {   // goes to the server's trash through the queue, conditional on the version the plan saw
                if (!sender_) {
                    finish_entry(e, "failed", "protocol_error");
                    count_failure(&res, "protocol_error");
                    continue;
                }
                QueueActions actions(*store_, rules_);
                const Result<int64_t> item = actions.add_sync_delete(pair->id, remote, e.plan_etag, folder ? Kind::Folder : Kind::File);
                if (!item.ok()) {
                    finish_entry(e, "failed", item.error.key);
                    count_failure(&res, item.error.key);
                    continue;
                }
                const SendReport report = sender_->run(cancel, item.value);
                const auto done = store_->queue_item(item.value);
                if (done && done->state == ItemState::Done) {
                    finish_entry(e, "done", "");
                    ++res.deleted;
                } else if (!report.stopped_by_key.empty() && (!done || done->state == ItemState::Queued || done->state == ItemState::Sending)) {
                    res.stopped_by_key = report.stopped_by_key;   // the item stays queued and the entry planned: the user can choose again
                } else {
                    const std::string key = done && !done->reason.empty() ? done->reason : std::string("write_failed");
                    finish_entry(e, "failed", key);
                    count_failure(&res, key);
                }
            } else {   // the reader's copy: only an unchanged file, re-checked now
                int removed = 0, kept = 0, failed = 0;
                const std::vector<SyncRecord> records = store_->sync_records(pair->id);
                for (const SyncRecord& rec : records) {
                    if (!(rec.rel == e.rel || (folder && rec.rel.size() > e.rel.size() && rec.rel.compare(0, e.rel.size() + 1, remote_prefix(e.rel)) == 0))) continue;
                    const Error r = remove_pair_copy(pair->local_folder, rec.local_rel, rec);
                    if (r.ok() || r.key == "not_found") {
                        store_->sync_record_drop(pair->id, rec.rel);
                        if (r.ok()) ++removed;
                    } else if (r.key == "local_changed") {
                        ++kept;
                    } else {
                        ++failed;
                    }
                }
                res.deleted += removed;
                res.skipped += kept;   // changed files are kept, and the result says how many
                if (failed > 0) {
                    finish_entry(e, "failed", "write_failed");
                    count_failure(&res, "write_failed");
                } else {
                    finish_entry(e, "done", kept > 0 ? "local_changed" : "");
                }
            }
        } else {
            finish_entry(e, "failed", "not_allowed");   // a choice that this kind of proposal never offers
            count_failure(&res, "not_allowed");
        }
        if (options_.hook) options_.hook("proposal_applied");
        progress();
    }
    // the files to send again, as one group of the queue, sent on its own
    if (!uploads.empty() && res.stopped_by_key.empty()) {
        const int failed_before = res.failed;
        run_uploads(*pair, plan_id, uploads, std::set<int>(), cancel, &res);
        for (const PlanEntry& e : entries)   // a folder's entry ends when its files went: done, or failed when any of them did not
            if (folder_uploads.count(e.seq) && res.stopped_by_key.empty()) finish_entry(e, res.failed > failed_before ? "failed" : "done", res.failed > failed_before ? std::string("write_failed") : std::string());
    }

    res.seconds = std::max<int64_t>(0, clock_->now() - started);
    store_->sync_set_result(pair->id, !res.stopped_by_key.empty() ? "interrupted" : (res.failed > 0 ? "partial" : "done"), json_of(res), clock_->now());
    if (options_.hook) options_.hook("proposals_applied");
    out.value = res;
    return out;
}

}  // namespace pbc
