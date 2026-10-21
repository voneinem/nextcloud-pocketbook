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
