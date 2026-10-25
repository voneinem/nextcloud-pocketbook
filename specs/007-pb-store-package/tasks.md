---

description: "Task list for PB Store Release Package"
---

# Tasks: PB Store Release Package

**Input**: Design documents from `/specs/007-pb-store-package/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/packaging.md, quickstart.md

**Tests**: REQUIRED. The constitution makes Test-First non-negotiable (Principle I): every test task below MUST be written
and observed FAILING before the implementation task it names (a test that calls a script option or file that does not exist
yet counts as failing). The exception is marked *guard*: a test that already passes today and protects behavior that must not
change.

**Organization**: Tasks are grouped by user story (US1 ... US4 of spec.md). Paths are relative to the repository root. This
feature changes build/release scripts and docs only: **no file under `app/`, `core/`, `bridge/`, `qml/` or `res/` changes**
(FR-017).

## Format: `[ID] [P?] [Story] Description`

- **[P]**: can run in parallel (different files, no dependency on an incomplete task)
- **[Story]**: US1..US4 as in spec.md
- **[MANUAL]**: needs a person, the ARM build, the reader or a publishing step; an implementing agent skips these and
  reports them as open

## Conventions that apply to every task

- Bash with `set -euo pipefail`, the style of `scripts/package-device.sh` and `scripts/check-version-bump.sh` (functions,
  short comments, messages on stderr, `exit 1`). No new language, no new library.
- **No absolute local paths** in any tracked file (`scripts/check-constitution.sh` R5), including docs and examples: write
  `<path to the tool>`, never a real directory. **No tool location** anywhere but `PATH` or `PBSTORE_CATALOG`.
- The store's tool must be **tag v0.1.4 or newer** (`+` in file names, research R-1). Test doubles for `pbstore-catalog` are
  small shell scripts created in the test's temp dir. The real tool is used only where a task says so, found by the same
  lookup as the script (`PBSTORE_CATALOG`, else `PATH`); when it is absent that part of the test prints `SKIP` and the test
  exits 77 only if *nothing* else ran (CTest `SKIP_RETURN_CODE 77`).
- The fake ARM "binary" is a file containing the text of `VERSION` (the script only greps it). The tests pass a fake device
  probe (a text file) as 4th argument and set `PBC_QT_IMAGE_PLUGINS` to a temp dir holding empty `libqjpeg.so` and
  `libqgif.so`, and unset `POCKETBOOK_SDK`, so the staged tree is the same on every machine.
- Fixed manifest values are exactly those of `data-model.md`; quote them from there, do not retype from memory.
- The existing modes `--check-size` and `--self-test` of `scripts/package-device.sh` keep working **without** the store's
  tool (the tool lookup comes after them).

---

## Phase 1: Setup

- [x] T001 Complete the baseline in `specs/007-pb-store-package/research.md` (the heading "Baseline record (0.6.0)" and R-1
  already hold the `check-package` output, made with tool v0.1.4 on `dist/pbcloud-inkpad-one-0.6.0.zip`): confirm that file
  still exists and has SHA-256 `0f4fc6aba31d112a6539e2c1729a58b8d09bf4f3d63260709030eb272e523165` (if not, rebuild it from the
  commit before this branch with the old script and update the hash); append `unzip -Z1` of it (files only, without the `pbcloud-zoneinfo/` lines, plus the count of those) as a fenced
  block to the same heading
- [x] T002 [P] Create `tests/packaging/applications.expected`: the sorted (`LC_ALL=C sort`) list of file paths under
  `applications/` of that baseline ZIP **other than** `applications/pbcloud-zoneinfo/...` (files only, no directory entries,
  no `INSTALL.md`; exactly these 22 lines: `applications/icons/pbcloud.bmp`, `applications/icons/pbcloud_f.bmp`,
  `applications/pbc-probe.bin`, `applications/pbcloud-cacert.pem`, `applications/pbcloud-emoji-OFL.txt`,
  `applications/pbcloud-emoji.ttf`, the 11 files under `applications/pbcloud-licenses/` (`LICENSE`, `licenses/Apache-2.0.txt`,
  `licenses/CC-BY-4.0.txt`, `licenses/GPL-3.0.txt`, `licenses/LGPL-2.1.txt`, `licenses/LGPL-3.0.txt`,
  `licenses/MPL-2.0.txt`, `licenses/curl.txt`, `licenses/inkfeed-MIT.txt`, `licenses/manifest.txt`,
  `third_party/noto-emoji/OFL.txt`), `applications/pbcloud-plugins/imageformats/libqgif.so`,
  `applications/pbcloud-plugins/imageformats/libqjpeg.so`, `applications/pbcloud-probe.sh`, `applications/pbcloud.app`,
  `applications/pbcloud.bin`). Put a `#` comment line first saying that the 599 `pbcloud-zoneinfo/` files are not listed
  because the test derives them from `third_party/tzdata`. This is the SC-003 reference

