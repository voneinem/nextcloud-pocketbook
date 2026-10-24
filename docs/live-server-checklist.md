# Live-server checklist

Template, reduced to the server behaviour that every client of a DAV server relies on. The items of a feature
are added with the feature's plan.

Every item starts as **unverified**: the tests of this project run against fake servers that implement what the
specifications say, not what a particular server does. An item changes only when someone has run it against a
real server and written the date and the result next to it. Use **app-specific passwords** and throw-away
test accounts; never paste real data, addresses or passwords into this file or into the repository. Anonymized
exchanges that were seen live become fixtures: drop the `.unverified` suffix only for those.

Targets: Nextcloud first, OpenCloud second.

Legend: **U** unverified · **V** verified (date, target, note) · **F** failed (what happened) · **n/a** the
target does not offer it.

Columns for every table: Nextcloud / OpenCloud. Write the result per target, for example `V 2026-11-02`.

## Connection and security

| ID | Item | Expected | Nextcloud | OpenCloud |
|----|------|----------|-----------|-----------|
| LV-1 | Connection with the right and a wrong app password | Success; the wrong password gives the password message and no other detail | U | U |
| LV-2 | A server with a certificate from a private authority (`custom-ca.pem`) and one with an unknown certificate | the first works, the second is refused; no way to skip the check | U | U |
| LV-3 | A redirect at the server (http to https, or to another path) | followed on https only; credentials not forwarded to another host | U | U |
| LV-4 | The server's time (HTTP `Date`) is read; set the reader's clock 1 hour off and sync | the clock warning appears | U | U |

## WebDAV basics

| ID | Item | Expected | Nextcloud | OpenCloud |
|----|------|----------|-----------|-----------|
| LV-D1 | `PROPFIND` with `Depth: 0` and `Depth: 1` on the account's home: names, sizes, ETags, last-modified, content types | answers parsed by the minimal XML reader; a malformed answer is refused, not guessed | U | U |
| LV-D2 | Create with `If-None-Match: *` | accepted; a second create of the same path is refused | U | U |
| LV-D3 | Update and delete with `If-Match` and a stale ETag | `412`; handled as a conflict; note any server that ignores the header | U | U |
| LV-D4 | Answers `403`, `404`, `413`, `507` on a write | the item stays marked with the reason; nothing lost | U | U |
| LV-D5 | Resources that disappear on the server while a change waits on the reader | the app says so; local data stays until the user decides | U | U |

## Nextcloud files client (feature 001)

Added by `specs/001-nextcloud-files-client/`. Same legend and rules as above. The expected behaviour is the contract in
`specs/001-nextcloud-files-client/contracts/server-requests.md`.

