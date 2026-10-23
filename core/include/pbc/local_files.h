#pragma once
// The files the app put on the reader and what may be done to them (specs/001-nextcloud-files-client, constitution Principle III):
// whether the user changed one, keeping the user's copy against a newer server version, resolving the path of a copy for opening it, and
// removing a copy, and - for a folder pair (specs/003-folder-sync) - creating the picker's "New folder" and removing a synced reader copy. This and
// downloader.cpp are the only places that delete or replace a user's file. No Qt.

#include <cstdint>
#include <string>

#include "pbc/source_walk.h"
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

// A file a folder pair has synchronized, as the 001 code sees a downloaded copy (research R-15): `local_path` is relative to the pair's folder
// (SyncedCopy::root), not to the download root, so the caller stats `root + "/" + local_path`. Read-only: nothing here changes a record.
LocalFile as_local_file(const std::string& remote_path, const SyncedCopy& copy);

// Removes the copy from the card and its record; the server is never contacted. A copy that was changed on the reader needs
// confirmed_loss (error local_changed otherwise). A missing file only loses its record.
Error remove_local_copy(Store& store, const std::string& download_root, const std::string& remote_path, bool confirmed_loss);

// Creates the folders below the download root for a relative path (without its last component if `include_last` is false), refusing links
// and files on the way (path_unsafe).
Error ensure_directories(const std::string& download_root, const std::string& relative_dir);

// "New folder" of the folder picker (specs/003-folder-sync): creates ONE folder `name` below `parent`, which is the picker root or a folder inside it
// (no link on the way, not in the app's own data folder). The name must already be one safe path component (sanitize_component leaves it
// unchanged, not hidden), so a hostile name is refused rather than silently changed: name_forbidden. An existing entry of that name, also one that
// differs only by case, is name_exists. source_not_allowed / source_unreadable for the parent, storage_full / storage_readonly / write_failed for the
// storage. Returns the canonical path of the new folder. The only place that creates a folder for the picker; the bridge only calls it.
Result<std::string> create_picker_folder(const SourceRules& rules, const std::string& parent, const std::string& name);

// A synced reader copy that the user chose to delete (specs/003-folder-sync R-10): the only function that removes a user's file for a pair. It removes the file
// only if it is a regular file inside `reader_folder` (absolute, a real folder, not a link), reached without a link on the way and by a safe relative path,
// and unchanged against `record` (size and mtime, or the stored SHA-256 when only the time differs: the rule of reader_changed). Then it removes the folders
// that became empty, up to but not including `reader_folder`. Errors: path_unsafe (a link, a path that leaves the folder, a folder or special file),
// not_found, local_changed (changed, or the record says nothing of what the copy was), write_failed. Never contacts the server; never touches a record.
Error remove_pair_copy(const std::string& reader_folder, const std::string& local_rel, const SyncRecord& record);

}  // namespace pbc
