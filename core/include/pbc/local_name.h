#pragma once
// Local names for downloaded files (specs/001-nextcloud-files-client, research R-9, FR-020, FR-022): a name from the server becomes one
// safe path component on the reader's FAT storage, the remote tree is mirrored under the download root, and a name that the storage
// cannot tell apart from another (it ignores case) gets a suffix instead of replacing anything. Names from the server are untrusted;
// nothing outside this module may build a local path from one. No Qt.

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pbc/types.h"

namespace pbc {

struct LocalNameOptions {
    size_t max_component_bytes = 120;   // FAT stores long names as UTF-16; 120 bytes leave room for a suffix and the .pbc-<id>.part name
    size_t max_path_bytes = 4000;
};

// One server name as one path component:
//  - invalid UTF-8 and control characters (also C1 and the bidirectional overrides) and  \ / : * ? " < > |  become '_'
//  - "", "." and ".." become "_"; trailing dots and blanks are removed
//  - names reserved by other systems (CON, PRN, AUX, NUL, COM1-9, LPT1-9, with or without an extension) get a '_' in front
//  - a name that begins with ".pbc-" (the temporary download files) gets a '_' in front
//  - at most max_component_bytes bytes, cut on a character boundary; an extension of up to 16 bytes is kept
std::string sanitize_component(std::string_view server_name, const LocalNameOptions& options = {});

// The local paths in use, keyed by their case-folded relative path; the value is the remote path that owns them. Directories are included,
// so a folder that differs from another only in case is recognised as another folder.
using TakenMap = std::map<std::string, std::string>;
// From the recorded copies: (remote path, relative local path) pairs. Both have the same number of components; the ancestors are paired too.
TakenMap build_taken(const std::vector<std::pair<std::string, std::string>>& remote_and_local);

// The relative local path (below `download_root`) for a remote file, mirroring the remote tree:
//  - every component is sanitized; a component owned by a different remote path, or present on the card but not recorded, gets " (2)",
//    " (3)" ... (before the extension for a file); a recorded mapping of the same remote path is reused
//  - existing folders are reused when they are real folders; a link or a file where a folder is needed, or a download root that is itself a
//    link or missing, is "path_unsafe"; a path of more than max_path_bytes (or more than 64 components) is "path_too_long"
//  - the remote root "/" is not a file: "protocol_error"
// Creates nothing. Collisions are decided case-insensitively, like the storage does.
Result<std::string> choose_local_path(const std::string& download_root, std::string_view remote_path, const TakenMap& taken,
                                      const LocalNameOptions& options = {});

// A name of the form "<base> (<n>)<ext>" for a file ("story.epub" -> "story (2).epub") or "<name> (<n>)" for a folder, within the limit.
std::string with_suffix(const std::string& component, int n, bool is_file, const LocalNameOptions& options = {});

}  // namespace pbc
