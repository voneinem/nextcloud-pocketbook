---

description: "Task list for the read-only Nextcloud files client"
---

# Tasks: Read-only Nextcloud Files Client

**Input**: Design documents from `/specs/001-nextcloud-files-client/`

**Prerequisites**: [plan.md](plan.md), [spec.md](spec.md), [research.md](research.md), [data-model.md](data-model.md),
[contracts/server-requests.md](contracts/server-requests.md), [contracts/core-interfaces.md](contracts/core-interfaces.md),
[contracts/bridge-qml.md](contracts/bridge-qml.md), [quickstart.md](quickstart.md)

**Tests**: REQUIRED. The constitution makes Test-First non-negotiable (Principle I): every implementation task is preceded
by test tasks, and each story has a "red" task that records the new tests failing for the right reason before any
implementation task of that story starts. Tests that use the fake Nextcloud define `PBC_FAKE_SERVER_TEST`
(`scripts/check-fake-markers.sh`). Hand-written server answers carry the `.unverified` suffix (Principle IX).

**Organization**: Tasks are grouped by user story (US1–US6 of the spec) so each story can be implemented, tested and
stopped independently. Priorities: US1–US3 are P1 (the minimum product), US4–US5 are P2, US6 is P3.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: can run in parallel (different files, no dependency on an unfinished task)
- **[Story]**: US1–US6, only in the user-story phases
- Paths are relative to the repository root. `core/include/pbc/X.h` and `core/src/X.cpp` come as a pair; every new
  `core/src/*.cpp` is added to `PBC_CORE_SOURCES` in `core/CMakeLists.txt` in the same task, every new bridge file to
  `bridge/CMakeLists.txt`, every new QML file to `qml/qmldir` (components) or `qml/qml.qrc` (pages).
- Every task that touches a user-visible text also adds the German entry to `qml/strings_de.tsv`
  (`scripts/extract-strings.py --check` must stay green).
- FR-/SC- numbers refer to [spec.md](spec.md); R- numbers to [research.md](research.md).

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: confirm the baseline and prepare the places where new fixtures, checklists and tools go.

- [X] T001 Build the host target and run the whole suite to confirm the baseline is green before any change: `cmake -S . -B build-host -G Ninja && cmake --build build-host && ctest --test-dir build-host --output-on-failure`; write nothing, stop and report if any test fails.
- [X] T002 [P] Create `tests/fixtures/nextcloud/` with subfolders `status/`, `ocs/`, `propfind/`, `report/`, `search/`, `login/` (each with a `.gitkeep`) and `tests/fixtures/nextcloud/README.md` stating: every file imitating a server answer ends in `.unverified` until a live run produced it (Principle IX, `docs/live-server-checklist.md`), uses only reserved example names (`example.org`, user `demo`), and is anonymized.
- [X] T003 [P] Add the items LV-N1 to LV-N9 to `docs/live-server-checklist.md` (all status **U**): LV-N1 status.php and capabilities fields and the version on Nextcloud 30 and a current major; LV-N2 the answer to Basic auth with an account password on a two-factor account (is there a readable "app password" marker?); LV-N3 quota properties and the sentinel values for unlimited/unknown; LV-N4 Login Flow v2 end to end (poll 404 until approved, 200 once, same-origin endpoints); LV-N5 availability markers (end-to-end encrypted folder, share without read permission, unmounted external storage, `oc:permissions` letters) in a depth-1 listing; LV-N6 `Range` and `If-Range` on file GET (206, 200 on ETag change, 416); LV-N7 folder ETag propagation after a change three levels down (informs a later optimization, not relied on); LV-N8 the favorites `REPORT` body; LV-N9 the `SEARCH` body and `d:like` behavior with `%`, `_` and `\`.
- [X] T004 [P] Add the items DT-NC-1 to DT-NC-10 to `docs/device-test-checklist.md` (all status **U**): DT-NC-1 bundled libcurl on the device (HTTPS, same-host redirect, Range, progress callback, 64 KiB buffer); DT-NC-2 credentials file mode and location and the in-app storage text; DT-NC-3 the download folder `PBCloud` appears in the reader's library and a `.pbc-*.part` file never does; DT-NC-4 speed of the checksum pass on a 500 MB file; DT-NC-5 opening a book in the viewer does not change size or mtime of the file; DT-NC-6 source of the reader's time zone (`global.cfg` key) and the UTC fallback; DT-NC-7 cold start ≤ 5 s, peak `VmHWM` ≤ 150 MB, 1,000-entry cached folder shown ≤ 2 s, page turn ≤ 1 s, first page of an uncached 1,000-entry folder over home Wi-Fi ≤ 10 s; DT-NC-8 Wi-Fi drop and return during a 500 MB download; DT-NC-9 refusal of a file of 4 GiB or more before any byte is written; DT-NC-10 number of e-ink refreshes during a download (coarse progress).
- [X] T005 [P] Run `scripts/check-fixtures.sh` on a throw-away sample `tests/fixtures/nextcloud/status/sample.json.unverified`; if the checker rejects the new directory or the reserved names, extend `scripts/check-fixtures.sh` (and its `--self-test`) so that `example.org`/`example.com` hosts and the `.unverified` suffix in `tests/fixtures/nextcloud/` are accepted and a real-looking host is still rejected; delete the sample afterwards.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: schema 2, transport additions, path utilities, error keys, the fake server skeleton and the new mechanical
checks. No user story starts before this phase is complete.

**⚠️ CRITICAL**: US1–US6 all depend on T006–T020.

- [X] T006 [P] Write `tests/unit/test_store_schema2.cpp` (red first): a fresh database is schema 2 with the tables of [data-model.md](data-model.md); an existing schema-1 file with settings and a `sync_state` row upgrades in one transaction after a backup copy and keeps both; schema 3 is refused with `schema_too_new` and left byte-identical; a failing migration is rolled back and the backup kept; the integrity check runs after the upgrade; CHECK constraints reject `account.id != 1`, `account.state` outside `('untested','ok','needs_signin','problem')`, `entries.kind` outside `('file','folder')`, `entries.availability` outside `('ok','no_permission','encrypted','unsupported','unreadable')`, `offline_marks.kind` outside `('file','folder')`, `downloads.state` outside `('running','waiting','paused')`; deleting a `folders` row cascades to its `entries`.
- [X] T007 Implement schema 2: add the tables `account`, `folders`, `entries`, `local_files`, `offline_marks`, `favorites`, `downloads` exactly as in [data-model.md](data-model.md) (all constraints and defaults quoted there, `PRIMARY KEY (parent, name)` on `entries`, `local_files.local_path UNIQUE`, `downloads.id INTEGER PRIMARY KEY AUTOINCREMENT`) as the 1→2 step in `core/src/store_migrations.cpp`, document the new tables in `core/src/store_schema.h`, and set `kSchemaVersion = 2` in `core/include/pbc/store.h`; `tests/unit/test_store_schema2.cpp` and the existing `tests/unit/test_store_migrations.cpp` / `test_store_open.cpp` must pass.
- [X] T008 [P] Write `tests/integration/test_transport_extensions.cpp` (red first, real `CurlTransport` against `tests/support/tls_test_server.h`): `save_append` appends to an existing file and truncates when false; `progress` is called at least once per second and on 64 KiB steps, and returning false stops the transfer with `Response::cancelled` and leaves the partial file as written so far; `identity_encoding` sends `Accept-Encoding: identity`; `buffer_size` is accepted; `max_body_bytes` counts `resume_from` plus received bytes; a redirect to another host is refused and credentials are not forwarded; a 4xx body does not create the file.
- [X] T009 Implement the transport additions per [contracts/core-interfaces.md](contracts/core-interfaces.md): fields `save_append`, `resume_from`, `progress`, `buffer_size`, `identity_encoding` on `Request` and `cancelled` on `Response` in `core/include/pbc/http_transport.h`; in `core/src/curl_transport.cpp` open the sink with `"ab"` when `save_append`, wire `CURLOPT_XFERINFOFUNCTION`/`CURLOPT_NOPROGRESS`, `CURLOPT_BUFFERSIZE`, and `Accept-Encoding: identity` instead of the empty `CURLOPT_ACCEPT_ENCODING` when `identity_encoding`; make `tests/support/fake_transport.h` honor `save_append` and `progress` where it writes a file; `tests/integration/test_transport_extensions.cpp` and all existing transport tests pass.
- [X] T010 [P] Write `tests/unit/test_remote_path.cpp` (red first): `normalize_remote_path` (`""`, `"/"`, `"a"`, `"//a///b/"`), `parent_of`, `join_remote`, `encode_path` (space as `%20`, never `+`, UTF-8 bytes, `/` kept, no Unicode normalization), `decode_href` (strips `/remote.php/dav/files/<uid>`, rejects an href outside the prefix, a prefix with another user id, `..` segments, invalid percent escapes, an embedded NUL, a trailing slash handled), `is_valid_name` rejecting empty, `.`, `..`, a name containing `/`, control characters, and names over 1,024 bytes.
- [X] T011 Implement `core/include/pbc/remote_path.h` and `core/src/remote_path.cpp` per [contracts/core-interfaces.md](contracts/core-interfaces.md) so `tests/unit/test_remote_path.cpp` passes; names are never normalized; add to `PBC_CORE_SOURCES`.
- [X] T012 [P] Write the test cases first in `tests/unit/test_dav_common.cpp` (red): reading an `oc:size` and an `nc:is-encrypted` property from a multistatus through `prop_in_ok_propstat`; `DavSession::status_error` maps 423 → `locked`, 429 → `rate_limited`, 503 → `maintenance`, 401 → `auth_failed`, 403 → `forbidden`, 404 → `not_found`, other 5xx → `server_error` (today 429 maps to `server_error`); `parse_retry_after` accepts delta-seconds and an HTTP date, caps at 900 s, and returns nullopt for garbage, negative numbers and a missing header. Then add `kNsOc = "http://owncloud.org/ns"` and `kNsNc = "http://nextcloud.org/ns"` to `core/include/pbc/dav_common.h`, `parse_retry_after`, and the new mappings in `core/src/dav_common.cpp` per [contracts/core-interfaces.md](contracts/core-interfaces.md) so the cases pass.
- [X] T013 [P] Write `tests/unit/test_error_keys.cpp` (red first): every key listed under "Error keys" in [contracts/server-requests.md](contracts/server-requests.md) exists once in `all_error_keys()`, maps to one `ErrorClass`, has no duplicate, and `error_from_key("x")` for an unknown key returns `protocol_error`.
- [X] T014 Implement `core/include/pbc/error_keys.h` and `core/src/error_keys.cpp` (the stable key list of [contracts/server-requests.md](contracts/server-requests.md), `all_error_keys()`, `class_of(key)`, `error_from_key`) so `tests/unit/test_error_keys.cpp` passes; add to `PBC_CORE_SOURCES`.
- [X] T015 [P] Write `tests/unit/test_fake_nextcloud.cpp` (red first, `PBC_FAKE_SERVER_TEST`): the fake serves `GET /status.php`, `GET /ocs/v1.php/cloud/capabilities` and `.../cloud/user` with Basic auth (401 for wrong credentials), `PROPFIND` depth 0 and 1 over a configurable in-memory tree with ETags, sizes, `oc:fileid`, `oc:permissions`, `oc:favorite`, `GET` of a file with `ETag` and `Content-Length`, a request counter per method, and refuses `Depth: infinity` with 403.
- [X] T016 Implement `tests/support/fake_nextcloud_server.h` (an `HttpTransport` with an in-memory tree, user `demo`, per-request log without secrets, failure injection hooks for status codes and delays reserved for later stories) so `tests/unit/test_fake_nextcloud.cpp` passes; it implements [contracts/server-requests.md](contracts/server-requests.md) sections 1–5 and 6 (full GET only for now) and nothing else.
- [X] T017 [P] Add red self-test samples to `scripts/check-constitution.sh --self-test` for the new rules, each of which must be detected: N1 an `http://` URL literal in `core/src/x.cpp` (and a clean sample with an `xmlns:oc="http://owncloud.org/ns"` text and a `kNsOc` constant that must pass); N2 a request with method `"PUT"`/`"MKCOL"`/`"MOVE"`/`"COPY"`/`"DELETE"`/`"PROPPATCH"`/`"LOCK"`/`"UNLOCK"` in `core/src/nc_x.cpp`, `core/src/downloader.cpp`, `core/src/update.cpp`, `bridge/src` or `app/` (and a clean sample: `request.method == "PUT"` in `core/src/curl_transport.cpp`, which is excluded); N3 `QTimer`, `QBasicTimer` or `sleep_for` in `bridge/src` or in `core/src` outside `downloader.cpp`; N4 a `log::text("name", …)`/`"href"`/`"term"`/`"url"`/`"user"`/`"login"` field; N5 `unlink(`, `remove(`, `rename(` or `std::filesystem::remove` in `core/src` outside `local_files.cpp`, `downloader.cpp`, `credentials_file.cpp`, `store*.cpp`; plus a clean sample for each that must pass.
- [X] T018 Implement rules N1–N5 in `scripts/check-constitution.sh` as specified in [research.md](research.md) R-16 (comments and `tests/`, `docs/`, `third_party/` excluded for N1; XML namespace URIs on the explicit exempt list for N1; the generic transport files excluded for N2) so that the self-test and `check_constitution` pass on the current tree, including `core/include/pbc/dav_common.h` and `core/src/curl_transport.cpp`.
- [X] T019 [P] Add the event codes of R-15 (`account.test`, `listing.refresh`, `download.start`, `download.done`, `download.cancel`, `download.fail`, `download.resume`, `update.run`, `search.server`, `cache.evict`, `space.remove`) to `core/src/log_codes.h` and a case in `tests/unit/test_log.cpp` proving that a field named `name`, `href`, `term`, `url`, `user`, `login`, `path` of a remote entry or `token` is redacted or refused. (Implementation note: `tests/unit/test_log.cpp` fails when a listed code is unused, so each code is added to `kAllowedCodes` by the task that first logs it; T019 itself extended the redaction in `core/src/log.cpp`.)
- [X] T020 Update `core/CMakeLists.txt`: link `pbc_core` to `OpenSSL::Crypto` on the host (`find_package(OpenSSL REQUIRED)`), and on the ARM target rely on the `libcrypto.so` already named in `cmake/curl_arm.cmake` (add it explicitly to `target_link_libraries` if the link needs it); keep `core/` free of Qt, InkView and SDK headers (rule R3).

