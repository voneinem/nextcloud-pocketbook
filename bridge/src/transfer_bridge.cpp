#include "transfer_bridge.h"

#include <QMetaObject>

#include "format_util.h"
#include "pbc/entry_state.h"
#include "pbc/local_files.h"
#include "pbc/remote_path.h"
#include "worker.h"

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }

QString state_text(pbc::JobState s) {
    switch (s) {
        case pbc::JobState::Running: return QStringLiteral("running");
        case pbc::JobState::Waiting: return QStringLiteral("waiting");
        case pbc::JobState::Paused: return QStringLiteral("paused");
        case pbc::JobState::Done: return QStringLiteral("done");
        case pbc::JobState::Cancelled: return QStringLiteral("cancelled");
        case pbc::JobState::Blocked: return QStringLiteral("blocked");
        default: return QStringLiteral("failed");
    }
}

int64_t warn_bytes(pbc::Store& store) {
    int mib = std::atoi(store.setting("large_file_warn_mib").c_str());
    if (mib < 1 || mib > 4095) mib = 100;
    return static_cast<int64_t>(mib) * 1024 * 1024;
}

}  // namespace

TransferBridge::TransferBridge(Config config, QObject* parent) : QObject(parent), c_(std::move(config)) {
    // at start, without any network: jobs the last run left behind become paused, and partial files nobody knows are removed
    pbc::Downloader dl(*c_.store, *c_.http, *c_.storage, *c_.clock, c_.download_root, c_.wifi_up, c_.options);
    dl.recover();
}

TransferBridge::~TransferBridge() {
    cancel_.flag = true;
    update_cancel_.flag = true;
    confirmUpdate(false);   // a worker that waits for the answer must not wait for ever
    if (c_.worker) c_.worker->wait_idle();
}

QString TransferBridge::jobDoneText() const { return human_size(done_); }
QString TransferBridge::jobTotalText() const { return total_ > 0 ? human_size(total_) : QString(); }
int TransferBridge::jobPercent() const {
    if (state_ == QLatin1String("done")) return 100;
    return total_ > 0 ? static_cast<int>(std::min<qint64>(100, done_ * 100 / total_)) : 0;
}

QVariantList TransferBridge::pausedDownloads() const {
    QVariantList out;
    for (const pbc::DownloadJournalRow& j : c_.store->journal()) {
        if (j.state != "paused") continue;
        QVariantMap m;
        m["id"] = static_cast<qlonglong>(j.id);
        m["remotePath"] = q(j.remote_path);
        m["name"] = display_safe(j.remote_path.substr(j.remote_path.rfind('/') + 1));
        m["doneText"] = human_size(j.received);
        out.append(m);
    }
    return out;
}

QVariantMap TransferBridge::details(const QString& remotePath) const {
    const std::string p = pbc::normalize_remote_path(remotePath.toStdString());
    QVariantMap m;
    m["remotePath"] = q(p);
    const auto entry = c_.store->entry(p);
    const auto local = c_.store->local_file(p);
    m["name"] = display_safe(entry ? entry->name : p.substr(p.rfind('/') + 1));
    m["kind"] = entry && entry->kind == pbc::Kind::Folder ? QStringLiteral("folder") : QStringLiteral("file");
    m["sizeText"] = entry && entry->size ? human_size(*entry->size) : (local ? human_size(local->size) : QString());
    m["mime"] = entry ? display_safe(entry->mime, 100) : QString();
    m["dateText"] = entry && entry->mtime && c_.format_date_time ? c_.format_date_time(*entry->mtime) : QString();
    m["available"] = entry ? entry->availability == pbc::Availability::Ok : local.has_value();
    m["reason"] = entry ? q(entry->reason) : QString();
    bool on_reader = false, can_open = false;
    if (local) {
        const auto abs = pbc::resolve_local_file(c_.download_root, local->local_path);
        can_open = abs.ok();
        on_reader = abs.ok();
    }
    m["onReader"] = on_reader;
    m["canOpen"] = can_open;
    m["localPath"] = local ? q(local->local_path) : QString();
    m["serverNewer"] = local && entry && !entry->etag.empty() && entry->etag != local->remote_etag && entry->etag != local->seen_etag;
    m["serverMissing"] = local && local->server_missing;
    {   // the state word of the list, and whether the file is kept offline
        const pbc::FileStat st = local ? pbc::stat_file(c_.download_root + "/" + local->local_path) : pbc::FileStat();
        m["state"] = q(pbc::state_key(pbc::compute_state(entry ? &*entry : nullptr, local ? &*local : nullptr, st)));
        m["kept"] = c_.store->in_scope(p);
        m["keptDirect"] = c_.store->is_marked(p);
    }
    if (local) {   // the copy as it is on the card now (it may have been changed there)
        const pbc::FileStat now = pbc::stat_file(c_.download_root + "/" + local->local_path);
        m["localSizeText"] = now.exists ? human_size(now.size) : QString();
        m["localDateText"] = now.exists && c_.format_date_time ? c_.format_date_time(now.mtime) : QString();
        m["localChanged"] = now.exists && pbc::local_copy_changed(*local, now);
    }
    return m;
}

