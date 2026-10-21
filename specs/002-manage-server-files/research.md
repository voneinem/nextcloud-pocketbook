# Research: Upload and Manage Files on the Server

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **Date**: 2026-10-20 | **Builds on**: [feature 001](../001-nextcloud-files-client/research.md)

Everything that depends on a real Nextcloud server or on the InkPad One is **unverified** until it has an entry in
`docs/live-server-checklist.md` (LV-W*) or `docs/device-test-checklist.md` (DT-W*) with a date (constitution IX). The server
facts come from `docs/reference/nextcloud-api-notes.md` (sections 5.3-5.6, 6, 8.1, 9), which is itself unverified. Each
decision names what it relies on and how a wrong assumption is contained.

Format per item: **Decision**, **Rationale**, **Alternatives considered**, **Unverified**.

There are no `NEEDS CLARIFICATION` items left in the plan: the spec's three clarifications are inputs (folder pick, no
Delete without trash, linking an uploaded local copy), and every other unknown below is a decision with a contained risk.

---

## R-1 Starting point: what feature 001 gives, what changes

**Decision.** Build on the code of feature 001 (version 0.1.0). Reuse unchanged: `DavSession`, `HttpTransport`/`CurlTransport`
(TLS rules, same-host redirect rule), `AuthGuard`, `FolderService` (listing fetch), the minimal XML reader, `remote_path`
(`encode_path`, `join_remote`, `parent_of`, `is_valid_name`), `hash` (SHA-256 and SHA-1), `Store` and its migration
mechanism, the `Worker` thread, `error_keys`, `age_text`, the QML component set, `fake_nextcloud_server.h`, the kill
harness, the host UI walk. Change only these seams, all additively:

| Seam | Change |
|------|--------|
| `HttpTransport::Request` (`http_transport.h`) | A request body can be a **range of a file**: `body_file`, `body_offset`, `body_length` (streamed with a 64 KiB read callback; the file is opened read-only without following links). Methods `PUT`, `MKCOL`, `MOVE`, `COPY`, `DELETE` are named in the comment of `method` (the transport already passes any method through `CURLOPT_CUSTOMREQUEST`). The unused multipart `upload_file`/`upload_field`/`upload_file_name` and its `curl_mime` code are **removed** (baseline leftover of the sibling apps; nothing in the product uses it). |
| `HttpTransport::Response` | New `delivery` (`not_sent` / `partly_sent` / `fully_sent`): how far the request got when there is no HTTP status. It is what separates "nothing happened, safe to try again on the user's tap" from "outcome unknown" (R-7). A `FakeTransport` that ignores it reports `not_sent` for connection errors. |
| `DavSession::status_error` (`dav_common.cpp`) | Adds the write-side statuses (R-12): 405, 409, 412, 413, 415, 507, 207-with-failures; the existing keys keep their meaning. |
| `nc_server.cpp` (`check_account`) | Also reads the capabilities the write side needs (R-9, R-10) and returns them in `AccountCheck`. |
| `Store` | Schema 3 -> 4 (data-model.md): `queue_items`, `server_caps`, and the settings `auto_sync`, `picker_view`. New typed methods in new `store_queue.cpp`. |
| `scripts/check-constitution.sh` | Rules N2 and N3 become allow-lists, one new rule N6 (R-18). Self-tests for all three. |
| `FolderPage.qml`, `HomePage.qml`, `SettingsPage.qml`, `AboutPage.qml` | Tools, an Auto Sync switch, text (R-16). |

**Rationale.** Constitution V (simplicity) and XI (same components). The write side is new logic in new modules; the
existing read side keeps its tests and meaning.

**Alternatives.** A second transport for uploads: rejected (two TLS paths double the review surface, as in 001 R-1).

---

## R-2 The write requests (server contract)

**Decision.** Only these requests are added; details, headers and mapping are in
[contracts/server-requests.md](contracts/server-requests.md).

| Operation | Request |
|-----------|---------|
| Small upload (file <= chunk size) | one `PUT` with `If-None-Match: *` (new) or `If-Match: <etag>` (replace) |
| Large upload | chunked upload v2: `MKCOL` of an upload folder, `PUT` per chunk, `PROPFIND` of the upload folder to resume, one final `MOVE`, `DELETE` of the upload folder on abort |
| New folder | `MKCOL` (fails with 405 if the name exists: that is the condition) |
| Rename / move | `MOVE` with `Destination`, `Overwrite: F`, `If-Match: <etag of the source>` |
| Copy | `COPY` with `Destination`, `Overwrite: F`, `If-Match: <etag of the source>` |
| Delete | `DELETE` with `If-Match: <etag>` |
| Existence / state check | `PROPFIND` `Depth: 0` (never `infinity`) |
| Quota before sending | `PROPFIND` `Depth: 0` on the home folder (existing request of 001 section 4) |

Never sent: `PROPPATCH`, `LOCK`, `UNLOCK`, a trash-bin request, a version request, `Depth: infinity`, and `Overwrite: T` (**never**: a user-confirmed replace of a file is a conditional `DELETE` of the target into the trash,
then the request with `Overwrite: F`; R-3, R-8; decision of the implementation, see "Implementation notes" at the end).

**Rationale.** These are exactly the operations the spec lists; each is the smallest request that does the job
(api-notes 5.3-5.6, 6). Trash is reached only implicitly: a `DELETE` on a server with the trash app moves the item into it.

**Alternatives.** Bulk upload: rejected (api-notes 5.9, not needed; several `PUT`s are simpler and each has its own condition).
`X-NC-WebDAV-Auto-Mkcol` (creates missing parents, NC 32+): rejected; the queue creates the parent folders itself as items
with dependencies (R-6), so each creation is user-visible and conditional.

**Unverified.** Every row: LV-W1 to LV-W12 (R-20).

---

## R-3 Conditions on every write (constitution III)

**Decision.** A table, enforced by a contract test that fails when a write request lacks its condition
(`test_write_conditions`, run against every operation in the fake server):