## Phase 2: Foundational (blocks every story)

- [x] T003 Create `packaging/manifest.template.json` with exactly the values of the "Manifest template" table in
  `data-model.md` (`schema_version` "1.0", `id` "pbcloud", `name` "PB Cloud", `version` "@VERSION@", `min_firmware`
  "6.11.1437", `target_device` "InkPad One", `menu_entry` {`id` "U_PbCloud", `title` "PB Cloud", `path`
  "applications/pbcloud.app", `icon` "applications/icons/pbcloud.bmp", `focused_icon` "applications/icons/pbcloud_f.bmp",
  `group` "@General"}, `data_dir` "system/pbcloud"); no `files` key; valid JSON (check with `python3 -m json.tool`)
- [x] T004 Create the test harness `tests/packaging/test_package_device.sh` (executable): `ROOT` from `$1` or the script's
  location; a temp dir with trap cleanup; `check`/`fail` helpers like `tests/launcher/test_launcher.sh`; a helper
  `build <out-dir> [version]` that creates the fake binary (text of `VERSION`), the fake probe and the fake plugin folder (see
  Conventions) and runs `scripts/package-device.sh <fake> "<version>" <out-dir> <fake-probe>` capturing stdout, stderr and
  the exit code; a helper `real_tool` that prints the real tool path or nothing; a helper `stub <dir> <name> <body>` that
  writes an executable stub tool into a temp bin dir. No test cases yet (the file runs and exits 0)
- [x] T005 Register the test in `tests/CMakeLists.txt` next to `launcher_script`:
  `add_test(NAME packaging_script COMMAND ${CMAKE_SOURCE_DIR}/tests/packaging/test_package_device.sh ${CMAKE_SOURCE_DIR})`
  and `set_tests_properties(packaging_script PROPERTIES SKIP_RETURN_CODE 77 LABELS "script")`, with a comment line naming
  spec 007

**Checkpoint**: `ctest -R packaging_script` runs and passes with no cases.

---

## Phase 3: User Story 1 - The release package is accepted by the PB Store contract check (Priority: P1) 🎯 MVP

**Goal**: the built ZIP has `manifest.json` and `applications/` at its root only and passes `check-package`.

**Independent Test**: with the real tool available, run `scripts/package-device.sh` on a fake binary, then
`pbstore-catalog check-package` on the ZIP: exit 0; `unzip -Z1` shows only `manifest.json` and `applications/...`; the
manifest shows the data-model values and the `VERSION`.

### Tests for User Story 1 (write first, observe failing)

- [x] T006 [US1] In `tests/packaging/test_package_device.sh` add case "package passes the store check" (real tool, else
  SKIP): build into a temp dir **with the real tool's directory prepended to `PATH`** (until US2 the script looks the tool
  up on `PATH` only), then `<real tool> check-package <zip>` exits 0. This case also proves that names with `+` pass (the
  real zone data is staged). Observed failing: the current script puts `INSTALL.md` at the root and writes no manifest
  (E-PKG-04, E-PKG-05). Run it and record the failure in `research.md` under "Test-first record"
