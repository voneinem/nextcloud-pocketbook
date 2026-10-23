#include "sync_bridge.h"

#include <QMetaObject>

#include <algorithm>
#include <cstdlib>

#include "format_util.h"
#include "pbc/age_text.h"
#include "pbc/error_keys.h"
#include "pbc/json_min.h"
#include "pbc/sync_planner.h"
#include "pbc/sync_rules.h"
#include "pbc/sync_runner.h"
#include "worker.h"

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }

// The reader folder as the page shows it: below the storage root, "Reader/Books" for /mnt/ext1/Books.
QString reader_name(const std::string& folder, const pbc::SourceRules& rules) {
    std::vector<std::string> roots = {rules.root};
    if (char* real = ::realpath(rules.root.c_str(), nullptr)) {
        roots.insert(roots.begin(), real);
        std::free(real);
    }
    for (std::string root : roots) {
        while (root.size() > 1 && root.back() == '/') root.pop_back();
        if (folder.size() > root.size() && folder.compare(0, root.size(), root) == 0 && folder[root.size()] == '/')
            return bridge_tr("Reader") + QStringLiteral("/") + display_safe(folder.substr(root.size() + 1), 120);
    }
    return display_safe(folder, 120);
}

QString direction_label(pbc::Direction d) {
    switch (d) {
    case pbc::Direction::Down: return bridge_tr("Download only");
    case pbc::Direction::Up: return bridge_tr("Upload only");
    case pbc::Direction::Both: return bridge_tr("Two-way");
    }
    return QString();
}

}  // namespace

SyncBridge::SyncBridge(Config config, QObject* parent) : QObject(parent), c_(std::move(config)), scheduler_(c_.auto_sync_quiet_ms) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, [this] { evaluate_auto_sync(); });
    // the folder rules of sync_rules.h run inside the same transaction as the write of a pair
    const pbc::SourceRules rules = c_.rules;
    const std::string download = c_.download_root;
    c_.store->set_pair_validator([rules, download](pbc::SyncPair& candidate, const std::vector<pbc::SyncPair>& others) -> pbc::Error {
        const pbc::PairCheck check = pbc::check_pair(candidate, others, rules, download);
        if (!check.ok) return pbc::error_from_key(check.error_key);
        candidate.local_folder = check.local_folder;
        return {};
    });
    mono_.start();
    c_.store->sync_recover();   // at start, without any network: what a kill left comparing or running becomes interrupted
    reload();
}

SyncBridge::~SyncBridge() {
    timer_.stop();
    cancel_.flag = true;
    if (c_.worker) c_.worker->wait_idle();   // no job may call back into a bridge that is gone
    c_.store->set_pair_validator(nullptr);
}

QVariantMap SyncBridge::error_map(const std::string& key) const {
    QVariantMap m;
    m["ok"] = false;
    m["errorKey"] = q(key);
    return m;
}

QString SyncBridge::describeDirection(const QString& direction) const {
    const auto d = pbc::parse_direction(direction.toStdString());
    if (!d) return QString();
    switch (*d) {
    case pbc::Direction::Down:
        return bridge_tr("Download only: files from the server are copied to the reader. Nothing on the reader is sent.");
    case pbc::Direction::Up:
        return bridge_tr("Upload only: new files from the reader are sent to the server. Nothing from the server is copied.");
    case pbc::Direction::Both:
        return bridge_tr("Two-way: new and changed files are copied in both directions. Nothing is ever deleted automatically.");
    }
    return QString();
}

