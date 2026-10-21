# Implementation Plan: Upload and Manage Files on the Server

**Branch**: `feature/002-manage-server-files` | **Date**: 2026-10-20 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/002-manage-server-files/spec.md`

## Summary

PB Cloud stops being read-only. From the reader the user uploads files and whole folders (picked in a file browser like PB
Notes' attach browser), creates folders, renames, moves, copies and deletes on the server. Every change is first written
to a **persistent queue** on the reader and shown as "not synced"; the queue is sent only when the user taps Send or, with
**Auto Sync** (on by default, switchable), ten seconds after the user's own action. Large files go in chunks and resume;
an operation whose outcome is unknown is kept and flagged, never repeated by the app. Nothing is overwritten or deleted
without a condition on the server version the reader last saw, and a conflict keeps both versions until the user decides.
Delete asks for confirmation, says it goes to the server's trash, and is not offered at all on a server without a trash.

Technical approach (details in [research.md](research.md)): add a write layer to the Qt-free core (`dav_write`,
`upload_session`, `queue_sender`, `auto_sync`, `check_outcome`, `new_name`, `source_walk`, `upload_source`, `server_caps`),
schema 4 with the `queue_items` and `server_caps` tables and an overlay query that merges open queue items into the folder
listing, a file-range request body and a `delivery` flag in the transport, three bridges (`queue`, `picker`, browse changes)
and four pages (`FilePickerPage`, `FolderChooserPage`, `QueuePage`, changes to `FolderPage`, `HomePage`, `SettingsPage`).
No new dependency. The test base is the stateful in-process fake Nextcloud extended with the write side and failure
injection (lost answers, dropped connections, ignored headers), plus a few tests through the real libcurl transport.

## Technical Context

**Language/Version**: C++17 (core, bridge); Qt 6.8 Quick/QML for the UI (device runs Qt 6.10.3, SDK sysroot 6.8.2); CMake

**Primary Dependencies**: libcurl 8.18.0 (static on the reader, system on the host), OpenSSL `libcrypto` (SHA-256, already
linked), SQLite (vendored amalgamation). **No new third-party dependency.**

**Storage**: SQLite `pbcloud.db` **schema 4** (`queue_items`, `server_caps`, settings `auto_sync`, `picker_view`,
`last_send_*`) in `/mnt/ext1/system/pbcloud/`; the user's source files are only read; no copy of an uploaded file is made

**Testing**: the project's own test framework (`tests/support/test_support.h`) under CTest; unit, integration against the
fake Nextcloud, real-libcurl tests against the local TLS server, bridge (Qt Test, offscreen), QML self-test, script checks,
kill/interruption harness, hostile-input fixtures, budget tests (unverified until measured on the device), the host UI walk

**Target Platform**: PocketBook InkPad One (RK3566, armhf, Qt on the device); host Linux x86-64 for the complete core test
suite (constitution II)

**Project Type**: desktop-style Qt Quick application with a Qt-free core library (device app plus host tests)

**Performance Goals** (budgets, **unverified until measured on the device**; they restate spec SC-001 to SC-016):

| Quantity | Budget |
|----------|--------|
| Pick -> listed as "not synced", no request | <= 5 taps from Upload; list updates <= 1 s |
| Auto Sync start after the last user action | 10 s (+-1 s) |
| Queue screen with 200 items | opens <= 2 s; page turn <= 1 s |
| SHA-256 pass before a large upload | >= 20 MB/s (measured, DT-W4) |
| Upload progress screen updates | <= 1 per 2 s or per 5 % |
| Cold start | <= 5 s (unchanged from 001; recovery of the queue adds no network and <= 100 ms for 2,000 rows) |

**Constraints**:

| Quantity | Budget |
|----------|--------|
| RAM | peak <= 150 MB (`VmHWM`) while uploading 500 MB, while walking a 2,000-item folder pick, and while showing a 200-item queue; 64 KiB transfer buffer, no file or chunk in memory |
| Installed size | package <= 25 MB (the `package-device.sh` gate); expected growth < 1 MB |
| Queue | 2,000 items; a folder pick counts toward it (walk: 2,000 items, depth 12); database growth <= 2 MB at the limit |
| Largest file | 4 GiB - 1 byte (FAT, source side); 0 bytes refused; chunks 10 MiB (server maximum if smaller, never below 5 MiB), one in flight, at most 10,000 chunks (never reached) |
| Network | none in the background; one account check at start; everything else on a tap, or after the user's own action when Auto Sync is on; no retry timer, no polling |
| Server contract | Nextcloud 30 or newer assumed (unverified); write methods `PUT`, `MKCOL`, `MOVE`, `COPY`, `DELETE` only, each with its condition |
| Auto Sync default | **on** (owner's decision in the spec; stated here as constitution VI requires) |

**Scale/Scope**: one account; a personal library; queues of up to a few hundred items; 7 user stories, 44 functional
requirements; about 10 new core modules, 2 new bridges, 3 new QML pages, changes to 4 pages

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design (verdict column).*

Constitution version 1.0.0 (ratified 2026-10-19). Result: **pass** with three noted points (III final-`MOVE` residual race,
III temporary upload folder, VI Auto Sync timer) and three owner points below. Principles III, VI and VIII are where this
feature touches the constitution's most specific text.

| Principle | Verdict and where it is satisfied |
|-----------|-----------------------------------|
| I Test-First | **Pass.** Every module has failing-first tests (research R-19): unit, fake-server contract tests of every write operation including `test_write_conditions`, real-libcurl tests, hostile and partial-input fixtures for each parser (upload-folder listing, sabre error bodies, capability lists, picker walk), kill tests at every step of an upload and of each single-request operation. The WebDAV write methods and the SQLite schema 4 migration are boundary behavior with contract/integration tests against the fake, never a real server. |
| II Host-testable core | **Pass.** All logic is in `core/`, Qt-free; `bridge/` holds only `QueueBridge` (with the one `QTimer`), `PickerBridge` and bridge changes; platform facts enter through `StorageInfo`, `Clock`, `Random`, `HttpTransport`, the Wi-Fi callback and `SourceRules` (the storage root). The scheduler takes time as a parameter, so Auto Sync is proven without sleeping. |
| III No silent data loss | **Pass with two notes.** (1) Queue rows are committed **before** any request, never discarded, evicted or overwritten until confirmed or removed by the user, idempotent and resumable (chunk listing is the authority); `unknown` is kept and skipped by every run ([R-6](research.md), [R-7](research.md)). (2) Every server write is conditional: `If-None-Match: *` / `If-Match` on `PUT`, `Overwrite: F` plus `If-Match` on `MOVE`/`COPY`, `If-Match` on `DELETE`, `MKCOL` by nature; a 412 or detected conflict is a conflict that keeps both versions; weak or empty ETags are never used for a conditional write ([R-3](research.md)). (3) Delete needs confirmation, goes to the trash, is not offered without one ([R-9](research.md)); removing a local copy never touches the server; a server deletion never removes a local file ([R-10](research.md)). (4) **Note - chunked final `MOVE`:** which conditions the server honors there is undocumented; the sender sends them and adds a `PROPFIND` pre-check, leaving a seconds-wide race only if the server ignores them; LV-W6 closes it ([Complexity Tracking](#complexity-tracking)). (5) **Note - the upload folder:** `MKCOL`, chunk `PUT` and `DELETE` on the app's own temporary session under `/dav/uploads/` carry no condition because they never touch user data; a folder `DELETE` of the user's files always does. (6) Free space is checked before the download side as before; on the upload side the quota is checked before sending when known (FR-038). |
| IV Security & Privacy | **Pass.** TLS and https-only unchanged; write requests go through `DavSession` (same-host rule, credentials never in a URL, a redirect to another host refused for writes too). Names are untrusted both ways: server names by 001's rules, source names by `check_new_name`; **the picker cannot select the app's data folder, credentials or log, and links are never followed** (`source_walk`, re-checked with `O_NOFOLLOW` at send); server error bodies are read only for a whitelisted class name and never shown or logged ([W10](contracts/server-requests.md)). No new data leaves the device other than the user's chosen files to the user's own server. Logs carry codes, counts and item numbers only ([R-17](research.md)). Credential storage is unchanged (001 R-6 evaluation stands). |
| V Simplicity & bounded footprint | **Pass.** No new dependency; queue bounded at 2,000; pick walk bounded; streaming with a 64 KiB buffer, one chunk in flight; `Depth: 0`/`1` only; the metadata cache is untouched by the queue and eviction never touches queue rows; largest file stated (4 GiB - 1; `PUT` and chunk sizes in [R-22](research.md)); no previews in the tiles, no parallel chunks, no bulk upload, no retry machinery (YAGNI). |
| VI E-ink UX & battery | **Pass with a note.** Page buttons, coarse progress (<= 1 update per 2 s or 5 %), no animation, no polling. Network happens at start (the one account check), on explicit taps, and - the Auto Sync trigger the constitution allows - once, 10 s after the user's own action; it is switchable and **on by default**; it never fires at start, on Wi-Fi coming up, after a failure or after a send, and never switches Wi-Fi on. **Note:** one single-shot `QTimer` is added in `queue_bridge.cpp`; rule N3 gets exactly that allow-list entry with a self-test ([R-14](research.md), [R-18](research.md)). The UI shows the offline state and the time of the last refresh as before, and the queue screen the time of the last send. |
| VII Licensing | **Pass.** No code or text copied from Nextcloud; the protocol is reimplemented from the public documentation and RFCs 4918, 7232, 7233, 3986. The picker and the scheduler adapt patterns of notes-pocketbook (MIT, same author): attribution added to `NOTICE` and `licenses/manifest.txt` and the About box. No new component, no Nextcloud logo; the app stays unofficial (README). |
| VIII Quality gates | **Pass.** `pre-merge.sh` unchanged in shape; N2 becomes an allow-list (write methods only in `dav_write.cpp` and `queue_sender.cpp`; `PROPPATCH`/`LOCK`/`UNLOCK` forbidden everywhere), N3 gets one entry, new N6 (source files opened read-only), each with a self-test; `test_write_conditions` and the extended `test_no_background_network` act as rules ([R-18](research.md)). Version bump 0.1.0 -> 0.2.0 and `CHANGELOG.md` are the last phase of tasks.md; the ARM build takes the SDK only from `POCKETBOOK_SDK` / `-DSDK_PATH`. |
| IX Specs-driven & honest verification | **Pass.** Every server and device behavior of the write side is labelled unverified; fixtures carry `.unverified`; LV-W1..W12 and DT-W1..W9 are added to the checklists by the task list ([R-20](research.md)); results are recorded when obtained. |
| X English documentation | **Pass.** All artifacts, comments, identifiers, log keys and test names in English; German only in `qml/strings_de.tsv` and non-English test data (umlaut file names). |
| XI Consistent look and feel | **Pass with a note.** Only existing components plus a tile grid taken from the sibling's picker; Auto Sync wording, switch and place as in PB Notes and PB Office; no bordered text button, no animation. **Note:** `FolderPage` loses the Update, Favorites and Search tools (they stay on Home) to make room for Add, Select and Queue - a visible change of 001's page, owner point 3. |

Mechanical constraints (Security & Data Handling): the data folder, database and credentials file keep their permissions;
the queue adds no new file. **Technology constraints check**: C++17, Qt 6.8 Quick/QML, CMake, libcurl, OpenSSL, SQLite;
WebDAV with libcurl and the existing minimal XML parser (also for the upload-folder listing and sabre bodies); no new XML
or WebDAV library. App name `pbcloud` unchanged; no collision with PB Notes or PB Office. Passes.

**Post-design re-check (after Phase 1):** the design artifacts do not introduce a violation. The three things that came
closest are decided and contained: Replace never uses `Overwrite: T` (it cannot carry a condition on the target): it is a conditional
`DELETE` of the old file into the trash followed by a request with `Overwrite: F`, limited to files and never offered for folders (R-3); the final chunk `MOVE`
race (above); the 500/502/504 answers of a write are treated as `unknown` (R-7). No principle is deviated from without a
Complexity Tracking row.

### Open points for the owner (not violations)

1. **Top level of the picker.** The sibling lists seven curated folders. This plan adds the app's own download folder
   `PBCloud` as an eighth, because uploading an edited copy back is the use case of the spec's clarification 3. Say if the
   list should be exactly the sibling's seven (then uploading a downloaded copy needs a copy first) or have other entries.
2. **Wi-Fi and Auto Sync.** The spec says the app never turns Wi-Fi on and an offline Auto Sync leaves the queue waiting. PB
   Notes changed its own Auto Sync after a device test so that it asks the system to connect (its research R-3). This plan
   follows the spec as written; DT-W9 measures what a user with Wi-Fi normally off experiences. If the owner prefers the
   sibling's behavior, it is a one-line change in `QueueBridge` plus an amendment of the spec's assumption.
3. **FolderPage tools.** Update, Favorites and Search leave the folder page's bottom panel (they stay on Home) so that Add,
   Select and Queue fit at the usual touch size. The 001 UI walk and QML tests are updated accordingly.
4. **Replace is files only.** The spec (FR-021) lists Keep both / Replace / Skip for any name collision; this plan never offers
   Replace when the existing entry or the item is a folder (replacing a folder would delete it recursively), and
   treats a file-over-folder or folder-over-file collision as Keep both / Skip only. `/speckit-analyze` should flag the
   wording difference; the spec text should be narrowed to match (FR-021, Story 3 scenario 9).
5. **Sizes and delay.** 10 MiB chunks, the 10 s Auto Sync delay and the 2,000-item queue are working values chosen for the
   budgets above; none is a requirement of the spec.

## Project Structure

### Documentation (this feature)

```text
specs/002-manage-server-files/
├── plan.md              # This file (/speckit-plan command output)
├── research.md          # Phase 0 output: decisions R-1 .. R-22
├── data-model.md        # Phase 1 output: schema 4, queue state machine, listing overlay, effects on 001 records
├── quickstart.md        # Phase 1 output: how to validate on host, fake server, real server, device
├── contracts/
│   ├── server-requests.md   # every write request to the server, conditions, mapping, error keys (unverified)
│   ├── core-interfaces.md   # Qt-free C++ interfaces of the new core modules
│   └── bridge-qml.md        # bridges, properties, pages, UI rules, texts
├── checklists/
│   └── requirements.md  # specification quality checklist
└── tasks.md             # Phase 2 output (/speckit-tasks command - NOT created by /speckit-plan)
```

### Source Code (repository root)

```text
core/
├── include/pbc/
│   ├── dav_write.h  upload_session.h  queue_sender.h  check_outcome.h           # new: write requests, chunked upload, the run, the read-only check
│   ├── queue_types.h  store_queue.h  auto_sync.h                                # new: item model, store API (members of Store), scheduler
│   ├── new_name.h  server_caps.h  source_walk.h  upload_source.h                # new: names, capabilities, picker walk, source identity
│   ├── http_transport.h  dav_common.h  store.h  nc_server.h  error_keys.h       # changed (additive; upload_* multipart fields removed)
│   └── (all 001 headers kept)
└── src/                                                                          # one .cpp per header above; store_queue.cpp; store_migrations.cpp (3 -> 4);
                                                                                  # curl_transport.cpp (file-range body, delivery); log_codes.h (new codes)
