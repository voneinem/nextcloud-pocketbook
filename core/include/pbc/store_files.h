#pragma once
// Row types of the files client's part of the store (specs/001-nextcloud-files-client, data-model.md). The methods that read and write
// them are members of Store (store.h), implemented in store_files.cpp. No Qt.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbc {

// The one account of this feature. The app password is NOT here: it lives in the credentials file (credentials_file.h).
struct AccountRow {
    std::string base_url;        // canonical https address, no trailing slash
    std::string login_name;      // for Basic authentication
    std::string user_id;         // for DAV paths; may differ from the login name
    std::string server_name;     // at most 64 bytes
    std::string server_version;
    std::optional<int64_t> quota_used;
    std::optional<int64_t> quota_total;   // nullopt = unknown, -1 = unlimited
    std::optional<int64_t> checked_at;
    std::string state = "untested";       // untested | ok | needs_signin | problem
    std::string problem_key;              // "" = none
};

// ---- cached listings (data-model.md "folders" and "entries") ----

inline constexpr int kMaxEntriesPerFolder = 10000;     // more is refused: listing_too_large
inline constexpr int kDefaultCachedEntries = 50000;    // the bounds of the metadata cache (Principle V)
inline constexpr int kDefaultCachedFolders = 2000;

enum class Kind { File, Folder };
enum class Availability { Ok, NoPermission, Encrypted, Unsupported, Unreadable };

// One file or folder of a server listing, as stored. Names and paths are exactly what the server sent.
struct RemoteEntry {
    std::string name;
    std::string path;                  // the full remote path ("/Books/story.epub")
    Kind kind = Kind::File;
    std::optional<int64_t> size, mtime, fileid;
    std::string etag, mime, permissions, checksum, reason;
    bool favorite = false;
    Availability availability = Availability::Ok;
};

// The result of one depth-1 listing.
struct Listing {
    std::string folder_etag;
    std::optional<int64_t> folder_fileid;
    std::vector<RemoteEntry> entries;
    int skipped_foreign = 0;           // addresses of another account or another folder
    int skipped_hostile = 0;           // names that cannot be one path segment
    bool partial = false;              // some entries failed while others succeeded
};

struct FolderRow {
    std::string path, etag;
    std::optional<int64_t> fileid;
    std::optional<int64_t> fetched_at;   // nullopt: never confirmed by the server ("not synced")
    int64_t last_access_at = 0;
    int entry_count = 0;
    bool gone = false;
};

enum class SortKey { Name, Date, Size };

// A result of the offline search. source: "reader" (a cached listing) | "favorites" | "download" (a downloaded copy)
struct SearchHit {
    RemoteEntry entry;
    std::string source;
};

struct EntryPage {
    std::vector<RemoteEntry> rows;
    int total = 0;
};

// ---- downloaded copies and the journal (data-model.md "local_files", "downloads") ----

// A file on the reader that came from the server. Durable user data: only the Space screen removes a row.
struct LocalFile {
    std::string remote_path;
    std::string local_path;            // relative to the download root
    std::string remote_etag;           // the server version this copy was downloaded from
    std::string seen_etag;             // the server version the user chose to keep their own copy against ("" = none)
    int64_t size = 0;                  // as it was right after the download (stat)
    int64_t mtime = 0;
    std::optional<int64_t> remote_size;
    std::optional<int64_t> remote_mtime;
    int64_t downloaded_at = 0;
    bool server_missing = false;       // the last listing of the parent did not contain it
};

// One transfer in progress. Written before the first request of a job.
struct DownloadJournalRow {
    int64_t id = 0;
    std::string remote_path;
    std::string target_path;           // relative to the download root
    std::string expected_etag;         // a strong ETag usable for If-Range, "" if none
    std::optional<int64_t> expected_size;
    int64_t received = 0;
    std::string state;                 // running | waiting | paused
    int64_t started_at = 0;
    int64_t updated_at = 0;
    // The downloads of a folder pair (specs/003-folder-sync): the pair that records the result, the absolute folder `target_path` is relative to
    // (empty = the download root), the checksum the file is verified against and whether the version in `expected_etag` is required (If-Match).
    int64_t pair_id = 0;
    std::string target_root;
    std::string checksum;
    bool strict = false;
};

// A "keep offline" mark (data-model.md "offline_marks"): a file, or a folder with everything below it.
struct MarkRow {
    std::string remote_path;
    Kind kind = Kind::File;
    int64_t marked_at = 0;
};

}  // namespace pbc
