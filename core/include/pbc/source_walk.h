#pragma once
// The reader's storage as the source of uploads (specs/002-manage-server-files, research R-13, constitution Principle IV): which paths may be
// a source and the bounded walk of a picked folder. Links are never followed, hidden entries, special files and unreadable files are
// skipped, and the app's own data folder (database, credentials, log) can never be a source. Qt-free.

#include <cstdint>
#include <string>
#include <vector>

#include "pbc/types.h"

namespace pbc {

struct SourceRules {
    std::string root;                  // the storage root (/mnt/ext1; on the host PBC_PICKER_ROOT)
    std::vector<std::string> deny;     // folders that can never be a source (the app's data folder)
    int max_items = 2000;              // folders + files of one walk
    int max_depth = 12;
};

enum class SourceKind { File, Folder };

struct SourceEntry {
    std::string rel;                   // path relative to the picked folder ("" for the picked folder itself)
    std::string name;                  // the last name segment (the folder's own name for the picked folder)
    SourceKind kind = SourceKind::File;
    int64_t size = 0;                  // files only
};

struct SourceWalk {
    std::vector<SourceEntry> entries;  // the picked folder first, every folder before what it contains
    int folders = 0;                   // including the picked folder
    int files = 0;
    int64_t bytes = 0;
    int skipped = 0;                   // hidden, links, special, unreadable, denied
    bool over_limit = false;           // more than max_items items or deeper than max_depth: the walk stopped
};

// An absolute path strictly below the root, without "." or ".." segments, with no symbolic link on the way and not inside a denied folder.
// Returns the canonical path. source_not_allowed for all of that, source_unreadable when it does not exist.
Result<std::string> check_source_path(const std::string& path, const SourceRules& rules);

struct SourceListing {
    std::vector<SourceEntry> entries;  // folders first, then files, each in case-insensitive name order
    bool unreadable = false;           // not a folder, missing, not readable, outside the rules or a link
};
// One folder for the picker. Hidden entries, links, special files and unreadable files are left out. At the root of the rules only the
// existing folders named in `top_folders` are listed (compared as whole names without regard to case); an empty list means no filter.
SourceListing list_source_dir(const std::string& path, const SourceRules& rules, const std::vector<std::string>& top_folders);

// A bounded walk of a picked folder (empty for anything that is not a folder).
SourceWalk walk_source(const std::string& folder, const SourceRules& rules);

}  // namespace pbc