QVariantMap SyncBridge::pair_map(const pbc::SyncPair& p, const std::optional<pbc::AccountRow>& account) const {
    QVariantMap m;
    const bool same_account = account && account->base_url == p.server && account->user_id == p.user_id && p.problem_key != "account_changed";
    m["id"] = static_cast<qlonglong>(p.id);
    m["remoteFolder"] = display_safe(p.remote_folder, 120);
    m["readerFolder"] = reader_name(p.local_folder, c_.rules);
    m["remotePath"] = q(p.remote_folder);
    m["readerPath"] = q(p.local_folder);
    m["direction"] = q(pbc::direction_text(p.direction));
    m["directionText"] = direction_label(p.direction);
    m["usable"] = same_account;
    // the state key the page words: a problem of the pair first, then the lock/plan state, then whether it ever synced
    std::string state;
    if (!same_account) state = "account_changed";
    else if (!p.problem_key.empty()) state = p.problem_key == "no_network" ? "waiting" : p.problem_key;
    else if (p.state == "comparing" || p.state == "running" || p.state == "interrupted" || p.state == "plan_ready") state = p.state;
    else state = p.last_outcome == "never" ? "never" : "in_sync";
    if (waiting_pairs_.count(p.id) && (state == "never" || state == "in_sync")) state = "waiting";   // an Auto Sync comparison found no Wi-Fi
    m["state"] = q(state);
    m["problemKey"] = q(same_account ? p.problem_key : std::string("account_changed"));
    QString text;
    if (state == "never") text = bridge_tr("Never synced");
    else if (state == "in_sync") text = bridge_tr("In sync");
    else if (state == "comparing") text = bridge_tr("Comparing");
    else if (state == "running") text = bridge_tr("Syncing");
    else if (state == "interrupted") {
        text = bridge_tr("Interrupted");
        if (const auto plan = c_.store->sync_interrupted_plan(p.id)) {   // what the run had done when it stopped
            int done = 0, total = 0;
            for (const pbc::PlanEntry& e : c_.store->sync_plan_page(*plan, "transfers", 0, pbc::kSyncMaxPlanEntries).rows) {
                if (e.removed) continue;
                ++total;
                if (e.state == "done") ++done;
            }
            if (total > 0) text = bridge_tr("Interrupted: %1 of %2 done").arg(done).arg(total);
        }
    }
    else if (state == "plan_ready") text = bridge_tr("Plan ready");
    else if (state == "waiting") text = bridge_tr("Waiting: the reader is offline");
    else if (state == "server_folder_missing") text = bridge_tr("Server folder not found");
    else if (state == "reader_folder_missing") text = bridge_tr("Reader folder not available");
    else if (state == "account_changed") text = bridge_tr("Other account");
    else if (state == "auth_failed") text = bridge_tr("Sign in again");
    m["stateText"] = text;
    m["lastText"] = p.last_sync_at && c_.format_date_time ? bridge_tr("Last sync: %1").arg(c_.format_date_time(static_cast<qint64>(*p.last_sync_at))) : QString();
    m["heldFiles"] = p.held_files ? QVariant(static_cast<qlonglong>(*p.held_files)) : QVariant(-1);
    m["heldBytes"] = p.held_bytes ? QVariant(static_cast<qlonglong>(*p.held_bytes)) : QVariant(-1);
    m["maxFiles"] = static_cast<qlonglong>(p.max_files);
    m["maxBytes"] = static_cast<qlonglong>(p.max_bytes);
    m["usageText"] = p.held_files && p.held_bytes
                         ? bridge_tr("%1 of %2 files, %3 of %4").arg(*p.held_files).arg(p.max_files).arg(human_size(*p.held_bytes)).arg(human_size(p.max_bytes))
                         : bridge_tr("Up to %1 files, %2").arg(p.max_files).arg(human_size(p.max_bytes));
    m["planReady"] = false;
    m["planOverLimit"] = false;
    m["planCounts"] = QVariantMap();
    m["startupSync"] = p.startup_sync;
    m["autoSync"] = p.auto_sync;
    return m;
}

int64_t SyncBridge::ready_plan_of(const pbc::SyncPair& pair) const {
    if (pair.state != "plan_ready") return 0;
    return c_.store->sync_ready_plan(pair.id).value_or(0);
}

void SyncBridge::reload() {
    const auto account = c_.store->account();
    QVariantList list;
    int ready = 0;
    for (const pbc::SyncPair& p : c_.store->sync_pairs()) {
        QVariantMap m = pair_map(p, account);
        if (m.value("usable").toBool()) {
            if (const int64_t plan_id = ready_plan_of(p); plan_id > 0) {
                if (const auto plan = c_.store->sync_plan(plan_id)) {
                    ++ready;
                    m["planReady"] = true;
                    m["planId"] = static_cast<qlonglong>(plan_id);
                    m["planOverLimit"] = !plan->over_limit.empty();
                    QVariantMap counts;
                    counts["downloads"] = plan->totals.n_download;
                    counts["uploads"] = plan->totals.n_upload;
                    counts["keptBoth"] = plan->totals.n_keep_both;
                    counts["proposals"] = plan->totals.n_prop_server + plan->totals.n_prop_reader;
                    m["planCounts"] = counts;
                    m["stateText"] = plan->over_limit.empty() ? bridge_tr("Plan ready - %1 downloads, %2 uploads").arg(plan->totals.n_download).arg(plan->totals.n_upload)
                                                                : bridge_tr("Plan ready, over the limit");
                }
            }
        }
        list.append(m);
    }
    pairs_ = list;
    ready_count_ = ready;
}

void SyncBridge::refresh() {
    reload();
    emit changed();
}

// Applies the keys of `spec` that are present. For a new pair the unset ones keep the defaults of default_pair().
QVariantMap SyncBridge::apply_spec(pbc::SyncPair* pair, const QVariantMap& spec, bool adding) {
    const pbc::Direction before = pair->direction;
    if (spec.contains("remoteFolder")) pair->remote_folder = spec.value("remoteFolder").toString().toStdString();
    if (spec.contains("readerFolder")) pair->local_folder = spec.value("readerFolder").toString().toStdString();
    if (spec.contains("direction")) {
        const auto d = pbc::parse_direction(spec.value("direction").toString().toStdString());
        if (!d) return error_map("protocol_error");
        pair->direction = *d;
    }
    if (spec.contains("maxFiles")) pair->max_files = spec.value("maxFiles").toLongLong();
    if (spec.contains("maxBytes")) pair->max_bytes = spec.value("maxBytes").toLongLong();
    if (spec.contains("startupSync")) pair->startup_sync = spec.value("startupSync").toBool();
    if (spec.contains("autoSync")) pair->auto_sync = spec.value("autoSync").toBool();
    // FR-005: a pair that can send reader files to the server needs the explicit confirmation of the user
    const pbc::Direction from = adding ? pbc::Direction::Down : before;
    if (pbc::needs_send_confirmation(from, pair->direction) && !spec.value("confirmed").toBool()) return error_map("confirmation_needed");
    return QVariantMap();
}

