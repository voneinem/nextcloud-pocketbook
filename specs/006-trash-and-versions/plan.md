# Implementation Plan: Trash and File Versions

**Branch**: `feature/006-trash-and-versions` | **Date**: 2026-10-24 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/006-trash-and-versions/spec.md`

## Summary

The user opens the server's **trash** (from the Home page and from the folder page), sees every deleted file and folder with its
original location, deletion date and size, and **restores** one item at a time: to its original place when that is free, otherwise (or
by choice) to a folder picked with the existing folder chooser, never overwriting or renaming anything that exists. For a file the user
opens **Versions** in its details, sees the older versions (date, size) and restores one; the server keeps the current content as one more
version. **Delete for good** (one item) and **Empty trash** are available only on the trash screen, after a confirmation that names what
is lost (one dialog for a file, two successive dialogs for a folder and for Empty trash, Cancel is the default). When the server's trash
or versions app is off, the app says so in plain words and offers no action that cannot work. Every action is one explicit request on a
live connection: nothing is queued, polled, retried or cached beyond the last trash list.

Technical approach (details in [research.md](research.md)): five small Qt-free core modules (`trash_types`, `trash_parse`, `trash_rules`,
`trash_api`, plus `store_trash` in the store) on top of the existing `DavSession`, `xml_min`, `json_min`, `remote_path`, `utf8`,
`new_name`; two bridges (`TrashBridge`, `VersionsBridge`); two pages (`TrashPage`, `VersionsPage`) plus a Trash entry on `HomePage` and
`FolderPage`, a Versions tool on `DetailsPage`, and one additive option on `FolderChooserPage` (server folders only). **One schema change
(5 -> 6: the last trash list), no new dependency, no new timer.** Two new mechanical rules (N11 the trash and versions endpoints are
named only in `trash_parse.cpp`/`trash_api.cpp`; N12 the delete-for-good call exists only in `TrashBridge` and never in the queue, sync,
update or Auto Sync code) and an N2 allow-list entry for `trash_api.cpp` (`MOVE` and `DELETE`). The test base is the in-process fake
Nextcloud extended with a trash side, a versions side, capability switches and failure injection.

**Constitution:** Principle III said a server deletion "goes to the server trash"; Delete for good removes from the trash itself. The
owner approved the amendment and it is in force as **constitution 1.3.0 (2026-10-24)**: the trash screen is the one exception, with the
confirmation chain this plan implements, and writes without an ETag are bound to what the user saw. Stories 1 to 3 and 5 do not depend on
it; Story 4 depends on it and nothing else (owner point 1, Complexity Tracking).

## Technical Context

**Language/Version**: C++17 (core, bridge); Qt 6.8 Quick/QML for the UI (device runs Qt 6.10.3, SDK sysroot 6.8.2); CMake

**Primary Dependencies**: libcurl 8.18.0 (static on the reader, system on the host), OpenSSL, SQLite. **No new third-party dependency.**
New code reuses `DavSession`, `xml_min`, `json_min`, `fetch_entry`, `remote_path`, `utf8`, `new_name`, `log` (no secret
is involved, so no `SecretText`).

**Storage**: `pbcloud.db` goes from **schema 5 to schema 6**: one table `trash_items` (the last fetched trash list, at most 1,000 rows)
and the settings `trash_fetched_at` and `trash_truncated`. The migration is one-way and tested (constitution, Development Workflow); an
older build refuses the newer database. No file is written; no version list is stored (versions are shown only while online).

**Testing**: the project's own framework (`tests/support/test_support.h`) under CTest: unit tests (parsers, rules, restore planning,
confirmation content), integration against the fake Nextcloud (every request of [contracts/trash-requests.md](contracts/trash-requests.md),
refusals, unknown outcome, stale state, the migration), a real-libcurl test against the local TLS server for the new verbs, bridge tests
(Qt Test, offscreen), `app --self-test` for the new pages, the host UI walks 21 and 22, script checks with self-tests, hostile-input
fixtures (`*.unverified`), a budget test (unverified until measured on the device).

**Target Platform**: PocketBook InkPad One (RK3566, armhf); host Linux x86-64 for the complete core test suite (constitution II)

**Project Type**: desktop-style Qt Quick application with a Qt-free core library (device app plus host tests)

**Performance Goals** (budgets, **unverified until measured on the device**; they restate spec SC-001, SC-007):

| Quantity | Budget |
|----------|--------|
| Find and restore a deleted file to its free original place | <= 4 taps from the folder listing (Trash, the item, Restore, confirm), < 30 s of the user's own time (SC-001) |
| Trash tap -> first page of a 1,000-item list | <= 6 s on the device's Wi-Fi; parse + store + model build <= 150 ms host, <= 700 ms device |
| Version list of 100 entries | <= 4 s; parse + model build <= 50 ms host, <= 300 ms device |
| Empty trash of 1,000 items | one request at a time, coarse progress (a screen update every 10% or 25 items at most), cancellable between requests |
| Cold start | unchanged <= 5 s: no trash or versions code runs at start |

**Constraints**:

| Quantity | Budget |
|----------|--------|
| RAM | peak <= 150 MB (`VmHWM`) as before; trash and versions state <= 1.5 MB (1,000 trash rows, 200 version rows, one dialog) |
| Storage | the stored trash list <= 1 MB (1,000 rows, names <= 255 bytes, locations <= 1,024 bytes); package <= 25 MB (gate), expected growth < 120 KB |
| Response size | trash list <= 4 MiB; version list <= 1 MiB; other answers <= 64 KiB; 1,000 trash rows and 200 version rows kept; more are cut and the screen says so; every name/location bounded and sanitized |
| Network | none in the background and none at start; every request follows a tap on a trash or versions screen; one request at a time; connect 10 s, total 30 s; no retry of a write; no queue |
| Server contract | Nextcloud 30 or newer assumed (unverified); trashbin and versions DAV endpoints; write verbs `MOVE` and `DELETE` on those endpoints only, in `trash_api.cpp` |
| Largest supported file | not applicable (no file data moves; the server restores); paths <= 4,096 bytes |
| Auto Sync | not involved: no trash or versions action is ever triggered by Auto Sync, folder sync, update or start (stated because constitution VI requires every plan to say so) |

**Scale/Scope**: one account; a personal library; a trash of tens to a few hundred items (1,000 supported); files with a handful to
~100 versions; 5 user stories, 36 functional requirements; 5 new core modules plus store additions, 2 new bridges, 2 new pages, changes
to 5 files (`HomePage`, `FolderPage`, `DetailsPage`, `FolderChooserPage`, `Main.qml`)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design (verdict column).*

Constitution version 1.3.0 (amended 2026-10-24 for this feature). Result: **pass; the three deviations from "conditional writes" and the
trash-screen exception are the ones the amendment of Principle III now provides for**. Principles III, IV and VI are where this feature touches the most specific text.

| Principle | Verdict and where it is satisfied |
|-----------|-----------------------------------|
| I Test-First | **Pass.** Failing-first tests per module (research R-18): unit tests for both parsers (malformed, truncated, oversized, hostile names and locations, duplicates, a multistatus in which some members failed), the restore planner, the confirmation content and the capability states; fake-server contract tests of every request incl. the lost-answer and refusal cases; a real-libcurl test; the migration test 5 -> 6 with the older-build refusal; `test_trash_privacy`. The trashbin and versions endpoints are boundary behavior tested against the fake, never a real server. |
| II Host-testable core | **Pass.** All logic is in `core/` (Qt-free); `bridge/` holds `TrashBridge` and `VersionsBridge` only; platform facts (Wi-Fi, clock) enter through `Config` structs as in the other bridges. Dates take "now" as a parameter. |
| III No silent data loss | **Pass, under the clause of 1.3.0 for writes without an ETag and its trash-screen exception.** Nothing is overwritten: restore never replaces, merges or renames an existing entry (pre-check of the target plus `Overwrite: F` where the destination is a files path, R-5, R-6); a version restore keeps the current content as a version (server rule, verified by listing again, R-9); nothing is queued; an unknown outcome is kept as unknown and never repeated (R-10). **Recorded as 1.3.0 requires (writes without an ETag):** (a) **T4, restore to the original place**: the request names no target path, so no `If-None-Match: *` is possible; bound to what the user saw by a probe of the original place before the request and again after it (R-5), and it can never replace anything (the server restore does not replace; a collision that appears late is reported, nothing is renamed by the app); (b) **T5, restore to a chosen place**: probe before and after plus `Overwrite: F` on a files destination (R-6); (c) **T7, version restore**: no `If-Match` on the file; a re-read of the file's current ETag immediately before (R-9), a residual race seconds wide that costs at most a concurrent edit that stays available as a version; (d) **T8, delete for good**: the item's unique trash name as identity, so a request can only ever hit the item the user saw (R-7). **Trash-screen exception:** Delete for good and Empty trash are allowed on the trash screen only, with the confirmation chain of R-8/R-15, only the named items, only on a connected device, never queued or automatic; rules N11 and N12 enforce the confinement (spec FR-021..FR-026, FR-036). |
| IV Security & Privacy | **Pass.** TLS, https-only and the same-host rule come with `DavSession`; a trash address in an answer on another host is never followed (`url_for`). Server-supplied names and locations are untrusted: sanitized, bounded, never a local path (R-3, spec FR-004). Logs carry codes, status and counts only: **N8 is extended in spirit by a new rule N11** (the endpoints) and the trash modules use the existing field-name rules R6/R8/N4; no name, location or path is logged (spec FR-032). No secret is involved. The stored trash list holds names the user already sees in listings; it is removed with the account data (R-4). |
| V Simplicity & bounded footprint | **Pass.** No dependency; one small table; one request at a time; every answer bounded (4 MiB / 1 MiB, 1,000 / 200 rows); no previews, no download of a version, no history, no retry machinery, no queue (YAGNI). Largest file size: not applicable (stated). |
| VI E-ink UX & battery | **Pass.** Page buttons, no animation, no polling, no timers (rule N3 unchanged). Network happens only after a tap on a trash or versions screen (Trash, Refresh, Restore, Delete for good, Empty trash, Versions, a version's Restore). While offline the stored list is shown with its age and the actions say a connection is needed (spec FR-035). Empty trash shows coarse progress. |
| VII Licensing | **Pass.** The trashbin and versions DAV endpoints are reimplemented from public documentation and the observable behavior; no Nextcloud code, strings or documentation text copied; no new component, so `NOTICE`, `licenses/manifest.txt` and the About box component list are unchanged; new icons (trash, versions, restore) are drawn in the project's icon style. The app stays unofficial. |
| VIII Quality gates | **Pass.** `pre-merge.sh` unchanged in shape; N2 allow-list gains `trash_api.cpp` (`MOVE`, `DELETE`; `PUT`, `MKCOL`, `COPY`, `PROPPATCH`, `LOCK`, `UNLOCK` stay forbidden there); new N11 and N12 with self-tests; `test_no_background_network` is extended. Version bump 0.5.0 -> 0.6.0 and `CHANGELOG.md` are the last phase of tasks.md; the ARM build takes the SDK only from `POCKETBOOK_SDK` / `-DSDK_PATH`. |
| IX Specs-driven & honest verification | **Pass.** Every server and device behavior (the trashbin property names, original-location shape, restore to a chosen place, the server keeping the current content as a version, capability keys, behavior with the apps off, very large trash) is labelled unverified; fixtures carry `.unverified`; LV-T1..LV-T10 and DT-T1..DT-T4 are added to the checklists by the task list and results are recorded when obtained. |
| X English documentation | **Pass.** All artifacts, comments, identifiers, log keys, test names and script output in English; German only in `qml/strings_de.tsv` and non-English test data. |
| XI Consistent look and feel | **Pass with a note.** Existing components only (TopBar, BottomPanel, IconButton/ToolButton, InkRow, ModelPager/PageButtons, ChoiceDialog, ConfirmDialog, TextEntryDialog, FolderChooserPage, Style); no bordered text button, no animation. **Notes:** (1) the `FolderPage` normal-mode panel gains a Trash tool and `DetailsPage` a Versions tool - DT-T1 checks that both panels still fit at the usual touch size (owner point 3); (2) `FolderChooserPage` gets one additive option (server folders only, no queued ones). |

Mechanical constraints (Security & Data Handling): the data folder, database and credentials file keep their permissions; the new table
lives in the existing database. **Technology constraints check**: C++17, Qt 6.8 Quick/QML, CMake, libcurl, OpenSSL, SQLite; HTTP through
the existing transport; XML through `xml_min`; JSON through `json_min`; no new XML or WebDAV library. App name `pbcloud` unchanged; no
collision with PB Notes or PB Office; the one permitted contact with PB Office (feature 005) is untouched. Passes.

**Post-design re-check (after Phase 1):** the design artifacts introduce no violation beyond those above. The points that came closest are
decided and contained: **restore to a chosen place** may not be offered by every server, so the app degrades to an honest message instead
of an invented workaround (R-6); **Empty trash** is a series of single deletes of exactly the named items, never the "empty the
collection" request, because FR-023 forbids removing items that arrived later (R-8); **messages** are derived from rules and status, not
from server text (R-11), so spec FR-032 cannot be broken by a server's wording.

### Open points for the owner (not violations)

1. **Constitution amendment: done.** Principle III was amended to 1.3.0 on 2026-10-24 (the trash-screen exception and the clause for
   writes without an ETag), approved by the owner for this feature. Story 4 may be implemented; should the amendment ever be reverted, the
   build keeps `delete_enabled = false` and the plan ships Stories 1, 2, 3 and 5.
2. **Restore to a chosen place.** The trash endpoint documents a restore to the original place only. The plan tries a move of the trash
   item straight to a chosen files path (`Overwrite: F`); a server that refuses it gets the message "This server can restore only to the
   original place. Rename or move the file that is there, then restore again." LV-T5 decides; if it fails on both targets the spec's
   "chosen place" is delivered as that message (analysis may flag the spec wording).
3. **Trash tool placement.** The Trash entry is on the Home page and a tool in the normal-mode panel of the folder page (so SC-001's four
   taps hold); `DetailsPage` gets Versions as its fifth or sixth tool. DT-T1 checks both panels; if too tight, Trash moves to the top bar.
4. **Size beyond the bound.** The trash endpoint has no paging; one bounded answer is read (4 MiB) and 1,000 rows are kept (newest
   deletions first). Beyond that the screen says so, and Empty trash covers only the rows shown and says so (R-8).

## Project Structure

### Documentation (this feature)

```text
specs/006-trash-and-versions/
├── plan.md              # This file (/speckit-plan command output)
├── research.md          # Phase 0 output: decisions R-1 .. R-19
├── data-model.md        # Phase 1 output: entities, validation, state machines, schema 6
├── quickstart.md        # Phase 1 output: how to validate on host, fake server, real server, device
├── contracts/
│   ├── trash-requests.md    # every request to the server, mapping, bounds (unverified)
│   ├── core-interfaces.md   # Qt-free C++ interfaces of the new core modules, new error keys
│   └── bridge-qml.md        # TrashBridge, VersionsBridge, pages, dialogs, messages
├── checklists/
│   └── requirements.md  # specification quality checklist
└── tasks.md             # Phase 2 output (/speckit-tasks command - NOT created by /speckit-plan)
```

### Source Code (repository root)

```text
core/
├── include/pbc/
│   ├── trash_types.h  trash_parse.h  trash_rules.h  trash_api.h      # new: entities and limits, tolerant parsers, pure rules, the requests
│   ├── store.h  store_files.h                                        # changed (additive: schema 6, trash_items methods)
│   ├── error_keys.h                                                  # changed (additive: trash_* and version_* keys)
│   └── (all 001-005 headers kept)
└── src/                                                              # one .cpp per header above; store_trash.cpp, store_migrations.cpp (5 -> 6),
                                                                      # store_schema.h, error_keys.cpp, log_codes.h (trash.* codes)