- [x] T007 [US1] In the same file add case "top level and manifest" (needs only a **stub** tool, so it always runs): the stub
  `pbstore-catalog` implements `pack <dir> --out <zip>` by copying the whole `<dir>` tree to a side directory and creating
  an empty file at `<zip>` (no ZIP tool needed), and `check-package` as `exit 0`. Assert on the **staged directory** the stub
  saw: `manifest.json` exists, its `version` equals `cat VERSION`, `schema_version` "1.0", `id` "pbcloud", `name` "PB
  Cloud", `min_firmware` "6.11.1437", `target_device` "InkPad One", `menu_entry.id` "U_PbCloud", `title` "PB Cloud", `path`
  "applications/pbcloud.app", `icon` "applications/icons/pbcloud.bmp", `focused_icon` "applications/icons/pbcloud_f.bmp",
  `group` "@General", `data_dir` "system/pbcloud"; no `files` key in the staged manifest; the staged top level is only
  `manifest.json` and `applications`; no `INSTALL.md` anywhere. (Parse with `python3 -I -c 'import json,sys...'`.) Observed
  failing: no manifest is staged today
- [x] T008 [US1] Add case "version comes from one source": run `build` with a fake binary containing `9.8.7` and the
  version argument `9.8.7`; the ZIP is named `pbcloud-inkpad-one-9.8.7.zip` and the staged manifest says `9.8.7`. Add a second
  assertion: a fake binary that does **not** contain the version exits 1 with `version mismatch` and leaves no ZIP.
  Observed failing for the manifest part
- [x] T009 [US1] Add case "manifest equals INSTALL.md" (FR-003, *guard*: the template of T003 exists already, so it passes at once): from `INSTALL.md` take the JSON
  block under `"U_PbCloud"` (the lines between the first ```` ```json ```` fence after "Under the top-level" and its closing
  fence, wrapped in braces), and assert: `title` equals the template's `menu_entry.title`; `icon.path` and
  `focused_icon.path` equal the template's `icon` and `focused_icon`; `path` equals `/mnt/ext1/` + the template's
  `menu_entry.path`; the id after `"U_PbCloud"` equals `menu_entry.id`. To prove it can fail, change one value in a copy of the template once and watch it fail

### Implementation for User Story 1

- [x] T010 [US1] Edit `scripts/package-device.sh`: (a) add `$ROOT/packaging/manifest.template.json` to the required-input
  list and remove `$ROOT/INSTALL.md` from it; (b) delete the `install ... INSTALL.md` line; (c) after staging `applications/`,
  write `$stage/manifest.json` with `sed "s/@VERSION@/$VERSION/g" "$ROOT/packaging/manifest.template.json"`; (d) replace
  `zip -r -q` by `pbstore-catalog pack "$stage" --out "$ZIP"` followed by `pbstore-catalog check-package "$ZIP"`, the tool
  taken from `PATH` as a bare name for now (US2 adds the lookup), the tool's stdout redirected to stderr so stdout stays the
  ZIP path; keep `check_zip_size "$ZIP"` after it; (e) update the header comment: layout without `INSTALL.md`, with
  `manifest.json` created at build time
- [x] T011 [US1] Run `ctest -R packaging_script --output-on-failure`: T006-T009 now pass (T006 with the real tool). Run
  `scripts/package-device.sh` on a fake binary by hand and `unzip -Z1` the result to see the layout

**Checkpoint**: US1 delivers a store-valid package. Stop here for a minimal release if needed.

---

## Phase 4: User Story 2 - Reproducible build with the store's tool, or a clear failure (Priority: P2)

**Goal**: tool lookup through `PBSTORE_CATALOG` / `PATH`, a clear message when it is missing, a reproducibility proof on every
build, and no new ZIP after any failure.

**Independent Test**: build twice and compare; build with no tool and read the message.

### Tests for User Story 2 (write first, observe failing)

- [x] T012 [US2] In `tests/packaging/test_package_device.sh` add case "tool missing": `PATH` set to a temp dir holding only
  symlinks to the commands the script needs (`env` for the `#!/usr/bin/env bash` line, `bash`, `sed`, `grep`, `head`, `tr`,
  `sort`, `mktemp`, `install`, `mkdir`, `rm`, `cmp`, `cat`, `dirname`, `basename`, `mv`, `cp`, `find`, `stat`, `chmod`; `cd` is a
  builtin) and `PBSTORE_CATALOG` unset: exit code 1, stderr contains `pbstore-catalog not found` and `PBSTORE_CATALOG`, **no**
  `*.zip` in the output folder, and the check happens before any staging (stderr has no `missing input`), and the build ends within 5 seconds (SC-005). Observed failing:
  today the shell reports `command not found` with another exit code and text
