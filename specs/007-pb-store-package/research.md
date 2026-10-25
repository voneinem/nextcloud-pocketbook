# Research: PB Store Release Package

Each entry: decision, rationale, alternatives. "Trial" means a run made while planning (2026-10-25) on the real 0.6.0 package
contents (ARM build of `main`, `scripts/package-device.sh` with the device probe, no hand-off probe, the SDK's Qt image
plugins) with the store's tool built from the `appstore-pocketbook` tag v0.1.4 (`pbstore-catalog 0.1.4`, schema 1.x).

## R-1 Baseline and what the store's tool does with our files (trial)

**Baseline** (`check-package` on the package the unchanged script builds for 0.6.0, 4420690 bytes, 622 entries without
directory entries: `INSTALL.md` + 621 files under `applications/`):

| Tool | Result |
|------|--------|
| `pbstore-catalog 0.1.4` (tag v0.1.4) | exit 1: `E-PKG-04 INSTALL.md` and `E-PKG-05 manifest.json`, nothing else |
| a tool built before v0.1.4 (same as v0.1.3 for this rule) | exit 1: the same two, **plus 14 x `E-PKG-03`**: `applications/pbcloud-zoneinfo/GMT+0` and `Etc/GMT+0` ... `Etc/GMT+12` (`+` in the file name) |

So the request's "confirm there is nothing else" has an answer: with the right contract there is nothing else; with
v0.1.3 there is one more kind of finding. The store fixed this on 2026-10-25 for exactly these files (tag v0.1.4: "Package
file names may contain `+`. The IANA time-zone database that PB Cloud and PB Office ship has names such as `GMT+0` and
`Etc/GMT+1`"). **The contract and tool for this feature are v0.1.4 or newer.** The files themselves stay as they are: the
time zone names are the names the app looks up.

**Trial**: the baseline ZIP unpacked, `INSTALL.md` removed, the template manifest (version 0.6.0) added:
- `pack` exit 0, 4413918 bytes, prints size, SHA-256 and `app: pbcloud 0.6.0`; about 0.5 s.
- Packing the same directory twice gave byte-identical ZIPs.
- `check-package` on the result: exit 0 (`package ok`).
- The packed ZIP has no directory entries, and every file has mode 0644 (the old ZIP carried 0777 on `pbcloud.bin`,
  `pbcloud.app` and the probe scripts). The reader's storage is a FAT volume copied over USB, where modes do not exist
  (`docs/device-findings.md`: `chmod` is not observable), and the manual route of `INSTALL.md` never relied on them; the
  store installer is out of scope. Noted, not worked around (same finding as PB Notes 1.10.1).
- 4413918 bytes against the 25 MB budget: far below; the existing size check stays.

**Alternatives**: keep `zip` and add the manifest by hand (rejected: the spec requires the tool, and the file list with
SHA-256 would have to be computed by us).

## R-2 Finding the tool: `PBSTORE_CATALOG`, else `PATH`

**Decision**: if the environment variable `PBSTORE_CATALOG` is set, it must name an executable file (the tool itself); if it
is unset, `command -v pbstore-catalog`. A set-but-invalid variable is an error (it does not silently fall back to `PATH`).
No default path. The same resolved command runs `pack` and `check-package`. The lookup is the first thing the script does
(after reading `--check-size`/`--self-test`), so a missing tool is reported before any staging work (SC-005).

**Missing-tool message** (stderr, exit 1):
`pbstore-catalog not found. Put it on PATH or set PBSTORE_CATALOG to the tool (build it from the appstore-pocketbook repository, tag v0.1.4 or newer).`

**Rationale**: mirrors the project's rule for the SDK (`POCKETBOOK_SDK` only from the environment) and the name used by PB
Notes, so the maintainer sets one variable for both apps. A variable that is set wrongly is a configuration error the
maintainer wants to hear about.

**Alternatives**: a `--tool` argument (rejected: the script already takes positional arguments used by the README and
earlier docs); a default sibling-directory guess (rejected: a hidden path, and `check-constitution.sh` forbids absolute local
paths in tracked files); checking the tool's version (rejected: `--version` prints `0.1.4` for the tool, but a build made
before the `+` fix also prints `0.1.0`, so it cannot tell; the checker's own verdict is the test, and the failure message
for E-PKG-03 names the file).

