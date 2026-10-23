#pragma once
// The rules of a folder pair (specs/003-folder-sync/research.md R-2, contracts/core-interfaces.md "Rules of a pair"): which reader folders may be
// paired, when two pairs overlap, the limits, the directions and the free-space reserve. Pure functions; the only thing they read is the file
// system, to find the canonical form of the reader folder and to refuse links. No Qt.

#include <cstdint>
#include <string>
#include <vector>

#include "pbc/source_walk.h"
#include "pbc/sync_types.h"

namespace pbc {

struct PairCheck {
    bool ok = false;
    // pair_limit_invalid, source_not_allowed, reader_folder_missing, pair_folder_forbidden, pair_overlap, pair_limit; protocol_error for a server
    // folder that is no remote path. "" when ok.
    std::string error_key;
    std::string local_folder;   // the canonical reader folder to store when ok
};

// The picker root itself, the app's data folder (`rules.deny`), the download root, every folder inside one of them and every folder that contains
// one of them (a parent of the data folder would sync the app's own files). Used by check_pair and by the folder picker.
bool reader_folder_forbidden(const std::string& folder, const SourceRules& rules, const std::string& download_root);

// `existing` are the other pairs (a pair with the id of the candidate is ignored, so a changed pair does not overlap itself).
// `download_root` is the download folder of feature 001; the data folder is `rules.deny`.
PairCheck check_pair(const SyncPair& candidate, const std::vector<SyncPair>& existing, const SourceRules& rules, const std::string& download_root);

// true when the new direction can send reader files to the server and the old one could not (FR-005).
bool needs_send_confirmation(Direction from, Direction to);
bool direction_allows_to_reader(Direction d);
bool direction_allows_to_server(Direction d);

// down, 500 files, 2 GiB, both switches off (research R-11, R-13).
SyncPair default_pair(const std::string& server, const std::string& user_id);

// max(kSyncReserveMin, 2 % of the volume); an unknown volume (<= 0) gives the minimum.
int64_t reserve_for(int64_t volume_bytes);

}  // namespace pbc
