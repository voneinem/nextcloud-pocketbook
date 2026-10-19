# Spec Kit prompts for nextcloud-pocketbook

Paste the prompts in this order, one per Claude Code session step. Prompts A and B are plain Claude prompts
(no Spec Kit command). Everything from C on is a Spec Kit command.

Assumptions I made. Change the prompts before pasting if one is wrong:

- **App name**: "PB Cloud", binary `pbcloud`, C++ namespace/prefix `pbc`, data folder `system/pbcloud`. The word
  "Nextcloud" is a trademark, so the app name does not carry it. The repository name stays `nextcloud-pocketbook`.
- **Baseline**: `office-pocketbook` 0.3.0 (newest of your two apps; it already has libcurl/WebDAV code, the
  e-ink "built-in look" QML components, the quality-gate scripts and the latest constitution).
- **Reference app**: `nextcloud/android` is GPL-2.0 (contributions AGPL-3.0). Same handling as `notes-android`:
  reference-only, behavior is described in your own words in `docs/reference/`, no code, strings or comments are copied.
- **First release is read-only** (connect, browse, download, keep offline). Uploading, managing files, folder sync,
  sharing and trash follow as separate features, which keeps every slice small enough to check on the device.

```
Order:   A bootstrap -> B reference analysis -> C constitution
         then per feature: specify -> clarify -> (checklist) -> plan -> tasks -> analyze -> implement -> converge
Features 001 browse + download | 002 upload + manage | 003 folder sync | 004 share links | 005 trash + versions
```

---

## A. Bootstrap the baseline (plain prompt)

```text
Bootstrap this repository from /home/voneinem/Projekte/office-pocketbook, the same way office-pocketbook was bootstrapped
from notes-pocketbook (see `git show 76aca5a` in that repository for the pattern).

1. Copy the baseline of office-pocketbook (its current HEAD, version 0.3.0) into this repository. Do NOT copy:
   `.git`, `build-*`, `dist`, `specs/`, `README.md`, `CHANGELOG.md`, `INSTALL.md`, `.claude/`, `.specify/` (this repository
   already has its own Spec Kit setup and Claude skills, keep them untouched), `.specify/bugs`.
   Copy `.specify/branch-convention.yml` (gitflow, `feature/NNN-name`) because it is not installed here yet.
2. Copy `docs/reference/` (inkfeed-analysis.md, pocketbook-sdk-analysis.md, security-review.md) and add
   `docs/reference/office-pocketbook-constitution.md` (a copy of office-pocketbook's `.specify/memory/constitution.md`).
   Copy `docs/device-findings.md`, `docs/device-test-checklist.md`, `docs/live-server-checklist.md` as templates and
   reduce them to the generic parts (no mail or calendar items).
3. Rename: product "PB Cloud", binary/app/launcher `pbcloud`, CMake variable prefix `PBC_`, C++ namespace and include
   directory `pbc`, data directory `system/pbcloud`, package `pbcloud-inkpad-one-<version>.zip`. After the rename the app
   MUST NOT share a binary name, launcher entry or data directory with PB Notes or PB Office. Reset `VERSION` to 0.0.1.
4. Remove the mail, calendar and contacts domain code and its tests and fixtures (IMAP, SMTP, MIME, iCalendar, rrule,
   vCard, CalDAV/CardDAV sync, outbox, drafts, html_text, charset tables, event/contact/compose QML pages and models,
   mail/calendar fixtures and generators). Keep everything that is generic: libcurl session and transports, HTTP/DAV
   common code, the minimal XML parser, CA selection and `custom-ca.pem`, credentials file, SQLite store base and
   migrations, logging, paging, clock check, about box, version, the whole QML component set (TopBar, BottomPanel,
   IconButton, InkRow, ModelPager, PageButtons, dialogs, InkField, Style, strings_de.tsv mechanism), the app/bridge/
   platform glue, the probe tool and every script under `scripts/`.
   If you are not sure whether something is generic, KEEP it and list it in `docs/baseline-leftovers.md` so that the
   first plan can remove it deliberately.
5. Leave a minimal start page ("PB Cloud", version, "not connected") so that the host build, the ARM build and the
   whole test suite stay green. Update `scripts/check-constitution.sh`, `check-fixtures.sh`, `check-fake-markers.sh`
   and `pre-merge.sh` for the removed code; do not weaken any rule, only remove rules whose subject is gone.
6. Write a short `NOTICE` and `licenses/manifest.txt` entry set that still lists InkFeed-derived patterns and every
   remaining third-party component.
7. Run the host build, `ctest` and `scripts/pre-merge.sh`. Report what passes and what does not. Do not hide failures.

Do not write README, CHANGELOG or specs yet. Commit as
`chore: bootstrap nextcloud-pocketbook from office-pocketbook 0.3.0` with a body that says what was kept and removed.
Documentation is English only.
```

