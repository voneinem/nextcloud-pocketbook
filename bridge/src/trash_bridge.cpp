#include "trash_bridge.h"

#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <set>

#include "format_util.h"
#include "pbc/age_text.h"
#include "pbc/new_name.h"
#include "pbc/remote_path.h"
#include "pbc/trash_api.h"
#include "pbc/trash_rules.h"
#include "trash_support.h"
#include "worker.h"

namespace {

constexpr int64_t kAccountId = 1;   // the one account row of the store (Store::save_account)

QString age_kind_key(pbc::AgeKind k) {
    switch (k) {
        case pbc::AgeKind::Gone: return QStringLiteral("gone");
        case pbc::AgeKind::JustNow: return QStringLiteral("just_now");
        case pbc::AgeKind::Minutes: return QStringLiteral("minutes");
        case pbc::AgeKind::Hours: return QStringLiteral("hours");
        case pbc::AgeKind::Days: return QStringLiteral("days");
        case pbc::AgeKind::NotSynced: break;
    }
    return QStringLiteral("not_synced");
}

}  // namespace

TrashBridge::TrashBridge(Config config, QObject* parent) : QObject(parent), c_(std::move(config)) {
    availability_map_ = trash_support::availability_map(availability_);
}

TrashBridge::~TrashBridge() {
    if (c_.worker) c_.worker->wait_idle();   // no job may call back into a bridge that is gone
}

bool TrashBridge::canAct() const {
    return state_ == QLatin1String("listed") && !from_store_ && availability_.trash == pbc::FeatureState::On && !offline();
}

QString TrashBridge::ageKind() const { return age_kind_key(pbc::listing_age(c_.clock->now(), fetched_at_, false).kind); }
int TrashBridge::ageCount() const { return pbc::listing_age(c_.clock->now(), fetched_at_, false).n; }
bool TrashBridge::clockSuspect() const { return pbc::listing_age(c_.clock->now(), fetched_at_, false).clock_suspect; }

QString TrashBridge::chosenName() const {
    if (chosen_ < 0 || chosen_ >= static_cast<int>(items_.size())) return QString();
    return display_safe(items_[static_cast<size_t>(chosen_)].name, 120);
}

struct TrashBridge::Checked {
    pbc::Error error;                        // of the check of the apps
    pbc::TrashAvailability availability;
    bool listed = false;                     // the list below was read
    pbc::Error list_error;
    pbc::TrashList list;
};

void TrashBridge::with_network(quint64 generation, std::function<void()> go, std::function<void()> failed) {
    if (!offline() || !c_.ensure_network) {
        go();
        return;
    }
    // A tap needs the network: the system is asked to bring Wi-Fi up, as in the files client. Nothing is queued: if it does not come up, the tap fails.
    waiting_network_ = true;
    QPointer<TrashBridge> self(this);
    c_.ensure_network([self, generation, go, failed](bool online) {
        if (!self || generation != self->generation_) return;   // the screen was left meanwhile: nothing is sent
        self->waiting_network_ = false;
        if (online) go();
        else failed();
    });
}

void TrashBridge::clear_list() {
    items_.clear();
    rows_.clear();
    fetched_at_.reset();
    from_store_ = truncated_ = false;
    page_ = 0;
    page_count_ = 1;
    chosen_ = -1;
    chosen_trash_name_.clear();
}

QVariantMap TrashBridge::row_map(int index) const {
    const pbc::TrashItem& it = items_[static_cast<size_t>(index)];
    QVariantMap m;
    const bool actionable = !it.unreadable && it.availability == pbc::Availability::Ok;
    m.insert(QStringLiteral("index"), index);
    m.insert(QStringLiteral("name"), it.unreadable ? bridge_tr("Unreadable item") : display_safe(it.name, 120));
    m.insert(QStringLiteral("originalText"), it.original_known ? bridge_tr("From %1").arg(display_safe(it.original_dir, 120)) : bridge_tr("Unknown location"));
    QString deleted = bridge_tr("Deletion date unknown");
    if (it.deleted_at) deleted = bridge_tr("Deleted %1").arg(c_.format_date ? c_.format_date(*it.deleted_at) : QString::number(*it.deleted_at));
    m.insert(QStringLiteral("deletedText"), deleted);
    m.insert(QStringLiteral("sizeText"), it.kind == pbc::Kind::File && it.size ? human_size(*it.size) : QString());
    m.insert(QStringLiteral("isFolder"), it.kind == pbc::Kind::Folder);
    m.insert(QStringLiteral("unreadable"), it.unreadable);
    m.insert(QStringLiteral("actionable"), actionable);
    m.insert(QStringLiteral("chosen"), index == chosen_);
    m.insert(QStringLiteral("unknown"), !it.trash_name.empty() && it.trash_name == unknown_trash_name_);   // the outcome of its restore is not known
    return m;
}

