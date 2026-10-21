# Contract: Write requests to the Nextcloud server

**Feature**: [../spec.md](../spec.md) | **Research**: [../research.md](../research.md) | **Read side**: [feature 001 contract](../../001-nextcloud-files-client/contracts/server-requests.md) | **API notes**: `docs/reference/nextcloud-api-notes.md`

**Status of every row: UNVERIFIED** (constitution IX). The rows are assembled from the public documentation and RFCs
(4918, 7232, 7233, 3986). They become verified only through an entry in `docs/live-server-checklist.md` (LV-W*). Contract
tests run against the in-process fake in `tests/support/fake_nextcloud_server.h`, which implements *this file* and the 001
contract; a passing test proves the app follows the contract, not that a real server does.

Conventions are those of 001 (base address, `<uid>`, percent-encoded segments, Basic credentials through the transport,
`User-Agent: PBCloud/<version>`, same-host redirects only). Additional rules:

- `<file URL>` is `<base>/remote.php/dav/files/<uid>/<path>` (folder URLs with a trailing `/` only where a row says so).
- `Destination` is always a **full URL** with every segment encoded the same way, on the same origin as `<base>`.
- ETags are sent **exactly as stored** (quotes included). A weak (`W/`) or empty ETag is never sent in `If-Match`: the item is
  not queued (data-model "Validation").
- Request methods added to those of 001: `PUT`, `MKCOL`, `MOVE`, `COPY`, `DELETE`. Never sent: `PROPPATCH`, `LOCK`,
  `UNLOCK`, anything under `/remote.php/dav/trashbin/` or `/versions/`, `Depth: infinity`.
- In-memory answers are capped at 8 MiB as before; request bodies of file data stream from the source file with a 64 KiB
  buffer (never held in memory).
- Every write request carries a **condition** as the table says; `tests/integration/test_write_conditions.cpp` fails if one
  does not.

## W1. Capabilities read for the write side (extends 001 section 2)

`GET <base>/ocs/v1.php/cloud/capabilities?format=json` (the account check, same request) additionally reads, if present:

| JSON path under `ocs.data.capabilities` | Use |
|-----------------------------------------|-----|
| `files.undelete` (bool) | trash available -> Delete offered (unknown = not available) |
| `dav.chunking` (string, e.g. `1.0`) | chunked upload v2 available |
| `files.chunked_upload.max_size` (integer bytes) | maximum chunk size; used if smaller than 10 MiB and at least 5 MiB |
| `files.forbidden_filename_characters` / `forbidden_filenames` / `forbidden_filename_basenames` / `forbidden_filename_extensions` (arrays of strings) | name validation (research R-11); **the exact keys are unverified (LV-W10)**: a missing key is "no list" |

Bounded as in the data model; non-string members are skipped; a malformed value leaves the field unknown.

## W2. Quota before a send (the 001 request, section 4)

`PROPFIND <base>/remote.php/dav/files/<uid>/`, `Depth: 0` -> `quota-available-bytes` for the free-space check of a run
(research R-15.2). A failure here is not fatal for the run: sending goes on without the pre-check.

## W3. Small upload (file size <= chunk size)

| | |
|--|--|
| Request | `PUT <file URL>` with the file as body |
| Headers | `Content-Type: application/octet-stream`; `Content-Length`; `X-OC-MTime: <seconds>`; `OC-Checksum: SHA256:<hex>`; **new:** `If-None-Match: *`; **replace:** `If-Match: <etag>` |
| OK | 201 (created), 204 or 200 (replaced). Reads `OC-Etag` (else `ETag`) and `OC-FileId` from the answer; if neither ETag header is present, a `PROPFIND` `Depth: 0` on the target supplies ETag and file id |
| Errors | 412 -> `name_exists` (new) / `changed_on_server` (replace); 409 -> `target_gone`; 507 -> `server_storage_full`; 413 -> `file_too_large_server`; 415 -> `type_not_accepted`; 403/400 -> `name_forbidden` or `not_allowed` (section W12); 423 -> `locked`; 404 -> `source_gone` for a replace or `target_gone`; 401, 429, 503, 5xx as in W12 |
| Never | `PUT` without one of the two conditions; `OC-Chunked`; `X-NC-WebDAV-Auto-Mkcol` |

