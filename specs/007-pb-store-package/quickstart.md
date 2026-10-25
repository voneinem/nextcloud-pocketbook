# Quickstart: PB Store Release Package

Validation guide for feature 007. Interfaces: [contracts/packaging.md](contracts/packaging.md). Values:
[data-model.md](data-model.md).

## 0. Prerequisite: the store's tool

`pbstore-catalog` from `appstore-pocketbook`, **tag v0.1.4 or newer** (v0.1.3 rejects the `+` in the time zone names), on
`PATH` or named by the environment variable:

```bash
export PBSTORE_CATALOG=<path to the pbstore-catalog executable>
"$PBSTORE_CATALOG" --version        # expected: pbstore-catalog 0.1.4 ...
```

## 1. Host checks (no SDK, no device)

```bash
cmake -S . -B build-host -G Ninja && cmake --build build-host
ctest --test-dir build-host -R "packaging_script|check_release_selftest|package_size_selftest|check_version_bump_selftest" --output-on-failure
```

Expected: all pass. `packaging_script` runs the failure paths with stub tools; its happy-path part runs the real
`pbstore-catalog` when `PBSTORE_CATALOG` or `PATH` provides it and reports a skip otherwise.

## 2. Build the package (needs the ARM build, see README)

```bash
POCKETBOOK_SDK=<sdk> cmake -S . -B build-arm -DTARGET_TYPE=ARM -DCMAKE_BUILD_TYPE=Release -DPBC_CURL_PREFIX=$PWD/build-curl-arm
cmake --build build-arm
POCKETBOOK_SDK=<sdk> scripts/package-device.sh build-arm/app/pbcloud.app "" dist build-arm/app/probe/pbc-probe.bin
```

Expected: the path `dist/pbcloud-inkpad-one-<version>.zip` on stdout; on stderr the tool's `package ok`-style report (size,
SHA-256, `app: pbcloud <version>`) and the size line.

| Check | Command | Expected |
|-------|---------|----------|
| Store contract (SC-001) | `"$PBSTORE_CATALOG" check-package dist/pbcloud-inkpad-one-<version>.zip` | exit 0, no `E-PKG-*` |
| Top level (FR-001) | `unzip -Z1 dist/*.zip \| grep -v '^applications/'` | only `manifest.json` |
| Manifest values (FR-003) | `unzip -p dist/*.zip manifest.json` | the values of the data model; `version` = `cat VERSION` |
| Same files as 0.6.0 (SC-003) | unpack the 0.6.0 baseline package (built from `main` before this feature) and the new one; compare the trees under `applications/` with `diff -r` | identical, apart from `pbcloud.bin` (it carries the version string) and file modes |
| Reproducible (SC-002) | build twice, `sha256sum` both | identical |
| No INSTALL.md (FR-005) | `unzip -Z1 dist/*.zip \| grep -c INSTALL` | 0 |
| Menu entry equals INSTALL.md (FR-003) | compare `menu_entry` of the manifest with the JSON block in `INSTALL.md` section 2 | same id, title, icons; path `applications/pbcloud.app` is the file `/mnt/ext1/applications/pbcloud.app` names |

## 3. Failure paths

| Setup | Expected |
|-------|----------|
| tool not on `PATH`, `PBSTORE_CATALOG` unset | exit 1, `pbstore-catalog not found. Put it on PATH or set PBSTORE_CATALOG ...`, no ZIP (SC-005) |
| `PBSTORE_CATALOG=/nonexistent` | exit 1, `PBSTORE_CATALOG is set but is not an executable file` |
| `VERSION` changed, binary not rebuilt | exit 1, `version mismatch` (as before) |
| a `@X@` added to the template | exit 1, `unfilled placeholder` |
| a tool older than v0.1.4 | exit 1, the tool's `E-PKG-03` lines for the `GMT+N` names, `package check failed` |

## 4. Device check (once, on the reader; labelled unverified until done)

1. Read the firmware version on the reader (its own device information screen) and write it, with the date, to
   `docs/device-findings.md`. Expected `6.11.1437` (spec, Clarifications). If it differs, change `min_firmware` in
   `packaging/manifest.template.json` to the observed value, rebuild, and repeat section 2 before publishing.
2. Unzip the new package onto the reader as `INSTALL.md` describes (copy `applications/` over the old files, `U_PbCloud` is
   already registered, reboot) and start **PB Cloud**. Expected: the same as with 0.6.0 (DT-INS-1). Do not use a copy of the
   reader's data on the host (see the build notes).
3. Record both results in `research.md` under "Device check" and tick DT-PKG-1 in `docs/device-test-checklist.md`.

## 5. Release (maintainer, deliberate; after the merge)

```bash
git tag "v$(cat VERSION)" &&
scripts/check-release.sh &&                     # expected: release check passed (<version>)
git push origin "v$(cat VERSION)" &&
gh release create "v$(cat VERSION)" "dist/pbcloud-inkpad-one-$(cat VERSION).zip" --title "<version>" --notes-file <notes>
```

Chained on purpose: a mismatch stops the sequence before anything is pushed. To see it stop, run
`scripts/check-release.sh dist/pbcloud-inkpad-one-$(cat VERSION).zip v0.0.0`: expected exit 1 with a `release check FAILED`
line.

Then verify the published asset (US4, scenario 3):

```bash
gh release download "v$(cat VERSION)" --pattern '*.zip' --dir "$(mktemp -d)"   # then run check-package on it
```

Expected: `check-package` exit 0 on the downloaded file. Hand the owner the URL of the release (and of the ZIP asset) for the
store entry (US4, scenario 4).
