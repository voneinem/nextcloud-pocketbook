# Changelog

Versions follow MAJOR.MINOR.PATCH. A branch that changes the product raises VERSION and adds an entry here
(`scripts/check-version-bump.sh`). The tag `v<version>` is set after the merge.

## 0.5.0 - 2026-10-24

Mail link (specs/005-mail-link). Tested on the host with a stand-in for the mail app; on the reader DT-M1, DT-M3 and DT-M4 were run (the draft opens); DT-M2 (PB Office
running or missing) is still to be run (`docs/device-test-checklist.md`). The hand-off itself was observed on the reader with the probe of 0.4.0 (`docs/device-findings.md`). No database change.

### Mail link
- On the screen of a share link a new tool **Mail link** hands the link to PB Office, which opens a new mail message with it and the subject "Link to <name>". A
  confirmation (Cancel is the default) says where the link goes, that a password is not included and that the draft is saved to your Drafts folder later. You choose
  the recipient and send the mail in PB Office; PB Cloud sends nothing. It needs PB Office 0.4.0 or later, installed and not running.
- If PB Office does not take the link (it is running or not installed), a short notice says so the next time the app is shown; nothing is retried and the link is not
  shown again. A running PB Office does not take the link (observed on the reader).
- The link is never logged, stored or put on a clipboard, a password is never handed over, and the buffers that held the link are overwritten right after the call.
  New mechanical rule N10 (one call site for the hand-off, one marker, no logging in the hand-off modules); constitution 1.2.0 names the file that makes the call.

## 0.4.0 - 2026-10-22

Share links of files and folders (specs/004-share-links). Tested on the host against a fake Nextcloud server; nothing is verified against a real server or on a
reader (`docs/live-server-checklist.md` LV-L1 ..., `docs/device-test-checklist.md` DT-L1 ...). No database change: nothing about sharing is stored.

### Share links
- *Select* an entry in the folder listing (or open a file's details) and tap **Share**: the public links and the shares with users and groups of the item,
  page by page (users and groups are listed only; the app does not create, change or remove them).
- **Create link** with an optional password, an optional expiry date and read only or, for a folder, read and upload, after a confirmation that says what the
  link will allow. The link is shown in large letters on its own screen; **Copy** exists only where the platform has a system clipboard: not on the reader
  (a test showed that nothing copied there reaches another app), so the link is read and typed from the large display. Change a link's password, expiry date or permission one confirmed action at a time, and remove it after a confirmation.
- Clear messages when the server forbids public links, forces a password or an expiry date, limits the permissions or refuses an item; the controls show the
  server's rules before you act. A change made by someone else meanwhile is never overwritten; a lost answer is an unknown outcome and is never repeated.
- A tap on a sharing action with Wi-Fi off asks the system to bring Wi-Fi up, like the rest of the app, and goes on; if it does not come up the action says so and sends
  nothing. The app never does this by itself.
- Links and passwords are secrets: shown only on the screen you asked for, never in a list, a log, an error text or a file, never opened by the app; a typed
  password is sent once and discarded. New mechanical rules N8 (the share code cannot log), N9 (nothing opens a link), U7 and U8 (the link display is plain and
  cannot be tapped; a password entry is a secret field).

## 0.3.0 - 2026-10-22

Folder synchronization (specs/003-folder-sync). Tested on the host against a fake Nextcloud server; nothing is verified against a real server or on a
reader (`docs/live-server-checklist.md`, `docs/device-test-checklist.md`).

### Sync
- Pairs of a server folder and a reader folder, download only, upload only or two-way, with limits (files and bytes) and a free-space reserve.
- Comparisons only at start, on the button and shortly after your own change; they write nothing. You review the plan, approve it, and see the result.
- Uploads go through the queue of 0.2.0; downloads through the download engine with the planned version required.
- A change on both sides keeps both versions; deletions are proposals that need a confirmation naming the file; interrupted syncs are repeatable.
- Database schema 5 (sync pairs, records, plans; extra queue and download columns). A 0.2.0 database is migrated at start.

## 0.2.0 - 2026-10-21

Upload and manage files on the server (specs/002-manage-server-files). Tested on the host against a fake Nextcloud server with a write side; nothing
is verified against a real server or on a reader (`docs/live-server-checklist.md` LV-W1 ..., `docs/device-test-checklist.md` DT-W1 ...).

### Icon
- The launcher icon is the Nextcloud mark (three rings, black on white, no frame), redrawn from the selfh.st "nextcloud" icon (CC BY 4.0);
  attribution in NOTICE and the About screen.

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