| Operation | Condition sent | Meaning of 412 |
|-----------|----------------|----------------|
| `PUT` new file | `If-None-Match: *` | the name exists |
| `PUT` replace | `If-Match: <etag the reader last saw>` | the server version changed (or the file is gone: 404/412) |
| `MKCOL` | none needed: creation of an existing name is refused by the protocol (405) | n/a |
| `MOVE` / `COPY` | `Overwrite: F` and `If-Match: <etag of the source>` | **ambiguous**: the target exists, or the source changed |
| `DELETE` | `If-Match: <etag>` | the entry changed |
| chunked upload final `MOVE` | always `Overwrite: F`, **preceded by** the pre-check below; a replace first deletes the old version (next row) | see below |
| replace of a file (upload, rename, `MOVE`, `COPY`; user-confirmed, target is a **file**) | `DELETE <target>` with `If-Match: <the etag the user confirmed>` (into the trash; refused where the server has no trash), **then** the request with `Overwrite: F`. A small `PUT` replace is `If-Match` on the `PUT` itself | see below |
| chunk `PUT`, upload-folder `MKCOL`/`DELETE` | none: the app's own temporary upload session, not user data (R-4) | n/a |

A **weak** ETag (`W/...`) or an **empty** ETag never satisfies `If-Match`: such an item cannot be queued as replace, rename,
move, copy or delete; the user is told to refresh the folder first (api-notes 2.1). 412 on `MOVE`/`COPY` is **disambiguated**
by two `PROPFIND` `Depth: 0` (target, source) before anything is shown: target exists -> collision (name question);
else source changed or gone -> conflict `changed_on_server` / `source_gone`.

**Replace by `MOVE`/`COPY` (as implemented).** `Overwrite: T` cannot carry a condition on the target, so it is never used. The sender reads the target's ETag with a `PROPFIND` `Depth: 0`; if it differs from the one the user confirmed -> conflict `changed_on_server`; a missing target means nothing is left to overwrite. Otherwise it deletes the target with `If-Match` (the old version goes to the trash, so a replace needs a trash: without one the item is held `trash_unavailable`), clears `dst_etag` and sends the request with `Overwrite: F`. A change between the pre-check and the `DELETE` is caught by the `If-Match` of the `DELETE`; a death between the two requests leaves the item `unknown`, the old version in the trash and a resend completing the replace. **A folder is never replaced**: Replace is not offered when the existing entry or the item is a folder, or when a file would replace a folder or the reverse; only Keep both and Skip are.

**Final `MOVE` of a chunked upload.** Which conditions the server honors there is not documented (api-notes 6). So the
sender (a) sends them anyway, and (b) before the final `MOVE` runs a `PROPFIND` `Depth: 0` on the target: new file and the
name now exists -> collision; replace and the ETag differs from the one the reader last saw -> conflict. This leaves a
race of a few seconds between the check and the `MOVE` **only if the server ignores the headers**. That residual is stated
in the Constitution Check and in Complexity Tracking and is closed by LV-W6 (verify which headers the server honors); if
LV-W6 shows they are ignored, the residual is documented in README and the final `MOVE` for replace is limited to files
the app can re-check (a version-history copy on the server remains the user's recourse; version screens are out of scope).

**Rationale.** Principle III names `If-Match` and `If-None-Match: *`; RFC 4918 gives `Overwrite: F` for
`MOVE`/`COPY`; the combination turns an unconditional overwrite into a refused one on every path the server honors.

**Alternatives.** Check-then-write with a `PROPFIND` for every operation instead of headers: rejected as the primary
mechanism (a race the headers close); kept as the fallback for the one place the headers are not documented.

**Unverified.** LV-W2 (`If-Match` on `PUT`/`DELETE`), LV-W3 (`MOVE`/`COPY` with `Overwrite: F`), LV-W6 (final chunk `MOVE`).

---

## R-4 Chunked upload

**Decision.**

1. **Threshold and size.** A file of at most **10 MiB** is one `PUT`. A larger file is sent in chunks of **10 MiB**
   (the last may be smaller), or the server's `files.chunked_upload.max_size` if that is smaller (never below 5 MiB, the
   protocol minimum). At most 10,000 chunks: with 10 MiB chunks the largest file would be 97 GiB, far above the 4 GiB-1
   limit of FAT, so the limit never binds. If the server does not state `dav.chunking`, a large file is sent as **one
   `PUT`** (no resume; a failed one restarts) and the queue says so once.
2. **Names and ids.** Upload folder `/remote.php/dav/uploads/<uid>/pbc-<random 128-bit hex>`; the id is created when the item
   starts and stored in the row **before** the first request. Chunk names are zero-padded to five digits (`00001` ...
   `10000`), because the server assembles in name order and unpadded `10` would sort before `2`.
3. **Headers.** `Destination: <final URL>` on the upload-folder `MKCOL`, every chunk `PUT` and the final `MOVE`;
   `OC-Total-Length: <bytes>` on chunk `PUT`s and the final `MOVE` (the server checks the quota at the first chunk: 507 early);
   `X-OC-Mtime: <source mtime in seconds>` on the final `MOVE` (and on the small `PUT`: `X-OC-MTime`; the accepted marker is
   read, a missing one is not an error); `OC-Checksum: SHA256:<hex>` (R-5).
4. **Resume.** A started item keeps `upload_id`, `chunk_size`, `total_size` and the source identity in its row. On the next
   send: `PROPFIND` `Depth: 1` on the upload folder lists the chunks the server holds; a chunk is accepted only if its name is
   in range and its size equals the expected size; every other chunk is sent again. If the upload folder is gone (expired
   after 24 hours of inactivity, per the manual) a new id is used and the item starts over; the user is told.
5. **Final step.** Pre-check (R-3), then the final `MOVE`. Then a `PROPFIND` `Depth: 0` on the target: its size must equal
   `total_size` (and its checksum, if the server reports `oc:checksums`, must equal ours) -> done, with ETag and file id
   read from there. A size mismatch -> failed `size_mismatch`, nothing is deleted on the server automatically.
