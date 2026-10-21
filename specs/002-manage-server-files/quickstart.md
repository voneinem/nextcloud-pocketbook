# Quickstart: validating upload and file management

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **001**: [quickstart](../001-nextcloud-files-client/quickstart.md)

How to prove that the feature works, from the cheapest check to the real device. Every device and real-server result is
**unverified** until written into `docs/device-test-checklist.md` (DT-W*) or `docs/live-server-checklist.md` (LV-W*) with a
date. Contracts: [server requests](contracts/server-requests.md), [core](contracts/core-interfaces.md),
[bridge and QML](contracts/bridge-qml.md); data: [data-model.md](data-model.md).

## 1. Host build and the automatic gate

Same prerequisites as 001 (C++17, CMake, Ninja, libcurl 8.18.0 or newer, OpenSSL and the `openssl` CLI; Qt 6 optional).

```bash
cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
ctest --test-dir build-host --output-on-failure
scripts/pre-merge.sh            # version bump (0.2.0), build, all tests, secret scan, constitution/fixture/UI-style checks
```

Expected: every test passes, including `check_constitution` with the changed rules N2, N3 and the new N6 and their
self-tests, `check_fixtures`, `check_fake_markers`, `check_ui_style`, `check_translations`. Sanitizer run on a second build
directory (`-DPBC_SANITIZE=ON`): the hostile-input, kill and queue tests must pass under it.

## 2. Contract and interruption tests (no server, no device)

```bash
ctest --test-dir build-host -L unit          # names, keep-both, scheduler, queue rules, overlay, chunk arithmetic, parsers
ctest --test-dir build-host -L integration   # fake Nextcloud: every write operation, conflicts, unknown outcome, quota
ctest -R 'queue_(kill|recover)|upload_(resume|kill)' --test-dir build-host --output-on-failure
```

| Check | Expected |
|-------|----------|
| `test_write_conditions` (every operation against the fake) | every write request carries its condition; none without (SC-007, SC-008) |
| `test_upload_small` / `test_upload_chunked` (500 MB sparse on the host: 1 GiB in the budget test) | file identical on the fake server; peak memory flat; chunks are in `/dav/uploads/`, never in the user's folder (SC-004, SC-014) |
| `test_upload_resume` (drop after 40 %, reconnect, send again; source changed; upload folder expired) | only the missing chunks are sent plus one in flight; changed source restarts and says so; expired folder restarts (SC-005) |
| `test_unknown_outcome` (answer lost after the request was processed: for `PUT`, final `MOVE`, `MKCOL`, `MOVE`, `COPY`, `DELETE`) | exactly one item `unknown`, zero automatic repeats; "Check the server" reports `arrived`; "Send again" ends in the collision question, not in a duplicate (SC-006) |
| `test_collisions` (upload, new folder, rename, move, copy; weak and empty ETags; case-only names) | the question is asked; nothing replaced without confirmation; keep-both names are free; folders are never replaced (SC-007) |
| `test_conflicts` (server changed between listing and send; the fake ignores the final-`MOVE` headers) | the item is held with both versions intact; the pre-check catches the ignored header (SC-008) |
| `test_delete_trash` (trash capability present / absent / unknown) | confirmation names the trash; without a trash no `DELETE` is ever sent; local copies are never removed (SC-009, SC-013) |
| `test_refusals` (507, 413, 415, 403 invalid path, 423, 409, 207, hostile exception texts) | each key and message distinct; no server text shown or logged (SC-010) |
| `test_local_follow` (rename/move of an entry with a local copy and an offline mark; replace from the local copy) | records follow; the link rule applies only to the entry's own copy with unchanged identity (SC-013) |
| `test_queue_kill` (kill at every step) | no item lost or duplicated; `unknown` exactly where R-6.5 says; recovery sends nothing; the server shows no partial file (SC-002, SC-014) |
| `test_no_background_network` (extended) | Auto Sync on: no request at start, on Wi-Fi up, on a failed item, or after a send; one quiet period after a user action is the only trigger; Auto Sync off: none (SC-003) |
| `test_picker_bridge` / `test_source_hostile` | links, hidden, special files skipped; the app's data folder and credentials never selectable; bounds honored (SC-001, SC-016) |
| `test_budget_queue`, `test_budget_upload` | prints the 200-item queue open time (<= 2 s), page turn (<= 1 s), peak RSS during a 1 GiB streamed upload (<= 150 MB), database growth at 2,000 items (<= 2 MB). Marked **unverified until measured on the device** |