## R-3 Writing the manifest: one placeholder, `sed`

**Decision**: `packaging/manifest.template.json` holds the fixed values and `"version": "@VERSION@"`. The script runs
`sed "s/@VERSION@/$VERSION/g"` into the staging folder, after checking `$VERSION` matches `^[0-9]+\.[0-9]+\.[0-9]+$` (the same
rule as `check-version-bump.sh`, so the value needs no JSON escaping). It then fails if any `@[A-Z_]+@` remains. `files` is
not in the template: `pack` computes it.

**Rationale**: the docs of the store show exactly this shape; the version is validated first so `sed` is safe; the
leftover-placeholder check covers the "template changed, script not" edge case.

**Alternatives**: `jq` (rejected: a new tool dependency for one string); generating the JSON in the script (rejected: the
spec wants a template in the repository).

## R-4 Reproducibility check

**Decision**: pack twice into two temporary files in the temp dir, `cmp` them; on a difference fail with
`package is not reproducible: two builds from the same files differ`. The first file is then checked and published.

**Rationale**: `pack` is documented and observed (R-1) to be deterministic, so the second pack costs about half a second and
proves it for every build instead of trusting it; it would also catch a future tool change.

**Alternatives**: trust the documentation (rejected by FR-006); pack twice from two separate staging copies (rejected: the
only way the two could differ is the staging tree itself, which is one directory).

## R-5 Verifying and publishing atomically; the size budget

**Decision**: order inside the script: validate inputs and version -> resolve tool -> stage -> manifest -> remove any existing
target ZIP -> pack x2 -> `cmp` -> `check-package` on the first -> existing size check (`check_zip_size`) on it -> `mv` it to
`<out-dir>/pbcloud-inkpad-one-<version>.zip` -> print the path (stdout, as today). Any failure exits non-zero before the
`mv`, so the output folder never contains a new, unchecked ZIP; the old ZIP of the same version is removed first so it
cannot be mistaken for the result.

**Rationale**: matches the spec's edge cases (replace, never merge; no valid-looking ZIP after a failure). The size check
moves from "after zip" to "before publish" and keeps its message.

## R-6 Where the install instructions go

**Decision**: they are not shipped. `INSTALL.md` stays in the repository, the README links it (as today, line "Installation"),
and the GitHub release notes carry a tag-pinned link to it (`.../blob/v<version>/INSTALL.md`) next to the changelog entry.
`INSTALL.md` gets one sentence saying that the package also holds a `manifest.json` that the PB Store reads and that is to
be ignored for the manual steps, and its first sentence ("These steps install the app from the release zip") stays true.
Its steps (copy `applications/`, register `U_PbCloud`) are unchanged because the `applications/` set is unchanged.

**Alternatives**: a copy under `applications/` (rejected: it would add an installed file, against FR-011); the
instructions in the release notes only (rejected: they would be out of date on the next release, and the README is where
people look first).

## R-7 Tag/version check as a small gate script

**Decision**: `scripts/check-release.sh [<zip>] [<tag>]` (defaults: `dist/pbcloud-inkpad-one-$(cat VERSION).zip`,
`v$(cat VERSION)`). It reads `manifest.json` from the ZIP with `unzip -p`, extracts `"version"` with `sed`, and requires:
tag = `v` + manifest version, manifest version = `VERSION`, ZIP file name = `pbcloud-inkpad-one-<manifest version>.zip`.
Exit 1 with one line per mismatch. `--self-test` builds throw-away ZIPs (with `python3 -m zipfile`, so no `zip` command is
needed) to show pass and each mismatch fail. Not part of `pre-merge.sh` (no ZIP exists there); it is a step of the
documented release procedure and of `quickstart.md`.

**Rationale**: the store checks `E-ENT-14` (tag in `package_url` = manifest version) only when the entry is added, a
repository away; failing locally before the release is published is cheaper. A tag mismatch cannot be caught by
`check-package`.

**Alternatives**: do it inside `package-device.sh` (rejected: the tag may not exist at build time and the build must not
depend on git state); a CI workflow (rejected: CI is deferred in this project, see `pre-merge.sh`).

## R-8 Version number of this change: 0.6.1

