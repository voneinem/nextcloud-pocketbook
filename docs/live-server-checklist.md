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

## Recording

For every `V` or `F` add the date and one line (no addresses, no host names that identify the owner). Put
anonymized transcripts in `tests/fixtures/` with the suffix `.unverified` until a live run has produced
them; then rename. Findings that change a decision go into `docs/device-findings.md`.

Note on LV-W3 and LV-W6 (implementation, 2026-10-21): the app never relies on `Overwrite: T`. A replace of a file is a conditional `DELETE` of the old version
(`If-Match`, into the trash; refused where there is no trash) followed by the `MOVE` or `COPY` with `Overwrite: F`; a replace by a small `PUT` is `If-Match`.
The final chunk `MOVE` is always `Overwrite: F` and is preceded by a `PROPFIND` of the target, because which conditions that request honors is unverified.
