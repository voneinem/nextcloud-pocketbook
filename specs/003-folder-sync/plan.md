# Implementation Plan: Folder Synchronization

**Branch**: `feature/003-folder-sync` | **Date**: 2026-10-21 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/003-folder-sync/spec.md`

## Summary

PB Cloud can keep chosen folders in step. The user **pairs** a server folder with a folder on the reader, with a direction
(download only by default, upload only, or two-way) and per-pair limits on file count and total size. A comparison runs only on
the Sync button, at app start with Wi-Fi already on (per pair, off by default), or - Auto Sync, off by default per pair and
subordinate to the global switch - ten seconds after the user's own change; it **never** runs in the background and never turns
Wi-Fi on. A comparison produces a **plan** (downloads, uploads, skips with reasons, keep-both conflicts, deletion proposals,
counts and sizes, free space) and changes nothing. A comparison started by anything other than a tap only stores the plan and
marks the pair "plan ready"; **only the user's approval** of the reviewed plan transfers anything, and approving never confirms a
deletion. A file missing on one side is a **proposal** that needs its own confirmation; a file changed on both sides keeps both
versions; a plan over a limit is refused as a whole. Every sync is resumable and idempotent because a per-file **sync record**
advances only after a file is complete on both sides.

Technical approach (details in [research.md](research.md)): add a sync layer to the Qt-free core (`sync_types`, `sync_rules`,
`store_sync`, `sync_scan`, `sync_planner`, `sync_runner`, `sync_scheduler`, `conflict_name`), a pure planner whose decision table is
[contracts/sync-rules.md](contracts/sync-rules.md), schema 5 (`sync_pairs`, `sync_files`, `sync_plans`, `sync_plan_entries` and
three columns on `queue_items`), **downloads through the existing Downloader** (per-pair target root and a strict-version check)
and **uploads and server deletions through the 002 queue and sender** (so persistence, chunking, conditional writes, conflicts,
"outcome unknown" and refusal messages are inherited, not re-implemented). One new bridge (`SyncBridge`, with the second allowed
`QTimer`), four new pages (`SyncPage`, `PairPage`, `PlanPage`, `ResultPage`) and small changes to `HomePage`, `SettingsPage`,
`DetailsPage`, `FilePickerPage` (folder mode) and `QueueBridge`. No new dependency and **no new server request**: listings
(`PROPFIND Depth: 1`), `GET`, and the 002 write operations. The test base is the in-process fake Nextcloud with a few additions
(checksums in listings, partial listings, removals, hostile names, unavailable entries), the kill harness, and a few tests through
the real libcurl transport.

## Technical Context

**Language/Version**: C++17 (core, bridge); Qt 6.8 Quick/QML for the UI (device runs Qt 6.10.3, SDK sysroot 6.8.2); CMake

**Primary Dependencies**: libcurl 8.18.0 (static on the reader, system on the host), OpenSSL `libcrypto` (SHA-256/SHA-1/MD5 for
checksums, already linked), SQLite (vendored amalgamation). **No new third-party dependency.**

**Storage**: SQLite `pbcloud.db` **schema 5** (`sync_pairs`, `sync_files`, `sync_plans`, `sync_plan_entries`; `queue_items` + `pair_id`,
`sync_rel`, `sync_role`) in `/mnt/ext1/system/pbcloud/`; the user's files are only read (uploads) or written through the Downloader
(downloads); the database holds identities (versions, sizes, times, hashes), never file content

**Testing**: the project's own test framework (`tests/support/test_support.h`) under CTest; unit, integration against the fake
Nextcloud, real-libcurl tests against the local TLS server, bridge (Qt Test, offscreen), QML self-test, script checks, the
kill/interruption harness, hostile-input fixtures, budget tests (unverified until measured on the device), the host UI walk

**Target Platform**: PocketBook InkPad One (RK3566, armhf, Qt on the device); host Linux x86-64 for the complete core test suite
(constitution II)

**Project Type**: desktop-style Qt Quick application with a Qt-free core library (device app plus host tests)

**Performance Goals** (budgets, **unverified until measured on the device**; they restate spec SC-001 to SC-014):

| Quantity | Budget |
|----------|--------|
| Add a pair | <= 6 steps from the Sync screen; saving makes no request and no file change |
| Compare a 1,000-file pair | plan shown within 10 s on a simulated fast connection; page turn <= 1 s |
| Auto Sync comparison start after the user's last change | 10 s (+-1 s); only the affected pair |
| Start-up compare | once per pair with the switch on, only with Wi-Fi on, after the account check; adds no more than the listings it needs |
| Run progress screen updates | <= 1 per 2 s or per 5 % |
| Cold start | <= 5 s (unchanged; `sync_recover` adds no network and <= 100 ms for 20,000 rows) |

**Constraints**:

| Quantity | Budget |
|----------|--------|
| RAM | peak <= 150 MB (`VmHWM`) during a 1,000-file compare and during a 500 MB transfer; 64 KiB transfer buffer; two sorted vectors of at most 5,000 entries, no tree, no file in memory |
| Installed size | package <= 25 MB (the `package-device.sh` gate); expected growth < 300 KB |
| Bounds | 8 pairs; per scan side 500 folders, depth 20, 5,000 files; 5,000 plan entries; 20,000 records; database growth <= 10 MB at the bounds |
| Pair limits (defaults) | 500 files and 2 GiB per pair, configurable 1-5,000 files and 1 MiB-64 GiB; free-space reserve max(256 MiB, 2 % of the volume) |
| Largest file | 4 GiB - 1 byte on the reader (FAT); larger server files are skipped with the reason |
| Network | none in the background; the one account check at start; comparisons only on the three triggers (start-up, tap, Auto Sync after the user's own change); transfers only after the user approves a plan; no retry timer, no polling |
| Server contract | Nextcloud 30 or newer assumed (unverified); no new request kinds |
| Defaults of the triggers (constitution VI) | start-up sync **off** and Auto Sync **off** for a new pair (the spec's working default); Auto Sync also needs the global Auto Sync of 002 |

**Scale/Scope**: one account; a personal library; pairs of up to a few hundred files; 9 user stories, 53 functional requirements;
about 8 new core modules, 1 new bridge, 4 new QML pages, changes to 5 pages, 2 core modules and the queue bridge

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design (verdict column).*

Constitution version 1.0.0 (ratified 2026-10-19). Result: **pass** with three noted points (III records advance after the file is
complete; III reader-side deletion is unrecoverable by design after confirmation; VI the second `QTimer`) and four owner points
below. Principles III, VI and VIII are where this feature touches the constitution's most specific text.

| Principle | Verdict and where it is satisfied |
|-----------|-----------------------------------|
| I Test-First | **Pass.** Every module has failing-first tests (research R-19): a table-driven planner test for every row of the decision table in every direction, scan tests with hostile, partial and oversized input, store tests for schema 5, integration tests against the fake for each story, the kill harness at every named step (SC-004), real-libcurl tests, and the extended `test_no_background_network` as a rule. Boundary behavior (WebDAV listings, the 002 write operations, SQLite migration 4 -> 5) is tested against the fake and fixtures, never a real server or account. A change of the Downloader or the queue contract carries a contract test. |
| II Host-testable core | **Pass.** All logic - rules, scans, planner, runner, scheduler, conflict names, store - is in `core/`, Qt-free; `bridge/` holds only `SyncBridge` (property wrapper and the one `QTimer`); platform facts enter through `StorageInfo`, `Clock`, `HttpTransport`, `SourceRules`, `wifi_up`. The planner is a pure function; the scheduler takes time as a parameter, so Auto Sync is proven without sleeping. |
| III No silent data loss | **Pass with notes.** (1) Downloads use the Downloader unchanged in its guarantees (temporary file, atomic rename after size/checksum, resume, free-space check); a reader file the user changed is never overwritten (conflict -> keep both). (2) Uploads and server deletions are 002 queue items: persisted before any request, conditional (`If-Match`/`If-None-Match`), conflicts and unknown outcomes held, never repeated silently; a server deletion goes to the trash and is refused without one. (3) Nothing is deleted by a sync on its own: a missing file is a proposal that needs its own named confirmation, approving a plan never confirms one, and a changed file is never offered for deletion ([R-10](research.md)). (4) Records advance only after a file is complete on both sides, in the transaction of the work they record; the only window ("file complete, record not yet written") is closed by identity checks and ends in a visible conflict that keeps both versions, never a loss ([R-3](research.md)). (5) **Note - reader-side deletion:** the reader has no trash; a confirmed deletion of an *unchanged* synced copy is irreversible by this app, and the dialog says so; the function re-checks the file against its record and refuses a changed one. (6) A plan is checked against free space and a reserve before approval, again before every download; a full or read-only card is reported. (7) Removing a pair, changing a direction or lowering a limit never touches a file. |
| IV Security & Privacy | **Pass.** TLS and https-only unchanged (all traffic goes through `DavSession`; same-host rule; no credentials after a redirect to another host). Names from the server are untrusted: only `choose_local_path` builds a reader path inside the pair's folder (rule N7), links are never followed on either side, hidden files and the app's own temporary files are skipped, the reader folder of a pair cannot be the storage root, the app's data folder or download folder, or contain them ([R-2](research.md)). Reader names sent to the server pass `check_new_name`. Downloaded files stay untrusted data. Pairs and sync records hold identities only; logs carry codes, counts and ids, never names, paths, addresses or tokens ([R-17](research.md)). No data goes anywhere but the user's own server. Credential storage unchanged (001 R-6 stands). |
| V Simplicity & bounded footprint | **Pass.** No new dependency; every structure is bounded (8 pairs, 5,000 files per scan side, 5,000 plan entries, 20,000 records); walks use `Depth: 1` and read listings transiently without cache rows; streaming with a 64 KiB buffer; a flat merge of two sorted vectors instead of a tree; the largest file is stated (4 GiB - 1); no live watching, no parallel transfers, no retry machinery, no new request kind (YAGNI). |
| VI E-ink UX & battery | **Pass with a note.** No background activity. Network at start (the account check, and - only for pairs whose switch the user turned on - one comparison per pair when Wi-Fi is already on), on taps, and the one trigger the constitution allows: Auto Sync after the user's own change, switchable, **default off** per pair and gated by the global switch; it never fires at start, on Wi-Fi coming up, after a failure or a finished run, and never switches Wi-Fi on. Unattended comparisons only prepare a plan. Coarse progress (<= 1 update per 2 s or 5 %), page-based lists, no animation. **Note:** `sync_bridge.cpp` is the second allow-list entry of rule N3 (one single-shot `QTimer`) with a self-test ([R-18](research.md)). The UI shows the offline state and the age of each plan. |
| VII Licensing | **Pass.** No code or text copied from Nextcloud; behavior is reimplemented from the public API documentation and RFCs 4918, 7232, 7233, 3986. No new component; `NOTICE` and the About box gain nothing; the app stays unofficial (README). |
| VIII Quality gates | **Pass.** `pre-merge.sh` unchanged in shape; N3 and N5 allow-lists are extended by exactly the new files, new rule N7 (paths in the sync modules only through the audited constructors), each with a self-test; `test_no_background_network` extended; version bump 0.2.0 -> 0.3.0 and `CHANGELOG.md` are the last phase of tasks.md; the ARM cross-build takes the SDK only from `POCKETBOOK_SDK` / `-DSDK_PATH`. |
| IX Specs-driven & honest verification | **Pass.** Every server and device behavior this feature depends on is labelled unverified; fixtures carry `.unverified`; LV-S1..S8 and DT-S1..S8 are added to the checklists by the task list ([R-20](research.md)); results are recorded when obtained. |
| X English documentation | **Pass.** All artifacts, comments, identifiers, log keys and test names in English; German only in `qml/strings_de.tsv` and non-English test data (umlaut file names). |
| XI Consistent look and feel | **Pass with a note.** Only existing components plus a folder-only mode of the file browser; Auto Sync wording, switch and place as in PB Notes and PB Office; no bordered text button, no animation. **Note:** the Home page gets one more tool (Sync); `FolderPage` is not changed. |

Mechanical constraints (Security & Data Handling): the data folder, database and credentials file keep their permissions; the
sync adds no new file. **Technology constraints check**: C++17, Qt 6.8 Quick/QML, CMake, libcurl, OpenSSL, SQLite; WebDAV with
libcurl and the existing minimal XML parser; no new library. App name `pbcloud` unchanged. Passes.

**Post-design re-check (after Phase 1):** the design artifacts do not introduce a violation. The three places that came closest are
decided and contained: (a) a record that advances in the same transaction as the work it records, with the identity check
closing the one remaining window (R-3); (b) the reader-side deletion, which exists only as a confirmed, re-checked, named action
on an unchanged copy (R-10); (c) the conflict copies, which are one-sided by design so that the next sync does not bounce them
(R-9). No principle is deviated from without a Complexity Tracking row.

### Open points for the owner (not violations)

1. **Defaults of the triggers.** The spec's working default is start-up sync off and Auto Sync off for a new pair. Because an
   unattended comparison now only prepares a plan and can change nothing, "on" would be safe and more convenient; the cost is
   listing requests at every start. The default is one line in `default_pair()`; say if you prefer on.
2. **Reader-side deletion cannot be undone.** The reader has no trash. A confirmed "Delete on the reader" of an unchanged synced
   copy removes the file for good. The alternative (move to a hidden `.pbc-trash` folder) would fill the card unnoticed, which this
   feature exists to prevent, so it is not planned; the confirmation says so plainly.
3. **A diverged conflict stays diverged.** After keep-both, the two originals are never transferred again by the sync and are listed
   as "kept both" in every plan until the user removes one of them with ordinary file management (then it becomes a normal
   deletion proposal). A third change to a diverged file is therefore not synced; this is the price of never bouncing copies.
4. **Reader-side change detection is size plus modification time** (with a hash tie-break when a hash is known), the same rule as
   feature 001's "changed on the reader". A tool that rewrites the time without changing content makes a file look changed; the
   result is a conflict that keeps both versions, never a loss. DT-S2 measures how stable FAT times are in practice.
5. **Sizes and times.** 500 files / 2 GiB default limits, the 256 MiB / 2 % reserve, the 1-hour plan age and the 10 s Auto Sync
   delay are working values chosen for the budgets above; none is a requirement of the spec.

## Project Structure

### Documentation (this feature)

```text
specs/003-folder-sync/
├── plan.md              # This file (/speckit-plan command output)
├── research.md          # Phase 0 output: decisions R-1 .. R-22 and the FR traceability table
├── data-model.md        # Phase 1 output: schema 5, state machines, vocabulary of reasons and keys
├── quickstart.md        # Phase 1 output: how to validate on host, fake server, real server, device
├── contracts/
│   ├── sync-rules.md        # the planner's decision table, direction wall, limits, conflict naming, invariants (no new server request)
│   ├── core-interfaces.md   # Qt-free C++ interfaces of the new core modules and the additive changes to 002/001 seams
│   └── bridge-qml.md        # SyncBridge, pages, UI rules, texts
├── checklists/
│   └── requirements.md  # specification quality checklist
└── tasks.md             # Phase 2 output (/speckit-tasks command - NOT created by /speckit-plan)
```

### Source Code (repository root)

```text
core/
├── include/pbc/
│   ├── sync_types.h  sync_rules.h  sync_scan.h  sync_planner.h  conflict_name.h   # new: types, pair rules, both scans, the pure planner, conflict names
│   ├── sync_runner.h  sync_scheduler.h  store_sync.h                              # new: compare/run/apply, per-pair quiet period, Store members for the sync tables
│   ├── downloader.h  queue_actions.h  queue_sender.h  queue_types.h               # changed (additive: target_root/strict_version; add_sync_uploads; only_group; pair_id columns)
│   ├── store.h  local_files.h  entry_state.h  error_keys.h                        # changed (schema 5; remove_pair_copy, create_picker_folder, resolve_local_file(root); synced_copy; sync_pairs_mark_account; new keys)
│   └── (all 001 and 002 headers kept)
└── src/                                                                            # one .cpp per header above; store_migrations.cpp (4 -> 5); log_codes.h (new codes)
bridge/
├── include/  sync_bridge.h                                                         # new; queue_bridge.h (noteAction), picker_bridge.h (folderMode), browse_bridge.h (syncedBy)
└── src/      the matching .cpp files
app/
├── main.cpp                     # wires SyncBridge, calls startupCompare() after the account check when Wi-Fi is up
└── ui_script.{h,cpp}            # new verbs for the walk (add-pair, compare, approve, choose); host build only
qml/
├── pages/  SyncPage.qml  PairPage.qml  PlanPage.qml  ResultPage.qml  (new)
│           HomePage.qml  SettingsPage.qml  DetailsPage.qml  FilePickerPage.qml  (changed)
├── (existing components)  strings_de.tsv (German texts for every new string)  icons/ (existing set; arrows/warning added in style if missing)
tests/
├── unit/         test_sync_rules  test_sync_planner  test_sync_planner_limits  test_sync_scan_remote  test_sync_scan_local  test_sync_records
│                 test_sync_conflict_name  test_sync_scheduler  test_store_schema5  test_store_sync  test_downloader_target_root …
├── integration/  test_sync_download  test_sync_upload  test_sync_both  test_sync_conflicts  test_sync_deletions  test_sync_incomplete
│                 test_sync_limits  test_sync_kill  test_sync_idempotent  test_sync_concurrent  test_sync_hostile_names  test_sync_curl
│                 test_budget_sync
├── bridge/       test_sync_bridge  test_picker_bridge (folder mode)  test_browse_bridge (syncedBy)  test_queue_bridge (noteAction)
│                 test_no_background_network (extended)
├── qml/          sync pages (stubs extended)
├── support/      fake_nextcloud_server.h (extended: checksums in listings, partial listings, removals, unavailable entries, hostile names, 5,000-entry trees)
├── tools/        pbc_fake_nextcloud.cpp (console commands: remove, partial, ls-etag)
├── walks/        11-folder-sync.walk  (CTest ui_walk)
└── fixtures/nextcloud/   anonymized listings with checksums and partial answers, with the .unverified suffix
scripts/          check-constitution.sh (N3/N5 allow-lists, new N7, self-tests), run-host-demo.sh (Books tree, console commands), probe-device.sh (mtime probe)
docs/             live-server-checklist.md (LV-S1..S8), device-test-checklist.md (DT-S1..S8), device-findings.md
README.md  INSTALL.md  CHANGELOG.md  NOTICE  licenses/manifest.txt  VERSION (0.3.0)
```

**Structure Decision**: keep the four-layer layout unchanged (Principle II). The sync lives in new core modules beside the read and
write sides and **names no write verb**: everything that changes the server goes through `queue_actions`/`queue_sender`, and
everything that changes the reader through the Downloader and the one audited `remove_pair_copy`. This keeps rules N2 and N5
narrow and makes the sync's safety the sum of guarantees already proven in 001 and 002 plus the planner's table. Platform behavior
(Wi-Fi, storage root, free space) enters the core only through the existing small interfaces and `SourceRules`.

### Suggested delivery order (input for `/speckit-tasks`)

1. **Foundations**: schema 5 and its migration test; `sync_types`, `sync_rules`, `store_sync` (pairs, records, plans, bounds,
   recovery); `conflict_name`; the `queue_items` columns; N3/N5/N7 allow-lists with self-tests; the fake server's additions and
   fixtures; new error keys and log codes.
2. **US1 Pairs**: `SyncBridge` pair CRUD with the confirmation for sending directions, `SyncPage`, `PairPage`, the folder mode
   of `FilePickerPage`, the Home tool, `describeDirection`. Nothing is compared or transferred.
3. **US2 Review + US3 Download** (the P1 minimum together with 1, 5 and 8): `sync_scan` (both sides), the planner for the
   download direction and the skip reasons, the stored plan and its paging, `PlanPage`/`ResultPage`, the Downloader changes,
   `SyncRunner::compare`/`run` for downloads, the `synced_copy` view and the 001 screens, the pair lock and `sync_recover`.
4. **US5 Triggers**: `SyncScheduler`, `startupCompare`, the Auto Sync timer, `noteAction` hooks, the global switch, "plan ready"
   marks; the extended `test_no_background_network`.
5. **US8 Resumable and idempotent**: kill tests at every hook, idempotence and concurrency tests (most of the mechanism is built
   in steps 1-3; this step proves it and closes gaps).
6. **US4 Upload** (Outbox): `add_sync_uploads`, `QueueSender` group send, record advance in `queue_confirm`, the upload direction in the
   planner, unknown-outcome behavior.
7. **US6 Conflicts** and **US7 Deletions**: `keep_both` execution and the one-sided records; proposals, choices, **Apply choices**,
   `remove_pair_copy`, `add_sync_delete`; two-way direction.
8. **US9 Limits**: limit and reserve checks in the planner, over-limit handling, the usage line, limit editing.
9. **Polish**: budget tests, LV/DT checklist entries, the offline preview, German texts completed, walk 11, README/INSTALL/
   CHANGELOG, version 0.3.0, `package-device.sh` run.

US1, US2, US3, US5 and US8 together are the P1 minimum (pair, reviewed download sync, triggers, safe repetition); each later story
can be stopped after its own tests pass. The two-way direction is only offered in the UI once US6 and US7 are done (the
direction selector shows Download only and Upload only until then).

## Complexity Tracking

> Filled because several additions go beyond the smallest possible change; none violates the constitution.

| Addition | Why Needed | Simpler Alternative Rejected Because |
|----------|------------|-------------------------------------|
| `sync_files` records (a per-file agreement state) | Without them a sync cannot tell "new", "changed", "deleted" and "conflict" apart without trusting clocks (FR-023); they are also what makes repetition idempotent (FR-037/038) | Comparing two trees by name and size and deleting or overwriting on a difference is last-writer-wins and breaks Principle III |
| Persisted plans (`sync_plans`, `sync_plan_entries`) | An unattended comparison must wait for approval across restarts, and a plan of thousands of entries must be paged from SQL (Story 2, FR-033) | Recomputing the plan at every view costs a full network walk per page turn and cannot keep the user's removals or choices |
| One-sided conflict copy records and the `diverged` flag | A keep-both result that the next sync does not bounce between the sides, with a stable result (FR-022, SC-005) | Without them the "server copy" would be uploaded and the "reader copy" downloaded on the next sync, multiplying files without end |
| Uploads via the 002 queue (`pair_id`, `sync_rel`, `sync_role`, `only_group` send) | Reuse of every upload guarantee instead of a second uploader; confirmation advances the record in one transaction | A second upload path doubles the review surface of the most security-sensitive code and cannot match the unknown-outcome handling |
| Downloader `target_root`, `strip_prefix`, `strict_version` | A pair downloads into a folder the user chose, mirrors only the part below the pair's folder, and never lands a version other than the planned one | A second download engine would duplicate temporary files, resume and the free-space checks; changing 001's mirror tree to the pair folders would break 001 |
| `synced_copy` view and `resolve_local_file(root, ...)` | Story 3 scenario 8: the 001 screens show synced files as downloaded and can open them | Putting pair files into `local_files` collides with the primary key (one remote path, one copy) and would change 001's removal rules |
| Plan age limit and the offline preview | A plan prepared at start-up may be days old at approval, and proposals must not be acted on from stale data; offline preview satisfies Story 2.8 honestly | Approving any age lets the user act on stale deletions; no preview leaves the offline user with nothing |
| A second `QTimer` (single shot) in `sync_bridge.cpp` | Auto Sync's quiet period for pairs (constitution VI allows exactly this trigger) | A timer in QML or a sleeping thread is untestable on the host and breaks rule N3; the scheduler class keeps the logic Qt-free |
| Rule N7 (paths in the sync modules only through audited constructors) | Principle IV: a hostile server name must not become a reader path by accident | Review alone misses a stray `root + "/" + name` in a new module |
| A fake Nextcloud with checksums, partial listings, removals, unavailable entries and 5,000-entry trees | Principle I: boundary behavior of the new scans is proven against a repeatable in-process fake | Hand-built answers per test duplicate server logic and cannot model removal and change between compare and run consistently |