## B. Reference analysis of the Android app and the Nextcloud APIs (plain prompt)

```text
Create two reference documents, in the same style as /home/voneinem/Projekte/notes-pocketbook/docs/notes-android-features.md.

1. `docs/reference/nextcloud-android-features.md`: a feature and sync analysis of https://github.com/nextcloud/android.
   Clone it read-only with `git clone --depth 1` into a NEW EMPTY directory outside this repository (use the session
   scratchpad), never inside the project, and never commit it. Record the analyzed commit hash.
   Licensing note at the top: the project is GPL-2.0 (contributions AGPL-3.0). The document paraphrases observed behavior in
   my own words only; no source code, comments, documentation text or UI strings are copied; names of public Nextcloud API
   endpoints and field names are mentioned as facts about the public API. Mark statements you did not confirm in the client
   code as **(unverified)** or **(inferred)**.
   Cover: (1) user-visible features of the Files part (browse, sort, search, favorites, recents, offline/available
   offline, download, open, upload, create folder, rename, move, copy, delete, trash, versions, sharing, activity,
   details sidebar), (2) accounts and login (Login Flow v2, app passwords, 2FA, SSO), (3) the local metadata database
   and how folder refresh works (etag comparison, depth, conflict handling, file locking), (4) download and upload
   mechanics (resume, chunked upload, checksums, mtime handling, conflict dialog), (5) error handling (maintenance mode,
   401/403/404/412/423/507, quota, redirects, untrusted certificates), (6) a table "feature -> fits an e-ink reader with
   no background activity: yes / adapted / no, and why". Explicitly list what to leave out (Talk, auto-upload of photos,
   end-to-end encrypted folders, rich document editors, media player, push notifications, widgets, multiple accounts).

2. `docs/reference/nextcloud-api-notes.md`: the server contract the app needs, taken from the official public
   documentation (docs.nextcloud.com developer manual: WebDAV basics, OCS capabilities, Login Flow v2, chunked upload,
   trashbin, versions, sharing API, file locking, system tags/favorites) and RFC 4918 / RFC 7232 / RFC 7233. For each
   operation give method, path pattern, required headers, requested properties, success and error codes, and the
   gotchas (weak ETags, quoted ETags, `oc:fileid`, `oc:permissions`, `X-OC-Mtime`, `OC-Checksum`, `Depth: infinity`
   being disabled, URL encoding, `remote.php/dav/files/<user>/` vs the legacy path, 207 multistatus with partial errors,
   429/503 Retry-After). Label everything **unverified** until it has been exercised against a real server, and add the
   minimum Nextcloud server version you recommend supporting, with the reason.

Documentation is English only. Commit as `docs: add Nextcloud Android and API reference analyses`.
```

---

## C. `/speckit-constitution`

