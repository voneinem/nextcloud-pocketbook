# Quickstart: validating folder synchronization

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **002**: [quickstart](../002-manage-server-files/quickstart.md) | **001**: [quickstart](../001-nextcloud-files-client/quickstart.md)

How to prove that the feature works, from the cheapest check to the real device. Every device and real-server result is
**unverified** until written into `docs/device-test-checklist.md` (DT-S*) or `docs/live-server-checklist.md` (LV-S*) with a date.
Contracts: [sync rules](contracts/sync-rules.md), [core](contracts/core-interfaces.md), [bridge and QML](contracts/bridge-qml.md);
data: [data-model.md](data-model.md); decisions: [research.md](research.md).

## 1. Host build and the automatic gate

Same prerequisites as 001 and 002 (C++17, CMake, Ninja, libcurl 8.18.0 or newer, OpenSSL and the `openssl` CLI; Qt 6 optional).

```bash
cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
ctest --test-dir build-host --output-on-failure
scripts/pre-merge.sh            # version bump (0.3.0), build, all tests, secret scan, constitution/fixture/UI-style checks
```

Expected: every test passes, including `check_constitution` with the changed rules N3 and N5, the new N7 and their self-tests,
`check_fixtures`, `check_fake_markers`, `check_ui_style`, `check_translations`. A second build directory with
`-DPBC_SANITIZE=ON`: the hostile-input, kill and sync tests must pass under it.

## 2. Contract and interruption tests (no server, no device)

```bash
ctest --test-dir build-host -L unit          # rules, planner decision table, scans, records, conflict names, scheduler, schema 5
ctest --test-dir build-host -L integration   # fake Nextcloud: download/upload/two-way syncs, conflicts, deletions, kills
ctest -R 'sync_(kill|idempotent|concurrent)' --test-dir build-host --output-on-failure
```

| Check | Expected |
|-------|----------|
| `test_sync_planner` (every row of [sync-rules.md](contracts/sync-rules.md), all three directions) | the table is implemented as written; direction is a hard wall; deterministic (SC-002, SC-005, SC-006) |
| `test_sync_download` (100 files, 5 levels; then one changed, one added, one local-only file) | identical tree on the reader; second sync makes 0 transfers; local-only file untouched and never sent (SC-003) |
| `test_sync_upload` (Outbox with files and a subfolder; edit one afterwards) | files and folders on the fake server; the Outbox is unchanged; re-sync sends nothing; the edited file is planned as a changed upload; an unknown outcome is not repeated |
| `test_sync_both` / `test_sync_conflicts` | both directions transferred; a file changed on both sides, a first-sync name clash and delete-versus-change each keep both versions; repeating creates no further copies (SC-005) |
| `test_sync_deletions` | a missing file is only ever a proposal; approving a plan deletes nothing; each confirmed choice affects exactly its entry; server deletion goes to the trash; a changed reader file is never offered for deletion (SC-006) |
| `test_sync_incomplete` (partial multistatus, failed folder, server folder renamed, reader folder absent) | no proposal or record drop from an incomplete or unavailable side; the pair shows the reason |
| `test_sync_limits` | a plan over a limit is not approvable and transfers nothing; removing entries makes it fit; the app never selects entries; the reserve is kept; lowering a limit deletes nothing (SC-009, SC-013) |
| `test_sync_kill` (kill at every hook: `plan_written`, `approved`, `before_download`, `downloaded`, `record_committed`, `group_created`, `item_confirmed`, `proposals_applied`, `result_written`; 20 downloads + 5 uploads) | after restart and a repeated sync the final state of both sides and of the records equals the uninterrupted run; no file lost, duplicated or partial (SC-004) |
| `test_sync_hostile_names` | separators, `..`, control characters, overlong, case-only, normalization variants never reach a path outside the pair's reader folder or replace another entry (SC-012) |
| `test_sync_curl` | one download and one upload through the real libcurl transport and the local TLS server |
| `test_no_background_network` (extended) | start-up compare happens once and only with Wi-Fi on; idle hour, Wi-Fi on/off, finished or failed runs start nothing; Auto Sync compares only the affected pair after the quiet period; **no trigger approves or transfers** (SC-002, SC-007, SC-008) |
| `test_sync_bridge`, QML stubs | pair CRUD with the confirmation for sending directions, plan paging, approve/cancel/remove-entry, proposals, texts present in English and German (SC-014) |
| `test_budget_sync` | prints the compare time for 1,000 files (<= 10 s, page turn <= 1 s), peak RSS during the compare and during a 500 MB transfer (<= 150 MB), database growth at the bounds (<= 10 MB). Marked **unverified until measured on the device** |

## 3. Try the app on the host with the fake server