QVariantMap SyncBridge::addPair(const QVariantMap& spec) {
    const auto account = c_.store->account();
    if (!account) return error_map("auth_failed");
    pbc::SyncPair pair = pbc::default_pair(account->base_url, account->user_id);
    if (const QVariantMap refused = apply_spec(&pair, spec, true); !refused.isEmpty()) return refused;
    const auto r = c_.store->sync_pair_add(pair);
    if (!r.ok()) return error_map(r.error.key);
    reload();
    emit changed();
    const auto saved = c_.store->sync_pair(r.value);
    if (saved) noteLocalAction(q(saved->local_folder));   // the pair's own settings are a change of the user's: Auto Sync may compare it
    QVariantMap out;
    out["ok"] = true;
    out["id"] = static_cast<qlonglong>(r.value);
    return out;
}

QVariantMap SyncBridge::updatePair(qlonglong id, const QVariantMap& spec) {
    auto pair = c_.store->sync_pair(id);
    if (!pair) return error_map("not_found");
    if (const QVariantMap refused = apply_spec(&*pair, spec, false); !refused.isEmpty()) return refused;
    if (const pbc::Error e = c_.store->sync_pair_update(*pair); !e.ok()) return error_map(e.key);
    reload();
    emit changed();
    if (const auto saved = c_.store->sync_pair(id)) noteLocalAction(q(saved->local_folder));
    QVariantMap out;
    out["ok"] = true;
    return out;
}

QVariantMap SyncBridge::removePair(qlonglong id) {
    const int kept = static_cast<int>(c_.store->queue_sync_open_rels(id).size());
    if (const pbc::Error e = c_.store->sync_pair_remove(id); !e.ok()) return error_map(e.key);
    scheduler_.forget(id);
    armed_pairs_.erase(id);
    waiting_pairs_.erase(id);
    reload();
    emit changed();
    QVariantMap out;
    out["ok"] = true;
    out["queueItemsKept"] = kept;
    return out;
}


// ------------------------------------------------------------------------------------------------------------------------------ review

void SyncBridge::setPlanPageSize(int n) {
    n = std::max(1, std::min(n, 100));
    if (n == plan_page_size_) return;
    plan_page_size_ = n;
    emit changed();
}

namespace {

QString age_kind_text(pbc::AgeKind k) {
    switch (k) {
    case pbc::AgeKind::JustNow: return QStringLiteral("just_now");
    case pbc::AgeKind::Minutes: return QStringLiteral("minutes");
    case pbc::AgeKind::Hours: return QStringLiteral("hours");
    case pbc::AgeKind::Days: return QStringLiteral("days");
    default: return QStringLiteral("not_synced");
    }
}

QVariantMap error_result(qlonglong pair_id, const QString& key) {
    QVariantMap m;
    m["pairId"] = pair_id;
    m["downloaded"] = 0;
    m["uploaded"] = 0;
    m["deleted"] = 0;
    m["skipped"] = 0;
    m["keptBoth"] = 0;
    m["proposalsOpen"] = 0;
    m["failed"] = 0;
    m["seconds"] = 0;
    m["failures"] = QVariantList();
    m["stoppedBy"] = key;
    return m;
}

}  // namespace

QVariantMap SyncBridge::header_of(const pbc::SyncPlan& plan, bool preview) const {
    QVariantMap h;
    h["id"] = static_cast<qlonglong>(preview ? 0 : plan.id);
    h["pairId"] = static_cast<qlonglong>(plan.pair_id);
    h["createdText"] = c_.format_date_time ? c_.format_date_time(static_cast<qint64>(plan.created_at)) : QString();
    const pbc::Age age = pbc::listing_age(c_.clock->now(), plan.created_at, false);
    h["ageKind"] = age_kind_text(age.kind);
    h["ageCount"] = age.n;
    h["trigger"] = q(plan.trigger);
    h["state"] = q(plan.state);
    QVariantMap t;
    t["downloads"] = plan.totals.n_download;
    t["uploads"] = plan.totals.n_upload;
    t["skipped"] = plan.totals.n_skip;
    t["keptBoth"] = plan.totals.n_keep_both;
    t["proposalsServer"] = plan.totals.n_prop_server;
    t["proposalsReader"] = plan.totals.n_prop_reader;
    t["failed"] = plan.totals.n_failed;
    t["foldersNew"] = plan.totals.folders_new;
    t["inSync"] = plan.totals.n_in_sync;
    h["totals"] = t;
    QVariantMap z;
    z["downloadText"] = human_size(plan.totals.b_download);
    z["uploadText"] = human_size(plan.totals.b_upload);
    z["keptBothText"] = human_size(plan.totals.b_keep_both);
    h["sizes"] = z;
    h["freeText"] = plan.free_bytes ? human_size(*plan.free_bytes) : QString();
    h["quotaText"] = plan.quota_free ? human_size(*plan.quota_free) : QString();
    h["reserveText"] = human_size(plan.reserve_bytes);
    h["overLimit"] = q(plan.over_limit);
    h["overLimitText"] = QString();
    if (!plan.over_limit.empty()) {   // by how much: what the pair holds plus what the plan adds, against the limits
        if (const auto pair = c_.store->sync_pair(plan.pair_id)) {
            const qlonglong over_files = std::max<qlonglong>(0, pair->held_files.value_or(0) + plan.totals.n_download + plan.totals.n_keep_both - pair->max_files);
            const qlonglong over_bytes = std::max<qlonglong>(0, pair->held_bytes.value_or(0) + plan.totals.b_download + plan.totals.b_keep_both - pair->max_bytes);
            QStringList parts;
            if (over_files > 0) parts << bridge_tr("Over the file limit by %1.").arg(over_files);
            if (over_bytes > 0) parts << bridge_tr("Over the size limit by %1.").arg(human_size(over_bytes));
            h["overLimitText"] = parts.join(QLatin1Char(' '));
        }
    }
    h["incomplete"] = plan.incomplete;
    h["incompleteFolders"] = plan.incomplete_folders;
    h["truncated"] = plan.truncated;
    h["unapprovable"] = plan.unapprovable;
    const bool too_old = c_.clock->now() - plan.created_at > pbc::kSyncPlanMaxAgeS;
    h["stale"] = preview || too_old;
    const pbc::Approval a = pbc::can_approve(plan, c_.clock->now());
    h["approvable"] = a.ok;
    h["approveErrorKey"] = q(a.error_key);
    return h;
}

