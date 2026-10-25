# Feature Specification: PB Store Release Package

**Feature Branch**: `feature/007-pb-store-package`

**Created**: 2026-10-25

**Status**: Draft

**Input**: User description: "Build the PB Cloud release package so that it follows the PB Store package contract (appstore-pocketbook, tag v0.1.3). The PB Store lists apps only if their release ZIP follows the contract. The release trips E-PKG-04 (INSTALL.md at the ZIP root; only manifest.json and the applications/ tree are allowed there) and E-PKG-05 (no manifest.json). Run `pbstore-catalog check-package` on the current release ZIP first to confirm there is nothing else. Needed: a manifest.json at the ZIP root created at build time from a template in the repository (schema_version 1.0, id pbcloud, name PB Cloud, version from the VERSION file, min_firmware the lowest firmware the app was really tested on, target_device InkPad One, menu entry id U_PbCloud with title, path, icon and focused icon as in INSTALL.md, group @General, data_dir system/pbcloud); INSTALL.md leaves the ZIP root, where the instructions live instead is open (PB Notes keeps it in the repository and links it from the release); the package built with `pbstore-catalog pack`, the tool found through PATH or an environment variable (no hard-coded path), a clear failure when it is missing, built twice and compared to prove it is reproducible, the result passing `pbstore-catalog check-package`; the file name and the files under applications/ stay the same so a manual install keeps working; the release is tagged v<version> with the ZIP attached, the tag equals the manifest version. Out of scope: the store entry in appstore-pocketbook, any change of the app itself. When each release is out, give the owner its URL, who adds its entry as for PB Notes."

## Clarifications

### Session 2026-10-25

- Q: Was the reader's firmware 6.11.1437 when PB Cloud was run on it, so that `6.11.1437` can be the minimum firmware? → A: Yes, use `6.11.1437`; re-read the firmware on the reader during this release's device check and record it.
- Finding while planning (2026-10-25): the contract of `appstore-pocketbook` tag `v0.1.3` rejects the `+` in the bundled time zone names (`GMT+0`, `Etc/GMT+1`, 14 files, rule E-PKG-03); tag `v0.1.4` allows `+` and was made for this reason. The contract and the tool in this spec are therefore `v0.1.4` or newer, not `v0.1.3`. With the `v0.1.4` tool the unchanged 0.6.0 package breaks exactly E-PKG-04 and E-PKG-05.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - The release package is accepted by the PB Store contract check (Priority: P1)

The maintainer builds the release package for the current version. Before changing anything, the
maintainer runs the store's package checker on the package as it is built today, to confirm that
the two known violations are the only ones. After the change, the package carries a manifest at
its top level that describes PB Cloud for the store (identity, name, version, minimum firmware,
target device, menu entry, data folder), and nothing else sits at the top level except the
`applications/` tree. The store's own package checker finds no violation.

**Why this priority**: The store lists an app only if its release package follows the contract.
Today the package is expected to break two rules (E-PKG-04: a stray `INSTALL.md` at the top
level; E-PKG-05: no manifest), so PB Cloud cannot be listed at all. Fixing exactly these two,
and finding out whether anything else is wrong, is the whole point of the feature and is useful
on its own.

**Independent Test**: Run the store's package checker on the package built from the unchanged
code and note its findings. Build the package after the change, run the checker on the resulting
ZIP and verify it reports no violation. Open the ZIP and verify that its top level holds only the
manifest and the `applications/` folder, and that the manifest shows the agreed values and the
version from the `VERSION` file.

**Acceptance Scenarios**:

1. **Given** the package built from the unchanged code, **When** the maintainer runs the store's
   package checker on it, **Then** the findings are recorded; the expectation is exactly E-PKG-04
   and E-PKG-05, and any further finding is added to this feature's scope before work goes on.
2. **Given** a built package, **When** the maintainer runs the store's package checker on it,
   **Then** the checker passes with no error, in particular neither E-PKG-04 nor E-PKG-05.
3. **Given** the `VERSION` file says `0.6.1`, **When** the package is built, **Then** the manifest
   inside the ZIP names version `0.6.1` and the ZIP is named `pbcloud-inkpad-one-0.6.1.zip`.
4. **Given** a built package, **When** its top level is listed, **Then** it contains only the
   manifest and the `applications/` folder.
5. **Given** the manifest values in the repository, **When** the package is built, **Then** the
   manifest states: schema version `1.0`, id `pbcloud`, name `PB Cloud`, minimum firmware
   `6.11.1437`, target device `InkPad One`, menu entry id
   `U_PbCloud`, title `PB Cloud`, launcher `applications/pbcloud.app`, icon
   `applications/icons/pbcloud.bmp`, focused icon `applications/icons/pbcloud_f.bmp`, group
   `@General`, data folder `system/pbcloud`.

