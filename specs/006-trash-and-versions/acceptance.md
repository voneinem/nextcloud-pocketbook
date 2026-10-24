# Acceptance traceability: PB Cloud 0.6.0 (feature 006, trash and file versions)

Baseline before the feature (T001, 2026-10-24): `ctest` on the 0.5.0 tree was green, 179 tests (schema 5).

The tests that prove each story **on the host against the in-process fake Nextcloud** (`tests/support/fake_nextcloud_server.h`, which now has a trash side and a versions side).
A passing test proves that the app follows the contract in `contracts/trash-requests.md`; **nothing here is verified against a real Nextcloud server or on the InkPad One**:
LV-T1 .. LV-T10 in `docs/live-server-checklist.md` and DT-T1 .. DT-T4 in `docs/device-test-checklist.md` stay unverified until someone runs them and writes the date. Scenarios that
no test covers are listed at the end.

| Story | Evidence (host-verified) |
|-------|--------------------------|
| US1 See what was deleted | `test_trash_parse` (items, newest first, unreadable names, unsafe or overlong locations, impossible dates and sizes, availability, a failed member, the cut at 1,000, the 4 MiB limit, mutated input), `test_trash_list` (against the fake: fields, empty trash, 1,000 and 1,200 items, hostile fixture, failures with their codes, the stored list), `test_store_trash` and `test_store_migration_6` (schema 6, replace in one transaction, per account, bounded), `test_trash_bridge` (rows, paging, one chosen item, unreadable items cannot be chosen, offline stored list with its age, a lost connection keeps the list in view, truncation), walk 21 |
| US2 Restore a deleted file or folder | `test_trash_rules` (the plan table of R-5, target checks, confirmation content, refusal keys), `test_trash_restore` (free place, folder with content, taken name, missing folder never created, unknown place, late collision, a server that renames), `test_trash_restore_chosen` (free name, taken name never replaced, missing folder, a server that cannot restore to a chosen place, a 403 that did restore), `test_trash_refusals`, `test_trash_unknown_outcome`, `test_trash_requests` (every MOVE carries `Overwrite: F`), `test_folder_chooser_bridge` (server folders only), `test_trash_bridge` (the flow, the cache of the folder, the local copy that was "removed on the server", nothing queued), walk 21 (four taps, the chooser, the result) |
| US3 Versions of a file | `test_version_parse`, `test_versions_list`, `test_versions_restore` (the previous content kept as a version, a server that does not keep it is reported, nothing deleted), `test_versions_stale` (a changed file is refused before anything is sent), `test_versions_bridge`, walk 22 |
| US4 Delete for good, only knowingly | `test_trash_rules` (confirmation content of a file, a folder and Empty trash, the empty set, the loop rules), `test_trash_delete`, `test_trash_empty` (exactly the named items, one request at a time, never the collection, an item arriving later stays, locked / 401 / 429 / lost answer / cancel / 1,000 items), `test_trash_bridge` (the confirmation chain, every way it is dropped, progress, counts), `check-constitution.sh` N12, `check-ui-style.sh` U9, walk 22 |
| US5 Why trash or versions are not available | `test_trash_availability` (the table of R-2), `test_trash_disabled`, `test_trash_bridge` and `test_versions_bridge` (explain states, an unclear answer is never "off", no action offered), walk 23 |

## Success criteria