QVariantMap SyncBridge::plan(qlonglong planId) const {
    if (planId == 0) {
        if (preview_pair_ == 0) return QVariantMap();
        return header_of(preview_, true);
    }
    const auto plan = c_.store->sync_plan(planId);
    if (!plan) return QVariantMap();
    QVariantMap h = header_of(*plan, false);
    int removed = 0;
    for (const pbc::PlanEntry& e : c_.store->sync_plan_page(planId, "transfers", 0, pbc::kSyncMaxPlanEntries).rows)
        if (e.removed) ++removed;
    h["removedCount"] = removed;
    return h;
}

QVariantMap SyncBridge::row_of(const pbc::PlanEntry& e) const {
    QVariantMap r;
    r["seq"] = e.seq;
    r["nameText"] = display_safe(e.rel, 160);
    r["localNameText"] = e.local_rel != e.rel ? display_safe(e.local_rel, 160) : QString();
    auto last_name = [](const std::string& rel) { return rel.substr(rel.rfind('/') == std::string::npos ? 0 : rel.rfind('/') + 1); };
    r["copyReader"] = e.conflict_name.empty() ? QString() : display_safe(last_name(e.conflict_name), 160);          // the name of the server's version added on the reader
    r["copyServer"] = e.conflict_name_server.empty() ? QString() : display_safe(last_name(e.conflict_name_server), 160);   // the name of the reader's version added on the server
    r["action"] = q(pbc::plan_action_text(e.action));
    r["folder"] = e.kind == pbc::Kind::Folder;
    r["recordsBelow"] = e.records_below;
    r["sizeText"] = e.size ? human_size(*e.size) : QString();
    r["reason"] = q(e.reason);
    r["unapprovable"] = q(e.unapprovable);
    r["removed"] = e.removed;
    QStringList allowed;
    for (pbc::Choice c : e.allowed) allowed << q(pbc::choice_text(c));
    r["allowed"] = allowed;
    r["choice"] = q(pbc::choice_text(e.choice));
    r["state"] = q(e.state);
    r["resultKey"] = q(e.result_key);
    return r;
}

namespace {

// The entries of an in-memory plan (the offline preview) in a section, in plan order.
std::vector<pbc::PlanEntry> section_of(const pbc::SyncPlan& plan, const QString& section) {
    std::vector<pbc::PlanEntry> out;
    for (const pbc::PlanEntry& e : plan.entries) {
        const bool transfer = e.action == pbc::PlanAction::Download || e.action == pbc::PlanAction::Upload || e.action == pbc::PlanAction::KeepBoth;
        const bool proposal = e.action == pbc::PlanAction::ProposeDeleteServer || e.action == pbc::PlanAction::ProposeDeleteReader;
        const bool skip = e.action == pbc::PlanAction::Skip && e.reason != "gone_both";
        if ((section == QLatin1String("transfers") && transfer) || (section == QLatin1String("proposals") && proposal) || (section == QLatin1String("skips") && skip) ||
            section == QLatin1String("all"))
            out.push_back(e);
    }
    return out;
}

bool known_section(const QString& s) {
    return s == QLatin1String("transfers") || s == QLatin1String("skips") || s == QLatin1String("proposals");
}

}  // namespace

QVariantList SyncBridge::planPage(qlonglong planId, const QString& section, int page) const {
    QVariantList out;
    if (!known_section(section) || page < 0) return out;
    if (planId == 0) {
        const auto rows = section_of(preview_, section);
        for (size_t i = static_cast<size_t>(page) * plan_page_size_; i < rows.size() && i < static_cast<size_t>(page + 1) * plan_page_size_; ++i) out.append(row_of(rows[i]));
        return out;
    }
    for (const pbc::PlanEntry& e : c_.store->sync_plan_page(planId, section.toStdString(), page * plan_page_size_, plan_page_size_).rows) out.append(row_of(e));
    return out;
}

int SyncBridge::planPageCount(qlonglong planId, const QString& section) const {
    if (!known_section(section)) return 1;
    const int total = planId == 0 ? static_cast<int>(section_of(preview_, section).size()) : c_.store->sync_plan_page(planId, section.toStdString(), 0, 1).total;
    return std::max(1, (total + plan_page_size_ - 1) / plan_page_size_);
}

