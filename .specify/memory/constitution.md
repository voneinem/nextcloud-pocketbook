# PB Cloud (nextcloud-pocketbook) Constitution

## Core Principles

### I. Test-First (NON-NEGOTIABLE)
Tests MUST be written before the implementation they cover and MUST be observed failing before the
implementation is written (Red-Green-Refactor), for every feature and bug fix. No change is merged
while any test fails. Behavior at system boundaries - WebDAV (PROPFIND, GET, PUT, MKCOL, MOVE,
DELETE), OCS capabilities, Login Flow v2, HTTP/TLS transport, SQLite storage and schema migrations -
MUST be covered by integration or contract tests that run against an in-process fake Nextcloud
server and anonymized fixtures, never a real server or a real account. Parsers of untrusted input
(207 multistatus XML, OCS JSON, file and folder names, HTTP headers) MUST have tests for malformed,
truncated, oversized, hostile and partial-error input. Hostile input includes path traversal,
control characters, overlong names, Unicode normalization and case collisions, and a multistatus in
which some members failed. A change to a boundary format or contract MUST include a contract test.

Rationale: user files are irreplaceable; failing-first tests prove the tests can detect the defect,
and a fake server makes boundary behavior repeatable without a device or an account.

### II. Host-Testable Core and Layered Architecture
The code is layered as follows, and the dependency direction MUST NOT be reversed:

- `core/` - C++17, Qt-free and InkView-free: HTTP and WebDAV, XML, login, storage, sync.
- `bridge/` - the Qt layer exposing the core to QML.
- `qml/` - the Qt 6 / Qt Quick UI.
- `app/` and `platform/pocketbook/` - device glue (InkView, launcher).

`core/` MUST NOT include Qt, InkView or any SDK-only library, and MUST build and run its complete
test suite on Linux x86-64 with CMake/ctest without the device or the PocketBook SDK. The build
MUST provide both a host target and a device (ARM) target.

Rationale: fast, device-free tests make Test-First practical and keep the core portable.

### III. No Silent Data Loss
User data MUST NOT be lost or silently overwritten, whatever happens to the device (Wi-Fi drop,
sleep, battery death, app kill) or the server.

- A download MUST be written to a temporary file in the same directory as its target and renamed
  atomically only after the size and, where the server offers one, the checksum match. An
  interrupted download MUST leave no partial file visible to the reader's library, and MUST either
  resume (Range, with a validator) or restart cleanly.
- A local file the user changed MUST NOT be overwritten by a download. A file changed on both sides
  keeps both versions, and the user decides; neither version may be discarded automatically.
- Every server write and delete MUST be conditional: `If-Match` with the stored ETag, and
  `If-None-Match: *` for creation. An HTTP 412 or any other detected conflict is a conflict.
- Pending uploads, renames and deletes MUST be persisted before any network operation, MUST NOT be
  discarded, evicted or overwritten until the server has confirmed them, and MUST be idempotent and
  resumable: repeating an interrupted sync produces the same result as an uninterrupted one. An
  upload whose outcome is unknown (for example a timeout after the body was sent) MUST be kept and
  marked uncertain, and MUST NOT be repeated silently.
- Deleting on the server MUST require explicit user confirmation and goes to the server trash.
  Removing the local copy MUST NOT touch the server, and a server deletion MUST NOT remove local
  files that carry unconfirmed local changes.
- Free space MUST be checked before a download. A full or read-only storage MUST be reported to
  the user, never ignored.

Rationale: a last-writer-wins sync, a half-written file in the library, or an automatic repeat of
an upload destroys or duplicates data in ways that cannot be undone.

### IV. Security & Privacy
Files and accounts are private by default. The project MUST collect only the data needed for its
function, MUST NOT transmit user data to third parties, and MUST NOT commit secrets, credentials or
real user data to the repository. All external input MUST be validated before use.

- TLS certificate and host-name verification MUST be enabled for every connection. Disabling or
  weakening verification, including as a development or device workaround, MUST NOT be
  implemented, and no setting, option or flag may trust unverifiable certificates (this includes
  "trust self-signed" options). Trust anchors come from the device CA store when it is usable,
  otherwise from a CA file bundled with the application; a user-supplied `custom-ca.pem` in the
  application data directory is added to them, and Settings MUST show whether a custom CA is
  loaded.
