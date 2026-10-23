#include "queue_bridge.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>

#include <algorithm>
#include <chrono>

#include "format_util.h"
#include "pbc/auth_guard.h"
#include "pbc/log.h"
#include "pbc/remote_path.h"
#include "pbc/upload_source.h"
#include "worker.h"

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }

std::string base_name(const std::string& path) { return path.substr(path.rfind('/') + 1); }

QString kind_text(pbc::ItemKind k) { return QString::fromLatin1(pbc::item_kind_text(k)); }

// the server folders an item is about: where it goes, and where its source lies
std::vector<std::string> folders_of(const std::optional<pbc::QueueItem>& item) {
    std::vector<std::string> out;
    if (!item) return out;
    if (!item->dst_parent.empty()) out.push_back(item->dst_parent);
    if (!item->src_remote.empty()) out.push_back(pbc::parent_of(item->src_remote));
    return out;
}

std::vector<std::string> parents_of(const QStringList& remote_paths) {
    std::vector<std::string> out;
    for (const QString& p : remote_paths) out.push_back(pbc::parent_of(p.toStdString()));
    return out;
}

// Why a run stopped, as the page words it (FR-013): the connection is gone, the server asks for a pause, or the account needs a new sign-in.
QString notice_for(const std::string& key) {
    if (key == "no_connection" || key == "connection_lost" || key == "no_network" || key == "server_unreachable" || key == "timeout") return QStringLiteral("offline");
    if (key == "rate_limited") return QStringLiteral("rate_limited");
    if (key == "maintenance") return QStringLiteral("maintenance");
    if (key == "auth_failed") return QStringLiteral("signin");
    return QString();
}

QString check_text(pbc::Found f) {
    switch (f) {
        case pbc::Found::Arrived: return QStringLiteral("arrived");
        case pbc::Found::NotThere: return QStringLiteral("not_there");
        case pbc::Found::SomethingElse: return QStringLiteral("something_else");
        case pbc::Found::Unclear: break;
    }
    return QStringLiteral("unclear");
}

}  // namespace

QueueBridge::QueueBridge(Config config, QObject* parent)
    : QObject(parent), c_(std::move(config)), actions_(*c_.store, c_.rules), scheduler_(c_.auto_sync_quiet_ms) {
    mono_.start();
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, [this] { on_timer(); });
    c_.store->queue_recover();   // at start, without any network: rows that were being sent go back to the queue or become "outcome unknown"
    reload();
}

QueueBridge::~QueueBridge() {
    if (c_.worker) c_.worker->wait_idle();   // no job may call back into a bridge that is gone
}

void QueueBridge::setPageSize(int n) {
    n = std::max(1, std::min(n, 100));
    if (n == page_size_) return;
    const int first = page_ * page_size_;
    page_size_ = n;
    page_ = first / page_size_;
    reload();
    emit changed();
}