QVariantMap SyncBridge::removeEntry(qlonglong planId, int seq, bool removed) {
    if (planId == 0) return error_map("plan_not_approvable");
    if (const pbc::Error e = c_.store->sync_plan_remove_entry(planId, seq, removed); !e.ok()) return error_map(e.key);
    reload();
    emit changed();
    QVariantMap out;
    out["ok"] = true;
    return out;
}

QVariantMap SyncBridge::cancelPlan(qlonglong planId) {
    if (planId == 0) {   // the preview is only in memory
        preview_ = pbc::SyncPlan();
        preview_pair_ = 0;
        emit changed();
        QVariantMap out;
        out["ok"] = true;
        return out;
    }
    if (const pbc::Error e = c_.store->sync_plan_cancel(planId); !e.ok()) return error_map(e.key);
    reload();
    emit changed();
    QVariantMap out;
    out["ok"] = true;
    return out;
}

void SyncBridge::cancelRun() { cancel_.flag = true; }

QVariantMap SyncBridge::result_map(int64_t pair_id, const std::string& json, std::optional<int64_t> at) const {
    QVariantMap m = error_result(static_cast<qlonglong>(pair_id), QString());
    const auto parsed = pbc::parse_json(json);
    if (parsed.ok()) {
        const pbc::JsonValue& v = parsed.value;
        auto num = [&](const char* key) -> int {
            const pbc::JsonValue* n = v.get(key);
            return n ? static_cast<int>(n->as_int().value_or(0)) : 0;
        };
        m["downloaded"] = num("downloaded");
        m["uploaded"] = num("uploaded");
        m["skipped"] = num("skipped");
        m["keptBoth"] = num("kept_both");
        m["proposalsOpen"] = num("proposals_open");
        m["deleted"] = num("deleted");
        m["failed"] = num("failed");
        m["seconds"] = num("seconds");
        if (const pbc::JsonValue* by = v.get("stopped_by")) m["stoppedBy"] = q(by->text);
        QVariantList failures;
        if (const pbc::JsonValue* f = v.get("failures"))
            for (const pbc::JsonValue& item : f->items) {
                QVariantMap fm;
                const pbc::JsonValue* k = item.get("key");
                const pbc::JsonValue* c = item.get("count");
                fm["key"] = k ? q(k->text) : QString();
                fm["count"] = c ? static_cast<int>(c->as_int().value_or(0)) : 0;
                failures.append(fm);
            }
        m["failures"] = failures;
    }
    m["atText"] = at && c_.format_date_time ? c_.format_date_time(static_cast<qint64>(*at)) : QString();
    return m;
}

QVariantMap SyncBridge::resultFor(qlonglong pairId) const {
    const auto pair = c_.store->sync_pair(pairId);
    if (!pair) return QVariantMap();
    return result_map(pair->id, pair->last_result_json, pair->last_sync_at);
}

// ---- compare

void SyncBridge::compare(qlonglong id) {
    compare_queue_.clear();
    waiting_pairs_.erase(id);
    start_compare({id, "tap", true}, false);
}

void SyncBridge::compareAll() {
    compare_queue_.clear();
    first_ready_pair_ = first_ready_plan_ = 0;
    waiting_pairs_.clear();
    for (const QVariant& v : pairs_)
        if (v.toMap().value("usable").toBool()) compare_queue_.push_back({v.toMap().value("id").toLongLong(), "tap", true});
    compare_next();
}

void SyncBridge::compare_next() {
    if (compare_queue_.empty()) {
        if (first_ready_plan_ != 0) emit planReady(first_ready_pair_, first_ready_plan_);
        first_ready_pair_ = first_ready_plan_ = 0;
        evaluate_auto_sync();   // the single follow-up for a change that came while something was running
        return;
    }
    const Pending next = compare_queue_.front();
    compare_queue_.pop_front();
    start_compare(next, true);
}

void SyncBridge::start_compare(const Pending& what, bool then_next) {
    const qlonglong id = what.id;
    const bool unattended = what.trigger != "tap";
    const auto pair = c_.store->sync_pair(id);
    if (!pair) {
        if (then_next) compare_next();
        return;
    }
    if (busy_ || pair->state == "comparing" || pair->state == "running") {
        if (!unattended) emit comparisonFailed(id, QStringLiteral("sync_busy"));
        if (then_next) compare_next();
        return;
    }
    if (!pbc::pair_matches_account(*c_.store, *pair)) {
        if (!unattended) emit comparisonFailed(id, QStringLiteral("account_changed"));   // before anything: no request, no lock
        if (then_next) compare_next();
        return;
    }
    busy_ = true;
    busy_pair_ = id;
    stage_ = QStringLiteral("comparing");
    progress_text_.clear();
    percent_ = 0;
    cancel_.flag = false;
    emit changed();
    auto go = [this, what, then_next](bool online) {
        if (!online) {
            busy_ = false;
            busy_pair_ = 0;
            stage_ = QStringLiteral("idle");
            if (what.trigger != "tap") waiting_pairs_.insert(what.id);   // nobody asked: the pair waits, nothing retries
            reload();
            emit changed();
            if (what.trigger == "tap") emit comparisonFailed(what.id, QStringLiteral("no_network"));
            if (then_next) compare_next();
            return;
        }
        c_.worker->post([this, what, then_next] {
            pbc::Downloader dl(*c_.store, *c_.http, *c_.storage, *c_.clock, c_.download_root, c_.wifi_up, c_.downloader_options);
            pbc::RunnerOptions ro;
            ro.hook = c_.runner_hook;
            pbc::SyncRunner runner(*c_.store, *c_.http, dl, *c_.storage, *c_.clock, c_.rules, ro);
            const pbc::Result<int64_t> r = runner.compare(what.id, what.trigger, cancel_);
            QMetaObject::invokeMethod(this, [this, what, r, then_next] {
                finish_compare(what, r);
                if (then_next) compare_next();
                else evaluate_auto_sync();
            }, Qt::QueuedConnection);
        });
    };
    // a tap may switch Wi-Fi on; a start-up or Auto Sync comparison never does (the caller has seen that Wi-Fi is up)
    if (what.ensure_network && c_.ensure_network) c_.ensure_network(go);
    else go(!offline());
}

