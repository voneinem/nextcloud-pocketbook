#include "pbc/local_files.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "pbc/local_name.h"
#include "pbc/log.h"
#include "pbc/sync_scan.h"
#include "pbc/utf8.h"

namespace pbc {

FileStat stat_file(const std::string& path) {
    FileStat s;
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) return s;
    s.exists = true;
    s.regular = S_ISREG(st.st_mode);
    s.size = static_cast<int64_t>(st.st_size);
    s.mtime = static_cast<int64_t>(st.st_mtime);
    return s;
}

bool local_copy_changed(const LocalFile& recorded, const FileStat& now) {
    if (!now.exists || !now.regular) return true;
    return now.size != recorded.size || now.mtime != recorded.mtime;
}

Error keep_mine(Store& store, const std::string& remote_path, const std::string& server_etag) {
    if (!store.local_file(remote_path)) return make_error(ErrorClass::Storage, "not_found", "there is no copy of this file");
    return store.set_seen_etag(remote_path, server_etag);
}

namespace {

std::vector<std::string> split(const std::string& p) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos < p.size()) {
        if (p[pos] == '/') { ++pos; continue; }
        size_t next = p.find('/', pos);
        if (next == std::string::npos) next = p.size();
        parts.push_back(p.substr(pos, next - pos));
        pos = next;
    }
    return parts;
}

}  // namespace

Result<std::string> resolve_local_file(const std::string& root, const std::string& rel) {
    Result<std::string> r;
    auto unsafe = [&](const char* key) {
        r.error = make_error(ErrorClass::Storage, key, "the file is not a regular file inside the download folder");
        return r;
    };
    if (rel.empty() || rel[0] == '/') return unsafe("path_unsafe");
    struct stat st;
    if (::lstat(root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return unsafe("path_unsafe");
    const std::vector<std::string> parts = split(rel);
    if (parts.empty()) return unsafe("path_unsafe");
    std::string path = root;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i] == "." || parts[i] == "..") return unsafe("path_unsafe");
        path += "/" + parts[i];
        if (::lstat(path.c_str(), &st) != 0) return unsafe(errno == ENOENT ? "not_found" : "path_unsafe");
        if (S_ISLNK(st.st_mode)) return unsafe("path_unsafe");
        if (i + 1 < parts.size() ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)) return unsafe("path_unsafe");
    }
    r.value = path;
    return r;
}

LocalFile as_local_file(const std::string& remote_path, const SyncedCopy& copy) {
    LocalFile f;
    f.remote_path = remote_path;
    f.local_path = copy.local_rel;
    f.remote_etag = copy.etag;
    f.size = copy.size;
    f.mtime = copy.mtime;
    return f;
}

Error remove_local_copy(Store& store, const std::string& root, const std::string& remote_path, bool confirmed_loss) {
    const auto copy = store.local_file(remote_path);
    if (!copy) {
        // a file a folder pair keeps up to date is the pair's, not a downloaded copy: it is removed only by a deletion the user confirmed in a plan
        if (store.synced_copy(remote_path)) return make_error(ErrorClass::Unsupported, "managed_by_sync", "this file belongs to a folder pair");
        return {};   // nothing recorded: nothing to remove
    }
    const auto abs = resolve_local_file(root, copy->local_path);
    if (!abs.ok()) {
        if (abs.error.key == "not_found") return store.forget_local_file(remote_path);   // already gone from the card: only the record goes
        return abs.error;                                                                 // never delete a path that is not safe
    }
    if (local_copy_changed(*copy, stat_file(abs.value)) && !confirmed_loss)
        return make_error(ErrorClass::Storage, "local_changed", "the copy has changes that would be lost");
    if (::unlink(abs.value.c_str()) != 0 && errno != ENOENT) return make_error(ErrorClass::Storage, "write_failed", "the file could not be removed");
    log::event("space.remove", {log::count("files", 1)});
    return store.forget_local_file(remote_path);
}

