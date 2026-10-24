# Acceptance traceability: PB Cloud 0.5.0 (feature 005, mail link)

The tests that prove each story **on the host with a stand-in for the mail app** (`ShareBridge::Config::hand_off` is a fake in the tests; the walk uses the host
stand-in of `app/mail_handoff.cpp`, whose answer the walk sets with `handoff <n>`). A passing test proves what this project assumes PB Office and the reader do;
**nothing here is verified with PB Cloud's own tool on the InkPad One** (DT-M1 .. DT-M4 stay unverified until someone runs them and writes the date). What the
reader did with the hand-off *probe* (cold start accepted, running PB Office returns 1) is recorded in `docs/device-findings.md`. Scenarios that no test covers
are listed at the end.

| Story | Evidence (host-verified) |
|-------|--------------------------|
| US1 Send a link by mail from the link screen | `test_mail_link` (the confirmation text with and without a password; exactly one call with `{pbo-mail-v1, text=, textlen=, subject=}` and no `to=`; Cancel and leaving make no call; the link is gone after an accepted call and a second confirmation calls nothing; a hand-off started from inside the hand-off is `busy`; unreadable and foreign links are not mailed, a `not_secure` link is; no link, a running request; a long item name), `test_mail_handoff` (the argument list, lengths in bytes, limits, subject cleaning at a character boundary), walk 20 (part one: confirmation with Cancel first, Cancel changes nothing, confirm leaves the link screen in two taps, SC-001 tap budget) |
| US2 A hand-off that fails is explained, not repeated | `test_mail_link` (answers 1 and 127 and a missing function give `mail_not_accepted` and one call; the notice survives `clearSecrets()` and is dismissed on request; the notice and messages hold no link, subject or name; a link over 8192 bytes is not handed over), walk 20 (part two: the notice on the link screen, Back drops it) |
| US3 The link and password stay private | `test_mail_link` (the password is in no argument, not even its beginning; after an accepted and after a refused hand-off the link is in no log and no file below the data folder and the item name is in no log; no request, no Wi-Fi request, offline too), `test_no_background_network` (a confirmed hand-off makes no request and asks for no Wi-Fi), `test_mail_handoff` (`wipe()` overwrites the buffers), `scripts/check-constitution.sh` N10 and R11 with self-tests |

## Success criteria

| ID | Evidence |
|----|----------|
| SC-001 | walk 20 counts two taps from the link screen to the hand-off (`expect_taps_max 2`); the seconds to the editor are DT-M1 (unverified) |
| SC-002 | `test_mail_link`: one call after a confirmation; none after Cancel, leaving, a second confirmation or a retry by itself |
| SC-003 | `test_mail_link` privacy cases |
| SC-004 | `test_mail_link` (refusal), walk 20 |
| SC-005 | `scripts/check-constitution.sh` and its self-test, the extended `scripts/pre-merge.sh` |
| SC-006 | DT-M3 on the reader (unverified); the host equivalent is the privacy cases of `test_mail_link` |

## Not covered by a host test (stays manual or unverified)

- DT-M1: that PB Office really opens the draft when PB Cloud's own tool calls it (the probe did, twice, 2026-10-23/24), and the time to the editor.
- DT-M2: the value `OpenBook2` returns when PB Office is not installed, and the notice after the app was switched away (the dialog of `qml/Main.qml` opens on the
  application state change, which a host run cannot produce; the QML loads in `app --self-test` and the walk shows the same text on the link screen).
- DT-M3 and DT-M4: logs and files on the reader; what Back and Home show after the hand-off.
- The wiping of the buffers inside `ShareBridge::mailLink` cannot be observed from a test (the strings are destroyed); it is guaranteed by one scope object whose
  destructor wipes on every way out, and by the unit test of `MailHandoff::wipe()`.