**Checkpoint**: schema 2, transport additions, path utilities, error keys, fake skeleton and checks are green; user stories can start.

---

## Phase 3: User Story 1 - Connect to my Nextcloud (Priority: P1) 🎯 MVP

**Goal**: the user connects with address, user name and app password (Test shows server name, version, quota) or with the
browser login (link and QR code, tap Check); every named problem has its own actionable message; the app password is
stored honestly and the account password never.

**Independent Test**: against the fake Nextcloud, enter address, `demo` and the app password, tap Test and read the
result; then start the browser login, approve it in the fake, tap Check and see the account connected; try `http://`,
a wrong password, maintenance, an old version and an untrusted certificate and read the message of each.

### Tests for User Story 1 (write first, they must fail) ⚠️

- [X] T021 [P] [US1] `tests/unit/test_canonical_url.cpp`: `canonical_base_url` accepts `https://host`, `https://host:8443/sub/path/`, and a bare `host` or `host:port` (completed with `https://`); strips one trailing slash; refuses `http://…`, `ftp://…`, `file://…`, any other scheme, an address with user information (`https://user:pw@host`), a query or fragment, an empty host, blanks or control characters, and a host longer than 253 bytes, each with the key `scheme_not_allowed` or `address_invalid` (FR-004).
- [X] T022 [P] [US1] `tests/unit/test_server_status.cpp`: `classify_status_document` for a valid document, not JSON, JSON without `installed`, `installed:false`, `maintenance:true`, `needsDbUpgrade:true`, major version below 30 (`server_too_old`), a body over 64 KiB, `version` with hostile characters (sanitized to digits and dots, 32 characters) and a `productname` with control characters or over 64 characters (FR-006, R-3).
- [X] T023 [P] [US1] `tests/integration/test_account_check.cpp` (`PBC_FAKE_SERVER_TEST`, fixtures under `tests/fixtures/nextcloud/ocs/*.unverified`): success shows server name, version and quota; wrong credentials → `auth_failed`; an OCS message containing `app password` → `auth_app_password_required`; 503 and `maintenance:true` → `maintenance`; DAV 404/405/501 → `no_webdav`; a non-Nextcloud answer → `not_nextcloud`; transport errors `cert_untrusted`, `cert_expired`, `cert_host_mismatch`, `server_unreachable`, `no_network` pass through; `clock_implausible` when the injected `Clock` is before the build epoch, checked before any request; the HTTP `Date` header sets `clock_skew` and above 600 s the warning minutes; quota: available ≥ 0 gives total, `-3` unlimited, other negatives unknown; user id with `/`, `..` or control characters → `protocol_error`; 429 with `Retry-After`; two consecutive 401 set the account state `needs_signin` and no third request is sent (R-12); the typed app password never appears in any log line.
- [X] T024 [P] [US1] `tests/integration/test_login_flow.cpp` (`PBC_FAKE_SERVER_TEST`): `login_start` returns link, endpoint and token; a `login` or `poll.endpoint` on another host, port or scheme → `redirect_other_host`; a token over 512 characters or with non-printable characters → `protocol_error`; `login_check` sends exactly one request per call; 404 → pending and the flow stays usable; 200 returns the credentials once and `server` on another origin → `redirect_other_host`; any other status → `login_failed`; `login_discard` overwrites the token; no log line contains the token; the same flow object cannot be checked after discard.
- [X] T025 [P] [US1] `tests/unit/test_store_account.cpp`: `save_account`/`account`/`clear_account` round trip; only one row (`id = 1`); `base_url` must be https; `user_id` empty or containing `/`, control characters or `..` is rejected; `server_name` longer than 64 characters is cut; sign-out order: `forget_credentials` first, then `clear_account`, and a failure of the first leaves the row (Principle IV).
- [X] T026 [P] [US1] `tests/unit/test_qr_code.cpp`: generate reference matrices for at least six inputs (a short URL, a 120-character URL, a 200-character URL, an input at version boundaries, non-ASCII UTF-8, the 367-byte limit) with an independent encoder available on the development machine (for example the `qrencode` CLI or Python `segno`), store them as `tests/fixtures/qr/*.txt` with a header line naming the tool and version, and assert the app's matrix is bit-identical; 368 bytes → `qr_too_long`; the finder patterns, timing patterns and format information are checked separately (R-5).
- [X] T027 [P] [US1] `tests/bridge/test_connection_bridge.cpp` (Qt Test, offscreen): `testConnection` reports the state keys, clears the typed password from the bridge after use, saves nothing when the Test fails; `startLogin`/`checkLogin`/`cancelLogin` states `approved`, `pending`, `expired`, `failed`; `checkAtStart` sends nothing when `wifiConnected()` is false and exactly one request when true; `signOut` removes the password file before the account row; `credentialsProtection` reports the measured value; no network call is made by constructing the bridge or reading a property.
- [X] T028 [US1] Extend `tests/support/fake_nextcloud_server.h` with what T023/T024 need and nothing more: OCS error variants (401 with and without the app-password message, 429 with `Retry-After`, 503 maintenance), quota property variants, `POST /index.php/login/v2` and the poll endpoint with an `approve()` control, same-origin and cross-origin variants, `Date` headers, a hook to inject transport errors by key.
- [X] T029 [US1] Red run: build and run T021–T027; record in the commit message or a note that each new test fails for the intended reason (missing symbol or wrong result), not because of a test bug; do not start T030 before this is done (Principle I).