- Only `https://` endpoints are accepted. There MUST be no plaintext fallback and no downgrade on
  failure. Credentials MUST NOT be sent to a different host after a redirect; a redirect to
  another host MUST be refused.
- Only app passwords are used, never the account password. Login Flow v2 tokens and the app
  password it returns are secrets, and the flow's token MUST be discarded after use.
- Names supplied by the server are untrusted. A server-supplied name MUST NOT be used unsanitized
  as a local path: no `..`, no path separators, no characters reserved on the FAT storage, a
  limited length, and a collision-safe result (including case-insensitive collisions).
- Downloaded files are untrusted data. They MUST NOT be executed, and MUST be opened only by the
  reader's viewer that the user chose.
- Thumbnails and previews, if any, MUST come only from the configured server over https, MUST be
  bounded in size and number, and MUST be loaded on demand.
- Secrets (passwords, tokens) MUST NOT appear in logs, error messages shown to the user, or
  exported files. Logs MUST NOT contain server URLs, user names, file or folder names, paths,
  share links or tokens.
- Credential storage MUST be described honestly in the README and the About box. Where credentials
  are stored unencrypted on the device, the user MUST be told so, with the same plaintext-file
  caveat and measurement of the file permissions as in office-pocketbook. Every plan that touches
  credentials MUST evaluate a better-than-plaintext option (restrictive file permissions at
  minimum) and record the decision.

Rationale: a files client holds credentials that grant access to a personal cloud; an unverified
connection, a followed redirect or a server-chosen file name leaks the first or corrupts the
second.

### V. Simplicity & Bounded Footprint
Implement only what the current specification requires (YAGNI). The default dependency set is
libcurl, OpenSSL and SQLite (plus Qt for the UI layer only). A further dependency MUST NOT be added
without a written justification in the plan's complexity-tracking section covering its license,
binary size, ARM cross-compile feasibility and the simpler alternative that was rejected. Dependencies
MUST be pinned and reviewed.

Every plan MUST state measurable budgets for RAM, storage and cold-start time on the InkPad One,
and tests (marked unverified until measured on the device) MUST check them. The footprint of the
data MUST be bounded:

- Folder listings are fetched with `Depth: 1` only and cached only for visited folders.
- The metadata cache is bounded and evicted without touching pending uploads or deletes or
  unconfirmed local changes.
- Transfers stream to disk with a small fixed buffer; no file is held in memory.
- Each plan states the largest supported file size and accounts for the 4 GiB limit of FAT.

Rationale: a small, readable codebase is easier to secure and test, and the device has little
memory, storage and battery.

### VI. E-Ink UX & Battery
The UI MUST be designed for e-ink: black-and-white high contrast, large touch targets, no
animations, page-based lists instead of smooth or kinetic scrolling, minimal screen refreshes, and
operation with the on-screen keyboard. The app MUST NOT poll or sync in the background. Network
activity happens only on app start and on explicit user request. The only further trigger is the
Auto Sync setting, which reacts only to the user's own local change, is switchable, and whose
default each plan states. Progress of long transfers MUST use a coarse refresh budget (few screen
updates). Transfers MUST survive screen-off and Wi-Fi drops. The UI MUST always show the offline
state and the time of the last refresh of the folder shown.

Rationale: e-ink panels ghost and refresh slowly, and background radios drain a battery that is
meant to last weeks.

### VII. Licensing Hygiene
The project license is the MIT License; `LICENSE` states "Copyright (c) 2026 Volker von Einem".
Third-party components MUST be listed in `NOTICE`, in `licenses/manifest.txt` and in the offline
About box, and only licenses compatible with MIT distribution of the binary are allowed (no GPL
code linked or copied). nextcloud/android (GPL-2.0, with AGPL-3.0 contributions), nextcloud/server
(AGPL-3.0) and other Nextcloud client code are reference-only: no code, strings, comments or
documentation text MAY be copied; behavior MAY be reimplemented from the public API documentation,
from RFC 4918, 7232, 7233 and 3986, and from behavior-only descriptions. The Nextcloud name and logo
MUST NOT be used as the app's brand or icon, and the README MUST state that the app is unofficial.
Patterns adapted from InkFeed (MIT), notes-pocketbook and office-pocketbook (MIT, same author) MUST
be attributed in `NOTICE` where required.

