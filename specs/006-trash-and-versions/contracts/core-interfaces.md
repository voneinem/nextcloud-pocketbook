# Contract: Core interfaces (`core/`, Qt-free)

**Feature**: [../spec.md](../spec.md) | **Requests**: [trash-requests.md](trash-requests.md) | **Data model**: [../data-model.md](../data-model.md) | **004**: [core-interfaces](../../004-share-links/contracts/core-interfaces.md) | **002**: [core-interfaces](../../002-manage-server-files/contracts/core-interfaces.md)

Declarations only; the code is written test-first (tasks.md). Namespace `pbc`, headers in `core/include/pbc/`, no Qt, no InkView, no SDK
header (constitution II). Existing seams (`Clock`, `HttpTransport`, `DavSession`, `Result<T>`, `Error`, `JsonValue`, `RemoteEntry`,
`fetch_entry`, `check_new_name`, `WriteAnswer`, `Delivery`) are reused. New modules: `trash_types`, `trash_parse`, `trash_rules`,
`trash_api`; additions to `store` (`store_trash.cpp`) and `error_keys`. **Only `trash_api.cpp` names `MOVE` or `DELETE` for the trashbin
and versions endpoints** (rule N2 allow-list gets exactly that file); **only `trash_parse.cpp` and `trash_api.cpp` name the endpoints**
(rule N11).

## trash_types.h

```cpp
inline constexpr size_t kMaxTrashRows = 1000;
inline constexpr size_t kMaxVersionRows = 200;
inline constexpr size_t kMaxTrashBody = 4u << 20;
inline constexpr size_t kMaxVersionBody = 1u << 20;
inline constexpr size_t kMaxLocationBytes = 1024;

enum class FeatureState { On, Off, Unknown };
struct TrashAvailability { FeatureState trash = FeatureState::Unknown, versions = FeatureState::Unknown; std::string code; };   // code "T1-<status>" when a state is Unknown

struct TrashItem {
    std::string trash_name;                 // the unique identity, one path segment (research R-7)
    std::string name;                       // display only
    std::string original_dir, original_name;   // "" when unknown
    bool original_known = false, unreadable = false;
    std::optional<int64_t> deleted_at, size, fileid;
    Kind kind = Kind::File;
    Availability availability = Availability::Ok;
};
struct TrashList {
    std::vector<TrashItem> items;           // newest deletion first
    int total_seen = 0, skipped_hostile = 0;
    bool truncated = false, partial = false, from_store = false;
    std::optional<int64_t> fetched_at;
};

struct VersionRow { std::string version_id; std::optional<int64_t> mtime, size; bool future = false; };
struct VersionList {
    std::vector<VersionRow> rows;           // newest first
    int skipped = 0;  bool truncated = false;
    std::string file_etag;  int64_t file_id = 0;   // the stamp for the stale check
};

struct RestoreTarget { std::string folder, name; bool is_original = false; };
struct OperationResult { Error error; bool outcome_unknown = false; int removed = 0, not_removed = 0, not_tried = 0; std::string code; };
```

## trash_parse.h

```cpp
// A 207 answer of T2 -> TrashList. Total: any input gives either a list (possibly with unreadable items) or an error key
// (listing_too_large, malformed_xml, protocol_error). `user_id` fixes the expected href prefix. `now` bounds deletion times.
Result<TrashList> parse_trash(const Response& r, std::string_view user_id, int64_t now);
std::string trash_request_body();

// A 207 answer of T6 -> VersionList (without the file stamp, which the caller adds).
Result<VersionList> parse_versions(const Response& r, int64_t now);
std::string versions_request_body();

// The segment rule of research R-3 (also used for version ids with its own digit rule).
bool is_safe_segment(std::string_view s);
// The display form of a name from the server: controls removed, <= 255 bytes cut on a character boundary.
std::string display_name(std::string_view s);
// "Books/story.epub" -> {dir "/Books", name "story.epub", known true}; unsafe or too long -> known false.
struct OriginalPlace { std::string dir, name; bool known = false; };
OriginalPlace parse_original_location(std::string_view location);
```

## trash_rules.h (pure)

