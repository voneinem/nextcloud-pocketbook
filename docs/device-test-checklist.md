# Device test checklist (InkPad One)

Template, reduced to the items that every PB app needs. The items of a feature are added with the feature's plan.

Every item starts as **unverified**. An item changes only when someone has done it on the reader and written
the date and the result next to it. Measured values go in the "Value" column with their unit; if an item
cannot be done, write why. Nothing on this page may be copied from a host run: the host tests use fake servers
and a desktop CPU (see `docs/reference/security-review.md`).

How to run: build the package (`scripts/package-device.sh`), install it, register `U_PbCloud`, and keep
`system/pbcloud/pbcloud.log` (it holds counts and codes only, no personal data) for the evidence. Peak memory:
`grep VmHWM /proc/<pid>/status` while the app is open (`pidof pbcloud.bin`, over SSH/telnet if the reader
offers it; otherwise "not measured").

Legend: **U** unverified · **O** observed informally by the owner (no measured value) · **V** verified (date,
value) · **F** failed (what happened).

## Installation and start

| ID | Item | Expected | Status | Date | Value / note |
|----|------|----------|--------|------|--------------|
| DT-INS-1 | Install the package, register `U_PbCloud`, start the app | The icon (a cloud) appears; the app opens on the start page ("PB Cloud", the version, "Not connected") | U | | |
| DT-INS-2 | Cold start to the first usable screen | under 5 s | U | | |
| DT-INS-3 | Package size | at most 25 MB (`scripts/package-device.sh` refuses more) | U | | |
| DT-INS-4 | Upgrade over an older build keeps the data folder `system/pbcloud/` | Settings and account are still there; a newer database is refused with a clear message and left untouched | U | | Schema upgrades are covered on the host by `test_store_migrations` |
| DT-INS-5 | PB Notes, PB Office and InkFeed are untouched by installing and starting PB Cloud: PB Cloud shares no binary name, launcher entry or data folder with them | Their files and data folders are unchanged; both icons are in the launcher | U | | |

## Security and storage

| ID | Item | Expected | Status | Date | Value / note |
|----|------|----------|--------|------|--------------|
| DT-SEC-1 | The credential statement matches the measured protection: after saving a password, `ls -l system/pbcloud/credentials` on the reader | About says "only this app can read" only if the mode is 600; on FAT it must say that the storage cannot protect the file | U | | |
| DT-SEC-2 | Same check from a PC: can another user read `credentials` after mounting the reader? | Expected yes on FAT (documented); the text must say so | U | | |
| DT-SEC-3 | A private certificate authority: put `custom-ca.pem` in `system/pbcloud/`; Settings > Security says "loaded"; a server with an unknown certificate is refused without it | Settings shows the state; no way to skip the check | U | | |
| DT-SEC-4 | A wrong password, a wrong server name and no network each give their own message | Specific text, no password in the log | U | | |

## Interface and settings