Error ensure_directories(const std::string& root, const std::string& rel_dir) {
    struct stat st;
    if (::lstat(root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return make_error(ErrorClass::Storage, "path_unsafe", "the download folder is missing or is a link");
    std::string path = root;
    for (const std::string& part : split(rel_dir)) {
        if (part == "." || part == "..") return make_error(ErrorClass::Storage, "path_unsafe", "unusable folder name");
        path += "/" + part;
        if (::lstat(path.c_str(), &st) == 0) {
            if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) return make_error(ErrorClass::Storage, "path_unsafe", "a link or a file is where a folder is needed");
            continue;
        }
        if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
            return make_error(ErrorClass::Storage, errno == ENOSPC || errno == EDQUOT ? "storage_full" : (errno == EROFS || errno == EACCES ? "storage_readonly" : "write_failed"),
                              "a folder could not be created");
    }
    return {};
}

namespace {

std::string real_of(const std::string& path) {
    char* p = ::realpath(path.c_str(), nullptr);
    if (!p) return std::string();
    std::string out(p);
    std::free(p);
    return out;
}

}  // namespace

Result<std::string> create_picker_folder(const SourceRules& rules, const std::string& parent, const std::string& name) {
    Result<std::string> r;
    auto fail = [&](ErrorClass cls, const char* key, const char* detail) {
        r.error = make_error(cls, key, detail);
        return r;
    };
    // the name: already one safe, visible path component
    if (name.empty() || name.size() > 120 || name[0] == '.' || name.find('\0') != std::string::npos || sanitize_component(name) != name)
        return fail(ErrorClass::Server, "name_forbidden", "the name cannot be used for a folder on the reader");

    // the parent: the picker root or a folder inside it
    std::string root = rules.root;
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    std::string lexical = parent;
    while (lexical.size() > 1 && lexical.back() == '/') lexical.pop_back();
    std::string parent_real;
    const std::string root_real = real_of(root);
    if (root_real.empty()) return fail(ErrorClass::Storage, "source_not_allowed", "the storage root is not available");
    if (!lexical.empty() && lexical[0] == '/' && (lexical == root || lexical == root_real)) {
        parent_real = root_real;
    } else {
        const Result<std::string> allowed = check_source_path(parent, rules);
        if (!allowed.ok()) return fail(ErrorClass::Storage, allowed.error.key.c_str(), "the parent folder cannot be used");
        parent_real = allowed.value;
    }
    struct stat st;
    if (::stat(parent_real.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return fail(ErrorClass::Storage, "source_not_allowed", "the parent is not a folder");

    // an existing entry of this name, whatever its type and whatever the case
    const std::string folded = case_fold(name);
    if (DIR* d = ::opendir(parent_real.c_str())) {
        while (dirent* e = ::readdir(d)) {
            if (case_fold(e->d_name) == folded) {
                ::closedir(d);
                return fail(ErrorClass::Server, "name_exists", "something with this name is already there");
            }
        }
        ::closedir(d);
    } else {
        return fail(ErrorClass::Storage, "source_unreadable", "the parent folder cannot be read");
    }

    // created relative to the opened parent, so that no link can be swapped in between
    const int fd = ::open(parent_real.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return fail(ErrorClass::Storage, "source_not_allowed", "the parent folder cannot be opened");
    const int rc = ::mkdirat(fd, name.c_str(), 0755);
    const int err = errno;
    ::close(fd);
    if (rc != 0) {
        if (err == EEXIST) return fail(ErrorClass::Server, "name_exists", "something with this name is already there");
        return fail(ErrorClass::Storage, err == ENOSPC || err == EDQUOT ? "storage_full" : (err == EROFS || err == EACCES ? "storage_readonly" : "write_failed"),
                    "the folder could not be created");
    }
    r.value = (parent_real == "/" ? std::string() : parent_real) + "/" + name;
    return r;
}

Error remove_pair_copy(const std::string& reader_folder, const std::string& local_rel, const SyncRecord& record) {
    auto refuse = [](const char* key) { return make_error(ErrorClass::Storage, key, key); };
    struct stat st;
    if (reader_folder.empty() || reader_folder[0] != '/' || ::lstat(reader_folder.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return refuse("path_unsafe");
    // the relative path: components that are neither empty nor dots, no control character
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= local_rel.size()) {
        const size_t slash = local_rel.find('/', pos);
        const std::string part = local_rel.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (part.empty() || part == "." || part == "..") return refuse("path_unsafe");
        for (unsigned char c : part)
            if (c < 0x20 || c == 0x7f || c == '\\') return refuse("path_unsafe");
        parts.push_back(part);
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    if (parts.empty()) return refuse("path_unsafe");
    // every folder on the way is a real folder, the last component is the file
    std::string path = reader_folder;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        path += "/" + parts[i];
        if (::lstat(path.c_str(), &st) != 0) return refuse("not_found");
        if (!S_ISDIR(st.st_mode)) return refuse("path_unsafe");   // a link (not followed) or a file where a folder should be
    }
    const std::string file = path + "/" + parts.back();
    if (::lstat(file.c_str(), &st) != 0) return refuse("not_found");
    if (!S_ISREG(st.st_mode)) return refuse("path_unsafe");
    if (reader_changed(file, record)) return refuse("local_changed");
    if (::unlink(file.c_str()) != 0) return refuse(errno == ENOENT ? "not_found" : (errno == EROFS || errno == EACCES ? "storage_readonly" : "write_failed"));
    log::event("sync.reader_removed", {});
    // the folders that are empty now, the deepest first; the first one that is not empty ends it
    for (size_t n = parts.size() - 1; n >= 1; --n) {
        std::string dir = reader_folder;
        for (size_t i = 0; i < n; ++i) dir += "/" + parts[i];
        if (::rmdir(dir.c_str()) != 0) break;
    }
    return Error();
}

}  // namespace pbc
