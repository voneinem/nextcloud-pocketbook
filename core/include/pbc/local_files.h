#pragma once
// The files the app put on the reader and what may be done to them (specs/001-nextcloud-files-client, constitution Principle III):
// whether the user changed one, keeping the user's copy against a newer server version, resolving the path of a copy for opening it, and
// removing a copy. This and downloader.cpp are the only places that delete or replace a user's file. No Qt.

#include <cstdint>
#include <string>

#include "pbc/store.h"
#include "pbc/types.h"

namespace pbc {

// The state of a file on the card, without following links.
struct FileStat {
    bool exists = false;
    bool regular = false;     // a regular file (not a link, not a folder)
    int64_t size = 0;
    int64_t mtime = 0;
};
FileStat stat_file(const std::string& absolute_path);

// The copy differs from what the download left there (size or modified time), or it is no longer a regular file.
bool local_copy_changed(const LocalFile& recorded, const FileStat& now);

// "Keep my copy": records the server version the user was shown, so that it no longer counts as newer. The file is not touched.
Error keep_mine(Store& store, const std::string& remote_path, const std::string& server_etag);

// The absolute path of a recorded copy for opening it: the relative path must name a regular file inside `download_root`, with no
// link anywhere on the way. Error path_unsafe otherwise; not_found when the file is gone.
Result<std::string> resolve_local_file(const std::string& download_root, const std::string& local_relative);

// Removes the copy from the card and its record; the server is never contacted. A copy that was changed on the reader needs
// confirmed_loss (error local_changed otherwise). A missing file only loses its record.
Error remove_local_copy(Store& store, const std::string& download_root, const std::string& remote_path, bool confirmed_loss);

// Creates the folders below the download root for a relative path (without its last component if `include_last` is false), refusing links
// and files on the way (path_unsafe).
Error ensure_directories(const std::string& download_root, const std::string& relative_dir);

}  // namespace pbc
