# Quickstart: validating share links

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **003**: [quickstart](../003-folder-sync/quickstart.md) | **002**: [quickstart](../002-manage-server-files/quickstart.md) | **001**: [quickstart](../001-nextcloud-files-client/quickstart.md)

How to prove that the feature works, from the cheapest check to the real device. Every real-server and device result is
**unverified** until written into `docs/live-server-checklist.md` (LV-L*) or `docs/device-test-checklist.md` (DT-L*) with a date.
Contracts: [share requests](contracts/share-requests.md), [core](contracts/core-interfaces.md), [bridge and QML](contracts/bridge-qml.md);
data: [data-model.md](data-model.md); decisions: [research.md](research.md).

## 1. Host build and the automatic gate

Same prerequisites as 001-003.

```bash
cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
ctest --test-dir build-host --output-on-failure
scripts/pre-merge.sh            # version bump (0.4.0), build, all tests, secret scan, constitution/fixture/UI-style checks
```

Expected: every test passes, including `check_constitution` with the changed rule N2 (allow-list gets `share_api.cpp`), the new
N8 (no secret-capable logging in `share_*`) and N9 (nothing opens a link) and their self-tests, `check_fixtures` (the new
`tests/fixtures/nextcloud/shares/*.unverified`), `check_fake_markers`, `check_ui_style` (link display is plain text and has no
mouse handler), `check_translations`. A second build directory with `-DPBC_SANITIZE=ON`: the parser, hostile-input and privacy tests
must pass under it.

## 2. Contract and privacy tests (no server, no device)

```bash
ctest --test-dir build-host -L unit          # rules, dates, parsers, diagnosis, SecretText, error keys
ctest --test-dir build-host -L integration   # the fake Nextcloud with a shares side
ctest -R 'share_' --test-dir build-host --output-on-failure
```

| Check | Expected |
|-------|----------|
| `test_share_rules` (capability shapes of several versions, missing and contradictory keys) | unknown stays unknown; unknown is never "required" and never a limit |
| `test_share_date` | only `YYYY-MM-DD`; impossible dates, a time part, blanks refused; today allowed; today+days allowed; today+days+1 refused |
| `test_share_parse` (hostile: huge, truncated, wrong types, duplicate keys, control characters, 600 rows, unknown share types, a link on another host or `http://`, a hash in `share_with`) | total and bounded; never a crash; **a list result contains no link text even when every row has one**; kinds, permissions, `owned_by_me`, `has_password` right |
| `test_share_diagnose` | every rule violation gives its own key and marks the field; an unrecognized refusal gives `share_refused` with `E<status>` and no server text |
| `test_share_requests` | the exact method, URL, headers and form body of L1-L6; `POST` for create, `PUT` with one field for change, `DELETE` for remove; a password only in a body and never in a URL; credentials never in a URL; **no request is made before a confirmation** (SC-003) |
| `test_share_list` | file and folder lists, user/group/other rows read-only, inherited rows marked, 404 on L3 is silent, truncation at 500 with the message, offline sends nothing |
| `test_share_create` | defaults (read only, no password, no expiry); a folder with password, expiry and upload; a file never gets upload; the created link is shown from the server's answer only |
| `test_share_change` | set/replace/remove password, set/remove expiry, change permission; "changed meanwhile" sends nothing; the state shown is the server's |
| `test_share_remove` | no request before the confirmation, one after; the file is untouched on the fake server; "already gone" reads as already removed |
| `test_share_refusals` (fake switches: public links off, password enforced, expiry enforced with 7 days, upload off, item not shareable, sharing off, unknown refusal) | a distinct key and message for each; forced fields marked; the user's entries are still there (SC-006) |
| `test_share_unknown_outcome` (answer lost after the request went out, per request kind) | `share_unknown_outcome`, no automatic repeat, the stale note until Refresh, a repeated Create makes a second link (documented) |
| `test_share_privacy` | with a known password, token and link, after create/show/change/remove, a search of the log capture, the data directory, the database dump and the clipboard setter finds the password **nowhere**, the link/token **only** where the link page was open and after Copy, and the fake saw **no request to the link's address** (SC-004, SC-005) |
| `test_share_curl` | `POST`, `PUT` and `DELETE` with form bodies through the real libcurl transport and the local TLS server |
| `test_no_background_network` (extended) | nothing from sharing at start, idle, Wi-Fi changes, or leaving a screen; requests happen only after a tap (SC-009) |
| `test_share_bridge`, `app --self-test`, walk 18 | the property contract, `rows` have no link role, the password field is `forceSecret`, the link display has no mouse handler, texts exist in English and German (SC-010) |
| `test_budget_share` | prints parse + model time for 200 shares (<= 100 ms host) and peak RSS growth (<= 1 MB). Marked **unverified until measured on the device** |

## 3. Try the app on the host with the fake server

```bash
scripts/run-host-demo.sh            # fake server (HTTPS on localhost, own CA) and the app
```

The demo server's console gets the commands `share-rules <public|password|expiry N|upload|off>` (turn a rule on or off),
`share-list <path>`, `lose-next-answer` (as in 002) and `unshare <id>` (remove a link behind the app's back). Walk through:

1. **US1** Select a file in the folder listing (**Select**, tap the row) -> **Share** -> **Create link**: leave the defaults -> confirmation text -> **Create**. The link appears
   in large letters; the console shows one create request.
2. **US2** back on the Share list: one public link; add a user share and a group share through the console: they appear without a
   change control.
3. **US3** open the link: the whole text is shown; tap it: nothing happens; **Copy** is shown on the host and puts the text on the
   clipboard; Back: the clipboard is cleared when it still held the link.
4. **US4** **Password** -> type one -> confirm; **Expiry** -> `2026-12-31`; on a folder switch to **Read and upload**; with
   `share-rules password` on, try **Remove password**: refused with the reason.
5. **US5** **Remove link** -> the confirmation defaults to Cancel -> confirm; the list is empty; the file is still on the server.
6. **US6** run the console rules one by one and watch each message and the marked fields.
7. **US7** open the app's data folder (the demo prints its path): no password, link or token in any file; the log capture file
   shows only `share.*` lines with status and counts.

Scripted version: `scripts/ui-walk.sh --out <dir> tests/walks/18-share-links.walk`.

## 4. Real Nextcloud server (unverified until done)

Use a test account on a server you control, not the real data of the user. Work through `docs/live-server-checklist.md` LV-L1 ...
LV-L10 and write the date, server version and result into each row. Particularly: the `files_sharing` capability shape (LV-L1),
the permission values for folder links (LV-L2), where the password state appears (LV-L3), enforcement answers (LV-L6) and clearing
with empty values (LV-L7).

## 5. The reader (unverified until done)

Package with `scripts/package-device.sh` (SDK only from `POCKETBOOK_SDK`), install, and work through DT-L1 ... DT-L5 in
`docs/device-test-checklist.md`: the Share tool fits the select-mode panel of the folder page and the details panel of a file (DT-L1), whether text copied in the app can be pasted into
another app (DT-L2, decides `PBC_DEVICE_CLIPBOARD`), what happens to the screen state on sleep and app switch (DT-L3), the time to the
first page of a 200-share list and peak memory (DT-L4), and legibility of a 120-character link at reading distance (DT-L5).
