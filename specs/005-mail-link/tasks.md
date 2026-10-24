---

description: "Task list for mailing a share link from the link screen to PB Office"
---

# Tasks: Mail Link

**Input**: Design documents from `/specs/005-mail-link/`

**Prerequisites**: [plan.md](plan.md), [spec.md](spec.md), [research.md](research.md), [data-model.md](data-model.md),
[contracts/handoff-call.md](contracts/handoff-call.md), [contracts/bridge-qml.md](contracts/bridge-qml.md), [quickstart.md](quickstart.md)

**Tests**: REQUIRED. The constitution makes Test-First non-negotiable (Principle I): every implementation task is preceded by test tasks, and
each story has a "red" task that records the new tests failing for the right reason before any implementation task of that story starts. The real
call exists only on the reader, so the tests use an injected fake (`Config::hand_off`); what only the reader can show is a labelled device check
(DT-M1..DT-M4, unverified until run, Principle IX).

**Organization**: Tasks are grouped by user story (US1-US3 of the spec). All three are P1. **Phase order differs from story numbers on purpose:**
US3 (privacy) comes first because it defines the test harness the other stories are written inside (as in feature 004); US1 (send) then makes its
cases pass; US2 (a hand-off that fails) extends the same method.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: can run in parallel (different files, no dependency on an unfinished task)
- **[Story]**: US1-US3, only in the user-story phases
- Paths are relative to the repository root. `core/include/pbc/X.h` and `core/src/X.cpp` come as a pair; a new `core/src/*.cpp` is added to
  `PBC_CORE_SOURCES` in `core/CMakeLists.txt` in the same task; a new bridge test is added to both `foreach` lists in `tests/CMakeLists.txt`;
  a new walk to the `ui_walk` list in `app/CMakeLists.txt`. `tests/unit/test_*.cpp` is picked up by the glob.
- Every task that touches a user-visible text also adds the German entry to `qml/strings_de.tsv` (`scripts/extract-strings.py --check` must stay green).
- FR-/SC- numbers refer to [spec.md](spec.md); R- numbers to [research.md](research.md).
- No task adds a TLS bypass, a timer, polling, a retry, a queue, a stored copy of a link, a log field with a link, subject, item name or password, a
  second `OpenBook2` call site, a password parameter, a `to=` key, or anything that opens a link (rules N1-N10 enforce this mechanically).
- Documents outside `specs/`, `docs/reference/`, `NOTICE` and `licenses/` write "PB Office" with a space and never the identifier of its launcher (rule R11); `docs/device-findings.md` and `app/mail_handoff.cpp` are the allowed exceptions.
- The constitution file is changed only by T001, through its own command; no later task edits it.

## Phase 1: Setup (the amendment the hand-off needs)

**Purpose**: the rule that keeps the name of the other app out of the source tree gets its one new entry before any file names it.

- [X] T001 Amend `.specify/memory/constitution.md` from 1.1.0 to 1.2.0 with `/speckit-constitution` (MINOR, owner-approved when the 005 specification was started): the Technology paragraph names `app/mail_handoff.cpp` as the third place where the launcher path of PB Office may appear (after `app/probe/pbc_handoff.cpp` and `docs/device-findings.md`), states that the single `OpenBook2` call of the product is in that file, and keeps every other rule; set the amended date. Do not touch any other principle.
- [X] T002 Update `scripts/check-constitution.sh` for R11: add `app/mail_handoff.cpp` to the allowed files of the R11 case and its header comment, and add self-test cases: a pass case for `app/mail_handoff.cpp` with the launcher path, and a violation case for `app/other_glue.cpp` with the launcher path. Run `bash scripts/check-constitution.sh --self-test` (red before the script change is made, green after).

---

## Phase 2: Foundational (blocking: the argument list and the rule that confines the call)

**Purpose**: the pure part of the hand-off and the mechanical rule N10. No user story starts before both are green.