QVariantMap QueueBridge::row_map(const pbc::QueueItem& item, const std::optional<pbc::AccountRow>& account) const {
    QVariantMap m;
    m["id"] = static_cast<qlonglong>(item.id);
    m["groupId"] = item.group_id ? QVariant(static_cast<qlonglong>(*item.group_id)) : QVariant(0);
    m["kind"] = kind_text(item.kind);
    m["entryKind"] = item.entry_kind == pbc::Kind::Folder ? QStringLiteral("folder") : QStringLiteral("file");
    const bool deleting = item.kind == pbc::ItemKind::Delete;
    m["name"] = display_safe(deleting ? base_name(item.src_remote) : item.dst_name);
    m["targetText"] = display_safe(deleting ? pbc::parent_of(item.src_remote) : item.dst_parent, 120);
    m["sizeText"] = item.kind == pbc::ItemKind::Upload && item.src_size ? human_size(*item.src_size) : QString();
    m["reason"] = q(item.reason);
    m["reasonText"] = QString();   // the page translates the key
    m["progressText"] = item.state == pbc::ItemState::Sending && item.id == run_item_ ? run_progress_text_ : QString();
    QString state = QString::fromLatin1(pbc::item_state_text(item.state));
    QString waiting_for;
    if (account && (item.server != account->base_url || item.user_id != account->user_id)) {
        state = QStringLiteral("other_account");
    } else if (item.state == pbc::ItemState::Queued && item.depends_on) {
        const auto dep = c_.store->queue_item(*item.depends_on);
        if (dep && dep->state != pbc::ItemState::Done) {
            state = QStringLiteral("waiting");
            waiting_for = display_safe(dep->dst_name);
        }
    }
    m["state"] = state;
    m["waitingFor"] = waiting_for;
    // an upload whose file on the reader is not the one that was picked any more (a changed size or time): what is sent is the file as it is then
    bool source_changed = false;
    if (item.kind == pbc::ItemKind::Upload && item.state != pbc::ItemState::Done && item.src_size && item.src_mtime) {
        const auto now = pbc::inspect_source(item.src_local, c_.rules);
        source_changed = now.ok() && (now.value.size != *item.src_size || now.value.mtime != *item.src_mtime);
    }
    m["sourceChanged"] = source_changed;
    // an item that a pair's sync created: the server folder of the pair, so that the Queue says where it came from
    m["syncedBy"] = QString();
    if (item.pair_id) {
        if (const auto pair = c_.store->sync_pair(*item.pair_id)) m["syncedBy"] = display_safe(base_name(pair->remote_folder.empty() ? std::string("/") : pair->remote_folder), 60);
        else m["syncedBy"] = QStringLiteral("-");
    }
    m["canRetry"] = item.state == pbc::ItemState::Failed;
    m["canResolve"] = item.state == pbc::ItemState::Conflict || item.state == pbc::ItemState::Unknown ||
                      (item.state == pbc::ItemState::Failed && (item.reason == "name_forbidden" || item.reason == "name_exists" || item.reason == "target_gone"));
    return m;
}

void QueueBridge::reload() {
    const auto account = c_.store->account();
    const pbc::QueuePage p = c_.store->queue_page(page_ * page_size_, page_size_);
    total_ = p.total;
    page_count_ = std::max(1, (total_ + page_size_ - 1) / page_size_);
    if (page_ >= page_count_) {   // the queue got shorter
        page_ = page_count_ - 1;
        reload();
        return;
    }
    open_count_ = c_.store->queue_count_open();
    items_.clear();
    for (const pbc::QueueItem& item : p.rows) items_.append(row_map(item, account));
}

QVariantMap QueueBridge::ask_map(const pbc::NameAsk& ask) {
    const QString id = QStringLiteral("ask-%1").arg(++ask_counter_);
    asks_[id] = ask;
    ask_order_.push_back(id);
    while (ask_order_.size() > 400) {   // only the latest questions are kept
        asks_.erase(ask_order_.front());
        ask_order_.pop_front();
    }
    QVariantMap m;
    m["askId"] = id;
    m["name"] = display_safe(ask.name, 120);
    m["targetFolder"] = display_safe(ask.target_folder, 120);
    m["existingIsFolder"] = ask.existing_is_folder;
    m["inQueue"] = ask.in_queue;
    m["replacePossible"] = ask.replace_possible;
    m["refreshNeeded"] = ask.refresh_needed;
    m["suggested"] = display_safe(ask.suggested, 120);
    m["existingSizeText"] = ask.existing_size ? human_size(*ask.existing_size) : QString();
    m["existingDateText"] = ask.existing_mtime && c_.format_date_time ? c_.format_date_time(*ask.existing_mtime) : QString();
    return m;
}