bridge/
├── include/  trash_bridge.h  versions_bridge.h                       # new
└── src/      trash_bridge.cpp  versions_bridge.cpp                   # new; worker.cpp reused; folder_chooser_bridge (additive option: serverOnly)
app/
├── main.cpp                     # wires TrashBridge and VersionsBridge (store, worker, transport, clock, wifi)
├── ui_script.{h,cpp}            # new verbs for the walks (trash, restore, delete-for-good, empty-trash, versions, restore-version); host build only
qml/
├── pages/  TrashPage.qml  VersionsPage.qml  (new)
│           HomePage.qml  FolderPage.qml  DetailsPage.qml  FolderChooserPage.qml  (changed)
├── Main.qml (changed: context properties)  Problems.qml (changed: titles/actions)  strings_de.tsv (German texts for every new string)
├── icons/  trash.svg  restore.svg  versions.svg (new, in the project's icon style, where the set has none)
tests/
├── unit/         test_trash_parse  test_trash_rules (planner, targets, confirmation content)  test_version_parse  test_trash_availability
│                 test_store_trash  test_store_migration_6  test_error_keys (extended)
├── integration/  test_trash_requests (the recorder of every flow)  test_trash_list  test_trash_restore  test_trash_restore_chosen  test_trash_delete  test_trash_empty
│                 test_trash_refusals  test_trash_unknown_outcome  test_trash_privacy  test_trash_curl (real libcurl, TLS server)
│                 test_versions_list  test_versions_restore  test_versions_stale  test_trash_disabled
│                 test_no_background_network (extended)  test_budget_trash
├── bridge/       test_trash_bridge  test_versions_bridge
├── support/      fake_nextcloud_server.h (extended: trash side, versions side, capability switches, failure injection)
├── tools/        pbc_fake_nextcloud.cpp (console: trash-add, trash-list, trash-disable, versions-disable, lose-next-answer)
├── walks/        21-trash-restore.walk  22-versions-and-delete-for-good.walk  (CTest ui_walk)
└── fixtures/nextcloud/trash/   anonymized trash and versions answers with the .unverified suffix
scripts/          check-constitution.sh (N2 entry, N11, N12, self-tests), check-ui-style.sh (irreversible-dialog rule), run-host-demo.sh (console)
docs/             live-server-checklist.md (LV-T1..LV-T10), device-test-checklist.md (DT-T1..DT-T4), device-findings.md
README.md  CHANGELOG.md  VERSION (0.6.0)
```

**Structure Decision**: keep the four-layer layout unchanged (Principle II). The trash and versions code is a block of new Qt-free
modules next to the existing ones, with `trash_api.cpp` as the only file that names `MOVE` or `DELETE` for the trashbin and versions
endpoints, so rule N2 stays an allow-list. The irreversible call is confined by name and by rule (N11, N12), not by convention.
Platform behavior (Wi-Fi, clock) enters only through the bridges' `Config`. The fake Nextcloud grows a trash side and a versions side so
the demo server and the tests exercise the same contract.

### Suggested delivery order (input for `/speckit-tasks`)

1. **Foundations**: `trash_types`; `trash_parse` (trash and versions answers, hostile fixtures); `trash_rules` (availability states,
   restore planner, confirmation content); new error keys with German texts; schema 6 and `store_trash`; the fake server's trash and
   versions sides and capability switches; rule N2 entry and N11 with self-tests; log codes.
2. **US5 Explain disabled apps** (P2 but cheap and it guards every screen): `fetch_availability`, the two plain messages, the generic
   message with a code, `test_trash_disabled`.
3. **US1 See what was deleted** (P1): `list_trash`, stored list, `TrashBridge.open/refresh/rows`, `TrashPage`, the Trash entries,
   offline behavior, truncation notice.
4. **US2 Restore** (P1): target probe, `restore_item` to the original place, the chosen-place path with `FolderChooserPage(serverOnly)` and the
   new-name entry, refusal and unknown-outcome handling, local-copy state following the listing.
5. **US3 Versions** (P2): `list_versions`, `VersionsBridge`, `VersionsPage`, the Versions tool, `restore_version` with the stale check.
6. **US4 Delete for good** (P2, under the trash-screen exception of constitution 1.3.0): `delete_item`, the one and two-step confirmations, `empty_trash`
   as single deletes of the named set with progress and cancel, rule N12 with its self-test.
7. **Polish**: budget test, LV/DT checklist entries, README/About text, walks 21 and 22, German texts completed, version 0.6.0,
   `package-device.sh` run.

US1, US2 and US5 are the P1 minimum (see, restore, understand); US3 and US4 can each be stopped after their own tests pass.

## Complexity Tracking

> Filled because several additions go beyond the smallest possible change; the points that touch the constitution are marked.

| Addition | Why Needed | Simpler Alternative Rejected Because |
|----------|------------|-------------------------------------|
| Delete for good under the **trash-screen exception of constitution 1.3.0** (done) | Spec FR-021 to FR-026 and FR-036 lift feature 002's "never delete permanently" for the trash screen only; the exception is now written in Principle III | Without it delete for good would be an "undocumented exception in a plan", which Governance forbids; dropping Story 4 removes a requirement the owner asked for |
| Restore to the original place (T4) without a condition header | The request names no target path, so `If-None-Match: *` cannot be expressed; 1.3.0 asks the plan to record the check used instead | A blind restore could land beside a name created in the meantime without the user knowing; the probe before and after, and the plain report, make it visible (R-5) |
| Item identity by unique trash name instead of `If-Match` on trash deletes | Principle III asks for conditional deletes; trash items have no usable ETag | A blind delete by position or by original name could hit a different item that reached the trash with the same name; the unique name cannot (R-7) |
| Re-read of the file's ETag before a version restore | The restore request has no `If-Match` on the file; FR-018 requires a refusal when the file changed | Skipping it silently restores over newer content; the residual race is seconds wide and the newer content stays as a version (R-9) |
| Schema 6 with one table for the last trash list | Spec FR-003 shows the stored list offline, with its age | An in-memory list is lost on exit and would make the offline case untestable; a file would add a second storage format |
| `Empty trash` as one `DELETE` per named item | Spec FR-023: items that reached the trash after the list was read must stay | The single "empty the collection" request removes unseen items too |
| Rules N11 and N12 in `scripts/check-constitution.sh` | Principle VIII: every scriptable rule must be scripted; "delete for good only from the trash screen, never from sync/update/queue" is the feature's main promise | Review alone has missed such call sites; the script and its self-test make it mechanical |
| Extending the fake Nextcloud with trash and versions sides | Principle I: boundary behavior is proven against a repeatable in-process fake | Hand-scripted answers per test duplicate server logic and cannot model the unique-name rule, lost answers and disabled apps consistently |