6. **Source changed.** The identity (size and modification time in seconds) is checked when the item starts, before every
   chunk, and before the final `MOVE`. A change -> `DELETE` of the upload folder (the app's own folder), a new id, and the
   item starts over from the current content; the user is told once (spec FR-015).
7. **Cancel / abandon.** Cancel stops after the chunk in flight and keeps the item resumable; **Remove** of an item that has an
   upload folder first sends `DELETE` of that folder (best effort; the 24-hour expiry is the backstop), then removes the row.
   Nothing incomplete is ever visible under the user's files, because chunks live only in `/dav/uploads/`.
8. **Streaming.** Each chunk is one `PUT` with a body range of the source file, read through the transport's 64 KiB callback.
   No chunk is held in memory (constitution V). One chunk in flight (api-notes 6: one radio, one card).
9. **Timeouts.** Connect 15 s; no total timeout for a chunk `PUT`; libcurl's low-speed guard of the existing policy (less
   than 1 byte/s for 30 s) ends a stalled transfer.

**Rationale.** It follows the manual's v2 protocol, makes the interruption cost at most one chunk (FR-014), and keeps
unfinished data out of the user's folders (FR-016).

**Alternatives.** TUS or a custom resumable protocol: rejected (not offered by Nextcloud's DAV). `Content-Range` PUT:
rejected (not supported). Parallel chunks: rejected (api-notes 6; one worker, one radio). 5 MiB chunks: more requests for
no gain on Wi-Fi; 10 MiB is the figure the notes name as the fallback of other clients.

**Unverified.** LV-W5 (v2 protocol end to end), LV-W6 (final `MOVE` conditions), LV-W7 (upload-folder listing for resume),
DT-W3 (chunk size vs. memory and time on the device).

---

## R-5 Source identity, checksum and modification time

**Decision.** When an item starts, the app opens the source file read-only with no link following, takes `fstat` (must be a
regular file, size 1 byte to 4 GiB-1) and records **size and mtime in seconds** in the row. It then computes **SHA-256**
once in a streaming pass (64 KiB, cancellable, progress "Preparing", coarse) and stores the hex in the row, so a resume
does not hash again. The hash is sent as `OC-Checksum: SHA256:<hex>` and compared with `oc:checksums` after assembly if the
server reports it. `X-OC-MTime` carries the source mtime so the server shows the reader's date (api-notes 5.3; the accepted
marker is not required).

