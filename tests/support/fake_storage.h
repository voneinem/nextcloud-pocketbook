#pragma once
// A StorageInfo whose free space, file-size limit and writability a test sets (the reader's card is not available on the host).

#include <optional>
#include <string>

#include "pbc/storage_info.h"

namespace pbtest {

struct FakeStorage : pbc::StorageInfo {
    std::optional<int64_t> free = 10ll << 30;           // 10 GiB
    int64_t limit = pbc::kFatMaxFileBytes;
    bool read_only = false;

    std::optional<int64_t> free_bytes(const std::string&) const override { return free; }
    int64_t max_file_bytes(const std::string&) const override { return limit; }
    bool writable(const std::string&) const override { return !read_only; }
};

}  // namespace pbtest