---

### User Story 2 - The package is built reproducibly with the store's tool, or fails clearly (Priority: P2)

The maintainer builds the package with the store's own packing tool, so the ZIP is laid out the
way the store expects. Building the same files twice yields the identical ZIP, and the build
verifies this itself. The tool is located through the search path or an environment variable; no
machine-specific location is written into the repository. When the tool cannot be found, the
build stops with a message that says what is missing and how to point the build at it.

**Why this priority**: A package that is only correct by accident of how it was zipped can drift
away from the contract. Using the store's tool and proving reproducibility makes the result
trustworthy and the checksum of the store entry stable. It builds on User Story 1 and is second.

**Independent Test**: Build the package twice from the same files and compare the two ZIPs byte
for byte. Then build once with the tool neither on the search path nor named by the environment
variable and verify the build stops with a clear message and leaves no ZIP behind.

**Acceptance Scenarios**:

1. **Given** the packing tool is on the search path, **When** the package is built twice from
   unchanged files, **Then** both ZIPs are byte-identical and the build reports the
   reproducibility check as passed.
2. **Given** the packing tool is not on the search path but the environment variable names it,
   **When** the package is built, **Then** the build uses that tool and succeeds.
3. **Given** the tool is neither on the search path nor named by the environment variable, **When**
   the package is built, **Then** the build fails before producing a ZIP, and the message states
   that the tool is missing and how to make it available.
4. **Given** the environment variable names something that is not an executable file, **When** the
   package is built, **Then** the build fails and says that the variable is set but unusable,
   rather than falling back silently to another tool.
5. **Given** the two builds in the reproducibility check produce different ZIPs, **When** the check
   runs, **Then** the build fails and says so, and no ZIP is left in the output folder as a valid
   result.

---

### User Story 3 - Manual installation keeps working and its instructions stay reachable (Priority: P2)

A reader who installs by hand (unzip onto the reader's storage, register the app in the
Applications menu) still gets exactly the files under `applications/` they get today, under the
same name, so the manual steps do not change. The install instructions no longer sit at the top
level of the ZIP, but a reader can still find them.

**Why this priority**: Manual installation is how PB Cloud is used today, and the store route is
new and unproven; it must not break the existing one. It has the same priority as the build
guarantees because removing `INSTALL.md` from the top level otherwise strands manual installers.

**Independent Test**: Compare the file list under `applications/` of the new ZIP with that of the
package built from the unchanged code: the same files at the same paths. Starting from the
GitHub release page and from the README, verify a reader can find and follow the install
instructions without any file in the ZIP.

**Acceptance Scenarios**:

1. **Given** the package built before and after the change from the same inputs, **When** their
   `applications/` contents are compared, **Then** the same set of files exists at the same paths
   with the same content.
2. **Given** the new package, **When** a reader unzips it onto the reader's storage as before,
   **Then** the app starts after the usual menu registration, and the registration steps in the
   instructions still match what is needed.
3. **Given** the new package and its GitHub release, **When** a reader looks for install
   instructions, **Then** they find them in the repository (`INSTALL.md`, linked from the README)
   and linked from the notes of the GitHub release; the ZIP itself carries no copy.
4. **Given** the instructions say "Unzip the package and copy the `applications/` folder", **When**
   a reader follows them with the new ZIP, **Then** every step is still true (there is no longer
   an `INSTALL.md` in the ZIP, and the instructions do not point to one).

---

### User Story 4 - The release is tagged and published with the package attached (Priority: P3)

When a version is released, the repository is tagged with the version prefixed by `v`, and the
package is attached to the GitHub release of that tag. The tag, the version in the manifest and
the version in the ZIP name are the same number, so the store can find the right package for a
version without guessing. When the release is out, the maintainer gives the owner its URL so the
owner can add the store entry.

**Why this priority**: The store resolves a version to a release by its tag, so a mismatch makes
the package unfindable or wrong. It comes last because it concerns the act of releasing rather
than the package itself; the first two stories deliver a verifiable package without it.

**Independent Test**: For a release, read the tag name, the manifest version inside the attached
ZIP and the version in the ZIP's file name and verify all three denote the same version. Download
the ZIP from the GitHub release and run the store's package checker on it.

**Acceptance Scenarios**:

1. **Given** version `0.6.1` is released, **When** the release is published, **Then** the tag is
   `v0.6.1` and the ZIP `pbcloud-inkpad-one-0.6.1.zip` is attached to the GitHub release of that
   tag.
2. **Given** a tag `vX.Y.Z` and a package whose manifest names a different version, **When** the
   release is prepared, **Then** the mismatch is detected and the release does not go ahead.
