# Baseline leftovers

PB Cloud was bootstrapped from office-pocketbook 0.3.0 (see the bootstrap commit). The mail, calendar and contacts
domain was removed; everything that was not clearly domain code was **kept**. This file lists what was kept
although it is domain-flavoured, unused, or only half generic, so that the first plan can remove or reshape it
on purpose. Nothing here is a defect of the baseline: it builds, and all tests pass.

## Kept on purpose, no consumer yet

| Item | Why it is a leftover | Suggested decision |
|------|----------------------|--------------------|
| `core/include/pbc/tz.h`, `core/src/tz.cpp`, `core/include/pbc/civil.h`, `third_party/tzdata/`, `scripts/update-tzdata.sh`, `tests/unit/test_tz*.cpp`, `tests/unit/test_user_zone.cpp`, `PBC_ZONEINFO` and the `pbcloud-zoneinfo/` folder in the package, the "Time zone data" line of About | The IANA zone reader exists for calendar events. Only About uses it now (the release name). `civil.h` is still used by `clock_check`. The time zone page and bridge were removed with the calendar. | Remove `tz` and `tzdata` unless a feature shows times that need a zone; keep `civil.h` |
| `core/include/pbc/utf8.h`, `core/src/utf8.cpp`, the SQL function `pbc_fold` registered in `Store::open`, `tests/unit/test_utf8_cluster.cpp` | Grapheme and sort-key helpers for names and subjects; `pbc_fold` is registered but no query uses it. | Keep if lists are sorted by name, else remove |
| `Store::debug_exec`, `Store::debug_query` (test seams) | Used by the store tests only. | Keep |
| `SyncState` and the `sync_state` table | One row with outcome, problem key, counts and clock skew; the `problem_area` column is free text now. Nothing in the app writes it yet. | Keep as the home of the sync result of the first feature |

## Kept, mail protocols still named

| Item | Why |
|------|-----|
| `core/include/pbc/curl_session.h`, `core/src/curl_session.cpp`, `tests/unit/test_curl_session.cpp`, `tests/unit/test_error_mapping.cpp` | `Protocol::Imap` / `Protocol::Smtp`, `Security::StartTls` and the `imap(s)://`, `smtp(s)://` scheme table are part of the TLS policy and its tests. PB Cloud uses HTTPS only. |
| `kMinCurlVersionNum` (8.18.0), `cmake/curl_arm.cmake`, `scripts/build-curl-arm.sh`, the libcurl paragraph of `NOTICE` | The minimum version was chosen because libcurl 8.12.0 and 8.16.0 drop the contents of IMAP literals. Whether the reader's own libcurl is good enough for HTTPS/WebDAV is untested; see `docs/device-findings.md`. |
| `scripts/check-constitution.sh` rules R7 and R8, `scripts/probe-device.sh`, `app/probe/pbc_probe.cpp` (protocol list, charset list), `tests/launcher/test_probe.sh` | The rules and probe lines are unchanged. R7 forbids clear-text mail schemes outside `curl_session.cpp`; R8 forbids mail, calendar and contact words as log keys. They stay as written. |
| `core/include/pbc/dav_common.h` (`kNsCal`, `kNsCard`, `kNsCs`), `scripts/check-fixtures.sh` (the `calendarserver.org` host), the CalDAV samples inside `tests/unit/test_dav_common.cpp`, `test_xml_min.cpp`, `test_malformed_xml.cpp`, `test_http_transport_methods.cpp` | The DAV plumbing was written for CalDAV/CardDAV; the namespaces and sample answers remain. Nextcloud files use `DAV:` and `http://owncloud.org/ns` / `http://nextcloud.org/ns`. |

## Kept QML

