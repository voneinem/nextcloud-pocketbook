# Acceptance traceability: PB Cloud 0.4.0 (feature 004, share links)

The tests that prove each story **on the host against the fake Nextcloud** (`tests/support/fake_nextcloud_server.h`, shares side). "Walk" is
`tests/walks/18-share-links.walk` (create, show, change, remove) and `19-share-limits.walk` (the server's rules), both run by CTest `ui_walk`. A passing test
proves what this project assumes the server and the reader do; **nothing here is verified against a real Nextcloud server or on the InkPad One** (the LV-L and
DT-L items stay unverified until someone runs them and writes the date). This table maps stories to test files; it does not repeat each acceptance scenario of
[spec.md](spec.md). Scenarios that no test covers are listed at the end.

| Story | Evidence (host-verified) |
|-------|--------------------------|
| US7 Links and passwords stay private | `test_secret_text`, `test_share_privacy` (the probe searches the log and every file below the data folder; one case per flow), `test_share_bridge` (`clearSecrets`, in-place wipe of every shared copy, clipboard, application state, account change), `test_log`, rules N8 and N9 and U7 and U8 with their self-tests (`check_constitution`, `check_ui_style`), the `forceSecret` check of `app --self-test`, `test_share_form`, `test_share_requests` (password in the body only) |
| US2 See the existing links and shares | `test_share_parse` (the list type cannot hold a link), `test_share_list`, `test_share_requests` (L1-L3), `test_share_bridge` (list, paging, stale marks, unavailable entries, inherited rows), `test_budget_share`, walk 18 |
| US1 Create a public link | `test_share_create`, `test_share_requests` (L5 `POST`), `test_share_rules`, `test_share_date`, `test_share_diagnose`, `test_share_bridge` (confirmation, validation, one request, password overwritten), walk 18 (with the tap count of SC-001) |
| US3 Show the link so it can be typed or copied | `test_share_detail`, `test_share_bridge` (detail, Copy, Leave, link states), `test_share_privacy` (show case), `check_ui_style` U7, walk 18 |
| US4 Change a link | `test_share_change`, `test_share_requests` (L4 then `PUT`), `test_share_bridge` (confirmation, changed meanwhile), walk 18 |
| US5 Remove a link | `test_share_remove`, `test_share_requests` (L6), `test_share_bridge` (confirmation, already gone, refusal, unknown outcome), walk 18 |
| US6 Messages when the server limits sharing | `test_share_refusals`, `test_share_unknown_outcome`, `test_share_diagnose`, `test_failure_messages` (one message and one "what to do" per key, German included), `test_share_bridge` (message map, sharing off), walk 19 |

## Walks (host, with pictures)

`tests/walks/18-share-links.walk`: select Books in the folder listing, Share, the list (a link, a user, a group, another kind of share, the link of another
user, the notice that a second link is a second one), a user share opens a read-only summary; create a link with a password, an expiry date and the upload
permission (the confirmation says what it allows and defaults to Cancel; Cancel creates nothing); a link with the defaults (**SC-001: at most 8 taps**, 6
counted); the link screen (the whole link in large letters, tapping it does nothing, Copy); change the password, the expiry and the permission (each confirmed,
each stating its effect); remove the link (the confirmation names the item and defaults to Cancel; at most 4 taps, SC-008); the link of another user is shown
and cannot be changed; Back drops the link. `19-share-limits.walk`: the server forces a password and an expiry of at most 7 days and refuses upload: the form
marks both fields, shows the limit, disables Read and upload with the reason, and a tap on Create with nothing entered (and one with a date too far away) is
refused with the reason and nothing is created.

## Measured on the host (UNVERIFIED on the device)

`test_budget_share`: the parse of a 200-share list takes about 4 ms (budget 100 ms; 500 ms on the device), a 501st row is dropped, and 100 open/leave cycles
grow the process by about 200 KB (budget 1 MB). `test_share_curl` runs the whole flow (`GET`, `POST`, `PUT`, `DELETE` with form bodies) over the real libcurl
transport and TLS, checks that credentials never appear in a URL, that a redirect to another host is not followed and that an untrusted certificate stops every
request. `test_no_background_network` (extended) shows that sharing sends nothing at start, while idle, when Wi-Fi comes and goes or when a screen is left, and asks
the system to bring Wi-Fi up only after a tap (`sharing_asks_the_system_for_wifi_only_after_a_tap...`; `test_share_bridge` covers list, create and link with the hook), and is never started by an Auto Sync timer.

## Success criteria

| Criterion | Evidence |
|-----------|----------|
| SC-001 (<= 8 taps) | walk 18 `expect_taps_max 8` (6 counted: Select, the row, Share, Create link, Create, confirm) |
| SC-002 (120 characters complete) | `test_share_detail` (a 120-character link survives unchanged), `ShareLinkPage` wraps at any character (`check_ui_style` U7); legibility on the reader is DT-L5 (unverified) |
| SC-003 (nothing before the confirmation, one request after) | `test_share_requests`, `test_share_bridge` (create, change and remove without `confirmed` send nothing), walk 18 (Cancel creates nothing) |
| SC-004, SC-005 (no link, token or password anywhere; the password gone) | `test_share_privacy`, `test_share_bridge`, N8, N9, `test_share_create`/`test_share_change` (the password object is empty after every outcome); **RAM is best effort** (README) |
| SC-006 (distinct messages, entries kept) | `test_share_refusals`, `test_failure_messages`, walk 19 |
| SC-007 (200 shares in 5 s) | `test_budget_share` (parse and model time); the network time is DT-L4 (unverified) |
| SC-008 (remove in <= 4 taps from the selected item) | walk 18 `expect_taps_max 4`; the spec text was changed from 3 to 4 (Share, the link, Remove link, confirm) |
| SC-009 (no background network) | `test_no_background_network` |
| SC-010 (English and German, offline on every screen) | `scripts/extract-strings.py --check`, `test_failure_messages`, the offline lines of `SharePage` (`test_share_bridge`) |
| SC-011 (unverified labelled) | the LV-L and DT-L items; fixtures carry `.unverified` |

## Not covered by a test or not yet done

- Everything that needs a real server or the reader: LV-L1 to LV-L10 and DT-L1 to DT-L5. In particular the capability shape, the permission values of folder links,
  the member that carries the password state, what each enforcement answers, whether `shares/inherited` exists, and on the reader: the touch size of the Share
  tool in select mode, whether Copy can be pasted into another app (DT-L2 failed on 2026-10-23: `PBC_DEVICE_CLIPBOARD` is OFF), what a sleep or app switch does to the screen.
- "Sharing is not offered" has a bridge test and a page that reads it, but no walk (a walk cannot change the server's rules in the middle of a run).
- The password that the user types lives for a moment in QML/JavaScript strings, in `QString`s and in the transport's copy of the request body; the bridge
  overwrites the `QString`s in place and the core wipes its own copies, but a runtime copy cannot be reached. What the tests prove is that nothing is
  persisted, logged or shown (README, "Share links").
- The walk reaches Books as the second row of the root after walks 01-17; a later walk that adds a folder sorted before it would move the row.