```cpp
// The "capabilities" object of the answer (ocs.data.capabilities) -> the two states of research R-2; any value that is not an object, no "files"
// object, or a key that is not a boolean gives Unknown for what it concerns. Pure.
TrashAvailability availability_from_capabilities(const JsonValue& capabilities);

struct RestoreProbe { bool original_known = false; std::optional<RemoteEntry> at_original; std::optional<bool> parent_exists; };
enum class RestorePlan { OriginalFree, NameTaken, ParentMissing, ChoosePlace, Unavailable };
RestorePlan plan_restore(const TrashItem& item, const RestoreProbe& probe);

// Validates a chosen target: check_new_name, not the item's own place for a folder, not empty. error_key "" = ok.
struct TargetCheck { bool ok = false; std::string error_key; };   // name_forbidden | restore_into_self
TargetCheck check_restore_target(const TrashItem& item, const RestoreTarget& target, const ServerCaps& caps);

enum class ConfirmKind { RestoreProposal, DeleteFile, DeleteFolder, EmptyTrash, RestoreVersion };
struct ConfirmContent {
    ConfirmKind kind = ConfirmKind::DeleteFile;
    std::vector<std::string> shown_names;     // <= 5, each <= 40 bytes cut on a character boundary
    int items = 0, folders = 0, stage = 1;    // stage 2 only for DeleteFolder and EmptyTrash
    bool truncated_notice = false, destructive = false, default_cancel = true;
    std::string place;                        // RestoreProposal: the folder path shown
};
ConfirmContent confirm_for_delete(const TrashItem& item, int stage);
ConfirmContent confirm_for_empty(const std::vector<TrashItem>& named, bool list_truncated, int stage);
ConfirmContent confirm_for_restore(const TrashItem& item, const RestoreTarget& target);
ConfirmContent confirm_for_version(std::string_view file_name, const VersionRow& row);
int confirm_stages(ConfirmKind kind);          // 1 for DeleteFile, RestoreProposal, RestoreVersion; 2 for DeleteFolder, EmptyTrash

// An item the user can act on (not unreadable, availability Ok).
bool actionable(const TrashItem& item);
// The set Empty trash names: exactly the actionable items of the list as held, in order. Unreadable or unavailable items are counted
// as "cannot be removed here" and are not in the set.
struct EmptySet { std::vector<TrashItem> named; int not_actionable = 0; };
EmptySet empty_set_of(const TrashList& list);

// The key and the short code of a failed request. `op` is 1 (list), 4 (restore), 5 (restore chosen), 6 (versions), 7 (version restore), 8 (delete).
struct Refusal { std::string key, code; };
Refusal refusal_for(int op, long status);
```

## trash_api.h

```cpp
class TrashApi {
public:
    TrashApi(DavSession& session, std::string_view user_id, const Clock& clock);

    // T1. A failure gives both states Unknown, the code "T1-<status>" and the error key of the failure (auth_failed, no_network, server_error, ...):
    // the bridge shows the message of an Auth/Network/Tls key, and the generic message with the code for a Server/Protocol one.
    Result<TrashAvailability> fetch_availability();
    Result<TrashList> list_trash();                                                    // T2 (the caller stores it)

    // T3 probes, then the plan. Never writes.
    Result<RestorePlan> probe_restore(const TrashItem& item, RestoreProbe* out = nullptr);

    // T4: sends the MOVE only when `item` is actionable and its original place is known; verifies with a probe. result.outcome_unknown on a lost answer.
    OperationResult restore_original(const TrashItem& item);
    // T5: `target` must have passed check_restore_target; sends the MOVE with Overwrite: F; verifies.
    OperationResult restore_to(const TrashItem& item, const RestoreTarget& target);

    // T8: ONE trash item. Called only from TrashBridge (rule N12).
    OperationResult delete_item(const TrashItem& item);

    Result<VersionList> list_versions(const RemoteEntry& file);                        // T3 (stamp) + T6
    // Stale check (T3) then T7; verifies (T6 + T3). `shown` is the list the user saw.
    OperationResult restore_version(const RemoteEntry& file, const VersionList& shown, const VersionRow& row);
};

// Pure: the trash-specific key of an answer (status + delivery), and the Empty-trash loop's decision after one result.
Error interpret_trash_answer(const Response& r, int op, bool item_was_present);
enum class LoopStep { Continue, StopAuth, StopRateLimited, StopUnknown };
LoopStep loop_step_after(const OperationResult& r);
```

## store additions (`store.h`, `store_trash.cpp`)

```cpp
// schema 6 (research R-4); they return an Error (ok() = done), as the other store writes do
Error Store::replace_trash(const TrashList& list, int64_t fetched_at, int64_t account_id);   // one transaction
Result<TrashList> Store::load_trash(int64_t account_id) const;                                       // from_store = true; empty list if none
Error Store::remove_trash_item(std::string_view trash_name);
Error Store::clear_trash();                                                                    // also on account change/removal
```

## error_keys additions

New stable keys, each with a plain message and action in `qml/Problems.qml` and a German text in `qml/strings_de.tsv`:
`trash_unavailable`, `versions_unavailable`, `restore_name_taken`, `restore_parent_missing`, `restore_place_unsupported`,
`restore_into_self`, `trash_item_gone`, `version_changed_meanwhile`, `version_gone`, `no_older_versions`, `refresh_first`, `file_gone`,
`outcome_unknown` (reused if it exists from feature 002), `trash_not_actionable`. `test_error_keys` checks that every key is in
`all_error_keys()` with a class and that Problems.qml has a title for each.

## Logging (codes only)

`log_codes.h` gains `trash.open`, `trash.list`, `trash.restore`, `trash.restore_chosen`, `trash.delete`, `trash.empty`, `versions.list`,
`versions.restore`, `trash.fail`. Each log line carries `log::status`, `log::count` and a stable key at most; **no name, location, path,
trash name or version id** (rules R6/R8/N4; spec FR-032). `test_trash_privacy` searches the captured log for a name and a location.