```text
/speckit-constitution Create the constitution v1.0.0 (ratified 2026-10-19) for nextcloud-pocketbook, a Nextcloud files client for the PocketBook InkPad One ("PB Cloud"). Base it on docs/reference/office-pocketbook-constitution.md (the current constitution of my sibling app office-pocketbook, v1.2.0) and keep its structure, tone, "MUST" wording, Rationale lines and Governance section. Adapt it as follows.

Keep unchanged in spirit: I Test-First (NON-NEGOTIABLE), II Host-Testable Core and Layered Architecture (core/ Qt-free and InkView-free, bridge/, qml/, app/ and platform/pocketbook/), V Simplicity and Bounded Footprint (libcurl, OpenSSL, SQLite, plus Qt for the UI only; measurable RAM/storage/cold-start budgets in every plan, tests marked unverified until measured on the InkPad One), VI E-Ink UX and Battery (no background polling or syncing; network only on app start, on explicit request), VII Licensing Hygiene, VIII Quality Gates (scripts/pre-merge.sh, check-constitution.sh, check-fixtures.sh, check-ui-style.sh, version bump), IX Specs-Driven Workflow and Honest Verification, X English Documentation.

Change these for the new domain:
- I Test-First: boundary tests run against an in-process fake Nextcloud server (WebDAV PROPFIND/GET/PUT/MKCOL/MOVE/DELETE, OCS capabilities, Login Flow v2) and anonymized fixtures, never a real server or account. Parsers of untrusted input (207 multistatus XML, OCS JSON, file names, headers) MUST have tests for malformed, truncated, oversized, hostile (path traversal, control characters, overlong names, unicode normalization and case collisions) and partial-error input.
- III No Silent Data Loss, for files: a download is written to a temporary file in the same directory and renamed atomically only after the size and, where the server offers one, the checksum match; an interrupted download leaves no partial file visible to the reader's library and is resumable (Range) or restarted cleanly. A local file the user changed MUST NOT be overwritten by a download; a file changed on both sides keeps both versions and the user decides. Every server write and delete MUST be conditional (If-Match with the stored ETag; If-None-Match: * for creation); 412 or any detected conflict is a conflict. Pending uploads, renames and deletes are persisted before any network operation and are idempotent and resumable; an upload whose outcome is unknown is kept and marked uncertain, never repeated silently. Deleting on the server needs explicit confirmation and goes to the server trash; removing the local copy never touches the server and vice versa. Free space is checked before a download and a full or read-only storage is reported, never ignored.
- IV Security and Privacy: https only, no plaintext fallback, no TLS verification bypass, no "trust self-signed" option, device CA store else bundled CA file, plus `custom-ca.pem` (all as in office). Credentials are never sent to another host after a redirect; a redirect to another host is refused. App passwords only (never the account password); Login Flow v2 tokens are treated as secrets and discarded after use. Server-supplied names are untrusted: never used unsanitized as a local path (no `..`, no separators, no reserved characters of the FAT storage, length-limited, collision-safe). Downloaded files are untrusted data and are never executed; they are opened only by the reader's viewer the user chose. Thumbnails/previews, if any, come only from the configured server over https, bounded in size and number, on demand. Logs MUST NOT contain server URLs, user names, file or folder names, paths, share links or tokens. Credential storage is described honestly in README and About, with the same plaintext-file caveat and permission measurement as office-pocketbook.
- V Bounded footprint: folder listings are fetched with Depth: 1 only and are cached only for visited folders; the metadata cache is bounded and evicted without touching pending uploads/deletes or unconfirmed local changes; transfers stream to disk with a small fixed buffer; the largest supported file size is stated in each plan (mind the 4 GiB limit of FAT).
- VI E-ink UX: progress for long transfers uses a coarse refresh budget (few screen updates), transfers survive screen-off and Wi-Fi drops, and the UI always shows offline state and the time of the last refresh of the folder shown.
- VII Licensing: nextcloud/android (GPL-2.0, AGPL-3.0 contributions), nextcloud/server (AGPL-3.0) and other Nextcloud client code are reference-only: no code, strings, comments or documentation text is copied; behavior is reimplemented from the public API documentation and RFC 4918, 7232, 7233, 3986. The Nextcloud name and logo are not used as the app's brand or icon; the README states that the app is unofficial.

Remove: all IMAP, SMTP, MIME, HTML mail, remote-picture and outbox rules.

Add a new principle XI "Consistent Look and Feel": the app MUST look and behave like my sibling apps notes-pocketbook and office-pocketbook (the "built-in look" of the reader's own apps): reuse the existing QML component set (TopBar, BottomPanel, IconButton, InkRow, ModelPager/PageButtons, dialogs with footer buttons, Style), no bordered text buttons, no animations, page-based lists, icon + label tools in a bottom panel, hairline separators, the same home/settings/About structure, the same Auto-Sync wording, English source strings with a German translation in strings_de.tsv, and the same README/INSTALL/CHANGELOG/NOTICE conventions. `scripts/check-ui-style.sh` enforces what can be checked mechanically. Deviations need an amendment.

Technology section: C++17, Qt 6.8 Quick/QML, CMake, libcurl (own ARM build, the reader's libcurl is too old), OpenSSL, SQLite; WebDAV is spoken with libcurl and the existing minimal XML parser, no new XML or WebDAV library. SDK location only from POCKETBOOK_SDK or -DSDK_PATH. The app name, binary, launcher, data directory and package are `pbcloud` and never collide with PB Notes or PB Office. License MIT, "Copyright (c) 2026 Volker von Einem". Versioning rules, branch convention (feature/NNN-name), one-way tested schema upgrades and the Constitution Check in every plan are as in office-pocketbook. The first release version is 0.1.0.

Write the constitution in English, update .specify/memory/constitution.md, and give a short summary of the differences to the office-pocketbook constitution.
```

