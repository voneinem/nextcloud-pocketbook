#include "pbc/storage_info.h"

#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <limits>

namespace pbc {

namespace {

constexpr long kMsdosSuperMagic = 0x4d44;   // FAT, also reported for vfat

// The nearest existing ancestor of `path` (the path itself if it exists).
std::string existing_ancestor(std::string path) {
    struct stat st;
    while (path.size() > 1 && ::stat(path.c_str(), &st) != 0) {
        const size_t slash = path.rfind('/');
        if (slash == std::string::npos) return ".";
        path = slash == 0 ? "/" : path.substr(0, slash);
    }
    return path;
}

}  // namespace

std::optional<int64_t> PosixStorageInfo::free_bytes(const std::string& path) const {
    struct statvfs v;
    if (::statvfs(existing_ancestor(path).c_str(), &v) != 0) return std::nullopt;
    return static_cast<int64_t>(v.f_bavail) * static_cast<int64_t>(v.f_frsize);
}

int64_t PosixStorageInfo::max_file_bytes(const std::string& path) const {
    if (always_fat_) return kFatMaxFileBytes;
    struct statfs s;
    if (::statfs(existing_ancestor(path).c_str(), &s) == 0 && static_cast<long>(s.f_type) == kMsdosSuperMagic) return kFatMaxFileBytes;
    return std::numeric_limits<int64_t>::max();
}

std::optional<int64_t> PosixStorageInfo::total_bytes(const std::string& path) const {
    struct statvfs v;
    if (::statvfs(existing_ancestor(path).c_str(), &v) != 0) return std::nullopt;
    return static_cast<int64_t>(v.f_blocks) * static_cast<int64_t>(v.f_frsize);
}

bool PosixStorageInfo::writable(const std::string& path) const {
    const std::string dir = existing_ancestor(path);
    struct statvfs v;
    if (::statvfs(dir.c_str(), &v) == 0 && (v.f_flag & ST_RDONLY)) return false;
    return ::access(dir.c_str(), W_OK | X_OK) == 0;
}

}  // namespace pbc