QVariantMap TransferBridge::preflight(const QString& remotePath) const {
    QVariantMap m;
    const auto entry = c_.store->entry(pbc::normalize_remote_path(remotePath.toStdString()));
    const pbc::Preflight pf = pbc::preflight_download(*c_.storage, c_.download_root, entry ? entry->size : std::nullopt, warn_bytes(*c_.store));
    m["verdict"] = pf.verdict == pbc::Preflight::Verdict::Ok ? QStringLiteral("ok")
                 : pf.verdict == pbc::Preflight::Verdict::NeedsConfirmation ? QStringLiteral("confirm") : QStringLiteral("refused");
    m["errorKey"] = q(pf.error_key);
    m["sizeText"] = human_size(pf.size);
    m["freeText"] = human_size(pf.free);
    m["neededText"] = human_size(pf.needed);
    return m;
}

void TransferBridge::fail_now(const QString& key) {
    busy_ = false;
    state_ = QStringLiteral("failed");
    error_ = key;
    emit changed();
    emit jobFinished(state_);
}

void TransferBridge::download(const QString& remotePath, bool confirmed) {
    if (busy_) return;
    const std::string p = pbc::normalize_remote_path(remotePath.toStdString());
    const auto entry = c_.store->entry(p);
    if (!entry || entry->kind != pbc::Kind::File || entry->availability != pbc::Availability::Ok) return;
    path_ = q(p);
    const pbc::Preflight pf = pbc::preflight_download(*c_.storage, c_.download_root, entry->size, warn_bytes(*c_.store));
    if (pf.verdict == pbc::Preflight::Verdict::Refused) {
        done_ = total_ = 0;
        fail_now(q(pf.error_key));
        return;
    }
    if (pf.verdict == pbc::Preflight::Verdict::NeedsConfirmation && !confirmed) return;
    start(q(p), pbc::ConflictPolicy::Ask);
}

void TransferBridge::start(const QString& remotePath, pbc::ConflictPolicy policy) {
    const std::string p = remotePath.toStdString();
    const auto entry = c_.store->entry(p);
    if (!entry) return;
    pbc::DownloadRequest req;
    req.remote_path = p;
    req.size = entry->size;
    req.etag = entry->etag;
    req.checksum = entry->checksum;
    req.remote_mtime = entry->mtime;
    req.policy = policy;
    path_ = remotePath;
    run_job([req](pbc::Downloader& dl, pbc::Cancel& cancel, const std::function<void(const pbc::JobProgress&)>& progress) { return dl.run(req, cancel, progress); });
}

void TransferBridge::resume(qlonglong jobId) {
    if (busy_) return;
    for (const pbc::DownloadJournalRow& j : c_.store->journal())
        if (j.id == jobId) path_ = q(j.remote_path);
    run_job([jobId](pbc::Downloader& dl, pbc::Cancel& cancel, const std::function<void(const pbc::JobProgress&)>& progress) { return dl.resume(jobId, cancel, progress); });
}

