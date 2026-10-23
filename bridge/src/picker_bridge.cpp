#include "picker_bridge.h"

#include <algorithm>

#include <sys/stat.h>

#include <cstdlib>

#include "format_util.h"
#include "pbc/local_files.h"
#include "pbc/new_name.h"
#include "pbc/queue_types.h"
#include "pbc/sync_rules.h"
#include "pbc/upload_source.h"
#include "pbc/utf8.h"

namespace {

std::string base_name(const std::string& path) { return path.substr(path.rfind('/') + 1); }

std::string join_path(const std::string& dir, const std::string& name) {
    std::string d = dir;
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    return d == "/" ? "/" + name : d + "/" + name;
}

std::string real_path(const std::string& path) {
    char* p = ::realpath(path.c_str(), nullptr);
    if (!p) return std::string();
    std::string out(p);
    std::free(p);
    return out;
}

bool same_folder(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    const std::string ra = real_path(a), rb = real_path(b);
    return !ra.empty() && ra == rb;
}

bool inside_or_equal(const std::string& path, const std::string& folder) {
    if (folder.empty()) return false;
    const std::string rp = real_path(path), rf = real_path(folder);
    if (rp.empty() || rf.empty()) return false;
    return rp == rf || (rp.size() > rf.size() && rp.compare(0, rf.size(), rf) == 0 && rp[rf.size()] == '/');
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

void PickerBridge::setFolderMode(bool on) {
    if (on == folder_mode_) return;
    folder_mode_ = on;
    clear();
    emit folderModeChanged();
}

// Folders only. The app's data folder is hidden by the core's listing; here it is added back, disabled, so that the user sees why it is not offered.
QVariantList PickerBridge::list_folders(const QString& path) {
    QVariantList out;
    const std::string dir = path.toStdString();
    if (inside_or_equal(dir, c_.downloadRoot)) return out;   // the download folder is not browsed
    const pbc::SourceListing l = pbc::list_source_dir(dir, c_.rules, {});
    if (l.unreadable) return out;
    struct Row {
        std::string name, sort;
        bool disabled = false;
    };
    std::vector<Row> rows;
    for (const pbc::SourceEntry& e : l.entries) {
        if (e.kind != pbc::SourceKind::Folder) continue;
        const std::string abs = join_path(dir, e.name);
        rows.push_back({e.name, pbc::sort_key(e.name), same_folder(abs, c_.downloadRoot)});
    }
    for (const std::string& denied : c_.rules.deny) {   // the app's data folder, listed disabled in the folder that holds it
        const size_t slash = denied.rfind('/');
        if (slash == std::string::npos || slash == 0) continue;
        if (!same_folder(denied.substr(0, slash), dir)) continue;
        const std::string name = denied.substr(slash + 1);
        struct stat st;
        if (name.empty() || ::lstat(denied.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        rows.push_back({name, pbc::sort_key(name), true});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.sort != b.sort ? a.sort < b.sort : a.name < b.name; });
    for (const Row& r : rows) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), display_safe(r.name));
        m.insert(QStringLiteral("path"), QString::fromStdString(join_path(dir, r.name)));
        m.insert(QStringLiteral("dir"), true);
        m.insert(QStringLiteral("size"), 0LL);
        m.insert(QStringLiteral("sizeText"), QString());
        m.insert(QStringLiteral("selected"), false);
        m.insert(QStringLiteral("disabled"), r.disabled);
        m.insert(QStringLiteral("reason"), r.disabled ? QStringLiteral("pair_folder_forbidden") : QString());
        out.append(m);
    }
    return out;
}

QVariantMap PickerBridge::chooseFolder(const QString& path) {
    QVariantMap out;
    out.insert(QStringLiteral("ok"), false);
    out.insert(QStringLiteral("path"), QString());
    const std::string p = path.toStdString();
    if (pbc::reader_folder_forbidden(p, c_.rules, c_.downloadRoot)) {
        out.insert(QStringLiteral("errorKey"), QStringLiteral("pair_folder_forbidden"));
        return out;
    }
    const auto allowed = pbc::check_source_path(p, c_.rules);
    if (!allowed.ok()) {
        out.insert(QStringLiteral("errorKey"), allowed.error.key == "source_unreadable" ? QStringLiteral("reader_folder_missing") : QStringLiteral("source_not_allowed"));
        return out;
    }
    struct stat st;
    if (::stat(allowed.value.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        out.insert(QStringLiteral("errorKey"), QStringLiteral("source_not_allowed"));
        return out;
    }
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("path"), QString::fromStdString(allowed.value));
    out.insert(QStringLiteral("errorKey"), QString());
    return out;
}

QVariantMap PickerBridge::createFolder(const QString& parent, const QString& name) {
    QVariantMap out;
    const auto made = pbc::create_picker_folder(c_.rules, parent.toStdString(), name.toStdString());
    out.insert(QStringLiteral("ok"), made.ok());
    out.insert(QStringLiteral("path"), made.ok() ? QString::fromStdString(made.value) : QString());
    out.insert(QStringLiteral("errorKey"), made.ok() ? QString() : QString::fromStdString(made.error.key));
    return out;
}

QVariantList PickerBridge::listDirectory(const QString& path) {
    if (folder_mode_) return list_folders(path);
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
    if (folder_mode_) return QStringLiteral("source_not_allowed");   // this mode picks one folder, never files
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