void TrashBridge::rebuild_rows() {
    const int total = static_cast<int>(items_.size());
    page_count_ = std::max(1, (total + page_size_ - 1) / page_size_);
    page_ = std::max(0, std::min(page_, page_count_ - 1));
    rows_.clear();
    const int first = page_ * page_size_;
    for (int i = first; i < std::min(total, first + page_size_); ++i) rows_.append(row_map(i));
}

void TrashBridge::setPageSize(int n) {
    n = std::max(1, std::min(n, 100));
    if (n == page_size_) return;
    const int first = page_ * page_size_;
    page_size_ = n;
    page_ = first / page_size_;
    rebuild_rows();
    emit changed();
}

void TrashBridge::nextPage() {
    if (page_ + 1 >= page_count_) return;
    ++page_;
    rebuild_rows();
    emit changed();
}

void TrashBridge::previousPage() {
    if (page_ == 0) return;
    --page_;
    rebuild_rows();
    emit changed();
}

QVariantMap TrashBridge::choose(int index) {
    QVariantMap r;
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        r.insert(QStringLiteral("errorKey"), QStringLiteral("trash_item_gone"));
        return r;
    }
    const pbc::TrashItem& it = items_[static_cast<size_t>(index)];
    if (it.unreadable || it.availability != pbc::Availability::Ok) {
        r.insert(QStringLiteral("errorKey"), QStringLiteral("trash_not_actionable"));
        return r;
    }
    chosen_ = index;
    chosen_trash_name_ = it.trash_name;
    rebuild_rows();
    emit changed();
    r.insert(QStringLiteral("ok"), true);
    return r;
}

// The list of an earlier visit, for a connection that is not there: shown with its age, never as current.
bool TrashBridge::show_stored(const std::string& message_key) {
    const auto stored = c_.store ? c_.store->load_trash(kAccountId) : pbc::Result<pbc::TrashList>();
    if (!stored.ok() || !stored.value.fetched_at) return false;
    const std::string keep = chosen_trash_name_;
    items_ = stored.value.items;
    truncated_ = stored.value.truncated;
    fetched_at_ = stored.value.fetched_at;
    from_store_ = true;
    chosen_ = -1;
    for (size_t i = 0; i < items_.size() && !keep.empty(); ++i)
        if (items_[i].trash_name == keep) chosen_ = static_cast<int>(i);
    if (chosen_ < 0) chosen_trash_name_.clear();
    rebuild_rows();
    state_ = QStringLiteral("listed");
    message_ = trash_support::message_map(message_key);
    return true;
}

QVariantMap TrashBridge::open() { return start_check(); }
QVariantMap TrashBridge::refresh() { return start_check(); }

QVariantMap TrashBridge::start_check() {
    QVariantMap r;
    if (busy_) {
        r.insert(QStringLiteral("errorKey"), QStringLiteral("busy"));
        return r;
    }
    ++generation_;
    ++epoch_;
    pending_.reset();   // a proposal or a chosen place does not outlive a new look at the trash
    destruction_.reset();   // nor does a confirmation of an irreversible step: what it named may be out of date
    opened_key_ = account_key();
    message_.clear();
    const auto offline_failed = [this] {
        busy_ = false;
        if (!show_stored("no_network")) {
            clear_list();
            state_ = QStringLiteral("failed");
            message_ = trash_support::message_map("no_network");
        }
        emit changed();
    };
    if (offline() && !c_.ensure_network) {
        offline_failed();
        r.insert(QStringLiteral("errorKey"), QStringLiteral("no_network"));
        return r;
    }
    busy_ = true;
    state_ = QStringLiteral("checking");
    emit changed();
    const quint64 generation = generation_;
    const Config c = c_;
    with_network(
        generation,
        [this, c, generation] {
            c_.worker->post([this, c, generation] {
                Checked checked;
                trash_support::Online online(*c.store, *c.http);
                if (!online.ok()) {
                    checked.error = online.error();
                } else {
                    pbc::TrashApi api(online.dav(), online.user_id(), *c.clock);
                    auto a = api.fetch_availability();
                    online.record(a.error);
                    checked.error = a.error;
                    checked.availability = a.value;
                    if (a.error.ok() && a.value.trash == pbc::FeatureState::On) {
                        auto l = api.list_trash();
                        online.record(l.error);
                        checked.listed = true;
                        checked.list_error = l.error;
                        checked.list = std::move(l.value);
                        if (l.error.ok()) c.store->replace_trash(checked.list, c.clock->now(), kAccountId);   // a failed save only costs the offline copy
                    }
                }
                QMetaObject::invokeMethod(this, [this, generation, checked] { apply_checked(generation, checked); }, Qt::QueuedConnection);
            });
        },
        offline_failed);
    r.insert(QStringLiteral("ok"), true);
    return r;
}

