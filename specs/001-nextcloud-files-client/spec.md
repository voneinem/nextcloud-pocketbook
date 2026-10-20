# Feature Specification: Read-only Nextcloud Files Client

**Feature Branch**: `feature/001-nextcloud-files-client`

**Created**: 2026-10-19

**Status**: Draft

**Input**: User description: "A read-only Nextcloud files client for the PocketBook InkPad One. The user connects to their own Nextcloud server, browses their files and folders page by page, downloads files on demand, keeps chosen files offline and opens them in the reader. It looks and behaves like my other apps PB Notes and PB Office (built-in look, e-ink friendly, offline first, no background network activity). The Android app (docs/reference/nextcloud-android-features.md) is the behavior reference; keep only what makes sense on an e-ink reader.

Stories, in priority order:

P1 Connect to my Nextcloud. I enter the server address, user name and an app password, and a Test button tells me whether it works and shows the server's name, version and my quota. As an alternative I can start a login that shows me a link (and a short code) to open on my phone or laptop, then tap \"Check\" on the reader to finish; no password is typed on the reader. Only https is accepted. Clear, actionable messages for: wrong credentials, two-factor authentication needing an app password, server in maintenance mode, server too old or without WebDAV, address that is not a Nextcloud, untrusted certificate (explain the custom-ca.pem option), and a wrong reader clock. The app never stores the account password, and tells me honestly how the app password is stored.

P1 Browse my files. A list of the folders and files in the server's root, folders first, with name, size and modified date, page by page with page buttons (no kinetic scrolling), sort by name, date or size, a path line showing where I am and a way to go up. Opening a folder fetches its content on request; folders I visited stay readable without a connection, marked with the age of the listing. Refresh is explicit. Entries that are not available to me (no permission, encrypted folders, shares in a state the app does not support) are shown as such and do not crash anything.

P1 Download and open a file. Tapping a file shows its details (size, type, modified date, whether it is on the reader and whether the server version is newer) and offers Download. The download shows progress and can be cancelled; if the connection drops it resumes or restarts cleanly; no half-downloaded file ever shows up in the reader's library. The app checks free space first, warns about very large files and refuses files the storage cannot hold (including the FAT file-size limit). Downloaded books and documents land in a folder the reader's own library finds, and I can open a downloaded file in the reader's viewer with one tap. A local copy that I have changed is never overwritten without asking.

P2 Keep files offline and update them. I can mark files or whole folders as \"keep offline\". An Update action (never automatic) checks them against the server and downloads only what changed; the list shows per entry whether it is not downloaded, downloaded, outdated (changed on the server) or changed on the reader.

P2 Favorites and search. I can see the files I starred in Nextcloud and I can search by name, offline in what the reader knows and, when connected, on the server.

P3 Manage space on the reader. A screen lists what the app downloaded with sizes, lets me remove local copies (the server is never touched), shows free space, and bounds the metadata cache.

Out of scope for this feature (separate features later): uploading, creating folders, renaming, moving, deleting on the server, sharing, trash, versions, folder synchronization, several accounts, end-to-end encrypted folders, Talk, photos auto-upload, editing documents, notifications.