void SyncBridge::finish_compare(const Pending& what, const pbc::Result<int64_t>& r) {
    const qlonglong id = what.id;
    busy_ = false;
    busy_pair_ = 0;
    stage_ = QStringLiteral("idle");
    if (r.ok()) waiting_pairs_.erase(id);
    reload();
    emit changed();
    if (!r.ok()) {
        if (what.trigger == "tap") emit comparisonFailed(id, q(r.error.key));
        return;
    }
    if (what.trigger != "tap") return;   // an unattended comparison only prepared a plan: the pair says "plan ready", nothing opens by itself
    if (compare_queue_.empty() && first_ready_plan_ == 0) {   // a single tap: the plan is shown
        emit planReady(id, static_cast<qlonglong>(r.value));
        return;
    }
    const auto pair = c_.store->sync_pair(id);
    if (pair && pair->state == "plan_ready" && first_ready_plan_ == 0) {
        first_ready_pair_ = id;
        first_ready_plan_ = static_cast<qlonglong>(r.value);
    }
}

// ---- triggers other than a tap

void SyncBridge::startupCompare() {
    if (offline()) return;   // with Wi-Fi off at start nothing is requested; the app never turns it on
    for (const QVariant& v : pairs_) {
        const QVariantMap m = v.toMap();
        if (m.value("usable").toBool() && m.value("startupSync").toBool()) compare_queue_.push_back({m.value("id").toLongLong(), "startup", false});
    }
    if (!busy_) compare_next();
}

void SyncBridge::note_for_pairs(const std::vector<int64_t>& ids) {
    if (ids.empty() || c_.store->setting("auto_sync") == "0") return;   // the one switch in Settings still means "nothing automatic"
    bool any = false;
    for (const pbc::SyncPair& p : c_.store->sync_pairs()) {
        if (!p.auto_sync || std::find(ids.begin(), ids.end(), p.id) == ids.end()) continue;
        scheduler_.note_action(p.id, now_ms());
        armed_pairs_.insert(p.id);
        waiting_pairs_.erase(p.id);
        any = true;
    }
    if (any) evaluate_auto_sync();
}

void SyncBridge::noteAction(const QString& remotePath) {
    note_for_pairs(pbc::pairs_for_server_path(c_.store->sync_pairs(), remotePath.toStdString()));
}

void SyncBridge::noteLocalAction(const QString& localPath) {
    note_for_pairs(pbc::pairs_for_reader_path(c_.store->sync_pairs(), localPath.toStdString()));
}

// What the scheduler says now: compare the pairs that are due (one after the other, without switching Wi-Fi on), wait for the nearest quiet period with the
// one timer, or - when the quiet period ended without Wi-Fi - leave the pair waiting and retry nothing.
void SyncBridge::evaluate_auto_sync() {
    const std::vector<pbc::SyncPair> pairs = c_.store->sync_pairs();
    const bool global = c_.store->setting("auto_sync") != "0";
    const bool wifi = !offline();
    auto pair_on = [&](int64_t id) {
        for (const pbc::SyncPair& p : pairs)
            if (p.id == id) return p.auto_sync && pbc::pair_matches_account(*c_.store, p);
        return false;
    };
    const pbc::SyncDue due = scheduler_.next(now_ms(), global, wifi, busy_, pair_on);
    timer_.stop();
    const std::set<qlonglong> before = armed_pairs_;
    const std::set<qlonglong> waiting_before = waiting_pairs_;
    for (qlonglong id : before) {
        const bool is_due = std::find(due.pairs.begin(), due.pairs.end(), id) != due.pairs.end();
        if (is_due) {
            armed_pairs_.erase(id);
        } else if (!scheduler_.armed(id)) {   // dropped: off, or no Wi-Fi when the quiet period ended
            armed_pairs_.erase(id);
            if (global && !wifi && pair_on(id)) waiting_pairs_.insert(id);
        }
    }
    for (int64_t id : due.pairs) compare_queue_.push_back({static_cast<qlonglong>(id), "auto", false});
    if (due.wait_ms >= 0) timer_.start(std::max(1, due.wait_ms));
    if (!due.pairs.empty() && !busy_) compare_next();
    else if (waiting_pairs_ != waiting_before) {
        reload();
        emit changed();
    }
}

