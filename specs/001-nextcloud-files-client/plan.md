# Implementation Plan: Read-only Nextcloud Files Client

**Branch**: `feature/001-nextcloud-files-client` | **Date**: 2026-10-20 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/001-nextcloud-files-client/spec.md`

## Summary

PB Cloud becomes a read-only Nextcloud files client for the InkPad One: connect (app password or browser login),
browse folders page by page from a bounded local cache, download files safely (temporary file, resume, atomic
rename, free-space and FAT checks), keep files offline and update them on request, show favorites, search by name,
and manage local space. Nothing is ever written to the server, and the network is used only at app start (one account
check) and on explicit taps.

Technical approach (details in [research.md](research.md)): extend the existing Qt-free C++17 core with a WebDAV
listing parser and fetchers, a server probe and Login Flow v2 client, a download engine with a persisted journal,
a local-name sanitizer, an entry-state function and an Update runner; add schema 2 to the SQLite store (cache,
local copies, marks, favorites, download journal); expose four bridges (`connection`, `browse`, `transfer`, `space`)
to QML pages built only from the sibling apps' components. Three existing seams change additively: the HTTP request
(append, progress/cancel, identity encoding), the DAV constants, the store. The test base is a stateful in-process fake
Nextcloud plus a few tests through the real libcurl transport against the existing local TLS server.

## Technical Context

**Language/Version**: C++17 (core, bridge); Qt 6.8 Quick/QML for the UI (device runs Qt 6.10.3, SDK sysroot 6.8.2); CMake

**Primary Dependencies**: libcurl 8.18.0 (static on the reader, system on the host), OpenSSL (`libcrypto` for the checksum
check, already a shared library of the reader and already linked through libcurl), SQLite (vendored amalgamation).
**No new third-party dependency.** The QR encoder and the SHA/MD5 use are in-house code or the existing OpenSSL.

**Storage**: SQLite `pbcloud.db` schema 2 in `/mnt/ext1/system/pbcloud/`; the `credentials` file (app password only);
downloaded files in `/mnt/ext1/PBCloud/` on the reader's FAT storage; `custom-ca.pem` optional in the data folder

**Testing**: the project's own tiny test framework (`tests/support/test_support.h`) under CTest; unit, integration
(fake Nextcloud), bridge (Qt Test, offscreen), QML self-test, script checks; kill/interruption harness; hostile-input
fixtures; budget tests (unverified until measured on the device)

**Target Platform**: PocketBook InkPad One (RK3566, armhf user space, Qt on the device); host Linux x86-64 for the
complete core test suite (constitution II)

**Project Type**: desktop-style Qt Quick application with a Qt-free core library (device app plus host tests)

**Performance Goals** (budgets, **unverified until measured on the device**; they restate spec SC-004 to SC-014):

| Quantity | Budget |
|----------|--------|
| Cold start to the first usable screen | ≤ 5 s |
| Cached folder of 1,000 entries shown | ≤ 2 s; page turn ≤ 1 s |
| First page of an uncached 1,000-entry folder over home Wi-Fi | ≤ 10 s |
| Offline name search in a cache of 5,000 entries | ≤ 5 s including typing (query itself ≤ 100 ms) |
| Download progress screen updates | ≤ 1 per 2 s or per 5 % |

**Constraints**:

| Quantity | Budget |
|----------|--------|
| RAM | peak ≤ 150 MB (`VmHWM`) while browsing a 1,000-entry folder and while streaming a 500 MB file; transfers use a 64 KiB buffer, no file in memory |
| Installed size | package ≤ 25 MB (the existing `package-device.sh` gate, stricter than the 30 MB of SC-014) |
| Metadata cache | ≤ 50,000 entries, ≤ 2,000 folders, database file ≤ 50 MB |
| Largest supported file | 4 GiB − 1 byte (FAT); 4 GiB or more is refused before any byte is written. Exercised by budget tests: 1 GiB on the host, 500 MB on the device |
| Entries per folder | 10,000 (more: `listing_too_large`) |
| Update | 500 folders, depth 20 per run; the rest is reported as not checked |
| Network | none in the background; one account check at start when Wi-Fi is already on; everything else on a tap |
| Server contract | Nextcloud 30 or newer assumed (unverified); no write method is ever sent |

**Scale/Scope**: one account; a personal library of up to a few thousand files and a few hundred folders; 6 user
stories, 39 functional requirements; about 12 new core modules, 4 bridges, 8 new QML pages

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design (second table below).*

Constitution version 1.0.0 (ratified 2026-10-19). Result: **pass** with two noted points (XI, VI).

| Principle | Pre-research verdict | Post-design verdict and where it is satisfied |
|-----------|---------------------|-----------------------------------------------|
| I Test-First | Pass | Every module has failing-first tests: unit, fake-server integration, real-libcurl tests, hostile-input and kill tests ([research R-17](research.md)). Contract tests exist for the multistatus listing, the status/OCS answers, the Range answers and the Login Flow answers ([contracts/server-requests.md](contracts/server-requests.md)). Parsers: malformed, truncated, oversized, hostile, partial-error fixtures are listed per parser. |
| II Host-testable core | Pass | All new logic is in `core/`, Qt-free; platform facts enter through `StorageInfo`, `Clock`, `HttpTransport`, the Wi-Fi callback ([contracts/core-interfaces.md](contracts/core-interfaces.md)). The ARM target builds the same sources. |
| III No silent data loss | Pass | Temporary file in the target directory, atomic rename, size + checksum check, persisted journal before the first request, resume with a strong validator or clean restart, conflict page with no automatic choice, removal of a local copy never touches the server, a server deletion never removes a local copy, free space checked first ([research R-8, R-10](research.md), [data-model](data-model.md)). The "conditional writes / pending uploads" rules are not applicable: there are no server writes. |
| IV Security & Privacy | Pass with a note | TLS always verified, https only, same-host redirect rule, no cross-host poll endpoint; only app passwords; server names sanitized (R-9); downloads opened only by the reader's viewer; credential storage described honestly and a better option evaluated and rejected ([R-6](research.md)); no secrets, addresses, names or search terms in logs (R-15, rule N4). |
| V Simplicity & bounded footprint | Pass | No new dependency; Depth-1 only; visited folders only; bounded cache with defined eviction; streaming transfers; budgets stated above; largest file stated. |
| VI E-ink UX & battery | Pass with a note | Page buttons, coarse progress, no animation, no polling, no timers except the bounded download wait (rule N3). Note: the **Auto Sync** setting is **not offered** in this feature (default: not applicable, because there is no local change to push and nothing it could react to); the sibling wording returns with the first upload feature, with its default stated in that plan. The UI always shows offline state and the age of the listing. |
| VII Licensing | Pass | No code or text copied from Nextcloud; behavior reimplemented from the API notes and RFCs; QR encoder and all other code are original, MIT; no new `NOTICE` entry needed (the task list re-checks); the app stays unofficial and uses no Nextcloud logo. |
| VIII Quality gates | Pass | New rules N1–N5 with self-tests ([R-16](research.md)); `pre-merge.sh` unchanged; version bump to 0.1.0 and `CHANGELOG.md` are last-phase tasks; SDK location only through `POCKETBOOK_SDK`/`-DSDK_PATH`. |
| IX Specs-driven & honest verification | Pass | All server and device behavior is labelled unverified; fixtures carry `.unverified`; new LV-N1..LV-N9 and DT-NC-1..DT-NC-10 items are added to the checklists in the task list. |
| X English documentation | Pass | All artifacts, comments, identifiers and log keys in English; German only in `qml/strings_de.tsv` and non-English test data. |
| XI Consistent look and feel | Pass with a note | Only existing components (TopBar, BottomPanel, IconButton, InkRow, ModelPager/PageButtons, dialogs, Style). Home/Settings/About keep the structure. Note: no Auto Sync switch (see VI). |

Mechanical constraints (Security & Data Handling): app data directory, database and credentials file use restrictive
permissions where the platform allows, and the measured protection is reported (`credentials_protection`, shown in
Settings).

**Technology constraints check**: C++17, Qt 6.8 Quick/QML, CMake, libcurl, OpenSSL, SQLite; WebDAV with libcurl and the
existing minimal XML parser; no new XML or WebDAV library. App name `pbcloud`, data folder `pbcloud`, download folder
`PBCloud`: no collision with PB Notes or PB Office. Passes.

### Open points for the owner (not violations)

1. **Resolved:** the "short code" of the first spec version was replaced by "a link and a QR code" (FR-003, US1 scenario 2) on the owner's decision; [research R-4](research.md) records why.
2. **Library folder name** `PBCloud` and its visibility to the reader's library are unverified (DT-NC-3).
3. **Dates** use the reader's time zone read from its configuration; the key is unverified (DT-NC-6) with a UTC fallback
   that is labelled as such.

## Project Structure

### Documentation (this feature)

```text
specs/001-nextcloud-files-client/
├── plan.md              # This file (/speckit-plan command output)
├── research.md          # Phase 0 output: decisions R-1 .. R-19
├── data-model.md        # Phase 1 output: schema 2, entry state, download job states
├── quickstart.md        # Phase 1 output: how to validate on host, fake server, real server, device
├── contracts/
│   ├── server-requests.md   # every request to the server, parsing rules, error keys (unverified)
│   ├── core-interfaces.md   # Qt-free C++ interfaces of the new core modules
│   └── bridge-qml.md        # bridges, properties, pages, UI rules
├── checklists/
│   └── requirements.md  # specification quality checklist
└── tasks.md             # Phase 2 output (/speckit-tasks command - NOT created by /speckit-plan)
```

### Source Code (repository root)

```text
core/
├── include/pbc/
│   ├── remote_path.h  local_name.h  storage_info.h  hash.h  qr_code.h          # new: paths, names, platform seam, digests, QR
│   ├── nc_server.h  nc_login.h  nc_listing.h                                    # new: probe, Login Flow v2, WebDAV listing
│   ├── downloader.h  entry_state.h  update.h  store_files.h                     # new: download engine, states, Update, store API
│   ├── error_keys.h  auth_guard.h  age_text.h  reader_zone.h  local_files.h   # new: stable keys, 401 guard, age/date text, copy removal
│   ├── http_transport.h  dav_common.h  store.h                                  # changed (additive)
│   └── (existing files kept; sync_scheduler.h and paging.h removed)
└── src/                                                                          # one .cpp per header above, store_files.cpp,
                                                                                  # store_migrations.cpp (schema 1 -> 2), log_codes.h
