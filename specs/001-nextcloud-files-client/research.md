# Research: Read-only Nextcloud Files Client

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **Date**: 2026-10-19

Everything that depends on a real Nextcloud server or on the InkPad One is **unverified** until it has an entry in
`docs/live-server-checklist.md` (LV-*) or `docs/device-test-checklist.md` (DT-*) with a date (constitution IX).
The server facts come from `docs/reference/nextcloud-api-notes.md`, which is itself unverified. Each decision below
names what it relies on and how a wrong assumption is contained.

Format per item: **Decision**, **Rationale**, **Alternatives considered**, **Unverified**.

---

## R-1 Starting point: what the baseline gives and what is missing

**Decision.** Build on the baseline (`core/`, `bridge/`, `qml/`, `app/`) and add modules; change only three existing
seams:

1. `HttpTransport` / `Request` (`core/include/pbc/http_transport.h`): add `save_append` (continue a partial file),
   `progress` (a callback that sees bytes done/total and can cancel) and a `buffer_size` hint. A `FakeTransport` that
   ignores them keeps working.
2. `dav_common.h`: add the `oc:` and `nc:` namespace constants, `DavSession::send` keeps its signature.
3. `Store` (`store.h`): schema 2 (see [data-model.md](data-model.md)) and the typed methods for it, in new
   `store_files.cpp`.

The baseline already has: https-only TLS policy with a CA selector and `custom-ca.pem`, the file sink
(`Request::save_body_to`) with a size cap and ENOSPC detection, the minimal XML reader (DOCTYPE and entities refused),
`DavSession` (same-host rule, credentials never in the URL), `Credentials` file, `clock_check`, the single worker
thread, `SettingsBridge`, `AboutBridge`, QML components (TopBar, BottomPanel, IconButton, InkRow, ModelPager,
PageButtons, dialogs) and the check scripts.

**Rationale.** Constitution V (simplicity) and XI (same components). The seams change additively, so the existing
tests keep their meaning.

**Alternatives.** A second transport class for downloads: rejected, two TLS code paths double the review surface.

**Unverified.** That the bundled libcurl behaves on the device for HTTPS, redirects, ranges and timeouts (constitution
Technology section; `docs/device-findings.md` has only the protocol list). First task of the transfer work is a
device probe (DT-NC-1) before the download design is frozen; the plan states the fallback (full restart, no resume).

---

## R-2 Server contract used by this feature

**Decision.** Only the read side of the contract in `nextcloud-api-notes.md`:

| Need | Call | Notes |
|------|------|-------|
| Is this a Nextcloud, version, maintenance | `GET /status.php` (no auth) | fields `installed`, `maintenance`, `needsDbUpgrade`, `version`, `versionstring`, `productname` |
| Credentials, server name, version | `GET /ocs/v1.php/cloud/capabilities` with Basic auth | `OCS-APIRequest: true`, `Accept: application/json`; `version`, `capabilities.theming.name` |
| User id (differs from login name) | `GET /ocs/v1.php/cloud/user` | `id`; WebDAV paths use it |
| Quota | `PROPFIND` depth 0 on the home with `d:quota-available-bytes`, `d:quota-used-bytes` | one more request in the Test |
| File listing | `PROPFIND` `Depth: 1` | never `infinity` |
| Details of one item (Update of a single kept file) | `PROPFIND` `Depth: 0`, or the parent's depth-1 listing | prefer the parent listing: one request serves all kept siblings |
| Download | `GET`, with `Range` + `If-Range` to resume | |
| Favorites | `REPORT` with `oc:filter-files` | |
| Server search | `SEARCH /remote.php/dav/` `d:basicsearch`, `d:like` on `displayname` | whole account |
| Login | `POST /index.php/login/v2`, then `POST <poll endpoint>` | |

No write method is ever sent (FR-033). Rule `N2` of `check-constitution.sh` (R-16) fails the build when a request-building
source file names `PUT`, `MKCOL`, `MOVE`, `COPY`, `DELETE`, `PROPPATCH`, `LOCK` or `UNLOCK` as an HTTP method.

**Minimum server version.** 30 (api-notes section 3.1): below it the Test says "server too old". Features are gated by
what the server answers, not by the version; the version only produces the "too old" message and a soft note for
versions past end of life. **Unverified** (LV-N1).

**Alternatives.** The legacy `/remote.php/webdav/` path: rejected (api-notes 1). OCS `v2` for the user call: v1 is the
form the manual names; `statuscode` in the body is authoritative either way.

---

## R-3 Error classes for the connection Test (US1)

**Decision.** The Test runs these steps in order and stops at the first failure. Every outcome is a stable key that
the UI maps to one message with a next step (see [contracts/server-requests.md](contracts/server-requests.md) for the
exact mapping).