## 3. Try the app on the host with the fake server

```bash
scripts/run-host-demo.sh            # fake server (HTTPS on localhost, own CA) with the write side, and the app
```

The script prepares a throw-away data folder, download folder and a **picker root** with sample folders (`Downloads/`,
`Notes/`, a 300 MB sparse `big.bin`, a folder with subfolders, a link, and a copy of a downloaded file under `PBCloud/`). The fake
server's console has the commands `touch <path>` (change a file), `lose-next-answer` (the unknown-outcome case), `drop-after
<bytes>`, `quota <bytes>`, `lock <path>`, `no-trash`, `ignore-conditions`. Walk through:

1. **US1** Files -> **Add** -> *Upload files* -> the picker (list/tile toggle remembered after a restart) -> pick two files and
   a folder (**Add this folder**: "2 folders, 5 files") -> the queue notice; the listing shows them **Not synced**; the fake
   server's console shows no request yet.
2. **US2** Settings: Auto Sync on (default). Wait 10 s: the queue is sent, the marks go away. Switch Auto Sync off, queue a file,
   nothing is sent; **Queue** -> **Send** sends it. Start `big.bin`, `drop-after 100000000`, **Send** (stops, items stay
   queued), **Send** again: only missing chunks. `lose-next-answer` on a small upload: the item shows **Outcome unknown**,
   Auto Sync and **Send** skip it; **Check the server** says it arrived.
3. **US3** Upload a name that exists: Keep both / Replace (confirmation names the file) / Skip. `touch` the server file after the
   listing was fetched, then send a queued replace: "changed on the server", both versions kept.
4. **US4** **New folder**, queue an upload into it before sending, **Rename** another entry (a case-only change too), send: the
   server shows the folder with the file and the new name. A name with a forbidden character is refused at once.
5. **US5** **Select** two entries -> **Move** -> choose a folder (offline: stored listings only) -> **Copy** a folder; a
   destination inside the source is refused. After sending, the offline mark and local copy of a moved file follow it.
6. **US6** **Select** -> **Delete**: the dialog names the trash; the entry stays with "Delete pending"; send; it is gone.
   `no-trash` and restart the account check: Delete says the server has no trash and queues nothing. A locked file is shown
   as locked and retried only on a tap.
7. **US7** `quota 1000`: the upload fails with the storage-full message before any byte is sent when the quota is known, and with
   the server's refusal otherwise; forbidden name, size and type refusals each show their own message.

Pass criterion: every step behaves as in the matching acceptance scenario of the spec. The UI walk
`tests/walks/03-upload-and-manage.walk` (CTest `ui_walk`) scripts steps 1, 2, 4 and 6.

## 4. Real server (unverified until done)

Use a throw-away test account on a Nextcloud 30 or newer server with an **app password** only; never commit addresses or
passwords. Work through `docs/live-server-checklist.md` items LV-W1 to LV-W12: capabilities for the write side, conditions on
`PUT`, `MOVE`, `COPY`, `DELETE`, `MKCOL` answers, chunked upload v2 end to end, which conditions the final chunk `MOVE`
honors, upload-folder listing and expiry, checksum and modification time, `DELETE` into the trash, the forbidden-name
capability lists, the status codes of every refusal (also behind a reverse proxy), `207` member failures. Anonymized exchanges
become fixtures: rename `*.unverified` only for exchanges that were seen live.

## 5. Device (InkPad One), unverified until done

```bash
POCKETBOOK_SDK=<sdk path> scripts/package-device.sh      # builds and checks the size gate (25 MB)
scripts/probe-device.sh                                  # now also: PUT from a file range, delivery after a cut
```

Install, then work through `docs/device-test-checklist.md` items DT-W1 to DT-W9: a `PUT` from a file range with progress and
cancel on the bundled libcurl; `delivery` after a cut Wi-Fi mid-body; chunk size versus memory and time; speed of the
SHA-256 pass on a 500 MB file; a 500 MB upload with a Wi-Fi drop and resume; power loss during an upload; e-ink refreshes
during an upload; the picker's real top level and the `PBCloud` folder; Auto Sync on a reader whose Wi-Fi is off. Measured
values go in the "Value" column with the build version and date.
