# Quickstart: validating trash and versions

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **005**: [quickstart](../005-mail-link/quickstart.md) | **004**: [quickstart](../004-share-links/quickstart.md) | **002**: [quickstart](../002-manage-server-files/quickstart.md) | **001**: [quickstart](../001-nextcloud-files-client/quickstart.md)

How to prove that the feature works, from the cheapest check to the real device. Every real-server and device result is
**unverified** until written into `docs/live-server-checklist.md` (LV-T*) or `docs/device-test-checklist.md` (DT-T*) with a date.
Contracts: [requests](contracts/trash-requests.md), [core](contracts/core-interfaces.md), [bridge and QML](contracts/bridge-qml.md);
data: [data-model.md](data-model.md); decisions: [research.md](research.md).

## 1. Host build and the automatic gate

Same prerequisites as 001-005.

```bash
cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
ctest --test-dir build-host --output-on-failure
scripts/pre-merge.sh            # version bump (0.6.0), build, all tests, secret scan, constitution/fixture/UI-style checks
```

Expected: every test passes, including `check_constitution` with the changed rule N2 (allow-list gets `trash_api.cpp`) and the new
N11 (endpoints named only in two files) and N12 (the delete call only in `TrashBridge`) with their self-tests; `check_fixtures` (the new
`tests/fixtures/nextcloud/trash/*.unverified`); `check_ui_style` (destructive dialogs default to Cancel); `check_translations`. A second
build with `-DPBC_SANITIZE=ON`: the parser, hostile-input and privacy tests pass under it.

## 2. Contract and unit tests (no server, no device)

```bash
ctest --test-dir build-host -L unit
ctest --test-dir build-host -L integration
ctest -R 'trash_|versions_|store_trash|migration_6' --test-dir build-host --output-on-failure
```

| Check | Expected |
|-------|----------|
| `test_trash_availability` | the table of research R-2, every row: key present/missing/false, no `files` object, failed request -> On/Off/Unknown; the two apps judged independently |
| `test_trash_parse` (hostile: huge, truncated, wrong namespaces, duplicates, names with `..` `/` control characters and 300 bytes, a 403 member, locations with `..`/empty segments/2 KB, deletion times negative/huge/missing) | total and bounded; never a crash; an unusable item is listed as unreadable and is neither restorable nor deletable; the sort is newest first |
| `test_version_parse` | same shape; ids not digits are skipped; a missing date is "unknown", a future date is flagged; the collection itself is not a row |
| `test_trash_rules` | every row of the restore planner; the confirmation content of each kind and stage; the empty set is exactly the actionable items; a truncated list is announced |
| `test_store_trash`, `test_store_migration_6` | replace is one transaction (a kill leaves the old or the new list); schema 5 -> 6 keeps every other table; an older build refuses schema 6 |

## 3. Against the fake Nextcloud (the in-process server)

| Test | Expected |
|------|----------|
| `test_trash_list` | one row per item with original location, date, size; newest first; paged; an empty trash says so; offline shows the stored list with its age and disables server actions |
| `test_trash_restore` | free original place: the item is back, leaves the list, the folder cache above is cleared, no file on the reader changed; name taken and folder missing: nothing sent but probes, the choose-another-folder flow starts; a late collision (the name appears between probe and restore) leaves the existing entry untouched and reports plainly |
| `test_trash_restore_chosen` | chosen folder, free name: restored there with `Overwrite: F`; name taken there: a new name is required and no replace is possible; a server answering "unsupported" gives `restore_place_unsupported` and changes nothing |
| `test_trash_delete` | one confirmation for a file, two for a folder; Cancel at either deletes nothing; leaving the screen, Refresh or an account change discards the confirmation; exactly the named item is removed |
| `test_trash_empty` | exactly the named set is removed one request at a time; an item added mid-way stays; a locked item is counted and the loop continues; a 401 stops the loop; a lost answer stops it and is reported unknown; Cancel stops between two requests; the summary counts removed / not removed / not tried |
| `test_trash_refusals`, `test_trash_unknown_outcome` | each refusal gives its plain key and a short code; nothing is repeated by the app; "outcome unknown" is kept until refresh |
| `test_versions_list`, `test_versions_restore`, `test_versions_stale` | date and size, newest first; restore makes the chosen content current and the previous content a version; no version disappears; a file changed after the list is refused with nothing sent; a folder has no Versions tool |
| `test_trash_disabled` | trash off: the plain explanation, no action; versions off: its explanation; one on and one off: each judged on its own; an unclear answer: the generic message with a code, never "disabled" |
| `test_trash_privacy` | a search of the log, the database (outside the stored trash table), settings and cache finds no name or location from the trash |
| `test_trash_curl` | the real libcurl against the TLS test server carries `MOVE` with `Destination` and `Overwrite: F` and `DELETE`, and refuses another host |
| `test_no_background_network` | no trash or versions request at start, on the Home page, on the folder page, on resume or by timer |
| `test_budget_trash` | 1,000 rows and 200 versions within the host budgets of the plan (device columns stay unverified) |

## 4. Host demo (console and UI walks)

```bash
scripts/run-host-demo.sh                       # the fake Nextcloud with a console
# console: trash-add <path>, trash-list, trash-disable, versions-disable, lose-next-answer
ctest -R 'ui_walk' --test-dir build-host        # walks 21 (trash and restore) and 22 (versions and delete for good)
```

Walk 21: open the trash from the folder page, choose an item, restore to the original place (4 taps), then one whose name is taken (choose
another folder). Walk 22: list the versions of a file and restore one; Empty trash with both dialogs; Cancel at the second. Screens are
checked by the app's self-test and the walk's screen dump; no screenshot is stored.

## 5. A real server (unverified until run)

Use a throw-away account and an **app password**. Work through LV-T1..LV-T10 in `docs/live-server-checklist.md`: capabilities of both apps;
the trash properties and the original-location shape; restore to the original place and a late collision; restore to a chosen place and the
`Overwrite: F` answer; Empty trash by single deletes on 50 items; the version list; the version restore keeping the current content as a
version; a stale file during a version restore; a server with each app disabled. Write the date and result per row; turn a seen exchange into
an anonymized fixture and drop its `.unverified` suffix only then.

## 6. The reader (unverified until run)

Package with `scripts/package-device.sh` (SDK path from `POCKETBOOK_SDK` only), copy to the reader, then DT-T1..DT-T4 in
`docs/device-test-checklist.md`: the Trash and Versions tools fit at touch size; the two-dialog flow is readable on e-ink and Cancel is the
default; a 1,000-item trash pages without delay; Empty trash survives screen-off with coarse progress. Measure `VmHWM` and the package size
against the plan's budgets; record the numbers.
