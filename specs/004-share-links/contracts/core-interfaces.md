# Contract: Core interfaces (`core/`, Qt-free)

**Feature**: [../spec.md](../spec.md) | **Requests**: [share-requests.md](share-requests.md) | **Data model**: [../data-model.md](../data-model.md) | **003**: [core-interfaces](../../003-folder-sync/contracts/core-interfaces.md) | **002**: [core-interfaces](../../002-manage-server-files/contracts/core-interfaces.md)

Declarations only; the code is written test-first (tasks.md). Namespace `pbc`, headers in `core/include/pbc/`, no Qt, no InkView, no
SDK header (constitution II). Existing seams (`Clock`, `HttpTransport`, `DavSession`, `Result<T>`, `Error`, `JsonValue`, `ServerCaps`,
`AccountCheck`) are reused. New modules: `share_types`, `share_rules`, `share_date`, `share_parse`, `share_api`, `share_diagnose`,
`secret_text`. **Only `share_api.cpp` names `PUT` or `DELETE`** (rule N2 allow-list gets exactly that file). No new store member, no
schema change.

## secret_text.h

```cpp
// A secret string: move-only, wiped (secure_wipe) in the destructor and by release(); no copy, no stream operator, no to_string.
class SecretText {
public:
    SecretText() = default;
    explicit SecretText(std::string&& value);     // takes the buffer (no copy remains); an lvalue is refused at compile time
    SecretText(SecretText&&) noexcept;            SecretText& operator=(SecretText&&) noexcept;
    SecretText(const SecretText&) = delete;       SecretText& operator=(const SecretText&) = delete;
    ~SecretText();
    bool empty() const;
    size_t size() const;
    const std::string& reveal() const;            // for form_encode only (share_api.cpp); N8 forbids it elsewhere in share_*
    void wipe();
};
```

## share_types.h

```cpp
enum class ShareKind { PublicLink, User, Group, Other };
enum class LinkPermission { ReadOnly, ReadUpload, UploadOnly, Other };
enum class LinkState { Ok, DifferentAddress, NotSecure, Unreadable };

struct ShareRow {                       // cannot hold a link (R-5)
    int64_t id = 0;  ShareKind kind = ShareKind::Other;
    LinkPermission permission = LinkPermission::Other;  int permission_bits = 0;
    std::optional<std::string> expires;  bool expires_unreadable = false;     // "YYYY-MM-DD"
    bool has_password = false;  std::string label, with_name;                 // sanitized, <= 64 bytes
    bool owned_by_me = false, inherited = false, limited = false;
};
struct ShareDetail { ShareRow row; std::string link;  LinkState link_state = LinkState::Unreadable; };   // link empty unless Ok/DifferentAddress/NotSecure

struct ShareTarget { std::string path;  bool is_folder = false; };

struct LinkRequest {                    // Create
    LinkPermission permission = LinkPermission::ReadOnly;                     // ReadOnly | ReadUpload
    SecretText password;  std::optional<std::string> expiry;                  // "YYYY-MM-DD"
};
struct LinkChange {                     // Change: exactly one action
    enum class Action { SetPassword, ClearPassword, SetExpiry, ClearExpiry, SetPermission } action;
    SecretText password;  std::string expiry;  LinkPermission permission = LinkPermission::ReadOnly;
};
struct Snapshot { ShareKind kind; int permission_bits; std::optional<std::string> expires; bool has_password; };   // R-7
Snapshot snapshot_of(const ShareRow&);
LinkPermission link_permission_from_bits(int bits, bool is_folder);   // data-model table; the reshare bit 16 is ignored
int link_bits_for(LinkPermission);                                      // ReadOnly -> 1, ReadUpload -> 15, else 0
```

## share_rules.h

