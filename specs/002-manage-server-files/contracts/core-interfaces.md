# Contract: Core interfaces (`core/`, Qt-free)

**Feature**: [../spec.md](../spec.md) | **Data model**: [../data-model.md](../data-model.md) | **Server contract**: [server-requests.md](server-requests.md) | **001**: [core-interfaces](../../001-nextcloud-files-client/contracts/core-interfaces.md)

Declarations only; the code is written test-first (tasks.md). All live in namespace `pbc`, headers in `core/include/pbc/`,
no Qt, no InkView, no SDK header (constitution II). Existing seams (`Clock`, `Random`, `Cancel`, `HttpTransport`,
`Result<T>`, `Error`, `StorageInfo`, `DavSession`, `Store`) are reused. Error keys are those of
[server-requests.md](server-requests.md).

## Changed existing seams (additive)

```cpp
// http_transport.h
struct Request {
    // ...existing fields...
    // A request body that is a range of a file (PUT of a file or of one chunk). Opened read-only, links not followed, streamed
    // with the 64 KiB read callback; Content-Length is body_length. Exclusive with `body`. Refused before anything is sent
    // when the file is not a regular file, is shorter than offset+length, or cannot be read (Response::error_key = "source_unreadable").
    std::string body_file;
    long long body_offset = 0;
    long long body_length = 0;
    // removed: upload_file, upload_field, upload_file_name (the multipart body no product code uses)
};
enum class Delivery { NotSent, PartlySent, FullySent };   // how far the request got
struct Response {
    // ...existing fields...
    Delivery delivery = Delivery::NotSent;   // meaningful when status == 0; with a status it is FullySent
};

// dav_common.h
// DavSession::status_error additionally maps (server-requests.md W10): 405, 409, 412, 413, 415, 507, 207-with-member-failures.
// It never reads a body for the mapping; the exception class is read by classify_write_answer (below).

// store.h: kSchemaVersion = 4; typed methods for the queue in store_queue.h (members of Store).
```

## Server capabilities for the write side

```cpp
// server_caps.h
struct ServerCaps {
    std::optional<bool> trash;                 // files.undelete; nullopt = unknown (counts as false for Delete)
    std::optional<bool> chunking;              // dav.chunking
    std::optional<int64_t> max_chunk;          // files.chunked_upload.max_size
    std::string forbidden_chars;               // up to 64 characters
    std::vector<std::string> forbidden_names, forbidden_basenames, forbidden_extensions;   // up to 64 x 32 bytes each
    std::optional<int64_t> checked_at;
};
bool delete_offered(const ServerCaps&);                      // trash == true
int64_t chunk_size_for(const ServerCaps&);                   // 10 MiB; the server maximum if in [5 MiB, 10 MiB); never below 5 MiB
bool chunked_available(const ServerCaps&);                   // chunking == true

// nc_server.h: AccountCheck gains `ServerCaps caps;` read from the capabilities answer (parse failures leave a field unknown).
// store.h: std::optional<ServerCaps> server_caps() const;  Error save_server_caps(const ServerCaps&);  cleared by save_account/clear_account.
```

## Names

```cpp
// new_name.h
struct NameCheck { bool ok; std::string error_key; };         // name_forbidden, path_unsafe, ...
// is_valid_name (remote_path.h) plus the server lists of ServerCaps (characters, whole names, base names, extensions; case-insensitive
// for the lists, byte-exact elsewhere). Pure.
NameCheck check_new_name(std::string_view name, const ServerCaps& caps);
// "name (2).ext", "name (3).ext" ...: the first name not in `taken` (compared byte-exactly) that also passes check_new_name; the number
// goes before the last extension, at the end of a name without one or of a dotfile; within 255 bytes the stem is shortened, never the extension.
std::string keep_both_name(std::string_view name, const std::function<bool(std::string_view)>& taken, const ServerCaps& caps);
```

## The reader-side source (picker walk, checks at send)