```bash
scripts/run-host-demo.sh            # fake server (HTTPS on localhost, own CA) and the app, with a picker root that has sample folders
```

The demo script prepares a throw-away data folder, download folder and picker root, and the server holds a `Books` tree (about 40
files, two levels, one hostile name, one file without permission). The fake server's console has the commands of 002 plus `touch
<path>` (change a file), `remove <path>`, `partial <folder>` (listing fails halfway), `ls-etag`. Walk through:

1. **US1** Home -> **Sync** -> **Add pair**: server folder `Books` (chooser), reader folder `Books` (created with New folder), direction
   left at *Download only* with its summary; limits shown; Save. The pair says "Never synced"; the console shows no request.
   Add a second pair `Outbox` -> `Inbox` as *Upload only*: the confirmation names both folders. An overlapping or forbidden
   reader folder is refused with its reason.
2. **US2** Tap the pair -> **Sync**: the review shows downloads with counts, sizes, free space; nothing has changed on disk yet.
   Remove one entry, **Approve**: the result shows the counts; the removed entry appears again at the next **Sync**. Cancel a
   second review: nothing changes.
3. **US3** `touch` a server file and add one; **Sync**: exactly those two are planned. Put an extra file in the reader folder:
   never sent, never offered for deletion. The hostile name appears with a safe local name; the no-permission file is *skipped*.
4. **US4** Put files in `Outbox`, **Sync**: uploads in the plan; approve; the queue shows the group; the files stay in `Outbox`;
   a second sync plans nothing; edit a file: planned as a changed upload.
5. **US5** Switch *Sync at start* on, restart the app with Wi-Fi on: one comparison, the pair shows "Plan ready", **nothing was
   transferred**. With *Auto Sync* on, queue an upload into `Books/` from the folder page: after 10 s that pair (only) shows
   "Plan ready". Turn Wi-Fi off and on: no request.
6. **US6** Direction *Two-way*; change the same file on both sides (`touch` and edit); **Sync**: *keep both* in the plan; approve;
   both versions are on both sides under distinct names; sync again: nothing new.
7. **US7** Delete a file on the server (`remove`) and one on the reader: both are *Proposals*; **Approve** deletes nothing; choose
   *Delete on the reader* for one and *Copy it back* for the other, **Apply choices**: each confirmation names the file and side.
8. **US8** Start a large sync and kill the app (or `drop-after`); restart: the pair says "Interrupted"; **Sync** again gives the
   same final state as an uninterrupted run, no partial file in the library.
9. **US9** Set the limit to 10 files: the review says "Over the file limit by N" and **Approve** is disabled; raise the limit or
   remove entries; the usage line on the Sync screen shows held against the limits.

Pass criterion: every step behaves as in the matching acceptance scenario of the spec. The UI walks
`tests/walks/11-folder-sync.walk` (steps 1, 2), `12-sync-both-ways.walk` (6), `13-sync-proposals.walk` (7) and `14-sync-upload-only.walk` (4) (CTest `ui_walk`) script them with pictures; `15-sync-limits.walk` (9: the over-limit plan and the limit confirmation), `16-sync-stopped.walk` with `17-sync-interrupted.walk` (8: the run that is stopped, the pair's "Interrupted: n of m" line, the next sync; and the start-up half of 5) add to them. The rest of step 5 is covered by `test_no_background_network` and `test_sync_bridge`, step 8 by `test_sync_kill`, step 9 by `test_sync_limits`.

## 4. Real server (unverified until done)

Use a throw-away test account on a Nextcloud 30 or newer server with an **app password** only; never commit addresses or
passwords. Work through `docs/live-server-checklist.md` items LV-S1 to LV-S8: checksums in listings, ETag stability across a
rename and a desktop-client sync, the `Depth: 1` walk of a 1,000-file tree, a listing that fails halfway, conditional replace of
a synced file, server deletion from a proposal into the trash, unavailable entries inside a paired folder, external changes
between comparing and running. Anonymized exchanges become fixtures; rename `*.unverified` only for exchanges that were seen live.

## 5. Device (InkPad One), unverified until done

```bash
POCKETBOOK_SDK=<sdk path> scripts/package-device.sh      # builds and checks the size gate (25 MB)
scripts/probe-device.sh                                  # now also: mtime resolution and stability of copied files (DT-S2)
```

Install, then work through `docs/device-test-checklist.md` items DT-S1 to DT-S8: start-up compare with Wi-Fi on and off;
modification-time behavior of FAT files copied from a PC; the reader's library finding downloaded files in the pair folder; an
Outbox written by another PocketBook app; the free-space reserve on a nearly full card; peak memory and compare time; a battery
pull during a sync; removable storage absent. Measured values go in the "Value" column with the build version and date.
