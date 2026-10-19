# Nextcloud Android app: feature and sync analysis (Files part)

> **Licensing note.** The analyzed project (nextcloud/android) is licensed GPL-2.0 (`LICENSE.txt`); the README says
> that contributions from 2016-06-16 on are licensed AGPL-3.0-or-later, and source files carry SPDX lines such as
> "GPL-2.0-only AND (AGPL-3.0-or-later OR GPL-2.0-only)". This document paraphrases observed behavior in my own words
> only. No source code, comments, documentation text or UI strings were copied. Names of public Nextcloud API
> endpoints, header names and field names are mentioned as facts about the public API.
>
> Analyzed checkout: a read-only `git clone --depth 1` of https://github.com/nextcloud/android, kept outside this
> repository and never committed. Commit `0ae8a0be73d895be691463cc09979210b8e429b3` (merge of PR #17830,
> 2026-10-19). `app/build.gradle.kts` names version 35.1.0, minSdk 28, targetSdk 36; the local Room database is at
> schema version 106; the client's own constant for the oldest supported server is Nextcloud 20. Code root:
> `app/src/main/java/com/owncloud/android/` (older code, Java) and `.../com/nextcloud/client/` (newer code, Kotlin).
>
> **What was not analyzed.** All HTTP/WebDAV request building (the actual PROPFIND bodies, the headers, chunk
> handling, checksum computation, status-code mapping) lives in a separate repository, `nextcloud/android-library`,
> which the app pins by commit (`42ca8fd5598ac7c8320a985563f9ecf606a06d87` in `gradle/libs.versions.toml`). That
> repository was not cloned. Whatever depends on it is marked **(inferred)** or **(unverified)**. The server
> contract itself is in `docs/reference/nextcloud-api-notes.md`.
>
> Statements marked **(unverified)** or **(inferred)** were not confirmed directly in the client code.

---

## 1. Overview

The app is the official Nextcloud client for Android. Its Files part is a classic remote file browser with a local
cache: the folder tree and the metadata of every folder the user has opened are kept in a Room/SQLite database,
file contents are stored as ordinary files in the app's storage area on demand, and a set of background workers
keeps selected content fresh and pushes queued changes.

Technology that matters for behavior (not to be copied): Room for the database, WorkManager for background jobs,
foreground activities plus event-bus messages for UI updates, the separate android-library for WebDAV/OCS calls,
Android's AccountManager for credentials, and Gson for JSON. A very large part of the code (media gallery, Talk
hooks, photo auto-upload, rich document editors, end-to-end encryption, assistant, widgets) is unrelated to plain
file sync and is out of scope here.

The server side is plain WebDAV (`remote.php/dav`) plus OCS endpoints. The client never needs anything beyond
what the official developer manual documents, apart from a few app-specific OCS endpoints (rich documents,
direct editing) that this project does not need.

---

## 2. User-visible features of the Files part

Navigation: a drawer and a bottom bar. Entries relevant to files: all files, personal files, favorites, shared
with/by me, recent files, group folders, activity, uploads list, files on the device, trash bin, settings.
(Gallery, albums and the assistant sit in the same menu; they are not part of this analysis.)

- **Browse.** Opening a folder shows the cached listing at once and starts a refresh in the background (section 4).
  The list can be a plain list or a grid; a path breadcrumb is shown. While the folder screen is visible the app
  polls the current folder's ETag every 30 seconds and refreshes the list when it changed.
- **Sort.** Six orders: name A-Z / Z-A, date old-new / new-old, size small-big / big-small. Two separate switches
  decide whether folders always come before files and whether favorites come first. The chosen order is stored per
  kind of list (trash bin, local file list, favorites list) **(inferred from the enum of list kinds)**.
- **Search.** A search field over the current scope. The app issues a server-side search (a `SearchRemoteOperation`
  from the library; the wire form is a WebDAV `SEARCH` or `REPORT` **(inferred)**) and, for favorites and shares,
  first shows the cached rows. A separate "unified search" screen queries the server's search providers through
  OCS (class `UnifiedSearchRemoteOperation`).
