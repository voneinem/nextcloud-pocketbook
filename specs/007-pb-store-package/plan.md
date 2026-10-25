# Implementation Plan: PB Store Release Package

**Branch**: `feature/007-pb-store-package` | **Date**: 2026-10-25 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/007-pb-store-package/spec.md`

## Summary

The release ZIP gets a `manifest.json` at its root, loses `INSTALL.md` from its root, and is assembled by the store's own
tool `pbstore-catalog pack`, so that `pbstore-catalog check-package` passes. A baseline run on today's 0.6.0 package
([research.md](research.md) R-1) shows exactly E-PKG-04 and E-PKG-05 with the tool at `appstore-pocketbook` **v0.1.4**; with
v0.1.3 there are 14 more E-PKG-03 errors (the `+` in `GMT+0`, `Etc/GMT+1` ... of the bundled time zone data), which is why
the contract version in this feature is v0.1.4, not the v0.1.3 of the request. It is a **packaging change only**: no app
code, no QML, no core, no schema. The files under `applications/` are the ones `scripts/package-device.sh` already installs.
It ships in one slice, modelled on the finished PB Notes feature (`notes-pocketbook` spec 011):

1. **Manifest template** `packaging/manifest.template.json` with every fixed value and one placeholder, `@VERSION@`.
2. **`scripts/package-device.sh`** stages the same `applications/` tree, writes the manifest (version from the variable that
   names the ZIP and is checked against the binary), finds the tool (`PBSTORE_CATALOG`, else `PATH`; clear failure otherwise),
   packs twice and requires byte-identical results, runs `check-package`, applies the existing 25 MB budget, and only then
   puts the ZIP in the output folder. `INSTALL.md` is no longer an input or an entry. The `--check-size` and `--self-test`
   modes and the optional probe arguments stay as they are.
3. **`scripts/check-release.sh`** compares tag, `VERSION`, ZIP name and the manifest inside the ZIP (FR-013) and has a
   `--self-test` like the other gates. The tag, the GitHub release and the notes stay a deliberate step of the maintainer,
   documented in the README.
4. **Docs**: README (build + tool + release steps; the stale `# the zip with INSTALL.md` line), `INSTALL.md` (one sentence
   about `manifest.json`, one sentence on where the instructions live), `docs/device-findings.md` (the firmware reading),
   `docs/device-test-checklist.md` (one item), `CHANGELOG.md`.
5. **Release** 0.6.1 (PATCH: the installed app is the 0.6.0 app apart from its version string). Tag `v0.6.1` after the merge,
   GitHub release with the ZIP, URL handed to the owner (SC-007).

Resolved unknowns: [research.md](research.md) (R-1 ... R-9). Interfaces: [contracts/packaging.md](contracts/packaging.md).
Files and values: [data-model.md](data-model.md). Validation walk-through: [quickstart.md](quickstart.md).
A trial on the real 0.6.0 package contents (R-1) already showed that a staged tree with this manifest packs, packs
reproducibly and passes `check-package`.

## Technical Context

**Language/Version**: Bash (existing scripts are `#!/usr/bin/env bash`, `set -euo pipefail`); JSON for the template. No C++/QML.

**Primary Dependencies**: `pbstore-catalog` from `appstore-pocketbook` (tag v0.1.4 or newer; contract schema 1.0) as an
external, maintainer-installed build tool: not vendored, not a dependency of the app. `unzip` and `python3` (read the
manifest back out of the ZIP in `check-release.sh`; build throw-away ZIPs in the self-test), `cmp`, `sed`, `mktemp`
(coreutils). `zip` is no longer used by the package script.

**Storage**: none. Files only: template in `packaging/`, staging in a temp dir, output in `dist/` (git-ignored).

**Testing**: shell tests registered in CTest like `package_size_selftest` / `launcher_script`:
`tests/packaging/test_package_device.sh` (drives the script with a fake ARM "binary" and **stub** `pbstore-catalog`
executables for the failure paths; runs the real tool for the happy path when it is available, else reports SKIP) and
`scripts/check-release.sh --self-test`. The real-tool and device walk-through is `quickstart.md`.

**Target Platform**: Linux x86-64 maintainer machine (the ARM binary is cross-built as before); the package targets the
InkPad One, firmware >= 6.11.1437.

**Project Type**: single application with a host-testable core (unchanged); this feature touches only build/release scripts,
one template and documents.

**Performance Goals**: build overhead of two packs + one check on a 4.4 MB package with 621 files: about 0.5 s per pack
(measured, R-1). SC-005: a missing tool is reported before any staging work.

**Constraints**: no machine-specific path to the tool in the repository (`scripts/check-constitution.sh` rejects absolute
local paths in tracked files); `applications/` content and the ZIP file name unchanged; the build never leaves a ZIP that
looks valid after a failure; the 25 MB package budget still applies; nothing in the app changes.