---

## D. Feature 001 - Browse and download (the MVP)

### D1. `/speckit-specify`

```text
/speckit-specify A read-only Nextcloud files client for the PocketBook InkPad One. The user connects to their own Nextcloud server, browses their files and folders page by page, downloads files on demand, keeps chosen files offline and opens them in the reader. It looks and behaves like my other apps PB Notes and PB Office (built-in look, e-ink friendly, offline first, no background network activity). The Android app (docs/reference/nextcloud-android-features.md) is the behavior reference; keep only what makes sense on an e-ink reader.

Stories, in priority order:

P1 Connect to my Nextcloud. I enter the server address, user name and an app password, and a Test button tells me whether it works and shows the server's name, version and my quota. As an alternative I can start a login that shows me a link (and a short code) to open on my phone or laptop, then tap "Check" on the reader to finish; no password is typed on the reader. Only https is accepted. Clear, actionable messages for: wrong credentials, two-factor authentication needing an app password, server in maintenance mode, server too old or without WebDAV, address that is not a Nextcloud, untrusted certificate (explain the custom-ca.pem option), and a wrong reader clock. The app never stores the account password, and tells me honestly how the app password is stored.

P1 Browse my files. A list of the folders and files in the server's root, folders first, with name, size and modified date, page by page with page buttons (no kinetic scrolling), sort by name, date or size, a path line showing where I am and a way to go up. Opening a folder fetches its content on request; folders I visited stay readable without a connection, marked with the age of the listing. Refresh is explicit. Entries that are not available to me (no permission, encrypted folders, shares in a state the app does not support) are shown as such and do not crash anything.

P1 Download and open a file. Tapping a file shows its details (size, type, modified date, whether it is on the reader and whether the server version is newer) and offers Download. The download shows progress and can be cancelled; if the connection drops it resumes or restarts cleanly; no half-downloaded file ever shows up in the reader's library. The app checks free space first, warns about very large files and refuses files the storage cannot hold (including the FAT file-size limit). Downloaded books and documents land in a folder the reader's own library finds, and I can open a downloaded file in the reader's viewer with one tap. A local copy that I have changed is never overwritten without asking.

P2 Keep files offline and update them. I can mark files or whole folders as "keep offline". An Update action (never automatic) checks them against the server and downloads only what changed; the list shows per entry whether it is not downloaded, downloaded, outdated (changed on the server) or changed on the reader.

P2 Favorites and search. I can see the files I starred in Nextcloud and I can search by name, offline in what the reader knows and, when connected, on the server.

P3 Manage space on the reader. A screen lists what the app downloaded with sizes, lets me remove local copies (the server is never touched), shows free space, and bounds the metadata cache.

Out of scope for this feature (separate features later): uploading, creating folders, renaming, moving, deleting on the server, sharing, trash, versions, folder synchronization, several accounts, end-to-end encrypted folders, Talk, photos auto-upload, editing documents, notifications.

Cross-cutting requirements: offline first; everything that depends on the server shows its state ("not synced", age of the listing, last refresh time); network activity only on app start (when Wi-Fi is already on), on explicit user action; English interface with a German translation; nothing the user typed or downloaded is lost or silently overwritten; names from the server are never trusted as local paths. Success criteria should be measurable: for example a cached folder of 1,000 entries is shown within a stated time and navigable page by page, a download interrupted by a Wi-Fi drop completes after reconnecting without corrupt files, and budgets for RAM, storage and cold start are stated (unverified until measured on the device).
```

