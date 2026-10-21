#include "browse_bridge.h"

#include <QMetaObject>
#include <QVariantMap>

#include <algorithm>

#include "format_util.h"
#include "pbc/age_text.h"
#include "pbc/entry_state.h"
#include "pbc/local_files.h"
#include "pbc/folder_service.h"
#include "pbc/remote_path.h"
#include "pbc/server_caps.h"
#include "pbc/utf8.h"
#include "worker.h"

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }

bool network_key(const std::string& k) { return k == "no_network" || k == "server_unreachable" || k == "timeout"; }

QString availability_text(pbc::Availability a) {
    switch (a) {
        case pbc::Availability::NoPermission: return QStringLiteral("no_permission");
        case pbc::Availability::Encrypted: return QStringLiteral("encrypted");
        case pbc::Availability::Unsupported: return QStringLiteral("unsupported");
        case pbc::Availability::Unreadable: return QStringLiteral("unreadable");
        default: return QStringLiteral("ok");
    }
}

}  // namespace

BrowseBridge::BrowseBridge(Config config, QObject* parent) : QObject(parent), c_(std::move(config)) {
    const std::string last = c_.store->setting("last_folder");
    if (!last.empty()) path_ = q(pbc::normalize_remote_path(last));
    reload();
}

BrowseBridge::~BrowseBridge() {
    if (c_.worker) c_.worker->wait_idle();
}

QVariantList BrowseBridge::pathSegments() const {
    QVariantList out;
    QVariantMap root;
    root["label"] = QString();
    root["path"] = QStringLiteral("/");
    out.append(root);
    const std::string p = path_.toStdString();
    size_t pos = 1;
    while (pos < p.size()) {
        size_t next = p.find('/', pos);
        if (next == std::string::npos) next = p.size();
        QVariantMap seg;
        seg["label"] = display_safe(p.substr(pos, next - pos), 80);
        seg["path"] = q(p.substr(0, next));
        out.append(seg);
        pos = next + 1;
    }
    return out;
}

QString BrowseBridge::sortKey() const {
    const std::string k = c_.store->setting("sort_key");
    return k == "date" || k == "size" ? q(k) : QStringLiteral("name");
}
bool BrowseBridge::sortAscending() const { return c_.store->setting("sort_dir") != "desc"; }

QString BrowseBridge::listingState() const {
    if (!has_listing_) return QStringLiteral("none");
    if (gone_) return QStringLiteral("gone");
    if (!fetched_at_) return QStringLiteral("not_synced");
    return fresh_ ? QStringLiteral("fresh") : QStringLiteral("cached");
}

QString BrowseBridge::listingAgeKind() const {
    switch (pbc::listing_age(c_.clock->now(), fetched_at_, gone_).kind) {
        case pbc::AgeKind::Gone: return QStringLiteral("gone");
        case pbc::AgeKind::JustNow: return QStringLiteral("just_now");
        case pbc::AgeKind::Minutes: return QStringLiteral("minutes");
        case pbc::AgeKind::Hours: return QStringLiteral("hours");
        case pbc::AgeKind::Days: return QStringLiteral("days");
        default: return QStringLiteral("not_synced");
    }
}
int BrowseBridge::listingAgeCount() const { return pbc::listing_age(c_.clock->now(), fetched_at_, gone_).n; }
bool BrowseBridge::clockSuspect() const { return pbc::listing_age(c_.clock->now(), fetched_at_, gone_).clock_suspect; }

QVariantMap BrowseBridge::row_map(const pbc::RemoteEntry& e, const std::string& source) const {
    QVariantMap m;
    m["name"] = display_safe(e.name);
    m["kind"] = e.kind == pbc::Kind::Folder ? QStringLiteral("folder") : QStringLiteral("file");
    m["sizeText"] = e.kind == pbc::Kind::File && e.size ? human_size(*e.size) : QString();
    m["dateText"] = e.mtime && c_.format_date ? c_.format_date(*e.mtime) : QString();
    m["availability"] = availability_text(e.availability);
    m["reason"] = q(e.reason);
    m["remotePath"] = q(e.path);
    m["folderPath"] = q(pbc::parent_of(e.path));
    m["source"] = q(source);
    m["favorite"] = e.favorite;
    QString state;   // folders have none; one stat per visible file at most
    if (e.kind == pbc::Kind::File) {
        const auto local = c_.store->local_file(e.path);
        const pbc::FileStat st = local && !c_.download_root.empty() ? pbc::stat_file(c_.download_root + "/" + local->local_path) : pbc::FileStat();
        state = QString::fromLatin1(pbc::state_key(pbc::compute_state(&e, local ? &*local : nullptr, st)));
    }
    m["state"] = state;
    m["kept"] = c_.store->in_scope(e.path);
    m["keptDirect"] = c_.store->is_marked(e.path);
    return m;
}