- **Favorites.** A star toggle per file or folder (a PROPPATCH of `oc:favorite` **(inferred from the documented
  API)**). The favorites list is a server query; the cached favorites of the account are shown immediately while
  the query runs. Favorites are also the group the periodic content refresh cares about in older code paths
  (log messages say "favourites" for kept-in-sync files).
- **Recents.** A server-side search for files modified during the last 14 days, limited to 100 results, sorted
  newest first, not grouped by favorites.
- **Available offline.** Two separate mechanisms exist:
  1. *Downloaded copies are kept fresh.* Any file the user downloaded stays in local storage. A periodic job walks
     the local tree, compares folder ETags and re-syncs the downloaded files below a changed folder
     (`OfflineSyncWork`). A user can also trigger "sync this folder" which refreshes the folder and downloads the
     content of the files in it.
  2. *Internal two-way sync folders.* The user marks folders; a periodic worker (`InternalTwoWaySyncWork`)
     synchronizes them in both directions: remote changes are pulled, files that are new in the local copy of the
     folder are uploaded, new local sub-folders are created on the server, and a file that vanished from the local
     copy while its ETag still equals the server's is deleted on the server. The worker only runs on Wi-Fi with the
     setting on, outside power-save mode, and it checks free space before it starts.
- **Download / open.** Tapping a file downloads it (progress in a notification and in the row), then opens it in an
  in-app preview (images, text, PDF, audio/video playback) or hands it to another app ("open with"). A file can be
  exported (copied) to a user-chosen place on the device. A whole folder can be downloaded by a folder worker.
  Streaming media directly from the server is possible through the server's direct-download link endpoint.
- **Upload.** Entry points: the "+" sheet (upload files from the device, upload through another app's picker,
  camera, document scan), the Android share sheet (a separate receiver activity lets the user pick the target
  folder), and the offline queue. Each upload is a row in an uploads list with states in progress, succeeded,
  failed, cancelled, plus a conflict state. Upload preferences per transfer: Wi-Fi only, while charging only,
  what to do with the local file afterwards (keep, move into the app's storage, delete) and a name-collision
  policy (ask, overwrite, rename, skip).
- **Create folder.** From the "+" sheet; also possible offline (queued, section 4.5).
- **Rename, move, copy.** Rename is an in-place WebDAV MOVE **(inferred)**; move and copy go through a folder picker; the code
  refuses moving or copying a folder into itself or its descendants, and refuses a rename or move onto an
  existing name (the "overwrite" flag is off).
- **Delete.** WebDAV DELETE; whether the item goes to the server's trash bin is a server decision (with the
  trashbin app enabled it does). A delete done while offline is queued and applied later.
- **Trash bin.** A screen listing trashed items of the account, with restore, delete permanently, and empty the
  trash bin (classes `ReadTrashbinFolderRemoteOperation`, `RestoreTrashbinFileRemoteOperation`,
  `RemoveTrashbinFileRemoteOperation`, `EmptyTrashbinRemoteOperation`).
