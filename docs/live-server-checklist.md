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

## Recording

For every `V` or `F` add the date and one line (no addresses, no host names that identify the owner). Put
anonymized transcripts in `tests/fixtures/` with the suffix `.unverified` until a live run has produced
them; then rename. Findings that change a decision go into `docs/device-findings.md`.