```cpp
// source_walk.h
struct SourceRules {
    std::string root;                  // the storage root (/mnt/ext1; host: PBC_PICKER_ROOT)
    std::vector<std::string> deny;     // folders that can never be a source (the app's data folder)
    int max_items = 2000;
    int max_depth = 12;
};
enum class SourceKind { File, Folder };
struct SourceEntry { std::string rel;        // path relative to the picked folder ("" for a picked file)
                     std::string name; SourceKind kind; int64_t size = 0; };
struct SourceWalk { std::vector<SourceEntry> entries;   // folders before the files they contain
                    int folders = 0, files = 0; int64_t bytes = 0; int skipped = 0; bool over_limit = false; };
// Canonical path check: the real path of `path` lies under rules.root and not under any rules.deny; a link anywhere on the way is not followed.
Result<std::string> check_source_path(const std::string& path, const SourceRules&);
// Bounded walk of a picked folder; links, hidden entries, non-regular files and unreadable ones are skipped (counted); over the bound the
// walk stops with over_limit = true and the pick is refused by the caller.
SourceWalk walk_source(const std::string& folder, const SourceRules&);

// upload_source.h
struct SourceIdentity { int64_t size = 0; int64_t mtime = 0; };    // mtime in seconds
// Opens read-only with O_NOFOLLOW, fstat: regular file, 1 byte .. 4 GiB-1. Error keys source_unreadable, source_not_allowed, too_large_for_storage.
Result<SourceIdentity> inspect_source(const std::string& path, const SourceRules&);
bool same_identity(const SourceIdentity&, const SourceIdentity&);
Result<std::string> hash_source(const std::string& path, Cancel&, const std::function<void(int64_t done, int64_t total)>& progress);   // SHA-256 hex, 64 KiB, streaming
```

## Queue model and store API

```cpp
// queue_types.h
enum class ItemKind { Upload, Mkdir, Rename, Move, Copy, Delete };
enum class ItemState { Queued, Sending, Done, Failed, Conflict, Unknown };
struct QueueItem {                       // data-model.md queue_items, same names
    int64_t id = 0; std::optional<int64_t> group_id;
    ItemKind kind; Kind entry_kind; ItemState state = ItemState::Queued; std::string step;
    std::string server, user_id;
    std::string src_local; std::optional<int64_t> src_size, src_mtime;
    std::string src_remote, src_etag; std::optional<int64_t> src_fileid;
    std::string dst_parent, dst_name; bool replace = false; std::string dst_etag;
    std::optional<int64_t> depends_on;
    std::string upload_id; std::optional<int64_t> chunk_size; std::string checksum;
    std::string result_etag; std::optional<int64_t> result_fileid;
    std::string reason; int64_t created_at = 0, updated_at = 0;
};
inline constexpr int kMaxQueueItems = 2000;

// store_queue.h (members of Store)
Result<int64_t> queue_add(const QueueItem&);                              // validates (data-model "Validation"), enforces the bound
Result<std::vector<int64_t>> queue_add_group(const std::vector<QueueItem>&);   // all or none; depends_on given as an index into the vector
std::optional<QueueItem> queue_item(int64_t id) const;
struct QueuePage { std::vector<QueueItem> rows; int total = 0; };
QueuePage queue_page(int offset, int limit) const;                        // all states, creation order
int queue_count_open() const;                                             // not Done
int queue_count_sendable(const std::string& server, const std::string& user_id) const;   // Queued, dependency done, same account
std::optional<QueueItem> queue_next(const std::string& server, const std::string& user_id) const;   // oldest sendable
Error queue_update(const QueueItem&);                                     // fields the sender/UI may change (state, step, upload fields, reason, names, replace/dst_etag)
Error queue_remove(int64_t id);                                           // never removes a Sending row; removes dependants' dependency by failing them (reason target_gone)
// One transaction: marks the item Done and applies the effects of research R-10 (cache patch, local copy / mark rewrite or link).
Error queue_confirm(int64_t id, const std::string& etag, std::optional<int64_t> fileid, const ConfirmContext&);
struct ConfirmContext { std::string download_root; std::optional<SourceIdentity> source_now; };   // for the link rule
// At start (no network): Sending -> Queued for steps prepare|chunks, -> Unknown for single|assemble|request. Returns how many of each.
struct RecoveryReport { int requeued = 0, unknown = 0; };
RecoveryReport queue_recover();
Error queue_clear_done();                                                 // leaving the queue screen
// The listing with the overlay of research R-6.8 (data-model "Listing overlay").
struct OverlayRow { RemoteEntry entry; std::string pending; std::string sync_state; int64_t item_id = 0; };
struct OverlayPage { std::vector<OverlayRow> rows; int total = 0; };
OverlayPage entries_page_overlay(const std::string& folder, SortKey, bool ascending, int offset, int limit) const;
std::vector<QueueItem> queue_items_for_source(const std::string& remote_path) const;   // open items about an entry (delete of a folder warns about them)
std::vector<QueueItem> queue_items_below(const std::string& folder) const;               // FR-035
```