void TransferBridge::run_job(std::function<pbc::DownloadOutcome(pbc::Downloader&, pbc::Cancel&, const std::function<void(const pbc::JobProgress&)>&)> work) {
    busy_ = true;
    state_ = QStringLiteral("running");
    error_.clear();
    attempt_ = 0;
    done_ = total_ = 0;
    cancel_.flag = false;
    emit changed();
    c_.ensure_network([this, work = std::move(work)](bool online) {
        if (!online) {
            done_ = total_ = 0;
            fail_now(QStringLiteral("no_network"));
            return;
        }
        c_.worker->post([this, work] {
            pbc::Downloader dl(*c_.store, *c_.http, *c_.storage, *c_.clock, c_.download_root, c_.wifi_up, c_.options);
            const pbc::DownloadOutcome out = work(dl, cancel_, [this](const pbc::JobProgress& p) {
                QMetaObject::invokeMethod(this, [this, p] {
                    state_ = state_text(p.state);
                    done_ = p.done;
                    total_ = p.total;
                    attempt_ = p.attempt;
                    emit changed();
                }, Qt::QueuedConnection);
            });
            QMetaObject::invokeMethod(this, [this, out] {
                busy_ = false;
                state_ = state_text(out.state);
                error_ = q(out.error_key);
                if (out.state == pbc::JobState::Done) done_ = total_ = std::max<qint64>(done_, total_);
                emit changed();
                emit jobFinished(state_);
            }, Qt::QueuedConnection);
        });
    });
}

void TransferBridge::cancel() { cancel_.flag = true; }

void TransferBridge::resolveConflict(const QString& remotePath, const QString& choice) {
    if (busy_) return;
    const std::string p = pbc::normalize_remote_path(remotePath.toStdString());
    if (choice == QLatin1String("keep_mine")) {
        const auto entry = c_.store->entry(p);
        pbc::keep_mine(*c_.store, p, entry ? entry->etag : std::string());
        emit changed();
    } else if (choice == QLatin1String("keep_both")) {
        start(q(p), pbc::ConflictPolicy::KeepBoth);
    } else if (choice == QLatin1String("replace")) {
        start(q(p), pbc::ConflictPolicy::Replace);
    }
}

void TransferBridge::open(const QString& remotePath) {
    const auto local = c_.store->local_file(pbc::normalize_remote_path(remotePath.toStdString()));
    if (!local) return;
    const auto abs = pbc::resolve_local_file(c_.download_root, local->local_path);
    if (!abs.ok() || !c_.open_file) return;
    c_.open_file(q(abs.value));
}

QString TransferBridge::lastUpdateText() const {
    const auto t = c_.store->sync_state().last_finished;
    return t && c_.format_date_time ? c_.format_date_time(*t) : QString();
}

QString TransferBridge::lastUpdateOutcome() const { return q(c_.store->sync_state().outcome); }

void TransferBridge::setKept(const QString& remotePath, const QString& kind, bool on) {
    if (kind != QLatin1String("folder") && kind != QLatin1String("file")) return;
    const pbc::Kind k = kind == QLatin1String("folder") ? pbc::Kind::Folder : pbc::Kind::File;
    const std::string p = pbc::normalize_remote_path(remotePath.toStdString());
    if (p.empty() || p == "/" ) return;
    c_.store->set_mark(p, k, on);
    emit changed();
}

namespace {

QVariantList path_list(const std::vector<std::string>& v) {
    QVariantList out;
    for (const std::string& s : v) out.append(q(s));
    return out;
}

QVariantMap report_map(const pbc::UpdateReport& r) {
    QVariantMap m;
    m["folders"] = r.folders;
    m["files"] = r.files;
    m["added"] = r.added;
    m["updated"] = r.updated;
    m["skipped"] = r.skipped;
    m["needDecision"] = r.need_decision;
    m["removed"] = r.removed;
    m["failed"] = r.failed;
    m["uncheckedFolders"] = r.unchecked_folders;
    m["stoppedBy"] = q(r.stopped_by_key);
    m["decisionPaths"] = path_list(r.decision_paths);
    m["removedPaths"] = path_list(r.removed_paths);
    return m;
}

}  // namespace