Cross-cutting requirements: offline first; everything that depends on the server shows its state (\"not synced\", age of the listing, last refresh time); network activity only on app start (when Wi-Fi is already on), on explicit user action; English interface with a German translation; nothing the user typed or downloaded is lost or silently overwritten; names from the server are never trusted as local paths. Success criteria should be measurable: for example a cached folder of 1,000 entries is shown within a stated time and navigable page by page, a download interrupted by a Wi-Fi drop completes after reconnecting without corrupt files, and budgets for RAM, storage and cold start are stated (unverified until measured on the device)."

## Clarifications

### Session 2026-10-19

- Q: What may the app do over the network when it starts with Wi-Fi already on? → A: One account check only (confirm the login works, show the server state); no listing is fetched or refreshed at start.
- Q: When the user searches on the server, should it cover the whole account or only the current folder? → A: The whole account.
- Q: Login Flow v2 offers no short code; should the browser login show a link and a QR code instead? → A: Yes; the reader shows the link and a QR code of the same link (decided by the owner after analysis).
- Q: When a folder is marked "keep offline", does that include its subfolders and files added on the server later? → A: Yes; subfolders are included and Update also downloads newly added files.
- Q: Should downloaded files be stored in a tree mirroring the server's folders or in one flat folder? → A: A mirrored tree under one app folder, with every name made safe for the reader's storage.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Connect to my Nextcloud (Priority: P1)

The owner of a Nextcloud server connects the reader to their own account so that the other stories can work. They can connect in two ways. In the first, they type the server address, their user name and an app password into a settings screen and tap Test. In the second, they start a browser login: the reader shows a link and a QR code, the user opens the link or scans the code on a phone or laptop, approves the access there, and then taps Check on the reader. No password is typed on the reader in the second way. Only encrypted (https) server addresses are accepted. When something is wrong the app says what, in plain words, and what the user can do about it. The app never stores the account password, and it tells the user honestly how and where the app password is kept on the reader.

**Why this priority**: Nothing else works without a working connection. It is also the part where a mistake is most costly, because it handles a credential for a personal cloud.

**Independent Test**: Against a simulated server, enter an address, user name and app password, tap Test and read the result; then start the browser login, approve it on the simulated server, tap Check and see the account connected. Delivers a verified connection and a clear statement of the server's name, version and the user's quota.

**Acceptance Scenarios**:

1. **Given** valid server address, user name and app password, **When** the user taps Test, **Then** the app reports success and shows the server's name, its version and the user's quota (used and total, or "unlimited").
2. **Given** the user has no app password, **When** they start the browser login, **Then** the app shows a link and a QR code of the same link, and tapping Check before approval says that the login is not approved yet without discarding the pending login.
3. **Given** the user approved the login on another device, **When** they tap Check on the reader, **Then** the app stores the connection, shows the account as connected and no longer holds the temporary login token.
4. **Given** an address that begins with `http://` or has no scheme that can be completed to https, **When** the user tries to save or test it, **Then** the app refuses it and explains that only https is accepted; it never retries over plain http.
5. **Given** wrong credentials, **When** the user taps Test, **Then** the app says the user name or app password is wrong.
6. **Given** an account with two-factor authentication where the user typed the account password, **When** they tap Test, **Then** the app says an app password is needed and explains how to create one.
7. **Given** a server in maintenance mode, a server that is too old, a server without file access, or an address that is not a Nextcloud, **When** the user taps Test, **Then** each case gets its own message that names the problem and a next step.
8. **Given** a server whose certificate is not trusted by the reader, **When** the user taps Test, **Then** the app refuses the connection, explains that a `custom-ca.pem` file can be added for a private certificate authority, and shows in Settings whether such a file is loaded. There is no option to ignore the certificate problem.
9. **Given** a reader whose clock is far from the real time, **When** a connection fails because of it, **Then** the app says the reader's date and time are probably wrong instead of reporting a generic certificate error.
10. **Given** a connected account, **When** the user opens Settings or About, **Then** they read a plain statement of how the app password is stored on the reader, including that the storage cannot make the file private if that is the case.

---

### User Story 2 - Browse my files (Priority: P1)

The user looks through their Nextcloud files on the reader. The first screen after connecting shows the server's root folder. Folders come first, then files; each row shows the name, the size (for files) and the modified date. The list is split into pages with page buttons; there is no kinetic scrolling. The user can sort by name, date or size. A path line shows the current folder and the user can go up one level. Opening a folder fetches its content from the server only because the user asked for it. A folder that was visited before can be opened without a connection and is marked with the age of its listing. Refresh is a separate, explicit action. Entries the user cannot use (no permission, encrypted folders, shares in an unsupported state) are shown and marked as unavailable.

**Why this priority**: Browsing is the core of a files client and the entry point to downloads.

**Independent Test**: Against a simulated server with a tree of folders and files, open the root, open a subfolder, go up, change the sort, turn Wi-Fi off, and reopen a visited folder from the cache. Delivers a navigable, honest view of the user's files.

**Acceptance Scenarios**:

1. **Given** a connected account and a reachable server, **When** the user opens the app's file view, **Then** the root folder's content is listed with folders first and then files, each with name, size and modified date.
2. **Given** a folder with more entries than fit on a page, **When** the user taps the page buttons, **Then** the list moves one page at a time and shows which page of how many is displayed.
3. **Given** a listing, **When** the user chooses sort by name, date or size, **Then** the order changes accordingly, folders remain before files, and the choice is kept for the next folder.
4. **Given** the user is in a nested folder, **When** they tap up, **Then** the parent folder is shown, and the path line always shows where they are.
5. **Given** a folder never visited, **When** the user opens it, **Then** its content is fetched once and stored for later.
6. **Given** a folder visited earlier and no connection, **When** the user opens it, **Then** the stored listing is shown, marked with the age of the listing ("not synced" if it was never refreshed since a change is known), and no error blocks the view.
7. **Given** a shown listing, **When** the user taps Refresh, **Then** the listing is fetched again and the shown age is reset; without that tap, an already shown listing is not refreshed.
8. **Given** a listing that contains an entry the user may not read, an encrypted folder, or a share in an unsupported state, **When** the listing is shown, **Then** that entry appears marked as unavailable with a reason, cannot be opened, and the rest of the list works.
9. **Given** a folder that is deleted on the server since it was cached, **When** the user refreshes its parent, **Then** the app shows that the folder is gone from the server and does not crash.

---

### User Story 3 - Download and open a file (Priority: P1)

The user taps a file and sees its details: size, type, modified date, whether a copy is on the reader, and whether the server's version is newer than that copy. The details offer Download. While the file downloads the app shows progress and offers Cancel. If the Wi-Fi drops, the download resumes after reconnecting or restarts cleanly, and a half-finished file is never visible to the reader's own library. Before downloading, the app checks free space, warns about very large files and refuses files the storage cannot hold, including files over the 4 GiB limit of the reader's FAT storage. Downloaded books and documents go to a folder the reader's library scans, and one tap opens the file in the reader's viewer. If the user has changed the local copy, a download never overwrites it without asking.

**Why this priority**: Getting a book or document from the cloud onto the reader and reading it is the reason the app exists.

**Independent Test**: Against a simulated server, download a small and a large file, cancel one, cut the connection during another and restore it, then open a finished download in the viewer. Delivers files on the reader that are complete or absent, never partial.

**Acceptance Scenarios**:

1. **Given** a file entry, **When** the user taps it, **Then** a details view shows size, type, modified date, whether a local copy exists and whether the server version is newer than it.
2. **Given** the details of a file that is not on the reader, **When** the user taps Download, **Then** the app checks free space, starts the download and shows progress that updates coarsely.
3. **Given** a running download, **When** the user taps Cancel, **Then** the download stops, the partial data is removed or kept out of sight, and the file does not appear in the reader's library.
4. **Given** a running download, **When** the connection drops and later returns, **Then** the download completes (resumed if the server version is unchanged, otherwise restarted) and the finished file is complete and intact.
5. **Given** a download that is interrupted, the app is closed or the reader loses power, **When** the user looks at the reader's library, **Then** no partial file is listed; on next start, leftover partial data is cleaned up or offered for resume.
6. **Given** free space smaller than the file, **When** the user taps Download, **Then** the app refuses and says how much is needed and how much is free.
7. **Given** a file larger than a set warning size, **When** the user taps Download, **Then** the app warns about the size and asks for confirmation before starting.
8. **Given** a file of 4 GiB or more, **When** the user taps Download, **Then** the app refuses because the reader's storage cannot hold it, and explains why.
9. **Given** a completed download of a book or document, **When** the user taps Open, **Then** the file opens in the reader's viewer.
10. **Given** a local copy the user has changed, **When** the user taps Download or Update, **Then** the app does not overwrite it and asks what to do; the user can keep the local version, keep both versions, or replace the local version after an explicit confirmation.
11. **Given** a server-supplied file name that contains path separators, `..`, control characters, characters not allowed on the FAT storage, an overlong name, or a name that differs from another only by case, **When** it is downloaded, **Then** the local file gets a safe, unique name inside the download folder and nothing is written outside it.

---

### User Story 4 - Keep files offline and update them (Priority: P2)

The user marks files or whole folders as "keep offline". The Update action, started by the user and never run automatically, checks the marked items against the server and downloads only those that changed. The list shows for each entry one of four states: not downloaded, downloaded, outdated (changed on the server), or changed on the reader.

**Why this priority**: It turns the app from a one-off fetcher into a way to have a reading list on the reader that can be brought up to date over Wi-Fi when the user chooses.

**Independent Test**: Mark one file and one folder as keep offline, change a file on the simulated server, tap Update and verify that only that file is downloaded, and that each entry's state is shown correctly.

**Acceptance Scenarios**:

1. **Given** a file or folder in the list, **When** the user marks it "keep offline", **Then** the mark is shown on the entry and persists across restarts.
2. **Given** items marked keep offline, **When** the user taps Update, **Then** the app compares each with the server, downloads only those whose server version differs from the downloaded one, and reports how many were checked, updated, skipped and failed.
3. **Given** no tap on Update, **When** time passes or the app starts, **Then** no marked item is checked or downloaded.
4. **Given** a marked file whose server version has changed, **When** its listing is shown, **Then** it is shown as outdated.
5. **Given** a marked file whose local copy the user changed on the reader, **When** Update would replace it, **Then** the app does not replace it, shows it as changed on the reader, and asks the user what to do.
6. **Given** a file that is changed on both the reader and the server, **When** the user resolves it, **Then** both versions are kept until the user decides, and neither is discarded automatically.
7. **Given** a marked file that no longer exists on the server, **When** the user taps Update, **Then** the local copy is kept, the entry is flagged as removed on the server, and the user decides what to do with it.
8. **Given** an Update interrupted by a Wi-Fi drop or app exit, **When** the user taps Update again, **Then** the result is the same as an uninterrupted Update, and no completed download is repeated needlessly.

---

### User Story 5 - Favorites and search (Priority: P2)

The user opens a list of the files they starred in Nextcloud. The user can search by name: offline across everything the reader already knows (cached listings and downloads), and, when connected and the user asks for it, across the server.

**Why this priority**: Finding a known book in a large tree by tapping through folders is slow on e-ink; favorites and search shorten the path.

**Independent Test**: With some favorites on the simulated server and a populated cache, open the favorites, search while offline and see results from the cache, then search while connected including the server and see more results.

**Acceptance Scenarios**:

1. **Given** a connected account, **When** the user opens Favorites, **Then** the app fetches and shows the starred entries; if it cannot connect, it shows the last stored list marked with its age.
2. **Given** a cache of visited folders, **When** the user searches while offline, **Then** matching entries from the cache are listed, marked as results from the reader's own knowledge, and no network access occurs.
3. **Given** a connection, **When** the user chooses to search the server too, **Then** matching entries from the server are added and marked as server results.
4. **Given** a search result or favorite, **When** the user taps it, **Then** it behaves like the same entry in a folder listing: files open their details, folders open their content.

---

### User Story 6 - Manage space on the reader (Priority: P3)

The user opens a screen that lists everything the app downloaded with its size, shows free space on the reader, and lets them remove local copies one by one. Removing a local copy never changes anything on the server. The app keeps the folder listing cache within a stated size limit.

**Why this priority**: The reader has limited storage; the user needs to reclaim it without risk to the cloud data.

**Independent Test**: Download several files, open the space screen, remove one local copy, and confirm it is gone from the reader and still on the simulated server, and that free space is updated.

**Acceptance Scenarios**:

1. **Given** downloaded files, **When** the user opens the space screen, **Then** each downloaded file is listed with its size, and the total used by the app and the free space are shown.
2. **Given** a downloaded file, **When** the user removes the local copy after confirming, **Then** the file is deleted from the reader and the server is not contacted.
3. **Given** a local copy that was changed on the reader, **When** the user removes it, **Then** the app warns that the changes will be lost and requires explicit confirmation.
4. **Given** the folder listing cache has reached its size limit, **When** another folder is visited, **Then** the oldest unused listings are dropped first, and no downloaded file, offline mark or local change is lost.

---

### Edge Cases

- The server's folder listing is extremely large (tens of thousands of entries), is truncated, is malformed, or has some entries that failed while others succeeded: the valid entries are shown, the bad ones are skipped or marked unreadable, and the app does not crash.
- The server sends file names with unusual Unicode (different normalization forms of the same name), very long names, control characters or names that collide on a case-insensitive storage.
- The server certificate expires, changes, or the server redirects to another host: the app refuses to send credentials to the other host and says so.
- The app password is revoked on the server while the app holds it: the next server action reports that the password is no longer valid and offers to reconnect; cached listings and downloads stay available.
- The reader sleeps, the screen turns off, or the battery dies during a download: the temporary data is never mistaken for a finished file.
- The storage is full, read-only, or the memory card is removed mid-download.
- The same file is downloaded twice, or two tapped downloads are queued at the same time.
- A file changes on the server between the details view and the end of the download.
- A server time zone or a wrong reader clock makes modified dates look wrong or in the future.
- The user starts the browser login and never approves it, or approves it after it expired.
- Two-factor authentication or a login policy that blocks app passwords is active.
- Nothing is cached and there is no connection: the app says so plainly and offers Settings and Retry.

## Requirements *(mandatory)*

### Functional Requirements

**Connection**

- **FR-001**: Users MUST be able to connect by entering a server address, a user name and an app password, and to test the connection before relying on it.
- **FR-002**: A successful test MUST show the server's name, its version, and the user's quota (used, total, or unlimited).
- **FR-003**: Users MUST be able to connect through a browser login that displays a link and a QR code of that link, which they approve on another device and then complete on the reader with a Check action, without typing any password on the reader.
- **FR-004**: The system MUST accept only https server addresses, MUST NOT fall back to plain http, and MUST NOT send credentials to a different host after a redirect.
- **FR-005**: The system MUST verify the server's certificate and host name for every connection, MUST offer no option to disable this, and MUST allow trust to be extended only through a `custom-ca.pem` file; Settings MUST show whether such a file is loaded.
- **FR-006**: The system MUST show a distinct, actionable message for each of: wrong credentials, two-factor authentication requiring an app password, server in maintenance mode, server too old, server without file access, an address that is not a Nextcloud, an untrusted certificate (with the `custom-ca.pem` explanation), and a reader clock that is wrong.
- **FR-007**: The system MUST NOT store the account password and MUST state honestly, in Settings and About, how and where the app password is stored, including any limit on protecting it on the reader's storage.
- **FR-008**: The system MUST discard the temporary browser-login token as soon as it is used, expires, or is cancelled, and MUST NOT write secrets (passwords, tokens), server addresses, user names or file names to logs or to error messages shown to the user.

**Browsing**

- **FR-009**: The system MUST list a folder's entries with folders first, then files, each showing name, size (files) and modified date, in pages with page buttons and without kinetic scrolling.
- **FR-010**: Users MUST be able to sort by name, date or size and MUST see a path line for the current folder and have a way to go up one level.
- **FR-011**: The system MUST fetch a folder's content from the server only when the user opens it or taps Refresh, MUST fetch only the one level shown, and MUST NOT refresh a shown listing on its own.
- **FR-012**: The system MUST store the listing of every visited folder, MUST show a stored listing when there is no connection, and MUST mark every stored listing with its age, using "not synced" where it has never been confirmed by the server.
- **FR-013**: The system MUST show entries that are unavailable to the user (no permission, encrypted folder, share in an unsupported state) marked as unavailable with a reason, MUST NOT allow them to be opened or downloaded, and MUST keep the rest of the list working.
- **FR-014**: The system MUST treat everything received from the server (listings, names, sizes, dates, headers, file content) as untrusted, MUST bound its size, and MUST skip or mark any malformed item without failing the whole operation.

**Download and open**

- **FR-015**: Tapping a file MUST show its details: size, type, modified date, whether a local copy exists, and whether the server version is newer than the local copy.
- **FR-016**: Users MUST be able to download a file, see coarse progress, and cancel the download.
- **FR-017**: A download MUST never leave a partial file visible to the reader's library; the file MUST appear under its final name only when it is complete and its size, and its checksum where the server provides one, match.
- **FR-018**: After a connection drop the system MUST resume the download if the server's version is unchanged, otherwise restart it cleanly; after an app exit or power loss it MUST NOT treat leftover partial data as a finished file.
- **FR-019**: Before starting, the system MUST check free space, MUST warn and ask for confirmation above a large-file warning size, and MUST refuse a file the storage cannot hold, including any file of 4 GiB or more.
- **FR-020**: Downloaded files MUST be placed in a folder that the reader's own library scans, in a folder tree under that folder that mirrors the server's folder tree (each folder name made safe as in FR-022), and Users MUST be able to open a downloaded file in the reader's viewer with one tap. The system MUST NOT execute downloaded files.
- **FR-021**: The system MUST NOT overwrite a local copy that the user changed, MUST ask what to do, and MUST keep both versions until the user decides when a file has changed on both the reader and the server.
- **FR-022**: The system MUST convert every server-supplied name to a safe local name: no path separators or `..`, no control characters or characters reserved on the reader's storage, a limited length, and a result that cannot collide with another file even on a case-insensitive storage.
- **FR-023**: Storage that is full or read-only MUST be reported to the user and never ignored.

**Offline and update**

- **FR-024**: Users MUST be able to mark files and whole folders as "keep offline" and to remove the mark; the mark MUST persist across restarts.
- **FR-025**: The system MUST provide an Update action that runs only when the user starts it, compares marked items with the server, downloads only items that changed, and reports how many were checked, updated, skipped and failed.
- **FR-026**: The system MUST show for every entry one of these states: not downloaded, downloaded, outdated (changed on the server), changed on the reader; and MUST show a conflict state when both sides changed and a "removed on the server" state when a downloaded file is gone from the server.
- **FR-027**: Repeating an interrupted Update MUST produce the same result as an uninterrupted one and MUST NOT download an already completed item again unnecessarily.
- **FR-028**: A file or folder removed on the server MUST NOT cause deletion of its local copy; the user decides.

**Favorites and search**

- **FR-029**: Users MUST be able to view the entries they starred in Nextcloud, with the last stored list shown (and marked with its age) when there is no connection.
- **FR-030**: Users MUST be able to search by name offline within what the reader knows (stored listings, favorites and downloads), and, when connected and only when they ask, on the server as well; results MUST show their source.

**Space management**

- **FR-031**: The system MUST list what it downloaded with sizes, show free space, and let the user remove a local copy; removing a local copy MUST NOT contact or change the server, and MUST require explicit confirmation if the copy has local changes.
- **FR-032**: The system MUST keep the stored folder listings within a stated size bound, dropping the oldest unused listings first, and MUST NOT drop downloaded files, offline marks or unconfirmed local changes to do so.

**Cross-cutting**

- **FR-033**: The system MUST be read-only toward the server: it MUST NOT upload, create, rename, move, delete or share anything.
- **FR-034**: The system MUST perform network activity only when the app starts (and only when Wi-Fi is already on) and on explicit user action, and MUST NOT poll or synchronize in the background. The only network action at start MUST be one account check that confirms the login still works and shows the server state; the app MUST NOT fetch or refresh any listing at start.
- **FR-035**: Everything that depends on the server MUST show its state: offline or online, the age of a listing, and the time of the last refresh.
- **FR-036**: The interface MUST be in English with a German translation, and MUST look and behave like the other apps PB Notes and PB Office: black-and-white high contrast, large touch targets, no animations, icon-and-label tools in a bottom panel, the same home, settings and About structure.
- **FR-037**: The system MUST NOT lose or silently overwrite anything the user typed or downloaded, whatever happens to the connection, the reader's power, or the server.
- **FR-038**: Searching on the server MUST cover the whole account, whichever folder the user is in when starting the search.
- **FR-039**: Marking a folder as "keep offline" MUST cover all files in the folder and in its subfolders, and Update MUST also download files newly added to a kept folder on the server, in addition to updating changed ones; the free-space and large-file checks of FR-019 apply to each file, and nothing is downloaded without the user starting Update.

### Key Entities

- **Account**: the single connection to the user's own server: address, user name, the app password (never the account password), the connection state and, once tested, server name, version and quota.
- **Remote entry**: a file or folder on the server as last seen: name, kind, size, modified date, a version marker from the server, whether it is a favorite, and its availability (usable, no permission, encrypted, unsupported share).
- **Folder listing**: the stored content of one visited folder and the time it was last confirmed by the server.
- **Local copy**: a file on the reader that came from a remote entry: its safe local name, size, the server version it was downloaded from, and whether the user has changed it since.
- **Offline mark**: the user's choice to keep a file or folder available offline and up to date on request.
- **Download**: one transfer of a remote file to the reader: progress, temporary state and outcome (completed, cancelled, failed, resumable).
- **Entry state**: not downloaded, downloaded, outdated, changed on the reader, conflict, or removed on the server.
- **Search result**: an entry found by name, with its source (reader or server).

## Success Criteria *(mandatory)*

All time and size figures below are budgets and are unverified until measured on the InkPad One and, where a real server matters, against a real Nextcloud server (see Assumptions).

### Measurable Outcomes

- **SC-001**: A user who has a server address and an app password can connect and see the result of the Test (server name, version, quota) within 60 seconds of opening Settings, on a working network.
- **SC-002**: A user can complete the browser login (start, approve on another device, Check) without typing any password on the reader, and the temporary login token is gone from the reader's storage after use in 100% of tested runs.
- **SC-003**: Each of the eight named connection problems (wrong credentials, two-factor, maintenance, too old, no file access, not a Nextcloud, untrusted certificate, wrong clock) produces its own message in 100% of the simulated cases, and no case can be bypassed by an option that turns off certificate checks or allows plain http.
- **SC-004**: A stored folder with 1,000 entries is shown within 2 seconds of opening it, without a network, and every page of it is reachable using the page buttons only; a page turn completes within 1 second.
- **SC-005**: The first page of a not-yet-visited folder of 1,000 entries appears within 10 seconds of opening it over a typical home Wi-Fi connection.
- **SC-006**: In 100% of tested interruptions (Wi-Fi drop, cancel, app kill, simulated power loss) during a download, no partial file is visible to the reader's library, and a download interrupted by a Wi-Fi drop completes after reconnecting with a file identical to the server's.
- **SC-007**: A file of 500 MB can be downloaded without the app's memory use growing with the file size; peak memory use of the app stays within 150 MB during any download and while browsing a 1,000-entry folder.
- **SC-008**: A refused download (not enough space, 4 GiB or more, storage read-only) is refused before any data is written in 100% of tested cases and the message states the reason.
- **SC-009**: In 100% of tested cases a local copy changed by the user is not overwritten by a download or Update without the user's explicit choice.
- **SC-010**: With no tap on Update, Refresh or Search-on-server, and with Wi-Fi on, the app makes no network request other than the single start-up account check defined in FR-034, in 100% of tested sessions; with Wi-Fi off it makes none.
- **SC-011**: After marking 20 files as keep offline and changing 3 of them on the server, one Update downloads exactly those 3 and reports 20 checked, 3 updated, in 100% of tested runs.
- **SC-012**: A user can find a known file by name in the stored listings, with no network, within 5 seconds of typing the search text, in a cache of 5,000 entries.
- **SC-013**: Removing a local copy frees at least the file's size on the reader and causes no request to the server in 100% of tested cases.
- **SC-014**: The app starts to the home screen within 5 seconds on the InkPad One, installs in no more than 30 MB of storage excluding downloads, and keeps its stored listings within 50 MB.
- **SC-015**: Every screen string is available in English and German, and every screen that depends on the server shows its offline state and the age or time of its last refresh.
- **SC-016**: Names from the server that contain path separators, `..`, control characters or case-only differences never cause a file to be written outside the download folder or to replace another file, in 100% of hostile-input tests.

## Assumptions

- The user has one Nextcloud account on a server they run or control; several accounts are not supported in this feature.
- The InkPad One's built-in storage is FAT-formatted, cannot hold files of 4 GiB or more, and cannot make files private; this has been observed on the sibling apps and is carried over (see `docs/device-findings.md`).
- The app is used with Wi-Fi switched on by the user; the app itself never turns Wi-Fi on.
- The reader's own library scans a specific folder for books and documents; the exact folder is a planning decision. The user does not choose a folder in this feature.
- The large-file warning size is a planning decision; the working default is 100 MB.
- Uploading, creating folders, renaming, moving, deleting on the server, sharing, trash, versions, folder synchronization, several accounts, end-to-end encrypted folders, Talk, photo auto-upload, editing documents and notifications are out of scope and are separate features.
- Behavior that depends on a real Nextcloud server (login flow, quota, favorites, server search, version differences, proxies) and on the real device (timing, memory, storage, Wi-Fi) is **unverified** until exercised there; the success criteria are budgets, not measurements.
- The Android app described in `docs/reference/nextcloud-android-features.md` is a behavior reference only; this spec reimplements behavior and copies nothing from it.
- "Changed on the reader" is judged by comparing the local copy with what was downloaded; the precise method is a planning decision.
- The server may display the file's modified date in a different time zone; the reader shows the date it is given, corrected for its own time zone when the reader's clock is right.
