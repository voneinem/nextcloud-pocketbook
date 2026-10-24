#include "versions_bridge.h"

#include <QMetaObject>
#include <QPointer>

#include <algorithm>

#include "format_util.h"
#include "pbc/remote_path.h"
#include "pbc/trash_api.h"
#include "pbc/trash_rules.h"
#include "trash_support.h"
#include "worker.h"

VersionsBridge::VersionsBridge(Config config, QObject* parent) : QObject(parent), c_(std::move(config)) {
    availability_map_ = trash_support::availability_map(availability_);
}

VersionsBridge::~VersionsBridge() {
    if (c_.worker) c_.worker->wait_idle();   // no job may call back into a bridge that is gone
}

bool VersionsBridge::canAct() const {
    return (state_ == QLatin1String("listed") || state_ == QLatin1String("empty")) && availability_.versions == pbc::FeatureState::On && !offline();
}

struct VersionsBridge::Checked {
    pbc::Error error;                        // of the check of the apps
    pbc::TrashAvailability availability;
    bool listed = false;
    pbc::Error list_error;
    pbc::VersionList list;
};

struct VersionsBridge::Finished {
    pbc::Error error;
    pbc::OperationResult result;
    bool have_after = false;
    pbc::VersionList after;
};

void VersionsBridge::with_network(quint64 generation, std::function<void()> go, std::function<void()> failed) {
    if (!offline() || !c_.ensure_network) {
        go();
        return;
    }
    waiting_network_ = true;
    QPointer<VersionsBridge> self(this);
    c_.ensure_network([self, generation, go, failed](bool online) {
        if (!self || generation != self->generation_) return;   // the screen was left meanwhile: nothing is sent
        self->waiting_network_ = false;
        if (online) go();
        else failed();
    });
}

QVariantMap VersionsBridge::refuse(const char* key) const {
    QVariantMap r;
    r.insert(QStringLiteral("errorKey"), QString::fromLatin1(key));
    return r;
}

QString VersionsBridge::date_text(const std::optional<int64_t>& epoch) const {
    if (!epoch) return bridge_tr("Unknown date");
    if (c_.format_date_time) return c_.format_date_time(*epoch);
    if (c_.format_date) return c_.format_date(*epoch);
    return QString::number(*epoch);
}

QVariantMap VersionsBridge::row_map(int index) const {
    const pbc::VersionRow& v = list_.rows[static_cast<size_t>(index)];
    QVariantMap m;
    m.insert(QStringLiteral("index"), index);
    m.insert(QStringLiteral("dateText"), date_text(v.mtime));
    m.insert(QStringLiteral("sizeText"), v.size ? human_size(*v.size) : QString());
    m.insert(QStringLiteral("future"), v.future);
    m.insert(QStringLiteral("chosen"), index == chosen_);
    return m;
}

void VersionsBridge::rebuild_rows() {
    const int total = static_cast<int>(list_.rows.size());
    page_count_ = std::max(1, (total + page_size_ - 1) / page_size_);
    page_ = std::max(0, std::min(page_, page_count_ - 1));
    rows_.clear();
    const int first = page_ * page_size_;
    for (int i = first; i < std::min(total, first + page_size_); ++i) rows_.append(row_map(i));
}

void VersionsBridge::setPageSize(int n) {
    n = std::max(1, std::min(n, 100));
    if (n == page_size_) return;
    const int first = page_ * page_size_;
    page_size_ = n;
    page_ = first / page_size_;
    rebuild_rows();
    emit changed();
}

void VersionsBridge::nextPage() {
    if (page_ + 1 >= page_count_) return;
    ++page_;
    rebuild_rows();
    emit changed();
}

void VersionsBridge::previousPage() {
    if (page_ == 0) return;
    --page_;
    rebuild_rows();
    emit changed();
}

QVariantMap VersionsBridge::choose(int index) {
    if (index < 0 || index >= static_cast<int>(list_.rows.size())) return refuse("version_gone");
    chosen_ = index;
    rebuild_rows();
    emit changed();
    QVariantMap r;
    r.insert(QStringLiteral("ok"), true);
    return r;
}

void VersionsBridge::cancelStep() { pending_ = false; }

