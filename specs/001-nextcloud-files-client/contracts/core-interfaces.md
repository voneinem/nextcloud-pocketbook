# Contract: Core interfaces (`core/`, Qt-free)

**Feature**: [../spec.md](../spec.md) | **Data model**: [../data-model.md](../data-model.md)

Declarations only; the code is written test-first (tasks.md). All live in namespace `pbc`, headers in
`core/include/pbc/`, no Qt, no InkView, no SDK header (constitution II). Existing seams (`Clock`, `Random`, `Cancel`,
`HttpTransport`, `Result<T>`, `Error`) are reused. Error keys are those of
[server-requests.md](server-requests.md).

## Changed existing seams

```cpp
// http_transport.h — additive
struct Request {
    // …existing fields…
    bool save_append = false;     // with save_body_to: open for append instead of truncating (resume)
    long long resume_from = 0;    // informs the sink of the offset (bytes already in the file); used for the max_body_bytes check
    // Called from the transfer thread at least once per second and on every 64 KiB; false = cancel the transfer.
    std::function<bool(long long done, long long total)> progress;
    long buffer_size = 0;         // 0 = libcurl default; 65536 for file transfers
    bool identity_encoding = false;  // file transfers: Accept-Encoding: identity
};
struct Response { /* …existing… */ bool cancelled = false; };   // the progress callback returned false

// dav_common.h — additive
inline constexpr const char* kNsOc = "http://owncloud.org/ns";
inline constexpr const char* kNsNc = "http://nextcloud.org/ns";

// store.h — schema 2 (data-model.md). kSchemaVersion = 2. New typed methods are declared in store_files.h and
// implemented in store_files.cpp as members of Store.
```

```cpp
// dav_common.h — additive: the Retry-After value of a 429/503 answer, in seconds from now, capped at 900 (15 minutes);
// accepts delta-seconds and an HTTP date (parse_http_date); nullopt when absent or unusable.
std::optional<int64_t> parse_retry_after(const Headers& headers, int64_t now);
// DavSession::status_error additionally maps 423 -> locked, 429 -> rate_limited, 503 -> maintenance (the caller may refine
// 503 with status.php), other 5xx -> server_error.
```

## Paths and names

```cpp
// remote_path.h
std::string normalize_remote_path(std::string_view raw);                  // "" and "/" -> "/", no trailing slash, no "//"
std::string parent_of(std::string_view remote_path);                      // "/a/b" -> "/a", "/a" -> "/", "/" -> "/"
std::string join_remote(std::string_view folder, std::string_view name);
std::string encode_path(std::string_view remote_path);                    // percent-encode each segment (UTF-8)
Result<std::string> decode_href(std::string_view href, std::string_view user_id);   // strips /remote.php/dav/files/<uid>
bool is_valid_name(std::string_view name);                                // data-model validation

// local_name.h (research R-9)
struct LocalNameOptions { size_t max_component_bytes = 120; size_t max_path_bytes = 4000; };
std::string sanitize_component(std::string_view server_name, const LocalNameOptions& = {});
// Chooses the local relative path for a remote path inside `download_root`: sanitizes every component, resolves
// case-insensitive collisions against the directory on disk and `taken` (paths already recorded), and verifies that
// the result stays inside the root (canonical prefix check, symbolic links not followed).
Result<std::string> choose_local_path(const std::string& download_root, std::string_view remote_path,
                                      const std::vector<std::string>& taken_lower /* folded */);
```

## Server probe and login