| ID | Item | Expected | Nextcloud | OpenCloud |
|----|------|----------|-----------|-----------|
| LV-N1 | `status.php` and the capabilities answer on Nextcloud 30 and on a current major | fields `installed`, `maintenance`, `needsDbUpgrade`, `version`, `productname`, `theming.name` exist as assumed; version below 30 is reported "too old" | U | U |
| LV-N2 | Basic authentication with the account password on a two-factor account | a 401; note whether the answer carries a readable "app password" marker | U | U |
| LV-N3 | Quota properties of the home folder | `quota-used-bytes` and `quota-available-bytes`; note the values meant for unlimited and unknown | U | U |
| LV-N4 | Login Flow v2 end to end | poll answers 404 until approved, then 200 once; `login` and `poll.endpoint` are on the server's own origin | U | U |
| LV-N5 | Availability markers in a depth-1 listing: an end-to-end encrypted folder, a share without read permission, an unmounted external storage; the letters of `oc:permissions` | each is recognised as `encrypted`, `no_permission`, `unsupported`; the rest of the listing is intact | U | U |
| LV-N6 | `Range` and `If-Range` on a file GET | 206 with the right `Content-Range`; 200 when the ETag differs; 416 for a wrong range | U | U |
| LV-N7 | Folder ETag after a change three levels down | the ancestors' ETags change (informs a later optimization; the app does not rely on it) | U | U |
| LV-N8 | Favorites `REPORT` with `oc:filter-files` | the starred files and folders are returned with the requested properties | U | U |
| LV-N9 | `SEARCH` with `d:like` on `displayname`, with `%`, `_` and `\` in the term | name matches within the account, limit honoured; note how the three characters behave | U | U |

## Nextcloud files client write side (feature 002)

Added by `specs/002-manage-server-files/`. Same legend and rules as above. The expected behaviour is the contract in
`specs/002-manage-server-files/contracts/server-requests.md`.

| ID | Item | Expected | Nextcloud | OpenCloud |
|----|------|----------|-----------|-----------|
| LV-W1 | Capabilities for the write side: `dav.chunking`, `files.undelete`, `files.chunked_upload.max_size`, the four forbidden-name lists | present with the assumed JSON paths; note what a missing key means | U | U |
| LV-W2 | `If-Match` and `If-None-Match: *` on `PUT` | a stale ETag and an existing name give 412; a `W/` ETag never matches `If-Match` | U | U |
| LV-W3 | `MOVE` and `COPY` with `Overwrite: F` and `If-Match` | 412 for an existing target or a changed source (note which); folders; a target inside the source is refused | U | U |
| LV-W4 | `MKCOL` answers | 201 with `OC-Etag` and `OC-FileId`; 405 for an existing name; 409 for a missing parent | U | U |
| LV-W5 | Chunked upload v2 end to end | `MKCOL`, five-digit chunk `PUT`s, final `MOVE`, abort `DELETE`; early 507 through `OC-Total-Length` | U | U |
| LV-W6 | Which of `If-Match`, `If-None-Match`, `Overwrite`, `OC-Checksum` the final chunk `MOVE` honors | the conditions the app sends are enforced, or the finding is recorded and the app keeps its `PROPFIND` pre-check | U | U |
| LV-W7 | `PROPFIND` of an upload folder and the 24-hour expiry | chunk names and sizes listed; an expired folder is gone | U | U |
| LV-W8 | `OC-Checksum` and `X-OC-MTime` | the checksum is reported back in `oc:checksums`; the modification time is applied (accepted marker) | U | U |
| LV-W9 | `DELETE` of a file and of a folder | lands in the trash; `If-Match` on `DELETE` is enforced; 404 for a path that is gone | U | U |
| LV-W10 | The capability lists of forbidden names, characters, base names and extensions | JSON paths and contents as assumed | U | U |
| LV-W11 | The status code of every refusal: quota, size, type, locked, forbidden name, missing parent, permission, also after a write behind a reverse proxy (500/502/504) | the mapping of the contract holds; note the exception classes seen | U | U |
| LV-W12 | `207` member failures on a folder `DELETE`, `MOVE` and `COPY` | the members that failed are listed; nothing else is changed | U | U |

## Folder synchronization (feature 003)

Added by `specs/003-folder-sync/`. Same legend and rules as above. The expected behaviour is in
`specs/003-folder-sync/research.md` (R-3, R-4, R-10, R-14) and its contracts.

| ID | Item | Expected | Nextcloud | OpenCloud |
|----|------|----------|-----------|-----------|
| LV-S1 | `oc:checksums` in listings | present for files, with the algorithm per file (`SHA256:`, `SHA1:`, `MD5:`); note which server settings change it | U | U |
| LV-S2 | ETag stability of a file across a rename and across a desktop-client sync | the file ETag does not change by a rename or by a sync that did not change content; note what does change it | U | U |
| LV-S3 | `Depth: 1` walk of a tree of 1,000 files | time and number of requests; no truncated answers; note paging | U | U |
| LV-S4 | A listing that fails halfway (multistatus with member errors, a cut connection) | the scan is marked incomplete and no deletion is proposed from it | U | U |
| LV-S5 | Conditional replace of a synced file (`If-Match` with the recorded ETag) | a stale ETag gives 412 and the 002 conflict handling keeps both versions | U | U |
| LV-S6 | Server deletion from a proposal | the file lands in the trash; `If-Match` is enforced | U | U |
| LV-S7 | Unavailable entries inside a paired folder (no permission, encrypted) | they are listed as unavailable or not at all; the scan completes and nothing is proposed for them | U | U |
| LV-S8 | External changes between the compare and the run | the changed entry ends `changed_during_sync` and is not transferred | U | U |

## Share links (feature 004)

Added by `specs/004-share-links/`. Same legend and rules as above. The expected behaviour is in
`specs/004-share-links/research.md` (R-1 to R-9, R-20) and its contracts.

| ID | Item | Expected | Nextcloud | OpenCloud |
|----|------|----------|-----------|-----------|
| LV-L1 | Shape of the `files_sharing` capabilities across server versions | the keys of `contracts/share-requests.md` L1 exist (`public.enabled`, `password.enforced`, `expire_date.*`, `upload`); note differences per version | U | U |
| LV-L2 | Permission values of public links | a read-only link is `1`; a folder link with upload is `15`; a file link is `1` (or `3` when editable); note the values the server returns | U | U |
| LV-L3 | Where the password state of a link appears in the answer | `password` and/or `share_with` is non-empty for a link with a password; the value is a hash, not the password | U | U |
| LV-L4 | `GET .../shares/inherited` | lists the shares that reach the item from a parent folder, or answers 404/405/501 where not offered | U | U |
| LV-L5 | `reshares=true` on `GET .../shares?path=` | includes shares made by other users that the account can see | U | U |
| LV-L6 | The HTTP status and OCS `statuscode` of each enforcement (public links off, password enforced, expiry enforced, expiry too far, upload not allowed, item not shareable) | each is a 400/403/404 class answer that the re-read of the capabilities explains (research R-4); note the values | U | U |
| LV-L7 | Clearing a password (`password=`) and an expiry (`expireDate=`) with empty form values | the field is cleared; note refusals when the server forces them | U | U |
| LV-L8 | `DELETE` of an unknown share id | 404; nothing else changes | U | U |
| LV-L9 | The host of a link behind a reverse proxy or with a configured public address | note whether it differs from the connected host (the app flags a different address) | U | U |
| LV-L10 | Form-encoded `POST` and `PUT` bodies on `/ocs/v2.php` and `/ocs/v1.php` | both are accepted; the HTTP status mirrors the OCS status on v2 | U | U |

## Trash and versions (feature 006)

Added by `specs/006-trash-and-versions/`. Same legend and rules as above. The expected behaviour is in `specs/006-trash-and-versions/research.md` (R-1 to R-10) and
`contracts/trash-requests.md`. Everything below is what the plan assumes from the public documentation of the trashbin and versions endpoints; nothing was seen live.

| ID | Item | Expected | Nextcloud | OpenCloud |
|----|------|----------|-----------|-----------|
| LV-T1 | The capabilities with the trash app and the versions app enabled and disabled | `files.undelete` and `files.versioning` are `true` while the app is on and the key is missing or `false` when it is off; note the shapes per version and what a server without the `files` object does | U | U |
| LV-T2 | `PROPFIND` Depth 1 on `/remote.php/dav/trashbin/<uid>/trash/` with the properties of T2 | `nc:trashbin-filename`, `nc:trashbin-original-location` (a path relative to the files, with the name), `nc:trashbin-deletion-time` (epoch seconds), size, resource type, `oc:fileid`, permissions and `nc:is-encrypted` come back; note the shape of the location of an item deleted from a shared folder or from the root, the href of a folder (trailing slash) and the permission letters | U | U |
| LV-T3 | `MOVE` of a trash item to `.../trashbin/<uid>/restore/<name>` | the item returns to its recorded place; a taken name and a missing folder are refused (note the status and body) and nothing existing is replaced; what the server does when the name appears between a probe and the request (refuse or rename) | U | U |
| LV-T4 | Restore of a folder | the folder returns with its whole content as one item | U | U |
| LV-T5 | `MOVE` of a trash item to a files path with `Overwrite: F` (restore to a chosen place) | accepted (201/204) with a free name; 412 for a taken name; 409 for a missing folder; note the status and body of a server that does not allow it (the plan degrades to a message) | U | U |
| LV-T6 | `DELETE` of one trash item by its name, and of an item that is gone | 204; 404 for a gone item; a folder goes with its content; the collection is never deleted by the app (Empty trash is single deletes of 50 items: time and any rate limit, note both) | U | U |
| LV-T7 | `PROPFIND` Depth 1 on `/remote.php/dav/versions/<uid>/versions/<fileid>/` | the older versions with `d:getlastmodified` and `d:getcontentlength`; the version id is the modification time in digits; the current content is not listed; note the answer for a file without versions (207 with the collection only, or 404) | U | U |
| LV-T8 | `MOVE` of a version to `.../versions/<uid>/restore/target` | the file's content becomes the version's and the content that was current is listed as a version afterwards (the plan relies on it and reports a server that does not); note whether the restored version stays in the list | U | U |
| LV-T9 | A version restore while the file changed meanwhile | the app refuses before sending (the ETag of the file is read first); note what the server does with a stale restore and whether it keeps the newer content as a version | U | U |
| LV-T10 | A server with the trash app disabled and one with the versions app disabled | the capabilities say so; the endpoints answer 404 (note); the app explains in plain words and offers no action; deleting on such a server is permanent (feature 002) | U | U |

## Recording

For every `V` or `F` add the date and one line (no addresses, no host names that identify the owner). Put
anonymized transcripts in `tests/fixtures/` with the suffix `.unverified` until a live run has produced
them; then rename. Findings that change a decision go into `docs/device-findings.md`.

Note on LV-W3 and LV-W6 (implementation, 2026-10-21): the app never relies on `Overwrite: T`. A replace of a file is a conditional `DELETE` of the old version
(`If-Match`, into the trash; refused where there is no trash) followed by the `MOVE` or `COPY` with `Overwrite: F`; a replace by a small `PUT` is `If-Match`.
The final chunk `MOVE` is always `Overwrite: F` and is preceded by a `PROPFIND` of the target, because which conditions that request honors is unverified.