### Implementation for User Story 1

- [X] T030 [US1] Implement `canonical_base_url`, `classify_status_document` and `check_account` in `core/include/pbc/nc_server.h` and `core/src/nc_server.cpp` per [contracts/core-interfaces.md](contracts/core-interfaces.md) and [contracts/server-requests.md](contracts/server-requests.md) sections 1–4 (step order of R-3, sanitizing of version and name, quota sentinels, user id validation, `Date` skew via `clock_check.h`, `Retry-After` through `parse_retry_after` for a 429 or 503 answer, local `clock_implausible` test using a build epoch added to `core/src/version.cpp`); add to `PBC_CORE_SOURCES`; `test_canonical_url`, `test_server_status`, `test_account_check` pass.
- [X] T031 [US1] Implement the account methods (`account`, `save_account`, `clear_account`, `set_account_state`) in `core/include/pbc/store_files.h` and `core/src/store_files.cpp`, with the validation of [data-model.md](data-model.md) (`base_url` https only, `user_id` non-empty without `/`, control characters or `..`, `server_name` at most 64 characters); add `core/src/store_files.cpp` to `PBC_CORE_SOURCES`; `tests/unit/test_store_account.cpp` passes.
- [X] T032 [US1] Implement the consecutive-401 rule (R-12) as `core/include/pbc/auth_guard.h` and `core/src/auth_guard.cpp`: a counter kept in `account.state`/`problem_key`, `blocked()` true after two consecutive `auth_failed` until the user tests again, reset on any success; `check_account` and later fetchers consult it before sending; covered by the `needs_signin` cases of `tests/integration/test_account_check.cpp`.
- [X] T033 [US1] Implement `login_start`, `login_check` and `login_discard` in `core/include/pbc/nc_login.h` and `core/src/nc_login.cpp` per [contracts/server-requests.md](contracts/server-requests.md) section 9 (same-origin checks, token held in memory only and overwritten on discard, exactly one request per check); add to `PBC_CORE_SOURCES`; `tests/integration/test_login_flow.cpp` passes.
- [X] T034 [US1] Implement the QR encoder `qr_encode` in `core/include/pbc/qr_code.h` and `core/src/qr_code.cpp` (byte mode, versions 1–12, error correction L, mask by the penalty rule, `qr_too_long` above 367 bytes); add to `PBC_CORE_SOURCES`; `tests/unit/test_qr_code.cpp` passes. This task and T037 are the separable QR cut line of R-5.
- [X] T035 [US1] Implement `bridge/include/connection_bridge.h` and `bridge/src/connection_bridge.cpp` per [contracts/bridge-qml.md](contracts/bridge-qml.md): properties `state`, `serverName`, `serverVersion`, `quotaText`, `checkedAt`, `problemKey`, `clockWarningMinutes`, `busy`, `loginActive`, `loginUrl`, `loginExpiresInMinutes`, `credentialsProtection`; invokables `testConnection`, `startLogin`, `checkLogin`, `cancelLogin`, `signOut`, `checkAtStart`; all requests on the `Worker`; `device.connectNetwork()` is called first for every tap-started action (never for `checkAtStart`); the typed password is cleared after use; credentials saved with `save_credentials` then the account row; register in `bridge/CMakeLists.txt` (`PBC_BRIDGE_SOURCES`, `PBC_BRIDGE_HEADERS`, `PBC_BRIDGE_MOC_HEADERS`); `tests/bridge/test_connection_bridge.cpp` passes (add it to the list in `tests/CMakeLists.txt`).
- [X] T036 [US1] Add the QR image provider `bridge/include/qr_image_provider.h` and `bridge/src/qr_image_provider.cpp` (a `QQuickImageProvider` rendering the matrix of `connection.loginUrl` black and white at an integer scale with a 4-module quiet zone, one image per login) and register it in `app/main.cpp` as `image://qr/`.
- [X] T037 [US1] Add `qml/pages/LoginPage.qml` (link as large selectable text in groups, the QR image, minutes left, **Check** and **Cancel** tools in a bottom panel, the answers `pending`/`expired`/`failed` as plain text) built only from existing components; register in `qml/qml.qrc`.
- [X] T038 [US1] Rework `qml/pages/SettingsPage.qml` into the account section of [contracts/bridge-qml.md](contracts/bridge-qml.md): `InkField`s for address, user name and app password (password masked), **Test**, **Log in with a browser**, **Sign out** (with `ConfirmDialog`), a control for the large-file warning size (setting `large_file_warn_mib`, whole MiB from 1 to 4095, default 100), the result block (server name, version, quota, last check time), the customer-visible problem message with its "what to do" line for each key of FR-006 (wrong credentials, two-factor/app password, maintenance, too old, no file access, not a Nextcloud, untrusted certificate with the `custom-ca.pem` explanation and the loaded/not loaded status, wrong clock), and the clock warning line; no Auto Sync switch (plan, Principle VI note).
- [X] T039 [US1] Write the honest credential-storage texts (FR-007, R-6) in `qml/pages/SettingsPage.qml` and `qml/pages/AboutPage.qml`: the app password is kept as plain text in a file in the app's data folder, this storage cannot restrict access to it (or "is restricted to this user" when `credentialsProtection` is `enforced`), anyone with the storage can use it until it is revoked in Nextcloud under Security, the account password is never stored; plus the unofficial-app notice; both in English and German.
- [X] T040 [US1] Update `qml/pages/HomePage.qml`: the state line (`connection.state`: "Connected as … , checked 14:32", the problem message, "Not connected", or "Offline, not checked") built from `connection`, same structure as the sibling apps' home page (compare with the start pages of the sibling apps notes-pocketbook and office-pocketbook before editing); the tools of later stories are added in their own phases.
- [X] T041 [US1] Add all US1 texts (every problem message and "what to do" line of the keys in [contracts/server-requests.md](contracts/server-requests.md), the Settings and Login labels, the storage texts) to `qml/strings_de.tsv` and run `python3 scripts/extract-strings.py --check`.
- [X] T042 [US1] Wire `app/main.cpp`: create `ConnectionBridge`, register context property `connection` and the image provider, call `checkAtStart()` once after the window is shown, only when `device.wifiConnected()`; add `tests/integration/test_no_background_network.cpp` (`PBC_FAKE_SERVER_TEST`) with the first cases: Wi-Fi on and an account → exactly one request (the capabilities call) and then none; Wi-Fi off → none; no account → none (FR-034, SC-010).
- [X] T043 [US1] Run the US1 acceptance walk with the fake server (quickstart section 3, step 1) and the full suite; fix only what the tests or the walk show; confirm that `check_constitution`, `check_ui_style`, `check_translations` and `check_fake_markers` pass. (Done with the automated tests of every acceptance scenario and the QML self-test; the interactive walk through the demo server is repeated in T129 once T118 provides it.)

**Checkpoint**: US1 works alone: a verified connection, all named problems, honest storage text.

---

## Phase 4: User Story 2 - Browse my files (Priority: P1)

**Goal**: folders and files are listed page by page from a bounded cache, sorted, with a path line and up navigation;
visited folders stay readable offline with the age of the listing; refresh is explicit; unavailable entries are shown
as such.

**Independent Test**: against the fake with a tree of folders and files, open the root and a subfolder, go up, change
the sort, switch the fake off and reopen the visited folder from the cache; the encrypted and no-permission entries are
marked and harmless.

### Tests for User Story 2 (write first, they must fail) ⚠️

