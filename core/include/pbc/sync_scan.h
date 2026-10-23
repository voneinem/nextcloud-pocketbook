#pragma once
// Scanning both sides of a folder pair (specs/003-folder-sync, research R-3 and R-4): the server folder with Depth-1 listings read transiently, the
// reader folder without following links, the records' notion of "unchanged", and the first-sync identity check. Everything here only reads: it creates
// no cache row, no queue item, no file. Each scan returns one flat vector sorted by `rel` (bytes); the planner merges them in one pass. A scan that did
// not see everything says so (`incomplete`): what is absent from an incomplete scan is unknown, never missing (FR-029). Qt-free.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pbc/dav_common.h"
#include "pbc/source_walk.h"
#include "pbc/store_files.h"
#include "pbc/sync_types.h"
#include "pbc/types.h"

namespace pbc {

struct ScanEntry {
    std::string rel;                     // the path below the pair's folder, no leading "/"
    Kind kind = Kind::File;
    int64_t size = 0;
    std::string etag, checksum;          // server side: as the listing said ("" = none)
    std::optional<int64_t> mtime;
    Availability availability = Availability::Ok;
    std::string skip_reason;             // "" or unavailable, not_regular, source_unreadable, name_collision
};

struct Scan {
    std::vector<ScanEntry> entries;      // sorted by rel (bytes)
    bool incomplete = false;             // a folder was not listed or not read, a listing was partial, a bound was hit
    int folders_not_listed = 0;
    int dropped_names = 0;               // server names that cannot be one path segment, left out
    int skipped_hidden = 0;              // reader: hidden names (also the temporary files of the downloader)
    std::string stopped_by_key;          // "" or why the scan ended early: server_folder_missing, reader_folder_missing, cancelled, a general problem
};

// The server folder of the pair: one PROPFIND Depth 1 per folder, breadth-first, through `session`. Bounded by kSyncMaxFolders, kSyncMaxDepth and
// kSyncMaxFiles. A partial answer, a folder that fails (but not for a general reason) and a bound make the scan incomplete and keep what was listed;
// the pair's own folder missing ends it with server_folder_missing; a refused login, a server that is down or in maintenance ends it with that key.
Scan scan_server(DavSession& session, std::string_view user_id, const SyncPair& pair, Cancel& cancel);

// The reader folder of the pair: opendir/fstatat with AT_SYMLINK_NOFOLLOW, bounded the same way. Hidden names, links, special files and unreadable
// entries are not followed (links, special files and unreadable files are listed with their skip reason); two names that differ only by case are
// reported as name_collision (the second one). A folder that is missing or not a plain folder ends it with reader_folder_missing.
Scan scan_reader(const SyncPair& pair, const SourceRules& rules, Cancel& cancel);

// `root` + "/" + `rel`, or "" when `rel` is not a safe relative path (empty, absolute, an empty, "." or ".." component, a control character, more than 4,000
// bytes). The only place the sync modules join a reader path by hand (rule N7).
std::string join_path_checked(const std::string& root, const std::string& rel);

// R-3. Reader side changed: not a regular file any more, a different size, or a different modification time (2-second resolution) - unless only the time
// differs and the record has the SHA-256 of the file as agreed and the file still has it. Without that hash a differing time counts as changed.
bool reader_changed(const std::string& path, const SyncRecord& record, Cancel* cancel = nullptr);
// Server side changed: the ETag differs byte-wise from the recorded one, or the listing has none. Size and date never decide.
bool server_changed(const ScanEntry& entry, const SyncRecord& record);
// First-sync identity: equal size and the strongest checksum of the listing equals the one computed over the reader file. False without a checksum, with
// another one, or with another size (then the file is not read at all).
bool identical_by_checksum(const std::string& reader_file, const ScanEntry& server_entry, Cancel& cancel);

}  // namespace pbc
