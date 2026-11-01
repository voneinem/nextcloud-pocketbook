# Bug Fix: Open says why it cannot open a file (no ending in the name)

- **Slug**: open-synced-file-silent
- **Fixed**: 2026-11-01
- **Assessment**: ./assessment.md
- **Status**: applied (host-verified only; the device check DT-O1 to DT-O3 is open)

## Summary

A downloaded file whose name has no ending (such as `Baguette magique`) is no longer handed to the reader, which picks the viewer by the ending. Open now shows a dialog with the full file name and says the extension is missing. Any other failed hand-off (no copy, unsafe path, reader refused) is now reported and logged instead of ignored. No extension is added to any name (reporter's decision).

## Changes

| File | Change | Notes |
|------|--------|-------|
| `core/include/pbc/local_name.h`, `core/src/local_name.cpp` | added `has_file_extension(name)` | reuses `split_extension`; an ending is 1-15 bytes after the last dot (not the first character) with no blank |
| `bridge/include/transfer_bridge.h`, `bridge/src/transfer_bridge.cpp` | `open()` returns `{ok, reason, name}` instead of `void` | reason `no_extension` (not handed over) or `cannot_open`; name through `display_safe`; logs `open.handoff result=<reason>`, no path |
| `core/src/log_codes.h` | added `open.handoff` | `test_log` requires every logged code to be listed |
| `qml/pages/DetailsPage.qml` | Open calls `page.openFile()`, new `openFail` dialog | same pattern as `shareFail`; the name sits on its own line below the text |
| `qml/strings_de.tsv` | two German strings | `scripts/extract-strings.py --check`: 816 strings, 816 translations, 0 problems |
| `tests/unit/test_local_name.cpp` | added test | see below |
| `tests/bridge/test_transfer_bridge.cpp` | updated one test, added one | `open_ok` switch for the faked hand-off |
| `docs/device-test-checklist.md` | new section DT-O1 to DT-O3 | all `U` (not run) |

## Diff Highlights (optional)

```cpp
const std::string file_name = abs.value.substr(abs.value.rfind('/') + 1);
if (!pbc::has_file_extension(file_name)) return failed("no_extension", file_name);   // the reader picks the viewer by the ending
if (!c_.open_file(q(abs.value))) return failed("cannot_open", file_name);
```

## Tests Added or Updated

- `tests/unit/test_local_name.cpp::a_name_has_an_extension_when_a_short_ending_without_blanks_follows_the_last_dot` — `story.epub`, `archive.tar.gz` have one; `Baguette magique`, `.hidden`, `Pie. Part two` and `""` do not.
- `tests/bridge/test_transfer_bridge.cpp::open_says_why_it_did_not_hand_the_file_over` — an extension-less file is not handed over and the answer is `no_extension` with the full name; a refusal by the hand-off is `cannot_open` with the name.
- `tests/bridge/test_transfer_bridge.cpp::open_hands_only_a_regular_file_inside_the_download_folder_to_the_reader` — updated for the new return value; the unknown path now answers `cannot_open`.

## Local Verification

- `cmake --build build-host` → builds.
- `ctest --test-dir build-host` (full) → 204 of 205 passed; the failure was `test_log` ("code not listed in log_codes.h: open.handoff"), caused by this change and fixed by listing the code.
- After the fix, `ctest -R 'test_log|test_transfer_bridge|test_local_name|ui_walk|strings|style'` → 7 of 7 passed, including the scripted UI walk. The rest of the suite was not re-run after the one-line `log_codes.h` change; it passed in the full run before it.
- Not verified: the dialog on the e-ink panel and the viewer opening on the reader (DT-O1 to DT-O3). No ui-walk step taps Open on an extension-less file, so the dialog's look is unchecked.

## Deviations from Assessment

- The assessment listed `docs/device-test-checklist.md` loosely ("next to the DT-M entries"); a new section DT-O was added instead, because DT-M belongs to feature 005.
- `core/src/log_codes.h` was not in the list of files; it is required by `test_log` for the new event.
- `VERSION` and `CHANGELOG.md` were not changed; see Follow-ups.
- The open question about the sync-pair folder mismatch was answered with the recommendation (track separately); nothing was changed for it.

## Follow-ups

- Bump the version and add a CHANGELOG entry (`scripts/check-version-bump.sh` expects it on a branch that changes the product; the work is on `main`, uncommitted). Suggested: 0.6.2.
- Track the latent bug separately: `DeviceBridge::openFile` (`app/device_bridge.cpp:135`) accepts only paths under `/mnt/ext1/PBCloud`, but `TransferBridge::open` also passes sync-pair folder paths. A pair whose reader folder is outside `PBCloud` is now refused with "The reader could not open this file:" instead of silently.
- Run DT-O1 to DT-O3 on the reader and look at the dialog with a long, wrapped name.
- The second extension-less file on the server (`Saftiger veganer Schoko-Kirschkuchen vom Blech`) needs renaming to end in `.pdf` (Rename exists since feature 002).