QVariantMap QueueBridge::result_map(const pbc::AddResult& r) {
    QVariantMap m;
    m["queued"] = static_cast<int>(r.queued.size());
    QVariantList ids;
    for (int64_t id : r.queued) ids.append(static_cast<qlonglong>(id));
    m["ids"] = ids;
    m["errorKey"] = r.error.ok() ? QString() : q(r.error.key);
    QVariantList refused;
    for (const pbc::PickRefusal& x : r.refused) {
        QVariantMap one;
        one["name"] = display_safe(base_name(x.path), 120);   // a name to show, never a path
        one["key"] = q(x.key);
        refused.append(one);
    }
    m["refused"] = refused;
    QVariantList asks;
    for (const pbc::NameAsk& a : r.asks) asks.append(ask_map(a));
    m["asks"] = asks;
    return m;
}

QVariantMap QueueBridge::addUploads(const QString& targetFolder, const QStringList& paths) {
    std::vector<std::string> picked;
    for (const QString& p : paths) picked.push_back(p.toStdString());
    const pbc::AddResult r = actions_.add_uploads(targetFolder.toStdString(), picked);
    reload();
    emit changed();
    if (!r.queued.empty()) note_action({targetFolder.toStdString()});
    return result_map(r);
}

QVariantMap QueueBridge::addFolderPick(const QString& targetFolder, const QStringList& folderPaths) {
    std::vector<std::string> picked;
    for (const QString& p : folderPaths) picked.push_back(p.toStdString());
    pbc::FolderPick pick = actions_.count_folder_pick(targetFolder.toStdString(), picked);
    QVariantMap m;
    m["errorKey"] = pick.error.ok() ? QString() : q(pick.error.key);
    m["folders"] = pick.folders_count;
    m["files"] = pick.files;
    m["items"] = pick.items();
    m["bytes"] = static_cast<qlonglong>(pick.bytes);
    m["bytesText"] = human_size(pick.bytes);
    m["skipped"] = pick.skipped;
    m["overLimit"] = pick.over_limit;
    m["headroom"] = actions_.headroom();
    QVariantList refused;
    for (const pbc::PickRefusal& x : pick.refused) {
        QVariantMap one;
        one["name"] = display_safe(base_name(x.path), 120);
        one["key"] = q(x.key);
        refused.append(one);
    }
    m["refused"] = refused;
    QString token;
    if (pick.error.ok()) {
        token = QStringLiteral("pick-%1").arg(++pick_counter_);
        picks_[token] = std::move(pick);
        pick_order_.push_back(token);
        while (pick_order_.size() > 4) {   // only the last few counted picks are kept
            picks_.erase(pick_order_.front());
            pick_order_.pop_front();
        }
    }
    m["token"] = token;
    return m;
}

QVariantMap QueueBridge::confirmFolderPick(const QString& token, const QString& policy) {
    const auto it = picks_.find(token);
    if (it == picks_.end()) {
        QVariantMap none = result_map(pbc::AddResult());
        return none;   // an unknown or used token queues nothing
    }
    const pbc::FolderPick pick = std::move(it->second);
    picks_.erase(it);
    pick_order_.erase(std::remove(pick_order_.begin(), pick_order_.end(), token), pick_order_.end());
    pbc::PickPolicy how = pbc::PickPolicy::Ask;
    if (policy == QLatin1String("keep_both")) how = pbc::PickPolicy::KeepBoth;
    else if (policy == QLatin1String("replace")) how = pbc::PickPolicy::Replace;
    else if (policy == QLatin1String("skip")) how = pbc::PickPolicy::Skip;
    const pbc::AddResult r = actions_.confirm_folder_pick(pick, how);
    reload();
    emit changed();
    if (!r.queued.empty()) note_action({pick.target});
    return result_map(r);
}

void QueueBridge::remove(int id) {
    const auto item = c_.store->queue_item(id);
    if (!item) return;
    if (!item->upload_id.empty() && item->state != pbc::ItemState::Done && item->state != pbc::ItemState::Sending) {
        // the upload folder on the server goes first (best effort), which needs the worker
        const std::vector<std::string> folders = folders_of(item);
        c_.worker->post([this, id, folders] {
            make_sender({})->remove_item(id);
            QMetaObject::invokeMethod(this, [this, folders] {
                reload();
                emit changed();
                note_action(folders);
            }, Qt::QueuedConnection);
        });
        return;
    }
    actions_.remove_item(id);   // an item that is being sent is refused by the store
    reload();
    emit changed();
    note_action(folders_of(item));
}

