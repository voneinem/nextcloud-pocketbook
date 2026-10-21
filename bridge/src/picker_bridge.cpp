#include "picker_bridge.h"

#include <algorithm>

#include "format_util.h"
#include "pbc/new_name.h"
#include "pbc/queue_types.h"
#include "pbc/upload_source.h"

namespace {

std::string base_name(const std::string& path) { return path.substr(path.rfind('/') + 1); }

std::string join_path(const std::string& dir, const std::string& name) {
    std::string d = dir;
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    return d == "/" ? "/" + name : d + "/" + name;
}

}  // namespace

PickerBridge::PickerBridge(Config config, QObject* parent) : QObject(parent), c_(std::move(config)) {}

QString PickerBridge::view() const {
    return c_.store && c_.store->setting("picker_view") == "tile" ? QStringLiteral("tile") : QStringLiteral("list");
}

void PickerBridge::setView(const QString& view) {
    if (!c_.store || (view != QLatin1String("list") && view != QLatin1String("tile")) || view == this->view()) return;
    if (!c_.store->set_setting("picker_view", view.toStdString()).ok()) return;
    emit viewChanged();
}

int PickerBridge::fileCount() const {
    int n = 0;
    for (const Pick& p : sel_) n += p.files;
    return n;
}
int PickerBridge::folderCount() const {
    int n = 0;
    for (const Pick& p : sel_) n += p.folders;
    return n;
}
qlonglong PickerBridge::byteCount() const {
    qlonglong n = 0;
    for (const Pick& p : sel_) n += p.bytes;
    return n;
}
QString PickerBridge::byteText() const { return human_size(byteCount()); }
int PickerBridge::items_in_selection() const { return fileCount() + folderCount(); }

int PickerBridge::maxItems() const {
    if (!c_.store) return pbc::kMaxQueueItems;
    return std::max(0, pbc::kMaxQueueItems - c_.store->queue_page(0, 0).total - items_in_selection());
}

std::vector<PickerBridge::Pick>::iterator PickerBridge::find(const std::string& path) {
    return std::find_if(sel_.begin(), sel_.end(), [&](const Pick& p) { return p.path == path; });
}

QVariantList PickerBridge::listDirectory(const QString& path) {
    QVariantList out;
    std::vector<std::string> top;
    for (const QString& t : c_.topFolders) top.push_back(t.toStdString());
    const pbc::SourceListing l = pbc::list_source_dir(path.toStdString(), c_.rules, top);
    if (l.unreadable) return out;
    for (const pbc::SourceEntry& e : l.entries) {
        const std::string abs = join_path(path.toStdString(), e.name);
        QVariantMap m;
        m.insert(QStringLiteral("name"), display_safe(e.name));
        m.insert(QStringLiteral("path"), QString::fromStdString(abs));
        m.insert(QStringLiteral("dir"), e.kind == pbc::SourceKind::Folder);
        m.insert(QStringLiteral("size"), static_cast<qlonglong>(e.size));
        m.insert(QStringLiteral("sizeText"), e.kind == pbc::SourceKind::File ? human_size(e.size) : QString());
        m.insert(QStringLiteral("selected"), std::any_of(sel_.begin(), sel_.end(), [&](const Pick& p) { return p.path == abs; }));
        out.append(m);
    }
    return out;
}

QString PickerBridge::toggle(const QString& path) {
    const std::string p = path.toStdString();
    const auto it = find(p);
    if (it != sel_.end()) {
        sel_.erase(it);
        emit selectionChanged();
        return QString();
    }
    const auto identity = pbc::inspect_source(p, c_.rules);
    if (!identity.ok()) return QString::fromStdString(identity.error.key);
    const pbc::ServerCaps caps = c_.store ? c_.store->server_caps().value_or(pbc::ServerCaps()) : pbc::ServerCaps();
    if (!pbc::check_new_name(base_name(p), caps).ok) return QStringLiteral("name_forbidden");
    if (maxItems() < 1) return QStringLiteral("queue_full");
    Pick pick;
    pick.path = p;
    pick.files = 1;
    pick.bytes = identity.value.size;
    sel_.push_back(pick);
    emit selectionChanged();
    return QString();
}

QVariantMap PickerBridge::addFolder(const QString& path) {
    QVariantMap out;
    out.insert(QStringLiteral("ok"), false);
    out.insert(QStringLiteral("overLimit"), false);
    out.insert(QStringLiteral("errorKey"), QString());
    const std::string p = path.toStdString();
    const auto it = find(p);
    if (it != sel_.end()) {   // a second add unselects it
        sel_.erase(it);
        out.insert(QStringLiteral("ok"), true);
        out.insert(QStringLiteral("folders"), 0);
        out.insert(QStringLiteral("files"), 0);
        out.insert(QStringLiteral("bytes"), 0);
        out.insert(QStringLiteral("bytesText"), QString());
        emit selectionChanged();
        return out;
    }
    const auto allowed = pbc::check_source_path(p, c_.rules);
    if (!allowed.ok()) {
        out.insert(QStringLiteral("errorKey"), QString::fromStdString(allowed.error.key));
        return out;
    }
    const pbc::SourceWalk walk = pbc::walk_source(allowed.value, c_.rules);
    if (walk.entries.empty()) {
        out.insert(QStringLiteral("errorKey"), QStringLiteral("source_unreadable"));   // not a folder
        return out;
    }
    if (walk.over_limit) {
        out.insert(QStringLiteral("overLimit"), true);
        out.insert(QStringLiteral("errorKey"), QStringLiteral("queue_full"));
        return out;
    }
    if (walk.folders + walk.files > maxItems()) {
        out.insert(QStringLiteral("errorKey"), QStringLiteral("queue_full"));
        return out;
    }
    Pick pick;
    pick.path = p;
    pick.folder = true;
    pick.files = walk.files;
    pick.folders = walk.folders;
    pick.bytes = walk.bytes;
    sel_.push_back(pick);
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("folders"), walk.folders);
    out.insert(QStringLiteral("files"), walk.files);
    out.insert(QStringLiteral("bytes"), static_cast<qlonglong>(walk.bytes));
    out.insert(QStringLiteral("bytesText"), human_size(walk.bytes));
    emit selectionChanged();
    return out;
}

void PickerBridge::clear() {
    if (sel_.empty()) return;
    sel_.clear();
    emit selectionChanged();
}

QStringList PickerBridge::selectedFiles() const {
    QStringList out;
    for (const Pick& p : sel_)
        if (!p.folder) out << QString::fromStdString(p.path);
    return out;
}

QStringList PickerBridge::selectedFolders() const {
    QStringList out;
    for (const Pick& p : sel_)
        if (p.folder) out << QString::fromStdString(p.path);
    return out;
}
