# Quickstart: validating Mail Link

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **Contracts**: [handoff-call.md](contracts/handoff-call.md), [bridge-qml.md](contracts/bridge-qml.md)

Status of everything below: host steps are checked by tests; reader steps are **unverified** until run (constitution IX).

## 1. Host: unit and bridge tests (no reader, no server)

```bash
cmake -S . -B build-host -G Ninja && cmake --build build-host
ctest --test-dir build-host -R 'mail_handoff|mail_link|share_privacy|no_background_network'
```

Expected: all pass. They prove: one call after confirmation, none after Cancel, the arguments equal `{marker, text=<link>, textlen, subject}`, the password is
not in them, a non-zero answer gives the notice and no second call, nothing in the log.

## 2. Host: scripts

```bash
bash scripts/check-constitution.sh && bash scripts/check-constitution.sh --self-test
```

Expected: N10 and R11 pass and their self-tests detect a violation (a second `OpenBook2` site, the marker elsewhere, a log call in the hand-off modules).

## 3. Host: the walk with the stand-in

```bash
scripts/ui-walk.sh --out /tmp/walk tests/walks/20-mail-link.walk
```

Expected: the confirmation shows with Cancel first; Cancel changes nothing; confirming with the stand-in answering 0 leaves the link screen; with the
stand-in answering 1 the notice dialog appears and the link is not shown again. Screenshots are written to the given folder.

## 4. Reader (InkPad One) - DT-M1 .. DT-M4

Prerequisites: PB Office 0.4.0 or later installed and **not running**; PB Cloud 0.5.0 installed; Wi-Fi on; a file on the server that can be shared.

| ID | Steps | Expected |
|----|-------|----------|
| DT-M1 | Share a file, open the link, tap Mail link, confirm | PB Office opens a new message with the link as its text and the subject "Link to <name>"; two taps; time from the confirm tap to the open editor is at most 10 s (note the seconds, SC-001); the draft can be deleted afterwards |
| DT-M2 | Start PB Office and leave it open, switch to PB Cloud, repeat; then remove PB Office's launcher (or use a reader without it) and repeat | the notice "PB Office could not take the link" the next time PB Cloud is shown; no draft; no second try; record the returned value for the missing case (open item O-1) |
| DT-M3 | After DT-M1 and DT-M2 mount the reader and search `system/pbcloud/` (logs, database, probe files) for the link text and the item name | nothing found |
| DT-M4 | After DT-M1 press Back in PB Office, then Home | PB Office's main screen, then PB Cloud with its list (the link screen is gone); record whether the link screen was already gone when PB Cloud came back |

Record the result with the date next to the items in `docs/device-test-checklist.md`.
