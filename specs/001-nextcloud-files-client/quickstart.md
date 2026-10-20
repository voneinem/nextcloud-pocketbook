# Quickstart: validating the Nextcloud files client

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md)

How to prove that the feature works, from the cheapest check to the real device. Every device and real-server result
is **unverified** until written into `docs/device-test-checklist.md` (DT-NC-*) or `docs/live-server-checklist.md`
(LV-N*) with a date.

## 1. Host build and the automatic gate

Prerequisites: a C++17 compiler, CMake, Ninja, libcurl 8.18.0 or newer, OpenSSL and the `openssl` CLI (the TLS tests start
local HTTPS servers). Qt 6 (Core, Gui, Quick, Test) is optional; without it the bridge, the app and the bridge tests are
skipped.

```bash
cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
ctest --test-dir build-host --output-on-failure
scripts/pre-merge.sh            # version bump, build, all tests, secret scan
```

Expected: every test passes, including `check_constitution` (rules R1–R11 and N1–N5), `check_fixtures`,
`check_fake_markers`, `check_ui_style`, `check_translations`. Sanitizer run: `cmake -DPBC_SANITIZE=ON` on a second build
directory; the hostile-input and interruption tests must pass under it.

## 2. Contract and interruption tests (no server, no device)

```bash
ctest --test-dir build-host -L unit          # parsers, names, states, QR, preflight arithmetic
ctest --test-dir build-host -L integration   # fake Nextcloud: login, listing, download, update, search
ctest -R 'download_(kill|resume|drop)' --test-dir build-host --output-on-failure
```

What to look for:

| Check | Expected |
|-------|----------|
| `test_download_kill` (kill after the journal row, mid-body, before verify, before rename, after rename) | after restart cleanup: no `.pbc-*.part`, journal consistent, a final file is complete or absent (SC-006) |
| `test_download_resume` (drop after 40 %, then reconnect, ETag unchanged / changed / weak) | 206 resume; 200 → restart; weak ETag → restart; identical bytes to the server's (SC-006) |
| `test_name_hostile` | no path leaves the download root, no overwrite on a case-only collision (SC-016) |
| `test_listing_hostile` (truncated, partial, billion-laughs, 10,001 entries, foreign hrefs) | valid entries stored, bad ones counted, no crash |
| `test_no_background_network` (Wi-Fi on, no taps) | exactly one request at start, none afterwards; with Wi-Fi off none (SC-010) |
| `test_budget_*` | prints cache-open time for 1,000 entries (≤ 2 s), page turn (≤ 1 s), peak RSS during a 1 GiB streamed download (≤ 150 MB), database size at the cache limits (≤ 50 MB). Marked **unverified until measured on the device** |

## 3. Try the app on the host with the fake server

```bash
scripts/run-host-demo.sh            # starts tests/tools/pbc_fake_nextcloud (HTTPS on localhost, own CA) and the app
```

The script creates a throw-away data folder and download folder, writes the fake server's CA as `custom-ca.pem`, and
prints the address and an app password for the fake account (an example value, not a secret). Walk through:

1. **US1** Settings → enter the printed address, `demo` and the printed app password → **Test**: server name, version,
   quota appear. Change the password by one letter → the "wrong credentials" message. Use `http://…` → refused.
   Start **Log in with a browser** → link and QR code shown → **Check** says "not approved yet" → approve in the fake
   server's console (`approve` line) → **Check** connects.
2. **US2** Files → root listing, folders first; **Sort** by date and size; open `Books/`, go up; stop the fake server,
   reopen `Books/` (cached, "Updated … ago"); restart it and **Refresh**. The folder `Locked/` is shown as not available.
3. **US3** Open a book → **Download** → progress text → **Open** (the host app reports the path it would hand to the
   viewer). Start the 300 MB `large.bin`, kill the server mid-way, restart it → the download resumes. **Cancel** another
   one → no file in the download folder. Edit a downloaded file, then **Download** again → the conflict page.
4. **US4** Mark `Books/` **Keep offline**, change a file on the fake server (`touch` command of its console), tap
   **Update** → only that file is fetched; the list shows `outdated` before and `downloaded` after.
5. **US5** Favorites shows the two starred files; Search `story` offline finds cached entries; **Also search the server**
   adds more.
6. **US6** Storage → sizes, **Remove** a copy; the fake server's file is still there.

Pass criterion: every step behaves as in the matching acceptance scenario of the spec.

## 4. Real server (unverified until done)

Use a throw-away test account on a Nextcloud 30 or newer server with an **app password** only; never commit addresses or
passwords. Work through `docs/live-server-checklist.md` items LV-N1 to LV-N9 (added by this feature): status and
capabilities fields, the 2FA answer, quota sentinels, Login Flow v2, availability markers (encrypted folder, share
without read permission, unmounted external storage), `Range`/`If-Range`, folder-ETag propagation, the favorites body,
the search body. Record the result per item. Anonymized exchanges become fixtures: rename `*.unverified` only for
exchanges that were seen live.

## 5. Device (InkPad One), unverified until done

```bash
POCKETBOOK_SDK=<sdk path> scripts/build-curl-arm.sh      # once
POCKETBOOK_SDK=<sdk path> scripts/package-device.sh      # builds and checks the size gate (25 MB)
scripts/probe-device.sh                                  # libcurl, TLS, ranges, free-space and storage facts
```

Install, then work through the new items of `docs/device-test-checklist.md`: DT-NC-1 (libcurl HTTPS, redirect, Range on
the device), DT-NC-2 (credentials file mode and the in-app text), DT-NC-3 (download folder appears in the library, no
`.part` file listed), DT-NC-4 (hash pass speed), DT-NC-5 (opening a book does not change the file), DT-NC-6 (time zone
source), DT-NC-7 (cold start ≤ 5 s, peak memory ≤ 150 MB, 1,000-entry cached folder shown ≤ 2 s, page turn ≤ 1 s),
DT-NC-8 (Wi-Fi drop and return during a 500 MB download), DT-NC-9 (4 GiB refusal with a sparse server file),
DT-NC-10 (e-ink refresh count during a download, coarse progress). Measured values go in the "Value" column.