- [x] T013 [US2] Add case "environment variable": a stub tool at `<tmp>/tools/mytool` (not named `pbstore-catalog`),
  `PBSTORE_CATALOG=<tmp>/tools/mytool`, `PATH` without any tool: the build succeeds and the stub was called (it appends to a
  log). Add a second assertion: `PBSTORE_CATALOG=<tmp>/nonexistent` with a good tool on `PATH` exits 1 with `PBSTORE_CATALOG
  is set but is not an executable file` and does **not** fall back to `PATH`. Observed failing
- [x] T014 [US2] Add case "reproducibility": (a) a stub whose `pack` writes the current nanosecond time into the ZIP (so two
  packs differ): exit 1, stderr `package is not reproducible`, no ZIP in the output folder; (b) a stub whose `pack` is
  deterministic: exit 0, and `pack` was called twice (log). Observed failing: the script packs once
- [x] T015 [US2] Add case "check failure leaves nothing": a stub whose `check-package` prints `error[E-PKG-04] x: y` to
  stderr and exits 1: build exits 1, that line is on the build's stderr, `package check failed` follows, and the output
  folder contains no `*.zip` - also when a ZIP of the same version existed before (create a placeholder file with that name
  first; it must be gone). Observed failing
- [x] T016 [US2] Add case "bad inputs": version argument `1.2` or `1.2.x` -> exit 1 `version is not MAJOR.MINOR.PATCH`; a
  template with an extra `@X@` needs a different `ROOT` (the script derives it from its own location), so build a temporary
  root: copy `scripts/` and `packaging/` from the repository into it, symlink `platform`, `third_party`, `res`, `licenses`,
  `LICENSE` and `VERSION` from the repository, an empty `VERSION` with no version argument -> exit 1 `no version` (copy `VERSION` instead of symlinking it for this case); append `@X@` to the **copied** `packaging/manifest.template.json`, and run the
  copied script -> exit 1 `unfilled placeholder`; a missing template (delete it in that copy) -> `missing input:
  ...manifest.template.json`. Observed failing for the first two
- [x] T017 [US2] Add case "size budget and old modes" (*guard* for the existing behavior): `PBC_MAX_ZIP_BYTES=10` with a
  deterministic stub that writes a longer file as the ZIP -> exit 1, `larger than the budget`, no ZIP in the output folder;
  `scripts/package-device.sh --self-test` and `--check-size <file>` still succeed with `PATH` holding **no** store tool
  (they must not need it)

### Implementation for User Story 2

- [x] T018 [US2] Edit `scripts/package-device.sh`: add `resolve_tool` (R-2): if `PBSTORE_CATALOG` is set and not empty, require
  `[ -f ... ] && [ -x ... ]` else print the "is set but is not an executable file" message and exit 1; otherwise
  `command -v pbstore-catalog` else print the "not found" message of `contracts/packaging.md` and exit 1. Call it right after
  the `--check-size` / `--self-test` branches and the version argument handling, **before** the input checks. Use `"$PBSTORE"`
  everywhere the tool is run