| ID | Evidence |
|----|----------|
| SC-001 | walk 21 counts four taps from the folder listing to a confirmed restore (`expect_taps_max 4`); the time of the user is DT-T2 (unverified) |
| SC-002 | `test_trash_restore`, `test_trash_restore_chosen`, `test_trash_requests`: no request without `Overwrite: F`, no existing entry changed in any flow |
| SC-003 | `test_trash_bridge` (no `DELETE` before the last confirmation, none after Cancel, leaving, refresh, account change or app switch, only the named set), `test_trash_empty` |
| SC-004 | `test_versions_restore`, `test_versions_bridge`: the previous content is a version afterwards, no `DELETE` under `versions` |
| SC-005 | `test_trash_disabled`, `test_trash_availability`, walk 23 |
| SC-006 | `test_trash_refusals`, `test_trash_unknown_outcome`, `test_trash_privacy`, `test_trash_delete`, `test_trash_empty` |
| SC-007 | `test_budget_trash` (host: 1,000 items parsed, stored and loaded in about 60 ms; 200 versions in about 5 ms; no growth over 100 reads); the device numbers are DT-T3 (unverified) |
| SC-008 | every server and device behavior is labelled unverified: LV-T1 .. LV-T10, DT-T1 .. DT-T4, fixtures with `.unverified` |

## Not covered by a host test (stays manual or unverified)

- Everything a real server decides: the property names and the shape of the original location (LV-T2), the restore verbs and a restore to a chosen place (LV-T3 .. LV-T5), the
  server keeping the current content as a version (LV-T8), the capability keys (LV-T1, LV-T10). If LV-T5 fails, the "chosen place" is delivered as the message that the server can
  restore only to the original place (plan, owner point 2).
- The panels at touch size with the new tools (DT-T1), the readability of the two-dialog flows on e-ink (DT-T2), the time and memory of a large trash (DT-T3), Empty trash with the
  screen and Wi-Fi switched off (DT-T4).
- The wording of a restore that "landed elsewhere" is judged by size and kind (the app cannot know the name the server chose): LV-T3 records what a real server does.

## Deviations from the written design (recorded while building)

- Fixtures are named `x.unverified.xml` (the project convention), not `x.xml.unverified`.
- Log codes are added to `core/src/log_codes.h` only when used (`test_log` fails on a code that is listed and unused).
- `refresh_first` keeps its older error class Unsupported; `FolderChooserBridge` got the property `serverOnly`; `Store::forget_folder_after_change` is new (the cache of the folder a
  restore went to is dropped, the versions above are forgotten): the "removed on the server" mark of a local copy is a stored flag, so the bridge clears it for a restore to the
  original place (research R-14 said it was derived from the listing).
- The walk language is generic (`tap`, `waitfor`, `expect`), so no walk verbs were added; the console of the fake server got `trash-add`, `trash-list`, `trash-disable`,
  `trash-enable`, `versions-disable`, `versions-enable`, `add-file`, `versions-add`, `versions-list` and `restore-to-files`. Walk 23 (both apps off) was added next to walks 21 and 22.
- `TrashApi::empty_trash` holds the loop of Empty trash (the bridge calls it); rule N12 covers `delete_item` and `empty_trash`. `OperationResult` got `removed_trash_names`,
  `unknown_trash_name` and `previous_not_kept`; `VersionList` and `TrashList` carry the short `code` of a failed fetch; `TrashBridge` got `canAct`, `progress`, `result` and the
  slots `onApplicationStateChanged` and `onConnectionChanged`; a second `confirmStep()` while the run goes on answers `nothing_pending` (the chain is used up).
- U9 and N12 were written together with their self-tests in one step (T055/T059), not red first.

## Gates (2026-10-24)

`scripts/pre-merge.sh` passed on the host: 203 tests including `ui_walk` (walks 01-23), the secret scan, the constitution check with N2, N11 and N12 and their self-tests,
`check-ui-style.sh` with U9, the fixture check, the translation check and the version check (0.6.0). The sanitizer build (`build-asan`) passes every test of the feature
(`test_trash_parse`, `test_version_parse`, `test_store_trash`, `test_store_migration_6`, the restore, delete, empty, versions, privacy, curl and budget tests, `test_trash_bridge`,
`test_versions_bridge`, `test_no_background_network`). The device package `pbcloud-inkpad-one-0.6.0.zip` was built with `scripts/package-device.sh` (4.4 MB, limit 25 MB); no
device item (DT-T1 .. DT-T4) has been run.