bridge/
├── include/  queue_bridge.h  picker_bridge.h                                     # new; browse_bridge.h (overlay rows, select mode), connection_bridge.h (caps)
└── src/      the matching .cpp files; worker.cpp reused
app/
├── main.cpp                     # wires the two bridges, the picker root and top folders, the Auto Sync config
├── ui_script.{h,cpp}            # new verbs for the walk (pick, send, resolve); host build only
└── device_bridge.{h,cpp}        # unchanged (wifiConnected is used by Auto Sync)
qml/
├── pages/  FilePickerPage.qml  FolderChooserPage.qml  QueuePage.qml  (new)
│           FolderPage.qml  HomePage.qml  SettingsPage.qml  DetailsPage.qml  AboutPage.qml  (changed)
├── (existing components)  strings_de.tsv (German texts for every new string)  icons/ (existing set; a missing glyph added in style)
tests/
├── unit/         test_new_name  test_server_caps  test_auto_sync  test_queue_rules  test_queue_recover  test_overlay  test_upload_plan
│                 test_upload_folder_parse  test_write_answer_map  test_disambiguate_412  test_source_walk  test_source_hostile
│                 test_store_schema4  test_store_queue  test_http_transport_body_file  test_sabre_error_hostile …
├── integration/  test_write_conditions  test_upload_small  test_upload_chunked  test_upload_resume  test_unknown_outcome
│                 test_collisions  test_conflicts  test_move_copy  test_rename_mkdir  test_delete_trash  test_refusals
│                 test_local_follow  test_queue_kill  test_upload_curl (real libcurl, TLS server)  test_no_background_network (extended)
│                 test_budget_queue  test_budget_upload
├── bridge/       test_queue_bridge  test_picker_bridge  test_browse_bridge (extended)
├── qml/          file_picker, queue_page, folder_page_select (stubs extended)
├── support/      fake_nextcloud_server.h (extended: write side, uploads, quota, locks, trash flag, failure injection)
├── tools/        pbc_fake_nextcloud.cpp (console commands: touch, lose-next-answer, drop-after, quota, lock, no-trash, ignore-conditions)
├── walks/        03-upload-and-manage.walk  (CTest ui_walk); 01 and 02 updated for the FolderPage tools
└── fixtures/nextcloud/   anonymized write-side answers with the .unverified suffix
scripts/          check-constitution.sh (N2 allow-list, N3 entry, N6, self-tests), run-host-demo.sh (picker root, console), probe-device.sh (new probes)
docs/             live-server-checklist.md (LV-W1..W12), device-test-checklist.md (DT-W1..W9), device-findings.md
README.md  INSTALL.md  CHANGELOG.md  NOTICE  licenses/manifest.txt  VERSION (0.2.0)
```

**Structure Decision**: keep the four-layer layout unchanged (Principle II). The write side goes into new core modules next
to the read side, with `dav_write.cpp` and `queue_sender.cpp` as the only files that name a write method, so rule N2 can be an
allow-list of two names. Platform behavior (Wi-Fi, the storage root, free space) enters the core only through the existing small
interfaces and `SourceRules`. The fake Nextcloud grows the write side so the demo server and the tests exercise the same
contract.

### Suggested delivery order (input for `/speckit-tasks`)

1. **Foundations**: schema 4 and its migration test; `queue_types`/`store_queue` with recovery rules; transport additions
   (file-range body, `delivery`) and their real-libcurl tests; `new_name`, `server_caps`, `source_walk`, `upload_source`;
   the fake server's write side skeleton; rules N2/N3/N6 with self-tests; new error keys.
2. **US1 Upload (picker and queue)**: `PickerBridge`, `FilePickerPage`, `QueueBridge.addUploads`, overlay rows in the
   folder list, "not synced" marks, folder pick (US1 scenarios 6-8). Nothing is sent yet.
3. **US2 Send**: `dav_write`, `QueueSender` (small `PUT` first), `QueuePage`, Send, `unknown` handling and the outcome check,
   then chunked upload and resume (`upload_session`), then `AutoSyncScheduler` + the timer + the Settings switch.
4. **US3 Collisions and conflicts**: the name question, keep-both, replace with confirmation, the held-item choices,
   the final-`MOVE` pre-check, 412 disambiguation.
5. **US4 New folder and rename**; **US5 Move and copy** (`FolderChooserPage`, select mode); local-copy and mark follow.
6. **US6 Delete** (capability, dialogs, conflict, no-trash path); **US7 refusals** (the key map and every message; most of
   the mapping is built in step 3 and completed here).
7. **Polish**: budget tests, LV/DT checklist entries, FolderPage/Home/About text changes, walk 03, README/INSTALL/CHANGELOG/NOTICE,
   German texts completed, version 0.2.0, `package-device.sh` run.

US1-US3 together are the P1 minimum (upload, send, never overwrite); each later story can be stopped after its own tests pass.
US7's mapping is needed by US2, so its tests start in step 3.

## Complexity Tracking

> Filled because several additions go beyond the smallest possible change; none violates the constitution.

| Addition | Why Needed | Simpler Alternative Rejected Because |
|----------|------------|-------------------------------------|
| `queue_items` table with dependencies, a state machine and a recovery rule | Principle III: pending changes persisted before any request, never lost, idempotent, `unknown` kept; folder picks need parents created first | A file-per-item spool or an in-memory queue has two sources of truth and cannot express dependencies, conflict state and the unknown-outcome rule |
| Chunked upload v2 with resume through the upload-folder listing | Spec FR-014: large files in chunks, an interruption costs one chunk | A single `PUT` restarts a 500 MB file on every Wi-Fi drop; no other resumable protocol exists on the server |
| The `PROPFIND` pre-check before the final chunk `MOVE` and before a replace by `MOVE`/`COPY` | The conditions the server honors there are undocumented (api-notes 6) or impossible (`Overwrite: T` carries none on the target); Principle III forbids an unconditional overwrite | Trusting the headers blindly risks a silent overwrite on a server that ignores them; refusing replace entirely contradicts FR-021. The residual race of a few seconds is stated and closed or accepted by LV-W6 |
| Unconditional `MKCOL`/`PUT`/`DELETE` on the app's own upload session under `/dav/uploads/` | These touch only temporary chunks the app created, not user data; conditions there add nothing and the `DELETE` is the cleanup that keeps unfinished files out of sight (FR-016) | A conditional form does not exist for these resources; leaving abandoned sessions would rely on the 24-hour expiry alone |
| One `QTimer` (single shot) in `queue_bridge.cpp` | Auto Sync's quiet period of 10 s (constitution VI allows exactly this trigger) | A timer in QML or a thread that sleeps would be untestable on the host and break rule N3; the scheduler class keeps the logic Qt-free and time-parameterised |
| Source walk and path checks (`source_walk`, `upload_source`) with `O_NOFOLLOW` | Principle IV: an upload must never be able to read the app's database or credentials through a crafted link or a swapped file | Trusting the picker's list is a time-of-check/time-of-use hole; the checks cost one `realpath` and one `fstat` per file |
| SHA-256 pass before a large upload | Puts the checksum into a request header before the body (a header cannot follow the body) and gives the outcome check and later downloads a strong comparison | Skipping it leaves the unknown-outcome check on the size alone; the cost is one sequential read (DT-W4) |
| Extending the fake Nextcloud with a write side and failure injection (lost answer, ignored conditions) | Principle I: boundary behavior is proven against a repeatable in-process fake | Hand-scripted queues per test duplicate server logic and cannot model chunk folders, ETag changes and lost answers consistently |
| Removing the multipart `upload_file` fields from `Request` | The new file-range body supersedes them; leaving two ways to send a file doubles the review surface | Keeping the unused field invites misuse and leaves untested code in a security-sensitive struct |


## Implementation notes (2026-10-21)

See "Implementation notes" at the end of [research.md](research.md). Changes to this plan: Replace never uses `Overwrite: T` (conditional delete into the trash, then `Overwrite: F`);
the queue logic is in `core/` (`queue_actions`, `new_name`, `source_walk`, `upload_source`, `upload_session`, `queue_sender`, `check_outcome`, `auto_sync`) and the bridges are thin;
`FolderChooserBridge` serves the destination chooser; the write-method allow-list of rule N2 also names `app/probe/pbc_probe.cpp` (one `PUT`, DT-W1); `_FILE_OFFSET_BITS=64` is set for all targets.
