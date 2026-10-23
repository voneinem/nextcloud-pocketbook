# Data Model: Share Links

**Feature**: [spec.md](spec.md) | **Research**: [research.md](research.md)

**Nothing in this feature is persisted.** The database stays at schema 5 (feature 003); no table, column, setting or file is added;
`VERSION` 0.4.0 changes no stored format, so a 0.3.0 and a 0.4.0 build open the same database. Everything below lives in memory for
the life of a sharing screen (spec FR-011, FR-027). Types are Qt-free (`core/include/pbc/share_types.h`, `share_rules.h`).

## Entities

### ShareTarget (the shareable item)

| Field | Type | Rule |
|-------|------|------|
| `path` | string | a server path of feature 001 (`remote_path.h`): starts with `/`, no `..`, no empty segment; folders without a trailing slash; at most 4,096 bytes |
| `is_folder` | bool | from the 001 entry; decides the permission choices |
| `shareable` | bool | false for an unavailable entry (no permission, encrypted, unsupported share state): Share is disabled with the reason |
| `display_name` | string | the item's name for dialogs, passed through `display_safe()`; never logged |

### ShareKind and LinkPermission

`ShareKind`: `PublicLink` (OCS type 3), `User` (0), `Group` (1), `Other` (every other type). Only `PublicLink` is ever changed.

`LinkPermission` is derived from the permission bits and `is_folder`:

| Bits (share bit `16` is ignored) | File | Folder |
|------|------|--------|
| `1` (read only) | `ReadOnly` | `ReadOnly` |
| contains read (1) and create (4) | `Other` | `ReadUpload` (the value sent for new links is `15`) |
| `4` only (create) | `Other` | `UploadOnly` (listed, never offered for new links) |
| anything else (for example `3`, a file link that recipients may edit) | `Other` | `Other` |

`Other` is shown as "Custom permission" and is changed only if the user picks one of the two offered values (`ReadOnly`,
`ReadUpload`).

### ShareRow (list entry; cannot hold a link)