**Decision**: PATCH -> `VERSION` 0.6.1, tag `v0.6.1`.

**Rationale**: the app does not change; the package does (a fix of the artifact). The ARM binary must be rebuilt because it
carries the version string (the packaging script refuses a mismatch), which changes the CMake configure step's version and
nothing else. `scripts/check-version-bump.sh` requires the bump, since `scripts/` and `packaging/` count as product files.
0.6.0 has no tag and no GitHub release; it is not tagged retroactively (it was never published), so the first GitHub release
of PB Cloud is 0.6.1. The existing tag `v0.5.0` stays.

**Alternatives**: tag and publish 0.6.0 as it is (rejected: its package breaks the contract, the whole point of this feature);
MINOR 0.7.0 (rejected: no feature for the reader).

## R-9 `min_firmware`, and what "tested" means for PB Cloud

**Decision**: `"6.11.1437"`, confirmed by the owner (spec, Clarifications). Evidence in the repository: PB Cloud 0.1.0 ran on
the owner's InkPad One on 2026-10-20 (`docs/device-findings.md`), the same reader on which PB Notes recorded firmware 6.11.1437
on 2026-10-25. The repository holds no firmware reading for PB Cloud itself, so a reading is part of this release's device
check (quickstart section 4, checklist item DT-PKG-1) and is written to `docs/device-findings.md` with the date. If it
differs, `min_firmware` becomes the observed value in the template before the release is published. It is a fixed value of
the template; nothing in the build derives it.

**Alternatives**: a lower or "unknown" value (rejected: not observed, and FR-015 forbids claiming it); reading the firmware
from the device at build time (rejected: the build machine is not the reader).

## Out of scope, noted for later

- The catalog entry (`catalog/entries/pbcloud.json`) in `appstore-pocketbook`; its `package_url` must use the tag chosen here
  (`https://github.com/voneinem/nextcloud-pocketbook/releases/download/v0.6.1/pbcloud-inkpad-one-0.6.1.zip`). The owner adds it;
  the maintainer hands over the release URL (SC-007).
- Other models and firmware are untested and not claimed.
- Whether the store's installer needs the file modes that `pack` drops (R-1).

## Baseline record (0.6.0)

`pbstore-catalog check-package` (0.1.4) on the package the unchanged script built for 0.6.0, exit 1:

```text
error[E-PKG-04] INSTALL.md: "INSTALL.md" is not allowed at the package root (only manifest.json and the applications/ tree)
error[E-PKG-05] manifest.json: manifest.json is missing from the package root
```

That package is kept (git-ignored) as `dist/pbcloud-inkpad-one-0.6.0.zip`, SHA-256
`0f4fc6aba31d112a6539e2c1729a58b8d09bf4f3d63260709030eb272e523165`; it is the reference for SC-003 and the baseline of the
quickstart comparison. If it is lost, rebuild it from `main` (the commit before this branch) with the old script.

The files under `applications/` of that package (621): the 22 listed in `tests/packaging/applications.expected` (task T001
creates it from this baseline) and the 599 of `pbcloud-zoneinfo/` (598 time zone files and `VERSION`).

Entries of that package without the `applications/pbcloud-zoneinfo/` lines (22 files under `applications/` plus `INSTALL.md`);
the 599 zone lines are not repeated here (621 files under `applications/` in all):

```text
INSTALL.md
applications/icons/pbcloud.bmp
applications/icons/pbcloud_f.bmp
applications/pbc-probe.bin
applications/pbcloud-cacert.pem
applications/pbcloud-emoji-OFL.txt
applications/pbcloud-emoji.ttf
applications/pbcloud-licenses/LICENSE
applications/pbcloud-licenses/licenses/Apache-2.0.txt
applications/pbcloud-licenses/licenses/CC-BY-4.0.txt
applications/pbcloud-licenses/licenses/GPL-3.0.txt
applications/pbcloud-licenses/licenses/LGPL-2.1.txt
applications/pbcloud-licenses/licenses/LGPL-3.0.txt
applications/pbcloud-licenses/licenses/MPL-2.0.txt
applications/pbcloud-licenses/licenses/curl.txt
applications/pbcloud-licenses/licenses/inkfeed-MIT.txt
applications/pbcloud-licenses/licenses/manifest.txt
applications/pbcloud-licenses/third_party/noto-emoji/OFL.txt
applications/pbcloud-plugins/imageformats/libqgif.so
applications/pbcloud-plugins/imageformats/libqjpeg.so
applications/pbcloud-probe.sh
applications/pbcloud.app
applications/pbcloud.bin
```