bridge/
├── include/  connection_bridge.h  browse_bridge.h  transfer_bridge.h  space_bridge.h  qr_image_provider.h
└── src/      the matching .cpp files; worker.cpp reused
app/
├── main.cpp                     # wires the bridges, the image provider, the account check at start
└── device_bridge.{h,cpp}        # downloadRoot, openFile inside the root
qml/
├── pages/  HomePage.qml(changed)  FolderPage.qml  DetailsPage.qml  ConflictPage.qml  UpdatePage.qml
│           FavoritesPage.qml  SearchPage.qml  SpacePage.qml  LoginPage.qml
│           SettingsPage.qml(changed)  AboutPage.qml(changed)
├── (existing components)  strings_de.tsv (German texts)  icons/ (existing set)
tests/
├── unit/         test_remote_path  test_local_name  test_name_hostile  test_listing_parse  test_listing_hostile
│                 test_entry_state  test_preflight  test_qr_code  test_checksums  test_server_status  test_error_keys
│                 test_cache_eviction  test_store_schema2 …
├── integration/  test_login_flow  test_account_check  test_browse_cache  test_download_basic  test_download_resume
│                 test_download_kill  test_download_drop  test_download_curl (real libcurl, TLS server)
│                 test_update  test_favorites_search  test_no_background_network  test_budget_cache  test_budget_download
├── bridge/       test_connection_bridge  test_browse_bridge  test_transfer_bridge  test_space_bridge
├── support/      fake_nextcloud_server.h (new)  fake_storage.h (new)  (existing fake_transport.h, tls_test_server.h, kill_harness.h …)
├── tools/        pbc_fake_nextcloud.cpp  (HTTPS demo server for scripts/run-host-demo.sh)
└── fixtures/nextcloud/   anonymized *.xml / *.json answers with the .unverified suffix
scripts/          check-constitution.sh (rules N1–N5), run-host-demo.sh (starts the fake server), probe-device.sh (new probes)
docs/             live-server-checklist.md (LV-N1..LV-N9), device-test-checklist.md (DT-NC-1..DT-NC-10), device-findings.md
README.md  INSTALL.md  CHANGELOG.md  VERSION (0.1.0)
```

**Structure Decision**: keep the baseline's four-layer layout unchanged (Principle II). New logic goes into new
core modules rather than into existing ones, so the sibling-derived code stays recognizable. Platform behavior
(free space, file-system limit, Wi-Fi, opening a book) enters the core only through small interfaces. The fake
Nextcloud lives in `tests/support` and is reused by the demo server, so the demo exercises the same contract the
tests check.

### Suggested delivery order (input for `/speckit-tasks`)

1. **Foundations**: schema 2 and its migration test; transport additions (append, progress, identity); `remote_path`,
   `local_name`, `storage_info`; `fake_nextcloud` skeleton; new check rules N1–N5 with self-tests.
2. **US1 Connect**: `nc_server`, error keys, `nc_login`, `ConnectionBridge`, Settings page, honest storage text; QR
   encoder and image provider last (separable).
3. **US2 Browse**: `nc_listing` parser and fetcher, store listing API with eviction, `BrowseBridge`, `FolderPage`, sort,
   paging, cached/offline states, availability markers.
4. **US3 Download**: preflight, `Downloader` with journal, resume, verification, conflict handling,
   `TransferBridge`, `DetailsPage`, `ConflictPage`, open in viewer; device probe DT-NC-1 early.
5. **US4 Offline/Update**: marks, `entry_state`, `Updater`, `UpdatePage`, state words in lists.
6. **US5 Favorites/search**: favorites fetch and cache, offline search, server search, pages.
7. **US6 Space**: `SpaceBridge`, `SpacePage`, cache bound and clear.
8. **Polish**: budget tests, checklists LV/DT, baseline clean-up (R-18), README/INSTALL/CHANGELOG/NOTICE, version 0.1.0,
   German texts completed, `package-device.sh` run.

US1–US3 together are the P1 minimum; each later story can be stopped after its own tests pass.

## Complexity Tracking

> Filled because three additions go beyond the smallest possible change; none violates the constitution.

| Addition | Why Needed | Simpler Alternative Rejected Because |
|----------|------------|-------------------------------------|
| In-house QR encoder (`qr_code.cpp`, about 300 lines) | The login link is one long URL; typing it into a phone is the failure the browser login exists to avoid. The reader has no good browser | A third-party library adds an LGPL/ARM dependency (constitution V, VII); typing 120+ characters defeats the story. Separable: dropping it keeps a working text link |
| Linking `pbc_core` to OpenSSL `libcrypto` for SHA-256/SHA-1/MD5 file digests | The server publishes checksums (`oc:checksums`); verifying them is the constitution's requirement for a completed download "where the server offers one" | A hand-written hash would be new security-sensitive code; OpenSSL is already in the default dependency set and already a shared library of the reader that libcurl links. Host tests already link it |
| Download journal table and a recover step at start | A kill at any point must leave no partial file visible and allow resume (Principle III, SC-006) | Scanning for `.part` files without a record cannot resume safely (no validator, no expected size) and cannot tell an app file from a user's file |
| A stateful fake Nextcloud (`tests/support/fake_nextcloud_server.h`) plus a demo HTTPS server | Principle I requires contract tests against an in-process fake; the scripted `FakeTransport` queue cannot model Range, ETag changes and drops consistently | Hand-scripted queues per test duplicate server logic and drift from the contract |
