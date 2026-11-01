# Bug Verification: Open says why it cannot open a file (no ending in the name)

- **Slug**: open-synced-file-silent
- **Tested**: 2026-11-01
- **Assessment**: ./assessment.md
- **Fix**: ./fix.md
- **Result**: verified (reader check added 2026-11-01)

## Summary

Update 2026-11-01: the new build was installed on the reader and the owner confirmed that it works; the log shows the new event. The text below is the host result from before that check; the reader row in the table and the residual risks are updated.

On the host the fix holds: Open on a downloaded file without an ending shows the dialog with the full name and the missing extension instead of doing nothing, a refused hand-off is reported, and the whole suite (205 tests) passes. It was `partial` at first because the reader had not been tried; that check is now done.

## Checks Performed

| Check | Command / Action | Result | Notes |
|-------|------------------|--------|-------|
| Reproduction (post-fix), host | A scratch walk (not added to the repo): the fake server gets `/Books/Baguette` (no ending, type `application/octet-stream`), the app connects, downloads it and taps Open (`scripts/ui-walk.sh --out <scratch> <scratch walk>`) | pass | `waitfor dialogText the extension is missing` and `expect dialogText Baguette` held; the picture shows the dialog "No app on the reader can open this file. Its name has no ending (the extension is missing):" with `Baguette` below and an OK button; the viewer was not started |
| Failure path, host | The same walk with `Emma.epub` (row 2 by mistake) | pass | The host has no viewer, so the hand-off returned `false` and the dialog "The reader could not open this file:" with `Emma.epub` appeared, as designed |
| Reproduction (post-fix), reader | New build installed on the reader (`applications/pbcloud.bin` replaced); owner tapped Open on the extension-less file, then checked a `.pdf` file | pass | Owner: "worked". `pbcloud.log` line `2026-11-01T11:15:22 open.handoff result=no_extension`; no file name, folder or path anywhere in the log (0 hits). DT-O1 to DT-O3 are recorded in `docs/device-test-checklist.md` |
| New / updated tests | `ctest --test-dir build-host -R 'test_log\|test_transfer_bridge\|test_local_name'` (earlier) and the full run below | pass | Includes `open_says_why_it_did_not_hand_the_file_over` and `a_name_has_an_extension_when_a_short_ending_without_blanks_follows_the_last_dot` |
| Regression suite | `ctest --test-dir build-host -j4` | pass | 205 of 205, including `test_log` (now lists `open.handoff`), `ui_walk` (270 s) and the UI-style check |
| Strings | `scripts/extract-strings.py --check` (run when the strings were added) | pass | 816 strings, 816 translations, 0 problems; not re-run after, no string changed since |
| Lint / type-check | the project's own checks are CTest entries (UI style, strings, version) | pass | covered by the full run; no separate linter configured |

## Output Excerpts

```
[walk] ... (no FAILED lines)           # the extension-less file: dialog text and name expected and found
100% tests passed, 0 tests failed out of 205
Total Test time (real) = 270.22 sec
```

Earlier run of the same walk with `Emma.epub`: `expect dialogText Baguette -> text is: The reader could not open this file: Emma.epub` (the walk was wrong about the row, the app behaved as designed).

## Residual Risks

- The dialog was seen on the reader by the owner, who reported "worked"; its look with a long, wrapping name and the number of screen refreshes were not noted. A successful open writes no log line, so DT-O2 and DT-O3 rest on the owner's statement.
- The version string is still 0.6.1 although the binary differs; the next release needs a version bump and a CHANGELOG entry (nothing is committed).
- The sync-pair folder mismatch (`DeviceBridge::openFile` accepts only paths under `PBCloud`) is not fixed and was not tested; such a pair file now gets the "could not open" dialog instead of silence.
- The card now holds extra copies from the owner's testing in `PBCloud/Rezepte` (for example `Baguette magique (2).pdf`, `Baguette magique (Kopie)`); they are not recorded as app problems.

## Recommendation

Close the bug. Before the next release: bump the version (0.6.2 suggested), add the CHANGELOG entry, and decide whether the sync-pair folder problem becomes its own bug.