- **Versions.** In the details sidebar, the "activity" tab shows the file's activity entries and, when the server
  advertises the versioning capability, the list of stored versions (listed by the file's numeric id) with a
  *restore* action. There is no delete or label function for versions.
- **Sharing.** The sharing tab lists and creates shares: user, group, e-mail, public link, federated user and
  group, team ("circle"), and Talk room. Permissions, password, expiry date, note, label and the download-allowed
  attribute are editable; links can be copied. A "shared with others" list view and a "shared with me" view exist.
  Separately, a "send" action hands the downloaded file (or a link) to another Android app through the system share sheet.
- **Activity.** The activity feed of the account and of a single file (OCS activity API).
- **Details sidebar.** Three tabs: activities (with versions), sharing, and details (size, dates, owner, tags,
  location data, lock state **(inferred)**). Tags and comments are visible; the unread-comments count is stored
  per file.
- **File locking.** If the server advertises locking, a lock/unlock action appears; the lock owner and lock state
  are shown. The client calls a lock-toggle operation and then simply refreshes the list.
- **Misc.** Pin a file to the Android home screen; set a picture as wallpaper; text and office documents can be
  edited through server-side web editors in a web view (out of scope); end-to-end encrypted folders (out of
  scope). A "files on this device" view lists the local storage without any server access.

---

## 3. Accounts and login

**Account storage.** Accounts are Android `AccountManager` accounts; the account name is built from the login name and
the server URI (the exact format was not checked). The credentials (login name and app password) sit in the account
manager. Per account the database holds the capability
row and an arbitrary key/value table. Several accounts can exist; the code is written so that every table row
carries the account.

**Login Flow v2 (the main path).**
1. The user enters the server address. The app normalizes it (adds a scheme, strips a trailing `/remote.php/dav`
   or `/index.php/apps/...` fragment).
2. The app posts an empty body, anonymously, to `<server>/index.php/login/v2`.
3. The answer carries a browser URL (`login`) and a polling block (`poll.token`, `poll.endpoint`). A fallback
   path exists for answers that do not parse into the expected shape.
4. The browser URL is opened in a Custom Tab / Auth Tab (or the default browser). The user logs in there, including
   two-factor authentication, and grants access.
5. While the app is in the foreground it posts the token to the poll endpoint every 30 seconds. A 200 answer carries
   `server`, `loginName` and `appPassword`; the app stores them and ends polling. The token is only valid for a
   limited time (the official doc says 20 minutes).
6. The app then checks the server (the `status.php` document and an unauthenticated probe to detect the auth
   method, via `GetServerInfoOperation`) and resolves the real user id before creating the account.

Because the result is an *app password*, two-factor authentication and OIDC/SSO logins on the server side are handled
entirely in the browser; the client never sees a second factor and never stores the user's main password.

**Other login paths in the code.**
- A QR/deep-link login with a custom scheme whose payload has the form `server:...&user:...&password:...`
  (`DeepLinkLoginActivity`, `LoginUrlInfo`), the Login Flow v1 result format.
- A web-view login for branded builds that configure a fixed login URL.
- A manual path with user name and password, which is the same account-creation code with a user-entered value.
- "Single Sign-On" in this app means the *provider* side: other Nextcloud apps (Notes, Talk) can ask the Files app
  for the account through a permission dialog (`SsoGrantPermissionActivity`). It is not a login method of this app.

**Credentials failure at run time.** A 401 anywhere makes the screen open a credentials-update activity that runs the
login flow again for the same account (`requestCredentialsUpdate`). An upload that fails this way is stored as a
"credential error" and its notification leads to the same activity; that result is *not* on the list of
non-retryable upload results, so the queue keeps it for another try.

**Server version gate.** The code defines the minimum supported server as Nextcloud 20; below it the account setup
fails with a bad-version error. Capabilities are refreshed whenever the root folder is refreshed (section 4.4).

---

## 4. Local metadata database and folder refresh

### 4.1 Tables (schema 106)

Entity names are the table names. Only the file-related ones matter.

| Table | Content |
|-------|---------|
| `filelist` | one row per known remote file or folder: name, remote path, parent row, size, mtime, mime type, **etag**, **etag_on_server**, **etag_in_conflict**, `remote_id` (the server's file id string), `local_id` (numeric file id), permissions string, favorite, hidden, shared flags, owner, locked fields (lock state, type, owner, token, timeout), tags, local storage path, last sync date for properties, last sync date for data, **modified_at_last_sync_for_data**, thumbnail flag, read-only flag, two-way-sync timestamp and result, the encrypted-folder columns |
| `capabilities` | one row per account: server version, sharing flags, chunking flags and `chunked_upload_max_size`, versioning, undelete, locking version, forbidden file name/character/extension/base-name lists (JSON), server branding, and many app-specific flags |
| `ocshares` | cached shares per file |
| `list_of_uploads` | the upload queue: local path, remote path, status, local behavior, name-collision policy, wifi/charging flags, last result, size |
| `offline_operations` | queued changes made while offline: parent file row, path, operation type (create folder, create file, rename, remove), file name, created/modified time |
| `arbitrary_data` | per-account key/value store (settings, flags) |
| `filesystem`, `synced_folders` | the media auto-upload bookkeeping (out of scope) |
| `virtual` | file rows that belong to virtual lists (favorites, gallery) |

Two ETag columns exist on purpose: `etag` is the ETag *of the content the app last synchronized* for a file, while
`etag_on_server` is what the last listing said. For folders the folder's own `etag` is the signal for "something
below me changed".

### 4.2 Folder refresh (the central algorithm)

`RefreshFolderOperation`, started whenever a folder is opened, on pull-to-refresh, and when the ETag poll finds a change:

1. **Cheap check.** Unless the caller forces a full read, the app reads the folder's own properties (a depth-0
   PROPFIND, via `ReadFileRemoteOperation` **(inferred depth)**). If the folder is gone (404) the local folder and
   its subtree are removed from the database and the downloaded files below it are deleted.
2. **Compare ETags.** The remote folder ETag is compared with the stored one, case-insensitively. A missing or empty
   ETag on either side counts as "changed". If it did not change, the children come straight from the database;
   no listing request is made.
3. **Listing.** Otherwise the folder is listed (`ReadFolderRemoteOperation`; the result list has the folder itself at
   index 0 and its direct children after it, so this is a depth-1 listing **(inferred)**). The app never walks a
   whole tree with one request.
4. **Merge.** For each remote child the app builds a fresh row and merges the *local-only state* into it: row id,
   local storage path, data-sync dates, thumbnail info, share flags. A child that is not in the database gets an
   empty content ETag (it has never been downloaded).
5. **Content ETag rule.** The row keeps the *old* content ETag as long as the file's content has not been
   downloaded again. If the file is downloaded locally and the server ETag differs from the stored one, the
   server's ETag is recorded in `etag_in_conflict`. That field is what later makes an upload conditional.
6. **Save in one batch.** The new children are inserted or updated and every previously known child that was *not*
   in the listing is deleted (the row, and the local file if it was downloaded) in one content-provider batch,
   together with the folder row. (The code does not look for unsynced local edits of a removed file at this
   point **(unverified: the check may live elsewhere)**.)
7. **Failure.** If the listing fails, the folder's stored ETag is cleared so the next refresh does a full read.
8. **Side effects.** For the root folder, the refresh also updates server version, capabilities and the user profile
   *after* the listing, so they never delay the file list. Shares are read from the same listing's properties,
   because share changes do not change the folder ETag.

A refresh can finish with a "conflict" result code when kept-in-sync files below the folder could not be reconciled.

### 4.3 Per-file reconciliation (`SynchronizeFileOperation`)

Used by the periodic jobs, the "sync" menu action, and the conflict screens.

- A file that has never been downloaded is simply downloaded when content sync is requested.
- Otherwise the app asks whether the server side changed (the stored content ETag differs from the server's; if no
  ETag is stored, the server mtime is compared with the mtime recorded at the last data sync) and whether the local
  side changed (the local file's modification time is newer than the last data sync).
- Both changed: a **conflict** is recorded (the server's ETag is stored in `etag_in_conflict`) and a conflict result
  is returned; nothing is overwritten.
- Only local changed: upload with the "overwrite" policy (if uploads are allowed for this call).
- Only server changed: download.
- Neither: nothing. A conflict marker is cleared whenever the result is anything but a conflict.
- If the file is gone on the server (404), the local copy and row are removed.

### 4.4 Capabilities and server information

The root refresh calls the status document and the capabilities endpoint and stores the answer in `capabilities`.
Everything optional (versioning, locking, chunk size, forbidden names, direct editing) is read from that row at run
time; the code gates features by capability flags and by server version only where a flag does not exist (for
example the Windows-compatible-names rule: always on for 30 and 31, flag-controlled from 32, off before 30).

### 4.5 Offline operations queue

Changes made without a connection (create folder, create file, rename, remove) are stored in `offline_operations`
and shown immediately in the list marked as pending. A worker that runs every five minutes, and is also started by
several screens, processes the queue in order:

- *Create folder* when the folder already exists on the server: adopt the server's folder and drop the operation.
- *Rename/remove/create* when the server's file differs from the cached one: **conflict**. A rename or remove
  conflict is dropped with a notification; a create conflict opens the same conflict dialog as an upload.
- Remove or rename of something that no longer exists locally: dropped silently.
- A create that finds an identical file already there: dropped silently.
- Errors "folder already exists" and "locked" are not reported to the user; other errors produce a notification.
- Later queued operations that depended on a parent are updated after the parent succeeds.

### 4.6 File locking

Locks are server-side (the files_lock app); the client shows the owner and offers lock/unlock. Lock state is stored
in the file row and refreshed with the normal listing. A locked target makes an upload or edit fail with the
"locked" result code, which the workers treat as a *general* (not file-specific) error: they stop and retry later
instead of failing the whole queue. The app does not take locks by itself during uploads **(inferred; locking in
this app is a user action)**.

### 4.7 Schedules, in one place

| Job | Interval / trigger | Conditions |
|-----|--------------------|------------|
| ETag poll of the open folder | every 30 s while the folder screen is visible | foreground only |
| Offline operations worker | every 5 min, and started from several screens (list, details, conflict decisions) and from the application class **(the exact app-start trigger is inferred)** | needs network and a reachable server |
| Periodic downloaded-files sync | default periodic interval of 15 min (the platform's minimum) | unmetered network; the job body does nothing in power-save mode |
| Internal two-way sync | periodic | Wi-Fi, setting on, not in power-save mode, free space |
| Health-status job | every 24 h | only when the server advertises a "security guard" capability; it collects upload and conflict problems for a report (not a capabilities refresh) |
| Login flow poll | every 30 s while logging in | foreground |

---

## 5. Download and upload mechanics

### 5.1 Download

- The transfer goes to a temporary file in a per-account temp area, then the file is *moved* over the final path
  with replace semantics; a failed move gives a distinct "could not move into place" result. (The partial file is
  never visible under the final name.) **No resume of a half-finished download was found** in the app code; the
  library may offer ranged requests **(unverified)**.
- The ETag and modification time of the downloaded content come from the transfer operation, i.e. from the
  response of the GET (`modificationTimestamp`, `etag` of the library operation **(inferred: headers of the response)**).
- After a successful download the row is updated: content ETag, mtime, the "last data sync" date, size from the real
  local file, mime type, storage path; then the conflict marker is cleared. The local file's own modification time is
  set to the server's modification time.
- A file name that is not valid on the local file system is refused before the request (an invalid-name result).
- Free-space checks exist for the two-way-sync worker (remote size minus local size against free space), not for
  single downloads **(inferred)**.
- A folder cannot be downloaded as one file; a folder worker downloads its files one by one.

### 5.2 Upload

1. **Preconditions.** Wi-Fi-only and charging-only flags, power-save mode, existence of the local file, and a "walled
   garden" connectivity check; each failing precondition becomes a *delayed* result, and the transfer is retried
   later. Retries inside the upload worker back off linearly by about 3.5 s per failure, capped at 120 s, with a
   small random jitter.
2. **Name collision.** Before sending, the client asks the server whether the target path exists. Policy decides:
   skip (report OK), rename (find the next free name such as "name (2)"), overwrite, or ask the user. For "ask" the
   client first checks whether the remote file is in fact identical to the local one and treats that as success;
   otherwise it reports a sync conflict and a notification opens the conflict dialog.
3. **Source copy.** Depending on the "local behavior", the file is copied or moved into the app's storage before
   sending. If it cannot be read through a normal path (for example a removable card) it is copied to a temp file
   first, and a length mismatch aborts with a lock-failed result.
4. **Read lock.** The app takes a shared lock on the local file while reading it, and keeps going if another reader
   already holds one.
5. **Size decides the transport.** If the file is larger than the chunk size for the current network (the library
   decides; Wi-Fi gets larger chunks; the server's maximum chunk size from the capabilities row bounds it), a
   *chunked upload operation* is used; otherwise a plain single-request upload operation. Both receive the local
   file's last-modification time in **seconds** (and the creation time) so that the server can store the same
   mtime; the library sets the corresponding request headers **(inferred: `X-OC-Mtime` / `X-OC-CTime`)**.
   Checksum headers are not set anywhere in the app code; if the library sends `OC-Checksum` it does so internally
   **(unverified)**.
6. **Conditional overwrite.** Both transports get the file's `etag_in_conflict` value. The only way a stale write is
   stopped is the server's precondition answer: a **412 Precondition Failed** is translated into the sync-conflict
   result, and the conflict is stored on the file row (the library presumably sends the value as `If-Match`
   **(inferred)**).
7. **After success.** The local behavior step runs (delete, move or keep the source), the upload row is marked
   done, and a follow-up refresh updates ETag and metadata.
8. **Chunk resume.** Chunk bookkeeping (the upload folder id, the "resume" case for end-to-end encrypted uploads)
   exists; whether a plain interrupted chunked upload resumes from the last good chunk is **(unverified)**; it is
   implemented in the library.

### 5.3 Conflict dialog

Reached from the uploads list, a notification or the file's menu. It shows the local and the server version side by
side (size, date, thumbnail) and offers three outcomes:

- **Keep local:** upload again with the overwrite policy.
- **Keep server:** discard the queued upload and download the server's version over the local file.
- **Keep both:** upload with the rename policy, so the server keeps the old file and receives the new one under a
  free name.

For offline operations the dialog has the analogous outcomes (keep the queued change, keep the server's item, keep
both). There is no merge, no diff, and no automatic resolution.

---

## 6. Error handling

The library maps HTTP status and exceptions to one `ResultCode` enum; the app maps those codes to messages, to
retry decisions and to follow-up screens. The list of upload outcomes persisted in the database is: uploaded,
no network, credential error, folder error, conflict, file error, privileges error, cancelled, file not found,
delayed for Wi-Fi, service interrupted, delayed for charging, maintenance mode, lock failed, delayed in power-save
mode, untrusted certificate, virus detected, local storage full, sync conflict, cannot create file, local copy
failed, quota exceeded, same-file conflict, skipped.

| Situation | Observed handling |
|-----------|-------------------|
| **Maintenance mode** | A dedicated result code. The file list screen shows an info box; the account-setup screen shows a maintenance message; uploads store a "maintenance" result and are retried. The code that turns the HTTP answer into this result is in the library (the server answers 503 plus a maintenance flag in the status document **(inferred)**). |
| **401 Unauthorized / account exception** | Open the credentials-update activity (login flow) and show a message. An upload is recorded as a credential error and stays retryable. |
| **403 Forbidden** | Mapped to a "forbidden" result; messages differ for copy, move, rename, create and upload. A forbidden-character-in-name variant exists for servers that reject names. |
| **404 Not found** | During a folder refresh: delete the local folder subtree. During a file sync: delete the local copy. During an upload's parent check: treated as "folder missing", the app can create the parent. |
| **412 Precondition Failed** | On upload: becomes the sync-conflict result and the conflict dialog (section 5.3). |
| **423 Locked** | Result "locked". Message says the file is locked; workers treat it as non-file-specific, so the rest of the queue waits; the offline-operation worker silently ignores it. |
| **507 / quota** | A quota-exceeded result with its own message; classified as a general error (the rest of the queue will fail too) and as non-retryable for uploads. |
| **Other 5xx, timeouts, no host** | Wrong-connection, timeout and host-not-available results with messages; uploads retry with backoff. |
| **Redirects** | Handled in the library and in the account-setup URL normalization; the app itself has no redirect logic besides storing the corrected base URL **(unverified)**. The setup code strips the DAV path from user input. |
| **Untrusted certificate** | A recoverable SSL result code opens a dialog with the certificate details; the user may accept it, and the certificate is then remembered in a per-app trust store. A non-recoverable SSL error shows a plain message. The file list and the upload paths each have their own call into this dialog. |
| **Terms of service** | A dedicated result code opens a terms dialog (server-side "terms of service" app). |
| **Out of memory** | A distinct result code with its own empty-state. |
| **Name rules** | The client validates names before sending, using the server's capability lists of forbidden names, characters, extensions and base names, plus the Windows-compatible-names rule (section 4.4), and strips non-printable Unicode when any list is present. |

Errors are classified as *file-specific* (the next item may still work) or *general* (credentials, quota, locked,
maintenance, untrusted domain, wrong account, bad server version); a general error stops a batch. A second,
separate classification decides whether an upload is *retried*: these upload results are final (file not found,
file error, cannot create file, the two conflict results, same-file conflict, local copy failed, virus detected,
quota exceeded, privileges error, user cancellation); every other result, including unknown, stays retryable.

---

## 7. Feature → fit for an e-ink reader with no background activity

Context: the target device has slow, costly screen refreshes, a small input surface, no reliable background
execution (the app runs only while it is open, plus whatever the platform allows), and Wi-Fi that is often off.
Fit: **yes** = take it as is, **adapted** = take the idea with changes, **no** = leave out.

| Feature | Fit | Why / how |
|---------|-----|-----------|
| Browse folders with cached listing | yes | Cached rows first, refresh on open. Paged list, no animation. |
| ETag-compare refresh, depth-1 listing | yes | The cheapest correct pattern; one small request per open folder. |
| 30-second ETag poll while open | no | Needless radio use and screen redraws. Refresh on open and on an explicit button. |
| Sort (name, date, size) and "folders first" | yes | Cheap and local. Drop "favorites first" unless favorites are cheap to show. |
| Grid view | no | Thumbnails cost memory and refreshes; a list is enough. |
| Thumbnails/previews | adapted | Only for images, small and gray, on demand; optional. |
| In-folder search by name | adapted | Local filter on the cached folder; server search only on request. |
| Unified search (providers) | no | Not a file function. |
| Favorites list and toggle | adapted | Toggle needs a network write; queue it. The list can be the cached favorites plus one server query. |
| Recents | adapted | One server query (modified in the last N days); useful for a reader. |
| Offline: keep downloaded copies | yes | The core of an e-reader use. |
| Periodic refresh of downloaded files (background job) | adapted | Run the same reconciliation on app start / on user request ("Sync now"), not in the background. |
| Internal two-way sync folders | adapted | Pull direction yes (a "keep this folder on the reader" mark); push of new local files only on explicit sync; deletion propagation off by default. |
| Offline operations queue (create folder, rename, remove, create file) | adapted | Keep for rename/remove/create-folder; apply on the next sync, with the same conflict rules; no notifications, show a list instead. |
| Download a file | yes | Temp file then rename; show progress by byte count, not animation. |
| Resume of a partial download | adapted | Use a ranged GET when the temp file exists and the ETag is unchanged (RFC 7233); the Android app has no equivalent. **(unverified)** |
| Open in the built-in viewer | adapted | The reader's own viewers (epub/pdf/txt) are better than an in-app one; "open with" is a launcher call on the device. |
| Upload from the device | adapted | Pick a file from the reader's storage; no camera, no scanner. |
| Share-sheet receiver | no | No inter-app sharing on the device. |
| Chunked upload | adapted | Needed for large files on weak Wi-Fi; implement the v2 protocol after the plain PUT works. |
| Wi-Fi-only / charging-only / power-save conditions | adapted | "Only on Wi-Fi" is meaningful; charging and power-save are not. Ask the device whether Wi-Fi is up. |
| Name-collision policy dialog | adapted | Keep "overwrite / keep both / skip" but as one page, not a notification. |
| Conflict dialog (keep local / server / both) | yes | Same three outcomes; a text page with sizes and dates (no thumbnails). Never overwrite silently. |
| Create folder | yes | |
| Rename, move, copy | yes | One picker page; refuse move into itself and rename onto an existing name, as the Android app does. |
| Delete | yes | Confirm; show that the server may keep a trash copy. |
| Trash bin | adapted | List, restore, delete permanently, empty; low priority, one page. |
| Versions: list and restore | adapted | Read-only list with a restore button; low priority. |
| Sharing: create a public link | adapted | Show/copy-by-reading the link text; password and expiry; no sharee search, no federation, no teams. |
| Sharing: user/group shares, federation | no | Heavy, rarely needed on a reader. |
| Activity feed | no | Not useful offline; maybe later as a read-only page. |
| Details sidebar (size, dates, owner, lock state, ETag) | adapted | One plain details page; no tabs. |
| File locking | adapted | Display lock state; do not lock automatically. A locked target is a "try later" result. |
| Pin to home screen, wallpaper | no | Android only. |
| Login Flow v2 | adapted | The browser step needs a second device or the reader's browser; see the API notes for a QR/URL-display approach and a manual app-password entry fallback. |
| App passwords, 2FA, SSO on the server | yes | All of them work through Login Flow v2 / an app password; the reader only stores the app password. |
| Untrusted-certificate dialog | adapted | Show details; this project's constitution forbids skipping verification, so the only accepted route is installing the authority as `custom-ca.pem`. |
| Maintenance mode info | yes | A clear message and no retry loop. |
| Quota exceeded message | yes | |
| Forbidden-names capability validation | yes | Validate names locally before sending; cheap and avoids round trips. |

### 7.1 Explicitly leave out

- **Talk** (chat and calls), and the Talk share type.
- **Auto-upload of photos and videos** (synced folders, content observers), the media gallery and albums.
- **End-to-end encrypted folders** and their metadata handling.
- **Rich document and text editors** (Collabora/OnlyOffice direct editing, templates, web-view editors) and the
  document scan feature.
- **Media player and streaming** inside the app.
- **Push notifications** and the notification-polling worker; notifications in general.
- **Home-screen widgets** and the dashboard widget.
- **Multiple accounts** (one account only; the data model can still carry an account column).
- Also out: the assistant, the contacts and calendar backup/import, user status, external links, the app lock and
  screenshot blocking, the ecosystem/community banners, the logs/ETM developer screens.

---

## 8. What to take from the design (lessons for a PocketBook client)

1. **Two ETags per file** (content ETag at last sync, ETag seen on the server) plus the content ETag stored at the
   moment of download give a clean three-way comparison without hashing: base (stored), local (mtime newer than last
   sync), remote (listing ETag). Reuse it.
2. **Folder ETag as the only change signal for a tree.** Never walk a subtree unless the folder's ETag differs; this
   keeps requests per open to one. (Relies on the server updating ancestor ETags on every change below; documented
   server behavior, **(unverified)** until run against a server.)
3. **Merge fresh server data with local-only state** instead of replacing rows; delete only rows that were *not* in
   a successful listing.
4. **Conditional writes.** The client stores the ETag it saw when the conflict arose and sends it back with the next
   write; a 412 is the only safety net. A PocketBook client should send `If-Match` on every overwrite and delete of
   a file it has a stored ETag for, not only after a conflict was noticed.
5. **Offline queue with conflict rules per operation type** is small and worth copying; drop the notifications.
6. **Failure classes** (general vs file-specific) decide whether to continue a batch. Keep the distinction.
7. **Name validation from capabilities** avoids pointless uploads.
8. **What the Android app lacks** and a reader would want: a visible per-folder "last synced" line, a manual "sync
   now" for everything marked offline, ranged-GET resume of downloads, and an explicit "pending changes" list.

---

## 9. Open points not verified

- Exact request forms of every WebDAV call (PROPFIND body and depth, SEARCH or REPORT for search/favorites/recents,
  PROPPATCH for favorites, chunk handling, headers `X-OC-Mtime`, `OC-Checksum`, `If-Match`, `Destination`): in the
  separate library.
- Whether a failed chunked upload resumes, and what the chunk size is on Wi-Fi vs mobile data.
- Whether a download can be resumed.
- How a redirect from `http` to `https` or to another path is handled below the app level.
- The exact HTTP answer for maintenance mode (503 and flags) and the shape of the rate-limit answer (429) in the
  library's mapping; the app reacts to result codes, not to status numbers.
- Whether removal of a file during a listing refresh checks for unsynced local edits.
- The ETag semantics of ancestors after a deep change, and whether the server ever sends weak ETags to this client.
- Behavior of share, versions and trash operations beyond the calls named above; only skimmed.
- All of the above should be confirmed against a real server with `docs/live-server-checklist.md`.