- [X] T044 [P] [US2] `tests/unit/test_listing_parse.cpp` with fixtures in `tests/fixtures/nextcloud/propfind/*.xml.unverified`: a normal depth-1 answer (the folder itself dropped by path comparison, not by position; `oc:size` preferred over `getcontentlength`; ETag stored with quotes and `W/`; `getlastmodified` parsed with `parse_http_date`, NULL when unparsable); foreign hrefs counted as `skipped_foreign`; names rejected by `is_valid_name` counted as `skipped_hostile`; a response without a 2xx `propstat` → `availability = unreadable`, `reason = status_<code>`; `nc:is-encrypted` = `1` → `encrypted`; `oc:permissions` present without `G` → `no_permission`; a 2xx `propstat` lacking both `getetag` and `resourcetype` → `unsupported` with `reason = unsupported_state`; a 404 `propstat` for one property is not an error; a partial answer sets `partial`; exactly 10,000 entries parse and 10,001 give `listing_too_large`; `mime` cut at 100 characters and `checksum` at 256 bytes.
- [X] T045 [P] [US2] `tests/unit/test_listing_hostile.cpp` (hostile and malformed input, Principle I): truncated XML, two roots, a DOCTYPE/billion-laughs file from `tests/fixtures/malformed/xml/`, `multistatus_half_valid.xml`, 8 MiB+1 body, depth 65 nesting, names with `..`, `/`, NUL, control characters, bidi overrides, 4,000-byte names, NFC/NFD pairs and case-only twins, hrefs with invalid percent escapes; every input gives a result or an error key, never a crash or a hang (run also in the `PBC_SANITIZE` build).
- [X] T046 [P] [US2] `tests/unit/test_store_listing.cpp`: `replace_listing` replaces a folder's rows in one transaction and sets `fetched_at`; a failed refresh keeps old rows and the old `fetched_at`; `mark_folder_gone` sets `gone = 1` and keeps rows; `entries_page` sorts by name with `pbc_fold` and a byte tie-break, by date, by size, ascending and descending, folders always first, with `offset`/`limit` and `total`; `folder()` updates `last_access_at`; eviction (50,000 entries, 2,000 folders) removes the oldest `last_access_at` first, never the root and never the folder being written, in the same transaction, and leaves `local_files`, `offline_marks`, `favorites` and `downloads` untouched; entries violating the name rules are not stored.
- [X] T047 [P] [US2] `tests/unit/test_age_text.cpp`: `listing_age(now, fetched_at, gone)` returns keys and counts for "just now", minutes, hours, days, "not synced" (NULL `fetched_at`), "removed on the server"; a `fetched_at` in the future (wrong clock) is treated as "just now" and flagged.
- [X] T048 [P] [US2] `tests/unit/test_date_text.cpp`: `reader_zone_from_config` reads `timezone=<name>` from a sample `global.cfg` (fixture marked unverified), falls back to UTC with the suffix `UTC` when the key or the zone data is missing; the date `YYYY-MM-DD` and the date with time are formatted through `tz.h` for a summer and a winter date in `Europe/Berlin` and `America/New_York` (R-14).
- [X] T049 [P] [US2] `tests/integration/test_browse_cache.cpp` (`PBC_FAKE_SERVER_TEST`): opening an uncached folder fetches it once with `Depth: 1` and never `infinity`; a second open uses the cache and sends nothing; with the fake switched off the visited folder is served from the cache with its age; a refresh that fails (503, drop, 401) keeps the old rows; a 404 on refresh marks the folder gone and keeps it readable; a refresh after a server-side change replaces the rows; a never-visited folder offline gives "not available offline"; encrypted, no-permission and unsupported entries are returned marked and the rest is intact (FR-009 to FR-014).
- [X] T050 [P] [US2] `tests/bridge/test_browse_bridge.cpp` (Qt Test, offscreen): `rows` holds one page only for the given `pageSize`; `nextPage`/`previousPage`/`goToPage` are local and send no request; `setSort` persists `sort_key`/`sort_dir`; `up()` and `open()` on a cached folder send nothing; `refresh()` is the only call that fetches a shown folder; a tap on an unavailable entry does not navigate; names passed to QML are `displaySafe()`.
- [X] T051 [P] [US2] `tests/integration/test_budget_cache.cpp`: build a cache of 1,000 entries (and one of 50,000 entries over many folders), measure time to open a cached folder and to fetch a page, database file size at the cache limits; also time parsing and storing a 1,000-entry `PROPFIND` answer from the fake (the host part of SC-005; the 10 s end-to-end figure is measured on the device, DT-NC-7); assert ≤ 2 s, ≤ 1 s per page and ≤ 50 MB on the host and print the figures; mark the test and its output "unverified until measured on the device" (SC-004, SC-014).
- [X] T052 [US2] Extend `tests/support/fake_nextcloud_server.h` for listings: configurable availability markers (`nc:is-encrypted`, permissions without `G`, a response without 2xx propstat), foreign hrefs, a folder with 10,000/10,001 entries, partial multistatus, malformed bodies, a switch that makes the whole server unreachable, a counter of `Depth` headers.
- [X] T053 [US2] Red run: build and run T044–T051; record that each fails for the intended reason; do not start T054 before.

### Implementation for User Story 2

- [X] T054 [US2] Implement `listing_request_body`, `parse_listing` and `fetch_listing` in `core/include/pbc/nc_listing.h` and `core/src/nc_listing.cpp` per [contracts/server-requests.md](contracts/server-requests.md) section 5 (every parsing rule 1–8, `Depth: 1` always, never `infinity`, 10,000-entry cap, error mapping 404 → gone, 401, 403, 429, 503, 5xx); add to `PBC_CORE_SOURCES`; `test_listing_parse`, `test_listing_hostile` pass.
- [X] T055 [US2] Implement the listing methods of `Store` in `core/src/store_files.cpp` / `core/include/pbc/store_files.h`: `replace_listing` (one transaction, then eviction), `mark_folder_gone`, `folder`, `entries_page`, and the settings keys `sort_key`, `sort_dir`, `last_folder`; sorting with `pbc_fold`; the bound constants 50,000 entries / 2,000 folders in one named place; `tests/unit/test_store_listing.cpp` and `tests/integration/test_budget_cache.cpp` pass.
- [X] T056 [P] [US2] Implement `core/include/pbc/age_text.h` and `core/src/age_text.cpp` (`listing_age`) and `core/include/pbc/reader_zone.h` and `core/src/reader_zone.cpp` (`reader_zone_from_config`, date formatting through `tz.h`) so `test_age_text` and `test_date_text` pass; add to `PBC_CORE_SOURCES`.
- [X] T057 [US2] Implement `bridge/include/browse_bridge.h` and `bridge/src/browse_bridge.cpp` per [contracts/bridge-qml.md](contracts/bridge-qml.md) (mode `folder`, `path`, `pathSegments`, `page`, `pageCount`, `pageSize`, `rows`, `sortKey`, `sortAscending`, `listingAgeText`, `listingState`, `offline`, `busy`, `errorKey`; invokables `open`, `up`, `refresh`, `nextPage`, `previousPage`, `goToPage`, `setSort`; signal `showDetails`); fetches run on the `Worker`; a tap-started fetch calls `device.connectNetwork()` first; size text locale-aware; register in `bridge/CMakeLists.txt` and `tests/CMakeLists.txt`; `tests/bridge/test_browse_bridge.cpp` passes.
- [X] T058 [US2] Add `qml/pages/FolderPage.qml` per [contracts/bridge-qml.md](contracts/bridge-qml.md): `TopBar` with the folder name and an up button, a path line (middle-shortened), the listing-age line, `InkRow`s with name, size, date and the state word/kept mark slots, an unavailable marker with a reason on tap, `PageButtons` with "page n of m", a bottom panel with **Refresh** and **Sort** (a `ChoiceDialog` for name/date/size and direction), the offline notice; when nothing is cached and the reader is offline the page says so plainly and offers the tools **Settings** and **Retry** (FR-035, edge case); no kinetic scrolling, no animation; register in `qml/qml.qrc`.
- [X] T059 [US2] Add the **Files** tool to `qml/pages/HomePage.qml` (opens `FolderPage` at the last folder from `last_folder` or `/`), wire `browse` in `app/main.cpp`, and add all US2 texts to `qml/strings_de.tsv` (age phrases, offline notice, unavailable reasons, sort labels, "page n of m"); run `python3 scripts/extract-strings.py --check` and `scripts/check-ui-style.sh`.
- [X] T060 [US2] Run the US2 acceptance walk (quickstart section 3, step 2) with the fake and the full suite; confirm SC-004 figures are printed and the data-model bounds hold.

**Checkpoint**: US1 + US2: connect and browse, offline and cached, no downloads yet.

---

## Phase 5: User Story 3 - Download and open a file (Priority: P1)

**Goal**: the user sees file details, downloads with coarse progress and cancel, survives connection drops, never gets a
partial file in the library, is protected by space and size checks, can open the file in the reader's viewer, and never
loses a changed local copy.

**Independent Test**: against the fake, download a small and a large file, cancel one, cut the connection during another
and restore it, open a finished download, and change a downloaded file locally before downloading again.

### Tests for User Story 3 (write first, they must fail) ⚠️