void TrashBridge::apply_checked(quint64 generation, const Checked& checked) {
    if (generation != generation_) return;   // the screen was left meanwhile: nothing from this answer is kept
    busy_ = false;
    availability_ = checked.availability;
    availability_map_ = trash_support::availability_map(availability_);
    const auto problem = [this](const pbc::Error& e, const std::string& code) {
        // the connection (no Wi-Fi, a certificate, a wrong password) is told as such; an unclear answer is a generic message with its code (spec FR-030)
        if (e.cls == pbc::ErrorClass::Network && show_stored(e.key)) return;
        clear_list();
        const bool plain = trash_support::is_connection_problem(e) || e.key == "listing_too_large";
        state_ = plain ? QStringLiteral("failed") : QStringLiteral("explainUnknown");
        message_ = trash_support::message_map(e.key, plain ? std::string() : code);
    };
    if (!checked.error.ok() && trash_support::is_connection_problem(checked.error)) {
        problem(checked.error, availability_.code);
    } else if (availability_.trash == pbc::FeatureState::Off) {
        clear_list();
        state_ = QStringLiteral("explainOff");
        message_ = trash_support::message_map("trash_unavailable");
    } else if (availability_.trash == pbc::FeatureState::Unknown) {
        clear_list();
        state_ = QStringLiteral("explainUnknown");   // never "off": the answer was missing or unclear (spec FR-030)
        message_ = trash_support::message_map(checked.error.ok() ? std::string("protocol_error") : checked.error.key, availability_.code);
    } else if (checked.listed && !checked.list_error.ok()) {
        problem(checked.list_error, checked.list.code);
    } else if (checked.listed) {
        const std::string keep = chosen_trash_name_;
        unknown_trash_name_.clear();   // a fresh list shows what really happened
        items_ = checked.list.items;
        truncated_ = checked.list.truncated;
        fetched_at_ = c_.clock->now();
        from_store_ = false;
        chosen_ = -1;
        for (size_t i = 0; i < items_.size() && !keep.empty(); ++i)
            if (items_[i].trash_name == keep) chosen_ = static_cast<int>(i);
        if (chosen_ < 0) chosen_trash_name_.clear();
        page_ = 0;
        rebuild_rows();
        state_ = QStringLiteral("listed");
    }
    emit changed();
}

// ---- Restore (US2) ----

namespace {

QString plan_key(pbc::RestorePlan p) {
    switch (p) {
        case pbc::RestorePlan::OriginalFree: return QStringLiteral("original_free");
        case pbc::RestorePlan::NameTaken: return QStringLiteral("name_taken");
        case pbc::RestorePlan::ParentMissing: return QStringLiteral("parent_missing");
        case pbc::RestorePlan::ChoosePlace: return QStringLiteral("choose_place");
        case pbc::RestorePlan::Unavailable: break;
    }
    return QStringLiteral("unavailable");
}

const char* confirm_kind_key(pbc::ConfirmKind k) {
    switch (k) {
        case pbc::ConfirmKind::RestoreProposal: return "restore";
        case pbc::ConfirmKind::DeleteFile: return "delete_file";
        case pbc::ConfirmKind::DeleteFolder: return "delete_folder";
        case pbc::ConfirmKind::EmptyTrash: return "empty_trash";
        case pbc::ConfirmKind::RestoreVersion: return "restore_version";
    }
    return "restore";
}

QVariantMap confirm_map(const pbc::ConfirmContent& c) {
    QVariantMap m;
    m.insert(QStringLiteral("kind"), QString::fromLatin1(confirm_kind_key(c.kind)));
    QStringList names;
    for (const std::string& n : c.shown_names) names << display_safe(n, 60);
    m.insert(QStringLiteral("names"), names);
    m.insert(QStringLiteral("items"), c.items);
    m.insert(QStringLiteral("folders"), c.folders);
    m.insert(QStringLiteral("stage"), c.stage);
    m.insert(QStringLiteral("truncatedNotice"), c.truncated_notice);
    m.insert(QStringLiteral("destructive"), c.destructive);
    m.insert(QStringLiteral("defaultCancel"), c.default_cancel);
    m.insert(QStringLiteral("originalPlace"), c.original_place);
    m.insert(QStringLiteral("place"), display_safe(c.place, 200));
    return m;
}

}  // namespace