| Step | Request | Failure keys |
|------|---------|--------------|
| 0 local | clock plausibility, address syntax | `scheme_not_allowed`, `address_invalid`, `clock_implausible` |
| 1 | `GET /status.php` | TLS keys of `map_curl_error` (`cert_untrusted`, `cert_expired`, `cert_host_mismatch`), `server_unreachable`, `no_network`, `not_nextcloud` (not JSON, or `installed` missing), `maintenance`, `server_too_old` |
| 2 | `GET .../cloud/capabilities`, Basic | `auth_failed`, `auth_app_password_required`, `maintenance` (503), `rate_limited` (429) |
| 3 | `GET .../cloud/user` | `auth_failed`, `protocol_error` |
| 4 | `PROPFIND` depth 0 on home, quota | `no_webdav` (404/405/501 on the DAV path), `forbidden`, `protocol_error` |

- **Two-factor authentication.** Nextcloud answers a Basic-auth request with an account password on a 2FA account with
  a 401. Whether it says so in a way a client can read is **not known**. Rule: a 401 is `auth_failed`, and the message
  for `auth_failed` *always* contains the sentence that an app password is needed when two-factor authentication is on
  and how to create one. The distinct key `auth_app_password_required` is produced only if the answer carries a
  recognizable marker (an `app password` hint in the OCS `message`, bounded to 512 bytes, case-insensitive). The marker
  is a guess: **unverified** (LV-N2); the spec scenario 6 is satisfied by the always-present hint, and by the distinct
  key when the server cooperates.
- **Wrong clock.** (a) A local test: `now` earlier than the build time of the binary (`version.cpp` gets the build
  epoch) is `clock_implausible` and is checked before any connection. (b) After any success, the HTTP `Date` header
  gives `clock_skew` (existing `clock_check.h`); more than 600 s raises the warning banner. (c) A TLS failure with
  `cert_expired` adds "check the reader's date and time" to its message (the baseline has no separate key for a
  certificate that is not yet valid). No connection is made
  just for the clock.
- **Maintenance.** `status.php` `maintenance: true`, or 503 on a later call, gives `maintenance` and no retry loop.
- **Server name.** `capabilities.theming.name` if present, else `productname`, else "Nextcloud"; length bounded to 64
  characters and control characters removed before display.

**Rationale.** Matches the spec's eight named problems one to one (FR-006) and keeps each testable with a fake.

**Alternatives.** One generic "cannot connect" message: rejected by FR-006.

**Unverified.** Status fields (api-notes 3), the 2FA answer, the maintenance answer (503 on DAV is inferred), quota
property sentinels (api-notes 9). LV-N1, LV-N2, LV-N3.

---

## R-4 Login without typing a password (US1)

**Decision.** Login Flow v2 as in api-notes section 4, with these constraints:

1. `POST /index.php/login/v2` anonymous. The returned `poll.endpoint` and `login` must be **https and the same origin**
   as the address the user typed; anything else is refused (`redirect_other_host`), so a hostile server cannot make
   the reader send the token elsewhere.
2. The poll token lives **in memory only** (never in the database, the settings or a log). A restart of the app ends
   the flow; the user starts again. Valid time is 20 minutes; the screen shows the remaining time coarsely.
3. **No polling timer.** The user taps **Check**; the app sends exactly one `POST` to the poll endpoint. 404 means
   "not approved yet" and the flow stays open; 200 stores `server`, `loginName`, `appPassword` at once (the answer
   comes only once), then the token is discarded. This satisfies "no background network activity" and the spec's tap
   on Check.
4. After success the app resolves the user id (`cloud/user`) and runs the normal Test.
5. **What is shown.** The login URL as large, selectable plain text, broken into groups, and as a **QR code** drawn
   from an in-house encoder (R-5). The URL's token is the only identifier Login Flow v2 offers.

**Decided (owner, after analysis): the spec was amended to "a link and a QR code" (FR-003, US1 scenario 2).** Earlier wording of the spec: "a link and a **short code**".
Login Flow v2 has **no short code**: the server issues one long URL and a poll token, and the browser page shows no
code to compare. A locally invented code would not be checkable by the user and would only look like a security
feature. The plan therefore delivers *link plus QR code*, and the spec now says so.

**Alternatives.** The reader's browser (`OpenBook(BROWSER_FOR_AUTH, url)` as notes-pocketbook does for links): works
but the built-in browser is slow and its use for a login is a larger trust question; offered as a secondary
"open here" action is rejected (YAGNI, and `Qt.openUrlExternally` is banned by `check-ui-style.sh` U6). Device flow
(RFC 8628): the server does not offer it.

**Unverified.** Login Flow v2 end to end (LV-N4). The token length is assumed to fit a QR code of version 10 or less;
the encoder refuses longer input and the UI then shows only the text (tested).