- [x] T019 [US2] Edit `scripts/package-device.sh`: validate the version with `[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]`
  (message from the contract) before it is used; after writing the manifest, fail with `manifest template has an unfilled
  placeholder: <text>` if `grep -Eo '@[A-Z_]+@'` finds one
- [x] T020 [US2] Edit `scripts/package-device.sh`: replace the single pack by R-4/R-5: `rm -f "$ZIP"`; pack to `$work/a.zip`
  and `$work/b.zip` (`$work` a second temp dir, outside `$stage`, removed by the trap); `cmp -s` them, else the
  not-reproducible message; `check-package "$work/a.zip"`, on failure print `package check failed` and exit 1; run
  `check_zip_size "$work/a.zip"` (on failure exit 1 as today); `mv "$work/a.zip" "$ZIP"`; print `$ZIP` on stdout. Redirect the
  tool's stdout to stderr throughout
- [x] T021 [US2] Run `ctest -R packaging_script --output-on-failure`: T012-T017 pass. Run the real tool by hand through the
  quickstart section 2 rows "Reproducible" and "Store contract"

**Checkpoint**: US1 + US2 give a trustworthy, reproducible build.

---

## Phase 5: User Story 3 - Manual installation keeps working, instructions stay reachable (Priority: P2)

**Goal**: the files under `applications/` are exactly the 0.6.0 set; the instructions live in the repository and the release
notes, with no copy in the ZIP.

**Independent Test**: compare the file list with `tests/packaging/applications.expected` and the zone data; follow the README
to `INSTALL.md`.

### Tests for User Story 3

- [x] T022 [US3] In `tests/packaging/test_package_device.sh` add case "applications set unchanged" (*guard*; uses the stub from
  T007, which records the staged tree): the sorted list of staged files under `applications/` equals the lines of
  `tests/packaging/applications.expected` (comments skipped) **plus** `applications/pbcloud-zoneinfo/VERSION` **plus**
  `applications/pbcloud-zoneinfo/<path>` for every file found under `third_party/tzdata/zoneinfo`; and the staged
  `applications/pbcloud.app`, `pbcloud-probe.sh` and `pbcloud-cacert.pem` equal their sources byte for byte (`cmp`). This
  passes already after US1; keep it so a later change to the file set fails loudly. Also assert that no staged file is named
  `INSTALL.md` (FR-005), and build once more with a fake hand-off probe as 5th argument: the staged tree then also holds
  `applications/pbc-handoff.bin` and `applications/pbcloud-handoff-target.app` and nothing else new (the manifest lists whatever is
  staged)
- [x] T023 [P] [US3] In the same file add a doc check: `README.md` links `](INSTALL.md)` and does **not** contain the text
  `the zip with INSTALL.md`; `INSTALL.md` contains `merging it with the existing` (the manual steps survive) and
  `manifest.json` (the sentence of T025). Observed failing: the README line 169 still says "the zip with INSTALL.md"

### Implementation for User Story 3

- [x] T024 [P] [US3] Edit `README.md` "Installation": say that `INSTALL.md` lives in the repository and is linked from each
  GitHub release, and that it is not inside the package; keep the in-short sentence
- [x] T025 [P] [US3] Edit `INSTALL.md`: below the first paragraph add one sentence that the package also contains a
  `manifest.json` at its root (read by the PB Store, ignored here: copy only the `applications/` folder) and that these steps
  are not inside the package but in the repository, linked from each GitHub release. Nothing else changes (FR-011)
- [x] T026 [US3] Run `ctest -R packaging_script`: T022, T023 pass

**Checkpoint**: manual installers lose nothing.

---

## Phase 6: User Story 4 - The release is tagged and published with the package attached (Priority: P3)

**Goal**: a local gate that the tag, `VERSION`, ZIP name and manifest version agree, and the documented release steps.

