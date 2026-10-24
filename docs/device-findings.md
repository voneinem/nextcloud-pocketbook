# Device findings

Template. Facts observed on the PocketBook InkPad One. Everything not listed here is **unverified** (constitution,
Principle IX). Each entry has a date and the way it was observed.

PB Cloud has not been run on the reader yet: it has no findings of its own. The facts below are device facts that
were observed with the probe of the sibling app office-pocketbook (PB Office 0.1.0, the probe started through the launcher's
`run-probe` flag, the reader not on Wi-Fi). They describe the reader, not this app, and are carried over as the
starting point. Run the probe of this app (`scripts/probe-device.sh`) and record its own output below.

## Inherited device facts (observed 2026-10-14)

| Item | Result | Status |
|---|---|---|
| libcurl on the reader | 8.12.0 (OpenSSL 3.5.4); the protocol list includes `https` | Protocol list **verified** on the device. The app carries its own static libcurl 8.18.0 (`scripts/build-curl-arm.sh`); that was decided because the reader's libcurl drops the contents of IMAP literals, which PB Cloud does not use. Whether the reader's libcurl is good enough for HTTPS/WebDAV is **unverified**; the minimum version in `core/include/pbc/curl_session.h` stays until the first plan decides |
| `chmod` / protected location | the data folder is **vfat**; `chmod 0600` is **not observable** (the mode reads back 0777) | **Verified.** The credentials file cannot be made private on this storage; the in-app text must say so. No protected location was found |
| `/etc/localtime`, `TZ` | `TZ` is unset; `/etc/localtime` is a link to a relative target named `timezone` | **Partly open.** Must not be relied on: the app uses bundled tzdata |
| gconv modules | `/usr/lib/gconv` present; iconv to UTF-8 works for the 16 charsets probed (ISO-8859-1/2/15, Windows-1250/1251/1252, KOI8-R, ISO-2022-JP, Shift_JIS, EUC-JP, GB2312, GBK, Big5, EUC-KR, UTF-7, UTF-16LE) | **Verified** for this firmware |
| System CA store | `/etc/ssl/certs/ca-certificates.crt` with 150 certificates; the bundled CA file in the package stays as the fallback | No HTTPS handshake was tested (no network) |

### Other device facts