### D2. `/speckit-clarify`

```text
/speckit-clarify Focus on: single account versus several accounts; where downloads are stored and how the reader's library finds them (and whether the folder structure of the server is mirrored); what "open in the reader" means technically on this device; how file-name collisions and illegal FAT characters are handled; what "outdated" and "changed on the reader" are based on (ETag, checksum, size, mtime); whether thumbnails or cover previews are wanted at all; the largest file size to support; how long cached listings are kept; behavior when the server certificate is untrusted. Prefer the simplest answer that keeps the constitution satisfied, and record each answer in the spec.
```

My recommended answers if you want to just accept: one account; downloads under one app folder in the library with the server's folder structure mirrored and sanitized names; open via the reader's viewer (verify on device, mark unverified); ETag is the primary comparison, size as sanity check, SHA-1 checksum when the server sends one; no thumbnails in 0.1.0; files up to 2 GiB; cached listings are kept until refreshed, evicted by size; untrusted certificate = error with the custom-ca.pem explanation, no bypass.

### D3. `/speckit-checklist` (optional, recommended once)

```text
/speckit-checklist security and data safety: check that the requirements for TLS, redirects, credentials, app passwords, Login Flow v2 tokens, untrusted server file names, atomic downloads, resume, overwrite protection, free-space handling and logging are complete, unambiguous and testable.
```

### D4. `/speckit-plan`

