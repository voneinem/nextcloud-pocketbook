# Nextcloud fixtures

Answers of a Nextcloud server as the app parses them: `status/` (status.php), `ocs/` (capabilities and user),
`propfind/` (WebDAV listings), `report/` (favorites), `search/` (server search), `login/` (Login Flow v2).

Rules:

- Every file imitating a server answer is **unverified** until a live run produced it (constitution Principle IX,
  `docs/live-server-checklist.md`). Such a file carries the suffix `.unverified` before the extension, for example
  `listing_basic.unverified.xml`, as described in `tests/fixtures/README.md`. The suffix is removed only for an
  exchange that was recorded from a real server and anonymized.
- Names are reserved example names only: hosts `example.org` / `cloud.example.org`, user `demo`, generated hex ETags.
- XML namespace URIs (`http://owncloud.org/ns`, `http://nextcloud.org/ns`) are protocol identifiers, not servers.
- No `Authorization` value, no real token, no real user data.

## Write side (feature 002)

`write/` holds the answers of the write requests (`put/`, `dav/` for `MKCOL`/`MOVE`/`COPY`/`DELETE`, `uploads/` for the chunked
upload folder, `capabilities/` for the capability lists, `errors/` for refusals with a sabre error body). The same rules
apply: reserved example names, the `.unverified` suffix before the extension until a live run produced the exchange, no
`Authorization` value. Hostile and partial variants (markup and control characters in an exception class, a truncated
multistatus) are kept here too.

## Folder synchronization (feature 003)

`sync/` holds the answers the folder scans read: `listings/` (complete multistatus listings with `oc:checksums`), `partial/` (a
multistatus in which some members failed, or that was cut short), `hostile/` (names that must never reach a path outside the
pair's reader folder) and `unavailable/` (entries without permission or encrypted). The same rules apply: reserved example
names, no `Authorization` value, and the `.unverified` suffix before the extension until a live run produced the exchange
(constitution Principle IX).

## Share links (feature 004)

`shares/` holds the answers the sharing screens read: `capabilities/` (the `files_sharing` object of the capabilities answer in several
shapes), `lists/` (share lists of a path), `details/` (single shares with and without a link, a hash in `share_with`, other share types) and
`hostile/` (huge, truncated, mistyped and control-character variants). The same rules apply: reserved example names and hosts, no
`Authorization` value, no real tokens, and the `.unverified` suffix before the extension until a live run produced the exchange
(constitution Principle IX).

## Trash and versions (feature 006)

`trash/` holds the answers the trash and versions screens read: `trash_list_*.unverified.xml` (the trashbin listing: a normal one,
an empty one and a hostile one with unsafe names, impossible locations, deletion times and sizes, a failed member, a foreign and a
duplicate href), `versions_list_*.unverified.xml` (the version list of a file: a normal and a hostile one) and
`caps_*.unverified.json` (the `files` object of the capabilities answer with the trash and versions apps on, off, missing and
without a `files` object). The same rules apply: reserved example names, user `demo`, no `Authorization` value, and the
`.unverified` suffix before the extension until a live run produced the exchange (constitution Principle IX). The file names
follow the project convention (`name.unverified.xml`), not the order written in the task list of feature 006.