## W4. Chunked upload v2 (file size > chunk size; `dav.chunking` present)

Upload folder `U = <base>/remote.php/dav/uploads/<uid>/pbc-<32 hex>`; final URL `F` = the `<file URL>` of the target.

| Step | Request | Headers | OK | Notes |
|------|---------|---------|----|-------|
| 1 | `MKCOL U` | `Destination: F`, `OC-Total-Length: <bytes>` | 201 | id stored in the row before this request |
| 2 | `PUT U/<nnnnn>` (nnnnn = `00001` ...) | `Destination: F`, `OC-Total-Length`, `Content-Length` | 201 | body = bytes `[(n-1)*c, min(n*c, total))` of the source; one in flight |
| resume | `PROPFIND U` `Depth: 1` | - | 207 | chunk names + `getcontentlength`; a chunk counts only if its size is the expected size; other chunks are sent again |
| 3a | `PROPFIND F` `Depth: 0` | - | 207 / 404 | **pre-check** (research R-3): new + exists -> `name_exists`; replace + ETag differs from `dst_etag` -> `changed_on_server` |
| 3b | `MOVE U/.file` | `Destination: F`, `OC-Total-Length`, `X-OC-Mtime: <seconds>`, `OC-Checksum: SHA256:<hex>`; always `Overwrite: F` (a replace first deletes the old version with `DELETE` and `If-Match: <dst_etag>`, row 3a') | 201 (new), 204 (replaced) | reads `OC-Etag`, `OC-FileId` |
| 3c | `PROPFIND F` `Depth: 0` | - | 207 | verifies `getcontentlength` = total (and `oc:checksums`, when reported, = ours) -> done; otherwise `size_mismatch` / `checksum_mismatch` |
| abort | `DELETE U` | - | 204 / 404 | cancel never deletes; **Remove** of an item or a changed source does |

Notes: the upload folder is the app's own temporary session under `/dav/uploads/`, not user data; steps 1, 2 and `DELETE U`
have no condition by design (research R-3); chunks are not visible in the user's files; an upload folder expires after 24 hours
of inactivity (manual) -> a resume that finds none starts over with a new id. A server without `dav.chunking`: the file is
sent by W3 as one `PUT` (no resume).

## W5. New folder

| | |
|--|--|
| Request | `MKCOL <file URL>/` (trailing slash) |
| OK | 201; reads `OC-Etag`, `OC-FileId` (else `PROPFIND` `Depth: 0`) |
| Errors | 405 -> `name_exists`; 409 -> `target_gone`; 403 -> `name_forbidden`/`not_allowed`; 423 -> `locked`; 507 -> `server_storage_full` |

## W6. Rename and move

| | |
|--|--|
| Request | `MOVE <source file URL>` (a folder source with a trailing `/`) |
| Headers | `Destination: <target URL>`; `Overwrite: F`; `If-Match: <source etag>`. Replace of a **file** (user-confirmed): never `Overwrite: T`; the target is deleted first with `DELETE` and `If-Match` (research R-3) |
| OK | 201 (created at the target), 204 (replaced) |
| Errors | 412 -> disambiguated by `PROPFIND` `Depth: 0` of target and source: target exists -> `name_exists`; else `changed_on_server` / `source_gone`; 409 -> `target_gone`; 403 -> `not_allowed` (also: the target lies inside the source); 404 -> `source_gone`; 423 -> `locked`; 507 -> `server_storage_full`; 207 -> `partial_failure` |
| Notes | rename = move within the folder (the target differs only in the last segment); a move keeps `oc:fileid`; destinations inside the source folder are refused before queuing |

## W7. Copy

Same as W6 with `COPY`. A copy creates a new `oc:fileid`; the new ETag and file id are read from the answer headers or a
`PROPFIND` `Depth: 0`. A copy of a folder copies its whole content on the server (no data through the reader).

## W8. Delete

| | |
|--|--|
| Request | `DELETE <file URL>` (a folder with a trailing `/`) |
| Header | `If-Match: <etag>` |
| OK | 204 or 200 (moved to the trash by the server); **404 is also done** ("already gone"); a folder `DELETE` is recursive |
| Errors | 412 -> `changed_on_server`; 403 -> `not_allowed`; 423 -> `locked`; 207 -> `partial_failure` (the cached folder is not patched) |
| Offered only if | `server_caps.trash = 1` (W1). Otherwise no request of this kind is ever sent |

## W9. Outcome check and conflict checks (read-only)

`PROPFIND <URL>` with `Depth: 0` and the body of 001 section 5 (properties `getetag`, `getlastmodified`, `getcontentlength`,
`resourcetype`, `oc:fileid`, `oc:checksums`, `oc:permissions`). Used for: the 412 disambiguation (W6), the pre-checks (W4 3a,
research R-3), the "Check the server" action (research R-7) and the ETag refresh at a conflict choice (research R-8.5).
Parsed by the existing listing parser with `single_folder = true`; 404 means "not there".

## W10. Mapping of answers to keys (research R-12)

Order of evaluation: transport error and `delivery` first (research R-7), then the status, then the **exception class** in a
sabre error body (`<s:exception>`), read by the minimal XML reader with a 64 KiB cap, compared against a fixed list of
known class-name suffixes (`InvalidPath`, `ForbiddenFileName`, `FileLocked`, `NotFound`, `EntityTooLarge`,
`UnsupportedMediaType`, `InsufficientStorage`, `Forbidden`); an unknown or hostile class is ignored. **No text from the
body is ever shown, stored or logged.**

| Status | Key |
|--------|-----|
| 507 / `InsufficientStorage` | `server_storage_full` |
| 413 / `EntityTooLarge` | `file_too_large_server` |
| 415 / `UnsupportedMediaType` | `type_not_accepted` |
| 403 or 400 with `InvalidPath` / `ForbiddenFileName` | `name_forbidden` |
| 403 otherwise | `not_allowed` |
| 423 / `FileLocked` | `locked` |
| 412 | `name_exists` or `changed_on_server` (per request, W3-W8) |
| 405 on `MKCOL` | `name_exists` |
| 409 | `target_gone` |
| 404 | `source_gone` (delete: done) |
| 207 | `partial_failure` |
| 401, 429, 503 | `auth_failed`, `rate_limited` (with `Retry-After`), `maintenance` |
| 500, 502, 504 on a write | `unknown` (research R-7) |
| other | `server_refused` + the numeric status as the short code |

## Error keys added to 001's list

`server_storage_full`, `file_too_large_server`, `type_not_accepted`, `name_forbidden`, `not_allowed`, `changed_on_server`,
`name_exists`, `target_gone`, `source_gone`, `partial_failure`, `server_refused`, `checksum_mismatch` and `size_mismatch`
(exist), `queue_full`, `refresh_first`, `source_unreadable`, `source_changed`, `source_not_allowed`, `trash_unavailable`,
`outcome_unknown`, `other_account`, `no_connection`. The first group is `Error::key` of the core; the UI has one message and
one "what to do" line per key, in English with a German translation, and a message never contains an address, a user name
or any name from the server.

## Fixtures

Anonymized answers for each row go to `tests/fixtures/nextcloud/` with the `.unverified` suffix (reserved example domains):
`put_201`, `put_412`, `put_507`, `put_413`, `put_415`, `put_403_invalid_path`, `put_423`, `mkcol_201`, `mkcol_405`,
`move_201`, `move_204`, `move_412`, `move_207_partial`, `copy_201`, `delete_204`, `delete_412`, `delete_207_partial`,
`upload_folder_propfind` (with a partial set of chunks, a wrong-size chunk, a hostile name), `capabilities_write`,
`sabre_error_hostile` (an exception class and message with markup and control characters). Hostile and partial cases are
listed per parser in tasks.md.
