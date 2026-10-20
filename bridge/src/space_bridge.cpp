#include "space_bridge.h"

#include <QMetaObject>
#include <QVariantMap>

#include <algorithm>

#include "format_util.h"
#include "pbc/entry_state.h"
#include "pbc/local_files.h"
#include "worker.h"

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }

}  // namespace

SpaceBridge::SpaceBridge(Config config, QObject* parent) : QObject(parent), c_(std::move(config)) { reload(); }

SpaceBridge::~SpaceBridge() {
    if (c_.worker) c_.worker->wait_idle();
}

void SpaceBridge::reload() {
    total_ = c_.store->local_file_count();
    page_count_ = std::max(1, (total_ + page_size_ - 1) / page_size_);
    page_ = std::max(0, std::min(page_, page_count_ - 1));
    items_.clear();
    for (const pbc::LocalFile& lf : c_.store->local_files(page_ * page_size_, page_size_)) {
        const pbc::FileStat st = pbc::stat_file(c_.download_root + "/" + lf.local_path);
        const auto entry = c_.store->entry(lf.remote_path);
        const pbc::EntryState state = pbc::compute_state(entry ? &*entry : nullptr, &lf, st);
        QVariantMap m;
        m["name"] = display_safe(lf.remote_path.substr(lf.remote_path.rfind('/') + 1));
        m["remotePath"] = q(lf.remote_path);
        m["sizeText"] = human_size(st.exists ? st.size : lf.size);
        m["state"] = QString::fromLatin1(pbc::state_key(state));
        m["changedLocally"] = state == pbc::EntryState::ChangedOnReader || state == pbc::EntryState::Conflict;
        items_.append(m);
    }
    used_ = human_size(c_.store->local_files_bytes());
    const auto free_bytes = c_.storage ? c_.storage->free_bytes(c_.download_root) : std::nullopt;
    free_ = free_bytes ? human_size(*free_bytes) : QString();
    cache_ = human_size(c_.store->database_bytes());
}

void SpaceBridge::setPageSize(int n) {
    n = std::max(3, std::min(n, 100));
    if (n == page_size_) return;
    const int first = page_ * page_size_;
    page_size_ = n;
    page_ = first / page_size_;
    reload();
    emit changed();
}

void SpaceBridge::refresh() {
    reload();
    emit changed();
}

void SpaceBridge::nextPage() {
    if (page_ + 1 >= page_count_) return;
    ++page_;
    reload();
    emit changed();
}

void SpaceBridge::previousPage() {
    if (page_ == 0) return;
    --page_;
    reload();
    emit changed();
}

void SpaceBridge::remove(const QString& remotePath, bool confirmedLoss) {
    const pbc::Error e = pbc::remove_local_copy(*c_.store, c_.download_root, remotePath.toStdString(), confirmedLoss);
    reload();
    emit changed();
    emit removeFinished(e.ok(), q(e.key));
}

void SpaceBridge::clearCache() {
    if (busy_) return;
    busy_ = true;
    emit changed();
    c_.worker->post([this] {
        const pbc::Error e = c_.store->clear_cache();
        QMetaObject::invokeMethod(this, [this, ok = e.ok()] {
            busy_ = false;
            reload();
            emit changed();
            emit cacheCleared(ok);
        }, Qt::QueuedConnection);
    });
}