```text
/speckit-plan Use the stack and layering of the constitution (C++17 core/ Qt-free, bridge/, qml/, libcurl, OpenSSL, SQLite). Reuse what the baseline already has instead of writing new code: dav_common, http_transport, curl_session, xml_min, ca_selector, credentials_file, the store base with migrations, paging, clock_check and the QML component set; list in research.md what is reused, what is adapted and what is new, and remove the leftovers listed in docs/baseline-leftovers.md that are in the way. Do not add a dependency.

Research and record (mark every point that needs the real device or a real server as unverified, and extend the probe tool to check what can be probed):
1. Nextcloud contract from docs/reference/nextcloud-api-notes.md: status.php and capabilities, PROPFIND Depth 1 with the minimal property set (resourcetype, getetag, getcontentlength, getlastmodified, getcontenttype, oc:fileid, oc:permissions, oc:favorite, oc:checksums), 207 handling incl. partial errors, ranges for GET, ETag quoting and weak ETags, Login Flow v2 (start, poll on user tap only, discard token), app-password handling, minimum server version, 429/503 Retry-After.
2. Does the device's libcurl matter here, or only the bundled 8.18 build? Redirect policy (same host only, credentials never forwarded).
3. Where downloads go on the InkPad One, how the library picks them up, how to open a file in the built-in viewer from the app (InkView/launcher mechanism), what happens to the reader's bookmarks and notes if a downloaded file is replaced by a newer version. Decide with evidence from the probe, otherwise state the assumption and mark it unverified.
4. Local path mapping: sanitizing server names for FAT, length limits, Unicode normalization, case-insensitive collisions, how a collision is resolved deterministically, the 4 GiB limit.
5. Data model: accounts (credentials file, not SQLite), folder listing cache, file entries with ETag/fileid/permissions/favorite, download state machine (queued, downloading, verifying, complete, failed, outdated, locally modified), keep-offline set, schema version 1 and the rule that an older build refuses a newer database.
6. Credential storage: evaluate a better-than-plaintext option, record the decision (restrictive permissions at minimum), and how the permission is measured and shown.
7. Budgets for RAM, storage, cold start, listing size (1,000 and 10,000 entries) and transfer buffer; streaming XML parsing or a bounded page size so a huge folder cannot exhaust memory.
8. Test strategy: extend the in-process fake server of the baseline into a fake Nextcloud (WebDAV + OCS + Login Flow v2) with fault injection (drop mid-download, 401, 403, 404, 412, 423, 429, 503, 507, redirect to another host, malformed and oversized XML, hostile names); contract tests per contract file; hand-written fixtures carry the `.unverified` marker.

Produce data-model.md, contracts/ (UI-independent core API, fake server contract, store schema), quickstart.md, and a Constitution Check that lists every principle (I-XI) with pass, justified deviation or not applicable. Include a final task group for the release: VERSION 0.1.0, CHANGELOG, README in the GitHub convention (badges, table of contents, unofficial-app statement, credential and privacy section), INSTALL.md, NOTICE, licenses/manifest.txt, About box, German strings, device-test-checklist and live-server-checklist entries.
```

### D5. `/speckit-tasks`

```text
/speckit-tasks Generate tasks per user story so that each story is independently testable and shippable in the order P1 (connect), P1 (browse), P1 (download and open), P2, P2, P3. Within each story put the failing tests first (Principle I), then the implementation. Add explicit tasks for: extending scripts/check-constitution.sh with new mechanical rules (no http:// URLs, no certificate-verification switches, no file names, paths or URLs in log calls, no raw server names used as paths) each with a self-test that proves the rule detects a violation; the fake Nextcloud server with fault injection; the probe additions; manual device tasks marked "unverified until done on the InkPad One"; and the release phase (VERSION 0.1.0, CHANGELOG, README, INSTALL, NOTICE, manifest, About, strings_de.tsv).
```

### D6. `/speckit-analyze`

```text
/speckit-analyze Check spec, plan and tasks against the constitution (all eleven principles) and against each other. Pay particular attention to: any requirement without a task or test, any task without a requirement, anything that could overwrite or lose a user file, any path that could use an unsanitized server name, anything that could send credentials to another host, any background network activity, UI that deviates from the built-in look of PB Notes and PB Office, and "verified" claims that were only simulated.
```

### D7. `/speckit-implement`

```text
/speckit-implement Implement in slices, one user story per slice, in priority order. After each slice: run the host build, ctest and scripts/pre-merge.sh, report the result honestly, commit, and STOP so that I can check the slice on the InkPad One before you continue. Tests first, observed failing, then the implementation. Mark every task done only when its test passes; leave device and live-server tasks open and list them for me.
```

### D8. `/speckit-converge` (after the last slice)

```text
/speckit-converge
```

---

## E. Later features (same cycle each: specify, clarify, plan, tasks, analyze, implement)

Start each on its own branch (`feature/NNN-...`) after the previous one is merged and tagged.

### 002 - Upload and manage files