void QueueBridge::leaveScreen() {
    c_.store->queue_clear_done();
    reload();
    emit changed();
}

void QueueBridge::refresh() {
    reload();
    emit changed();
}

void QueueBridge::nextPage() { goToPage(page_ + 1); }
void QueueBridge::previousPage() { goToPage(page_ - 1); }

void QueueBridge::goToPage(int page) {
    page = std::max(0, std::min(page, page_count_ - 1));
    if (page == page_) return;
    page_ = page;
    reload();
    emit changed();
}


// ---- sending ----

std::unique_ptr<pbc::QueueSender> QueueBridge::make_sender(const std::function<void(const pbc::SendProgress&)>& on_progress) {
    pbc::SenderOptions options;
    options.wifi_up = c_.wifi_up;
    options.on_progress = on_progress;
    return std::make_unique<pbc::QueueSender>(*c_.store, *c_.http, *c_.clock, random_, c_.rules, c_.download_root, options);
}

void QueueBridge::send() { start_run(true, 0); }

void QueueBridge::cancelSend() { cancel_.flag = true; }

void QueueBridge::retry(int id) {
    auto item = c_.store->queue_item(id);
    if (!item || item->state != pbc::ItemState::Failed) return;
    item->state = pbc::ItemState::Queued;
    item->step.clear();
    item->reason.clear();
    if (!c_.store->queue_update(*item).ok()) return;
    reload();
    emit changed();
    start_run(true, id);   // the user's tap on this item: it is sent alone
}

void QueueBridge::start_run(bool ensure_network, int64_t only) {
    if (running_) return;   // a running send already takes what is queued
    running_ = true;
    run_stage_ = QStringLiteral("preparing");
    run_progress_text_.clear();
    run_percent_ = 0;
    run_item_ = 0;
    waiting_notice_.clear();
    cancel_.flag = false;
    scheduler_.send_started(now_ms());
    timer_.stop();
    emit changed();
    const auto go = [this, only](bool online) {
        if (!online) {   // the tap could not bring Wi-Fi up: nothing is sent, the queue waits
            pbc::SendReport rep;
            rep.stopped_by_key = "no_connection";
            finish_run(rep);
            return;
        }
        struct Throttle {
            std::chrono::steady_clock::time_point last;
            int percent = -1;
            int64_t item = 0;
            std::string step;
        };
        auto throttle = std::make_shared<Throttle>();
        c_.worker->post([this, only, throttle] {
            const auto sender = make_sender([this, throttle](const pbc::SendProgress& p) {
                // coarse by design (the e-ink budget): at a new item or step, every progress_interval_ms, or every progress_step_percent
                const int percent = p.total > 0 ? static_cast<int>(p.done * 100 / p.total) : 0;
                const auto now = std::chrono::steady_clock::now();
                const bool moved = p.item_id != throttle->item || p.step != throttle->step;
                const bool due = moved || now - throttle->last >= std::chrono::milliseconds(c_.progress_interval_ms) ||
                                 percent >= throttle->percent + c_.progress_step_percent || percent < throttle->percent;
                if (!due) return;
                throttle->last = now;
                throttle->percent = percent;
                throttle->item = p.item_id;
                throttle->step = p.step;
                QMetaObject::invokeMethod(this, [this, p, percent] {
                    run_stage_ = p.step == "prepare" ? QStringLiteral("preparing") : QStringLiteral("sending");
                    run_item_ = p.item_id;
                    run_percent_ = percent;
                    run_progress_text_ = p.total > 0 ? human_size(p.done) + QStringLiteral(" / ") + human_size(p.total) : QString();
                    emit changed();
                }, Qt::QueuedConnection);
            });
            const pbc::SendReport rep = sender->run(cancel_, only);
            QMetaObject::invokeMethod(this, [this, rep] { finish_run(rep); }, Qt::QueuedConnection);
        });
    };
    if (ensure_network && c_.ensure_network) c_.ensure_network(go);
    else go(true);
}