// The restore in progress: the item (a copy: the list may change meanwhile), whether the original place can be proposed, and the place the user chose.
struct TrashBridge::Pending {
    pbc::TrashItem item;
    bool original_ok = false;       // the plan was OriginalFree: restoreConfirmed() may send
    pbc::RestoreTarget target;      // the chosen folder and name (is_original false)
    bool target_ready = false;      // the chosen place was probed and is free: restoreChosenConfirmed() may send
};

struct TrashBridge::Probed {
    pbc::Error error;
    pbc::RestorePlan plan = pbc::RestorePlan::Unavailable;
    std::string code;
};

struct TrashBridge::Finished {
    pbc::TrashItem item;
    bool original = true;
    pbc::RestoreTarget target;
    pbc::OperationResult result;
};

QVariantMap TrashBridge::refuse(const char* key) const {
    QVariantMap r;
    r.insert(QStringLiteral("errorKey"), QString::fromLatin1(key));
    return r;
}

// The checks every action of the trash screen starts with. An empty map: go on.
QVariantMap TrashBridge::check_action(bool needs_chosen) {
    if (busy_) return refuse("busy");
    if (state_ == QLatin1String("explainOff") || state_ == QLatin1String("explainUnknown")) return refuse("trash_unavailable");
    if (needs_chosen && (chosen_ < 0 || chosen_ >= static_cast<int>(items_.size()))) return refuse("trash_item_gone");
    if (offline()) return refuse("no_network");   // the actions need a connection (spec FR-035): nothing is queued, nothing asks for Wi-Fi
    return QVariantMap();
}

QVariantMap TrashBridge::restore() {
    if (const QVariantMap r = check_action(true); !r.isEmpty()) return r;
    const pbc::TrashItem item = items_[static_cast<size_t>(chosen_)];
    if (!pbc::actionable(item)) return refuse("trash_not_actionable");
    pending_.reset();
    result_.clear();   // the message of the last action is not the message of this one
    const quint64 epoch = epoch_;
    busy_ = true;
    emit changed();
    const Config c = c_;
    c_.worker->post([this, c, item, epoch] {
        auto probed = std::make_shared<Probed>();
        trash_support::Online online(*c.store, *c.http);
        if (!online.ok()) {
            probed->error = online.error();
        } else {
            pbc::TrashApi api(online.dav(), online.user_id(), *c.clock);
            const auto plan = api.probe_restore(item);
            online.record(plan.error);
            probed->error = plan.error;
            probed->plan = plan.value;
        }
        QMetaObject::invokeMethod(this, [this, item, epoch, probed] {
            busy_ = false;
            if (epoch != epoch_) {   // cancelled or the trash was opened again meanwhile
                emit changed();
                return;
            }
            if (!probed->error.ok()) {
                result_.clear();
                result_.insert(QStringLiteral("kind"), QStringLiteral("restore"));
                result_.insert(QStringLiteral("ok"), false);
                result_.insert(QStringLiteral("key"), QString::fromStdString(probed->error.key));
                result_.insert(QStringLiteral("code"), QString::fromStdString(pbc::refusal_for(4, 0).code));
                emit changed();
                emit actionFinished(result_);
                return;
            }
            pending_ = std::make_shared<Pending>();
            pending_->item = item;
            pending_->original_ok = probed->plan == pbc::RestorePlan::OriginalFree;
            pbc::RestoreTarget original;
            original.folder = item.original_dir;
            original.name = item.original_name;
            original.is_original = true;
            QVariantMap proposal = confirm_map(pbc::confirm_for_restore(item, original));
            proposal.insert(QStringLiteral("folder"), display_safe(item.original_dir, 200));
            proposal.insert(QStringLiteral("name"), display_safe(item.original_name, 120));
            emit changed();
            emit restorePlan(plan_key(probed->plan), proposal);
        }, Qt::QueuedConnection);
    });
    QVariantMap r;
    r.insert(QStringLiteral("ok"), true);
    return r;
}

