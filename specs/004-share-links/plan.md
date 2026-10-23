# Implementation Plan: Share Links

**Branch**: `feature/004-share-links` | **Date**: 2026-10-22 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/004-share-links/spec.md`

## Summary

From the folder listing the user selects a file or folder and opens **Share**, sees what is already shared (public links, and users and groups
read-only), creates a public link (optional password, optional expiry date, read-only or, for folders, read-and-upload), shows the
link in large plain letters so it can be typed or read out elsewhere (with a Copy action only where the reader has a clipboard other
apps can use), changes a link's password, expiry or permission, and removes a link after a confirmation. Every action is one explicit
request on a live connection: nothing is queued, retried, cached or opened. Links and passwords are secrets: shown only on the screen
asked for, never in a list, a log, an error record or a file, never fetched or opened by the app, and a typed password is sent once and
wiped. The server's sharing rules (links off, password or expiry forced, upload limited) are read when the screen opens, shown in the
controls, and explained in plain messages derived from the **rules and the status**, never from the server's text.

Technical approach (details in [research.md](research.md)): seven small Qt-free core modules (`secret_text`, `share_types`,
`share_rules`, `share_date`, `share_parse`, `share_diagnose`, `share_api`) on top of the existing `DavSession` and `json_min`; one
bridge (`ShareBridge`) and three pages (`SharePage`, `ShareLinkPage`, `ShareFormPage`) plus a Share tool in select mode of `FolderPage` (and on `DetailsPage` for a file) and one
additive property on `TextMenu`. **No schema change, no new dependency, no new timer.** Two new mechanical rules (N8 secret-safe
logging in the share modules, N9 nothing opens a link) and an N2 allow-list entry for `share_api.cpp` (the only module naming `PUT` and
`DELETE` for shares). The test base is the in-process fake Nextcloud extended with a shares side, rule switches and failure injection,
plus one real-libcurl test.

## Technical Context

**Language/Version**: C++17 (core, bridge); Qt 6.8 Quick/QML for the UI (device runs Qt 6.10.3, SDK sysroot 6.8.2); CMake

**Primary Dependencies**: libcurl 8.18.0 (static on the reader, system on the host), OpenSSL, SQLite (untouched). **No new third-party
dependency.** New code reuses `json_min`, `DavSession`, `remote_path`, `utf8`, `civil`, `secure_wipe`, `log`.

**Storage**: none added. `pbcloud.db` stays at **schema 5**; no file, setting or table is written by this feature. Links and passwords
are never stored (spec FR-025, FR-027).

**Testing**: the project's own test framework (`tests/support/test_support.h`) under CTest: unit, integration against the fake
Nextcloud, a real-libcurl test against the local TLS server, bridge (Qt Test, offscreen), `app --self-test` for the pages, the host UI
walk 18, script checks with self-tests, hostile-input fixtures (`*.unverified`), a budget test (unverified until measured on the device)

**Target Platform**: PocketBook InkPad One (RK3566, armhf); host Linux x86-64 for the complete core test suite (constitution II)

**Project Type**: desktop-style Qt Quick application with a Qt-free core library (device app plus host tests)

**Performance Goals** (budgets, **unverified until measured on the device**; they restate spec SC-001 to SC-011):

| Quantity | Budget |
|----------|--------|
| Create a read-only link from the listing | <= 8 taps/entries, < 1 min (SC-001) |
| Share tap -> first page of a 200-share list | <= 5 s on the device's Wi-Fi; parse + model build <= 100 ms host, <= 500 ms device (SC-007) |
| Remove a link | <= 4 taps once the item is selected: Share, the link, Remove link, confirm (SC-008) |
| Link display | 120 characters complete without horizontal scrolling (SC-002) |
| Cold start | unchanged <= 5 s: no sharing code runs at start |

**Constraints**:

| Quantity | Budget |
|----------|--------|
| RAM | peak <= 150 MB (`VmHWM`) as before; share state <= 1 MB (500 rows, one detail) |
| Installed size | package <= 25 MB (gate); expected growth < 150 KB |
| Response size | each answer <= 2 MiB; JSON depth 16, 40,000 nodes; 500 rows kept; label/name <= 64 bytes; link <= 512 bytes; password <= 256 bytes |
| Network | none in the background and none at start; every request follows a tap on a sharing screen; one request at a time; connect 10 s, total 30 s; no retry; no queue |
| Server contract | Nextcloud 30 or newer assumed (unverified); OCS Share API; write verbs `POST`, `PUT`, `DELETE` on `.../shares` only, in `share_api.cpp` |
| Largest supported file | not applicable (no file data moves); paths <= 4,096 bytes |
| Auto Sync | not involved (sharing is never triggered by it); stated because constitution VI requires every plan to say so |

**Scale/Scope**: one account; a personal library; items with a handful of shares (hundreds at most); 7 user stories, 33 functional
requirements; 7 new core modules, 1 new bridge, 3 new pages, changes to 5 files (`FolderPage`, `DetailsPage`, `TextMenu`, `InkField`/`TextEntryDialog`, `Main.qml`)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design (verdict column).*

Constitution version 1.0.0 (ratified 2026-10-19). Result: **pass** with two justified points (III conditional writes for shares, VI
sharing as an explicit-request network action) and four owner points below. Principles III, IV and VI are where this feature touches
the constitution's most specific text.

| Principle | Verdict and where it is satisfied |
|-----------|-----------------------------------|
| I Test-First | **Pass.** Every module has failing-first tests (research R-16): unit tests for rules, dates, parsers, diagnosis and `SecretText`; fake-server contract tests of every request including `test_share_requests`; a real-libcurl test; hostile input for every parser (huge, truncated, wrong types, duplicates, control characters, foreign hosts, 600 rows); and `test_share_privacy`, which searches everything the app writes for the password, token and link. The OCS Share API is boundary behavior tested against the fake, never a real server. |
| II Host-testable core | **Pass.** All logic is in `core/` (Qt-free); `bridge/` holds `ShareBridge` only; platform facts (Wi-Fi, clipboard availability, application state) enter through `ShareBridge::Config` and `app/main.cpp`. Dates take "today" as a parameter. |
| III No silent data loss | **Pass with one justified deviation.** No user file is touched: Remove deletes a *share*, not the item, and says so (spec FR-019); nothing is queued or retried; an unknown outcome is kept as unknown and never repeated (R-8). **Deviation:** the constitution's "every server write and delete is conditional (`If-Match`...)" has no counterpart for OCS shares (they carry no ETag). Replaced by the strongest check the interface allows: a **re-read and compare before every change** and the stale-state message (R-7); the remaining race is seconds wide and loses at most a concurrent edit of a share *setting*, never data. Recorded in Complexity Tracking. Delete confirmation is required; the server trash does not apply to shares (a removed link cannot be restored, and the confirmation says so). |
| IV Security & Privacy | **Pass.** TLS, https-only and the same-host rule come with `DavSession`. A link is never opened, fetched or previewed (N9); the **share modules cannot log** anything but codes, status and counts (N8, on top of R6/R8/N4 which already forbid `link`, `token`, `url`, `password` field names); a list type that cannot hold a link (R-5); the password lives in `SecretText`, is wiped after the request, is never in a URL, never persisted (R-9); the plain password field cannot offer Copy (`forceSecret`, R-10); server text is never shown (R-4). The constitution's "Logs MUST NOT contain share links or tokens" becomes a mechanical rule. Honest limits are stated in README: QML/JavaScript strings and runtime copies cannot be wiped; the guarantee is no persistence and no logging. Credential storage is unchanged (001 R-6). |
| V Simplicity & bounded footprint | **Pass.** No dependency; no table, queue or cache; one request at a time; every answer bounded (2 MiB, 500 rows, field limits); `Depth`-style limits do not apply; no previews, no QR codes, no history, no retry machinery (YAGNI). Largest file size: not applicable (stated). |
| VI E-ink UX & battery | **Pass.** Page buttons, no animation, no polling, no timers (rule N3 unchanged). Network happens only after a tap on a sharing screen (Share, Refresh, a link row, Create, Save, Remove); a tap while offline asks the system to bring Wi-Fi up (as in the files client, FR-006), and sharing never runs from Auto Sync or at start. The screens show the offline state and, because nothing is cached, never a stale list as current. |
| VII Licensing | **Pass.** The OCS Share API is reimplemented from the public documentation; no Nextcloud code, strings or documentation text copied; no new component, so `NOTICE`, `licenses/manifest.txt` and the About box component list are unchanged; the share icon is drawn in the project's icon style. The app stays unofficial. |
| VIII Quality gates | **Pass.** `pre-merge.sh` unchanged in shape; N2 allow-list gains `share_api.cpp` (and `PROPPATCH`/`LOCK`/`UNLOCK` stay forbidden there); new N8 and N9 with self-tests; the extended `test_no_background_network` and `test_share_requests` act as rules. Version bump 0.3.0 -> 0.4.0 and `CHANGELOG.md` are the last phase of tasks.md; the ARM build takes the SDK only from `POCKETBOOK_SDK` / `-DSDK_PATH`. |
| IX Specs-driven & honest verification | **Pass.** Every server and device behavior (capability shapes, permission values, password-state field, enforcement answers, clipboard on the reader, sleep behavior) is labelled unverified; fixtures carry `.unverified`; LV-L1..L10 and DT-L1..L5 are added to the checklists by the task list (R-20) and results are recorded when obtained. |
| X English documentation | **Pass.** All artifacts, comments, identifiers, log keys, test names and script output in English; German only in `qml/strings_de.tsv` and non-English test data. |
| XI Consistent look and feel | **Pass with a note.** Existing components only (TopBar, BottomPanel, IconButton/ToolButton, InkRow, ModelPager/PageButtons, ChoiceDialog, ConfirmDialog, TextEntryDialog, Style); no bordered text button, no animation. **Notes:** (1) `TextMenu` gets one additive property (`forceSecret`); (2) the select-mode panel of `FolderPage` gains a Share tool (six tools: Rename, Share, Move, Copy, Delete, Done) and `DetailsPage`'s panel may hold five (Download, Keep offline, Update, Open, Share) - DT-L1 checks that both fit at the usual touch size, owner point 3. |

Mechanical constraints (Security & Data Handling): the data folder, database and credentials file keep their permissions; this feature
writes no file. **Technology constraints check**: C++17, Qt 6.8 Quick/QML, CMake, libcurl, OpenSSL, SQLite; HTTP through the existing
transport; JSON through `json_min`; no new XML or WebDAV library. App name `pbcloud` unchanged; no collision with PB Notes or PB Office.
Passes.

**Post-design re-check (after Phase 1):** the design artifacts do not introduce a violation. The three points that came closest are
decided and contained: no `If-Match` for shares (R-7, Complexity Tracking); messages derived from rules and status, not from server
text (R-4), so FR-023 cannot be broken by a server's wording; the plain password field and the clipboard (R-10, R-11), where the
shared `TextMenu` could have offered Copy on a secret and the device default for Copy is off until DT-L2.

### Open points for the owner (not violations)

1. **Clipboard on the reader.** `qml/TextMenu.qml` records that the reader's clipboard is the app's own as far as *pasting from other
   apps* goes; copying *out* is unmeasured. The plan ships Copy **off** on the device (`PBC_DEVICE_CLIPBOARD=OFF`) and on in the host
   build; DT-L2 decides. If it stays off, the link is carried by reading it off the screen (the spec's clarified fallback).
2. **List size.** The OCS list has no paging for a path. The plan reads one bounded answer (2 MiB, 500 rows) and pages the *display*;
   beyond 500 shares the screen says so and points to the web interface. The spec's edge case ("fetched in bounded pieces") is met by
   the bound, not by true piecewise fetching. `/speckit-analyze` may flag the wording.
3. **Share tool placement.** A folder has no details page (its row opens the folder), so the Share tool goes in select mode of the folder page,
   next to Rename, Move, Copy, Delete and Done (six tools; the panel splits the width evenly), and on the details page for a file. DT-L1
   checks both panels at the usual touch size; if the select-mode panel is too tight, **Done** can become the top-bar action of the page.
4. **Past-date check clock.** "Not before today" is checked against the server's day from the HTTP `Date` header when known, otherwise
   the reader's clock; the server's refusal is final (R-12). The spec's "the reader's clock is never used to say a link is expired"
   holds: the clock is used only to refuse an obviously past *input* early.

## Project Structure

### Documentation (this feature)

```text
specs/004-share-links/
├── plan.md              # This file (/speckit-plan command output)
├── research.md          # Phase 0 output: decisions R-1 .. R-20
├── data-model.md        # Phase 1 output: in-memory entities, validation, state machine, secret lifecycle (no schema change)
├── quickstart.md        # Phase 1 output: how to validate on host, fake server, real server, device
├── contracts/
│   ├── share-requests.md    # every request to the server, mapping, bounds (unverified)
│   ├── core-interfaces.md   # Qt-free C++ interfaces of the new core modules, new error keys
│   └── bridge-qml.md        # ShareBridge, pages, dialogs, messages
├── checklists/
│   └── requirements.md  # specification quality checklist
└── tasks.md             # Phase 2 output (/speckit-tasks command - NOT created by /speckit-plan)
```

### Source Code (repository root)

```text
core/
├── include/pbc/
│   ├── secret_text.h  share_types.h  share_rules.h  share_date.h                   # new: secret holder, entities, rules from capabilities, dates
│   ├── share_parse.h  share_diagnose.h  share_api.h                                # new: tolerant parsers, validation and diagnosis, the six requests
│   ├── error_keys.h                                                                # changed (additive: share_* keys)
│   └── (all 001-003 headers kept)
└── src/                                                                            # one .cpp per header above; error_keys.cpp, log_codes.h (share.* codes)
bridge/
├── include/  share_bridge.h                                                       # new
└── src/      share_bridge.cpp                                                      # new; worker.cpp reused; connection_bridge (account-changed signal read only)
app/
├── main.cpp                     # wires ShareBridge (clipboard setter, connected host, user id); PBC_DEVICE_CLIPBOARD
├── ui_script.{h,cpp}            # new verbs for the walk (share, create-link, show-link, change-link, remove-link); host build only
qml/
├── pages/  SharePage.qml  ShareLinkPage.qml  ShareFormPage.qml  (new)
│           FolderPage.qml  DetailsPage.qml  (changed: Share tool)
├── TextMenu.qml (changed: forceSecret)  Main.qml (changed: context property)  strings_de.tsv (German texts for every new string)
├── icons/  share.svg (new, in the project's icon style, if the set has none)
tests/
├── unit/         test_secret_text  test_share_rules  test_share_date  test_share_parse  test_share_diagnose  test_share_form
│                 test_error_keys (extended)  test_log (extended)
├── integration/  test_share_requests  test_share_list  test_share_create  test_share_change  test_share_remove  test_share_refusals
│                 test_share_unknown_outcome  test_share_privacy  test_share_curl (real libcurl, TLS server)
│                 test_no_background_network (extended)  test_budget_share
├── bridge/       test_share_bridge
├── support/      fake_nextcloud_server.h (extended: shares side, rule switches, link addresses, failure injection)
├── tools/        pbc_fake_nextcloud.cpp (console: share-rules, share-list, unshare, lose-next-answer)
├── walks/        18-share-links.walk  (CTest ui_walk)
└── fixtures/nextcloud/shares/   anonymized share answers with the .unverified suffix
scripts/          check-constitution.sh (N2 entry, N8, N9, self-tests), check-ui-style.sh (link display rule), run-host-demo.sh (console)
docs/             live-server-checklist.md (LV-L1..L10), device-test-checklist.md (DT-L1..L5), device-findings.md
README.md  CHANGELOG.md  VERSION (0.4.0)
```

**Structure Decision**: keep the four-layer layout unchanged (Principle II). The sharing code is a block of new Qt-free modules next to
the existing ones, with `share_api.cpp` as the only file that names `PUT` or `DELETE` for shares, so rule N2 stays an allow-list. Secrets
are confined by types (`SecretText`, `ShareRow` without a link) and by script rules (N8, N9), not by convention. Platform behavior
(Wi-Fi, clipboard, application state) enters only through `ShareBridge::Config`. The fake Nextcloud grows a shares side so the demo
server and the tests exercise the same contract.

### Suggested delivery order (input for `/speckit-tasks`)

1. **Foundations**: `SecretText`; `share_types`; `share_rules`; `share_date`; `share_parse` (list and detail, hostile fixtures);
   `share_diagnose`; new error keys with German texts; the fake server's shares side and rule switches; rules N2 entry, N8, N9 with
   self-tests; log codes.
2. **US7 Privacy** (cross-cutting, P1): `test_share_privacy`, `SecretText` wiping, `forceSecret`, the plain-text link display rule in
   `check-ui-style.sh`, `clearSecrets()` paths. Written early so every later story is built inside it.
3. **US2 See the shares**: `ShareApi::fetch_rules`/`list`, `ShareBridge.open/refresh/rows`, `SharePage`, the Share tool on
   `FolderPage` (select mode) and `DetailsPage`, offline behavior.
4. **US1 Create** and **US3 Show**: `create`, the form page, the confirmation, `ShareLinkPage`, Copy and the clipboard constant.
5. **US4 Change** and **US5 Remove**: the pre-check and "changed meanwhile", the dialogs, remove with confirmation.
6. **US6 Messages**: `diagnose_refusal` with the second L1, the marked fields, every message; its validation half is built in step 4.
7. **Polish**: budget test, LV/DT checklist entries, README/About text, walk 18, German texts completed, version 0.4.0,
   `package-device.sh` run.

US7, US2, US1 and US3 together are the P1 minimum (see, create, show, safely); US4-US6 can each be stopped after their own tests pass.

## Complexity Tracking

> Filled because several additions go beyond the smallest possible change; none violates the constitution except the one noted.

| Addition | Why Needed | Simpler Alternative Rejected Because |
|----------|------------|-------------------------------------|
| Re-read-and-compare instead of an `If-Match` on share changes | Principle III asks for conditional writes; OCS shares have no ETag | A blind `PUT` silently overwrites a concurrent change; skipping Story 4 removes a requirement. The residual race is seconds wide and affects a setting, not user data (R-7) |
| `SecretText` (move-only, wiping) and a list type without a link | Principle IV: a secret that cannot be copied, printed or placed in a list cannot leak through a later careless change | Convention and review alone have missed such leaks; the compiler and one test make the guarantee mechanical |
| Rules N8 and N9 in `scripts/check-constitution.sh` | Principle VIII: every rule that can be scripted must be; "no share links in logs" and "never opened" are the feature's main promises | Relying on R6/R8/N4 field-name checks alone would still allow `qDebug() << link` or an `openUrl` |
| A second capabilities request while diagnosing a refusal | R-4: messages must come from rules, not from the server's text; the rules may have changed since the screen opened | Matching the server's message text breaks with every language and wording; showing it breaks FR-023 |
| `forceSecret` property on the shared `TextMenu` | A plain-text password field would otherwise offer Copy and put a secret on the clipboard | A masked field contradicts the spec (the user reads what they type); a separate text field component duplicates the menu |
| `form_encode` (20 lines) | The transport carries a body string; no existing code builds form bodies | A library helper is a new dependency for 20 lines |
| Extending the fake Nextcloud with a shares side and rule switches | Principle I: boundary behavior is proven against a repeatable in-process fake | Hand-scripted answers per test duplicate server logic and cannot model enforcement and lost answers consistently |

## Implementation notes (2026-10-22)

Changes to this plan, found while building (details in `tasks.md` and `acceptance.md`):

- **Entry point.** A folder has no details page, so **Share** is a tool in *select mode* of `FolderPage` (six tools) and on `DetailsPage` for a file (R-13, rewritten).
- **API shape.** `ShareApi::remove` is `remove_link` (rule N5 reserves the name `remove`); `create` and `change` take the rules the screen holds (no hidden capabilities
  read before a request); `change` also takes the `Snapshot` (which now holds `owned_by_me`); `detail(id, item_is_folder)`; `ShareOp` has a fifth value, `Detail`;
  `RefusalContext` carries the diagnosis input; `DavSession::send` got two additive parameters (`total_timeout_s`, `secret_body`).
- **Bridge.** `ShareBridge::message` carries `{key, field, fieldMask, limitDays, code}`; the texts come from `qml/Problems.qml` (`title`/`action` per key) and the page adds
  the limit in days and the short code. Results of the actions arrive as signals (`createFinished`, `changeFinished`, `removeFinished`, `linkOpened`); `leaveLink`,
  `clearMessage`, `dateText` and the detail keys `passwordValue`, `expiryValue`, `hasPassword`, `expires`, `permission` were added.
- **Link page.** The password, expiry and permission are changed by tapping their rows (not by bottom-panel tools); Copy and Remove link are the tools.
- **Walks.** `19-share-limits.walk` exists because the server's rules can only be set before a walk starts.
- **Order.** `ShareBridge` is declared before `SyncBridge` in `app/main.cpp` (destruction order, see T042 of `tasks.md`).
- **Fake server.** Control characters in JSON strings are now escaped (`quote_json`), as a real server does; the shares side has `set_share_password/permissions/expiration`.