- [X] T003 [P] Write `tests/unit/test_mail_handoff.cpp` (failing first) for `pbc::build_mail_handoff` and `pbc::clean_subject` ([contracts/handoff-call.md](contracts/handoff-call.md) section 1): the list is exactly `{"pbo-mail-v1", "text=<link>", "textlen=<n>", "subject=<subject>"}` in this order; `text=` equals the link byte for byte; `textlen` is the decimal byte length including multi-byte characters ("Äö€" is 7 bytes); there is **no `to=` key** and no other key; a link of exactly 8192 bytes is accepted and 8193 bytes gives `MailError::LinkTooLong`; an empty link gives `LinkEmpty`; a link with a control character (a byte below 0x20 or 0x7f) or invalid UTF-8 gives `LinkInvalid` and no argument list; `clean_subject` turns control characters and line breaks into one space, collapses blank runs, trims, and keeps at most 300 characters (code points) cut at a character boundary (a 4-byte character at the limit is dropped whole, never split); an empty name gives the prefix alone; `MailHandoff::wipe()` overwrites every string with zeros and empties the list; the type is move-only (a `static_assert` on `is_copy_constructible`); the link argument's buffer is taken (moved) and wiped.
- [X] T004 Run the new test and record it failing for the right reason (symbols missing, not a typo): `cmake --build build-host && ctest --test-dir build-host -R mail_handoff`.
- [X] T005 Write `core/include/pbc/mail_handoff.h` and `core/src/mail_handoff.cpp` per [contracts/handoff-call.md](contracts/handoff-call.md) section 1 (`kMailTextLimit = 8192`, `kMailSubjectLimit = 300`, `MailError`, move-only `MailHandoff` with `argv()` and `wipe()` and a wiping destructor, `build_mail_handoff(std::string&& link, const std::string& subject_line)`, `clean_subject`), reusing `pbc::utf8` and `pbc::secure_wipe`; the marker and the key names are string literals in this file only; no logging; add the source to `PBC_CORE_SOURCES` in `core/CMakeLists.txt`. `ctest -R mail_handoff` green.
- [X] T006 [P] Write the self-test cases of rule N10 in `scripts/check-constitution.sh` (failing first, in the `--self-test` block next to N8/N9): `OpenBook2(` in `bridge/src/a.cpp` is a violation, in `app/mail_handoff.cpp` it passes; the literal `pbo-mail-v1` in `app/a.cpp` is a violation, in `core/src/mail_handoff.cpp` it passes; `qDebug()`, `printf(` and `log::text(` in `core/src/mail_handoff.cpp` and `app/mail_handoff.cpp` are violations; the same text in `tests/` passes.
- [X] T007 Implement rule N10 in `scripts/check-constitution.sh` and describe it in the header comment: (a) `OpenBook2` appears in `core/`, `bridge/`, `app/` and `qml/` only in `app/mail_handoff.cpp` (comments, `app/probe/` and `tests/` are exempt as they are for the other call rules); `app/mail_handoff.cpp` does not look at the reader's applications (no `access(`, `stat(` or `std::filesystem`, spec FR-020); (b) the literal `pbo-mail-v1` appears in `core/`, `bridge/`, `app/` and `qml/` only in `core/src/mail_handoff.cpp`; (c) `core/src/mail_handoff.cpp`, `core/include/pbc/mail_handoff.h`, `app/mail_handoff.cpp` and `app/mail_handoff.h` cannot log: the pattern list of N8 (no `log::text`, `log::path`, `log::Field`, no `qDebug`/`qWarning`/`qInfo`/`qCritical`, no `std::cerr/cout/clog`, no `printf` family). `bash scripts/check-constitution.sh --self-test` and `bash scripts/check-constitution.sh` green.

**Checkpoint**: the argument list is built and tested without Qt or InkView; the rules are in force.

---

## Phase 3: User Story 3 - The link and password stay private (Priority: P1) - the harness

**Goal**: a test harness in which every later task of the feature is written; the cases are enabled as the flow they need appears.

**Independent Test**: [quickstart.md](quickstart.md) section 1: the privacy cases of `test_mail_link` pass once US1 is done.

