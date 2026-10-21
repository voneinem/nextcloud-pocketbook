#pragma once
// The file an upload reads (specs/002-manage-server-files, research R-5, constitution Principles III and IV): checked when it is picked and
// again when the item is sent, opened read-only without following links, identified by size and modification time so that a resume never mixes
// old and new content. Nothing here ever writes to the reader. Qt-free.

#include <cstdint>
#include <functional>
#include <string>

#include "pbc/source_walk.h"
#include "pbc/types.h"

namespace pbc {

struct SourceIdentity {
    int64_t size = 0;
    int64_t mtime = 0;   // seconds
};

// Opens read-only with O_NOFOLLOW and looks at the open file: a regular file of 1 byte up to 4 GiB - 1. Errors: source_not_allowed (outside
// the rules, a link, the data folder), source_unreadable (missing, a folder, empty, unreadable), too_large_for_storage (4 GiB or more).
Result<SourceIdentity> inspect_source(const std::string& path, const SourceRules& rules);
bool same_identity(const SourceIdentity& a, const SourceIdentity& b);

// SHA-256 (hex) of the file in one streaming pass of 64 KiB pieces. `progress(done, total)` is called after every piece. Cancel is asked
// before every piece (cancelled). The file is opened like inspect_source does (a link is source_not_allowed).
Result<std::string> hash_source(const std::string& path, Cancel& cancel, const std::function<void(long long done, long long total)>& progress);

}  // namespace pbc