| ID | Item | Expected | Status | Date | Value / note |
|----|------|----------|--------|------|--------------|
| DT-UI-1 | About: version, every license text offline, time zone data release, password statement | all texts open without network | U | | |
| DT-UI-2 | German interface (Settings > Language): all pages readable, no text cut off | layout holds with the longer texts | U | | |
| DT-UI-3 | Rotation and the Back key | the page rotates; Back pops one page and never leaves a blank screen | U | | |
| DT-UI-4 | On-screen keyboard in a text field: the field and the cursor line stay visible | usable | U | | |
| DT-UI-5 | Wrong clock (set the reader's date 1 hour off, sync with a server): the clock warning appears | warning above 10 minutes | U | | |

## Resources

| ID | Item | Expected | Status | Date | Value / note |
|----|------|----------|--------|------|--------------|
| DT-RES-1 | Peak memory while browsing, and during a full sync | at most 150 MB browsing, 250 MB syncing | U | | |
| DT-RES-2 | Idle battery: app open, nothing to do, 1 hour | no network use; battery drop noted | U | | |
| DT-RES-3 | Wi-Fi reconnect, sleep and wake during a sync | the sync ends in a state the user sees; nothing is lost | U | | |

## Nextcloud files client (feature 001)

Added by `specs/001-nextcloud-files-client/`. Same legend as above.

| ID | Item | Expected | Status | Date | Value / note |
|----|------|----------|--------|------|--------------|
| DT-NC-1 | The bundled libcurl on the reader: HTTPS to a test server, same-host redirect, `Range`/`If-Range`, progress callback and cancel, 64 KiB buffer (probe with `PBC_PROBE_URL`) | all work; if `Range` does not, the app restarts a download cleanly | U | | |
| DT-NC-2 | The credentials file: its mode and place on the reader, and the text shown in Settings and About | the text matches the measured protection | U | | |
| DT-NC-3 | The download folder `PBCloud`: finished files appear in the reader's library; a `.pbc-*.part` file never does | listed within the time the library needs; no partial file listed | U | | |
| DT-NC-4 | Speed of the checksum pass on a 500 MB file | note MB/s | U | | |
| DT-NC-5 | Opening a downloaded book in the viewer does not change the file's size or modified time | unchanged (so it is not reported "changed on the reader") | U | | |
| DT-NC-6 | Source of the reader's time zone (`global.cfg` key) and the UTC fallback | dates in the reader's zone, or "UTC" shown | U | | |
| DT-NC-7 | Cold start; peak `VmHWM`; a cached folder of 1,000 entries; a page turn; the first page of an uncached 1,000-entry folder over home Wi-Fi | ≤ 5 s; ≤ 150 MB; ≤ 2 s; ≤ 1 s; ≤ 10 s | U | 2026-10-20 (host only) | HOST ONLY, not the device (0.1.0 development build, desktop, offscreen): start of the app with all pages instantiated 0.75 s wall including 0.6 s of fixed waits of the self-test, peak 77 MB; cached folder of 1,000 entries opens in 0.002 s, a page turn 0.003 s; parse and store of 1,000 entries 0.08 s; database with 50,000 cached entries 18.3 MB; 1 GiB download 12.9 s with a peak of 18 MB and 21 progress reports. Nothing measured on the device or over Wi-Fi. |
| DT-NC-8 | Wi-Fi drop and return during a 500 MB download | completes after reconnecting, file identical to the server's, no corrupt file | U | | |
| DT-NC-9 | A file of 4 GiB or more (sparse file on the server) | refused before any byte is written, with the reason | U | | |
| DT-NC-10 | Number of e-ink refreshes during a download | few (progress text updates at most every 2 s or 5 %) | U | | |
| DT-NC-11 | The launcher filter (`pbcloud.app`) on the reader's shell: open a downloaded book, then read `system/pbcloud/pbcloud.log` | no line holds the name or path of the book (a notice stands in its place); the exit code of the app is still logged; the first run of 0.1.0 showed `iv_OpenBook filename = <path>` lines without the filter | U | | |

## Upload and manage (feature 002)

Added by `specs/002-manage-server-files/`. Same legend as above.

| ID | Item | Expected | Status | Date | Value / note |
|----|------|----------|--------|------|--------------|
| DT-W1 | The bundled libcurl on the reader: a `PUT` from a file range with progress and cancel (probe with `PBC_PROBE_PUT_URL` and a throw-away file of a test account: it is overwritten) | the range is sent exactly (the server reports 1 MiB for the second MiB); cancel stops the transfer | U | | |
| DT-W2 | `Response::delivery` after a cut Wi-Fi in the middle of a request body | `not_sent`, `partly_sent` and `fully_sent` are told apart (the probe prints `delivery` of every `PUT`; a cut Wi-Fi is done by hand); if not, every unanswered write is treated as outcome unknown | U | | |
| DT-W3 | Chunk size (10 MiB) against memory and time on the device | peak `VmHWM` <= 150 MB, no stall | U | | |
| DT-W4 | Speed of the SHA-256 pass on a 500 MB file | 20 MB/s or better; note MB/s | U | | |
| DT-W5 | A 500 MB upload with a Wi-Fi drop and return | only the missing chunks are sent; the file on the server is identical | U | | |
| DT-W6 | Power loss during an upload | the queue is intact after restart; items are `unknown` only where expected; no partial file on the server | U | | |
| DT-W7 | Number of e-ink refreshes during an upload | few (progress text at most every 2 s or 5 %) | U | | |
| DT-W8 | The picker's real top level and the `PBCloud` folder | the eight folders are listed when they exist; nothing else | U | | |
| DT-W9 | Auto Sync on a reader whose Wi-Fi is off | the queue waits and the user is told; note the experience (owner point 2 of plan.md) | U | | |

## Folder synchronization (feature 003)

Added by `specs/003-folder-sync/`. Same legend as above.

| ID | Item | Expected | Status | Date | Value / note |
|----|------|----------|--------|------|--------------|
| DT-S1 | Start-up compare with Wi-Fi on and with Wi-Fi off | with Wi-Fi on one comparison per pair that has the switch on and nothing is transferred; with Wi-Fi off nothing is requested | U | | |
| DT-S2 | Modification-time resolution and stability of FAT files (a file copied from a PC) | size and mtime of a copied file stay stable between two scans; note the resolution | U | | |
| DT-S3 | The reader's library finds the files downloaded into a pair folder | a book downloaded into the pair folder appears in the library | U | | |
| DT-S4 | An Outbox folder written by another PocketBook app (PB Notes export) | its files are planned as uploads and are not changed or moved by the sync | U | | |
| DT-S5 | The free-space reserve with a nearly full card | downloads that would go below the reserve are marked and not approvable; nothing fills the card | U | | |
| DT-S6 | Peak memory and compare time (a compare of 1,000 files; a 500 MB file transferred) | peak `VmHWM` <= 150 MB; compare <= 10 s; note both | U | | |
| DT-S7 | Kill or battery pull during a sync | after restart the pair says "Interrupted"; a new sync gives the same final state; no partial file in the library | U | | |
| DT-S8 | Removable storage absent while a pair points to it | the pair shows "Reader folder not available"; nothing is proposed or deleted | U | | |

## Share links (feature 004)

Added by `specs/004-share-links/`. Same legend and rules as above.

| ID | Item | Expected | Status | Date | Build/result |
|----|------|----------|--------|------|--------------|
| DT-L1 | The Share tool in the select-mode panel of the folder page (six tools: Rename, Share, Move, Copy, Delete, Done) and in the details panel of a file (five tools: Download, Keep offline, Update, Open, Share) | all fit at the usual touch size; if the select-mode panel is too tight, Done becomes a top-bar action | U | | |
| DT-L2 | Text copied in the app, pasted into another app | decides `PBC_DEVICE_CLIPBOARD` (a CMake option of `app/CMakeLists.txt`); note the result either way | O | 2026-10-23 | Owner copied a link in PB Cloud and pasted in PB Office: "the clipboard is empty". The reader has no system clipboard (PB Office records the same, DT-WR-5). `PBC_DEVICE_CLIPBOARD` is OFF; Copy is not offered on the reader |
| DT-L3 | The link screen on sleep and on switching apps | note whether Qt reports an inactive state and whether the link is cleared | U | | |
| DT-L4 | The Share screen with 200 shares: time from the tap to the first page, and peak memory | first page <= 5 s on Wi-Fi; peak `VmHWM` <= 150 MB; note both | U | | |
| DT-L5 | A 120-character link at reading distance, and the plain password field with the on-screen keyboard | the whole link is legible without scrolling sideways; the keyboard shows no suggestions for the password | U | | |

## Mail link (feature 005)

Added by `specs/005-mail-link/`. Same legend and rules as above. Prerequisite: PB Office 0.4.0 or later is installed and **not running** (a running PB Office
returns 1 and takes nothing; observed with the hand-off probe on 2026-10-24, `docs/device-findings.md`). Nothing below has been run with PB Cloud's own tool yet.

| ID | Item | Expected | Status | Date | Build/result |
|----|------|----------|--------|------|--------------|
| DT-M1 | Share a file, open the link, tap Mail link, confirm | PB Office opens a new message with the link as its text and the subject "Link to <name>"; two taps; the time from the confirm tap to the open editor is at most 10 s (note the seconds); the draft can be deleted afterwards | O | 2026-10-24 | Owner: the tool opened a new message in PB Office, "everything fine". PB Office logged `handoff.received text_chars=68 fields=3` and `handoff.drafted count=1`. The seconds to the editor were not noted (SC-001 timing unmeasured). Build 0.5.0 |
| DT-M2 | Start PB Office and leave it open, switch to PB Cloud, repeat; then (with PB Office not installed or its launcher renamed) repeat | the notice "PB Office could not take the link" the next time PB Cloud is shown; no draft; no second try; record the value that the call returned for the missing case (open item O-1 of `specs/005-mail-link/research.md`) | U | | |
| DT-M3 | After DT-M1 and DT-M2 mount the reader and search `system/pbcloud/` (logs, database, probe files) for the link text and the item name in the logs | nothing found | O | 2026-10-24 | Searched PB Cloud's `pbcloud.log`, `pbcloud.log.prev` and `pbcloud.db` after the run: no link address (`/s/`), no subject, no `pbo-mail` marker. The item name was not searched by name (the listing cache holds names by design); PB Office's log holds no link either. Only the cold, accepted case was run |
| DT-M4 | After DT-M1 press Back in PB Office, then Home | PB Office's main screen, then PB Cloud with its list (the link screen is gone); record whether the link screen was already gone when PB Cloud came back | O | 2026-10-24 | Owner: Back and Home after the hand-off behaved as expected ("everything fine"); whether the link screen was already gone on return was not noted separately |

## Recording

For each measured value write the build version (About) and the date. Findings that change a design decision
go into `docs/device-findings.md`.