void QueueBridge::finish_run(const pbc::SendReport& rep) {
    running_ = false;
    run_stage_ = QStringLiteral("done");
    run_item_ = 0;
    waiting_notice_ = notice_for(rep.stopped_by_key);
    if (rep.stopped_by_key == "auth_failed") pbc::AuthGuard(*c_.store).record(pbc::make_error(pbc::ErrorClass::Auth, "auth_failed"));
    reload();
    emit changed();
    QVariantMap m;
    m["done"] = rep.done;
    m["failed"] = rep.failed;
    m["conflict"] = rep.conflict;
    m["unknown"] = rep.unknown;
    m["left"] = rep.left;
    m["stoppedBy"] = q(rep.stopped_by_key);
    emit sendFinished(m);
    if (scheduler_.armed()) evaluate_auto_sync();   // a user action during the send: the single follow-up
}

void QueueBridge::sendAgain(int id) {
    const std::vector<std::string> folders = folders_of(c_.store->queue_item(id));
    if (!make_sender({})->send_again(id).ok()) return;
    reload();
    emit changed();
    note_action(folders);
}

void QueueBridge::checkUnknown(int id) {
    check_result_.clear();
    const auto go = [this, id](bool online) {
        if (!online) {
            check_result_ = QStringLiteral("unclear");
            emit changed();
            emit checkFinished(id, check_result_);
            return;
        }
        c_.worker->post([this, id] {
            const QString result = check_text(make_sender({})->check_unknown(id));
            QMetaObject::invokeMethod(this, [this, id, result] {
                check_result_ = result;
                emit changed();
                emit checkFinished(id, result);
            }, Qt::QueuedConnection);
        });
    };
    if (c_.ensure_network) c_.ensure_network(go);
    else go(true);
}

void QueueBridge::markDone(int id) {
    const std::vector<std::string> folders = folders_of(c_.store->queue_item(id));
    c_.worker->post([this, id, folders] {
        make_sender({})->mark_unknown_done(id);
        QMetaObject::invokeMethod(this, [this, folders] {
            reload();
            emit changed();
            note_action(folders);
        }, Qt::QueuedConnection);
    });
}

bool QueueBridge::autoSync() const { return c_.store->setting("auto_sync") != "0"; }

void QueueBridge::setAutoSync(bool on) {
    if (on == autoSync()) return;
    c_.store->set_setting("auto_sync", on ? "1" : "0");
    if (!on) {
        timer_.stop();
        evaluate_auto_sync();   // disarms
    }
    emit changed();
}

QString QueueBridge::lastRunText() const {
    const std::string at = c_.store->setting("last_send_at");
    if (at.empty() || !c_.format_date_time) return QString();
    return c_.format_date_time(static_cast<qint64>(std::atoll(at.c_str())));
}

QVariantMap QueueBridge::lastRunCounts() const {
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(c_.store->setting("last_send_counts")));
    return doc.isObject() ? doc.object().toVariantMap() : QVariantMap();
}

// ---- Auto Sync (research R-14): one single-shot timer, armed only by the user's own actions ----

void QueueBridge::note_action(const std::vector<std::string>& remote_folders) {
    if (autoSync()) pbc::log::event("autosync.arm", {});
    scheduler_.note_action(now_ms());
    evaluate_auto_sync();
    if (c_.on_user_action)
        for (const std::string& folder : remote_folders)   // the pairs that hold these folders may compare (they only store a plan)
            if (!folder.empty()) c_.on_user_action(folder);
}

void QueueBridge::on_timer() { evaluate_auto_sync(); }

