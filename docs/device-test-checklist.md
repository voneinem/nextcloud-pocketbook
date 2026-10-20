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

## Recording

For each measured value write the build version (About) and the date. Findings that change a design decision
go into `docs/device-findings.md`.