**Rationale.** The pass is the only way to put the checksum into a header before the body (api-notes says "read the file
once"; a header cannot follow the body). It gives the check "did my upload arrive" a stronger answer than the size alone
(R-7) and lets 001's download verify the file later. The cost is one sequential read of the file; DT-W4 measures it.

**Alternatives.** No checksum: rejected (the outcome check would rest on the size only). Hash during the upload: impossible
for a header. SHA-1: accepted by the server too, but 001 already prefers the strongest.

**Unverified.** DT-W4 (speed of the pass on the card), LV-W8 (`OC-Checksum` accepted and reported back).

---

## R-6 The queue: model, order, dependencies, persistence

**Decision.** A persistent table `queue_items` (data-model.md), one row per **atomic change**. A folder pick (FR-003) becomes
a **group** of rows (`group_id`): one `mkdir` per folder, one `upload` per file, each upload depending on the `mkdir` of its
folder (`depends_on`), created parents first. Rules:

1. **Persisted before any network request** (constitution III): the row is committed in one transaction (for a group: all
   rows or none) before the bridge tells the UI or arms Auto Sync.
2. **Bound:** 2,000 items. A pick that would exceed it is refused as a whole (`queue_full`); existing items are never dropped.
   Check: a folder pick counts folders and files first and shows both numbers for confirmation.
3. **Order:** by id (creation order). An item whose `depends_on` item is not `done` waits; if that item is `failed`,
   `conflict` or `unknown`, the dependent shows "waiting for <the other item>" and is not tried. Independent items continue.
4. **States:** `queued` ("not synced"), `sending`, `done` (kept for the "sent" line until the queue screen is left, then
   removed), `failed` (reason key), `conflict` (reason key + the user's open choice), `unknown` ("outcome unknown").
5. **Recovery at start (no network):** a row left in `sending` becomes `queued` if its step is idempotent (preparing, chunk
   transfer: the server listing is the truth) and `unknown` if its step could have changed the server (single `PUT`, final
   `MOVE`, `MKCOL`, `MOVE`, `COPY`, `DELETE`). Nothing is sent by recovery.
6. **No automatic retry, anywhere.** A `failed` item is retried only by the user's tap on Retry or Send. The sender never
   sleeps or loops; it stops the run at the first sign that the connection is gone or the server says 429 or 503 and tells
   the user (FR-013). (Unlike a download, an upload has no bounded wait: a download continues one request the user started;
   the queue may hold hundreds of requests and spec FR-013 forbids retrying by itself.)
7. **Account binding:** each row stores the server address and user id it was queued for; the sender skips (and the queue
   screen flags `other_account`) rows that do not match the current account (FR-039). Signing out keeps the queue.
8. **Listing overlay (FR-008):** the folder listing is built from the cached `entries` plus the open queue rows (data-model.md
   "Listing overlay"): new folders and uploads appear as pending rows; a rename shows its new name; a move shows the entry
   in the source as "moving" and in the target as pending; a delete keeps the entry with a pending mark. When the server
   confirms an item, the same transaction that marks it `done` patches the cached rows (R-10) without changing the folder's
   `fetched_at`, so the age of the listing stays honest.

**Rationale.** Principle III (persisted, never discarded, idempotent, uncertain kept) and spec FR-006 to FR-010, FR-019.

**Alternatives.** One row per group with a JSON payload: rejected (partial progress, retry and dependency per file need rows).
An in-memory queue with a journal: rejected (two sources of truth).

---

## R-7 Unknown outcome

**Decision.** For a write that changes the server in one request (single `PUT`, final `MOVE`, `MKCOL`, `MOVE`, `COPY`,
`DELETE`), the outcome is **unknown** when the answer did not arrive after the request may have reached the server:

| What the sender sees | Result |
|----------------------|--------|
| No connection could be made, TLS handshake failed, `delivery = not_sent` | `queued` again, shown as "failed: no connection" until the user taps Retry; **not** unknown |
| `delivery = partly_sent` or `fully_sent` and no HTTP status (timeout, reset) | `unknown` |
| HTTP 500, 502, 504 for a write | `unknown` (a proxy may have forwarded it) |
| HTTP 503, 429 | `failed` (`maintenance`, `rate_limited`): the server states it did not execute it |
| Any other HTTP status | mapped by R-12 |
| Chunk `PUT` with no answer | **not** unknown: the next send lists the upload folder (R-4) |
| App killed / power lost while `sending` (R-6.5) | `unknown` for non-idempotent steps |

An `unknown` item is skipped by every Send and Auto Sync run. The user gets three choices (FR-020):

1. **Check the server** — read-only: `PROPFIND` `Depth: 0` on the target (and the source for a move) and compare:
   upload: target exists with size = ours (and checksum = ours when reported) -> "most likely arrived"; target missing ->
   "not there"; target exists with another size -> "a different file is there". mkdir: folder exists. rename/move: target
   exists with the **file id** of the source (the id is stored from the listing and survives a move) and the source path is
   gone. copy: target exists (a copy has a new id, so only existence and size can be compared). delete: the path is gone.
   The check reports; **the user decides** (Mark as done / Send again / Remove). "Mark as done" runs the confirmation effects
   of R-10 as if the server had answered.
2. **Send again as a new item** — the row becomes `queued` with its conditions unchanged (so an upload that did arrive is
   refused with 412 and ends in the collision question, not in a duplicate).
3. **Remove** — drops the row (for an upload with an upload folder: its `DELETE`).

**Rationale.** Constitution III: "an upload whose outcome is unknown MUST be kept and marked uncertain, and MUST NOT be
repeated silently". Sending again keeps its conditions, so even a user's mistaken Send again cannot overwrite or duplicate.

**Alternatives.** Retry idempotently on its own (e.g. `PUT` with `If-None-Match: *` again): rejected by the spec ("never
repeated silently"). Treat every failure as unknown: rejected (the user would be asked about harmless "no connection" cases).

**Unverified.** `delivery` accuracy of libcurl on the device (DT-W6, tested with a cut Wi-Fi), the 5xx cases (LV-W11).

---

## R-8 Collisions, conflicts and the keep-both name

**Decision.**

1. **At queue time** the app already knows the cached listing of the target folder: a name that exists there (or among open
   queue rows for the same target) is asked at once: **Keep both**, **Replace** (files only, R-3), **Skip** (FR-021). Replace needs a strong,
   non-empty ETag of the existing entry (else: "Refresh this folder first"); it is confirmed with a dialog that names the entry
   and says the server's file will be replaced. A folder name that exists is never replaced: for a **folder pick** the existing
   server folder is used (merged), and the question applies to each file inside (with **Apply to all remaining** for the
   pick); for **New folder** the existing folder is shown and nothing is queued.
2. **At send time** the condition does the check (R-3): 412 on a creation -> `conflict` with reason `name_exists`; 412 on a
   replace/delete -> `changed_on_server`; the listing the user saw may have been old.
3. **Keep both** names: `name (2).ext`, `name (3).ext`, ... (the number goes before the last extension; a dotfile or a name
   without extension gets the number at the end), checked against the cached listing, open queue rows for the same
   target and the forbidden-name rules (R-11); the user may edit the name. At send time a 412 for the new name asks again
   (a new number is proposed).
4. **Conflict choices** (FR-023) are offered on the queue screen for the item: for an upload — Keep both (new name),
   Replace anyway (second confirmation: "The file on the server has changed since you last saw it. Replace it?"),
   Skip (remove the item); for rename/move/copy — Apply anyway (confirmation: "The entry has changed on the server") or
   Discard; for delete — Keep the entry (discard) or Delete anyway (confirmation). The server version is never touched
   before the choice, the reader's file never leaves its place.
5. **Replace anyway** refreshes the ETag of the target with a `PROPFIND` `Depth: 0` **at the moment of the choice**, shows the
   newer size/date, and queues the replace conditional on that new ETag: the confirmation is about a version the user saw.

**Rationale.** FR-021 to FR-025 and constitution III ("both versions kept, the user decides").

**Alternatives.** Automatic rename on collision: rejected (spec: ask). A per-file question for a 500-file folder pick without
"apply to all": rejected (unusable on e-ink).

---

## R-9 Delete, trash and servers without trash

**Decision.** Delete is offered only if the server reported `files.undelete` (trash app) in the last successful account
check (stored in `server_caps.trash`). **Unknown counts as not available** (conservative; the spec forbids permanent
deletion). Where it is not offered, the action's dialog says: "This server has no trash. Deleting here would be permanent,
so PB Cloud does not offer it. Delete in the web interface if you are sure." (FR-033). The confirmation (FR-030) names the
entry and says "It goes to the server's trash." plus, for a folder, "Everything inside goes with it." (and the count of
queued items inside, FR-035). A `DELETE` answering 404 counts as done ("already gone", api-notes 5.5); 207 with member
failures -> `failed` with `partial_failure` and the advice to refresh the folder; the cached folder is **not** patched in
that case. The trash bin itself is never read or changed (out of scope).

**Rationale.** Spec clarification 2, FR-030 to FR-035, constitution III.

**Alternatives.** Allow with a stronger warning: rejected by the owner (clarification 2).

**Unverified.** LV-W9 (`files.undelete` present on a normal server; a `DELETE` really lands in the trash).

---

## R-10 Effects of a confirmed change on the reader's own records

**Decision.** When the server confirms an item (or the user marks an unknown item done), **one transaction** sets the item
`done` and applies the effects:

| Item | Cache (`entries`/`folders`) | Local copy / mark (feature 001) |
|------|-----------------------------|----------------------------------|
| upload new | insert the file row (size, mtime, ETag, file id from the answer or the `PROPFIND`) | none (the source is not a downloaded copy) |
| upload replace | replace the row | **link**: if the source's path is the recorded local path of a `local_files` row for the **same remote path**, **and** the source identity (size, mtime) is unchanged since the item started, the row gets the new ETag, size, mtime (state `downloaded`); in every other case nothing changes (clarification 3, FR-029) |
| mkdir | insert the folder row | none |
| rename / move | delete the old row, insert the new | **follow**: `local_files.remote_path`, `offline_marks.remote_path` of the entry (and, for a folder, every path below it, by prefix) are rewritten; the local file on the card is **not** moved or renamed (it keeps its safe local name) |
| copy | insert the copy's row | none (a copy creates no local copy and no mark) |
| delete | delete the row (and rows below) | **none**: the entry is shown as `removed on the server` by the 001 rules; a local copy with unconfirmed changes is never touched |

The listing's `fetched_at` is not changed by any of this. If a patch cannot be applied (the folder is not cached) nothing is
written; the next visit fetches.

**Rationale.** FR-008 (the "not synced" mark goes away on confirmation), FR-029, FR-034 and clarification 3, all in the
same transaction so a kill cannot leave the mark and the queue disagreeing (constitution III).

**Alternatives.** Re-fetch the listing after every item: rejected (a request per item, and the age of the listing would lie
if the fetch failed). Move the local file on rename: rejected (a rename of the local file is a second data-moving step with its own failure modes; the app
folder keeps its safe names, and the mapping is in the database).

---

## R-11 Names: forbidden names, rules, source names

**Decision.** A new name (New folder, Rename, Keep both, an uploaded file's name) passes `is_valid_name` (001: not empty,
not `.`/`..`, no `/`, no control characters, at most 255 bytes for the segment, no `\`, no trailing space or dot) **plus** the
server's own lists when known: `server_caps` holds `forbidden_chars`, `forbidden_names`, `forbidden_basenames`,
`forbidden_extensions` (from the capabilities, bounded to 64 entries of 32 bytes each; JSON paths unverified, LV-W10; a
missing list means "no list", never "nothing is forbidden"). A name refused here is refused at once with the reason
(FR-004, FR-027). A name the server refuses later is the `name_forbidden` failure with an Edit action. Names are compared
byte-exactly and case-sensitively (a case-only rename is a different name). No Unicode normalization.

A file name taken from the reader's storage is used as the server name unchanged after the check; if it fails the check the
pick shows it as "name not accepted" and offers to rename it before queuing.

**Rationale.** Spec FR-004/027 and constitution IV (names are untrusted in both directions).

**Alternatives.** Silently rewrite forbidden characters: rejected (a silent change of a user's file name).

---

## R-12 Mapping of write answers to keys (FR-036, FR-037)

**Decision.** Primary mapping by status, refined by the **whitelisted** sabre exception class in the body when present
(the class name only; never the server's message, never displayed or logged):

| Status | Key | Notes |
|--------|-----|-------|
| 201, 204, 200 | done | per operation |
| 507 | `server_storage_full` | message: storage full, with free space when known (quota) |
| 413 | `file_too_large_server` | the server or a proxy limit; for a small `PUT` the next try may use chunks if `dav.chunking` exists |
| 415 | `type_not_accepted` | |
| 403 | `name_forbidden` if the exception class is `InvalidPath`/`ForbiddenFileName` family; else `not_allowed` | permission or policy |
| 400 | `name_forbidden` for an `InvalidPath` class, else `protocol_error` | |
| 423 | `locked` | existing key; never retried by itself |
| 412 | `changed_on_server` or `name_exists` | R-3, R-8 |
| 405 on `MKCOL` | `name_exists` | |
| 409 | `target_gone` (the parent folder is missing) | |
| 404 | `source_gone` (the source or its parent); `DELETE` -> done | |
| 207 | `partial_failure` | |
| 401 / 429 / 503 / 5xx / transport | existing keys; 500/502/504 for a write -> unknown (R-7) | |
| anything else | `server_refused` with the numeric status as a short code | the generic message of the spec |

New keys (added to `error_keys`, one English message and one "what to do" line each, with a German translation):
`server_storage_full`, `file_too_large_server`, `type_not_accepted`, `name_forbidden`, `not_allowed`, `changed_on_server`,
`name_exists`, `target_gone`, `source_gone`, `partial_failure`, `server_refused`, `size_mismatch` (exists), `queue_full`,
`source_unreadable`, `source_changed`, `source_not_allowed`, `trash_unavailable`, `outcome_unknown`, `other_account`.

**Rationale.** Constitution IV (no unsanitized server text); spec FR-036/040.

**Unverified.** LV-W11 (which status each refusal really returns on a stock server and behind common proxies).

---

## R-13 The reader-side file browser (FR-001 to FR-005)

**Decision.** A `PickerBridge` and `FilePickerPage.qml`, adapted from PB Notes' attach browser (spec 009 of notes-pocketbook,
MIT, same author; attribution in `NOTICE`). Same look: top bar with Cancel, Up and the layout toggle (grid/list icon),
path line, big rows or a grid of tiles, whole rows/tiles per page, `PageButtons`, 400 ms tap guard, layout remembered in the
setting `picker_view`. Differences, all required by the spec:

- **Multi-select:** a tap on a file toggles its mark (a check icon in the row/tile) instead of asking; a tap on a folder opens
  it. A bottom bar shows "<n> selected" with **Upload** and **Clear**. Picks stay while the user changes folders and layouts.
- **Pick a folder:** inside a folder (not at the top level) the tool **Add this folder** adds it to the selection (shown with
  a folder-check mark, "<folders> folders, <files> files, <size>" computed on demand by a bounded walk: at most 2,000 items and
  depth 12, links not followed, hidden entries skipped as in the sibling browser). Over the bound the walk stops and the pick is refused.
- **Top level:** the sibling's curated list (Audio Books, Downloads, Music, My pictures, Notes, Photo, screens) **plus the
  app's own download folder `PBCloud`**, so that a downloaded and edited copy can be uploaded back (clarification 3). The list
  is `PickerBridge::Config::topFolders`, set in `main.cpp`. **Owner point 1** (plan.md).
- **No previews** in the tiles (a folder/file glyph and the extension only): previews were a separate story in the sibling
  and are not in this spec (YAGNI, constitution V).
- **Safety:** entries that are links, not regular files/folders, or unreadable are not listed. The canonical path of every
  pick must lie under the reader's storage root (`/mnt/ext1`; host: `PBC_PICKER_ROOT`) and **not** under the app's data
  folder (`system/pbcloud`: database, credentials, log); a pick that fails is refused with `source_not_allowed`. The same
  checks run again when the item is sent (a file may be replaced by a link in between) with `O_NOFOLLOW` and `fstat`.
- **Sizes:** files of 0 bytes or 4 GiB or more are refused at the pick (FR-004).

**Rationale.** Constitution XI (same look and behavior) and the spec; Principle IV for the source safety (an upload reads
from a path the user chose; it must not be possible to upload the app's own secrets by a crafted link).

**Alternatives.** The device's system file dialog: not available to apps here (sibling finding). A QML-only browser: rejected,
the checks belong in testable C++ (host tests).

**Unverified.** The folder names on the device (DT-W8 lists the real top level).

---

## R-14 Auto Sync

**Decision.**

- Setting `auto_sync` in `settings`, **default on** (spec, owner's decision), a pill switch in Settings with the label
  "Auto Sync" (the sibling wording and place). Takes effect at once; the choice persists.
- `core/auto_sync.h`: a Qt-free `AutoSyncScheduler` with time passed in (pattern of PB Notes' `SyncScheduler`, MIT, attribution
  in `NOTICE`; the sibling's gap rule is dropped: a single quiet period suffices here). `note_action(now)` arms it;
  `next(now, inputs)` returns `Idle`, `Wait(ms)` or `Start` using these rules, in order: not armed -> Idle; switch off or no
  account -> disarm, Idle; a send is running -> Idle (armed; asked again when it ends: the single follow-up); nothing
  queued that can be sent -> disarm, Idle; quiet period of **10 s** since the last action not over -> Wait; Wi-Fi not
  connected -> disarm, Idle (the queue waits, FR-013); else Start. Constant overridable by `QueueBridge::Config` so bridge
  tests run in milliseconds.
- `QueueBridge` owns **one single-shot `QTimer`** armed from the scheduler's `Wait`; this is the only timer added. The
  mechanical rule N3 gets one allow-list entry for exactly this file (R-18).
- **User's own action** means: queueing an upload/folder/rename/move/copy/delete, removing an item, a conflict choice.
  Nothing else arms it: not app start, not Wi-Fi coming up, not a failed item, not a finished send.
- **Wi-Fi:** Auto Sync never switches Wi-Fi on (spec assumption, scenario 3 of Story 2). The explicit **Send** tap does
  what the other network taps of 001 do (ensure network, then send). **Owner point 2:** PB Notes changed this after a device
  test (its Auto Sync now asks the system to connect); this plan follows the spec as written.

**Rationale.** Principle VI allows the Auto Sync trigger and requires its default to be stated: **on**. It reacts to the
user's own local change only, is switchable, and has no retry timer, no polling and no start-up trigger.

**Alternatives.** Debounce with a timer in QML: rejected (untestable on the host). A gap rule as in the sibling: dropped
(the queue sends everything pending in one run; two runs 6 seconds apart cost nothing the user can see).

---

## R-15 The sender (the run)

**Decision.** `QueueSender` in `core/` runs on the one `Worker` thread, one run at a time, started by Send, by Auto Sync, or
by Retry of a single item.

1. Preconditions: an account and its app password; `AuthGuard` not blocking; the run is refused (`other_account`) for
   rows of another account.
2. **Quota check** at the start of a run: one `PROPFIND` `Depth: 0` on the home (001's request) -> `free`. The run sums the
   sizes of the not-yet-sent uploads; items are sent in order and an item whose size exceeds the remaining free space is
   marked `server_storage_full` **before** any byte is sent (FR-038); the others go on. A wrong guess (the server says 507
   anyway) is reported as the server's refusal.
3. Take the oldest sendable item (state `queued`, dependency done, same account); set `sending` (committed) and its step; do
   the work (R-4, R-2); write the outcome (R-7, R-10) in one transaction; report progress at most every 2 s or 5 % (coarse
   refresh budget); look at `Cancel` between chunks and at least once per second inside a transfer.
4. **Stop the run** (not the item) on: 401 (guard), 429 or 503 (key and wait time from `Retry-After` shown, no wait), no
   connection (`delivery = not_sent`), cancel. The remaining items stay `queued`.
5. **Continue the run** after an item-level failure (`failed`, `conflict`, `unknown`): independent items still go.
6. Returns a report: counts of done, failed, conflict, unknown, left; the queue screen shows it; nothing else is logged
   beyond codes and counts.
7. Rate limiting: after a 429 no request is sent until the user taps again.

**Rationale.** FR-010 to FR-018, FR-037/038, constitution VI (no background activity).

**Alternatives.** A parallel sender: rejected (one radio, one card, one worker, and ordering/dependencies).

---

## R-16 UI structure (constitution VI, XI)

**Decision.** Only existing components (`TopBar`, `BottomPanel`, `ToolButton`, `IconButton`, `InkRow`, `PageButtons`, dialogs
with footer buttons, `InkSwitch`, `ChoiceDialog`, `TextEntryDialog`, `ConfirmDialog`, `Style`); a tile grid in the picker
as in the sibling. New pages: `FilePickerPage`, `QueuePage`, `FolderChooserPage` (a folder-only variant of the browse list for
the target of upload, move and copy; offline it shows stored listings only).

- **FolderPage:** the bottom panel has seven cells at most. Normal mode: **Refresh**, **Keep offline** (as before), **Add**
  (opens a choice: *Upload files*, *New folder*), **Select**, **Sort**, **Queue** (shows the number of open items), and the
  state words and a "not synced" mark on rows. **Update, Favorites and Search leave this page's panel** (they stay on the
  Home page, where they already exist) to make room: a visible change of feature 001, flagged as **owner point 3**.
  Select mode: rows get check marks, the panel shows **Rename** (one entry), **Move**, **Copy**, **Delete**, **Done**.
- **QueuePage:** list of items (paged), each with a kind icon, name, target, state word; tools: **Send**, **Cancel send**,
  **Remove**, **Retry**, **Details/Resolve** for a held item; the line "Offline" and the last send time; the report of the
  last run. Reachable from FolderPage (**Queue**), from Home (a "Queue" tool with the count) and from the notice after a pick.
- **SettingsPage:** an *Auto Sync* switch row (same wording and place as the sibling apps), and the line "Sending: Auto Sync is
  on/off" in Home's state line.
- **AboutPage / README / INSTALL:** a sentence that the app now writes to the server only when told, on the user's action.
- **Texts:** English source in `qsTr`, German in `qml/strings_de.tsv`, checked by `scripts/extract-strings.py --check`. Every
  failure key of R-12 has a message and an action line; the delete confirmation and the replace confirmation are fixed
  texts (data-model of texts in contracts/bridge-qml.md).

**Rationale.** Spec FR-043; the sibling apps' look; the panel is equal-width cells, so fewer tools means larger targets.

---

## R-17 Logging

**Decision.** New codes in `log_codes.h`, used as they appear: `queue.add`, `queue.remove`, `queue.send`, `upload.start`,
`upload.done`, `upload.fail`, `upload.unknown`, `op.done`, `op.fail`, `op.unknown`, `autosync.arm`, `autosync.start`,
`autosync.drop`, `picker.refused`. Fields: codes, counts, sizes, HTTP statuses, error keys, a numeric item id. Never: names,
paths (remote or local), addresses, user names, ETags, checksums, the upload id, server text (constitution IV; rules R4/R6
and the new N4 sample stay).

---

## R-18 Mechanical checks (constitution VIII)

Changes to `scripts/check-constitution.sh`, each with a self-test sample that must be detected:

| Rule | Change |
|------|--------|
| N2 (was: no write method in request builders) | becomes an **allow-list**: an HTTP write method literal (`PUT`, `MKCOL`, `MOVE`, `COPY`, `DELETE`, `PROPPATCH`, `LOCK`, `UNLOCK`) is allowed only in `core/src/dav_write.cpp` and `core/src/queue_sender.cpp` (and the generic transport/test files as before); `PROPPATCH`, `LOCK`, `UNLOCK` stay forbidden **everywhere** (a new rule line: this feature never sends them). The bridge, the app and QML still must not name any |
| N3 (no timers) | one allow-list entry added: `bridge/src/queue_bridge.cpp` for `QTimer` only (`sleep` stays forbidden there) |
| N6 (new) | source files are opened read-only: `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`, `"w"`/`"a"` modes, `std::ios::out` are forbidden in `core/src/queue_sender.cpp`, `core/src/upload_source.cpp`, `bridge/src/picker_bridge.cpp` |
| N5 | unchanged: the sender never removes or renames a file on the reader |

Plus tests that act as rules: `test_write_conditions` (every write request has its condition, R-3) and
`test_no_background_network` (extended: with Auto Sync on, nothing is sent at start, on Wi-Fi up, or on a timer other than the
armed one; one quiet period after an action is the only trigger).

---

## R-19 Test strategy (constitution I, IX)

Three layers, written first and seen failing.

1. **Unit:** name validation and the keep-both name (`test_new_name`), forbidden lists, `AutoSyncScheduler` rules (all
   branches, time passed in), queue state machine and recovery rules, dependency ordering, ETag rules (weak/empty), 412
   disambiguation logic (pure function over two `PROPFIND` results), key mapping of write answers including hostile
   exception texts, `Retry-After`, picker walk bounds and link refusal, source identity, chunk arithmetic (10 MiB,
   10,000 limit, padding), upload-folder listing parse (hostile and partial), overlay SQL, schema 3 -> 4 migration (fresh,
   upgrade with data, refusal of schema 5, rolled back failure).
2. **Contract/integration** against an extended `fake_nextcloud_server.h` (write side: `PUT` with `If-Match`/`If-None-Match`,
   `MKCOL`, `MOVE`/`COPY` with `Overwrite`/`If-Match`, `DELETE` with trash flag, `/dav/uploads/` with chunks and expiry,
   quota with `507`, locks (`423`), size/type limits (`413`/`415`), forbidden names, `207` partial failures; failure injection:
   drop the connection after *n* bytes, lose the answer after the request was processed (**the key case for unknown
   outcome**), answer after a delay, ETag change between listing and send, a mode in which the server **ignores** the
   headers of the final `MOVE` to prove the pre-check; weak ETags; hostile names). Every test file defines `PBC_FAKE_SERVER_TEST`.
   A few tests go through the real `CurlTransport` against the local TLS server: a `PUT` from a file range, a cancel in the
   middle, `delivery` after a cut, a redirect refusal for a write.
3. **Bridge/QML:** `QueueBridge` (states, overlay properties, Auto Sync arming with a fake clock), `PickerBridge`
   (listing, selection, folder walk, refusals), QML self-test for `FilePickerPage`, `QueuePage`, `FolderPage` select mode and
   dialogs in both languages; the host UI walk gains `03-upload-and-manage.walk`.

Kill/interruption tests (`kill_harness.h`): kill the sender at every step of a chunked upload (after the row is `sending`,
after the upload folder exists, mid-chunk, after the last chunk, before and after the final `MOVE`, before the confirmation
commit) and of each single-request operation, restart, run recovery, and assert: no item lost, no duplicate item, `unknown`
exactly for the steps R-6.5 names, nothing sent by recovery, the server holds no visible partial file.

Budget tests (`tests/integration/test_budget_queue.cpp`, `test_budget_upload.cpp`) check the figures of the plan and are marked
unverified until measured on the device.

---

## R-20 Verification items (feeds the checklists, constitution IX)

Added to `docs/live-server-checklist.md` as **LV-W1 to LV-W12**: W1 capabilities for the write side (`dav.chunking`,
`files.undelete`, forbidden lists, `chunked_upload.max_size`); W2 `If-Match` / `If-None-Match` on `PUT`; W3 `MOVE`/`COPY` with
`Overwrite: F` and `If-Match`; W4 `MKCOL` answers; W5 chunked upload v2 end to end; W6 which conditions the final chunk `MOVE`
honors; W7 listing an upload folder for resume and the 24-hour expiry; W8 `OC-Checksum` and `X-OC-MTime`; W9 `DELETE` into the
trash and `If-Match` on `DELETE`; W10 the capability lists of forbidden names; W11 status codes of every refusal (quota,
size, type, locked, forbidden) on a stock server and behind a proxy; W12 folder `DELETE`/`MOVE` with `207` member failures.

Added to `docs/device-test-checklist.md` as **DT-W1 to DT-W9**: W1 a bundled-libcurl `PUT` from a file range with progress and
cancel; W2 `delivery` after a cut Wi-Fi during a body; W3 chunk size vs. memory and time; W4 speed of the SHA-256 pass on a
500 MB file; W5 a 500 MB upload with a Wi-Fi drop and resume; W6 power loss during an upload; W7 e-ink refresh count during an
upload; W8 the picker's real top level and the `PBCloud` folder; W9 Auto Sync on a reader whose Wi-Fi is off (owner point 2).

---

## R-21 Versions, schema, documents

`VERSION` 0.1.0 -> **0.2.0** (MINOR: a feature) with a `CHANGELOG.md` entry, as the last phase of tasks.md (constitution:
Development Workflow). Schema 3 -> 4 is one migration in `store_migrations.cpp`; an older build (schema 3) refuses a
schema-4 file (`schema_too_new`, already implemented). `README.md`: remove "read-only" from the description; state that
writes happen only on the user's action. `INSTALL.md`: nothing new to install. `NOTICE`/`licenses/manifest.txt`: attribution
for the picker and scheduler patterns from notes-pocketbook (MIT, same author); no new third-party component.
`docs/speckit-prompts.md` is not touched. Features 001's spec keeps its text (it describes 0.1.0); FR-044 of this spec states
which of its rules this feature lifts.

---

## R-22 Largest file, limits and budgets (constitution V)

| Quantity | Budget |
|----------|--------|
| Largest file that can be queued | 4 GiB - 1 byte (FAT, source side); 0 bytes refused. Exercised by budget tests: 1 GiB on the host (sparse), 500 MB on the device |
| Queue | 2,000 items, database growth <= 2 MB at the limit |
| Folder pick | 2,000 items counted, depth 12, as part of the queue bound |
| Chunk | 10 MiB (server maximum if smaller, never below 5 MiB); one in flight; read buffer 64 KiB |
| RAM | peak <= 150 MB (`VmHWM`) while uploading 500 MB and while the queue screen shows 200 items |
| Time | the queue screen with 200 items opens <= 2 s, page turn <= 1 s; the SHA-256 pass: measured (DT-W4), budget 20 MB/s or better; Auto Sync starts 10 s (+-1 s) after the last action |
| Installed size | package still <= 25 MB (the `package-device.sh` gate); expected growth < 1 MB |
| Screen updates during a transfer | <= 1 per 2 s or per 5 % |
| Network | none in the background; one account check at start; everything else on a tap or after the user's own action with Auto Sync on |
| Server contract | Nextcloud 30 or newer assumed (unverified) |

All figures are budgets, unverified until measured on the InkPad One.


## Implementation notes (2026-10-21)

What the implementation decided or found differently from the text above; the code and `acceptance.md` are the reference.

- **Replace** is a conditional `DELETE` of the old version into the trash and then `Overwrite: F` (R-3, R-8, R-9 above are corrected in place). It needs a trash.
- **Where the logic lives.** Collisions, folder picks, New folder, Rename (`rename_entry`), Move, Copy and Delete are in `core` (`QueueActions`), proven on the host;
  `QueueBridge` only wraps them. A taken name for a rename is a question like any other (keep both, replace a file, skip).
- **One open change per entry.** An entry that has an open item cannot be given a second one (`change_pending`): it avoids two changes racing on one version marker.
- **Versions above a change.** After a confirmed change the cached ETags of the folders above are cleared; a later move, copy, rename or delete of such a folder says
  "refresh first" instead of holding a false conflict caused by the reader's own change.
- **Capabilities.** When the reader never learned them (the start check runs only with Wi-Fi already on), the first Send or Check asks once (`quick_check`).
- **Free space.** `fetch_free_bytes` (one `PROPFIND`) is asked once per run, before the first upload; a failure is not fatal.
- **Large files.** `_FILE_OFFSET_BITS=64` for all targets, because the device is 32-bit and a file can be up to 4 GiB - 1.
- **Tests.** The kill tests die by an exception at the sender's test seam and reopen the data folder (a forked child would lose the fake server's state). QML is covered by
  the app self test (every page and component is instantiated, the new ones must be in the package) and walks 03 to 10.
- **Update.** The `update.run` log line carries the stop reason (a key), to find out why a first request after a Wi-Fi connect can fail.
- **Chooser tool.** The folder chooser has a "New folder" tool (the same queued action as on the Files page), so that a move can target a folder created on the spot; it sends nothing.
- **Review of additions (FR-041, FR-042).** Each of the additions above is a queue-side rule, a request made only on the user's own action (a send, a check, a listing the user opened) or a build setting; none runs by itself, and none logs a name, a path or an address.
- **Replace and the spec.** FR-021 and Story 3 scenario 9 name Replace for any collision; the product offers it for files over files only, as research R-8 says. The spec text
  should be narrowed the next time it is touched.