void QueueBridge::evaluate_auto_sync() {
    const auto account = c_.store->account();
    pbc::AutoSyncInputs in;
    in.enabled = autoSync();
    in.has_account = account.has_value();
    in.send_running = running_;
    in.sendable_items = account && c_.store->queue_count_sendable(account->base_url, account->user_id) > 0;
    in.wifi_up = !offline();
    const bool was_armed = scheduler_.armed();
    const pbc::AutoSyncDecision d = scheduler_.next(now_ms(), in);
    if (d.step == pbc::AutoSyncStep::Wait) {
        timer_.start(std::max(1, d.wait_ms));
        return;
    }
    timer_.stop();
    if (d.step == pbc::AutoSyncStep::Start) {
        pbc::log::event("autosync.start", {});
        start_run(false, 0);   // Auto Sync never switches Wi-Fi on
        return;
    }
    if (was_armed && !scheduler_.armed()) {
        pbc::log::event("autosync.drop", {});
        if (in.enabled && in.has_account && in.sendable_items && !in.wifi_up) {   // the quiet period ended without a connection: the queue waits
            waiting_notice_ = QStringLiteral("offline");
            emit changed();
        }
    }
}


// ---- the name question and the choices for held items (US3) ----

QVariantMap QueueBridge::answerCollision(const QString& askId, const QString& choice, const QString& newName) {
    const auto it = asks_.find(askId);
    if (it == asks_.end()) return result_map(pbc::AddResult());   // an unknown or answered question queues nothing
    const pbc::NameAsk ask = it->second;
    pbc::AskChoice how = pbc::AskChoice::Skip;
    if (choice == QLatin1String("keep_both")) how = pbc::AskChoice::KeepBoth;
    else if (choice == QLatin1String("replace")) how = pbc::AskChoice::Replace;
    const pbc::AddResult r = actions_.answer_ask(ask, how, newName.toStdString());
    if (r.error.ok()) asks_.erase(askId);   // a refused answer (a bad name, no strong version) leaves the question open for another try
    reload();
    emit changed();
    if (!r.queued.empty()) note_action({ask.target_folder});
    return result_map(r);
}

void QueueBridge::prepareResolve(int id) {
    const auto item = c_.store->queue_item(id);
    if (!item) return;
    // an upload, or a change held because the name at the target is taken: the target is looked at; otherwise the entry itself
    const bool upload = item->kind == pbc::ItemKind::Upload || item->reason == "name_exists";
    const std::string name = upload ? item->dst_name : item->src_remote.substr(item->src_remote.rfind('/') + 1);
    const auto go = [this, id, upload, name](bool online) {
        if (!online) {
            QVariantMap info;
            info["ok"] = false;
            info["errorKey"] = QStringLiteral("no_network");
            emit resolvePrepared(id, info);
            return;
        }
        c_.worker->post([this, id, upload, name] {
            const auto sender = make_sender({});
            const auto seen = upload ? sender->refresh_target(id) : sender->refresh_source(id);
            QMetaObject::invokeMethod(this, [this, id, seen, name] {
                QVariantMap info;
                info["ok"] = seen.ok();
                info["errorKey"] = seen.ok() ? QString() : q(seen.error.key);
                info["exists"] = seen.ok() && seen.value.has_value();
                info["name"] = display_safe(name, 120);
                info["sizeText"] = seen.ok() && seen.value && seen.value->size ? human_size(*seen.value->size) : QString();
                info["dateText"] = seen.ok() && seen.value && seen.value->mtime && c_.format_date_time ? c_.format_date_time(*seen.value->mtime) : QString();
                if (seen.ok()) prepared_etag_[id] = seen.value ? seen.value->etag : std::string();
                emit resolvePrepared(id, info);
            }, Qt::QueuedConnection);
        });
    };
    if (c_.ensure_network) c_.ensure_network(go);
    else go(true);
}