| Item | Why |
|------|-----|
| Icons `calendar`, `send`, `reply-all`, `note-plus`, `bubble-pointer`, `star`, `star-filled`, `filter`, … in `qml/icons/` | The whole icon set was kept. |
| `qml/pages/SettingsPage.qml` | Reduced to language, the certificate authority note and About. The "Sync after my changes" switch, mail and calendar options were removed with their bridges. |
| `qml/pages/HomePage.qml` | A stub: "PB Cloud", the version, "Not connected". |
| The JPEG/GIF plugin copy (`pbcloud-plugins/imageformats/`) in `scripts/package-device.sh` and `app/main.cpp` | Was for the pictures of mails; thumbnails of files will need the same decoders. |

## Kept tests and test support

| Item | Why |
|------|-----|
| `tests/support/tls_line_client.h` | A TLS line client for the self-tests of the fake IMAP/SMTP servers; no fake server is left. |
| `tests/support/kill_harness.h`, `tests/support/mutate.h` | The interruption and mutation helpers; `mutate.h` is used, `kill_harness.h` has no user yet. |
| `scripts/check-fake-markers.sh`, `PBC_FAKE_SERVER_TEST` in `tests/support/test_support.h` | The rule for tests that use a fake server. There is no fake server yet; the first Nextcloud fake must carry the marker. |
| `tests/templates/` | The performance tests of the notes baseline. They include headers that do not exist here and are not built; they only show the shape of a budget test. |
| `docs/reference/` | Copied unchanged from office-pocketbook: the analyses talk about mail and calendar. `office-pocketbook-constitution.md` is the constitution this project started from. The project's own `.specify/memory/constitution.md` is still the empty Spec Kit template; the scripts under `scripts/` enforce the rules of the copied one (the principles they name are those of that document). |

## Removed by the Nextcloud files feature (specs/001-nextcloud-files-client, research R-18)

- `sync_scheduler` (`core/include/pbc/sync_scheduler.h`, `core/src/sync_scheduler.cpp`, `tests/unit/test_sync_scheduler.cpp`): the app never
  syncs by itself, so nothing decides when to start. The Update is a tap.
- `paging` (`core/include/pbc/paging.h`, `core/src/paging.cpp`, `tests/unit/test_paging.cpp`): arithmetic of the message reader. The file lists
  page in the bridges; `qml/PageButtons.qml` and `qml/ModelPager.qml` stay and are used.
- FTS5 (`SQLITE_ENABLE_FTS5`, `tests/unit/test_fts5_available.cpp`): the offline search is a case-folded substring match on names
  (research R-17), which needs no full-text index.
- `qml/EventRow.qml`, `qml/ContactRow.qml`, `qml/DateTimePicker.qml`, `qml/EmojiPicker.qml` and their lines in `qmldir` and `qml.qrc`:
  dead components of the removed calendar and contact pages. Their German strings went with them.

Kept on purpose, now with a consumer: `utf8.h` and `pbc_fold` (sorting by name, the collision test of local names, the offline search),
`SyncState` (the result of the last Update and of the last account check), `civil.h`, `tz`/`tzdata` (the reader's time zone for dates).

## Removed beyond the plain domain code

- The IMAP self-test of the probe (`--imap-selftest`, `app/probe/imap_selftest.*`, `tests/launcher/test_imap_selftest.sh`,
  the self-test section of `scripts/probe-device.sh`) and the fake IMAP demo server (`tests/tools/pbo_fake_imap.cpp`).
- The generators of mail, calendar and contact fixtures (`scripts/gen-*.py`), with their output.
- `tests/contract/` (all contract tests were IMAP or DAV-for-calendar tests).
- The store is cut down to the `settings` and `sync_state` tables, schema 1, file `pbcloud.db`; the baseline has no
  migration. `Credentials` holds one `password` (it had three).

## Missing on purpose

- `README.md`, `INSTALL.md` and `CHANGELOG.md` were written with the Nextcloud files feature (0.1.0).
- `scripts/check-version-bump.sh` wants a `CHANGELOG.md` entry for every released version once a feature branch changes `VERSION`.
- The ARM package (`scripts/package-device.sh`) was not assembled in the bootstrap.