**Independent Test**: `scripts/check-release.sh --self-test`; then on a real package `scripts/check-release.sh`.

### Tests for User Story 4 (write first, observe failing)

- [x] T027 [US4] Create `scripts/check-release.sh` containing **only** the `--self-test` branch and a stub `check()` (header
  comment per `contracts/packaging.md`), in the style of `scripts/check-version-bump.sh`. The self-test builds throw-away ZIPs
  in a temp dir with `python3 -m zipfile -c <zip> <files...>` (no `zip` command needed): a ZIP
  `pbcloud-inkpad-one-1.2.3.zip` holding `manifest.json` `{"version": "1.2.3"}` with a `VERSION` file `1.2.3` -> expect pass
  for tag `v1.2.3`; fail for tag `v1.2.4`; fail when `VERSION` says `1.2.4`; fail when the ZIP is named `...-1.2.4.zip`; fail
  when the ZIP has no `manifest.json`; fail (exit 2) when the ZIP does not exist. Register
  `add_test(NAME check_release_selftest COMMAND ${CMAKE_SOURCE_DIR}/scripts/check-release.sh --self-test)` in
  `tests/CMakeLists.txt` beside `check_version_bump_selftest`. Observed failing (stub `check()` always passes)

### Implementation for User Story 4

- [x] T028 [US4] Implement `check()` in `scripts/check-release.sh` (R-7): arguments `[zip] [tag]` with the defaults of the
  contract; `VERSION` read like `package-device.sh` does; manifest version via `unzip -p "$zip" manifest.json | sed -n
  's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1`; three comparisons, one `release check FAILED: ...`
  line per mismatch on stderr, exit 1 for any, exit 2 for a missing ZIP; success prints `release check passed (<version>)`.
  `unzip` is the only tool beyond coreutils
- [x] T029 [US4] Run `ctest -R check_release_selftest --output-on-failure`: passes. Run `scripts/check-release.sh` by hand on
  a ZIP built in T011

**Checkpoint**: all four stories work; the release can be verified.

---

## Phase 7: Polish & cross-cutting

- [x] T030 [P] Edit `README.md`: (a) fix the package command in "Device build": it needs the built binary, so write
  `POCKETBOOK_SDK=<sdk> scripts/package-device.sh build-arm/app/pbcloud.app "" dist build-arm/app/probe/pbc-probe.bin` and
  drop the comment "the zip with INSTALL.md"; (b) add a sentence on what the script now does (manifest from
  [packaging/manifest.template.json](packaging/manifest.template.json) and `VERSION`, pack twice with `pbstore-catalog`,
  compare, `check-package`, then write `dist/pbcloud-inkpad-one-<version>.zip`; stops with a message when the tool is
  missing); (c) under "Building from source", next to the device-build block, add a requirement line: `pbstore-catalog` from the appstore-pocketbook repository (tag v0.1.4 or newer, older
  tags reject the time zone file names), on `PATH` or named by `PBSTORE_CATALOG`, no default location, plus `unzip` and
  `python3` for the release check and the packaging tests; (d) add a "Releasing" subsection under "Building from source" (and a line in the table of contents if it lists subsections) with the release steps block of
  `contracts/packaging.md` (tag, `scripts/check-release.sh`, push, `gh release create` with notes = changelog entry + a link
  to `INSTALL.md` at that tag), `scripts/check-release.sh` **chained** to the push and the release by `&&` so a mismatch stops
  the release (FR-013, US4 scenario 2); (e) the "Documentation" list gets `specs/007-pb-store-package/spec.md`
- [x] T031 [P] Add item `DT-PKG-1` to `docs/device-test-checklist.md` after `DT-INS-5`: "Read the firmware version on the
  reader; install the 0.6.1 package by hand over 0.6.0 and start the app" - expected "firmware is `6.11.1437` (else change
  `min_firmware` in `packaging/manifest.template.json`); the app starts as with 0.6.0"; status `U`, date and result empty