- [X] T008 [US3] Create `tests/bridge/test_mail_link.cpp` (Qt Test, offscreen, `PBC_FAKE_SERVER_TEST`) with an `Env` built like the one of `tests/bridge/test_share_bridge.cpp` (fake Nextcloud with a share that has a known link text and a known password, a `ShareBridge` with `Config::hand_off` set to a fake that **records** every argument vector it receives and returns a configurable answer, a log capture, a temporary data folder), and write these cases as **pending/failing** (they need `mailLink`): the password used when creating the link occurs in no recorded argument and not as a prefix of one (US3 scenario 1); the link occurs in the recorded arguments and nowhere else; after a full run (create with password, open, mail, answer 0; and a run with answer 1) a search of the log capture, the database file, the settings and cache files and the temporary data folder finds neither the link, nor the password, nor the item name (US3 scenario 2, SC-003, SC-006, using `tests/support/privacy_probe.h` as `test_share_privacy` does); no request is made because of the hand-off (the fake server's request count is unchanged by `mailLink`; US3 scenario 5). The wiping of the arguments is covered by T003 (a live `MailHandoff`); the bridge cannot observe a destroyed buffer, so this task adds no wipe case and says so in a comment.
- [X] T009 [US3] Register `test_mail_link` in both `foreach` lists in `tests/CMakeLists.txt` and run it: record the cases failing for the right reason (`mailLink` missing, not a typo or a setup error). This is the red task of US3; T011 and T018 are the red tasks of US1 and US2.

**Checkpoint**: the harness exists and is red for the right reason.

---

## Phase 4: User Story 1 - Send a link by mail from the link screen (Priority: P1) 🎯 MVP

**Goal**: Mail link on the link screen -> confirmation (Cancel default) -> one call -> PB Office opens a new message; two taps.

**Independent Test**: in the host build with the stand-in answering 0, tap Mail link and confirm: exactly one hand-off with the link as its text and the subject naming the item, no password, no recipient; Cancel makes none. On the reader (PB Office installed, not running): DT-M1.

### Tests for User Story 1 (write first, they must fail)

- [X] T010 [P] [US1] Add these cases to `tests/bridge/test_mail_link.cpp` ([contracts/bridge-qml.md](contracts/bridge-qml.md)): `mailLink(false)` on an open readable link returns `{needsConfirm: true, text}` where the text names PB Office and a new draft, says the draft is saved to the Drafts folder later, and mentions that a password is not included **only when the link has a password**, and the fake is not called (US1 scenarios 2, 4); `mailLink(true)` calls the fake **exactly once** with `argv == {"pbo-mail-v1", "text=" + link, "textlen=" + len, "subject=Link to " + item name}` (the English source of the prefix) and no `to=` (scenario 3, SC-002); after answer 0 the signal `mailHandedOver` is emitted once, `detail.linkText` is empty and the bridge's link is gone (scenario 6); a second `mailLink(true)` after acceptance returns `{errorKey: "busy"}` and makes no second call (FR-005); `mailLink` for a link with state `unreadable` or `different_address` returns `{errorKey: "mail_link_unavailable"}` with no call, for `not_secure` it works; with no link open it returns `{errorKey: "share_item_gone"}`; while a sharing request runs (`busy`) it returns `{errorKey: "busy"}` with no call; an item name with a line break and 400 characters gives a one-line subject of at most 300 characters; the bridge makes no call when `Config::hand_off` is empty (answered like US2, tested in T017).
- [X] T011 [US1] Run `ctest --test-dir build-host -R mail_link` and record the US1 cases failing for the right reason (the US3 cases of T008 stay red until T014).

### Implementation for User Story 1

- [X] T012 [US1] Extend `bridge/include/share_bridge.h`: `Config::hand_off` (`std::function<int(const std::vector<std::string>&)>`, empty = no hand-off possible), `Q_INVOKABLE QVariantMap mailLink(bool confirmed)`, `signal mailHandedOver()`, the private flags `mail_in_flight_` and `mail_sent_`, and the comment block that states the rules (nothing logged, nothing stored, one call, the password never involved). Implement `mailLink` in `bridge/src/share_bridge.cpp` per [research.md](research.md) R-4..R-7 and [contracts/bridge-qml.md](contracts/bridge-qml.md): checks in the order link open -> link state -> busy/in flight/sent -> text size; without `confirmed` return `{needsConfirm, text}` (two texts, with and without the password sentence, through `bridge_tr`); with `confirmed` set `mail_in_flight_`, build the subject with `bridge_tr("Link to %1")` and `item_name_` through `pbc::clean_subject`, build the list with `pbc::build_mail_handoff(std::string&&, ...)` from the link bytes, overwrite the temporary `QByteArray` and `std::string` in place, call `c_.hand_off` **once**, `wipe()` the `MailHandoff`, clear `mail_in_flight_` (hold the temporaries and the `MailHandoff` in one small RAII guard whose destructor wipes everything, so that no return path of `mailLink` skips the wipe, FR-015); on answer 0 set `mail_sent_`, call `leaveLink()` and emit `mailHandedOver()`. `clearSecrets()` and `setDetail()` reset `mail_in_flight_` and `mail_sent_`. No `log::` call, no `qDebug` (rule N8). The refused-answer path is T018.
- [X] T013 [US1] Write `app/mail_handoff.h` and `app/mail_handoff.cpp` ([contracts/handoff-call.md](contracts/handoff-call.md) section 2): `int send_mail_handoff(const std::vector<std::string>& argv)`; device (`PLATFORM_POCKETBOOK`): build a `std::vector<const char*>` of pointers into `argv` and call `OpenBook2(<launcher path of PB Office>, n, pointers, 0)` once, return its value; the path is a literal in this file only and the header does not name the other app; host: return the integer in the environment variable `PBC_HOST_HANDOFF_RESULT` (default 0, anything unparsable gives 1) and do nothing else; no logging; add the file to `PBC_APP_SOURCES` in `app/CMakeLists.txt`; wire `shc.hand_off = send_mail_handoff;` in `app/main.cpp` with a comment that the call is made only after the user's confirmation.
- [X] T014 [US1] Run `ctest --test-dir build-host -R 'mail_link|mail_handoff|share_bridge'`: the US1 cases and the privacy cases of T008 (US3) are green; fix the code, never the test, unless the test is wrong (say why).
- [X] T015 [P] [US1] Add the tool and the confirmation to `qml/pages/ShareLinkPage.qml` ([contracts/bridge-qml.md](contracts/bridge-qml.md)): `ToolButton { objectName: "shareLinkMail"; text: qsTr("Mail link"); icon: "send" }` left of Remove link, `visible` when `share.detail.linkState` is `ok` or `not_secure`, `enabled: !share.busy`; `askMail()` calls `share.mailLink(false)` and opens `ConfirmDialog { objectName: "shareMailConfirm"; title: qsTr("Mail link"); acceptText: qsTr("Mail link") }` (Cancel first and default) with the returned text, or shows the page banner for an `errorKey`; `onAccepted: share.mailLink(true)`; a `Connections` on `onMailHandedOver` pops the page like `onRemoveFinished`. Add `mail_link_unavailable` and `mail_too_long` to `qml/Problems.qml` (titles and action lines as in the contract). Add the German texts for the tool label, both confirmation texts, the subject prefix and the two messages to `qml/strings_de.tsv`; run `python3 scripts/extract-strings.py --check`.
- [X] T016 [US1] Add the walk verbs `mail-link`, `confirm-mail`, `cancel-mail` and `set-handoff <n>` (sets `PBC_HOST_HANDOFF_RESULT` for the process) to `app/ui_script.cpp` / `.h` (host build only) and write `tests/walks/20-mail-link.walk` part one: open the link screen of a created link, tap Mail link (screenshot of the confirmation with Cancel first), Cancel (the link screen is unchanged), Mail link again, confirm (the link screen is gone, the list is shown); register the walk in `app/CMakeLists.txt` (`ui_walk`). Read the screenshots. **Checkpoint**: run `ctest --test-dir build-host --output-on-failure` and the walk; US1 is done when both are green.

**Checkpoint**: the P1 minimum (send, privately) works on the host. DT-M1 and DT-M3 are the device checks of this story (T025).

---

## Phase 5: User Story 2 - A hand-off that fails is explained, not repeated (Priority: P1)

**Goal**: any answer other than 0, and a missing function, give one calm notice the next time the screen is shown; no second call; the link is not re-shown.

**Independent Test**: with the stand-in answering 1, confirm Mail link: one call, the notice dialog, no second call; DT-M2 on the reader.

### Tests for User Story 2 (write first, they must fail)

- [X] T017 [P] [US2] Add these cases to `tests/bridge/test_mail_link.cpp`: answer 1 -> `mailLink(true)` returns `{errorKey: "mail_not_accepted"}`, `mailNotice` is `"mail_not_accepted"`, exactly one call was made, no `mailHandedOver`; the same for answer 127 and for an empty `Config::hand_off` (US2 scenarios 1, 3); a second `mailLink(true)` after a refusal is allowed (the user's deliberate repeat) and is a second call only when the user confirms again, never by itself (scenario 2: advance the clock and process events, the count stays 1); `clearSecrets()` (the app switched away) keeps `mailNotice` and clears the link, the rows and the flags; `dismissMailNotice()` empties it and emits `mailNoticeChanged`; the text of the notice and every message contain neither the link, nor the subject, nor the item name (search the QVariant map and the strings of `Problems` keys via the bridge's message map); a link longer than 8192 bytes (set through `setDetail` in the test) is not handed over and answers `{errorKey: "mail_too_long"}` (scenario 4, FR-009).
- [X] T018 [US2] Run `ctest -R mail_link` and record the US2 cases failing for the right reason.

### Implementation for User Story 2

- [X] T019 [US2] In `bridge/include/share_bridge.h` and `bridge/src/share_bridge.cpp`: `Q_PROPERTY(QString mailNotice READ mailNotice NOTIFY mailNoticeChanged)`, `Q_INVOKABLE void dismissMailNotice()`, `signal mailNoticeChanged()`; in `mailLink(true)` a non-zero answer (or an empty `c_.hand_off`) sets `mail_notice_ = "mail_not_accepted"`, emits the signal, clears `mail_in_flight_`, and returns `{errorKey}`; the text-size check returns `mail_too_long` before any call; `clearSecrets()` does **not** clear `mail_notice_` (it holds a key only, [data-model.md](data-model.md) MailNotice). Still no logging.
- [X] T020 [US2] Show the notice: in `qml/pages/ShareLinkPage.qml` a banner with `Problems.title("mail_not_accepted") + " " + Problems.action("mail_not_accepted")` while the page is open and `share.mailNotice` is set (the app was not switched away); in `qml/Main.qml` a `ConfirmDialog { objectName: "mailNoticeDialog"; showCancel: false }` opened when `share.mailNotice !== ""` and the application is active, whose `onAccepted`/`onRejected` call `share.dismissMailNotice()` (a banner and the dialog never show together: the dialog opens only when no sharing screen is on the stack). Add `mail_not_accepted` (title and action as in [contracts/bridge-qml.md](contracts/bridge-qml.md)) to `qml/Problems.qml` and the German texts to `qml/strings_de.tsv`; run `python3 scripts/extract-strings.py --check`.
- [X] T021 [US2] Add the walk verb `dismiss-notice` to `app/ui_script.cpp` and write part two of `tests/walks/20-mail-link.walk`: `set-handoff 1`, open a link, Mail link, confirm, the notice dialog is shown (screenshot), `dismiss-notice`, the dialog is gone and no link is shown; read the screenshots.
- [X] T022 [US2] Run `ctest --test-dir build-host -R 'mail_link|mail_handoff'` and the walk; the US2 cases are green. Run the full suite `ctest --test-dir build-host --output-on-failure`.

**Checkpoint**: all three stories work on the host; the failure case is explained.

---

## Phase 6: Polish & cross-cutting

**Purpose**: the cross-cutting tests, the documents, the version, the gates. Nothing here is claimed as verified on the reader before the device checks are run.

- [X] T023 [P] Extend `tests/bridge/test_no_background_network.cpp`: a confirmed `mailLink` (answers 0 and 1) makes no request and does not ask the system for Wi-Fi (`ensure_calls` stays 0), also with `wifi_up` returning false (Mail link works offline, spec edge case), and nothing from this feature runs at app start or while idle (spec FR-017, constitution VI).
- [X] T024 [P] Update `docs/device-test-checklist.md` with section "Mail link (feature 005)" and rows DT-M1..DT-M4 exactly as in [quickstart.md](quickstart.md) section 4 including the timing of DT-M1 (status `U`, date and note empty), and add a note to `docs/device-findings.md` that DT-M2 records the value returned when PB Office is missing (open item O-1 of [research.md](research.md)).
- [X] T025 Build the device package (`scripts/build-curl-arm.sh` if needed, then the ARM build with `POCKETBOOK_SDK=<path>` given only in the command, then `scripts/package-device.sh`) so the owner can run DT-M1..DT-M4; this task only prepares the package and records its file name, it does not mark any device item as done.
- [X] T026 [P] Update `README.md`: what Mail link does, that it needs PB Office 0.4.0 or later installed and **not running**, and the honest limits (the system and PB Office receive the text and PB Office keeps a draft that later syncs to the Drafts folder; the app does not store, log or clipboard the link and overwrites its buffers; the password is never handed over; a running PB Office does not take the link). Update `INSTALL.md` with the same prerequisite in two lines.
- [X] T027 [P] Write `specs/005-mail-link/acceptance.md` mapping SC-001..SC-006 and the scenarios of the three stories to the test, walk or checklist item that proves them, and listing what stays manual or unverified (DT-M1..DT-M4).
- [X] T028 [P] Run `python3 scripts/extract-strings.py --check` and complete `qml/strings_de.tsv` for every new source string of the feature.
- [X] T029 Raise `VERSION` to `0.5.0` and add the entry "0.5.0 - 2026-10-24 ... Mail link (specs/005-mail-link)" to `CHANGELOG.md` in the style of 0.4.0 (what is new, the honest limit about a running PB Office, "tested on the host; DT-M1..DT-M4 not yet run").
- [X] T030 Configure the sanitizer build (`build-asan`, `-DPBC_SANITIZE=ON`) and run `test_mail_handoff`, `test_mail_link` and `test_share_bridge`: no finding.
- [X] T031 Run `scripts/pre-merge.sh` (version bump, build, all tests including `ui_walk`, secret scan, constitution/fixture/UI-style/translation checks): green. If anything fails, fix it before reporting; record the result in `acceptance.md`.

---

## Dependencies & Execution Order

### Phase dependencies

- Phase 1 (T001, T002) first: the rule must know the new file before the file exists.
- Phase 2 (T003-T007) blocks every story: the argument list and rule N10.
- Phase 3 (US3 harness) before Phase 4 and 5: the stories are written inside it.
- Phase 4 (US1) before Phase 5 (US2): US2 extends `mailLink`.
- Phase 6 last; T025 (the package) needs US1 and US2.

### Within each story

Tests first (they must fail), the red task, then core/bridge, then glue, then page, then the story's check task. US3's privacy cases are green only after T014.

### Parallel opportunities

- Phase 2: T003 and T006 together; T005 follows T004, T007 follows T006.
- US1: T010 with T015's preparation of strings is not allowed to overlap T012 (same bridge file); T013 (app glue) and T015 (QML) are in different files and can proceed together after T012.
- US2: T017 alone, then T019, then T020 and the walk.
- Polish: T023, T024, T026, T027, T028 are parallel.

---

## Implementation Strategy

### MVP first (US3, US1)

1. Phase 1 and 2: the amendment, the argument list, rule N10.
2. US3 harness (red), then US1: stop and validate on the host (the walk, the screenshots of the confirmation).
3. Build the package (T025) and run DT-M1 and DT-M3 on the reader before US2's wording is final: the notice text depends on what the reader returns when PB Office is missing (open item O-1).
4. US2, then Polish.

### Incremental delivery

- Each phase ends with its tests green. The version is raised once (T029).
- Unverified items stay unverified: no task marks a device behavior as verified.

### Notes

- Never edit a test to make it pass without understanding why it failed; a red test that fails for the wrong reason is fixed first (T004, T009, T011, T018).
- The password never enters the hand-off: there is no parameter for it (R-4). If a later change adds one, `test_mail_link` and the type must fail first.
- Only `app/mail_handoff.cpp` calls `OpenBook2` and names PB Office's launcher; only `core/src/mail_handoff.cpp` holds the marker; `scripts/check-constitution.sh` (N10, R11) enforces it.
- Commit after each task or logical group; commit messages in English (Principle X).