## Write operations (one request each)

```cpp
// dav_write.h : the only place (with queue_sender.cpp) that names a write method (rule N2)
struct WriteAnswer {
    Error error;                       // ok() = the server confirmed
    long status = 0;
    Delivery delivery = Delivery::NotSent;
    std::string etag;                  // OC-Etag, else ETag
    std::optional<int64_t> fileid;     // OC-FileId
    bool outcome_unknown = false;      // research R-7 table
    std::string exception_class;       // whitelisted suffix or ""
};
WriteAnswer put_small(DavSession&, std::string_view user_id, const QueueItem&, const std::string& checksum, Cancel&, ProgressFn);
WriteAnswer mkcol(DavSession&, std::string_view user_id, std::string_view remote_path);
WriteAnswer move_entry(DavSession&, std::string_view user_id, const QueueItem&);        // rename, move, replace of a file
WriteAnswer copy_entry(DavSession&, std::string_view user_id, const QueueItem&);
WriteAnswer delete_entry(DavSession&, std::string_view user_id, const QueueItem&);
Error classify_write_answer(const Response&, WriteKind, bool replacing);               // pure: status + body class -> key (server-requests.md W10)
// 412 disambiguation (pure): from the PROPFIND results of target and source.
enum class Cause412 { NameExists, ChangedOnServer, SourceGone };
Cause412 disambiguate_412(const std::optional<RemoteEntry>& target, const std::optional<RemoteEntry>& source, const std::string& expected_source_etag);

// upload_session.h : chunked upload v2
struct ChunkPlan { int64_t chunk_size; int count; };
std::optional<ChunkPlan> plan_chunks(int64_t total, int64_t chunk_size);                // nullopt above 10,000 chunks
std::string chunk_name(int n);                                                          // "00001"
struct UploadFolder { std::set<int> good; int bad = 0; bool gone = false; };
UploadFolder parse_upload_folder(const Response& propfind, const ChunkPlan&);          // hostile/partial safe
class UploadSession {
public:
    UploadSession(DavSession&, std::string_view user_id, QueueItem&, const ServerCaps&, Random&);
    // Steps; each returns the WriteAnswer of its request. The caller (sender) persists the item between steps.
    WriteAnswer open();                                    // MKCOL U
    UploadFolder list();                                   // PROPFIND U Depth 1
    WriteAnswer send_chunk(int n, Cancel&, ProgressFn);    // PUT U/nnnnn
    std::optional<RemoteEntry> precheck();                 // PROPFIND F Depth 0
    WriteAnswer assemble();                                // MOVE U/.file
    std::optional<RemoteEntry> verify();                   // PROPFIND F Depth 0
    WriteAnswer abort();                                   // DELETE U
};

// check_outcome.h : the read-only "Check the server" (research R-7)
enum class Found { Arrived, NotThere, SomethingElse, Unclear };
Found check_outcome(DavSession&, std::string_view user_id, const QueueItem&);
```

## The sender

