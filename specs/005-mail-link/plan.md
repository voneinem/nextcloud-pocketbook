# Implementation Plan: Mail Link

**Branch**: `feature/005-mail-link` | **Date**: 2026-10-24 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/005-mail-link/spec.md`

## Summary

On the link screen of a public link (feature 004) a new **Mail link** tool opens a confirmation (default Cancel) that says the link goes to PB Office as a
new draft, that a password is not included, and that the draft is later saved to the Drafts folder on the mail server. On confirmation the app makes
**one** hand-off call to the installed PB Office with the link as the text and a one-line subject (`Link to <item name>`), no recipient, checks the answer,
overwrites every buffer that held the link, and drops the link screen. A refused or impossible hand-off (PB Office missing, or running) is explained by a
short calm notice the next time the app's screen is shown; nothing is retried. The supported case is PB Office **not running**, which the reader tests of
2026-10-23/24 showed to work (return 0, a draft opens) while a running PB Office returns 1 and receives nothing.

Technical approach (details in [research.md](research.md)): one small Qt-free core module (`mail_handoff`) that builds and validates the argument list
(marker, text, text length, subject; limits; wipe); one new method and one injected function in `ShareBridge` (`mailLink`, `Config::hand_off`); one device
glue file (`app/mail_handoff.cpp`) that holds the only call of `OpenBook2` and the only occurrence of PB Office's launcher path; a tool, a confirmation
and a notice in QML. **No schema change, no new dependency, no network, no new timer, nothing stored.** One new mechanical rule (N10: the single call
site, the single path, no logging in the hand-off modules), an R11 allow-list entry for `app/mail_handoff.cpp` (constitution amendment 1.1.0 -> 1.2.0, the
first task) and an extended privacy test.

## Technical Context

**Language/Version**: C++17 (core, bridge, app glue); Qt 6.8 Quick/QML for the UI (device runs Qt 6.10.3, SDK sysroot 6.8.2); CMake

**Primary Dependencies**: none added. InkView (`OpenBook2`, already linked by the app) in the device glue only; `secure_wipe`, `utf8` and `display_safe`
from the core are reused.

**Storage**: none. `pbcloud.db` stays at **schema 5**; no file, setting or table is written by this feature. The link, the subject and the arguments live
only in memory for the duration of one call (spec FR-014, FR-015).

**Testing**: the project's own test framework (`tests/support/test_support.h`) under CTest: unit tests for `mail_handoff`, bridge tests (Qt Test,
offscreen) with an injected fake hand-off, the extended privacy test, the extended no-background-network test, script checks with self-tests (N10, R11),
`app --self-test`, a host UI walk (20) with a host stand-in whose answer can be chosen, and device checks DT-M1..DT-M4 (unverified until run).

**Target Platform**: PocketBook InkPad One (RK3566, armhf); host Linux x86-64 for the complete core and bridge test suite (constitution II)

**Project Type**: desktop-style Qt Quick application with a Qt-free core library (device app plus host tests)

**Performance Goals** (budgets, **unverified until measured on the device**; they restate spec SC-001):

| Quantity | Budget |
|----------|--------|
| Mail link tap -> confirmation shown | <= 100 ms (no I/O) |
| Confirm -> PB Office editor with the link (cold start) | <= 10 s on the device (SC-001); the call itself returned within the same second in the probe runs (2026-10-24) |
| Block of the UI thread by the call | <= 2 s (unverified; observed < 1 s); no timer, no retry |
| Cold start of PB Cloud | unchanged <= 5 s: no hand-off code runs at start |

**Constraints**:

| Quantity | Budget |
|----------|--------|
| RAM | peak <= 150 MB (`VmHWM`) as before; the hand-off holds at most 3 x 8 KiB for the duration of one call |
| Installed size | package <= 25 MB (gate); expected growth < 20 KB |
| Sizes | text <= 8192 bytes (a link is <= 512 bytes); subject <= 300 characters; no `to` key; no other key |
| Network | none: the feature makes no request and opens nothing; the link is not fetched (FR-017) |
| Interface | PB Office's `handoff-contract v1` (sibling project, `specs/003-receive-shared-text/contracts/handoff-contract.md`); PB Office 0.4.0 or later |
| Largest supported file | not applicable (no file data moves) |
| Auto Sync | not involved; stated because constitution VI requires every plan to say so |

**Scale/Scope**: one account; one link at a time; 3 user stories, 21 functional requirements; 1 new core module, 1 new device glue file, changes to 6 files
(`share_bridge.{h,cpp}`, `app/main.cpp`, `app/CMakeLists.txt`, `ShareLinkPage.qml`, `Main.qml`, `Problems.qml`) plus tests, scripts and docs.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design (verdict column).*

Constitution version 1.1.0 (amended 2026-10-24). Result: **pass** with one justified deviation (the first start of another program by this app for a
purpose other than opening a downloaded file, permitted by the technology paragraph of 1.1.0) and one amendment task (R11 for the new source file).

| Principle | Verdict and where it is satisfied |
|-----------|-----------------------------------|
| I Test-First | **Pass.** Every module has failing-first tests (research R-11): `test_mail_handoff` (argument list, subject cleaning at a character boundary, limits, wipe), `test_mail_link` (bridge: exactly one call after confirmation, none after Cancel, none twice, the password never in the arguments, text equals link, non-zero answer gives the notice and no second call, no log), the privacy cases of `test_mail_link` (they search everything the app writes, with `privacy_probe.h`), and script self-tests for N10 and R11. The real call is device-only and is injected (`Config::hand_off`), so the boundary is a fake; the one fact only the reader can give (what PB Office does with the call) is a labelled device check, DT-M1..DT-M4. |
| II Host-testable core | **Pass.** The argument building is in `core/` (Qt-free, no InkView); the bridge calls an injected function; the only InkView call is in `app/mail_handoff.cpp` (device glue), with a host stand-in. The core builds and tests without the SDK. |
| III No silent data loss | **Pass, not applicable to user files.** No file, share or server state is changed; a hand-off creates a *draft in PB Office* (PB Office's data, owned by it) and says so in the confirmation. No retry: a repeated call could create a duplicate draft (FR-005, FR-011). |
| IV Security & Privacy | **Pass.** The link leaves the app only in the one confirmed call; never logged (N8 covers `share_bridge.cpp`, N10 covers the hand-off modules), never stored, never on a clipboard, never opened (N9 unchanged; `OpenBook2` is not a link-opening call and is confined by N10); the password is not part of the hand-off by construction (the core function takes the link and the subject only) and has its own test; every buffer is wiped after the call (R-6). Honest limit stated in README: the system and PB Office receive the text; PB Office writes it to a private pending folder and a draft, and its arguments are visible in the system's process list for the moment before PB Office restarts without them (observed by PB Office's design, R11 of its project; unverified here, DT-M3). The notice and the subject contain no link. |
| V Simplicity & bounded footprint | **Pass.** No dependency, table, queue or timer; one call; a text bounded at 8192 bytes. Largest file size: not applicable (stated). |
| VI E-ink UX & battery | **Pass.** Two taps, no animation, no polling; no network at all; Wi-Fi is not requested (the hand-off needs none). The notice is a dialog with a footer button, like the other dialogs. |
| VII Licensing | **Pass.** PB Office's contract is read from the sibling project (same author, MIT); no code copied; no new component, so `NOTICE`, `licenses/manifest.txt` and the About box are unchanged. The mail icon is drawn in the project's icon style (or the existing `send` icon is used). |
| VIII Quality gates | **Pass.** `pre-merge.sh` unchanged in shape; new rule N10 and the R11 entry with self-tests; the extended tests act as rules; version 0.4.0 -> 0.5.0 and `CHANGELOG.md` are the last phase of tasks.md; the ARM build takes the SDK only from `POCKETBOOK_SDK`. |
| IX Specs-driven & honest verification | **Pass.** What the reader does is cited from `docs/device-findings.md` (observed 2026-10-23/24: cold start accepted, running PB Office returns 1); what has not been observed is labelled unverified and listed as DT-M1..DT-M4 in the checklist, with the results recorded when obtained. Nothing in this feature is claimed as verified on the reader before DT-M1. |
| X English documentation | **Pass.** All artifacts, comments, identifiers, log keys, test names, script output in English; German only in `qml/strings_de.tsv`. |
| XI Consistent look and feel | **Pass.** Existing components only (BottomPanel with a ToolButton, ConfirmDialog, Problems); the confirmation has Cancel as its first button; no bordered button, no animation. The icon is `send` (exists) so that no new asset is needed. |

Mechanical constraints (Security & Data Handling): the data folder, database and credentials file keep their permissions; this feature writes no file.
**Technology constraints check**: C++17, Qt 6.8 Quick/QML, CMake, libcurl, OpenSSL, SQLite; no new library; app name `pbcloud` unchanged. The constitution
(1.1.0) permits exactly one contact with PB Office, a single `OpenBook2` hand-off of a text with the return value checked and no password; it requires this
feature to amend the paragraph for its own source file, which task T001 does (R11 gains `app/mail_handoff.cpp`; no other rule is relaxed). No collision
with PB Notes or PB Office: PB Cloud writes nothing into PB Office's folders and reads nothing from them.

**Post-design re-check (after Phase 1):** the design artifacts introduce no violation. The three points that came closest are decided and contained:
the password cannot reach the hand-off because the builder has no parameter for it and the test searches for it (R-4); the link is dropped by the app
itself right after an accepted hand-off instead of waiting for the platform's background event (R-7); the failure notice survives the clearing of the
sharing screens but holds only a key (R-8).

### Open points for the owner (not violations)

1. **A running PB Office.** The reader returns 1 and delivers nothing. This feature supports the cold start only and says so in the notice. Supporting a
   running PB Office needs a listener in PB Office (their contract v2 or a request listener); not in this scope. The listener experiments of 2026-10-24
   ended without data (the reader restarted), so even PB Office's own planning has no result yet.
2. **Failure notice wording.** "PB Office could not take the link. Close PB Office and try again." is an instruction that is right for the running
   case and incomplete for a missing PB Office; one text covers both (a distinction would need a reliable test for "installed", which FR-020 forbids). The
   notice therefore adds "If PB Office is not installed, the link cannot be mailed." as its second sentence. `/speckit-analyze` may flag the length.
3. **Hand-off on the UI thread.** The call returned at once in the probe runs; the plan runs it on the UI thread (no worker, no timer). If the device
   check DT-M1 shows a visible block, a worker with a result signal replaces it without touching the contract (R-5).

## Project Structure

### Documentation (this feature)

```text
specs/005-mail-link/
├── plan.md              # This file (/speckit-plan command output)
├── research.md          # Phase 0 output: decisions R-1 .. R-12
├── data-model.md        # Phase 1 output: in-memory entities, states, lifetime and wiping of each copy (no schema change)
├── quickstart.md        # Phase 1 output: how to validate on host, with the stand-in, and on the reader
├── contracts/
│   ├── handoff-call.md  # the call as PB Cloud makes it, the core interface, limits, answers, what is promised and what is not
│   └── bridge-qml.md    # ShareBridge additions, the link page tool, the confirmation, the notice, messages, German strings
├── checklists/
│   └── requirements.md  # specification quality checklist
└── tasks.md             # Phase 2 output (/speckit-tasks command - NOT created by /speckit-plan)
```

### Source Code (repository root)

```text
core/
├── include/pbc/mail_handoff.h          # new: MailHandoff (argument list), build_mail_handoff(), clean_subject(), limits; wipes itself
└── src/mail_handoff.cpp                # new: the only file that holds the marker "pbo-mail-v1" and the key names
bridge/
├── include/share_bridge.h              # changed: Config::hand_off, mailLink(), mailNotice, dismissMailNotice(), signal mailHandedOver
└── src/share_bridge.cpp                # changed: mailLink (confirm step, build, one call, wipe, drop link), notice that survives clearSecrets()
app/
├── mail_handoff.h                      # new: int send_mail_handoff(const std::vector<std::string>& argv)  (no PB Office name in the header)
├── mail_handoff.cpp                    # new: the only OpenBook2 call and the only launcher path; host stand-in (answer from the environment)
├── main.cpp                            # changed: shc.hand_off = send_mail_handoff
├── CMakeLists.txt                      # changed: PBC_APP_SOURCES gains mail_handoff.cpp
└── ui_script.{h,cpp}                   # changed: walk verbs mail-link / confirm-mail / dismiss-notice (host build only)
qml/
├── pages/ShareLinkPage.qml             # changed: Mail link tool, confirmation, in-page notice while the screen is open
├── Main.qml                            # changed: the notice dialog (shown when the app is shown again and a notice is waiting)
├── Problems.qml                        # changed: mail_not_accepted, mail_too_long, mail_link_unavailable
└── strings_de.tsv                      # changed: German texts for every new string
tests/
├── unit/         test_mail_handoff
├── bridge/       test_mail_link
├── bridge/       test_mail_link (privacy cases included), test_no_background_network (extended)
├── walks/        20-mail-link.walk  (CTest ui_walk; host stand-in answers 0, then 1)
└── fixtures/     none
scripts/          check-constitution.sh (N10, R11 entry, self-tests)
docs/             device-test-checklist.md (DT-M1..DT-M4), device-findings.md (results when obtained)
.specify/memory/constitution.md   # amended in T001 (1.1.0 -> 1.2.0): the paragraph names app/mail_handoff.cpp
README.md  INSTALL.md  CHANGELOG.md  VERSION (0.5.0)
```

**Structure Decision**: keep the four-layer layout unchanged (Principle II). The pure part (what is handed over and in which form) is a core module with
tests; the only platform fact (the call and the launcher's path) is one small file in `app/`, reached through one injected function, so the bridge and
all its tests never see InkView or the name of the other app. The hand-off is therefore confined by construction (one function, one file) and
by script (N10), not by convention.

### Suggested delivery order (input for `/speckit-tasks`)

1. **Foundations**: the constitution amendment (R11 entry, version 1.2.0) and the script change with self-tests (N10 will fail until the files exist, so
   it lands with them); `mail_handoff` core module, failing tests first.
2. **US3 Privacy** (cross-cutting, P1): the privacy cases of `test_mail_link` (link, password, subject, item name searched in everything written) and
   `test_mail_link` privacy cases, written before the bridge method so that the method is built inside them.
3. **US1 Send**: `ShareBridge::mailLink` with the confirmation step, the tool and the confirmation in `ShareLinkPage`, `app/mail_handoff.cpp`, wiring
   in `main.cpp`, walk 20 part one.
4. **US2 Failure**: the non-zero answer path, `mailNotice`, the notice dialog in `Main.qml`, the messages, walk 20 part two.
5. **Polish**: German texts, README/INSTALL honest-limit text, DT-M1..DT-M4 in the checklist, version 0.5.0, `CHANGELOG.md`, `package-device.sh` run.

US3, US1 and US2 are each independently testable; US1 with US3 is the P1 minimum.

## Complexity Tracking

> Filled because several additions go beyond the smallest possible change; none violates the constitution.

| Addition | Why Needed | Simpler Alternative Rejected Because |
|----------|------------|-------------------------------------|
| A core module `mail_handoff` for an argument list of four strings | The format (marker, length, subject cleaning at a character boundary, limits) is a boundary contract with another app and needs host tests (Principle I); the bridge and the glue must not each re-implement it | Building the strings inline in the bridge cannot be tested without Qt and would put the contract in two places |
| Rule N10 and an R11 entry | Principle VIII: the promise "one call, one place, no logging" is the feature's main risk; the amendment of 1.1.0 asks for it | Review alone has missed such leaks; one grep with a self-test is cheap |
| `Config::hand_off` as an injected function and a host stand-in | The real call exists only on the reader; tests and the walk need a double that can answer 0, 1 or "missing" | A compile-time `#ifdef` inside the bridge hides the call from tests and mixes InkView into `bridge/` |
| The notice survives `clearSecrets()` (a key only) | After a refused call PB Office comes to the front and the app is switched away, which clears the sharing screens; the spec wants the message the next time the screen is shown (FR-011) | Showing it only on the link screen would never be seen in the running-PB-Office case, which is the case that fails |
| Dropping the link right after an accepted call | FR-015/FR-016: the link must not wait for a platform event that is unverified on the reader | Relying on the background event leaves the link on screen if the event does not come |