```cpp
struct ShareRules {
    std::optional<bool> api_enabled, public_enabled, upload_allowed, resharing;
    std::optional<bool> password_enforced, password_enforced_read_only, password_enforced_read_write, password_enforced_upload_only;
    std::optional<bool> expiry_enabled, expiry_enforced;  std::optional<int> expiry_days;     // 1..3650, else unknown
};
// Total: anything else gives unknown fields. `capabilities` is the "capabilities" object of the OCS answer.
ShareRules share_rules_from_capabilities(const JsonValue& capabilities);

bool sharing_offered(const ShareRules&);                              // api_enabled != false
bool link_creation_offered(const ShareRules&);                        // sharing_offered && public_enabled != false
bool password_required(const ShareRules&, LinkPermission);            // enforced_for_<permission> if present, else password_enforced == true
bool expiry_required(const ShareRules&);                              // expiry_enforced == true
std::optional<int> expiry_max_days(const ShareRules&);                // only when expiry_enforced and expiry_days is known (otherwise days is just a default)
bool upload_offered(const ShareRules&, bool is_folder);               // is_folder && upload_allowed != false
```

## share_date.h

```cpp
// "YYYY-MM-DD" only: four digits, two, two; a real calendar date; year 1970..9999. Nothing else (no time, no offset, no blanks).
std::optional<int64_t> parse_iso_day(std::string_view text);          // days since 1970-01-01 (civil.h)
std::string format_iso_day(int64_t days);
// `today`: days of the server's day (DavSession::last_server_time) or the reader's clock when unknown.
Error check_expiry(std::string_view text, int64_t today, std::optional<int> max_days);   // expiry_invalid | expiry_in_past | share_expiry_too_far
```

## share_parse.h

```cpp
struct ShareList { std::vector<ShareRow> rows;  int dropped = 0;  bool truncated = false; };
// L2/L3: total; rows beyond 500 set truncated; no code path reads "url" or "token".
Result<ShareList> parse_share_list(std::string_view body, const std::string& my_user_id, bool inherited);
// L4/L5: the first (or only) share object; the link is checked per data-model "ShareDetail".
Result<ShareDetail> parse_share_detail(std::string_view body, const std::string& my_user_id, std::string_view connected_host);
// The OCS envelope: Ok when statuscode is 100 or 200; otherwise the statuscode as an integer (never the message).
struct OcsStatus { bool ok = false;  int statuscode = 0; };
Result<OcsStatus> parse_ocs_status(std::string_view body);
```

Errors: `protocol_error` (key only, detail names the kind of problem, never content).

## share_diagnose.h

```cpp
struct ShareMessage {
    enum class Field { None, Password, Expiry, Permission };
    static constexpr unsigned kPassword = 1, kExpiry = 2, kPermission = 4;
    std::string key;                    // see "Error keys"; empty = nothing to report
    Field field = Field::None;          // the control the first message is about
    unsigned field_mask = 0;            // every control a violated rule is about
    std::optional<int> limit_days;      // the server's maximum, when an expiry rule is violated and the maximum is known
    std::string code;                   // "E403" for share_refused
};
enum class ShareOp { List, Create, Change, Remove };
// Checks a request against the rules BEFORE sending. Returns an empty key when the request may be sent.
ShareMessage validate_create(const ShareRules&, const ShareTarget&, const LinkRequest&, int64_t today);
ShareMessage validate_change(const ShareRules&, const ShareTarget&, const ShareRow& current, const LinkChange&, int64_t today);
struct RefusalContext {
    const ShareRules& fresh;  ShareTarget target;  ShareOp op;
    const LinkRequest* create;  const LinkChange* change;  const ShareRow* current;
    long http_status;  int ocs_status;                 // a 2xx answer with an OCS statuscode >= 400 counts as that status
    std::optional<bool> item_exists;                   // the one Depth-0 PROPFIND made on a 403/404 of the path (research R-4)
    int64_t today;
};
// R-4: after a server refusal and a fresh L1: the first violated rule, else item-gone / item-not-shareable (create, list) or
// not-changeable / gone (change, remove), else refused + code. 401/429/503 keep auth_failed / rate_limited / maintenance.
ShareMessage diagnose_refusal(const RefusalContext&);
```

Order of the rules: sharing off, public links off, password, expiry, upload. Format errors come first in `validate_*`
(`permission_invalid`, `password_invalid`, `expiry_invalid`, `expiry_in_past`). A password is checked through `SecretText::size()`,
`has_control_character()` and `all_blank()`, so the validators never call `reveal()`.

## share_api.h

