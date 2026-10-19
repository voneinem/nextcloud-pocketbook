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

## Recording

For each measured value write the build version (About) and the date. Findings that change a design decision
go into `docs/device-findings.md`.
