#pragma once
// The facts about the card that a download needs (specs/001-nextcloud-files-client, research R-13): free space, the largest file the
// file system can hold, whether it can be written. A seam, so that tests can play a full or read-only card. No Qt.

#include <cstdint>
#include <optional>
#include <string>

namespace pbc {

inline constexpr int64_t kFatMaxFileBytes = 4294967295ll;   // 4 GiB - 1: the largest file FAT can hold

class StorageInfo {
public:
    virtual ~StorageInfo() = default;
    // Free bytes for a normal user on the volume of `path` (the nearest existing parent is looked at); nullopt if unknown.
    virtual std::optional<int64_t> free_bytes(const std::string& path) const = 0;
    // The largest file that can be written there.
    virtual int64_t max_file_bytes(const std::string& path) const = 0;
    virtual bool writable(const std::string& path) const = 0;
};

class PosixStorageInfo : public StorageInfo {
public:
    // always_fat: the reader's card is FAT and may be reformatted, so the FAT limit applies whatever statfs says (the device build).
    explicit PosixStorageInfo(bool always_fat = false) : always_fat_(always_fat) {}
    std::optional<int64_t> free_bytes(const std::string& path) const override;
    int64_t max_file_bytes(const std::string& path) const override;
    bool writable(const std::string& path) const override;

private:
    bool always_fat_;
};

}  // namespace pbc