void QueueBridge::resolve(int id, const QString& choice, const QString& newName) {
    const std::string name = newName.toStdString();
    const std::string etag = prepared_etag_.count(id) ? prepared_etag_[id] : std::string();
    const bool prepared = prepared_etag_.count(id) == 1;
    prepared_etag_.erase(id);
    const std::vector<std::string> folders = folders_of(c_.store->queue_item(id));
    c_.worker->post([this, id, choice, name, etag, prepared, folders] {
        const auto sender = make_sender({});
        pbc::Error err;
        if (choice == QLatin1String("keep_both")) err = sender->resolve_keep_both(id, name);
        else if (choice == QLatin1String("edit")) err = sender->resolve_edit(id, name);
        else if (choice == QLatin1String("retarget")) err = sender->resolve_retarget(id, name);   // `newName` carries the folder
        else if (choice == QLatin1String("replace_anyway")) err = prepared ? sender->resolve_replace(id, etag) : pbc::make_error(pbc::ErrorClass::Protocol, "refresh_first");
        else if (choice == QLatin1String("apply_anyway")) err = prepared ? sender->resolve_apply_anyway(id, etag) : pbc::make_error(pbc::ErrorClass::Protocol, "refresh_first");
        else if (choice == QLatin1String("discard") || choice == QLatin1String("skip") || choice == QLatin1String("keep_entry")) err = sender->resolve_discard(id);
        else err = pbc::make_error(pbc::ErrorClass::Protocol, "protocol_error", "unknown choice");
        QMetaObject::invokeMethod(this, [this, id, err, folders] {
            reload();
            emit changed();
            emit resolveFinished(id, err.ok() ? QString() : q(err.key));
            if (err.ok()) note_action(folders);
        }, Qt::QueuedConnection);
    });
}

QVariantMap QueueBridge::newFolder(const QString& parent, const QString& name) {
    const pbc::AddResult r = actions_.new_folder(parent.toStdString(), name.toStdString());
    reload();
    emit changed();
    if (!r.queued.empty()) note_action({parent.toStdString()});
    QVariantMap m = result_map(r);
    m["exists"] = !r.error.ok() && r.error.key == "name_exists";
    return m;
}

QVariantMap QueueBridge::rename(const QString& remotePath, const QString& newName) {
    const pbc::AddResult r = actions_.rename_entry(remotePath.toStdString(), newName.toStdString());
    reload();
    emit changed();
    if (!r.queued.empty()) note_action({pbc::parent_of(remotePath.toStdString())});
    QVariantMap m = result_map(r);
    m["exists"] = !r.error.ok() && r.error.key == "name_exists";
    return m;
}

QVariantMap QueueBridge::moveOrCopy(const QString& kind, const QStringList& remotePaths, const QString& targetFolder) {
    std::vector<std::string> paths;
    for (const QString& p : remotePaths) paths.push_back(p.toStdString());
    const pbc::AddResult r = actions_.move_or_copy(kind == QLatin1String("copy") ? pbc::ItemKind::Copy : pbc::ItemKind::Move, paths, targetFolder.toStdString());
    reload();
    emit changed();
    if (!r.queued.empty()) {
        std::vector<std::string> folders = parents_of(remotePaths);
        folders.push_back(targetFolder.toStdString());
        note_action(folders);
    }
    return result_map(r);
}

QVariantMap QueueBridge::deleteEntries(const QStringList& remotePaths, bool confirmed) {
    std::vector<std::string> paths;
    for (const QString& p : remotePaths) paths.push_back(p.toStdString());
    if (!confirmed) {
        const pbc::DeletePreview p = actions_.delete_preview(paths);
        QVariantMap m;
        QStringList names;
        for (const std::string& n : p.names) names << display_safe(n, 80);
        m["names"] = names;
        m["entries"] = p.entries;
        m["folders"] = p.folders;
        m["itemsInside"] = p.items_inside;
        m["deleteOffered"] = p.offered;
        m["trashKnown"] = p.trash_known;
        return m;
    }
    const pbc::AddResult r = actions_.delete_entries(paths);
    reload();
    emit changed();
    if (!r.queued.empty()) note_action(parents_of(remotePaths));
    return result_map(r);
}