void TrashBridge::cancelStep() {
    ++epoch_;
    pending_.reset();
    destruction_.reset();
}

void TrashBridge::remove_item(const std::string& trash_name) {
    for (size_t i = 0; i < items_.size(); ++i)
        if (items_[i].trash_name == trash_name) {
            items_.erase(items_.begin() + static_cast<long>(i));
            break;
        }
    if (chosen_trash_name_ == trash_name) {
        chosen_ = -1;
        chosen_trash_name_.clear();
    } else {
        chosen_ = -1;   // the places moved: find the chosen item again
        for (size_t i = 0; i < items_.size() && !chosen_trash_name_.empty(); ++i)
            if (items_[i].trash_name == chosen_trash_name_) chosen_ = static_cast<int>(i);
    }
    if (c_.store) c_.store->remove_trash_item(trash_name);
    rebuild_rows();
}

void TrashBridge::run_restore(bool original) {
    auto finished = std::make_shared<Finished>();
    finished->item = pending_->item;
    finished->original = original;
    finished->target = pending_->target;
    busy_ = true;
    emit changed();
    const Config c = c_;
    c_.worker->post([this, c, finished] {
        trash_support::Online online(*c.store, *c.http);
        if (!online.ok()) {
            finished->result.error = online.error();
        } else {
            pbc::TrashApi api(online.dav(), online.user_id(), *c.clock);
            finished->result = finished->original ? api.restore_original(finished->item) : api.restore_to(finished->item, finished->target);
            online.record(finished->result.error);
        }
        QMetaObject::invokeMethod(this, [this, finished] { apply_finished(*finished); }, Qt::QueuedConnection);   // always applied: the request was sent
    });
}

QVariantMap TrashBridge::restoreConfirmed() {
    if (!pending_ || !pending_->original_ok) return refuse("nothing_pending");
    if (const QVariantMap r = check_action(false); !r.isEmpty()) return r;
    run_restore(true);
    QVariantMap r;
    r.insert(QStringLiteral("ok"), true);
    return r;
}

QVariantMap TrashBridge::restoreChosenConfirmed() {
    if (!pending_ || !pending_->target_ready) return refuse("nothing_pending");
    if (const QVariantMap r = check_action(false); !r.isEmpty()) return r;
    run_restore(false);
    QVariantMap r;
    r.insert(QStringLiteral("ok"), true);
    return r;
}

// Probes a chosen place on the worker and answers with targetChecked.
void TrashBridge::probe_target(const pbc::RestoreTarget& target) {
    busy_ = true;
    emit changed();
    const Config c = c_;
    const pbc::TrashItem item = pending_->item;
    const quint64 epoch = epoch_;
    c_.worker->post([this, c, item, target, epoch] {
        auto probed = std::make_shared<Probed>();
        trash_support::Online online(*c.store, *c.http);
        if (!online.ok()) {
            probed->error = online.error();
        } else {
            pbc::TrashApi api(online.dav(), online.user_id(), *c.clock);
            const auto plan = api.probe_place(target.folder, target.name);
            online.record(plan.error);
            probed->error = plan.error;
            probed->plan = plan.value;
        }
        QMetaObject::invokeMethod(this, [this, item, target, epoch, probed] {
            busy_ = false;
            if (epoch != epoch_ || !pending_) {
                emit changed();
                return;
            }
            QVariantMap answer;
            answer.insert(QStringLiteral("folder"), display_safe(target.folder, 200));
            answer.insert(QStringLiteral("name"), display_safe(target.name, 120));
            pending_->target_ready = false;
            if (!probed->error.ok()) {
                answer.insert(QStringLiteral("errorKey"), QString::fromStdString(probed->error.key));
            } else if (probed->plan == pbc::RestorePlan::OriginalFree) {
                pending_->target = target;
                pending_->target_ready = true;
                answer.insert(QStringLiteral("needsConfirm"), true);
                answer.insert(QStringLiteral("summary"), confirm_map(pbc::confirm_for_restore(item, target)));
            } else if (probed->plan == pbc::RestorePlan::NameTaken) {
                pending_->target = target;   // the folder stays chosen: the user types another name
                const pbc::ServerCaps caps = c_.store->server_caps().value_or(pbc::ServerCaps());
                const std::string folder = target.folder, wanted = target.name;
                const std::string suggestion = pbc::keep_both_name(wanted, [&](std::string_view n) {
                    return n == wanted || (c_.store && c_.store->entry(pbc::join_remote(folder, std::string(n))).has_value());
                }, caps);
                answer.insert(QStringLiteral("needsName"), true);
                answer.insert(QStringLiteral("name"), display_safe(suggestion, 120));
            } else {
                answer.insert(QStringLiteral("errorKey"), QStringLiteral("restore_parent_missing"));
            }
            emit changed();
            emit targetChecked(answer);
        }, Qt::QueuedConnection);
    });
}

