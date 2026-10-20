#include "pbc/local_files.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <vector>

#include "pbc/log.h"

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

Error remove_local_copy(Store& store, const std::string& root, const std::string& remote_path, bool confirmed_loss) {
    const auto copy = store.local_file(remote_path);
    if (!copy) return {};   // nothing recorded: nothing to remove
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

}  // namespace pbc