3. **Given** the attached ZIP downloaded from the release, **When** the store's package checker
   runs on it, **Then** it passes.
4. **Given** the release is published, **When** the maintainer reports back, **Then** the report
   contains the URL of the GitHub release (and of the attached ZIP) so the owner can write the
   store entry.

---

### Edge Cases

- The `VERSION` file is missing, empty or not a plain version number: the build stops with a
  message instead of writing a manifest with a bad version.
- The manifest template is missing, or a placeholder in it is left unfilled after the build: the
  build stops instead of producing a package with an incomplete manifest.
- A file that the manual install needs (launcher, program, icons, bundled certificates, emoji
  font, license texts, time zone data) is missing at build time: the build stops with the name of
  the missing input, as it does today.
- The binary does not contain the version from `VERSION`: the build stops, as it does today, so
  the ZIP name and the About screen cannot disagree.
- The optional parts of the package (device probe, hand-off probe, Qt image plugins) are absent
  or present as today: the manifest lists exactly the files that are in the package, whichever
  optional parts a build includes, and the checker is satisfied either way.
- The package is large in file count (the time zone data alone is about 600 files) or contains
  names the contract refuses (characters outside the allowed set, names that differ only by letter
  case, symbolic links, more than 1000 entries, more than 64 MiB): the checker's verdict decides,
  and a violation stops the build with the checker's message.
- A ZIP of the same version already exists in the output folder: it is replaced by the new build,
  never merged with it.
- The store's checker is not installed: the build fails clearly (as for the packing tool); the
  checker's verdict is a required step of the build, not an optional extra.
- The packing tool changes its rules in a later version and the package no longer passes: the
  build fails rather than publishing a package the store will reject.
- The existing size budget of the package (25 MB) still applies and still fails the build when
  exceeded.
- The release for a version is prepared while no tag exists for earlier versions (the repository
  has a tag for 0.5.0 but none for 0.6.0): only the tag of the version being released is created;
  earlier versions are not tagged retroactively by this feature.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The release package MUST contain a manifest file at its top level, and the only
  entries at the top level MUST be that manifest and the `applications/` folder.
- **FR-002**: The manifest MUST be generated at build time from a template kept in the repository,
  not written by hand per release; the template holds every value that does not change between
  releases.
- **FR-003**: The manifest MUST state: schema version `1.0`, app id `pbcloud`, name `PB Cloud`, the
  version read from the `VERSION` file, minimum firmware `6.11.1437`, target device
  `InkPad One`, and one menu entry with id `U_PbCloud`, title `PB Cloud`, launcher path
  `applications/pbcloud.app`, icon path `applications/icons/pbcloud.bmp`, focused icon path
  `applications/icons/pbcloud_f.bmp`, group `@General`, and data folder `system/pbcloud`. The menu
  entry values MUST equal those the install instructions tell a reader to register by hand.
- **FR-004**: The version in the manifest MUST come from the same single source (the `VERSION`
  file) as the ZIP file name and the version compiled into the app; the build MUST NOT let them
  disagree.
- **FR-005**: The install instructions MUST NOT be part of the package. They MUST remain in the
  repository (`INSTALL.md`, linked from the README) and be linked from the notes of each GitHub
  release, so a reader who installs manually can still find them. The instructions MUST NOT
  refer to a file inside the ZIP that no longer exists.
- **FR-006**: The package MUST be assembled with the store's packing tool, and the build MUST
  verify reproducibility by producing the package from the same files twice and requiring
  identical results.
- **FR-007**: The build MUST find the packing tool (and the checker) through the search path or
  through an environment variable, and MUST NOT contain a machine-specific location of it.
- **FR-008**: When the packing tool or the checker cannot be found, or the environment variable
  names something unusable, the build MUST fail before producing a package, with a message that
  says which tool is missing and how to make it available.
- **FR-009**: The build MUST run the store's package checker on the finished ZIP and MUST fail,
  without leaving a ZIP that looks valid in the output folder, if the checker reports any error.
- **FR-010**: The package file name MUST remain `pbcloud-inkpad-one-<version>.zip`.
- **FR-011**: The set of files under `applications/`, their paths and their content MUST stay the
  same as in the package built from the code before this feature, so that unzipping onto the
  reader's storage and registering the app by hand works exactly as before, with nothing added or
  removed. The existing build checks (version present in the binary, every license text present,
  size budget) MUST keep working.
- **FR-012**: A release MUST be tagged `v<version>`, with `<version>` equal to the version in the
  manifest, and the ZIP MUST be attached to the GitHub release of that tag.
- **FR-013**: The release procedure MUST detect and refuse a tag that differs from the manifest
  version, a ZIP whose name differs from it, or a `VERSION` file that differs from it.