```cpp
// nc_server.h
struct ServerInfo { std::string name, version; int major = 0; };
struct Quota { std::optional<int64_t> used, total; bool unlimited = false; };
struct AccountCheck { ServerInfo server; std::string user_id; Quota quota; std::optional<int64_t> server_time; };

// Runs steps 1–4 of research R-3 and stops at the first failure. `now`/`build_epoch` serve the local clock test.
Result<AccountCheck> check_account(HttpTransport& http, const std::string& base_url, const std::string& login_name,
                                   const std::string& app_password, const Clock& clock);
Result<std::string> canonical_base_url(std::string_view typed);          // https only; a bare host or host:port is completed with https://; http:// and every other scheme, user information, query and fragment are refused; one trailing slash stripped
Error classify_status_document(const Response& status_php, ServerInfo* out);   // status.php rules, testable alone

// nc_login.h
struct LoginStart { std::string login_url; std::string poll_endpoint; std::string token; };   // token: memory only
struct LoginResult { std::string server, login_name, app_password; };
Result<LoginStart> login_start(HttpTransport& http, const std::string& base_url);
// Ok + value.empty() == "not approved yet" (404); never loops.
Result<std::optional<LoginResult>> login_check(HttpTransport& http, const std::string& base_url, LoginStart& flow);
void login_discard(LoginStart& flow);                                    // overwrites the token, clears the fields

// qr_code.h (research R-5)
struct QrMatrix { int size = 0; std::vector<uint8_t> modules; bool at(int x, int y) const; };
Result<QrMatrix> qr_encode(std::string_view text);                       // error key "qr_too_long" above 367 bytes
```

## Listing, favorites, search

```cpp
// nc_listing.h
enum class Kind { File, Folder };
enum class Availability { Ok, NoPermission, Encrypted, Unsupported, Unreadable };
struct RemoteEntry {
    std::string name; Kind kind = Kind::File;
    std::optional<int64_t> size, mtime, fileid;
    std::string etag, mime, permissions, checksum, reason;
    bool favorite = false; Availability availability = Availability::Ok;
    std::string path;       // full remote path (used by favorites and search results)
};
struct Listing {
    std::string folder_etag; std::optional<int64_t> folder_fileid;
    std::vector<RemoteEntry> entries;
    int skipped_foreign = 0, skipped_hostile = 0; bool partial = false;
};
std::string listing_request_body();                                      // section 5 body
std::string favorites_request_body();
std::string search_request_body(std::string_view user_id, std::string_view term);   // empty if the term is unusable
Result<Listing> parse_listing(const Response& r, std::string_view user_id, std::string_view requested_path, bool single_folder);
Result<std::vector<RemoteEntry>> parse_flat_result(const Response& r, std::string_view user_id, size_t max_entries);   // favorites, search

// Fetchers (one request each, status mapped to error keys)
Result<Listing> fetch_listing(DavSession& session, std::string_view user_id, std::string_view remote_path);
Result<std::vector<RemoteEntry>> fetch_favorites(DavSession&, std::string_view user_id);
Result<std::vector<RemoteEntry>> search_server(DavSession&, std::string_view user_id, std::string_view term);
```

## Downloads

```cpp
// storage_info.h
class StorageInfo {
public:
    virtual ~StorageInfo() = default;
    virtual std::optional<int64_t> free_bytes(const std::string& path) const = 0;
    virtual int64_t max_file_bytes(const std::string& path) const = 0;    // 4 GiB - 1 on FAT and on the device target
    virtual bool writable(const std::string& path) const = 0;
};
class PosixStorageInfo : public StorageInfo { /* statvfs / statfs */ };

// hash.h
enum class HashAlgo { Sha256, Sha1, Md5 };
struct Checksum { HashAlgo algo; std::string hex; };
std::vector<Checksum> parse_checksums(std::string_view oc_checksums);    // "SHA1:ab… MD5:cd…", unknown ignored
Result<std::string> hash_file(const std::string& path, HashAlgo algo, Cancel& cancel);   // streaming, 64 KiB

// downloader.h
struct Preflight { enum class Verdict { Ok, NeedsConfirmation, Refused } verdict; std::string error_key; int64_t size = 0, free = 0, needed = 0; };
Preflight preflight_download(const StorageInfo&, const std::string& target_dir, std::optional<int64_t> size, int64_t warn_bytes);

enum class JobState { Running, Waiting, Paused, Done, Cancelled, Failed, Blocked };
struct JobProgress { JobState state; int64_t done = 0, total = 0; int attempt = 0; };
struct DownloadRequest { std::string remote_path, user_id; std::optional<int64_t> size; std::string etag, checksum; bool replace_unchanged = false; };
struct DownloadOutcome { JobState state; std::string error_key; std::string local_path; };

class Downloader {
public:
    Downloader(Store&, DavSession&, StorageInfo&, const Clock&, std::string download_root, std::function<bool()> wifi_up);
    // Blocks the worker thread until the job ends. `on_progress` is rate-limited by the caller (2 s / 5 %).
    DownloadOutcome run(const DownloadRequest&, Cancel&, const std::function<void(const JobProgress&)>& on_progress);
    // Start-up housekeeping (no network): research R-8 point 9.
    void recover();
};
```