---

## R-5 QR code encoder

**Decision.** A small Qt-free encoder in `core/src/qr_code.cpp`: byte mode, error correction level L or M, versions
1 to 12 (up to 367 bytes at level L), mask selection by the standard penalty rule, output a square boolean matrix. The
bridge turns the matrix into an image (`QQuickImageProvider`), drawn black and white with integer scale and a quiet
zone.

**Rationale.** No third-party code (constitution V, VII: nothing to attribute), roughly 300 lines, and it is
testable on the host with known vectors (ISO/IEC 18004 example codewords) and a round-trip decoder check in the test.
The alternative needs a dependency or typing 120 characters.

**Alternatives.** libqrencode (LGPL): rejected, license and a new ARM dependency. A QML `Canvas` drawing 3,000
rectangles: rejected, e-ink redraw cost; one image is one update.

**Cut line.** The QR code is its own task group inside US1. If it is dropped, US1 still works (text link), and
only the deviation note changes.

---

## R-6 Where the credentials live (Principle IV evaluation)

**Decision.** Keep the baseline's `credentials` file (plain `key=value`, atomic write, mode 0600 requested) for the
**app password only**. The address, login name and user id are not secrets; they go into the `account` table.

Evaluation of better-than-plaintext options, as the constitution requires:

| Option | Verdict |
|--------|---------|
| Plain file, 0600 | **Chosen.** On the reader's FAT storage the mode cannot be enforced (`docs/device-findings.md`: reads back 0777), and the app says so. |
| Obfuscation or encryption with a key stored beside the file or derived from a device id | Rejected: anyone who can read the file can read the key; it would imply protection that does not exist. |
| Ask for the password at every start | Rejected: unusable with the on-screen keyboard; the user would pick a weaker app password. |
| A protected location with real permissions | None found on the reader (device findings). Re-check in DT-NC-2. |
| Revoke on sign out (`DELETE /ocs/v2.php/core/apppassword`) | Deferred: it is a server call that changes account state. Sign out forgets the password locally and the text tells the user to remove the app password under **Security** in Nextcloud. |

The Settings and About text states: the app password is stored as plain text in a file in the app's data folder, the
reader's storage cannot restrict access to it, anyone who gets the storage can use the app password until it is revoked,
and revoking is done in Nextcloud. The measured protection (`credentials_protection`) is shown as "protected" or "not
protected on this storage". The account password is never typed on the reader in the login flow, and in the manual
form the user is told to enter an app password (FR-007).

**Unverified.** DT-NC-2 (mode, location, what the user sees).

---

## R-7 Listing, cache and entry availability (US2)

**Decision.**

- One `PROPFIND` depth 1 per folder open or refresh with the property list in
  [contracts/server-requests.md](contracts/server-requests.md). The first `d:response` (the folder itself) is dropped by
  comparing normalized paths, not by position. The response is parsed with `parse_xml` under the existing limits;
  the listing cap is **10,000 entries per folder** (response body cap stays 8 MiB). Larger: the first 10,000 are not
  guessed at; the folder shows `listing_too_large` with the count and nothing is cached for it.
- **Paths.** App-relative paths are decoded `d:href` with the `/remote.php/dav/files/<user-id>` prefix removed, with
  a leading `/`, no trailing `/`, root `/`. A `d:href` outside the prefix is dropped (counted, not shown). Names are
  stored exactly as sent (bytes), never normalized.
- **Atomic replacement.** A refresh replaces a folder's rows in one transaction only after the whole answer parsed;
  a failed refresh leaves the old rows and the old `fetched_at` (offline-first, FR-012).
- **Availability** of an entry is one of `ok`, `no_permission`, `encrypted`, `unsupported`, `unreadable`, with a short
  reason key:
  - `unreadable`: the response block has no 2xx `propstat` or lacks a required property;
  - `encrypted`: `nc:is-encrypted` is `1` (end-to-end encrypted folder);
  - `no_permission`: `oc:permissions` is present and lacks the readable letter `G` (api-notes 2.2, meaning inferred);
  - `unsupported`: `nc:mount-type` or share state the app does not handle (an unmounted external storage or a pending
    share); anything unknown but marked unavailable by the server.
  Such an entry is listed, marked "not available" with the reason, cannot be opened or downloaded, and never blocks
  the rest (FR-013). **All four detections are inferred** from the API notes: LV-N5 collects real answers; until then
  the fixtures carry `.unverified`.
- **Folder-gone.** 404 on a folder refresh: the cached folder stays readable, is marked "removed on the server", and
  the user is offered to go up (spec US2 scenario 9).
- **Sorting.** Name sort uses the existing `pbc_fold` (case-folded) with a byte-order tie-break; date and size sort in
  SQL. Folders first always. The pager runs a `LIMIT/OFFSET` query per page (page size from the QML height), so 1,000
  rows never sit in a QML model.