| Field | Type | Rule |
|-------|------|------|
| `id` | int64 | > 0; rows with a missing or non-numeric id are dropped and counted |
| `kind` | ShareKind | as above |
| `permission` | LinkPermission | for `User`/`Group`: derived the same way, shown read-only |
| `permission_bits` | int | 0..31 |
| `expires` | optional `YYYY-MM-DD` | the server's date; a malformed value gives nullopt and `expires_unreadable = true` |
| `has_password` | bool | links only; computed in the parser, the value itself is not copied out (R-5) |
| `label` | string | links; sanitized, <= 64 bytes, may be empty |
| `with_name` | string | users and groups: the display name, sanitized, <= 64 bytes |
| `owned_by_me` | bool | the answer's owner equals the signed-in user id (feature 001's `AccountCheck::user_id`); false is shown "not yours" and has no change/remove |
| `inherited` | bool | the row came from L3 (a parent folder's share) |
| `limited` | bool | the answer lacks the fields needed to show it (shown as "limited") |

Limits: at most 500 rows kept; ids unique per list (a duplicate id keeps the first).

### ShareDetail (one link, the only holder of a link)

`ShareRow` plus:

| Field | Type | Rule |
|-------|------|------|
| `link` | string | `https://` required, <= 512 bytes, ASCII without controls or blanks; otherwise `link_state = Unreadable` and `link` is empty |
| `link_state` | enum | `Ok`, `DifferentAddress` (host differs from the connected server's), `NotSecure` (`http://`), `Unreadable` |
| `token` | none | the token is part of the link text and is never kept separately |

### ShareRules (what the server says; every field optional = unknown)

| Field | Type | Meaning |
|-------|------|---------|
| `api_enabled` | optional bool | `files_sharing.api_enabled`; false -> "Sharing is not offered on this server" |
| `public_enabled` | optional bool | public links allowed; false -> `share_public_disabled` |
| `password_enforced` | optional bool | any enforcement |
| `password_enforced_for_read_only`, `_read_write`, `_upload_only` | optional bool | per permission, where the server says so; falls back to `password_enforced` |
| `expiry_enabled`, `expiry_enforced` | optional bool | an expiry is offered / required |
| `expiry_days` | optional int (1..3650) | the maximum distance from today in days; out-of-range values are unknown |
| `upload_allowed` | optional bool | `public.upload`; false -> read-and-upload unavailable |
| `resharing` | optional bool | read, shown nowhere (reserved; not used) |

Derived (pure functions, `share_rules.h`): `link_creation_offered`, `password_required(permission)`, `expiry_required`,
`expiry_max_days`, `upload_offered(is_folder)`. Unknown never produces "required" or a limit; unknown upload counts as offered (the
refusal explains), unknown public counts as allowed.

### LinkRequest (Create) and LinkChange (Change)

| Field | Type | Rule |
|-------|------|------|
| `permission` | `ReadOnly` / `ReadUpload` | `ReadUpload` only for a folder |
| `password` | `SecretText` | optional; <= 256 bytes; no control characters; non-empty when given; moved into the request and wiped after it |
| `expiry` | optional `YYYY-MM-DD` | valid calendar date, `today <= date <= today + days` |
| Change adds | `set_password`, `clear_password`, `set_expiry`, `clear_expiry`, `set_permission`: at most one per action | each action is one request |

`SecretText` is move-only, wipes its buffer in the destructor and in `release()` (`secure_wipe.h`), prints nothing, and has no
`operator<<`/`to_string`.

### ShareMessage (what the user is told)

| Field | Type | Rule |
|-------|------|------|
| `key` | string | one of the share error keys ([contracts/core-interfaces.md](contracts/core-interfaces.md)); the UI has one text and one "what to do" line per key |
| `field` | enum | `None`, `Password`, `Expiry`, `Permission`: marks the field the message is about |
| `limit_days` | optional int | filled for `share_expiry_too_far` / `share_expiry_required` |
| `code` | string | `E` + HTTP status for `share_refused`, otherwise empty |

A message holds no link, token, password, address or name.

## Share screen state machine

```
          open()                    L1+L2(+L3) ok
 Closed ----------> Loading --------------------------> Listed <--------------------+
   ^                   | fail (offline/auth/refused)       | showLink(id)            |
   |                   v                                   v                         |
   |                 Failed (message)                 LinkOpen (L4; link in memory)  |
   |                                                       | change / remove       |
   |   leave / account change / app inactive               v                         |
   +------------------------------------------------- Working (one request) -------+
                                                          | unknown outcome -> Listed with message + Refresh
```

* `Loading`, `Working`: one request at a time; a second action gives `busy` and is not queued.
* `Listed`, `LinkOpen`: no request is made without a tap (spec FR-030).
* Any state to `Closed`: `clearSecrets()` (link text, form fields, held `ShareDetail`, rules and rows are dropped).
* `Working` ends in `Listed` (the list is re-read after every create/change/remove) except when the answer is unknown: `Listed` shows
  the stale rows flagged "may be out of date" until Refresh (the app does not claim they are current, FR-032).

## Validation rules (before anything is sent)

| Input | Rule | Message key |
|-------|------|-------------|
| expiry malformed / impossible date | `YYYY-MM-DD`, `civil::valid_date` | `expiry_invalid` (client-side) |
| expiry in the past | `date < today` (server day if known) | `expiry_in_past` |
| expiry beyond the maximum | `date > today + expiry_days` | `share_expiry_too_far` |
| expiry required but empty | `expiry_enforced && empty` | `share_expiry_required` |
| password required but empty | `password_required(permission) && empty` | `share_password_required` |
| password given but empty-after-trim / too long / control chars | <= 256 bytes, printable | `password_invalid` |
| remove password while enforced | `password_required(current permission)` | `share_password_required` |
| upload permission while not allowed | `!upload_offered` | `share_upload_not_allowed` |
| public links not allowed | `public_enabled == false` | `share_public_disabled` |
| not a folder but ReadUpload | | cannot occur (control absent); core refuses with `permission_invalid` |

`expiry_invalid`, `expiry_in_past`, `password_invalid`, `permission_invalid` are client-side validation keys; they are not server
refusals and carry no code.

## Secret lifecycle (where a secret may exist)

| Secret | May exist in | Ends |
|--------|--------------|------|
| Typed password | the QML field; the bridge call's `QString` (filled with zeros after conversion); `SecretText` in the worker job; `Request::body` while the call runs | the field is cleared when the request is handed over; `SecretText` and `Request::body` are wiped after `perform`, on success, failure, cancel and exception |
| Link text | the OCS answer buffer; `ShareDetail::link`; `ShareBridge::linkText` (and the QML property bound to it); the clipboard after an explicit Copy | `clearSecrets()` wipes the answer buffer, the detail and the `QString`; the clipboard is cleared when leaving the link page if it still holds exactly the link |
| Token | inside the link text only | with the link |

**Never**: database, settings, files, logs, error records, exported files, the folder listing cache, notifications, the About box,
stdout/stderr. `test_share_privacy` enforces this by searching everything the app wrote (research R-9).

## Effects on records of earlier features

None. Feature 001's entries and states are read (the `shareable` flag and the item's name and kind); feature 002's queue and feature
003's pairs are not touched; no sharing action is queued, because every one needs a live connection (FR-006).
