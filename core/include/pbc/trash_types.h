#pragma once
// The entities of the trash and versions screens (specs/006-trash-and-versions, data-model.md): what the server's trash and a file's version list
// look like once parsed, and the outcome of one action. Everything that came from the server is untrusted and bounded (research R-3); a name
// or a location is never a local path. In memory only, except the last trash list (schema 6, store_trash.cpp). Qt-free.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pbc/store_files.h"
#include "pbc/types.h"

namespace pbc {

inline constexpr size_t kMaxTrashRows = 1000;
inline constexpr size_t kMaxVersionRows = 200;
inline constexpr size_t kMaxTrashBody = 4u << 20;     // 4 MiB
inline constexpr size_t kMaxVersionBody = 1u << 20;   // 1 MiB
inline constexpr size_t kMaxLocationBytes = 1024;
inline constexpr size_t kMaxSegmentBytes = 255;       // one name, one trash name, one segment of a location

// Whether a server app is on. Unknown is never "off": the answer was missing or unclear (research R-2).
enum class FeatureState { On, Off, Unknown };

struct TrashAvailability {
    FeatureState trash = FeatureState::Unknown;
    FeatureState versions = FeatureState::Unknown;
    std::string code;                          // "T1-<status>" when a state is Unknown (the short code of the generic message), else ""
};

// One file or folder of the server's trash. `trash_name` is its identity (unique per deletion) and only ever becomes a path segment built by the app.
struct TrashItem {
    std::string trash_name;                   // "" when the name could not be one path segment (then `unreadable`)
    std::string name;                         // display only, control characters removed, at most 255 bytes
    std::string original_dir;                 // "/Books"; "" when unknown
    std::string original_name;                // "" when unknown
    bool original_known = false;
    bool unreadable = false;                  // listed, but neither restorable nor deletable
    std::optional<int64_t> deleted_at;        // epoch seconds in [0, now + 1 day]
    std::optional<int64_t> size;              // files only
    std::optional<int64_t> fileid;
    Kind kind = Kind::File;
    Availability availability = Availability::Ok;
};

struct TrashList {
    std::vector<TrashItem> items;             // newest deletion first, at most kMaxTrashRows
    int total_seen = 0;                       // items listed before the cut
    int skipped_hostile = 0;                  // another account, a deeper level, a repeated address
    bool truncated = false;                   // more than kMaxTrashRows items
    bool partial = false;                     // a member of the answer failed
    bool from_store = false;                  // this is the stored copy (offline)
    std::optional<int64_t> fetched_at;
    std::string code;                         // "T2-<status>" when the fetch failed (the short code of the message), else ""
};

// An older content of one file. The current content is not a row.
struct VersionRow {
    std::string version_id;                   // 1 to 20 digits
    std::optional<int64_t> mtime, size;
    bool future = false;                      // the date lies more than a day ahead
};

struct VersionList {
    std::vector<VersionRow> rows;             // newest first, at most kMaxVersionRows
    int skipped = 0;                          // members with an unusable id or without properties
    bool truncated = false;
    std::string file_etag;                    // the file's ETag when the list was read: the stamp for the stale check
    int64_t file_id = 0;
    std::string code;                         // "T6-<status>" when the fetch failed (the short code of the message), else ""
};

// The place a trash item will come back to.
struct RestoreTarget {
    std::string folder;
    std::string name;
    bool is_original = false;
};

// The outcome of one action. `code` is built from the operation and the HTTP status only (research R-11): no name, no location.
struct OperationResult {
    Error error;
    bool outcome_unknown = false;
    int removed = 0, not_removed = 0, not_tried = 0;
    std::string code;
    bool landed_elsewhere = false;            // a restore that the server answered but that is not where it was meant to be
    bool previous_not_kept = false;           // a version restore after which the content that was current is not among the versions
    std::vector<std::string> removed_trash_names;   // Empty trash: the items that are gone (removed or already gone), in order
    std::string unknown_trash_name;           // a delete whose outcome is unknown: the item the user must look for after a refresh
};

}  // namespace pbc