## Offline marks, states, Update

```cpp
// entry_state.h
enum class EntryState { NotDownloaded, Downloaded, Outdated, ChangedOnReader, Conflict, RemovedOnServer, MissingLocally };
struct LocalFile { std::string remote_path, local_path, remote_etag, seen_etag; int64_t size = 0, mtime = 0; bool server_missing = false; };
struct FileStat { bool exists = false; int64_t size = 0, mtime = 0; };
EntryState compute_state(const RemoteEntry* cached, const LocalFile* local, const FileStat& stat);   // pure

// update.h
struct UpdateReport { int folders = 0, files = 0, updated = 0, added = 0, skipped = 0, need_decision = 0, removed = 0, failed = 0, unchecked_folders = 0; std::string stopped_by_key; };
class Updater {
public:
    Updater(Store&, DavSession&, Downloader&, StorageInfo&, const Clock&);
    // Never runs by itself. Cancellable. Same result when repeated after an interruption.
    UpdateReport run(std::string_view user_id, Cancel&, const std::function<void(const UpdateReport&)>& on_progress);
};
```

## Store (new typed methods)

```cpp
// store_files.h (members of Store)
Result<AccountRow> account() const;                        Error save_account(const AccountRow&);   Error clear_account();
Error replace_listing(const std::string& folder, const Listing&, int64_t fetched_at);   // one transaction + eviction
Error mark_folder_gone(const std::string& folder);
std::optional<FolderRow> folder(const std::string& path) const;                          // also touches last_access_at
struct Page { std::vector<EntryRow> rows; int total = 0; };
Page entries_page(const std::string& folder, SortKey, bool ascending, int offset, int limit) const;
std::vector<EntryRow> search_cached(std::string_view term, int limit) const;             // entries, favorites, local_files
Error replace_favorites(const std::vector<RemoteEntry>&, int64_t fetched_at);   std::vector<EntryRow> favorites() const;
// durable user data
std::optional<LocalFile> local_file(const std::string& remote_path) const;   std::vector<LocalFile> local_files(int offset, int limit) const;
Error commit_download(const LocalFile&, int64_t job_id);                 // local_files row + journal delete, one transaction
Error forget_local_file(const std::string& remote_path);                 // Space screen only
Error set_server_missing(const std::string& remote_path, bool missing);   Error set_seen_etag(const std::string&, const std::string&);
Error set_mark(const std::string& remote_path, Kind, bool on);   std::vector<MarkRow> marks() const;
Result<int64_t> journal_begin(const DownloadJournalRow&);   Error journal_update(int64_t id, int64_t received, const std::string& state);   Error journal_end(int64_t id);   std::vector<DownloadJournalRow> journal() const;
```

## Row types of the store API (fields as in [data-model.md](../data-model.md))

```cpp
enum class SortKey { Name, Date, Size };
struct AccountRow { std::string base_url, login_name, user_id, server_name, server_version, state, problem_key;
                    std::optional<int64_t> quota_used, quota_total, checked_at; };
struct FolderRow  { std::string path, etag; std::optional<int64_t> fileid, fetched_at; int64_t last_access_at = 0; int entry_count = 0; bool gone = false; };
struct EntryRow   { std::string parent, name, etag, mime, permissions, checksum, reason; Kind kind = Kind::File;
                    std::optional<int64_t> size, mtime, fileid; bool favorite = false; Availability availability = Availability::Ok;
                    std::string source; /* "reader" | "server" | "favorites" | "download" for search results */ };
struct MarkRow    { std::string remote_path; Kind kind = Kind::File; int64_t marked_at = 0; };
struct DownloadJournalRow { int64_t id = 0; std::string remote_path, target_path, expected_etag, state;
                            std::optional<int64_t> expected_size; int64_t received = 0, started_at = 0, updated_at = 0; };
```

## Rules that every implementation of these interfaces must keep (checked by tests)

1. No function sends a write method (N2) or `Depth: infinity`.
2. No function logs a server address, user name, remote/local name or path, search term or token (N4).
3. No function builds a file-system path from a server name except through `local_name.h` (N5).
4. A parser returns a result for every input within its limits; for any other input it returns an error, and it never
   throws, hangs or allocates beyond the limits (fuzz-style hostile fixtures, SC-016).