QVariantMap VersionsBridge::open(const QString& remotePath) {
    if (busy_) return refuse("busy");
    const std::string path = pbc::normalize_remote_path(remotePath.toStdString());
    const auto entry = c_.store ? c_.store->entry(path) : std::nullopt;
    if (!entry) return refuse("file_gone");
    if (entry->kind == pbc::Kind::Folder || entry->availability != pbc::Availability::Ok) return refuse("trash_not_actionable");   // a folder has no versions
    if (!entry->fileid || *entry->fileid <= 0) return refuse("refresh_first");
    path_ = path;
    file_name_ = display_safe(entry->name, 120);
    list_ = pbc::VersionList();
    rows_.clear();
    chosen_ = -1;
    return start_check();
}

QVariantMap VersionsBridge::reload() {
    if (busy_) return refuse("busy");
    if (path_.empty()) return refuse("file_gone");
    return start_check();
}

QVariantMap VersionsBridge::start_check() {
    ++generation_;
    pending_ = false;
    message_.clear();
    result_.clear();
    const auto entry = c_.store ? c_.store->entry(path_) : std::nullopt;
    if (!entry || !entry->fileid) return refuse("file_gone");   // the stored listing no longer has the file
    const pbc::RemoteEntry file = *entry;
    const auto offline_failed = [this] {
        busy_ = false;
        state_ = QStringLiteral("failed");
        message_ = trash_support::message_map("no_network");
        emit changed();
    };
    if (offline() && !c_.ensure_network) {
        offline_failed();
        return refuse("no_network");
    }
    busy_ = true;
    state_ = QStringLiteral("checking");
    emit changed();
    const quint64 generation = generation_;
    const Config c = c_;
    with_network(
        generation,
        [this, c, generation, file] {
            c_.worker->post([this, c, generation, file] {
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
                    if (a.error.ok() && a.value.versions == pbc::FeatureState::On) {
                        auto l = api.list_versions(file);
                        online.record(l.error);
                        checked.listed = true;
                        checked.list_error = l.error;
                        checked.list = std::move(l.value);
                    }
                }
                QMetaObject::invokeMethod(this, [this, generation, checked] { apply_checked(generation, checked); }, Qt::QueuedConnection);
            });
        },
        offline_failed);
    QVariantMap r;
    r.insert(QStringLiteral("ok"), true);
    return r;
}

void VersionsBridge::apply_checked(quint64 generation, const Checked& checked) {
    if (generation != generation_) return;   // the screen was left meanwhile: nothing from this answer is kept
    busy_ = false;
    availability_ = checked.availability;
    availability_map_ = trash_support::availability_map(availability_);
    const auto problem = [this](const pbc::Error& e, const std::string& code) {
        list_ = pbc::VersionList();
        chosen_ = -1;
        rows_.clear();
        const bool plain = trash_support::is_connection_problem(e) || e.key == "listing_too_large" || e.key == "file_gone" || e.key == "refresh_first";
        state_ = plain ? QStringLiteral("failed") : QStringLiteral("explainUnknown");
        message_ = trash_support::message_map(e.key, plain ? std::string() : code);
    };
    if (!checked.error.ok() && trash_support::is_connection_problem(checked.error)) {
        problem(checked.error, availability_.code);
    } else if (availability_.versions == pbc::FeatureState::Off) {
        list_ = pbc::VersionList();
        rows_.clear();
        chosen_ = -1;
        state_ = QStringLiteral("explainOff");
        message_ = trash_support::message_map("versions_unavailable");
    } else if (availability_.versions == pbc::FeatureState::Unknown) {
        list_ = pbc::VersionList();
        rows_.clear();
        chosen_ = -1;
        state_ = QStringLiteral("explainUnknown");   // never "off": the answer was missing or unclear (spec FR-030)
        message_ = trash_support::message_map(checked.error.ok() ? std::string("protocol_error") : checked.error.key, availability_.code);
    } else if (checked.listed && !checked.list_error.ok()) {
        problem(checked.list_error, checked.list.code);
    } else if (checked.listed) {
        list_ = checked.list;
        chosen_ = -1;
        page_ = 0;
        rebuild_rows();
        if (list_.rows.empty()) {
            state_ = QStringLiteral("empty");
            message_ = trash_support::message_map("no_older_versions");
        } else {
            state_ = QStringLiteral("listed");
        }
    }
    emit changed();
}