- Kernel `5.10.226pocketbook aarch64` with a 32-bit ARM user space (the app and the probe are armhf binaries).
- Memory: `MemTotal` 1,987,872 kB.
- Frame buffer: 1872 x 1404 pixels as reported by sysfs (the app's Qt window is 1404x1872 portrait).
- **Qt on the device is 6.10.3**; the SDK sysroot is Qt 6.8.2. An app built against the SDK runs on it, but anything Qt-version-sensitive is device-unverified.
- QML modules present on the device include `QtWebView`/`QtWebEngine`; the PB apps never use them (rule U5 of `scripts/check-ui-style.sh`).
- QPA platform plugins present: `linuxfb`, `minimal`, `offscreen`, `pocketbook2`, `pocketbook2egl`, `vnc`.

## PB Cloud findings

First run of PB Cloud 0.1.0 on the InkPad One, 2026-10-20, read from `system/pbcloud/pbcloud.log` after the owner's own try-out against the owner's own Nextcloud
(server type and version not recorded; the owner reported that everything worked). What the log shows, and nothing more:

| Observation | Evidence | Status |
|---|---|---|
| The app starts, runs and exits cleanly (exit code 0); no `mali0` refusal | `pbcloud.log`, two launches | Observed |
| The bundled libcurl reached the owner's server over https with the device CA store (`ca.selected source=device custom_certificates=0`); Test, one listing (5 entries) and an Update of 5 files (`added=5 failed=0`) worked | `listing.refresh result=ok entries=5`, `update.run files=5 added=5 updated=0 failed=0` | Observed once, one server. `Range`/`If-Range`, same-host redirects and cancel were not exercised (DT-NC-1 stays open) |
| Five downloads of 0.5 to 18.4 MB (37.3 MB together) finished within about 7 s on the owner's Wi-Fi, each with the size the server announced; the files are in `PBCloud/E-Books/` | `download.done bytes=...`, folder listing | Observed. One run is not a measurement (DT-NC-4, DT-NC-7 stay open) |
| A downloaded PDF was handed to the reader's viewer (`iv_OpenBook` with the full path, `task found`) and opened | log of the second launch; the owner reported it worked. One `hw_ipcrequest_int` "request failed" line precedes it and did not stop it | Observed. Whether the open changed the file's size or modified time (DT-NC-5) was not checked |
| The on-screen keyboard (latinime, German dictionary) came up for the address and password fields | `OpenKeyboard`, `latinime: ... DE` in the first launch | Observed |
| The credentials file has mode 644 as seen from the desktop on the FAT storage; it cannot be made private there | `stat` on the mounted reader | Consistent with the inherited finding above; the in-app text says so |
| The InkView SDK itself writes the full path of a book into the log (`PERF_TESTING START: (iv_OpenBook) filename = ...`, `task_findbybook(...)`) when a file is opened. The app's own log lines hold no names or paths | `pbcloud.log` | Observed; this is the SDK, not the app. The app cannot switch it off |

Not yet observed: the browser login, a dropped connection, a file of 4 GiB or more, the budgets (cold start, memory), the library listing the files without a rescan, and every
other LV and DT item. Nothing in the checklists was changed to verified on this evidence.

Second run, 2026-10-20 evening (database and log read from the mounted reader):

| Observation | Evidence | Status |
|---|---|---|
| `system/config/global.cfg` names the zone by city only: `timezone=Berlin` (not `Europe/Berlin`). Version 0.1.0 as first installed did not recognize it and showed dates in UTC (two hours early in summer time) | `global.cfg`, the owner's report of "19:15" for a check at 21:15 | Observed. The app now finds a unique city in the bundled zones (DT-NC-6 stays open until the new build is seen on the reader) |
| The log of a launch ended after the launcher's header: the app's lines were held back by `sed` in the first filtering launcher and lost when the app was stopped by the USB mode | `pbcloud.log` of 21:42 | Observed. The launcher now filters line by line with the shell (DT-NC-11) |
| The upgrade of the database to schema 3 ran on the first start (`user_version` 3, `pbcloud.db.bak` kept) and an Update at 21:43 was recorded in `sync_state` | the database file | Observed |
| "Last checked" stayed at the time of the start check because the account check runs only at start and only with Wi-Fi already on; since then any request the server accepts confirms the login | `account.checked_at` = 21:15 while the Update ran at 21:43 | Observed; behavior changed |


## Write side (feature 002, version 0.2.0)

No run of the upload, queue or manage functions on the reader or against a real Nextcloud server has happened yet. Everything below is what the design
rests on and is therefore **unverified** (items DT-W1 ... DT-W9 in `docs/device-test-checklist.md`, LV-W1 ... LV-W12 in `docs/live-server-checklist.md`):

- The bundled libcurl sends a request body from a range of a file with a 64 KiB read buffer, reports progress, can be cancelled in the middle of the
  body, and `Response::delivery` tells "nothing sent" from "partly sent" from "fully sent" after a cut connection (DT-W1, DT-W2).
- A chunk of 10 MiB streams without memory growth; the checksum pass over a 500 MB file is fast enough on the card (DT-W3, DT-W4).
- The reader's real storage shows the folders the picker lists, and the download folder `PBCloud` is among them (DT-W8).
- Auto Sync on a reader whose Wi-Fi is off leaves the queue waiting and says so (DT-W9).

## Starting one app from another with text (observed 2026-10-23, InkPad One, hand-off probe)

Observed with `pbc-handoff.bin` and `pbcloud-handoff-target.app` (INSTALL.md, "Hand-off probe"); the target was a stand-in script, **not PB Office**. The texts were fixed
test strings. Source: `system/pbcloud/handoff-probe.txt`.

- `OpenBook(<path of an .app>, "<text>", 0)` starts that file as an application and the text arrives as **one** argument (`$1`, `argc=1`), unchanged: spaces, quotes,
  non-ASCII letters and the euro sign intact. `OpenBook2(<path>, argc, argv, 0)` delivers each argument as given, including one with a line break.
- 1500 bytes in one argument arrived completely (no limit found up to that size).
- The call returns 0 at once; the target starts within about a second; its parent process is `init` and its working directory is `/`; `$0` is the path of the .app.
- The calling app **keeps running**: it receives EVT_BACKGROUND (152) and, after the target ended, EVT_FOREGROUND (151). It is not closed by the hand-off.
- The system's own log lines about the call name only the path of the started app (`iv_OpenBook filename = [<path>]`, `task_findbybook`, `task_findbyappname`),
  not the text; the launcher's log filter removes those lines anyway.
- **Not measured:** a real, long-running target such as PB Office; what happens when the target is **already running** (the log shows the system looks the app up
  by name, `task_findbyappname`, so a second start may switch to the running instance and the text may not arrive); the state of the caller after the user leaves
  the target; the length limit above 1500 bytes. While the target runs, its arguments are visible in its process list entry, so a launcher must not print or log
  them and should pass the text on in a way that is not a command-line argument of a long-running program.

### Handing text to the real PB Office 0.4.0 (observed 2026-10-23 and 2026-10-24, InkPad One, box D of the probe)

Box D calls `OpenBook2("/mnt/ext1/applications/pboffice.app", 5, {"pbo-mail-v1", "text=…", "textlen=…", "subject=…", "to=…"}, 0)` with a fixed test text of 110 bytes
(contract v1 of PB Office, `specs/003-receive-shared-text/contracts/handoff-contract.md`). Nothing is sent by PB Office; the result is a draft.

- **PB Office not running (cold start): works, twice.** The call returns 0, PB Office starts and opens a new message with the text, subject and recipient. Its log
  has `handoff.received text_chars=105` and `handoff.drafted count=1`; the folder `system/pboffice/handoff/` is empty afterwards; the test marker appears in no log
  and no probe file.
- **After the user leaves PB Office:** Back shows PB Office's main screen, not the caller; Home shows the caller (the probe screen) again. The caller received
  EVT_BACKGROUND at the call and EVT_FOREGROUND when it was shown again. Judged acceptable by the owner.
- **PB Office already running (case without a request listener in PB Office 0.4.0): not delivered, and the sender can tell.** `OpenBook2` returned **1**
  (observed twice, 09:03 and 09:10 on 2026-10-24) instead of 0; PB Office comes to the front and shows its own screen; no draft is made, nothing is logged by PB Office,
  nothing is left in `handoff/`. The caller still received EVT_BACKGROUND.
- **Not measured:** a running PB Office that has a request listener (the listener probe of PB Office, `run-listener-probe`). The probe runs that were meant for it ended
  without data: in two of them the listener instance was not the one running at the time of the call (the flag is used up by the first start), and in the last one
  the reader restarted (uptime 0 min in the next launch's log) before the call. A reader restart while the listener probe was active may be a finding for PB Office.

DT-M2 of feature 005 records the value that the call returns when PB Office is not installed: it is not known yet (open item O-1 of `specs/005-mail-link/research.md`).

### Mail link with PB Cloud's own tool (observed 2026-10-24, PB Cloud 0.5.0, InkPad One)

- PB Office not running: Mail link and its confirmation opened a new message in PB Office (DT-M1). PB Office's log shows one hand-over (`text_chars=68`) and one draft.
  PB Cloud's logs and database hold no link, subject or marker (DT-M3, searched for the link address and the marker, not for the item name).
- Not run: the running-PB-Office case and the missing-PB-Office case (DT-M2), so the notice after the app was switched away and the value returned for a missing PB Office
  are still unobserved. The seconds from the confirm tap to the editor were not noted.

## Trash and versions (feature 006, version 0.6.0)

Not measured yet. DT-T1 decides where the Trash tool lives: the Home page has eight tools now, the normal panel of the folder page seven, and the details panel of a file up
to seven with Versions. If a panel is too tight at the usual touch size, Trash moves to the top bar of the Home page and the folder page, and Versions becomes a row of the
details list; the pages and bridges stay as they are. DT-T2 to DT-T4 record how the confirmations read on the panel, the time and memory of a 1,000-item trash and the
behavior of Empty trash with the screen and Wi-Fi switched off. LV-T1 to LV-T10 in `docs/live-server-checklist.md` hold what only a real server can show; until they are run
the plan's restore to a chosen place may degrade to the message "This server can restore only to the original place".