void SyncBridge::previewOffline(qlonglong id) {
    if (busy_) return;
    busy_ = true;
    busy_pair_ = id;
    stage_ = QStringLiteral("comparing");
    cancel_.flag = false;
    emit changed();
    c_.worker->post([this, id] {
        pbc::Downloader dl(*c_.store, *c_.http, *c_.storage, *c_.clock, c_.download_root, c_.wifi_up, c_.downloader_options);
        pbc::SyncRunner runner(*c_.store, *c_.http, dl, *c_.storage, *c_.clock, c_.rules);
        pbc::Result<pbc::SyncPlan> r = runner.preview_from_cache(id, cancel_);
        QMetaObject::invokeMethod(this, [this, id, r] {
            busy_ = false;
            busy_pair_ = 0;
            stage_ = QStringLiteral("idle");
            if (r.ok()) {
                preview_ = r.value;
                preview_pair_ = id;
            }
            emit changed();
            if (r.ok()) emit previewReady(id);
            else emit comparisonFailed(id, q(r.error.key));
        }, Qt::QueuedConnection);
    });
}

// ---- approve and run

void SyncBridge::note_progress(const pbc::SyncResult& r, int n_download, int n_upload) {
    const int total = n_download + n_upload;
    const int done = r.downloaded + r.uploaded + r.failed;
    const int pct = total > 0 ? std::min(100, done * 100 / total) : 0;
    const qint64 now = mono_.elapsed();
    if (last_progress_ms_ >= 0 && now - last_progress_ms_ < c_.progress_interval_ms && pct - last_progress_percent_ < c_.progress_step_percent) return;
    last_progress_ms_ = now;
    last_progress_percent_ = pct;
    percent_ = pct;
    if (r.downloaded + r.failed < n_download || n_upload == 0) {
        stage_ = QStringLiteral("downloading");
        progress_text_ = bridge_tr("Downloading %1 of %2").arg(std::min(r.downloaded + r.failed + 1, n_download)).arg(n_download);
    } else {
        stage_ = QStringLiteral("uploading");
        progress_text_ = bridge_tr("Uploading %1 of %2").arg(std::min(r.uploaded + 1, n_upload)).arg(n_upload);
    }
    emit changed();
}

QVariantMap SyncBridge::setChoice(qlonglong planId, int seq, const QString& choice) {
    if (planId == 0) return error_map("plan_not_approvable");
    const std::string text = choice.toStdString();
    const auto parsed = text.empty() ? std::optional<pbc::Choice>(pbc::Choice::None) : pbc::parse_choice(text);
    if (!parsed) return error_map("not_allowed");
    if (const pbc::Error e = c_.store->sync_plan_set_choice(planId, seq, *parsed); !e.ok()) return error_map(e.key);
    reload();
    emit changed();
    QVariantMap out;
    out["ok"] = true;
    return out;
}

QVariantMap SyncBridge::applyChoices(qlonglong planId, bool confirmed) {
    if (planId == 0) return error_map("plan_not_approvable");
    const auto plan = c_.store->sync_plan(planId);
    if (!plan) return error_map("not_found");
    const auto pair = c_.store->sync_pair(plan->pair_id);
    if (!pair) return error_map("not_found");
    if (!pbc::pair_matches_account(*c_.store, *pair)) return error_map("account_changed");
    if (busy_ || pair->state == "comparing" || pair->state == "running") return error_map("sync_busy");
    if (plan->state != "ready" && plan->state != "done") return error_map("plan_not_approvable");
    if (c_.clock->now() - plan->created_at > pbc::kSyncPlanMaxAgeS) return error_map("plan_stale");
    QVariantList deletes;
    int transfers = 0, leaves = 0;
    for (const pbc::PlanEntry& e : c_.store->sync_plan_page(planId, "proposals", 0, pbc::kSyncMaxPlanEntries).rows) {
        if (e.state != "planned") continue;
        if (e.choice == pbc::Choice::Leave) ++leaves;
        else if (e.choice == pbc::Choice::TransferAgain) ++transfers;
        else if (e.choice == pbc::Choice::Delete) {
            QVariantMap d;
            d["nameText"] = display_safe(e.rel, 160);
            d["side"] = e.action == pbc::PlanAction::ProposeDeleteServer ? QStringLiteral("server") : QStringLiteral("reader");
            d["trash"] = e.action == pbc::PlanAction::ProposeDeleteServer;   // the server's trash; the reader has none
            d["folder"] = e.kind == pbc::Kind::Folder;
            d["recordsBelow"] = e.records_below;
            deletes << d;
        }
    }
    QVariantMap out;
    out["ok"] = true;
    out["deletes"] = deletes;
    out["transfers"] = transfers;
    out["leaves"] = leaves;
    if (!confirmed) return out;   // the confirmation is shown first; nothing is applied
    if (deletes.isEmpty() && transfers == 0 && leaves == 0) return error_map("not_allowed");
    const qlonglong pair_id = pair->id;
    busy_ = true;
    busy_pair_ = pair_id;
    stage_ = QStringLiteral("applying");
    progress_text_.clear();
    percent_ = 0;
    cancel_.flag = false;
    emit changed();
    auto go = [this, planId, pair_id](bool online) {
        if (!online) {
            busy_ = false;
            busy_pair_ = 0;
            stage_ = QStringLiteral("idle");
            emit changed();
            emit runFinished(error_result(pair_id, QStringLiteral("no_network")));
            return;
        }
        c_.worker->post([this, planId, pair_id] {
            pbc::Downloader dl(*c_.store, *c_.http, *c_.storage, *c_.clock, c_.download_root, c_.wifi_up, c_.downloader_options);
            pbc::RunnerOptions ro;
            ro.hook = c_.runner_hook;
            pbc::SyncRunner runner(*c_.store, *c_.http, dl, *c_.storage, *c_.clock, c_.rules, ro);
            pbc::SenderOptions so;
            so.wifi_up = c_.wifi_up;
            so.hook = c_.sender_hook;
            pbc::QueueSender sender(*c_.store, *c_.http, *c_.clock, random_, c_.rules, c_.download_root, so);
            runner.attach_queue(&sender);
            const pbc::Result<pbc::SyncResult> r = runner.apply_choices(planId, cancel_);
            QMetaObject::invokeMethod(this, [this, pair_id, r] {
                busy_ = false;
                busy_pair_ = 0;
                stage_ = QStringLiteral("idle");
                reload();
                if (r.ok()) {
                    const auto pair = c_.store->sync_pair(pair_id);
                    last_result_ = result_map(pair_id, pair ? pair->last_result_json : std::string("{}"), pair ? pair->last_sync_at : std::nullopt);
                } else {
                    last_result_ = error_result(pair_id, q(r.error.key));
                }
                emit changed();
                emit runFinished(last_result_);
            }, Qt::QueuedConnection);
        });
    };
    if (c_.ensure_network) c_.ensure_network(go);
    else go(true);
    return out;
}