QVariantMap BrowseBridge::overlay_row_map(const pbc::OverlayRow& r) const {
    QVariantMap m;
    if (r.pending_row) {   // it exists only in the queue: no download state, no marks, nothing to look up on the card
        const pbc::RemoteEntry& e = r.entry;
        m["name"] = display_safe(e.name);
        m["kind"] = e.kind == pbc::Kind::Folder ? QStringLiteral("folder") : QStringLiteral("file");
        m["sizeText"] = e.kind == pbc::Kind::File && e.size ? human_size(*e.size) : QString();
        m["dateText"] = QString();
        m["availability"] = QStringLiteral("ok");
        m["reason"] = QString();
        m["remotePath"] = q(e.path);
        m["folderPath"] = q(pbc::parent_of(e.path));
        m["source"] = QString();
        m["favorite"] = false;
        m["state"] = QString();
        m["kept"] = false;
        m["keptDirect"] = false;
    } else {
        m = row_map(r.entry, std::string());
        if (r.pending == "renamed") m["name"] = display_safe(r.entry.name);   // the new name; the path stays the entry's own
    }
    m["pending"] = q(r.pending);
    m["syncState"] = q(r.sync_state);
    m["itemId"] = static_cast<qlonglong>(r.item_id);
    m["pendingRow"] = r.pending_row;
    m["busy"] = r.item_id != 0;   // something is queued for this entry: no second change is queued on top of it
    m["selected"] = std::find(selected_.begin(), selected_.end(), r.entry.path) != selected_.end();
    return m;
}

void BrowseBridge::reload_flat() {
    rows_.clear();
    std::vector<FlatRow> list;
    if (mode_ == QLatin1String("favorites")) {
        for (pbc::RemoteEntry& e : c_.store->favorites()) list.push_back({std::move(e), "favorites"});
        fetched_at_ = c_.store->favorites_fetched_at();
        has_listing_ = fetched_at_.has_value();
        gone_ = false;
        const QString key = sortKey();
        const bool asc = sortAscending();
        std::stable_sort(list.begin(), list.end(), [&](const FlatRow& x, const FlatRow& y) {
            const bool fx = x.entry.kind == pbc::Kind::Folder, fy = y.entry.kind == pbc::Kind::Folder;
            if (fx != fy) return fx;   // folders first, whatever the direction
            if (key == QLatin1String("date")) {
                const int64_t a = x.entry.mtime.value_or(0), b = y.entry.mtime.value_or(0);
                if (a != b) return asc ? a < b : a > b;
            } else if (key == QLatin1String("size")) {
                const int64_t a = x.entry.size.value_or(-1), b = y.entry.size.value_or(-1);
                if (a != b) return asc ? a < b : a > b;
            }
            const std::string a = pbc::sort_key(x.entry.name), b = pbc::sort_key(y.entry.name);
            return key == QLatin1String("name") && !asc ? a > b : a < b;
        });
    } else {
        list = flat_;
        has_listing_ = true;
        fetched_at_.reset();
        gone_ = false;
    }
    const int total = static_cast<int>(list.size());
    page_count_ = std::max(1, (total + page_size_ - 1) / page_size_);
    if (page_ >= page_count_) page_ = page_count_ - 1;
    const int first = page_ * page_size_;
    for (int i = first; i < std::min(total, first + page_size_); ++i) rows_.append(row_map(list[static_cast<size_t>(i)].entry, list[static_cast<size_t>(i)].source));
}

void BrowseBridge::reload() {
    if (mode_ != QLatin1String("folder")) {
        reload_flat();
        return;
    }
    const auto folder = c_.store->folder(path_.toStdString());
    // a folder that only exists in the queue (a new folder) has no listing to fetch: what is queued for it is what there is
    bool pending_folder = false;
    if (!folder)
        for (const pbc::QueueItem& item : c_.store->queue_items_for_source(path_.toStdString()))
            if (item.kind == pbc::ItemKind::Mkdir && pbc::join_remote(item.dst_parent, item.dst_name) == path_.toStdString()) pending_folder = true;
    has_listing_ = folder.has_value() || pending_folder;
    fetched_at_ = folder ? folder->fetched_at : std::nullopt;
    gone_ = folder ? folder->gone : false;
    rows_.clear();
    folder_kept_ = path_ != QLatin1String("/") && c_.store->in_scope(path_.toStdString());
    folder_kept_direct_ = path_ != QLatin1String("/") && c_.store->is_marked(path_.toStdString());
    int total = 0;
    if (has_listing_) {
        pbc::SortKey key = pbc::SortKey::Name;
        if (sortKey() == QLatin1String("date")) key = pbc::SortKey::Date;
        else if (sortKey() == QLatin1String("size")) key = pbc::SortKey::Size;
        const pbc::OverlayPage p = c_.store->entries_page_overlay(path_.toStdString(), key, sortAscending(), page_ * page_size_, page_size_);
        total = p.total;
        page_count_ = std::max(1, (total + page_size_ - 1) / page_size_);
        if (page_ >= page_count_) {   // the folder got shorter
            page_ = page_count_ - 1;
            reload();
            return;
        }
        for (const pbc::OverlayRow& r : p.rows) rows_.append(overlay_row_map(r));
    } else {
        page_count_ = 1;
        page_ = 0;
    }
}