- [x] T032 Raise `VERSION` to `0.6.1` and add the `## 0.6.1 - <date>` entry to `CHANGELOG.md` (above 0.6.0): the release
  package now follows the PB Store package contract (a `manifest.json`, built with the store's tool at tag v0.1.4 or newer,
  reproducibly, checked); install instructions are no longer inside the package but in the repository and the release notes;
  `INSTALL.md` is unchanged for the manual route; "No change to the app, to the data or to the server behavior", in the style of
  earlier entries; state that the package check and the firmware value are unverified on the reader until DT-PKG-1 is done
- [x] T033 Run `scripts/check-version-bump.sh`, `scripts/check-constitution.sh` (R5: no absolute local path in the new files;
  no hard-coded SDK path) and `scripts/pre-merge.sh`; all green, including `packaging_script`, `check_release_selftest` and
  `package_size_selftest`. Fix findings in the files that cause them. Without the real tool the happy-path case prints SKIP;
  run once more with `PBSTORE_CATALOG` set so that nothing is skipped
- [x] T034 [MANUAL] Rebuild the ARM binary (README device build; `VERSION` is 0.6.1 now, so re-run the CMake configure), package
  it with `scripts/package-device.sh build-arm/app/pbcloud.app "" dist build-arm/app/probe/pbc-probe.bin` (the 0.6.0 baseline
  was built the same way, so the optional parts match), and run `quickstart.md` section 2 (all rows, including the `diff -r`
  against `dist/pbcloud-inkpad-one-0.6.0.zip` unpacked) and section 3 (failure paths) with the real tool. Record the outputs
  (check result, SHA-256, size) in `research.md` under "Build check"
- [x] T035 [MANUAL] (firmware read and recorded 2026-10-25; the by-hand install was not run, DT-PKG-1 stays open for it) Quickstart section 4 on the reader: read the firmware version and record it with the date in
  `docs/device-findings.md`; install the new package by hand as `INSTALL.md` describes and start the app; record "works like
  0.6.0" (or what differs) in `research.md` under "Device check" and fill DT-PKG-1. If the firmware is not `6.11.1437`, change
  `min_firmware`, repeat T034 and T033. Mention the file-mode note of R-1
- [ ] T036 [MANUAL] After the merge to `main` and only when the maintainer says so: quickstart section 5 - tag `v0.6.1`,
  `scripts/check-release.sh`, push the tag, create the GitHub release with the ZIP and the notes, download the asset and run
  `check-package` on it, then **give the owner the URL of the release and of the ZIP asset** for the store entry (SC-007).
  Outward-facing: do not do this unprompted

---

## Dependencies & Execution Order

- Phase 1 -> Phase 2 -> Phase 3 (US1) -> Phase 4 (US2). US3 and US4 depend only on Phase 2 and US1's script change (T010) for
  T022; US4 is otherwise independent of US1-US3.
- US3's doc edits (T024, T025) and Phase 7's T030/T031 touch different files and run in parallel; `README.md` is edited by T024
  and T030: do T024 first.
- `scripts/package-device.sh` is edited by T010, T018-T020 in that order (same file, no [P]).
- Phase 7: T032 (version) before T033 (the gate); T034-T036 last and manual. **T036 requires T035 done** (the firmware value is
  not published before it was read on the reader, FR-015) and T034 rerun if `min_firmware` changed.

### Parallel opportunities

- T001 and T002 (different files); T003 with T004/T005 after T002; T024, T025 and T031 together; T027 can start as soon as
  Phase 2 is done, in parallel with US2.

## Implementation Strategy

- **MVP**: Phases 1-3 (US1): a store-valid package. Phases 4-6 add the reproducibility proof, the clear failures, the
  unchanged-file guard and the release gate; Phase 7 documents and versions it.
- Test-First: for every implementation task, run the tests named just before it and watch them fail, then implement, then
  watch them pass.
- The [MANUAL] tasks are what makes the release real; the code tasks alone do not publish anything.