- [X] T061 [P] [US3] `tests/unit/test_local_name.cpp`: `sanitize_component` replaces control characters and `\ / : * ? " < > |` with `_`, maps `.`, `..` and empty to `_`, strips trailing dots and spaces, prefixes Windows device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9`, with and without extension), limits to 120 UTF-8 bytes on a character boundary keeping an extension of up to 16 bytes, converts to NFC, replaces invalid UTF-8; `choose_local_path` builds the mirrored tree, resolves collisions case-insensitively with ` (2)`, ` (3)` before the extension, reuses a recorded mapping, refuses a path over 4,000 bytes with `path_too_long`, and never returns a path outside the root.
- [X] T062 [P] [US3] `tests/unit/test_name_hostile.cpp` (SC-016): the hostile table of R-9 (`..`, `../x`, `a/b`, `a\b`, `CON.txt`, NUL, 300 combining marks, NFC/NFD pairs, `A.txt` versus `a.TXT`, names equal to `.pbc-1.part`, 1 to 4,000 bytes), a symbolic link planted at a folder level of the download root, a download root that itself is a link; assert nothing is created or replaced outside the root and no existing file is overwritten.
- [X] T063 [P] [US3] `tests/unit/test_storage_info.cpp`: `PosixStorageInfo` returns free bytes, writability and the FAT limit (4 GiB − 1) for a path on a temporary directory (the filesystem magic is mocked through a seam), and a read-only directory gives `writable == false`; a `tests/support/fake_storage.h` fake can be set to any free size, limit and read-only state.
- [X] T064 [P] [US3] `tests/unit/test_preflight.cpp`: `preflight_download` verdicts — size ≥ 4,294,967,296 → Refused `too_large_for_storage`; free < size + max(8 MiB, 1 % of size) → Refused `not_enough_space` with size, free and needed; size ≥ warn size (default 100 MiB, setting `large_file_warn_mib` 1–4095) → NeedsConfirmation; unknown size → Ok with a stream-time check; read-only → Refused `storage_readonly`; boundaries 4 GiB − 1, 4 GiB, free = needed − 1, free = needed (FR-019, SC-008).
- [X] T065 [P] [US3] `tests/unit/test_checksums.cpp`: `parse_checksums` for `SHA1:… MD5:… ADLER32:…` and `SHA256:…` (unknown algorithms ignored, malformed hex ignored, case-insensitive algorithm names), `hash_file` for SHA-256, SHA-1 and MD5 against known vectors with a cancel in the middle returning `cancelled`, empty file, and a file of 5 MiB read in 64 KiB steps.
- [X] T066 [P] [US3] `tests/unit/test_store_local.cpp`: `commit_download` writes the `local_files` row and deletes the journal row in one transaction (kill between is impossible: assert via a fault hook that neither or both happen); `journal_begin/update/end`; `local_file`, `local_files` paged; `forget_local_file`; `set_server_missing`, `set_seen_etag`; `local_path` uniqueness; `downloads.state` only `running`, `waiting`, `paused`.
- [X] T067 [P] [US3] `tests/integration/test_download_basic.cpp` (`PBC_FAKE_SERVER_TEST`): a complete download writes `.pbc-<id>.part` in the target directory, renames atomically after size and checksum match, records `local_files` (size and mtime taken from `stat` after the rename), deletes the journal row, and leaves no `.part`; a size mismatch → `size_mismatch`; a checksum mismatch → `checksum_mismatch` and no final file; a 404/403/401/423/429/503 maps to its key and leaves nothing; storage full during write → `storage_full`; an I/O error during write (the card removed, EIO) → `storage_full` or `storage_readonly`, no final file, no `.part`, no journal row; cancel at 50 % removes partial file and journal row and calls the progress callback ≥ once per second; the journal row exists before the first request.
- [X] T068 [P] [US3] `tests/integration/test_download_resume.cpp` (`PBC_FAKE_SERVER_TEST`): a drop after 40 % then reconnect with an unchanged strong ETag sends `Range: bytes=n-` and `If-Range`, accepts 206 only when `Content-Range` starts at `n`, and ends with bytes identical to the server's; the ETag recorded in `local_files.remote_etag` is the one of the response that delivered the bytes (a file replaced on the server between the details view and the download is recorded with the received version, and a response without an ETag leaves `remote_etag` empty); 200 to a ranged request restarts from zero (never appends a 200 body); 416 restarts once; a changed ETag restarts; a weak ETag restarts and never sends `If-Range`; five attempts / ten minutes then `paused`; a restart of the process finds the paused journal row and resumes with **Resume**; the wait uses only the Wi-Fi callback, no request (R-8).
- [X] T069 [P] [US3] `tests/integration/test_download_kill.cpp` (uses `tests/support/kill_harness.h`): kill the process at each of five points — after the journal row, mid-body, before verify, before rename, after rename; after the start-up `recover()` assert: no `.pbc-*.part` file without a journal row remains, rows without a partial file are deleted, `running` rows become `paused`, a final file exists only if complete, `local_files` and the final file agree (SC-006).
- [X] T070 [P] [US3] `tests/integration/test_download_curl.cpp` (real `CurlTransport` + `tests/support/tls_test_server.h`): a 20 MB download streams to disk with a 64 KiB buffer and constant memory (assert peak RSS growth below 20 MB), `Accept-Encoding: identity`, `Range` resume with 206, cancel through the progress callback, a redirect to another host refused, storage-full detection through a small tmpfs or the seam of `fake_storage.h`.
- [X] T071 [P] [US3] `tests/integration/test_download_conflict.cpp` (`PBC_FAKE_SERVER_TEST`): a local copy changed (size or mtime differs from `local_files`) is never overwritten by a download — the job ends `blocked` with the verified partial file kept; `keep_mine` records `seen_etag` and changes no file; `keep_both` downloads the server version under a collision-safe name ` (server copy)` and records only the new file; `replace` overwrites atomically and only after the second confirmation flag is passed; an unchanged copy is replaced atomically on update; a file present at the target but not recorded is treated as changed (FR-021, SC-009).
- [X] T072 [P] [US3] `tests/bridge/test_transfer_bridge.cpp` (Qt Test, offscreen): `details` fields for a file with and without a local copy and with a newer server version; `preflight` verdict map; `download` runs one job at a time (second tap → busy); progress properties update at most every 2 s or 5 %; `cancel`; `resume` of `pausedDownloads` after a restart; `open` refuses a path outside the download root, a directory, a link, or an unknown remote path, and calls `DeviceBridge::openFile` otherwise.
- [X] T073 [P] [US3] `tests/integration/test_budget_download.cpp`: stream a 1 GiB file from the fake through the real transport on the host; assert peak `VmHWM` ≤ 150 MB and that progress updates ≤ 1 per 2 s; print figures; marked "unverified until measured on the device" (SC-007).
- [X] T074 [US3] Extend `tests/support/fake_nextcloud_server.h` for downloads: `Range`/`If-Range`/`206`/`Content-Range`/`416`, ETag changes mid-download, weak ETags, drop after *n* bytes, `Content-Length` lies, 423/429/503 on `GET`, `oc:checksums` in listings, large synthetic files generated on the fly (no 1 GiB buffer in memory), and `tests/support/fake_storage.h`.
- [X] T075 [US3] Red run: build and run T061–T073; record that each fails for the intended reason; do not start T076 before.

### Implementation for User Story 3

- [X] T076 [US3] Implement `core/include/pbc/local_name.h` and `core/src/local_name.cpp` per R-9 (all six rules, NFC through a table added to `core/src/utf8.cpp`/`core/include/pbc/utf8.h`, canonical-prefix check without following links); add to `PBC_CORE_SOURCES`; `test_local_name` and `test_name_hostile` pass.
- [X] T077 [US3] Implement `core/include/pbc/storage_info.h` and `core/src/storage_info.cpp` (`StorageInfo`, `PosixStorageInfo` with `statvfs`/`statfs`, `min(fs limit, 4 GiB − 1)` on the device target) and `tests/support/fake_storage.h`; `test_storage_info` passes.
- [X] T078 [P] [US3] Implement `core/include/pbc/hash.h` and `core/src/hash.cpp` (`parse_checksums`, `hash_file` via OpenSSL EVP, 64 KiB reads, cancel between reads); `test_checksums` passes.
- [X] T079 [US3] Implement the local-file and journal methods of `Store` (`commit_download`, `local_file`, `local_files`, `forget_local_file`, `set_server_missing`, `set_seen_etag`, `journal_begin`, `journal_update`, `journal_end`, `journal`) in `core/src/store_files.cpp` with the constraints of [data-model.md](data-model.md); `test_store_local` passes.
- [X] T080 [US3] Implement `preflight_download` and the `Downloader` (`run`, `recover`) in `core/include/pbc/downloader.h` and `core/src/downloader.cpp` per R-8 points 1–12 (journal before the first request, temp file `.pbc-<id>.part` in the target directory, `Range` + `If-Range` only with a strong ETag, 206/200/416 handling, size and checksum verification, `fsync` then `rename`, recording `local_files.remote_etag` from the ETag header of the response that delivered the bytes, conflict check against `local_files`, the waiting loop with the Wi-Fi callback — delays 5, 15, 45, 60, 60 s, ten minutes — then `paused`, cancel through the progress callback, `libcurl` buffer 64 KiB, `identity_encoding`); it is the only place besides `local_files.cpp` allowed to call `unlink`/`rename` (rule N5) and the only core place allowed to sleep (rule N3); tests T064, T067–T071 and T073 pass. Start this task only after T086 (the early device probe) has been written; the probe results are recorded as soon as a device package exists (T087).
- [X] T081 [US3] Implement the conflict operations (`keep_mine`, `keep_both`, `replace` with its second-confirmation flag) in `core/include/pbc/local_files.h` and `core/src/local_files.cpp` (the module that owns removing and replacing user files; `forget_local_file` and the Space screen call it too); `test_download_conflict` passes.
- [X] T082 [US3] Extend `app/device_bridge.h` and `app/device_bridge.cpp`: property `downloadRoot` (`/mnt/ext1/PBCloud` on the device, `PBC_DOWNLOAD_DIR` default `./pbcloud-downloads` on the host), `openFile(path)` accepts only a canonical regular file inside the download root and then calls `OpenBook(path, "", 0)` (pattern of notes-pocketbook, Principle IV: only the reader's viewer opens downloads), logs the result without the path; add a case to the existing device-bridge tests.
- [X] T083 [US3] Implement `bridge/include/transfer_bridge.h` and `bridge/src/transfer_bridge.cpp` per [contracts/bridge-qml.md](contracts/bridge-qml.md): `details`, `preflight`, `download`, `cancel`, `resume`, `resolveConflict`, `open`, `jobState`, `jobProgressText`, `jobPercent`, `jobError`, `pausedDownloads`, signals; the job runs on the `Worker`; progress throttled to 2 s / 5 %; `device.connectNetwork()` is called first for every tap-started download, resume and Update; calls `Downloader::recover()` once at construction (no network); register in `bridge/CMakeLists.txt` and `tests/CMakeLists.txt`; `tests/bridge/test_transfer_bridge.cpp` passes.
- [X] T084 [US3] Add `qml/pages/DetailsPage.qml` (name, size, type, modified date, "On the reader: yes/no", "Server version: newer/same/unknown", a text progress line with bytes and **Cancel**, tools **Download**/**Open**/**Resume**, the large-file `ConfirmDialog`, the refusal messages for `not_enough_space` (with sizes), `too_large_for_storage`, `storage_readonly`, `storage_full`) and `qml/pages/ConflictPage.qml` (sizes and dates of both versions, tools **Keep my copy**, **Keep both**, **Replace my copy** with the second `ConfirmDialog` naming the size and modified time of the copy that will be lost); progress is plain text updated coarsely, no bar animation; register in `qml/qml.qrc`; connect `browse.showDetails` in `FolderPage.qml`.
- [X] T085 [US3] Add all US3 texts (details labels, progress and error texts, large-file warning, the conflict page) to `qml/strings_de.tsv`; wire `transfer` and the `DeviceBridge.downloadRoot` in `app/main.cpp`; run `python3 scripts/extract-strings.py --check`, `scripts/check-ui-style.sh`, `scripts/check-constitution.sh`.
- [X] T086 [P] [US3] Do this **before T080**: extend `app/probe/pbc_probe.cpp` and `scripts/probe-device.sh` with the facts DT-NC-1/3/9 need. Print the `statvfs` free space and filesystem type of `/mnt/ext1`, the result of creating a 5 GiB sparse file (expected failure on FAT), the bundled libcurl's version and feature flags (`CURL_VERSION_SSL`, `…_LIBZ`), and whether `OpenBook` is linkable. Add an optional probe URL (environment variable `PBC_PROBE_URL`, https only, any plain file server the owner runs with a file of at least 1 MiB): the probe then performs a ranged GET (`Range: bytes=1024-`, expects 206 and the right `Content-Range`), a GET with an `If-Range` that does not match (expects 200), a progress-callback cancel after 64 KiB, and a same-host redirect check if the URL redirects, and prints one result line for each (no URL, no host in the output). Without `PBC_PROBE_URL` those lines say `skipped`. Keep `tests/launcher/test_probe.sh` green (it never sets the variable).
- [X] T087 [US3] Run the US3 acceptance walk (quickstart section 3, step 3) with the fake, then the full suite; run the kill and resume tests also with `-DPBC_SANITIZE=ON`; confirm SC-006 to SC-009 and SC-016 tests are green. **Device checkpoint (owner, needs the SDK and the reader)**: build the package (`scripts/build-curl-arm.sh`, `scripts/package-device.sh`, see T127), install it, run the probe of T086 with `PBC_PROBE_URL`, and record DT-NC-1, DT-NC-2, DT-NC-3 and DT-NC-8 with dates in `docs/device-test-checklist.md` and `docs/device-findings.md` before US4 starts; if no SDK or reader is available in the session, write "not done" next to the items and continue, the design already falls back to a clean restart when `Range` does not work. **Status in this session: not done** (no SDK and no reader available); DT-NC-1, DT-NC-2, DT-NC-3 and DT-NC-8 stay **U**. The host runs (82 tests, also under AddressSanitizer and UBSan) are green.

**Checkpoint**: P1 complete: connect, browse, download, open. This is the minimum product (stop here for a first device test; the device checkpoint of T087 belongs here).

---

## Phase 6: User Story 4 - Keep files offline and update them (Priority: P2)

**Goal**: files and whole folders (with subfolders) can be marked "keep offline"; a user-started Update checks them and
downloads only what changed or is new; every entry shows its state; nothing changed on the reader is overwritten.

**Independent Test**: mark a file and a folder, change files on the fake server, tap Update and verify only the changed
and new files are fetched, the changed-on-reader file is left alone and reported, and the states are shown correctly.

### Tests for User Story 4 (write first, they must fail) ⚠️

- [X] T088 [P] [US4] `tests/unit/test_entry_state.cpp`: `compute_state` for every row of the state table in [data-model.md](data-model.md): no local row, missing file, `server_missing`, local changed by size, by mtime, server changed by ETag, by size and mtime when the ETag is empty, weak ETag equality, `seen_etag` suppressing `outdated`, `conflict`, `downloaded`; `removed_on_server` wins among states with a local row.
- [X] T089 [P] [US4] `tests/unit/test_store_marks.cpp`: `set_mark`/`marks` round trip; a more specific mark below a marked folder is kept when the folder mark is removed; removing a mark never touches `local_files`; scope helper `in_scope(remote_path)` is true for a marked file and for any depth below a marked folder, false for siblings and for a folder with a similar prefix (`/Books2` versus `/Books`).
- [X] T090 [P] [US4] `tests/integration/test_update.cpp` (`PBC_FAKE_SERVER_TEST`): Update lists each folder in scope with `Depth: 1` once, uses the answer transiently, and creates no `folders`/`entries` row for a folder that was not cached before (a cached folder in scope is refreshed like an explicit refresh; the cache row counts do not grow) — Principle V; downloads `outdated` and `not_downloaded` files, including files newly added to a kept folder and in subfolders; skips `downloaded`; leaves `changed_on_reader` and `conflict` untouched and reports them under `need_decision`; flags a file missing on the server as `removed` and keeps the local copy; the report counts folders, files, updated, added, skipped, need_decision, removed, failed, `unchecked_folders` (limit 500 folders and depth 20: a tree of 501 folders reports one unchecked); a general error (`not_enough_space`, `auth_failed`, `maintenance`) stops the batch and a per-file error (`forbidden`, `not_found`) does not; repeating an Update interrupted at every download boundary gives the same final state as an uninterrupted one and does not re-download a completed file; no request is sent without the user's `run`; `sync_state` records outcome and counts.
- [X] T091 [P] [US4] Extend `tests/bridge/test_transfer_bridge.cpp` and `tests/bridge/test_browse_bridge.cpp`: `setKept` adds and removes marks and the `kept` flag of rows; `update`/`cancelUpdate`/`updateRunning`/`updateReport`; the rows carry the state keys `not_downloaded`, `downloaded`, `outdated`, `changed_on_reader`, `conflict`, `removed_on_server`, `missing_locally`; the totals confirmation appears when the planned bytes exceed the warn size or the free space.
- [X] T092 [US4] Red run: build and run T088–T091; record that each fails for the intended reason; do not start T093 before.

### Implementation for User Story 4

- [X] T093 [US4] Implement `core/include/pbc/entry_state.h` and `core/src/entry_state.cpp` (`compute_state`, `EntryState`, `LocalFile`, `FileStat`) exactly as the pseudocode of [data-model.md](data-model.md); `test_entry_state` passes.
- [X] T094 [US4] Implement the mark methods (`set_mark`, `marks`, `in_scope`) in `core/src/store_files.cpp`; `test_store_marks` passes.
- [X] T095 [US4] Implement `core/include/pbc/update.h` and `core/src/update.cpp` (`Updater::run`) per R-10: group by parent, walk kept folders (500 folders, depth 20, no ETag shortcut), read each listing transiently (refresh the rows only of folders that were already cached; create no row for an unvisited folder), `set_server_missing`, downloads through `Downloader`, general versus per-file errors, cancel, totals before the first download, report and `sync_state` update; `test_update` passes.
- [X] T096 [US4] Extend `bridge/src/transfer_bridge.cpp` and `bridge/src/browse_bridge.cpp`: `setKept`, `update`, `cancelUpdate`, `updateReport`, `updateRunning`, the `state` and `kept` fields of `rows` computed with `compute_state` and one `stat` per visible row (page only); T091 tests pass.
- [X] T097 [US4] Update `qml/pages/FolderPage.qml` and `qml/pages/DetailsPage.qml`: state words (`Not downloaded`, `Downloaded`, `Outdated`, `Changed on the reader`, `Conflict`, `Removed on the server`) beside the size, a **Keep offline** toggle tool, an **Update** tool in the bottom panel of the folder page and the details page (for the file); add `qml/pages/UpdatePage.qml` (the report counts, the list "needs a decision" with a tap to `ConflictPage`, the list "removed on the server"); register in `qml/qml.qrc`; add the **Update** tool to `HomePage.qml`.
- [X] T098 [US4] Add all US4 texts to `qml/strings_de.tsv`; extend `tests/integration/test_no_background_network.cpp` with: Wi-Fi on and kept items → no Update, no listing, no download without the tap (FR-025, SC-010); run the string, style and constitution checks.
- [X] T099 [US4] Run the US4 acceptance walk (quickstart section 3, step 4) and the full suite; confirm SC-011 (20 kept files, 3 changed → exactly 3 downloaded and the report "20 checked, 3 updated") is a passing test.

**Checkpoint**: US4 works with US1–US3.

---

## Phase 7: User Story 5 - Favorites and search (Priority: P2)

**Goal**: the user sees the files starred in Nextcloud and finds files by name offline and, on request, on the server.

**Independent Test**: with two favorites on the fake and a populated cache, open Favorites online and offline, search
offline and see cached results, then search the server and see more.

### Tests for User Story 5 (write first, they must fail) ⚠️

- [X] T100 [P] [US5] `tests/unit/test_flat_result_parse.cpp` with fixtures `tests/fixtures/nextcloud/report/*.xml.unverified` and `tests/fixtures/nextcloud/search/*.xml.unverified`: `parse_flat_result` for favorites and search answers (entries from many folders, hrefs outside the prefix dropped, hostile names skipped, cap of 2,000 favorites / 100 search results, folders and files, `oc:favorite`), plus `favorites_request_body()` and `search_request_body()` exact output including the contents of [contracts/server-requests.md](contracts/server-requests.md) sections 7 and 8.
- [X] T101 [P] [US5] `tests/unit/test_search_term.cpp`: `search_request_body` removes `%`, `_` and `\`, XML-escapes `& < > " '`, returns an empty body (no request) when the term is empty after cleaning, never contains the term unescaped; the offline matcher uses the case-folded substring match with an escaped `LIKE` (a term of `50%_off` matches literally).
- [X] T102 [P] [US5] `tests/unit/test_store_favorites.cpp`: `replace_favorites` replaces all rows in one transaction and sets `favorites_fetched_at`, keeps the order received (`position`), keeps old rows on a failed replace; `search_cached` searches `entries`, `favorites` and `local_files`, returns the source of each row, honors the limit, and finds 5,000 rows in ≤ 100 ms.
- [X] T103 [P] [US5] `tests/integration/test_favorites_search.cpp` (`PBC_FAKE_SERVER_TEST`): `fetch_favorites` online stores and shows the list; offline the stored list is shown with its age; a failed fetch keeps the old list; `searchOffline` sends no request at all; `search_server` sends one `SEARCH` for the whole account on the explicit tap only, results are not written to the folder cache, more than 100 results sets `moreResults`; no log line contains the term (FR-029, FR-030, SC-012).
- [X] T104 [US5] Extend `tests/bridge/test_browse_bridge.cpp` with modes `favorites` and `search`, `showFavorites`, `searchOffline`, `searchServer`, the `source` field and `moreResults`; extend `tests/support/fake_nextcloud_server.h` with `REPORT` (`oc:filter-files` favorites) and `SEARCH` (`d:like` on displayname, limit) per [contracts/server-requests.md](contracts/server-requests.md).
- [X] T105 [US5] Red run: build and run T100–T104; record that each fails for the intended reason; do not start T106 before.

### Implementation for User Story 5

- [X] T106 [US5] Implement `favorites_request_body`, `search_request_body`, `parse_flat_result`, `fetch_favorites`, `search_server` in `core/src/nc_listing.cpp` / `core/include/pbc/nc_listing.h`; `test_flat_result_parse`, `test_search_term` pass.
- [X] T107 [US5] Implement `replace_favorites`, `favorites`, `search_cached` in `core/src/store_files.cpp`; the case-folded escaped `LIKE` through `pbc_fold`; `test_store_favorites` passes.
- [X] T108 [US5] Extend `bridge/src/browse_bridge.cpp` with the modes and invokables of [contracts/bridge-qml.md](contracts/bridge-qml.md); T103/T104 pass.
- [X] T109 [US5] Add `qml/pages/FavoritesPage.qml` and `qml/pages/SearchPage.qml` (an `InkField` with the on-screen keyboard, results as `InkRow` with their folder path and "from this reader"/"from the server", the tool **Also search the server**, "more results exist, refine the search"), add the **Favorites** and **Search** tools to `HomePage.qml` and `FolderPage.qml`, register pages in `qml/qml.qrc`, add the texts to `qml/strings_de.tsv`, and run the string and style checks.
- [X] T110 [US5] Run the US5 acceptance walk (quickstart section 3, step 5) and the full suite; extend `tests/integration/test_no_background_network.cpp` so a search term typed offline sends nothing.

**Checkpoint**: US5 works with US1–US4.

---

## Phase 8: User Story 6 - Manage space on the reader (Priority: P3)

**Goal**: the user sees what the app downloaded with sizes and free space, removes local copies without touching the
server, and the metadata cache stays bounded.

**Independent Test**: download several files, open the space screen, remove one copy, confirm it is gone locally and
still on the fake, free space updated, and a changed copy needs an explicit confirmation.

### Tests for User Story 6 (write first, they must fail) ⚠️

- [X] T111 [P] [US6] `tests/integration/test_space.cpp` (`PBC_FAKE_SERVER_TEST`): `remove` deletes the file and its `local_files` row and sends **zero** requests (the fake's counter stays unchanged); a copy in state `changed_on_reader` or `conflict` is refused unless `confirmedLoss` is true; a missing file still removes the row; removing a path outside the download root is impossible (hostile `local_path` rows are refused); `clearCache` drops listings but not `local_files`, `offline_marks`, `favorites`, `downloads` (FR-031, FR-032, SC-013).
- [X] T112 [P] [US6] `tests/bridge/test_space_bridge.cpp` (Qt Test, offscreen): paged `items` (20 per page) with name, size text and state; `usedText`, `freeText`, `cacheText` values; free space increases by at least the file size after a removal on a fake storage; the cache size text matches the database file size.
- [X] T113 [US6] Red run: build and run T111–T112; record that each fails for the intended reason; do not start T114 before.

### Implementation for User Story 6

- [X] T114 [US6] Add `remove_local_copy` and `clear_cache` to `core/include/pbc/local_files.h` / `core/src/local_files.cpp` and `core/src/store_files.cpp` (the only code that deletes user files; canonical-prefix check; no network); `test_space` passes.
- [X] T115 [US6] Implement `bridge/include/space_bridge.h` and `bridge/src/space_bridge.cpp` per [contracts/bridge-qml.md](contracts/bridge-qml.md); register in `bridge/CMakeLists.txt` and `tests/CMakeLists.txt`; `test_space_bridge` passes.
- [X] T116 [US6] Add `qml/pages/SpacePage.qml` (list with sizes and state words, used/free/cache lines, **Remove** with `ConfirmDialog`, the extra warning "your changes will be lost" for changed copies, **Clear cache**), the **Storage** tool on `HomePage.qml`, register in `qml/qml.qrc`, add texts to `qml/strings_de.tsv`, wire `space` in `app/main.cpp`, run the string and style checks.
- [X] T117 [US6] Run the US6 acceptance walk (quickstart section 3, step 6) and the full suite.

**Checkpoint**: all six stories work.

---

## Phase 9: Polish & Cross-Cutting Concerns

**Purpose**: demo tool, budgets, documentation, baseline clean-up, licensing, version, package.

- [X] T118 [P] Implement `tests/tools/pbc_fake_nextcloud.cpp` (an HTTPS server on localhost built on `tests/support/tls_test_server.h` and `fake_nextcloud_server.h`, generating its own CA, printing the address and an example app password, a console with `approve`, `touch <path>` and `drop`) and update `scripts/run-host-demo.sh` to start it, write its CA as `custom-ca.pem` into the throw-away data folder, set `PBC_DOWNLOAD_DIR`, and remove everything afterwards; add the target to `tests/CMakeLists.txt`.
- [X] T119 [P] Run all budget tests on the host (`test_budget_cache`, `test_budget_download`, plus a cold-start measurement of the host app with `PBC_DATA_DIR` on a 1,000-entry cache) and write the figures into the "Value" column of `docs/device-test-checklist.md` DT-NC-7 as host values clearly labelled "host, not the device"; the status stays **U**.
- [X] T120 [P] Remove the baseline leftovers decided in [research.md](research.md) R-18: `core/include/pbc/sync_scheduler.h`, `core/src/sync_scheduler.cpp`, `tests/unit/test_sync_scheduler.cpp`; `core/include/pbc/paging.h`, `core/src/paging.cpp`, `tests/unit/test_paging.cpp`; `SQLITE_ENABLE_FTS5` in `core/CMakeLists.txt` and `tests/unit/test_fts5_available.cpp`; `qml/EventRow.qml`, `qml/ContactRow.qml`, `qml/DateTimePicker.qml`, `qml/EmojiPicker.qml` with their `qmldir` lines; update `docs/baseline-leftovers.md` to say what was removed; build and run the whole suite.
- [X] T121 [P] Review `scripts/check-constitution.sh` rule R8 and the log field names against the new vocabulary (files, folders, favorites, search) and extend R8 only if a new word could carry server data; keep every self-test green.
- [X] T122 [P] Write `README.md` (what the app is, the unofficial notice, license MIT "Copyright (c) 2026 Volker von Einem", the honest credential-storage statement, the read-only scope, build and test commands from the quickstart, link to the specs), `INSTALL.md` (package, install on the InkPad One, the `custom-ca.pem` option, the download folder, the first Test), and `CHANGELOG.md` with a 0.1.0 entry; follow the conventions of the sibling apps' files (notes-pocketbook and office-pocketbook).
- [X] T123 [P] Check licensing (Principle VII): confirm that no code, string or comment was copied from Nextcloud sources; confirm `NOTICE`, `licenses/manifest.txt` and the About box list the same components (no new third-party component was added by this feature; the QR encoder and the digests are original code or OpenSSL, already listed); run `tests/unit/test_license_consistency.cpp`.
- [X] T124 Raise `VERSION` from `0.0.1` to `0.1.0` and make `scripts/check-version-bump.sh` pass with the `CHANGELOG.md` entry of T122 (constitution, Development Workflow).
- [X] T125 [P] Complete `qml/strings_de.tsv`: run `python3 scripts/extract-strings.py --check`, fix every missing or stale entry, and read every German text once for wording consistent with the sibling apps.
- [X] T126 Run `scripts/pre-merge.sh` (version bump, build, all tests, secret scan, constitution, fixtures, UI style) and a second full test run in a build directory configured with `-DPBC_SANITIZE=ON`; fix findings; both must be green.
- [X] T127 Build the device package: `POCKETBOOK_SDK=<sdk> scripts/build-curl-arm.sh` (once) and `POCKETBOOK_SDK=<sdk> scripts/package-device.sh`; confirm the size gate (≤ 25 MB) and that the ARM link resolves `libcrypto`; the SDK location comes only from `POCKETBOOK_SDK` or `-DSDK_PATH` (Principle VIII). If no SDK is available in the session, record this task as not done and leave it for the owner.
- [X] T128 Update `docs/device-findings.md` and `docs/live-server-checklist.md` with "PB Cloud findings: none yet" kept truthful (nothing is marked verified without a dated entry), and add the list of unverified assumptions of this feature (the contract rows of [contracts/server-requests.md](contracts/server-requests.md) and the DT-NC items) to `docs/reference/security-review.md` as an addendum.
- [X] T129 Validate the whole feature once against [quickstart.md](quickstart.md) sections 1–3 and tick off every acceptance scenario of [spec.md](spec.md) US1–US6 against a passing test or the walk; list any scenario without a test and add the missing test before closing.

---

## Dependencies & Execution Order

### Phase dependencies

- **Phase 1 Setup**: no dependencies; T002–T005 are parallel after T001.
- **Phase 2 Foundational**: depends on Phase 1; blocks all user stories. Inside it: T006→T007; T008→T009; T010→T011; T013→T014; T015→T016; T017→T018; T012, T019, T020 are independent.
- **US1 (Phase 3)**: depends on Phase 2. **US2 (Phase 4)** depends on Phase 2 and on the account/bridge parts of US1 (T031, T035) for a connected account in the walk, but its core and store work (T044–T056) is independent of US1. **US3 (Phase 5)** depends on US2 (cache entries and details) and on Phase 2. **US4 (Phase 6)** depends on US3. **US5 (Phase 7)** depends on US2 (listing parser) and is otherwise independent of US3/US4. **US6 (Phase 8)** depends on US3.
- **Inside US3**: T086 (the early device probe) comes before T080 (the downloader); the device checkpoint is part of T087.
- **Polish (Phase 9)**: depends on the stories that are in the release; T120 (clean-up) can run any time after Phase 2 but is placed last so it does not disturb the stories.

### Story completion order

```text
Phase 1 → Phase 2 → US1 ──┬─ US2 ──┬─ US3 ──┬─ US4
                          │        │        └─ US6
                          │        └─ US5
                          └──────────────────────→ Polish
```

### Within each story

1. Tests and fixtures first (marked [P] where in different files).
2. Extension of the fake server for the story.
3. Red run recorded.
4. Core modules, then store methods, then bridge, then QML, then strings, then wiring.
5. Acceptance walk and full suite before the next story.

### Parallel opportunities

- Phase 1: T002, T003, T004, T005 together.
- Phase 2: T006, T008, T010, T012, T013, T015, T017, T019 (all new test files or independent scripts) together; their implementation tasks follow each pair.
- US1 tests: T021–T027 together. US2 tests: T044–T051 together. US3 tests: T061–T073 together. US4 tests: T088–T091. US5 tests: T100–T103. US6 tests: T111–T112.
- Implementation in different files: T076, T077, T078 (US3) together; T056 beside T054/T055 (US2).
- After US3 is done, US4 (T088–T099) and US5 (T100–T110) can proceed in parallel on two branches because they touch different pages and mostly different store functions; the shared files `core/src/store_files.cpp`, `bridge/src/browse_bridge.cpp` and `tests/support/fake_nextcloud_server.h` need a merge.
- Polish: T118, T119, T120, T121, T122, T123, T125, T128 are parallel.

### Parallel example: User Story 3 tests

```text
Task: "tests/unit/test_local_name.cpp"            (T061)
Task: "tests/unit/test_name_hostile.cpp"          (T062)
Task: "tests/unit/test_preflight.cpp"             (T064)
Task: "tests/unit/test_checksums.cpp"             (T065)
Task: "tests/integration/test_download_resume.cpp" (T068)
Task: "tests/integration/test_download_kill.cpp"   (T069)
```

---

## Implementation Strategy

### MVP first (US1–US3 are the P1 product)

1. Phase 1 and Phase 2 (foundation).
2. US1: stop and validate with the fake server and the quickstart walk. The QR code (T034, T036, and the QR part of T037) can be postponed without blocking the rest.
3. US2: stop and validate browsing and offline cache.
4. US3: stop and validate downloads, interruption safety and opening in the viewer. Build the device package and run DT-NC-1 to DT-NC-3 and DT-NC-8 on the reader before starting US4 (T086 gives the early libcurl facts; if the device libcurl cannot honor `Range`, resume degrades to restart, which the design already allows).

### Incremental delivery

- Add US4 (offline and Update), then US5 (favorites and search), then US6 (space); each phase ends with its acceptance walk and a green full suite, and can be released on its own with a MINOR note in `CHANGELOG.md` only at the end (the version is raised once, T124).
- Unverified items stay unverified: no task marks a server or device behavior as verified; only the checklists do, with a date, after a real run.

### Notes

- Never edit a test to make it pass without understanding why it failed; a red test that fails for the wrong reason is fixed first (T029, T053, T075, T092, T105, T113).
- No task adds a write method, `Depth: infinity`, a TLS bypass, a plaintext fallback, a background timer, or a log field with server data; rules N1–N5 and R1–R11 enforce this mechanically.
- Commit after each task or logical group; commit messages in English (Principle X).

---

## Implementation notes (added while building)

- Tests that this file places in `tests/integration/` and that need Qt (`test_no_background_network`, the bridge tests, T042, `test_space_bridge`) live in
  `tests/bridge/`; `test_json_min.cpp`, `test_error_keys.cpp`, `test_entry_state.cpp`, `test_flat_result_parse.cpp` and `test_search_term.cpp` were added beside
  the named ones. Fixtures are named `<name>.unverified.<ext>`; the fake server is `tests/support/fake_nextcloud_server.h`.
- T087 (the device checkpoint of the download) was not done: there was no reader in the session. DT-NC-1/2/3/8 stay **U**. T099, T110, T117 and T129 were closed against
  host tests and the scripted walk (`acceptance.md`), not against a real server or the reader.
- T127: the package was built (`scripts/package-device.sh`, 3.5 MB, below the 25 MB gate) and `libcrypto.so.3` resolves in the ARM link; it was **not run** on a reader.
- A few small modules (for example `error_keys`) had their test and implementation written together instead of the test being observed red first.
- The demo and the walk (`scripts/run-host-demo.sh`, `scripts/ui-walk.sh`, `app/ui_script.*`) are host-only additions to T118; the acceptance walk is CTest `ui_walk`.
- Nothing was committed after the bootstrap commit; the work of this feature is in the working tree of branch `feature/001-nextcloud-files-client`.


## Phase 10: Convergence

- [X] T130 CRITICAL Keep file names and paths out of `pbcloud.log`: the launcher `platform/pocketbook/pbcloud.app` sends all stdout/stderr, including the InkView SDK lines `iv_OpenBook filename = <path>` and `task_findbybook('<path>')` seen on the reader, into the log; filter or redirect them (the shell on the reader may lack process substitution and `stdbuf`), keep the start-up evidence of a failed start, correct the statements "holds no addresses, names, paths or passwords" in `INSTALL.md` and `README.md`, add a launcher test with a fake binary that prints such lines, and record the re-check on the reader in `docs/device-test-checklist.md` per Constitution IV and FR-008 (contradicts)
- [X] T131 Show the time of the last account check and of the last Update on `qml/pages/HomePage.qml` (from `connection.checkedAt` and the stored sync state, through a bridge property), with German texts in `qml/strings_de.tsv` and a bridge test, per FR-035 and SC-015 (partial)
- [X] T132 Store availability and reason for favorites (`favorites` table, `Store::replace_favorites`, `Store::entry` fallback, a schema 2 migration step if the table changes) so that a favorite the user may not read is shown as unavailable and `TransferBridge::download` refuses it, with tests in `tests/unit/test_store_favorites.cpp` and `tests/bridge/test_browse_bridge.cpp`, per FR-013 (partial)
- [X] T133 Record the host-only scripted walk (`app/ui_script.{h,cpp}`, `scripts/ui-walk.sh`, `tests/walks/`, the `Qt6::Test` link of the host app, `scripts/run-host-demo.sh`) in `plan.md` as justified by T118 and T129, and remove the `inkfeed.bin` leftover from the `ps` diagnostic line of `platform/pocketbook/pbcloud.app`, per plan: project structure (unrequested)