- **Bound.** Metadata cache limit: 50,000 entries and 2,000 folders, least recently opened folders evicted first; the
  folder being shown and the root are never evicted. Downloads, marks and local-change records are in other tables and
  are never touched by eviction (FR-032).

**Alternatives.** Keeping the whole tree: rejected by Principle V (visited folders only). ETag shortcut to skip
unchanged listings: not needed because refresh is explicit and Depth-1 only.

---

## R-8 Download engine (US3)

**Decision.** `Downloader` in `core/` runs one job at a time on the worker thread.

1. **Preflight (before any byte).** Size from the cached entry (`oc:size` or `getcontentlength`); unknown size is allowed
   but then free space is checked against the `Content-Length` of the response and the transfer is stopped if it does
   not fit. Checks, in this order:
   - size ≥ 4 GiB (4,294,967,296 bytes) → refuse `too_large_for_storage` (FAT limit is 4 GiB − 1; spec: "4 GiB or more");
   - free space on the target volume (`statvfs`) < size + margin → refuse `not_enough_space` with both numbers; margin
     is the larger of 8 MiB and 1 % of the file size;
   - size ≥ **100 MiB** (setting `large_file_warn_mib`, default 100) → ask for confirmation first;
   - the target volume is read-only or the directory cannot be created → `storage_readonly`.
   The platform seam is `StorageInfo` (R-13) so the tests can fake a full or read-only card.
2. **Target path.** The mirrored tree (clarification) under the download root, every component sanitized (R-9). The
   mapping remote path → local path is stored in `local_files` and reused on every later download of the same entry,
   so a rename of a colliding neighbour never moves an existing file.
3. **Temporary file.** `<target dir>/.pbc-<job id>.part`: same directory (so the final rename is atomic on the same
   file system, constitution III), a dot name, and an extension that is never a book type, so the library does not index
   it (**unverified**, DT-NC-3). The job is written to the `downloads` journal *before* the first request (path, expected
   ETag, expected size).
4. **Request.** `GET` with `If-None-Match` not used. The ETag recorded as `local_files.remote_etag` (and as the resume
   validator) is the one of the **response that delivered the bytes**, not the listing value: a file that changed
   between the details view and the download is recorded with the version actually received, and the next listing shows it
   as `downloaded` or `outdated` correctly. If the response has no ETag, the listing value and the size are used and the
   record is marked as weaker (`remote_etag` empty, size and mtime compared). Resume: `Range: bytes=<n>-` and `If-Range: <strong ETag>` when a
   partial file exists, its size matches the journal, and the stored ETag is strong (no `W/`). Responses:
   - `206` with `Content-Range: bytes n-…/total` and `n` equal to our size → append;
   - `200` → the server ignored the range or the ETag changed: truncate and restart (never append a `200` body);
   - `416` → delete the partial file and restart once;
   - anything else maps through `DavSession::status_error`.
   If the probe (DT-NC-1 / LV-N6) shows that Nextcloud ignores `Range`, resume degrades to restart; behaviour stays
   correct, only slower. **Unverified.**
5. **Verification.** After the last byte: size must equal the expected size (`Content-Length` of the first response or
   the listing size; both when both exist). If the listing carried `oc:checksums`, the app computes the digest of the
   file with one streaming pass and compares (algorithms present in the list: SHA-256, SHA-1, MD5; the strongest
   available is used; unknown algorithms are ignored). A mismatch deletes the partial file and reports
   `checksum_mismatch`; the final name is never created.
6. **Commit.** `rename(temp, final)`. If `final` exists: it is either a recorded unchanged local copy (replace
   allowed, atomic `rename` replaces it) or it is **not** ours / changed (R-11): the job stops with `local_changed` and
   the UI shows the conflict choices. `fsync` of the file before the rename; the directory is not synced (FAT).
   The new `local_files` row (size, mtime as seen by `stat` after the rename, ETag, remote mtime) is written in the
   same transaction that deletes the journal row.
7. **Connection drops.** The job state becomes `waiting`. While waiting the worker looks at the local Wi-Fi state
   every 5 seconds (no network traffic), and when it is up continues with a ranged request. The wait is bounded at 10
   minutes and 5 attempts (delays 5, 15, 45, 60, 60 s); then the job becomes `paused` and the journal keeps it, so a
   later tap on **Resume** (also after an app restart) continues it. This is the continuation of an action the user
   started, not background sync (Principle VI).
8. **Cancel.** The progress callback sees a cancel flag at least once a second; on cancel the partial file and the
   journal row are removed (spec: "does not appear in the library").