```text
/speckit-specify Upload and manage files on the Nextcloud server from the PocketBook. Builds on feature 001 (spec in specs/001-*). The user can upload files from the reader's storage using a file browser like the attach browser of PB Notes (list and tile layout, remembered), into the folder currently shown or a chosen one; create folders; rename; move and copy within the server; and delete files and folders on the server. Everything is queued on the reader first and sent only on explicit request or shortly after the user's own action (Auto Sync, on by default and switchable), with each queued item marked "not synced". Uploads are resumable, large files go in chunks, and an upload whose outcome is unknown is kept and flagged, never repeated silently. Never overwrite silently: uploading a name that exists asks (keep both with a new name, replace after confirmation, or skip), replacing and deleting use the stored ETag, and a conflict (changed on the server meanwhile) keeps both versions until the user decides. Deleting asks for confirmation and says it goes to the server's trash; permanent deletion is not part of this feature. Handle full quota, forbidden names, files the server rejects (size or type), and a locked file with clear messages. Out of scope: sharing, trash and versions screens, folder synchronization, automatic upload of any kind, background activity.
```

### 003 - Folder synchronization

```text
/speckit-specify Folder synchronization between chosen Nextcloud folders and folders on the reader. Builds on features 001 and 002. The user pairs a server folder with a folder on the reader (for example a "Books" folder to read from, and an "Outbox" folder on the reader whose new files go to the server). Direction per pair: download only (default), upload only, or two-way. A sync runs only at app start (when Wi-Fi is already on), on the Sync button, or shortly after the user's own change; there is never a background sync. Before anything is changed the user can review what a sync will do (what is downloaded, uploaded, skipped), and the result is shown afterwards with counts. Deletions are never propagated automatically: a file removed on one side is shown as a proposal that needs confirmation. Changes on both sides keep both versions. The sync is resumable and idempotent: repeating an interrupted sync gives the same result. Bounded by a configurable file count and total size per pair so that the reader's storage cannot be filled unnoticed. Out of scope: photo auto-upload, live file watching, more than the paired folders.
```

### 004 - Share links

```text
/speckit-specify Share links from the PocketBook. Builds on feature 001. For a file or folder the user can create a public share link, optionally with a password, an expiry date and read-only or (for folders) upload permission; see the existing links and shares of that item (including shares with users and groups, read-only list); copy the link text so that it can be shown or typed elsewhere; change a link's password, expiry or permission; and remove a link after a confirmation. Links and passwords are shown as plain text only on the screen the user asked for, are never opened by the app, never written to logs, and the password is never stored on the reader after it was sent. Show clear messages when the server forbids public links, forces a password or an expiry date, or limits the permissions. Out of scope: creating shares with users or groups, share-by-mail, Talk, notes-to-recipient, QR codes.
```

### 005 - Trash and versions

```text
/speckit-specify Deleted files and file versions. Builds on features 001 and 002. The user can open the server's trash, see the deleted files with their original location and deletion date, restore a file or folder (to its original place, or to a chosen place if that is taken without overwriting anything) and delete items for good or empty the trash only after an explicit second confirmation that names what will be lost. For a file the user can list its older versions (date and size) and restore one, which keeps the current version as a new version rather than discarding it. The app explains when the server has the trash or versions app disabled. Out of scope: retention settings, comments, activity feed.
```

---

## F. Habits that keep the three apps consistent

- After each merged feature: tag `v<version>`, bump `VERSION` and `CHANGELOG.md` in the feature's last phase
  (enforced by `scripts/check-version-bump.sh`).
- Fixtures that imitate server answers carry the `.unverified` marker until a real Nextcloud has been tried; record
  results in `docs/live-server-checklist.md` and `docs/device-findings.md`.
- A bug on the device or the server goes through `/speckit-bug-assess` -> `/speckit-bug-fix` -> `/speckit-bug-test`.
- If the constitution needs to change, amend it with a version bump (`/speckit-constitution`), do not work around it.
