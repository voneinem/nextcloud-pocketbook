# Changelog

Versions follow MAJOR.MINOR.PATCH. A branch that changes the product raises VERSION and adds an entry here
(`scripts/check-version-bump.sh`). The tag `v<version>` is set after the merge.

## 0.2.0 - 2026-10-21

Upload and manage files on the server (specs/002-manage-server-files). Tested on the host against a fake Nextcloud server with a write side; nothing
is verified against a real server or on a reader (`docs/live-server-checklist.md` LV-W1 ..., `docs/device-test-checklist.md` DT-W1 ...).

### Upload
- *Add > Upload files*: a file browser of the reader's storage (list or tiles, remembered) with whole folders; the picks go into a persistent
  queue and show as **Not synced**. Sources are only ever read; links, hidden and special files are skipped and the app's own data folder can
  never be picked.
- A file up to 10 MiB is one request; larger files go in chunks of an upload folder of their own and are assembled by one final request. An
  interrupted upload continues with the missing chunks, starts over when the file changed, and leaves nothing incomplete in the user's folders.
- An upload whose outcome is unknown (the answer was lost) is kept and flagged; "Check the server", "Send again" and "Mark as done" decide.

### Manage
- New folder, rename, move, copy and delete (select mode). Delete goes to the server's trash and is not offered without one.
- Every write request carries its condition (`If-None-Match: *`, `If-Match` with the stored version, `Overwrite: F`). A taken name asks (keep
  both, replace after a confirmation, skip); a replace of a file is a conditional delete of the old version into the trash and a request that
  cannot overwrite. A change on the server meanwhile holds the item as a conflict with both versions intact.
- The reader's own records follow: a renamed or moved entry takes its downloaded copy and its offline mark along; a replacement uploaded from
  the entry's own downloaded copy links the copy to the new version.

### Queue and Auto Sync
- The Queue screen (Send, Cancel, Retry, Resolve, Remove). **Auto Sync** (Settings; on by default, switchable) sends once, a few seconds after
  the user's own action and only when Wi-Fi is already on; never at start, never when Wi-Fi comes back, never after a failure.
- Own messages for a full account (also found out before any byte is sent), a forbidden name (editable), a file too large, a type the server
  refuses, a locked file, a refused login.

### Changes to 0.1.0
- Database schema 4 (the queue, the server's capabilities). The **Update**, **Favorites** and **Search** tools moved from the Files page to the
  start page; the Files page has Refresh, Keep offline, Add, Select, Queue and Sort.

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
- **One-way database upgrade (schema 3).** The first start migrates the database and keeps `pbcloud.db.bak` until the next successful start;
  an older build refuses the new file. A database written by an earlier development build of 0.1.0 (schema 2) is upgraded by one more step that adds the availability
  of favorites (a favorite you may not read is shown as unavailable and is not downloaded). See `INSTALL.md`.

### Home page
- The home page shows when the account was last checked (the last time the server accepted the login: the start check, a Test, a listing, a download or an Update) and when the last Update ended.
- Dates follow the reader's time zone also when `global.cfg` names it by the city only (`timezone=Berlin`, seen on the InkPad One); a city that exists in several regions is not guessed and the dates stay in UTC.

### Log
- The launcher writes the log line by line while the app runs, so an app that is stopped (the reader's USB mode does that) still leaves its lines.
- The launcher removes the lines in which the reader's libraries print the path of an opened book (`iv_OpenBook`, `task_findbybook`) before they reach `pbcloud.log`; the app's own lines, its exit code and its start-up evidence stay (DT-NC-11).

### Removed
- Leftovers of the baseline: the sync scheduler, the page arithmetic of the message reader, FTS5 and the calendar and contact QML components
  (`docs/baseline-leftovers.md`).