Rationale: the binary is distributed to other users; unclear licensing blocks that, copied GPL or
AGPL code would force a license change, and the Nextcloud marks belong to Nextcloud GmbH.

### VIII. Quality Gates
Before any merge `scripts/pre-merge.sh` MUST pass: build, all tests, the secret scan, the
mechanical constitution checks (`scripts/check-constitution.sh`), the fixture check
(`scripts/check-fixtures.sh`), the UI style check (`scripts/check-ui-style.sh`) and the version
bump check. The mechanical checks MUST be extended with every new rule that can be checked by a
script (for example plaintext `http://` URLs, server data or file names in log calls) together with
a self-test proving that the rule detects a violation. Device packages MUST be built with
`scripts/package-device.sh`. The ARM cross-build MUST be reproducible and take the SDK location only
from the `POCKETBOOK_SDK` environment variable or the `-DSDK_PATH` CMake option; SDK or toolchain
paths MUST NOT be hard-coded in build files, scripts or documentation examples.

Rationale: a gate that runs locally and mechanically catches the violations that review misses.

### IX. Specs-Driven Workflow & Honest Verification
Every feature follows the Spec Kit flow: specification, clarification, plan, tasks, analysis,
implementation. The plan's Constitution Check MUST pass, or each deviation MUST be justified,
before design proceeds. Behavior that needs a real Nextcloud server, or the real device, MUST be
labelled "unverified" in specs, tests and fixtures until it has been exercised there; verification
results are recorded when obtained. Hand-written fixtures that imitate server responses carry the
`.unverified` marker.

Rationale: server behavior differs per version, proxy and configuration; claiming verified behavior
that was only simulated misleads the next reader.

### X. English Documentation
All documentation and developer-facing text MUST be written in English: Markdown files (README,
specs, plans, research, tasks, checklists, CHANGELOG, `docs/`), code comments, identifiers, test
names, commit messages, pull request texts, log and error keys, script output and CI messages.
The English text of a user-visible string is its source text (the `qsTr` argument).

Other languages are allowed only for:
- application localization files (for example `qml/strings_de.tsv`) and the translated texts they
  hold;
- test data and expected values that exercise non-English input or locale behavior (for example
  German umlauts, character sets, German file names, German output of a localized function), kept
  as data and explained by an English comment or test name;
- verbatim names and quotations that are not translatable (server folder names, product names).

Rationale: one documentation language keeps the code base readable for every contributor and
reviewer, while localization files and non-English test data are the product's purpose, not its
documentation.

### XI. Consistent Look and Feel
The app MUST look and behave like the sibling apps notes-pocketbook and office-pocketbook, that is
like the built-in look of the reader's own apps:

- It reuses the existing QML component set (TopBar, BottomPanel, IconButton, InkRow,
  ModelPager/PageButtons, dialogs with footer buttons, Style) instead of new look-alikes.
- There are no bordered text buttons and no animations; lists are page-based; tools are icon plus
  label in a bottom panel; separators are hairlines.
- The home, settings and About structure and the Auto Sync wording are the same as in the sibling
  apps.
- Source strings are English, with a German translation in `qml/strings_de.tsv`.
- The README, INSTALL, CHANGELOG and NOTICE follow the conventions of the sibling apps.

`scripts/check-ui-style.sh` enforces what can be checked mechanically. A deviation MUST be made by
amendment of this constitution, never by an undocumented exception in a plan.

Rationale: one owner uses all three apps on one reader; the same controls and wording mean the
user learns them once and the code base shares one component set.

## Security & Data Handling Constraints

- Data on the reader and on the server MUST NOT be deleted or overwritten without explicit user
  confirmation; operations on source data are read-only by default.
- Fixtures MUST be anonymized and use reserved example domains and addresses.
- Content received from a server (XML, JSON, names, headers, file bodies) MUST be size-limited and
  parsed defensively; a malformed item MUST be skipped or shown as unreadable, never crash the app
  or the sync.