void TransferBridge::update() {
    if (busy_ || update_busy_) return;
    update_busy_ = true;
    update_stage_ = QStringLiteral("checking");
    update_done_ = update_total_ = 0;
    update_report_.clear();
    update_cancel_.flag = false;
    {
        std::lock_guard<std::mutex> l(confirm_mu_);
        confirm_answer_ = 0;
    }
    emit changed();
    c_.ensure_network([this](bool online) {
        if (!online) {
            pbc::UpdateReport rep;
            rep.stopped_by_key = "no_network";
            finish_update(rep);
            return;
        }
        c_.worker->post([this] {
            pbc::Downloader dl(*c_.store, *c_.http, *c_.storage, *c_.clock, c_.download_root, c_.wifi_up, c_.options);
            pbc::Updater up(*c_.store, *c_.http, dl, *c_.storage, *c_.clock);
            pbc::UpdatePlan plan = up.plan(update_cancel_);
            bool go = plan.report.stopped_by_key.empty() && !plan.downloads.empty();
            if (go) {
                const pbc::Preflight pf = pbc::preflight_download(*c_.storage, c_.download_root, plan.bytes, warn_bytes(*c_.store));
                if (pf.verdict == pbc::Preflight::Verdict::Refused) {   // what the storage cannot hold is not even offered
                    plan.report.stopped_by_key = pf.error_key;
                    go = false;
                } else if (pf.verdict != pbc::Preflight::Verdict::Ok) {
                    const QString bytes = human_size(plan.bytes), freeText = human_size(pf.free);
                    const int total = static_cast<int>(plan.downloads.size());
                    QMetaObject::invokeMethod(this, [this, bytes, freeText, total] {
                        update_total_ = total;
                        update_stage_ = QStringLiteral("confirm");
                        emit changed();
                        emit updateNeedsConfirm(bytes, freeText);
                    }, Qt::QueuedConnection);
                    std::unique_lock<std::mutex> l(confirm_mu_);
                    confirm_cv_.wait(l, [this] { return confirm_answer_ != 0 || update_cancel_.flag.load(); });
                    go = confirm_answer_ == 1 && !update_cancel_.flag.load();
                    if (!go) plan.report.stopped_by_key = "cancelled";
                } else {
                    const int total = static_cast<int>(plan.downloads.size());
                    QMetaObject::invokeMethod(this, [this, total] { update_total_ = total; }, Qt::QueuedConnection);
                }
            }
            if (go) {
                QMetaObject::invokeMethod(this, [this] {
                    update_stage_ = QStringLiteral("downloading");
                    emit changed();
                }, Qt::QueuedConnection);
            }
            // also when nothing is fetched (a stopped, declined or empty plan): the result is recorded for the home page
            const pbc::UpdateReport rep = up.execute(plan, update_cancel_, [this](const pbc::UpdateReport& r) {
                const int done = r.added + r.updated + r.failed + r.need_decision;
                QMetaObject::invokeMethod(this, [this, done] {
                    update_done_ = done;
                    emit changed();
                }, Qt::QueuedConnection);
            });
            QMetaObject::invokeMethod(this, [this, rep] { finish_update(rep); }, Qt::QueuedConnection);
        });
    });
}

void TransferBridge::finish_update(const pbc::UpdateReport& rep) {
    update_busy_ = false;
    update_stage_ = QStringLiteral("done");
    update_report_ = report_map(rep);
    emit changed();
    emit updateFinished();
}

void TransferBridge::cancelUpdate() {
    update_cancel_.flag = true;
    confirm_cv_.notify_all();
}

void TransferBridge::confirmUpdate(bool yes) {
    {
        std::lock_guard<std::mutex> l(confirm_mu_);
        confirm_answer_ = yes ? 1 : 2;
    }
    confirm_cv_.notify_all();
}
