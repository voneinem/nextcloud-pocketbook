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

## Recording

For every `V` or `F` add the date and one line (no addresses, no host names that identify the owner). Put
anonymized transcripts in `tests/fixtures/` with the suffix `.unverified` until a live run has produced
them; then rename. Findings that change a decision go into `docs/device-findings.md`.
