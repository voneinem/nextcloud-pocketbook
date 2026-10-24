# Research: Mail Link

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **Date**: 2026-10-24

Decisions taken before the design was fixed. Evidence labels (constitution Principle IX):

- **[REPO]** read from this repository on 2026-10-24.
- **[SIBLING]** read from the sibling project PB Office (`/home/voneinem/Projekte/office-pocketbook`, same owner): its contract
  `specs/003-receive-shared-text/contracts/handoff-contract.md` (v1) and its `INSTALL.md`. A statement about what PB Office *does*; not observed by this
  project except where [DEVICE] says so.
- **[DEVICE]** observed on the InkPad One on 2026-10-23/24 with the hand-off probe of this project (`docs/device-findings.md`, section "Handing text to the
  real PB Office 0.4.0").
- **[UNVERIFIED]** assumed; has a device check in [Open items](#open-items).

---

## R-1 What the reader has been shown to do

**[DEVICE]** `OpenBook2("/mnt/ext1/applications/pboffice.app", 5, {"pbo-mail-v1", "text=…", "textlen=…", "subject=…", "to=…"}, 0)`:

- PB Office **not running**: the call returns 0, PB Office starts and opens a new message with the text, subject and recipient (observed twice). Its log has
  `handoff.received text_chars=105` and `handoff.drafted count=1`; its `handoff/` folder is empty afterwards; the marker appears in no log.
- PB Office **running** (0.4.0, no request listener): the call returns **1** (observed twice), PB Office comes to the front on its own screen, no draft is made.
- The caller gets EVT_BACKGROUND at the call; after leaving PB Office with Back the user sees PB Office's main screen, Home returns to the caller.
- The system's log lines about the call name only the path.

**Decision**: the supported case is the cold start; the return value is the only signal for the rest: 0 = accepted, anything else = not accepted.

## R-2 Where the call and the launcher path live

**Decision**: one new file `app/mail_handoff.cpp` holds the only `OpenBook2` call of the product and the only occurrence of the launcher path; its header
`app/mail_handoff.h` declares `int send_mail_handoff(const std::vector<std::string>& argv)` and does not name the other app. `main.cpp` injects it as
`ShareBridge::Config::hand_off`. The bridge, the core and all tests never see InkView or the path.

**Rationale**: constitution 1.1.0 allows exactly one hand-off and requires the new file to be named in R11; a single file is the smallest thing to name.
The existing `OpenBook()` in `app/device_bridge.cpp` opens a downloaded file with the viewer and is a different, older exception (N9).

**Alternatives**: the call inside `DeviceBridge` (rejected: it is QObject glue for orientation, Wi-Fi and the viewer; a hand-off there needs a QML-visible
method that could be called from anywhere); the call in `bridge/` behind `#ifdef` (rejected: mixes InkView into the testable layer).

## R-3 The argument list

**[SIBLING]** contract v1: `argv = {"pbo-mail-v1", "text=<text>", "textlen=<n>", "subject=<one line>", ["to=…"]}`. `textlen` is required when `text=` is
present and must equal the byte length of the text as received; unknown keys are ignored; a key given twice makes the hand-over malformed; the text is at
most 8192 bytes (provisional, never below 6000), the subject at most 300 characters after cleaning; invalid bytes and control characters (except line feed
and tab) in the text become U+FFFD.

**Decision**: PB Cloud sends exactly `pbo-mail-v1`, `text=<link>`, `textlen=<bytes>`, `subject=<subject>`; **no `to`**, no other key. A link that contains a
control character or is not valid UTF-8 is not handed over (the bridge treats it as unreadable already; the core refuses it a second time). A link longer than
8192 bytes is not handed over and the user is told (FR-009); in practice a link is at most 512 bytes (feature 004 bound), so this guard never fires on valid
input.

**Rationale**: the smallest set that the contract defines for a mail with a link; omitting `to` leaves the choice of the recipient to the user (spec).

## R-4 The password cannot reach the call

**Decision**: `build_mail_handoff(link, subject)` has no parameter for a password and the bridge never passes it; the bridge has no access to the password
after the request that created or changed the link (`SecretText` is consumed in the core, feature 004 R-9). `test_mail_link` creates a link with a password,
mails it and searches every argument for the password and for its prefix; the privacy cases of `test_mail_link` search everything the app writes.

**Rationale**: a rule that the type system keeps is stronger than one a test must keep; the test is the second line.

## R-5 The call runs synchronously on the UI thread

**[DEVICE]** The call returned within the same second in all probe runs.

**Decision**: `mailLink(true)` calls the injected function on the UI thread and returns the result. No worker, no timer (rule N3), no retry. A re-entrancy
guard (`mail_in_flight_`) and a flag set on acceptance make a second confirmation a no-op.

**Rationale**: a worker adds a state (in flight, result signal) and a second place where the link must be kept alive; a block of well under a second does
not justify it. If DT-M1 shows a visible block, the worker variant replaces the synchronous call without a change of the contract.

**Alternatives**: `QtConcurrent`/Worker (rejected for now, see above).

## R-6 Wiping

**Decision**: the arguments live in a move-only `MailHandoff` (core) that owns four `std::string`s and overwrites them in `wipe()` and in its destructor
(`secure_wipe`). The bridge builds the link bytes from `link_text_` into a `std::string`, passes it to the builder, overwrites that string and the
intermediate `QByteArray` in place right after the builder returns, and wipes the `MailHandoff` right after the call returned (accepted or not). The injected
function receives a const reference and keeps nothing. A mutable `char*` array for `OpenBook2` is built from the strings inside `app/mail_handoff.cpp` as
pointers into them (no copy).

**Limits stated honestly** (README): copies made by Qt (the `QString` of the link on the link screen, QML strings) are wiped in place by the existing
`clearSecrets()`; what the system and PB Office do with the text afterwards is theirs. No guarantee beyond "not persisted, not logged, buffers overwritten".

## R-7 The link is dropped by the app after an accepted call

**[DEVICE]** The caller receives EVT_BACKGROUND at the call; whether Qt maps it to `Qt::ApplicationInactive/Hidden` on the reader is the same unverified
question as for the sleep behaviour of feature 004 (DT-L5).

**Decision**: after the call returned 0 the bridge calls `leaveLink()` (drops the link and the clipboard copy if any), emits `mailHandedOver()` and the page
pops. The link is therefore gone whether or not the platform reports the switch. The list screen below stays as it is (feature 004 behaviour).

**Rationale**: spec FR-015/FR-016 want the link gone after the hand-off; making it independent of an unverified event is cheap.

## R-8 The failure notice and when it is shown

**Decision**: a non-zero answer sets `mailNotice` to a key (`mail_not_accepted`) - nothing else, no link, no item name - and, when the app is still active
(PB Office missing: the system did not switch), the link page shows the message at once as a banner like the other sharing messages. When the app is
switched away (PB Office running came to the front), `clearSecrets()` closes the sharing screens as designed, but **keeps `mailNotice`** (it holds no secret);
`Main.qml` shows it in a dialog with one OK button when the application is active again, and dismisses it with `dismissMailNotice()`. No retry; the link is
not re-shown (spec FR-011).

**Rationale**: the spec requires the message "the next time the screen is shown" and forbids re-showing the dropped link; a key that outlives the screens
is the smallest carrier. It is kept in memory only (a restart forgets it).

**Alternatives**: store the notice in settings (rejected: persistence for a hint); show nothing and rely on PB Office's screen (rejected: PB Office shows its
own screen without saying why nothing was made).

## R-9 The wording of the notice

**Decision**: title "PB Office could not take the link." and action "Close PB Office and try again. If PB Office is not installed, the link cannot be
mailed." (German in `qml/strings_de.tsv`). The text names no item and no link. One message covers both causes because PB Cloud does not inspect the reader's
applications (FR-020, clarified 2026-10-24) and the return value does not distinguish them (1 was observed for a running PB Office; the value for a missing
file is not known - **[UNVERIFIED]**, DT-M2).

## R-10 The tool on the link screen

**Decision**: a `ToolButton` "Mail link" with the existing `send` icon in the bottom panel of `ShareLinkPage`, visible when the link state is `ok` or
`not_secure` (a readable link on the connected host; the page already warns about `not_secure`), hidden for `unreadable` and `different_address` (FR-001),
disabled while a sharing request runs (`share.busy`, FR-005). It sits left of Remove link; Copy stays hidden on the reader (feature 004). Tapping it asks the
bridge `mailLink(false)`; an acceptable link answers `{needsConfirm, text}` and the page opens a `ConfirmDialog` whose first (default) button is Cancel and
whose second is "Mail link". Confirm calls `mailLink(true)`.

**Rationale**: the panel already holds Copy (device: hidden) and Remove link; three tools fit (feature 004's panel check DT-L1 covers up to five).

## R-11 Tests

**Decision** (failing first, in this order):

1. `test_mail_handoff` (unit): marker first; `text=` equals the link exactly; `textlen` equals the byte length (including multi-byte characters); `subject=`
   cleaned (control characters and line breaks become one space, runs collapsed, trimmed), at most 300 characters, cut at a character boundary (a 4-byte
   character at the limit is dropped whole, never split), empty item name gives a subject of the prefix alone; no `to=` key; text over 8192 bytes refused;
   control characters and invalid UTF-8 in the link refused; `wipe()` zeroes the buffers; the object is not copyable.
2. `test_mail_link` (bridge, fake `hand_off`): exactly one call after confirmation; none after Cancel (no `confirmed`); none for an unreadable or foreign-host
   link; none while a sharing request runs; a second confirmation after acceptance makes no second call; the arguments equal the link and contain no password
   (link created with a password); answer 0 -> `mailHandedOver`, link gone; answer 1 and answer from an empty `hand_off` -> notice `mail_not_accepted`, no second
   call; the notice survives `clearSecrets()` and is cleared by `dismissMailNotice()`; the log captured during all of it contains no link, subject, item name.
3. Privacy cases in `test_mail_link` (with `tests/support/privacy_probe.h`, as `test_share_privacy` does for the core): after a mail-link run with known link and password, a search of everything the app writes (database, settings, cache,
   exports, log) finds neither; the link occurs only in the arguments seen by the fake.
4. `test_no_background_network` (extended): confirming makes no request and does not ask for Wi-Fi, also with the reader offline (the feature works offline).
5. Script self-tests: N10 detects `OpenBook2` outside `app/mail_handoff.cpp`, the marker outside `core/src/mail_handoff.cpp`, a log call in the hand-off modules;
   R11 allows `app/mail_handoff.cpp` and still refuses another new file.
6. Walk 20 (host UI): link screen -> Mail link -> confirmation (Cancel default) -> Cancel (nothing) -> again -> confirm (stand-in answers 0: the link screen is gone) ->
   again with the stand-in set to 1: the notice dialog.

## R-12 Version and documents

**Decision**: a feature, so VERSION 0.5.0 and a `CHANGELOG.md` entry in the last phase. README gets the honest-limits sentence and the dependency on
PB Office 0.4.0 or later; INSTALL.md says that Mail link needs PB Office installed and not running; the About box component list is unchanged (no
component). The device checklist gets DT-M1..DT-M4.

---

## Open items

| ID | Item | Why open | Closes when |
|----|------|----------|-------------|
| O-1 | What `OpenBook2` returns when PB Office is not installed | only a running PB Office (1) and a started one (0) have been observed | DT-M2 |
| O-2 | Whether Qt reports the switch to PB Office as an inactive application state on the reader | same as DT-L5 of feature 004; the plan does not depend on it (R-7) | DT-L5 / DT-M1 |
| O-3 | Whether the arguments of the call stay visible in the process list of the system while PB Office restarts itself | the PB Office project's design says they are visible only briefly (its R11); not measured from PB Cloud | DT-M3 |
| O-4 | Whether a running PB Office can ever be supported | needs a listener in PB Office; its experiment ended without data | a later feature |
| O-5 | Length of the UI block by the call | observed < 1 s in the probe | DT-M1 timing |