- **FR-014**: The project documentation that describes building and releasing the package MUST be
  updated to describe the new steps, the tool lookup and the environment variable; the install
  instructions MUST state where they live now.
- **FR-015**: The minimum firmware in the manifest MUST be the lowest firmware version on which
  PB Cloud has actually been run, and the firmware reading and the date it was observed MUST be
  recorded in the project's device findings. A value that was not observed MUST NOT be claimed.
- **FR-016**: The same mechanical-check practice as for other build rules applies: every new rule
  of the package build or the release check (tool lookup, placeholder left unfilled, version
  mismatch) MUST have a self-test proving that it detects a violation.
- **FR-017**: Nothing in the app itself, nor in the store entry in the `appstore-pocketbook`
  repository, changes as part of this feature. Raising `VERSION` and adding the `CHANGELOG.md` entry
  for the changed release artifact is the only change that touches a product-version file.

### Key Entities

- **Release package**: the ZIP the reader or the store downloads for one version; holds the
  manifest and the `applications/` tree.
- **Package manifest**: the description of the app for the store (identity, version, minimum
  firmware, target device, menu entry, data folder, list of files); generated per build from the
  template, the `VERSION` file and the staged files.
- **Manifest template**: the file in the repository holding the manifest values that do not change
  between releases.
- **Release**: a version tag `v<version>` on the repository together with its GitHub release and
  the attached package, whose URL is handed to the owner.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The store's package checker reports zero errors for the package of the next release
  (today: the expectation is two, E-PKG-04 and E-PKG-05, confirmed by running the checker before
  the change).
- **SC-002**: Building the package twice from the same files yields byte-identical ZIPs in 100% of
  builds, and the build proves this itself every time.
- **SC-003**: The `applications/` contents of the new package are identical in file list, paths and
  content to those of the package built before the change (0 differences); a reader following the
  manual steps needs no step that differs from today's.
- **SC-004**: For every release, tag, manifest version and ZIP name show the same version number
  (0 mismatches), and the package can be downloaded from the GitHub release of that tag.
- **SC-005**: With the packing tool unavailable, the build stops with a message naming the missing
  tool in under 5 seconds and produces no ZIP.
- **SC-006**: The maintainer can go from a finished build of the app to a store-valid package with
  a single command and no hand-edited files.
- **SC-007**: After the release is published, the maintainer hands the owner one URL from which the
  owner can write the store entry without further questions about the package.

## Assumptions

- The sibling feature for PB Notes (`notes-pocketbook`, spec 011) is the model: the same decisions
  are taken here unless the user description says otherwise. In particular, the install
  instructions are not shipped in the ZIP; the repository keeps `INSTALL.md`, linked from the
  README, and the notes of each GitHub release link to it.
- The author's InkPad One is the only tested device. Its firmware is `6.11.1437`: confirmed by the
  owner as the firmware PB Cloud was run on (clarification 2026-10-25) and observed on 2026-10-25
  for PB Notes on the same reader. PB Cloud's own device findings do not yet record a reading, so
  the firmware MUST be re-read on the reader during this release's device check and recorded
  (FR-015); if the reading differs, the observed value is used instead. Other firmware versions
  and device models are untested and not claimed.
- The store's package contract is the one of `appstore-pocketbook` tag `v0.1.4` (see Clarifications:
  `v0.1.3` is too old for the time zone data of this package); a newer contract version is out of
  scope until adopted deliberately.
- The store's packing tool and checker are the same command-line program (`pbstore-catalog`),
  installed by the maintainer on the build machine; installing it is not part of this feature. It
  is not on the search path of the development machine at the time of writing, so the build is run
  with the environment variable (or after the tool is put on the path).
- "The current release ZIP" is the package the existing build produces for version 0.6.0: the
  repository has no published GitHub release or attached ZIP yet. The check before the change ran
  on that package, built locally from the unchanged code (result in `research.md`).
- This feature changes the released artifact, so it raises `VERSION` from `0.6.0` to `0.6.1` and
  adds a `CHANGELOG.md` entry as the last phase of its tasks (the existing version-bump rule). The
  tag for the release is `v0.6.1`, set after the merge, as the changelog already states.
- Publishing the GitHub release remains a deliberate step done by the maintainer after the merge,
  as for earlier releases; this feature provides the correct package and the tag/version check, not
  an unattended publish. The user asked to be given the release URL when it is out.
- The menu entry in the manifest describes the same entry that the manual install registers in the
  reader's desktop configuration (`U_PbCloud`, group `@General`), so the two routes agree.
- The package build keeps its optional parts (device probe, hand-off probe, Qt image plugins) as
  they are; whether a given release includes them is decided by how it is built, not by this
  feature.
- The store entry in `appstore-pocketbook`, PB Notes and PB Office, and any change of the app
  itself are out of scope.