5. `Downloader::run` never leaves a `.pbc-*.part` file or a journal row for `Done`, `Cancelled` or `Failed`.

## As built (differences from the declarations above)

The code was written test-first against the declarations; where it differs, this section is the truth. Nothing here changes a requirement.

- **Store.** `account()` returns `std::optional<AccountRow>`. `entry(path)` looks in the cached listings, then in the favorites, then in the results of
  the last server search (a temporary table of the connection, never in the database file). `search_cached(term, limit)` returns
  `std::vector<SearchHit>` (`SearchHit{RemoteEntry entry; std::string source}`, source `reader` | `favorites` | `download`); `favorites()` returns
  `std::vector<RemoteEntry>`; `favorites_fetched_at()`, `set_search_results(entries)`, `clear_cache()` (listings only; then `VACUUM`),
  `local_files_bytes()`, `database_bytes()`, `local_file_count()`, `local_path_pairs()`, `is_marked(path)` and `in_scope(path)` were added.
  `StoreOptions` holds `max_cached_entries` and `max_cached_folders`. The setting `cache_owner` (the server and user the cached lists belong to) makes `save_account` drop the cached lists, the favorites and `favorites_fetched_at` when another server or user is saved; downloads and marks stay.
- **Listing, favorites, search.** `nc_listing.h` also holds `clean_search_term`, `kMaxFavorites` (2000) and `kMaxSearchResults` (100).
  `fetch_favorites` and `search_server` return up to one entry more than the cap (the extra one means "more exist"); the `SEARCH` body asks for
  `kMaxSearchResults + 1` results. `FolderService` has `refresh_favorites()` and `search_server(term)` returning `FlatResult{error, entries, more}`;
  both go through the sign-in check and `AuthGuard` like `load`.
- **Names.** `choose_local_path(root, remote_path, taken, options)` takes a `TakenMap` (`build_taken(pairs)`) and `with_suffix`; names are not
  Unicode-normalized (research R-9 is amended); only case-insensitive collisions are resolved.
- **Downloader.** `Downloader(store, http, storage, clock, download_root, wifi_up, options)`; `run(request, cancel, on_progress)`, `resume(job_id, ...)`,
  `recover()`, `root()`. `DownloadRequest.policy` is `ConflictPolicy::{Ask, Replace, KeepBoth}` (a changed copy is decided before any request;
  `KeepBoth` saves "name (server copy).ext" and does not record it). `DownloaderOptions{delays_s, max_wait_s, sleep_ms, hook}`.
  `preflight_download(storage, target_dir, size, warn_bytes)` returns `Preflight{verdict, error_key, size, free, needed}`.
- **Update.** `update.h`: `Updater(store, http, downloader, storage, clock)` with `plan(cancel)` (the walk: one transient listing per folder, at most 500
  folders and 20 levels, nothing downloaded), `execute(plan, cancel, on_progress)` and `run(...)`. `UpdateReport{folders, files, updated, added, skipped,
  need_decision, removed, failed, unchecked_folders, stopped_by_key, decision_paths, removed_paths, failed_paths}`. A copy changed only on the reader
  counts as `skipped`; only a copy changed on both sides is `need_decision`. A folder that was cached before is refreshed like an explicit refresh; one
  that was never opened gets no cache row. The outcome goes to `sync_state` (`success` | `partial` | `failed`, counts as JSON, area `update`).
- **Other modules** not named above: `error_keys`, `auth_guard`, `age_text`, `reader_zone`, `local_files`, `json_min`, `folder_service`, `entry_state`
  (`compute_state`, `state_key`), `hash`, `storage_info`, `update`. `secure_wipe.h` moved to `core/include/pbc/`. The QR encoder accepts 367 bytes.
- **Transport.** A redirect to another host, port or scheme is refused (`redirect_other_host`); `CURLE_PARTIAL_FILE` is `connection_lost`.
- **Log codes** are added to `core/src/log_codes.h` with their first use: `update.run`, `favorites.refresh`, `search.server`, `cache.cleared`, `space.remove`
  (and those of the earlier stories). No code carries a name, a path, a term or an address.