QVariantMap TrashBridge::chooseTarget(const QString& folderPath) {
    if (!pending_) return refuse("nothing_pending");
    if (const QVariantMap r = check_action(false); !r.isEmpty()) return r;
    pbc::RestoreTarget target;
    target.folder = pbc::normalize_remote_path(folderPath.toStdString());
    const pbc::TrashItem& item = pending_->item;
    target.name = item.original_known ? item.original_name : item.name;
    const pbc::ServerCaps caps = c_.store->server_caps().value_or(pbc::ServerCaps());
    const pbc::TargetCheck check = pbc::check_restore_target(item, target, caps);
    if (!check.ok && check.error_key != "name_forbidden") return refuse(check.error_key.c_str());   // a folder that cannot be used
    pending_->target = target;
    pending_->target_ready = false;
    if (!check.ok) {   // the proposed name is not allowed there: the user types one
        QVariantMap answer;
        answer.insert(QStringLiteral("needsName"), true);
        answer.insert(QStringLiteral("folder"), display_safe(target.folder, 200));
        answer.insert(QStringLiteral("name"), QString());
        QMetaObject::invokeMethod(this, [this, answer] { emit targetChecked(answer); }, Qt::QueuedConnection);
        QVariantMap r;
        r.insert(QStringLiteral("ok"), true);
        return r;
    }
    probe_target(target);
    QVariantMap r;
    r.insert(QStringLiteral("ok"), true);
    return r;
}

QVariantMap TrashBridge::setTargetName(const QString& name) {
    if (!pending_ || pending_->target.folder.empty()) return refuse("nothing_pending");
    if (busy_) return refuse("busy");
    pbc::RestoreTarget target = pending_->target;
    target.name = name.toStdString();
    const pbc::ServerCaps caps = c_.store->server_caps().value_or(pbc::ServerCaps());
    const pbc::TargetCheck check = pbc::check_restore_target(pending_->item, target, caps);
    if (!check.ok) return refuse(check.error_key.c_str());
    if (offline()) return refuse("no_network");
    pending_->target = target;
    pending_->target_ready = false;
    probe_target(target);
    QVariantMap r;
    r.insert(QStringLiteral("ok"), true);
    return r;
}

// The result of a restore request: the list, the store, the cache and the marks follow what the server said.
void TrashBridge::apply_finished(const Finished& f) {
    busy_ = false;
    const pbc::OperationResult& r = f.result;
    result_.clear();
    result_.insert(QStringLiteral("kind"), QStringLiteral("restore"));
    result_.insert(QStringLiteral("ok"), r.error.ok());
    result_.insert(QStringLiteral("key"), QString::fromStdString(r.error.key));
    result_.insert(QStringLiteral("code"), QString::fromStdString(r.error.ok() ? std::string() : r.code));
    result_.insert(QStringLiteral("outcomeUnknown"), r.outcome_unknown);
    result_.insert(QStringLiteral("landedElsewhere"), r.landed_elsewhere);
    if (r.error.ok()) {
        const std::string folder = f.original ? f.item.original_dir : f.target.folder;
        remove_item(f.item.trash_name);
        if (c_.store) {
            c_.store->forget_folder_after_change(folder);   // the next visit of that folder fetches it; the versions above are forgotten
            if (f.original && !r.landed_elsewhere) {        // a local copy that was "removed on the server" is itself again; no file is touched
                const std::string path = pbc::join_remote(f.item.original_dir, f.item.original_name);
                for (const auto& pair : c_.store->local_path_pairs())
                    if (pair.first == path || pair.first.compare(0, path.size() + 1, path + "/") == 0) c_.store->set_server_missing(pair.first, false);
            }
        }
    } else if (r.error.key == "trash_item_gone") {
        remove_item(f.item.trash_name);   // it is not in the trash any more
    } else if (r.outcome_unknown) {
        unknown_trash_name_ = f.item.trash_name;
        rebuild_rows();
    }
    pending_.reset();
    emit changed();
    emit actionFinished(result_);
}

