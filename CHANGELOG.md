# Changelog

Versions follow MAJOR.MINOR.PATCH. A branch that changes the product raises VERSION and adds an entry here
(`scripts/check-version-bump.sh`). The tag `v<version>` is set after the merge.

## 0.1.0 - 2026-10-20

First feature of PB Cloud: a read-only Nextcloud files client for the PocketBook InkPad One (specs/001-nextcloud-files-client). Everything
below is tested on the host against a fake Nextcloud server; nothing is verified against a real server or on a reader yet
(`docs/live-server-checklist.md` LV-N1 ..., `docs/device-test-checklist.md` DT-NC-1 ...).

### Connect
- Server address, user name and app password with a Test that shows the server name, version and quota; or a browser login (Login Flow v2) with a
  link and a QR code. https only, certificates always verified, no redirect to another host.
- A message and a way forward for each failure: wrong credentials, two-factor authentication, maintenance mode, server too old or without
  WebDAV, not a Nextcloud, untrusted certificate (`custom-ca.pem`), wrong reader clock.
- The app password is stored as plain text and the app says so, measuring what the reader's storage does with the file. The account password
  is never stored. At start, one check that the login still works, only when Wi-Fi is already on.

### Browse
- Folders first, page by page, sorted by name, date or size; a path line and Up. Folders are fetched on the first visit and on Refresh only;
  visited folders stay readable offline with the age of the list. Entries that are not available are shown as such.

### Download and open
- Details of a file, download with coarse progress, cancel, resume after a lost connection, free-space check, warning for large files,
  refusal for what the storage cannot hold (including the 4 GiB limit of FAT), no half-downloaded file in the library, and a one-tap open in
  the reader's viewer. A copy changed on the reader is never overwritten without asking (keep mine, keep both, replace after a second
  confirmation).

### Keep offline, favorites, search, storage
- Keep files and folders offline; **Update** (only on a tap) downloads what is new or changed and leaves conflicts to the user.
- Favorites of the account; search by name offline and, on a tap, on the server.
- A Storage page with the downloaded copies, free space and the cache; removing a copy never touches the server.

### Data
- **One-way database upgrade (schema 2).** The first start migrates the database and keeps `pbcloud.db.bak` until the next successful start;
  an older build refuses the new file. See `INSTALL.md`.

### Removed
- Leftovers of the baseline: the sync scheduler, the page arithmetic of the message reader, FTS5 and the calendar and contact QML components
  (`docs/baseline-leftovers.md`).