QVariantMap SyncBridge::approve(qlonglong planId) {
    if (planId == 0) return error_map("plan_not_approvable");   // the offline preview can never be approved
    const auto plan = c_.store->sync_plan(planId);
    if (!plan) return error_map("not_found");
    const auto pair = c_.store->sync_pair(plan->pair_id);
    if (!pair) return error_map("not_found");
    if (!pbc::pair_matches_account(*c_.store, *pair)) return error_map("account_changed");
    if (busy_ || pair->state == "comparing" || pair->state == "running") return error_map("sync_busy");
    if (const pbc::Approval a = pbc::can_approve(*plan, c_.clock->now()); !a.ok) return error_map(a.error_key);
    const qlonglong pair_id = pair->id;
    const int n_down = plan->totals.n_download, n_up = plan->totals.n_upload;
    busy_ = true;
    busy_pair_ = pair_id;
    stage_ = n_down > 0 || n_up == 0 ? QStringLiteral("downloading") : QStringLiteral("uploading");
    progress_text_ = n_down > 0 ? bridge_tr("Downloading %1 of %2").arg(1).arg(n_down) : n_up > 0 ? bridge_tr("Uploading %1 of %2").arg(1).arg(n_up) : QString();
    percent_ = 0;
    last_progress_ms_ = -1;
    last_progress_percent_ = -100;
    cancel_.flag = false;
    emit changed();
    auto go = [this, planId, pair_id, n_down, n_up](bool online) {
        if (!online) {
            busy_ = false;
            busy_pair_ = 0;
            stage_ = QStringLiteral("idle");
            emit changed();
            emit runFinished(error_result(pair_id, QStringLiteral("no_network")));
            return;
        }
        c_.worker->post([this, planId, pair_id, n_down, n_up] {
            pbc::Downloader dl(*c_.store, *c_.http, *c_.storage, *c_.clock, c_.download_root, c_.wifi_up, c_.downloader_options);
            pbc::RunnerOptions ro;
            ro.hook = c_.runner_hook;
            ro.on_progress = [this, n_down, n_up](const pbc::SyncResult& r) {
                QMetaObject::invokeMethod(this, [this, r, n_down, n_up] { note_progress(r, n_down, n_up); }, Qt::QueuedConnection);
            };
            pbc::SyncRunner runner(*c_.store, *c_.http, dl, *c_.storage, *c_.clock, c_.rules, ro);
            pbc::SenderOptions so;   // the uploads of the plan go through the queue of feature 002, as one group sent on its own
            so.wifi_up = c_.wifi_up;
            so.hook = c_.sender_hook;
            pbc::QueueSender sender(*c_.store, *c_.http, *c_.clock, random_, c_.rules, c_.download_root, so);
            runner.attach_queue(&sender);
            const pbc::Result<pbc::SyncResult> r = runner.run(planId, cancel_);
            QMetaObject::invokeMethod(this, [this, pair_id, r] {
                busy_ = false;
                busy_pair_ = 0;
                stage_ = QStringLiteral("idle");
                progress_text_.clear();
                reload();
                if (r.ok()) {
                    const auto pair = c_.store->sync_pair(pair_id);
                    last_result_ = result_map(pair_id, pair ? pair->last_result_json : std::string("{}"), pair ? pair->last_sync_at : std::nullopt);
                } else {
                    last_result_ = error_result(pair_id, q(r.error.key));
                }
                emit changed();
                emit runFinished(last_result_);
                evaluate_auto_sync();   // a change the user made during the run: the single follow-up
            }, Qt::QueuedConnection);
        });
    };
    if (c_.ensure_network) c_.ensure_network(go);
    else go(true);
    QVariantMap out;
    out["ok"] = true;
    return out;
}