// ---- Delete for good and Empty trash (US4) ----

QString TrashBridge::account_key() const {
    const auto a = c_.store ? c_.store->account() : std::nullopt;
    return a ? QString::fromStdString(a->base_url + "\n" + a->user_id) : QString();
}

void TrashBridge::onApplicationStateChanged(Qt::ApplicationState state) {
    if (state == Qt::ApplicationActive) return;
    ++epoch_;
    pending_.reset();
    destruction_.reset();   // a request in flight is not touched
}

void TrashBridge::onConnectionChanged() {
    ++epoch_;
    pending_.reset();
    destruction_.reset();
    if (state_ != QLatin1String("closed") && !busy_ && account_key() != opened_key_) {   // another account: its trash is not shown
        clear_list();
        availability_ = pbc::TrashAvailability();
        availability_map_ = trash_support::availability_map(availability_);
        message_.clear();
        result_.clear();
        state_ = QStringLiteral("closed");
        emit changed();
    }
}

// what the named items of a chain are, for the dialogs
struct TrashBridge::Destruction {
    pbc::ConfirmKind kind = pbc::ConfirmKind::DeleteFile;
    std::vector<pbc::TrashItem> named;   // a copy: later changes of the list do not alter what was named
    bool truncated = false;
    int stage = 1;
};

QVariantMap TrashBridge::destruction_content(const Destruction& d) const {
    if (d.kind == pbc::ConfirmKind::EmptyTrash) return confirm_map(pbc::confirm_for_empty(d.named, d.truncated, d.stage));
    return confirm_map(pbc::confirm_for_delete(d.named.front(), d.stage));
}

QVariantMap TrashBridge::beginDelete() {
    if (!c_.delete_enabled) return refuse("not_available");
    if (const QVariantMap r = check_action(true); !r.isEmpty()) return r;
    const pbc::TrashItem& item = items_[static_cast<size_t>(chosen_)];
    if (!pbc::actionable(item)) return refuse("trash_not_actionable");
    pending_.reset();
    destruction_ = std::make_shared<Destruction>();
    destruction_->kind = item.kind == pbc::Kind::Folder ? pbc::ConfirmKind::DeleteFolder : pbc::ConfirmKind::DeleteFile;
    destruction_->named.push_back(item);
    return destruction_content(*destruction_);
}

QVariantMap TrashBridge::beginEmpty() {
    if (!c_.delete_enabled) return refuse("not_available");
    if (const QVariantMap r = check_action(false); !r.isEmpty()) return r;
    pbc::TrashList held;
    held.items = items_;
    const pbc::EmptySet set = pbc::empty_set_of(held);
    if (set.named.empty()) return refuse("trash_item_gone");   // nothing to delete
    pending_.reset();
    destruction_ = std::make_shared<Destruction>();
    destruction_->kind = pbc::ConfirmKind::EmptyTrash;
    destruction_->named = set.named;
    destruction_->truncated = truncated_;
    return destruction_content(*destruction_);
}

QVariantMap TrashBridge::confirmStep() {
    if (!destruction_) return refuse("nothing_pending");
    if (const QVariantMap r = check_action(false); !r.isEmpty()) return r;
    if (destruction_->stage < pbc::confirm_stages(destruction_->kind)) {   // the next confirmation is shown first; nothing is sent
        ++destruction_->stage;
        return destruction_content(*destruction_);
    }
    run_destruction();
    QVariantMap r;
    r.insert(QStringLiteral("started"), true);
    return r;
}

void TrashBridge::cancelRun() {
    if (auto flag = cancel_run_) flag->store(true);
}

struct TrashBridge::Destroyed {
    pbc::ConfirmKind kind = pbc::ConfirmKind::DeleteFile;
    std::vector<pbc::TrashItem> named;
    pbc::OperationResult result;
};

