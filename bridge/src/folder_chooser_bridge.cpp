#include "folder_chooser_bridge.h"

#include <QMetaObject>

#include <algorithm>

#include "format_util.h"
#include "pbc/folder_service.h"
#include "pbc/remote_path.h"
#include "worker.h"

FolderChooserBridge::FolderChooserBridge(Config config, QObject* parent) : QObject(parent), c_(std::move(config)) { reload(); }

FolderChooserBridge::~FolderChooserBridge() {
    if (c_.worker) c_.worker->wait_idle();   // no job may call back into a bridge that is gone
}

void FolderChooserBridge::setPageSize(int n) {
    n = std::max(1, std::min(n, 100));
    if (n == page_size_) return;
    const int first = page_ * page_size_;
    page_size_ = n;
    page_ = first / page_size_;
    reload();
    emit changed();
}

QVariantList FolderChooserBridge::pathSegments() const {
    QVariantList out;
    QVariantMap root;
    root["label"] = QString();
    root["path"] = QStringLiteral("/");
    out.append(root);
    std::string built;
    size_t pos = 1;
    while (pos < path_.size()) {
        const size_t next = path_.find('/', pos);
        const std::string seg = path_.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        built += "/" + seg;
        QVariantMap m;
        m["label"] = display_safe(seg, 60);
        m["path"] = QString::fromStdString(built);
        out.append(m);
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return out;
}

void FolderChooserBridge::reload() {
    all_.clear();
    const auto folder = c_.store->folder(path_);
    has_listing_ = folder.has_value() && folder->fetched_at.has_value();
    const pbc::OverlayPage p = c_.store->entries_page_overlay(path_, pbc::SortKey::Name, true, 0, 2000);
    for (const pbc::OverlayRow& r : p.rows) {
        if (r.entry.kind != pbc::Kind::Folder || r.entry.availability != pbc::Availability::Ok) continue;
        if (r.pending == "deleting" || r.pending == "moving") continue;   // not a place to go to any more
        QVariantMap m;
        m["name"] = display_safe(r.entry.name, 120);
        m["remotePath"] = QString::fromStdString(r.pending_row || r.item_id != 0 ? pbc::join_remote(path_, r.entry.name) : r.entry.path);
        m["pending"] = r.pending_row;
        all_.push_back(m);
        if (r.pending_row) has_listing_ = true;
    }
    page_count_ = std::max(1, (static_cast<int>(all_.size()) + page_size_ - 1) / page_size_);
    page_ = std::min(page_, page_count_ - 1);
    rows_.clear();
    for (int i = page_ * page_size_; i < std::min<int>(static_cast<int>(all_.size()), (page_ + 1) * page_size_); ++i) rows_.append(all_[static_cast<size_t>(i)]);
}

void FolderChooserBridge::open(const QString& remotePath) {
    path_ = remotePath.isEmpty() ? std::string("/") : pbc::normalize_remote_path(remotePath.toStdString());
    page_ = 0;
    error_.clear();
    reload();
    emit changed();
    fetch_if_missing();
}

void FolderChooserBridge::up() {
    if (path_ == "/") return;
    open(QString::fromStdString(pbc::parent_of(path_)));
}

void FolderChooserBridge::refresh() {
    reload();
    emit changed();
}

void FolderChooserBridge::nextPage() {
    if (page_ + 1 >= page_count_) return;
    ++page_;
    reload();
    emit changed();
}

void FolderChooserBridge::previousPage() {
    if (page_ == 0) return;
    --page_;
    reload();
    emit changed();
}

void FolderChooserBridge::fetch_if_missing() {
    if (has_listing_ || busy_ || !c_.worker) return;
    if (offline()) return;   // stored listings only
    busy_ = true;
    emit changed();
    const std::string path = path_;
    const auto go = [this, path](bool online) {
        if (!online) {
            busy_ = false;
            emit changed();
            return;
        }
        c_.worker->post([this, path] {
            pbc::FolderService service(*c_.store, *c_.http, *c_.clock);
            const pbc::FolderResult r = service.load(path, pbc::Fetch::IfMissing);
            QMetaObject::invokeMethod(this, [this, path, r] {
                busy_ = false;
                if (path == path_) {
                    error_ = r.error.ok() ? QString() : QString::fromStdString(r.error.key);
                    reload();
                }
                emit changed();
            }, Qt::QueuedConnection);
        });
    };
    if (c_.ensure_network) c_.ensure_network(go);
    else go(true);
}