void BrowseBridge::setPageSize(int n) {
    n = std::max(1, std::min(n, 100));
    if (n == page_size_) return;
    const int first = page_ * page_size_;
    page_size_ = n;
    page_ = first / page_size_;
    reload();
    emit changed();
}

void BrowseBridge::open(const QString& remotePath) {
    const std::string p = pbc::normalize_remote_path(remotePath.toStdString());
    if (!c_.store->entry(p)) {   // not on the server as far as we know: a row that exists only in the queue
        for (const pbc::QueueItem& item : c_.store->queue_items_for_source(p)) {
            if (item.src_remote == p) continue;   // the item is about an entry of this name as a source: not a pending row
            if (item.entry_kind == pbc::Kind::File) {
                emit showQueueItem(static_cast<int>(item.id));
                return;
            }
        }
    }
    if (const auto entry = c_.store->entry(p)) {
        if (entry->availability != pbc::Availability::Ok) {
            emit showReason(q(entry->reason));
            return;
        }
        if (entry->kind == pbc::Kind::File) {
            emit showDetails(q(p));
            return;
        }
    }
    if (busy_) return;
    leave_selection();
    mode_ = QStringLiteral("folder");
    more_ = false;
    path_ = q(p);
    page_ = 0;
    error_.clear();
    skipped_ = 0;
    fresh_ = false;
    reload();
    if (has_listing_) {
        offline_ = false;
        c_.store->set_setting("last_folder", p);
        emit changed();
        return;
    }
    fetch(false);
}

void BrowseBridge::up() {
    if (!canGoUp()) return;
    open(q(pbc::parent_of(path_.toStdString())));
}

void BrowseBridge::refresh() {
    if (busy_) return;
    if (mode_ == QLatin1String("favorites")) fetch_favorites();
    else if (mode_ == QLatin1String("folder")) fetch(true);
}

void BrowseBridge::fetch_favorites() {
    busy_ = true;
    error_.clear();
    emit changed();
    c_.ensure_network([this](bool online) {
        if (!online) {
            busy_ = false;
            error_ = QStringLiteral("no_network");
            offline_ = true;
            reload();
            emit changed();
            return;
        }
        c_.worker->post([this] {
            pbc::FolderService svc(*c_.store, *c_.http, *c_.clock);
            const pbc::FlatResult r = svc.refresh_favorites();
            QMetaObject::invokeMethod(this, [this, r] {
                busy_ = false;
                error_ = r.error.ok() ? QString() : q(r.error.key);
                offline_ = !r.error.ok() && network_key(r.error.key);
                more_ = r.more;
                reload();
                emit changed();
            }, Qt::QueuedConnection);
        });
    });
}

void BrowseBridge::showFavorites() {
    if (busy_) return;
    leave_selection();
    mode_ = QStringLiteral("favorites");
    more_ = false;
    page_ = 0;
    error_.clear();
    offline_ = false;
    reload();
    emit changed();
    if (!has_listing_) fetch_favorites();   // never fetched: once, when the network can be had
}

void BrowseBridge::searchOffline(const QString& term) {
    if (busy_) return;
    mode_ = QStringLiteral("search");
    term_ = term;
    more_ = false;
    page_ = 0;
    error_.clear();
    flat_.clear();
    for (pbc::SearchHit& h : c_.store->search_cached(term.toStdString(), 200)) flat_.push_back({std::move(h.entry), std::move(h.source)});
    reload();
    emit changed();
}

void BrowseBridge::searchServer(const QString& term) {
    if (busy_) return;
    mode_ = QStringLiteral("search");
    term_ = term;
    more_ = false;
    page_ = 0;
    error_.clear();
    busy_ = true;
    emit changed();
    const std::string t = term.toStdString();
    c_.ensure_network([this, t](bool online) {
        if (!online) {
            busy_ = false;
            error_ = QStringLiteral("no_network");
            offline_ = true;
            reload();
            emit changed();
            return;
        }
        c_.worker->post([this, t] {
            pbc::FolderService svc(*c_.store, *c_.http, *c_.clock);
            const pbc::FlatResult r = svc.search_server(t);
            QMetaObject::invokeMethod(this, [this, r] {
                busy_ = false;
                error_ = r.error.ok() ? QString() : q(r.error.key);
                offline_ = !r.error.ok() && network_key(r.error.key);
                if (r.error.ok()) {
                    flat_.clear();
                    for (const pbc::RemoteEntry& e : r.entries) flat_.push_back({e, "server"});
                    more_ = r.more;
                }
                reload();
                emit changed();
            }, Qt::QueuedConnection);
        });
    });
}