- The application data directory, the database file and the credentials file MUST use restrictive
  permissions where the platform allows it, and the real protection MUST be measured and reported.

## Technology Stack & Build Constraints

- The stack is C++17, Qt 6.8 Quick/QML for the UI, CMake, libcurl for networking, OpenSSL for TLS
  and SQLite for storage (see `docs/reference/inkfeed-analysis.md` and
  `docs/reference/pocketbook-sdk-analysis.md`). WebDAV is spoken with libcurl and the existing
  minimal XML parser; no new XML or WebDAV library is added. Changing any of these requires a
  constitution amendment.
- libcurl is built for ARM from source as part of this project, because the reader's own libcurl is
  too old. Whether the bundled libcurl behaves on the device (HTTPS, redirects, ranges, timeouts) MUST
  be verified and recorded before the transfer design is fixed.
- The SDK location comes only from `POCKETBOOK_SDK` or `-DSDK_PATH` (Principle VIII).
- The app name, binary, launcher, data directory and package are `pbcloud`, and they MUST NOT
  collide with PB Notes or PB Office: no shared binary name, launcher entry or data directory.
  The one permitted contact with PB Office is a hand-off of a text by a single `OpenBook2` call to
  its installed launcher. That call exists once in the product, in `app/mail_handoff.cpp`, and is
  made only after the user's confirmed tap. It passes a text and a subject only (never a password
  and never a recipient), and its return value MUST be checked. PB Cloud MUST NOT otherwise start,
  write to or read the data of another application. The name of the PB Office launcher may
  therefore appear in `app/mail_handoff.cpp`, in the hand-off probe source
  (`app/probe/pbc_handoff.cpp`) and in `docs/device-findings.md`, and in no other tracked file
  outside the places listed in `scripts/check-constitution.sh` (rule R11).
- The license is MIT, "Copyright (c) 2026 Volker von Einem" (Principle VII).

## Licensing & Attribution

- Build, launcher and packaging patterns adapted from InkFeed keep their attribution in `NOTICE`.
- The README MUST state the license and that the app is unofficial before the first public push.
- Adding or removing a third-party component requires updating `NOTICE`, `licenses/manifest.txt`
  and the About box in the same change.

## Development Workflow

- Every change goes through review; the reviewer MUST verify compliance with this constitution and
  MUST reject TLS-verification bypasses, plaintext fallbacks, hard-coded SDK paths, unconditional
  server writes, unsanitized server-supplied names, code copied from GPL or AGPL projects, a
  deviation from the sibling look and feel, and documentation outside English (Principle X).
- Branches follow the gitflow convention in `.specify/branch-convention.yml`
  (`feature/NNN-name`); bugs are triaged with the bug extension (`.specify/bugs/<slug>/`).
- Test gates: all tests pass and no new secrets are detected before merge (Principle VIII).
- Release version: every merged change to the product raises `VERSION` (MINOR for a feature, PATCH
  for a fix, MAJOR for a breaking change such as an irreversible data upgrade) and adds an entry to
  `CHANGELOG.md`. The first release version is 0.1.0. The bump is a task of the feature (last phase
  of `tasks.md`) and is enforced by `scripts/check-version-bump.sh` in `scripts/pre-merge.sh`. After
  the merge the release is tagged `v<version>`. Changes nobody can see (refactoring) may opt out
  with the variable documented in `scripts/check-version-bump.sh`, stated in the pull request.
- Database schema upgrades are one-way and MUST be tested; an older build MUST refuse a newer
  database rather than damage it.

## Governance

This constitution supersedes other project practices. Amendments MUST be made by editing this file
with a documented rationale and a version bump, and MUST include any migration steps needed for
existing specs, plans or code. A principle may be relaxed only by amendment, never by an
undocumented exception in a plan.

Versioning follows semantic versioning: MAJOR for backward-incompatible removal or redefinition of
principles or governance; MINOR for a new principle or section or materially expanded guidance;
PATCH for clarifications and wording fixes.

Compliance is reviewed in every pull request and in a mandatory Constitution Check in every plan,
which lists each principle with pass, justified deviation or not applicable. Unjustified violations
block the merge.

**Version**: 1.2.0 | **Ratified**: 2026-10-19 | **Last Amended**: 2026-10-24