## Test-first record

2026-10-25, before `scripts/package-device.sh` was changed: `tests/packaging/test_package_device.sh` (tool v0.1.4 named by
`PBSTORE_CATALOG`) failed with 39 `FAIL` lines in all cases except the guards ("manifest equals INSTALL.md", the
`version mismatch` and size-budget parts, `--self-test` and `--check-size` without the tool). Examples: the real
`check-package` rejected the package built by the old script (E-PKG-04, E-PKG-05); no `manifest.json` was staged; a missing tool
ended in exit 127 (`command not found`) instead of exit 1 with the message; a non-reproducible, a failing and a bad-version
build all exited 0 or with the wrong message.

`scripts/check-release.sh --self-test` with a stub `check()` that always passes: `self-test failed: tag differs (exit 0, wanted 1)`.
After the implementation of `check()`: `check-release.sh self-test passed`. After the script changes, `ctest -R packaging_script`
passed with the real tool (v0.1.4): the package built from the fake binary and the real zone data passed `check-package`.

## Build check (0.6.1, quickstart sections 2 and 3)

Date 2026-10-25. ARM build (`cmake --build build-arm`, ELF 32-bit ARM EABI5, contains `0.6.1`), packaged with
`scripts/package-device.sh build-arm/app/pbcloud.app "" dist build-arm/app/probe/pbc-probe.bin` (the options of the 0.6.0
baseline: device probe, no hand-off probe, the SDK's Qt image plugins), `pbstore-catalog 0.1.4` built from the tag v0.1.4 and
named by `PBSTORE_CATALOG`.

| Check | Result |
|-------|--------|
| `check-package dist/pbcloud-inkpad-one-0.6.1.zip` | exit 0, `package ok`, 4413890 bytes, SHA-256 `94e07092335ad83bcf1c7ec1be8e13f26fe2fe6e6f29803f5d000b0b93767c87` |
| Top level | only `manifest.json` (plus `applications/`); 0 entries named `INSTALL` |
| Manifest | all values of `data-model.md`, `version` 0.6.1, 621 files listed |
| `applications/` vs the 0.6.0 package | `diff -r` of the unpacked trees: the only difference is `applications/pbcloud.bin` (the version string `0.6.1` against `0.6.0`; the `strings` of both differ in that line and two noise strings); 621 files, same paths and content otherwise |
| Reproducible across builds | a second full build with the same options: same SHA-256 (a first attempt without `POCKETBOOK_SDK` left out the two Qt plugin files and so gave another hash, as it should) |
| `scripts/check-release.sh <zip> v0.6.1` / `v0.0.0` | `release check passed (0.6.1)` / exit 1 `tag v0.0.0 does not match manifest version 0.6.1` |
| Tool not on `PATH`, variable unset | exit 1 after 0.01 s, `pbstore-catalog not found. Put it on PATH or set PBSTORE_CATALOG ...`, no output folder created |
| `PBSTORE_CATALOG=/nonexistent` | exit 1, `PBSTORE_CATALOG is set but is not an executable file: /nonexistent` |
| Version argument 9.9.9, binary built for 0.6.1 | exit 1, `version mismatch`, no ZIP |
| Older tool | not run here; the baseline of R-1 shows the 14 `E-PKG-03` lines |

The ZIP is 4413890 bytes, the 0.6.0 ZIP 4420690 bytes (no `INSTALL.md`, no directory entries, a manifest with the file list added).

## Device check (0.6.1, quickstart section 4)

Date 2026-10-25. Firmware of the owner's InkPad One: `6.11.1437`, reported by the owner, so `min_firmware` stays as in the template.
The manual install of the 0.6.1 package over 0.6.0 and the start of the app were **not run** before the release; the 0.6.1 package
differs from the 0.6.0 one only in `applications/pbcloud.bin` (the version string; build check above). DT-PKG-1 stays open for the
install part. The file-mode note of R-1 applies (`pack` writes 0644; the reader's FAT volume has no modes).