9. **Start-up cleanup.** At app start (no network) `.pbc-*.part` files without a journal row are deleted; journal rows
   whose partial file is gone are deleted; journal rows with a partial file are shown as **paused** downloads.
10. **Buffers and progress.** libcurl `CURLOPT_BUFFERSIZE` 64 KiB, no whole-file buffering; progress is reported to the UI
    at most once every 2 seconds or 5 % (coarse refresh budget), and the QML draws a text line with bytes, not a moving
    bar (Principle VI).
11. **Limits.** Largest supported file: 4 GiB − 1 byte (FAT). Largest file exercised by the budget tests: 1 GiB on the
    host, 500 MB on the device (SC-007).
12. **Timeouts.** Connect 15 s; no total timeout for a file transfer, but libcurl's low-speed guard (existing policy: less
    than 1 byte/s for 30 s) ends a stalled transfer, which then enters the waiting state.

**Rationale.** Each point is a direct reading of constitution III and spec FR-016 to FR-023, SC-006 to SC-009.

**Alternatives.** Partial file in a private directory then `rename` across directories: rejected, may be a copy on a
different volume and not atomic. Resumable downloads in the library with a `.part` suffix: same as chosen. Parallel
downloads: rejected, one radio, one SD card, one worker.

**Unverified.** `Range`/`If-Range` on Nextcloud (LV-N6), library behaviour for dot files (DT-NC-3), the speed of the
checksum pass on the device (DT-NC-4).

---

## R-9 Local names and the mirrored tree

**Decision.** `local_name.h` turns a server name into one path component:

1. Treat the name as UTF-8; invalid sequences become `_`. Normalization is **not** applied to the server side (the
   remote name stays byte-exact) but the local component is converted to NFC so the same visible name always gets
   the same local name (the baseline's `utf8.h` has the needed tables or the function is added there; see tasks).
2. Replace control characters (U+0000 to U+001F, U+007F), `\ / : * ? " < > |` with `_`. A name that is `.`, `..` or
   empty becomes `_`. Strip trailing dots and spaces. Names that are reserved device names on Windows-formatted cards
   (`CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9`, with or without extension) get a `_` prefix.
3. Limit: 120 UTF-8 bytes per component (FAT stores long names as UTF-16, 255 units; 120 bytes leaves room for the
   collision suffix and for the `.pbc-<id>.part` name), cut on a character boundary, the extension (up to 16 bytes)
   kept.
4. Collisions are decided **case-insensitively** by comparing the `pbc_fold` form of the component with every
   sibling already on disk and every sibling already recorded in `local_files` for that directory. A collision appends
   ` (2)`, ` (3)` … before the extension. The result is stored and reused (R-8 point 2).
5. The full local path is checked to stay below 4,000 bytes; a deeper tree is `path_too_long` and that file is skipped
   with a message (not truncated into a collision).
6. The app never calls `open`, `rename` or `unlink` on a path that did not come out of this function and was
   verified to be inside the download root (a canonical-path prefix check, with symbolic links not followed).

**Tests (Principle I).** Hostile table: `..`, `../x`, `a/b`, `a\b`, `CON.txt`, NUL and control characters, overlong
names (1 byte to 4,000 bytes), a name made of 300 combining marks, NFC versus NFD pairs, `A.txt` versus `a.TXT`,
trailing dots, names that equal the temp-file pattern `.pbc-1.part`, and a symbolic link planted at a folder level
(SC-016).

---

## R-10 Entry states and Update (US4)

**Decision.** The state of an entry is computed, never stored, from three facts (see
[data-model.md](data-model.md) for the table):

| Fact | Source |
|------|--------|
| the server's current ETag and size | the newest cached listing (`entries`) |
| the ETag and size the local copy came from | `local_files.remote_etag`, `remote_size` |
| the local copy as it is now | `stat(local_path)`: size and mtime against `local_files.size`, `local_files.mtime` |

States: `not_downloaded`, `downloaded`, `outdated` (server differs, local unchanged), `changed_on_reader` (local
differs, server same), `conflict` (both differ), `removed_on_server` (the last successful listing of the parent did
not contain it), `missing_locally` (the recorded file is gone; treated as `not_downloaded` for a download but
shown so, never silently re-downloaded outside Update of a kept item).

**Update** (user starts it; one worker job; cancellable):
1. Collect the kept items (`offline_marks`). Group files by parent folder; for kept folders walk all subfolders.
2. For each folder: one `PROPFIND` depth 1. The answer is used **transiently**: Update never creates a cache row
   (`folders`/`entries`) for a folder the user has not opened (Principle V: cached only for visited folders). If the folder
   is already cached, its rows are refreshed exactly like an explicit refresh; otherwise the listing is read, the
   decisions are made, and it is discarded. State is carried by `local_files` (`remote_etag`, `server_missing`), not by
   the cache. Walk bound: 500 folders and depth 20
   per Update; beyond that the report says how many folders were not checked. No ETag shortcut for subtrees, because
   the propagation of folder ETags is **unverified** (api-notes 2.1); an optimization can follow LV-N7.
3. For every file in a kept scope: `not_downloaded` and `outdated` are downloaded (new files in kept folders
   included); `changed_on_reader` and `conflict` are **not** touched and are listed in the report as needing a
   decision; `removed_on_server` is flagged; everything else is skipped.
4. The report counts: checked folders, checked files, updated, new, skipped, needs a decision, removed on server,
   failed. Every download is the normal `Downloader` job, including the preflight; a failed preflight on one file
   (`not_enough_space`) stops the rest of the batch (a general error) while a per-file error (`forbidden`,
   `not_found`) does not (the Android notes' general versus file-specific split).
5. **Idempotence.** A finished download updates `local_files` in the same transaction as the commit (R-8 point 6),
   so a re-run skips it. An interrupted Update leaves at most the journal row of the file in progress.
6. The batch confirmation: before the first download the app shows the totals (count, bytes, free space) when the
   total exceeds the large-file warning size or the free space; otherwise it starts.

**Conflict resolution** (a text page with sizes and dates, no thumbnails): *Keep my copy* (records the server's
current ETag in `local_files.seen_etag`, the local file is untouched, the state becomes `changed_on_reader` until the
server changes again), *Keep both* (the server version is
downloaded under a collision-safe name ` (server copy)`; both are recorded as separate `local_files` rows only for the
new one), *Replace my copy* (requires a second confirmation that names the size and modified time of the copy that
will be lost). No choice is made by a timer or default.

**Unverified.** That a size-and-mtime comparison is enough on FAT (2 s granularity): a change that keeps size and mtime
inside the same 2-second slot is not detected; accepted and written in the user-facing help (DT-NC-5 checks that
opening a book in the viewer does not change the file).

---

## R-11 Favorites and search (US5)

**Decision.**

- **Favorites.** `REPORT` on the user's root with `oc:filter-files` and `<oc:favorite>1</oc:favorite>`; the answer is
  parsed by the listing parser; success replaces the `favorites` table and sets `favorites_fetched_at`. Offline the
  stored list is shown with its age. Favorites may be folders; they open like folders. **Inferred body shape**
  (api-notes 5.7): LV-N8.
- **Offline search.** A name search over `entries`, `favorites` and `local_files` with the case-folded substring match
  (`pbc_fold(name) LIKE '%' || fold(term) || '%' ESCAPE '\'`). No FTS: 5,000 rows are scanned in well under
  50 ms, so `SQLITE_ENABLE_FTS5` and `test_fts5_available.cpp` are removed (baseline-leftovers decision). Results show
  their folder path and "from this reader".
- **Server search.** Only on the user's explicit tap ("Also search the server"), `SEARCH /remote.php/dav/` with
  `d:basicsearch`, scope `files/<user-id>`, `d:like` on `{DAV:}displayname` with `%term%`, `d:limit` 100, `d:orderby`
  last-modified descending. The term has `%`, `_` and `\` removed before use (the escape syntax of the server is not
  documented) and is XML-escaped. Results are held in memory only (not written to the folder cache, so no cache growth);
  tapping one fetches its parent listing if needed. Result count over 100 shows "more results exist, refine the
  search". **Inferred request shape** (api-notes 5.8): LV-N9.
- The search term is a user input and appears in no log (R-15).

---

## R-12 Network policy and the account check at start

**Decision.** (clarified: one account check only)

- **Start.** If `device.wifiConnected()` is already true and an account exists, the home page runs *one*
  `GET /ocs/v1.php/cloud/capabilities` and shows the result as the state line ("Connected as …, checked 14:32" or the
  problem). No listing, favorites or Update at start. If Wi-Fi is off, nothing is sent and the home page says
  "Offline, not checked".
- **Never turns Wi-Fi on by itself.** A user action that needs the network (Refresh, Download, Update, Test) first calls
  `device.connectNetwork()` as the sibling apps do, and only with the tap as the trigger.
- **Failure policy.** After two consecutive 401 answers the account state becomes `needs_signin` and no further request
  is sent until the user opens Settings and tests again (api-notes 9: brute-force protection). 429 and 503: one
  message with the wait time from `Retry-After` when present; no automatic retry. 500, 502, 504: no automatic retry
  except inside a running download (R-8 point 7).
- **No timers** other than the download wait (R-8) and the login-flow expiry display. A scripted check
  (`check-constitution.sh` rule N3) fails the build if `QTimer` or `std::this_thread::sleep_for` appears in `bridge/`
  or `core/` outside `download` code and `device_bridge`.

**Rationale.** Principle VI and the clarification; an account check is the smallest start action that answers "does
my login still work".

---

## R-13 Platform seams: free space, file-system limit, Wi-Fi, opening a file

**Decision.**

- `StorageInfo` (core interface): `free_bytes(path)`, `max_file_bytes(path)`, `is_writable(path)`. POSIX implementation
  with `statvfs`/`statfs`: the file-system magic `MSDOS_SUPER_MAGIC` (also used for exFAT/vfat reports) gives
  4 GiB − 1; others report "no known limit" (and the 4 GiB rule still applies on the device build because the card
  may be reformatted: the check uses `min(fs limit, 4 GiB − 1)` on the device target, the fs limit only on the host).
- **Opening.** `DeviceBridge::openFile` pattern from notes-pocketbook: canonical path must be inside the download root,
  regular file, then `OpenBook(path, "", 0)`; the reader chooses the viewer. The app never executes a downloaded file
  and offers no "open with" choice (Principle IV).
- **Library folder.** `/mnt/ext1/PBCloud/` (host: `PBC_DOWNLOAD_DIR`, default `./pbcloud-downloads`). It is a visible
  folder on the card so the library scans it; the data folder `/mnt/ext1/system/pbcloud/` is not. Whether the
  library lists the files at once or needs a rescan is **unverified** (DT-NC-3).
- Time zone for dates: see R-14.

---

## R-14 Dates and time zone

**Decision.** `d:getlastmodified` is an HTTP date in GMT and is stored as epoch seconds. The list shows the date
(`YYYY-MM-DD`), the details page shows date and time, both in the reader's zone. The reader's zone cannot be taken
from `TZ` or `/etc/localtime` (`docs/device-findings.md`), so the baseline's `tz.h` with bundled zone data stays. The zone
name is read from the reader's configuration (`/mnt/ext1/system/config/global.cfg`, key `timezone`, **unverified** key
and format, DT-NC-6); when no zone can be determined the dates are shown in UTC with the suffix "UTC". There is no
time zone page in this feature (Settings text: "Dates use the reader's time zone"). Decision from the baseline
leftovers: keep `tz.h`, `tzdata`, `civil.h`; drop nothing here.

**Alternative.** Always UTC: rejected, a book dated the day before is confusing; always device-local Qt: unreliable
here.

---

## R-15 Logging

**Decision.** Existing `pbc::log` with new event codes in `log_codes.h`: `account.test`, `listing.refresh`,
`download.start|done|cancel|fail|resume`, `update.run`, `search.server`, `cache.evict`, `space.remove`. Fields: codes,
HTTP status, counts, sizes, error keys, a job number. Never: server address, user or login name, remote or local
names and paths, search terms, share or login URLs, tokens (Principle IV). The existing name-based redaction and rules
R4/R6 stay; two new rules (R-16).

---

## R-16 New mechanical checks (Principle VIII)

Added to `scripts/check-constitution.sh`, each with a self-test sample that must be detected:

| Rule | Detects |
|------|---------|
| N1 | an `http://` URL literal outside `tests/`, `docs/`, `third_party/` and comments (plaintext endpoints). XML namespace URIs are not endpoints and are exempt: the constants `kNs*` in `core/include/pbc/dav_common.h`, and any `xmlns` attribute text in a request body (`http://owncloud.org/ns`, `http://nextcloud.org/ns`, `http://calendarserver.org/ns/`); the exempt list is an explicit, reviewed list in the script |
| N2 | an HTTP write method (`PUT`, `MKCOL`, `MOVE`, `COPY`, `DELETE`, `PROPPATCH`, `LOCK`, `UNLOCK`) as a request method in the code that builds requests: `core/src/nc_*.cpp`, `downloader.cpp`, `update.cpp`, `bridge/src` and `app/` (read-only client). The generic transport (`curl_transport.cpp`, `http_transport.cpp`, `dav_common.cpp`) must be able to carry any method and is excluded; a test (`test_http_transport_methods`) still covers it |
| N3 | `QTimer`, `QBasicTimer` or `sleep_for` in `bridge/src`, `core/src` outside `downloader.cpp`/`device_bridge` (no background activity) |
| N4 | a log call that passes a field named `name`, `href`, `term`, `url`, `user`, `login` (server data in logs) |
| N5 | `remove(`, `unlink(`, `rename(` or `std::filesystem::remove` in `core/src` outside `local_files.cpp`, `downloader.cpp`, `credentials_file.cpp`, `ca_selector.cpp` (its own temporary CA file), `curl_transport.cpp` (its own sink file) and `store*.cpp` (all file mutation goes through the audited modules) |

`check-ui-style.sh` gets no new rule; the new QML must use the existing components. `check-fixtures.sh` already
enforces anonymized fixtures; the new fixtures use `example.org`-class reserved names and the `.unverified` suffix.

---

## R-17 Test strategy (Principle I, IX)

**Decision.** Three layers; every test is written first and seen failing.

1. **Unit** (`tests/unit/`): path normalization and encoding, local-name sanitizer (hostile table), listing parser
   (with fixtures for each availability class), entry-state function, FAT limit and preflight arithmetic, QR encoder
   vectors, checksum parser, error-key mapping, `Retry-After` parsing, cache eviction order.
2. **Contract/integration** against `tests/support/fake_nextcloud_server.h`: a stateful in-memory Nextcloud implementing
   `HttpTransport` (status.php, OCS, PROPFIND depth 0/1 with a configurable tree, GET with `Range`/`If-Range`/ETag, REPORT,
   SEARCH, Login Flow v2) with failure injection (drop the connection after *n* bytes, 401, 403, 404, 429 with
   `Retry-After`, 503 maintenance, weak ETags, ETag change mid-download, a partial multistatus, hostile names, huge
   listings, redirect to another host). Every test file that uses it defines `PBC_FAKE_SERVER_TEST`
   (`check-fake-markers.sh`). A smaller set goes through the real `CurlTransport` against `tls_test_server.h` to prove
   `Range`, append, progress/cancel, redirect refusal and storage-full behaviour of libcurl itself.
3. **Bridge/QML** (`tests/bridge/`, QML self-test via `app_self_test`): list paging, state badges, the conflict
   dialog, texts in both languages.

Interruption tests use `kill_harness.h` (already in the baseline, now with its first user): kill at every step of a
download (after the journal row, mid-body, before verify, before rename, after rename) and assert the invariants of
SC-006: no `.part` or half file visible after restart cleanup, journal consistent, final file complete.

Budget tests (`tests/integration/test_budget_*.cpp`) check the numbers of the plan; they are marked **unverified until
measured on the device** and print their figures; the device values go to `docs/device-test-checklist.md`.

New fixtures live in `tests/fixtures/nextcloud/` with the `.unverified` suffix until a live run produces them
(`docs/live-server-checklist.md` LV-N*).

---

## R-18 Baseline clean-up decisions (from `docs/baseline-leftovers.md`)

| Item | Decision |
|------|----------|
| `sync_scheduler` | Remove (no auto sync, no consumer; the constitution's Auto Sync setting belongs to a feature with local changes) |
| `paging.h` / `paging.cpp` | Remove (message-reader arithmetic); keep `PageButtons.qml`, `ModelPager.qml` |
| `tz`, `tzdata`, `civil.h` | **Keep** (R-14) |
| `utf8.h`, `pbc_fold` | **Keep** (sort, collision test, search) and extend with NFC |
| FTS5 | Remove |
| `SyncState` | Keep; it records the last Update (outcome, counts) and the last account check |
| `EventRow`, `ContactRow`, `DateTimePicker`, `EmojiPicker` | Remove (dead calendar/contact components) |
| IMAP/SMTP entries in `curl_session` | Leave (not touched; out of scope) |
| `dav_common.h` CalDAV/CardDAV namespaces and samples | Leave the constants, add `oc`/`nc`; replace CalDAV samples in the tests that the new fixtures supersede |
| `INSTALL.md`, `README.md`, `CHANGELOG.md` | Write (needed by `package-device.sh` and the version-bump gate) |

These are tasks in the last phase before the version bump; none blocks a user story.

---

## R-19 Versions and schema

`VERSION` 0.0.1 becomes 0.1.0 (first feature, constitution Development Workflow). Schema 1 → 2 is one migration in
`store_migrations.cpp`; an older build refuses the newer database (`schema_too_new`, already implemented and
tested). Because it is the first real migration the test covers: fresh create, upgrade from a schema-1 file
containing settings and a `sync_state` row, refusal of schema 3, failed migration rolled back with the backup kept.

---

## Amendments made while building

- **R-9 (local names).** The names are not normalized to NFC: the file system compares names byte-wise except for case, so two names that differ only
  in normalization are two files and are both kept; `choose_local_path` resolves case-insensitive collisions only (a suffix before the extension).
- **R-8 (downloads).** The transport refuses a redirect to another host, port or scheme (`redirect_other_host`), a partial answer from `CURLE_PARTIAL_FILE`
  is `connection_lost`, and a 206 that does not start where the request asked is refused. A weak ETag is never an `If-Range` validator.
- **R-10 (Update).** A copy changed only on the reader is `skipped`; only a copy changed on both sides needs a decision. A cached folder is refreshed by an
  Update like an explicit refresh; a folder never opened gets no cache row.
- **R-17 (search).** The `SEARCH` body asks for 101 results so that "more exist" can be told; the app shows 100. Server results live in a temporary table of
  the database connection (never in the file) so that a tap on a result can show and download it.