```cpp
class ShareApi {
public:
    ShareApi(DavSession& session, const Clock& clock, std::string user_id);
    struct Listing { ShareList own;  ShareList inherited;  bool inherited_failed = false; };
    Result<ShareRules> fetch_rules();                                                                    // L1
    Result<Listing> list(const ShareTarget&);                                                            // L2 + L3
    Result<ShareDetail> detail(int64_t id, bool item_is_folder);                                         // L4
    // `rules` are those the screen holds (fetched when it opened); a violation is reported before anything is sent.
    Result<ShareDetail> create(const ShareTarget&, const ShareRules&, LinkRequest&&);                    // L5 POST; wipes the password
    Result<ShareDetail> change(const ShareTarget&, const ShareRules&, int64_t id, const Snapshot& shown, LinkChange&&);   // L4, compare, L5 PUT
    Error remove_link(int64_t id, const ShareTarget&, const ShareRules&, bool owned_by_me = true);                                  // L6; share_gone for 404
    // About the last call that failed (cleared by the next call):
    const ShareMessage& message() const;    // key, marked fields, limit_days, short code; key mirrors the Error key
    const ShareRow& current() const;        // after share_changed_meanwhile: the share as it is now
    bool outcome_unknown() const;           // R-8: the request may have been processed
};
// Exposed for tests: the form body and its encoding (the only place a password is turned into bytes).
std::string form_encode(const std::vector<std::pair<std::string, std::string>>& fields);
```

Every method returns after exactly the requests its row in share-requests.md lists; none retries; none logs a field other than
`log::status` and `log::count` under the codes `share.open`, `share.create`, `share.change`, `share.remove`, `share.fail`
(rule N8). A password is held in a `SecretText` inside the call; `Request::body` is wiped with `secure_wipe` after `perform`, on every
path (a scope guard).

## Error keys (added to `error_keys.cpp`, one UI text and one "what to do" line each)

Classes are those of the existing `ErrorClass` (`types.h`): None, Network, Tls, Auth, Server, Protocol, Unsupported, Storage, Cancelled. There is no class for conflicts or validation: a refusal by the server is `Server`, a lost answer is `Network`, and client-side input errors are `Protocol` (they never come from the network; the UI treats them by key, not by class).

| Key | Class | Meaning |
|-----|-------|---------|
| `share_not_offered` | Server | sharing is not available on this server |
| `share_public_disabled` | Server | public links are not allowed |
| `share_password_required` | Server | a password is required (field marked) |
| `share_expiry_required` | Server | an expiry date is required (field marked; maximum when known) |
| `share_expiry_too_far` | Server | the date is beyond the server's maximum |
| `share_upload_not_allowed` | Server | read-and-upload is not allowed for links |
| `share_item_not_shareable` | Server | this item cannot be shared |
| `share_item_gone` | Server | the item was renamed, moved or deleted since the listing (the Depth-0 check answered 404) |
| `share_not_changeable` | Server | the user may not change this share |
| `share_gone` | Server | the share no longer exists (remove: "already removed") |
| `share_changed_meanwhile` | Server | the link changed after the screen was opened |
| `share_unknown_outcome` | Network | the request went out, the answer is unknown; Refresh |
| `share_list_too_big` | Server | more shares than can be listed |
| `share_refused` | Server | refused without a known reason; short code |
| `expiry_invalid`, `expiry_in_past`, `password_invalid`, `permission_invalid` | Protocol | client-side input errors; send nothing |

`test_error_keys` asserts that every key has a German translation and a "what to do" line.

## Changed existing seams (additive)

```cpp
// log_codes.h: + "share.open", "share.create", "share.change", "share.remove", "share.fail"   (test_log polices use and listing)
// error_keys.cpp: + the keys above
// types.h: nothing
// nc_server.h: nothing (L1 is requested by ShareApi::fetch_rules with the same headers; check_account/quick_check unchanged)
```

## As built (2026-10-22)

- `ShareApi::remove` is `remove_link(id, target, rules, owned_by_me = true)`; `detail(id, item_is_folder)`; `ShareOp` has `Detail`; `Snapshot` has `owned_by_me`;
  `parse_share_list` and `parse_share_detail` take `item_is_folder`; `ShareApi::server_time()` gives the HTTP `Date` of the last answer.
- `SecretText` also has `has_control_character()` and `all_blank()` so that the validators never call `reveal()`.
- `DavSession::send(method, url, body, headers, cap, total_timeout_s = 0, secret_body = false)`.