QVariantMap VersionsBridge::restore() {
    if (busy_) return refuse("busy");
    if (state_ == QLatin1String("explainOff") || state_ == QLatin1String("explainUnknown")) return refuse("versions_unavailable");
    if (chosen_ < 0 || chosen_ >= static_cast<int>(list_.rows.size())) return refuse("version_gone");
    if (offline()) return refuse("no_network");   // the action needs a connection (spec FR-035): nothing is queued
    const pbc::VersionRow& row = list_.rows[static_cast<size_t>(chosen_)];
    const pbc::ConfirmContent content = pbc::confirm_for_version(c_.store->entry(path_) ? c_.store->entry(path_)->name : std::string(), row);
    QVariantMap summary;
    summary.insert(QStringLiteral("kind"), QStringLiteral("restore_version"));
    QStringList names;
    for (const std::string& n : content.shown_names) names << display_safe(n, 60);
    summary.insert(QStringLiteral("names"), names);
    summary.insert(QStringLiteral("dateText"), date_text(content.version_mtime));
    summary.insert(QStringLiteral("sizeText"), content.version_size ? human_size(*content.version_size) : QString());
    summary.insert(QStringLiteral("stage"), content.stage);
    summary.insert(QStringLiteral("destructive"), content.destructive);
    summary.insert(QStringLiteral("defaultCancel"), content.default_cancel);
    pending_ = true;
    QVariantMap r;
    r.insert(QStringLiteral("needsConfirm"), true);
    r.insert(QStringLiteral("summary"), summary);
    return r;
}

QVariantMap VersionsBridge::restoreConfirmed() {
    if (!pending_) return refuse("nothing_pending");
    if (busy_) return refuse("busy");
    if (offline()) return refuse("no_network");
    if (chosen_ < 0 || chosen_ >= static_cast<int>(list_.rows.size())) return refuse("nothing_pending");
    const auto entry = c_.store ? c_.store->entry(path_) : std::nullopt;
    if (!entry) return refuse("file_gone");
    pending_ = false;   // one confirmation, one request
    const pbc::RemoteEntry file = *entry;
    const pbc::VersionList shown = list_;
    const pbc::VersionRow row = list_.rows[static_cast<size_t>(chosen_)];
    busy_ = true;
    emit changed();
    const Config c = c_;
    c_.worker->post([this, c, file, shown, row] {
        auto finished = std::make_shared<Finished>();
        trash_support::Online online(*c.store, *c.http);
        if (!online.ok()) {
            finished->result.error = online.error();
        } else {
            pbc::TrashApi api(online.dav(), online.user_id(), *c.clock);
            finished->result = api.restore_version(file, shown, row, &finished->after);
            online.record(finished->result.error);
            finished->have_after = finished->result.error.ok() && finished->after.file_id != 0;
        }
        QMetaObject::invokeMethod(this, [this, finished] { apply_finished(*finished); }, Qt::QueuedConnection);   // always applied: the request was sent
    });
    QVariantMap r;
    r.insert(QStringLiteral("ok"), true);
    return r;
}

void VersionsBridge::apply_finished(const Finished& f) {
    busy_ = false;
    const pbc::OperationResult& r = f.result;
    result_.clear();
    result_.insert(QStringLiteral("kind"), QStringLiteral("restore_version"));
    result_.insert(QStringLiteral("ok"), r.error.ok());
    result_.insert(QStringLiteral("key"), QString::fromStdString(r.error.key));
    result_.insert(QStringLiteral("code"), QString::fromStdString(r.error.ok() ? std::string() : r.code));
    result_.insert(QStringLiteral("outcomeUnknown"), r.outcome_unknown);
    result_.insert(QStringLiteral("previousNotKept"), r.previous_not_kept);
    if (r.error.ok()) {
        if (f.have_after) {
            list_ = f.after;
            chosen_ = -1;
            page_ = 0;
            rebuild_rows();
            state_ = list_.rows.empty() ? QStringLiteral("empty") : QStringLiteral("listed");
        }
        // the file changed on the server: the cached listing of its folder is dropped (the next visit fetches it, a local copy then shows outdated by the
        // ordinary rule); no file of the reader, no mark and no queue item is touched
        if (c_.store) c_.store->forget_folder_after_change(pbc::parent_of(path_));
    }
    emit changed();
    emit actionFinished(result_);
}