```cpp
// queue_sender.h : names write methods only through dav_write.h / upload_session.h
struct SendReport { int done = 0, failed = 0, conflict = 0, unknown = 0, left = 0; std::string stopped_by_key; };
struct SendProgress { int64_t item_id; ItemState state; int64_t done = 0, total = 0; std::string step; };
struct SenderOptions { std::function<bool()> wifi_up; ProgressFn on_progress; std::function<void(const char* step)> hook; /* test seam: after "sending", "chunk", "assemble", "request", "confirm" */ };
class QueueSender {
public:
    QueueSender(Store&, HttpTransport&, StorageInfo&, const Clock&, Random&, SourceRules, std::string download_root, SenderOptions = {});
    // One run: research R-15. Blocks the worker thread. Never sleeps or loops on a failure. `only` = a single item (Retry), 0 = the whole queue.
    SendReport run(Cancel&, int64_t only = 0);
    // The user's choices on held items (each is a short transaction; no network except where noted).
    Error resolve_keep_both(int64_t id, const std::string& new_name);        // checks the name, sets queued
    Error resolve_replace(int64_t id);                                       // PROPFIND (network): refreshes dst_etag, then queued
    Error resolve_apply_anyway(int64_t id);                                  // rename/move/copy/delete: refreshes src_etag (network), queued
    Error resolve_discard(int64_t id);                                       // removes the item (upload folder deleted first)
    Found check_unknown(int64_t id);                                         // read-only (network)
    Error mark_unknown_done(int64_t id);                                     // runs the R-10 effects as if confirmed
    Error send_again(int64_t id);                                            // Unknown -> Queued, conditions unchanged
    Error remove_item(int64_t id);
};
```

## Auto Sync

```cpp
// auto_sync.h : time passed in, no timers, no sleeping
enum class AutoSyncStep { Idle, Wait, Start };
struct AutoSyncInputs { bool enabled, has_account, send_running, sendable_items, wifi_up; };
struct AutoSyncDecision { AutoSyncStep step = AutoSyncStep::Idle; int wait_ms = 0; };
class AutoSyncScheduler {
public:
    explicit AutoSyncScheduler(int quiet_ms = 10000);
    void note_action(int64_t now_ms);                      // the user's own action arms it (and restarts the quiet period)
    void send_started(int64_t now_ms);                     // any send, manual or automatic: disarms if it started after the last action
    AutoSyncDecision next(int64_t now_ms, const AutoSyncInputs&);   // research R-14 rules, in order
    bool armed() const;
};
```

## Rules that every implementation of these interfaces must keep (checked by tests)

1. No write request is sent without its condition (server-requests.md table); `test_write_conditions` runs every operation.
2. No function sends `PROPPATCH`, `LOCK`, `UNLOCK`, a trash or version request, or `Depth: infinity`.
3. No function logs a server address, user name, remote or local name or path, ETag, checksum, upload id or server text (N4).
4. The sender never creates, writes, truncates, renames or deletes a file on the reader (N5, N6); source files are opened
   read-only without following links.
5. A parser returns a result for every input within its limits; for any other input an error; it never throws, hangs or
   allocates beyond the limits (hostile fixtures).
6. `QueueSender::run` starts no thread, timer or sleep, and sends nothing for an item in state Conflict, Unknown or Failed.
7. Recovery (`queue_recover`) and every pure function touch no network.
8. `Sending` is set (committed) **before** the first request of an item, and the outcome is committed in the same
   transaction as the effects of R-10.


## Changes made during implementation (2026-10-21)

Additions to the interfaces above; the headers in `core/include/pbc/` are the reference.

- `QueueActions` (`queue_actions.h`): `add_uploads`, `count_folder_pick`, `confirm_folder_pick(pick, PickPolicy)`, `answer_ask(NameAsk, AskChoice, new_name)`, `new_folder`,
  `rename_entry`, `move_or_copy`, `delete_preview`, `delete_entries`, `remove_item`; `NameAsk` (kinds Upload, Move, Copy, Rename), `AddResult{queued, refused, asks}`.
- `Store`: `name_use`, `queue_pending_mkdir`, `queue_confirm(id, etag, fileid, ConfirmContext)` with the effects of research R-10 and the forgetting of cached folder versions.
- `dav_write.h`: `files_url`, `put_small`, `mkcol`, `move_entry`, `copy_entry`, `delete_entry`, `interpret_write_answer`, `disambiguate_412`.
- `nc_listing.h`: `fetch_entry`, `fetch_free_bytes`.
- `QueueSender`: `run`, `check_unknown`, `mark_unknown_done`, `send_again`, `remove_item`, `resolve_keep_both`, `refresh_target`, `resolve_replace`, `refresh_source`,
  `resolve_apply_anyway`, `resolve_edit`, `resolve_discard`; test seam names `sending`, `upload_id`, `chunk`, `assemble`, `delete_old`, `request`, `confirm`.