void BrowseBridge::showFolder() {
    if (mode_ == QLatin1String("folder")) return;
    mode_ = QStringLiteral("folder");
    more_ = false;
    page_ = 0;
    error_.clear();
    reload();
    emit changed();
}

void BrowseBridge::fetch(bool always) {
    busy_ = true;
    error_.clear();
    emit changed();
    const std::string path = path_.toStdString();
    c_.ensure_network([this, path, always](bool online) {
        if (!online) {
            busy_ = false;
            error_ = QStringLiteral("no_network");
            offline_ = true;
            reload();
            emit changed();
            return;
        }
        c_.worker->post([this, path, always] {
            pbc::FolderService svc(*c_.store, *c_.http, *c_.clock);
            const pbc::FolderResult r = svc.load(path, always ? pbc::Fetch::Always : pbc::Fetch::IfMissing);
            QMetaObject::invokeMethod(this, [this, path, r] {
                busy_ = false;
                if (path_.toStdString() != path) return;   // the user moved on while the answer was on its way
                error_ = r.error.ok() ? QString() : q(r.error.key);
                offline_ = !r.error.ok() && network_key(r.error.key);
                fresh_ = r.fetched;
                skipped_ = r.skipped;
                if (r.error.ok()) c_.store->set_setting("last_folder", path);
                reload();
                emit changed();
            }, Qt::QueuedConnection);
        });
    });
}

void BrowseBridge::nextPage() { goToPage(page_ + 1); }
void BrowseBridge::previousPage() { goToPage(page_ - 1); }

void BrowseBridge::goToPage(int page) {
    page = std::max(0, std::min(page, page_count_ - 1));
    if (page == page_) return;
    page_ = page;
    reload();
    emit changed();
}

void BrowseBridge::setSort(const QString& key, bool ascending) {
    if (key != QLatin1String("name") && key != QLatin1String("date") && key != QLatin1String("size")) return;
    c_.store->set_setting("sort_key", key.toStdString());
    c_.store->set_setting("sort_dir", ascending ? "asc" : "desc");
    page_ = 0;
    reload();
    emit changed();
}

QStringList BrowseBridge::selectedPaths() const {
    QStringList out;
    for (const std::string& p : selected_) out << q(p);
    return out;
}

bool BrowseBridge::deleteOffered() const {
    const auto caps = c_.store->server_caps();
    return caps && pbc::delete_offered(*caps);
}

void BrowseBridge::leave_selection() {
    selecting_ = false;
    selected_.clear();
}

void BrowseBridge::setSelecting(bool on) {
    if (on == selecting_) return;
    selecting_ = on;
    if (!on) selected_.clear();
    reload();
    emit changed();
}

void BrowseBridge::toggleSelect(const QString& remotePath) {
    if (!selecting_ || mode_ != QLatin1String("folder")) return;
    const std::string p = remotePath.toStdString();
    const auto it = std::find(selected_.begin(), selected_.end(), p);
    if (it != selected_.end()) {
        selected_.erase(it);
    } else {
        bool selectable = false;
        for (const QVariant& v : rows_) {
            const QVariantMap m = v.toMap();
            if (m.value("remotePath").toString().toStdString() == p && !m.value("busy").toBool() && !m.value("pendingRow").toBool() &&
                m.value("availability").toString() == QLatin1String("ok"))
                selectable = true;
        }
        if (!selectable) return;
        selected_.push_back(p);
    }
    reload();
    emit changed();
}

void BrowseBridge::selectAllOnPage() {
    if (!selecting_ || mode_ != QLatin1String("folder")) return;
    for (const QVariant& v : rows_) {
        const QVariantMap m = v.toMap();
        const std::string p = m.value("remotePath").toString().toStdString();
        if (m.value("busy").toBool() || m.value("pendingRow").toBool() || m.value("availability").toString() != QLatin1String("ok")) continue;
        if (std::find(selected_.begin(), selected_.end(), p) == selected_.end()) selected_.push_back(p);
    }
    reload();
    emit changed();
}

void BrowseBridge::clearSelection() {
    selected_.clear();
    reload();
    emit changed();
}

void BrowseBridge::refreshRows() {
    reload();
    emit changed();
}