void TrashBridge::run_destruction() {
    auto d = std::make_shared<Destroyed>();
    d->kind = destruction_->kind;
    d->named = destruction_->named;
    destruction_.reset();   // one confirmation chain, one run
    busy_ = true;
    cancel_run_ = std::make_shared<std::atomic<bool>>(false);
    const auto cancel = cancel_run_;
    progress_.clear();
    progress_.insert(QStringLiteral("done"), 0);
    progress_.insert(QStringLiteral("total"), static_cast<int>(d->named.size()));
    progress_.insert(QStringLiteral("removed"), 0);
    progress_.insert(QStringLiteral("notRemoved"), 0);
    emit changed();
    const Config c = c_;
    const int total = static_cast<int>(d->named.size());
    const int step = std::max(1, std::min(25, total / 10));   // a screen update about every tenth item, at most every 25th: a coarse refresh budget
    c_.worker->post([this, c, d, cancel, total, step] {
        trash_support::Online online(*c.store, *c.http);
        if (!online.ok()) {
            d->result.error = online.error();
            d->result.not_tried = total;
        } else {
            pbc::TrashApi api(online.dav(), online.user_id(), *c.clock);
            if (d->kind == pbc::ConfirmKind::EmptyTrash) {
                d->result = api.empty_trash(d->named, [this, cancel, total, step](const pbc::EmptyProgress& p) {
                    if (p.done % step == 0 && p.done < total) {
                        QMetaObject::invokeMethod(this, [this, p] {
                            progress_.insert(QStringLiteral("done"), p.done);
                            progress_.insert(QStringLiteral("removed"), p.removed);
                            progress_.insert(QStringLiteral("notRemoved"), p.not_removed);
                            emit changed();
                        }, Qt::QueuedConnection);
                    }
                    return !cancel->load();
                });
            } else {
                d->result = api.delete_item(d->named.front());
            }
            online.record(d->result.error);
        }
        QMetaObject::invokeMethod(this, [this, d] { apply_destroyed(*d); }, Qt::QueuedConnection);   // always applied: requests were sent
    });
}

void TrashBridge::remove_items(const std::vector<std::string>& trash_names) {
    std::set<std::string> gone(trash_names.begin(), trash_names.end());
    std::vector<pbc::TrashItem> kept;
    for (const pbc::TrashItem& i : items_)
        if (i.trash_name.empty() || !gone.count(i.trash_name)) kept.push_back(i);
    items_ = std::move(kept);
    chosen_ = -1;
    for (size_t i = 0; i < items_.size() && !chosen_trash_name_.empty(); ++i)
        if (items_[i].trash_name == chosen_trash_name_) chosen_ = static_cast<int>(i);
    if (chosen_ < 0) chosen_trash_name_.clear();
    if (c_.store)
        for (const std::string& n : trash_names) c_.store->remove_trash_item(n);
    rebuild_rows();
}

void TrashBridge::apply_destroyed(const Destroyed& d) {
    busy_ = false;
    cancel_run_.reset();
    const pbc::OperationResult& r = d.result;
    const bool empty = d.kind == pbc::ConfirmKind::EmptyTrash;
    result_.clear();
    result_.insert(QStringLiteral("kind"), empty ? QStringLiteral("empty") : QStringLiteral("delete"));
    result_.insert(QStringLiteral("ok"), empty ? (r.error.ok() || r.error.key == "cancelled") : r.error.ok());
    result_.insert(QStringLiteral("key"), QString::fromStdString(r.error.key));
    result_.insert(QStringLiteral("code"), QString::fromStdString(r.error.ok() ? std::string() : r.code));
    result_.insert(QStringLiteral("removed"), r.removed);
    result_.insert(QStringLiteral("notRemoved"), r.not_removed);
    result_.insert(QStringLiteral("notTried"), r.not_tried);
    result_.insert(QStringLiteral("outcomeUnknown"), r.outcome_unknown);
    result_.insert(QStringLiteral("cancelled"), r.error.key == "cancelled");
    if (empty) {
        remove_items(r.removed_trash_names);
    } else if (r.removed > 0) {
        remove_items({d.named.front().trash_name});
    }
    if (r.outcome_unknown) {
        unknown_trash_name_ = r.unknown_trash_name;
        rebuild_rows();
    }
    progress_.insert(QStringLiteral("done"), r.removed + r.not_removed + (r.outcome_unknown ? 1 : 0));
    progress_.insert(QStringLiteral("removed"), r.removed);
    progress_.insert(QStringLiteral("notRemoved"), r.not_removed);
    emit changed();
    emit actionFinished(result_);
}