**Scale/Scope**: 1 new JSON template, 1 script edited (about 40 lines net), 1 script added (about 70 lines with self-test),
1 test script and its expected-file list, 5 doc files, `VERSION`/`CHANGELOG.md`.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle / rule (constitution 1.3.0) | Pre-research | Post-design | Evidence |
|---------------------------------------|--------------|-------------|----------|
| I. Test-First | PASS | PASS | Each behavior is a script outcome a shell test observes: manifest content, top-level entries, tool lookup order, missing tool, unusable variable, non-reproducible pack, failing check, size budget, tag mismatch. `tasks.md` orders the packaging test and the `check-release.sh` self-test (observed failing against today's script) before the script changes. Boundary format (the ZIP/manifest) is covered by running the store's own checker on a built package |
| II. Host-testable core, layering | PASS | PASS | `core/`, `bridge/`, `qml/`, `app/` untouched. The tests run on the host without the SDK (a fake binary containing the version string) |
| III. No silent data loss | PASS (not applicable to user data) | PASS | No user data touched. The package still contains nothing of `system/pbcloud/` (database, credentials, log), and `data_dir` tells the store to keep it on removal; `PBCloud/` downloads are not in the package |
| IV. Security & privacy | PASS | PASS | No network access added (`pack` and `check-package` are offline). No secret, no user data. The tool is run from `PATH` or an explicit variable the maintainer controls; nothing is downloaded or executed from the network. The manifest holds no personal data |
| V. Simplicity & bounded footprint | PASS | PASS | No new dependency in the app. One template, one environment variable, one new gate script. Rejected: a JSON tool (`jq`) for one string, a CMake packaging target, a release workflow, vendoring the store's tool, a version check of the tool. The package budget (25 MB) is kept and still enforced (4.4 MB now) |
| VI. E-ink UX & battery | N/A | N/A | No UI, no runtime behavior |
| VII. Licensing hygiene | PASS | PASS | `package-device.sh` is already attributed to InkFeed in `NOTICE`; the change is our own. `pbstore-catalog` (same author, MIT) is run, not copied or shipped. No component added or removed, so `NOTICE` and `licenses/manifest.txt` are unchanged. Unofficial-app statement unchanged |
| VIII. Quality gates | PASS | PASS | Packages are still built with `scripts/package-device.sh`. The SDK location still comes only from `POCKETBOOK_SDK`; the tool location only from `PBSTORE_CATALOG`/`PATH`, no path in scripts or docs (placeholders only). Every new rule has a self-test (FR-016). `pre-merge.sh` runs the new CTest entries |
| IX. Specs-driven workflow, honest verification | PASS | PASS | Spec, clarify, plan, tasks, analyze, implement. What needs the reader (the firmware reading, manual install of the new package) is labelled unverified until done and recorded in `docs/device-findings.md` and `research.md`. The firmware value is not claimed beyond what was observed |
| X. English documentation | PASS | PASS | All new text is English; script messages are English |
| XI. Consistent look and feel | PASS | PASS | No UI. README/INSTALL/CHANGELOG changes follow the sibling PB Notes 1.10.1 wording |
| Stack and build constraints | PASS | PASS | No stack change; both targets unchanged. App/binary/launcher/data-dir names are untouched, so no collision with PB Notes or PB Office; the manifest's `id`, menu id and `data_dir` differ from theirs |
| Release version (workflow) | PASS | PASS | Last phase of `tasks.md`: `VERSION` 0.6.1 + `CHANGELOG.md` entry, enforced by `check-version-bump.sh`; tag `v0.6.1` after the merge, verified by `check-release.sh` |

No Complexity Tracking entry is needed.

## Project Structure

### Documentation (this feature)

```text
specs/007-pb-store-package/
├── plan.md              # This file
├── research.md          # Phase 0: R-1 ... R-9, baseline
├── data-model.md        # Phase 1: package, manifest, template, release
├── quickstart.md        # Phase 1: build, check, device check, tag, publish, verify
├── contracts/
│   └── packaging.md     # script interfaces, template, error messages
├── checklists/requirements.md
└── tasks.md             # Phase 2 (/speckit-tasks - not created here)
```

### Source Code (repository root)

```text
packaging/
└── manifest.template.json        # NEW   fixed manifest values + @VERSION@

scripts/
├── package-device.sh             # CHG   stage manifest, find tool, pack twice + cmp, check-package, size budget, publish ZIP;
│                                 #       INSTALL.md no longer an input or entry; header comment updated
└── check-release.sh              # NEW   tag == VERSION == ZIP name == manifest version (+ --self-test)

tests/
├── CMakeLists.txt                # CHG   register packaging_script and check_release_selftest
└── packaging/
    ├── test_package_device.sh    # NEW   fake binary + stub/real pbstore-catalog; failure paths; applications/ list
    └── applications.expected     # NEW   the files under applications/ other than the time zone data

README.md                         # CHG   build + tool + release steps, INSTALL.md location
INSTALL.md                        # CHG   a sentence about manifest.json and about where these steps live
CHANGELOG.md, VERSION             # CHG   0.6.1
docs/device-findings.md           # CHG   firmware reading (after the device check)
docs/device-test-checklist.md     # CHG   DT-PKG-1: firmware reading and manual install of the new package
```

**Structure Decision**: keep the single-script packaging (`scripts/package-device.sh`) and add the template under a new
top-level `packaging/` folder, the location the store's adoption guide suggests and PB Notes uses. The tag check is its own
small script because it runs at release time, after the ZIP exists, not at build time, and it follows the pattern of the
other `scripts/check-*.sh` gates (including `--self-test`).

## Complexity Tracking

No constitution violation; nothing to justify.
