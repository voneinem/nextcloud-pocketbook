# Acceptance traceability: PB Cloud 0.3.0 (feature 003, folder sync)

The tests that prove each story **on the host against the fake Nextcloud** (`tests/support/fake_nextcloud_server.h`). "Walk" is `tests/walks/11-folder-sync.walk`
(CTest `ui_walk`). A passing test proves what this project assumes the server and the reader do; **nothing here is verified against a real Nextcloud server or on
the InkPad One** (the LV-S and DT-S items stay unverified until someone runs them and writes the date). This table maps stories to test files; it does not
repeat each acceptance scenario of [spec.md](spec.md). Scenarios that no test covers are listed at the end.

| Story | Evidence (host-verified) |
|-------|--------------------------|
| US1 Pair a server folder with a reader folder | `test_sync_rules`, `test_store_sync`, `test_store_schema5`, `test_picker_bridge` (folder mode), `test_sync_bridge` (pairs), walk 11 |
| US3 Download only sync | `test_sync_scan_remote`, `test_sync_scan_local`, `test_sync_records`, `test_sync_planner`, `test_downloader_target_root`, `test_sync_download`, `test_sync_hostile_names`, `test_synced_copy` |
| US2 Review before anything changes | `test_sync_plan_review`, `test_sync_preview`, `test_sync_bridge` (compare, plan, approve, cancel, result), walk 11 |
| US5 Start, button, after your change; never in the background | `test_sync_scheduler`, `test_sync_bridge` (triggers), `test_queue_bridge` (`on_user_action`), `test_no_background_network` |
| US8 Interrupted syncs finish with the same result | `test_sync_kill` (kill at every hook of the runner, downloader and sender, clash included), `test_sync_idempotent`, `test_sync_concurrent` |
| US4 Send an Outbox folder | `test_queue_sync_group`, `test_sync_upload`, `test_sync_planner` (rows 7, 13, quota) |
| US6 Both versions are kept | `test_sync_conflict_name`, `test_sync_planner` (rows 9, 10, 14, 21), `test_sync_conflicts` |
| US7 Deletions are proposals | `test_local_remove_pair_copy`, `test_sync_planner` (rows 15-20, folders), `test_sync_deletions`, `test_sync_incomplete`, `test_sync_bridge` (choices) |
| US9 Limits protect the storage | `test_sync_planner_limits`, `test_sync_limits`, `test_sync_bridge` (over limit) |

## Walks (host, with pictures)

`tests/walks/11-folder-sync.walk` (pair, review, approval, result, removal), `12-sync-both-ways.walk` (two-way, keep both), `13-sync-proposals.walk` (proposal,
choice, confirmation naming the file, apply), `14-sync-upload-only.walk` (Outbox upload through the queue), `15-sync-limits.walk` (a plan over the limit and the
limit-raise confirmation), `16-sync-stopped.walk` + `17-sync-interrupted.walk` (the app closed in the middle of a run, the "Interrupted: n of m" line, the start-up
comparison, the next sync finishing the file). Walk 11 also asserts the taps of the shortest way to a saved pair (SC-001, below).

## Measured on the host (UNVERIFIED on the device)

`test_budget_sync` prints the compare time for 1,000 files, the page-turn time, peak memory, database growth at the bounds and `sync_recover` for 20,000 records;
`test_sync_curl` runs a pair download and upload over the real transport and checks that a redirect never carries the credentials; the device package builds
(4.2 MB of 25 MB) and `scripts/probe-device.sh` has the modification-time probe of DT-S2. `test_budget_sync_large` runs a 500 MB file through `SyncRunner` in each direction,
stops it in the middle and repeats the sync (download over the real transport: peak 20 MB on the host, 10 s; upload against the fake that keeps none of the bytes: 21 MB, resumed
from the queue, 501 MB sent in all).

## Not covered by a test or not yet done

- The quickstart steps 5 (Wi-Fi off at start, the Auto Sync delay, no request on a timer or when files appear) and 9 (the reserve of free space) have walks only in part: the start-up half of step 5 is checked by walk 17 (the pair with Sync at start has a plan ready, the others were left alone), the over-limit plan and the limit-raise confirmation by walk 15; the rest is covered by `test_no_background_network`, `test_sync_bridge` and `test_sync_limits`.
- SC-001 counts steps. A step is read as a screen of its own (Add pair, the server folder, the reader folder, Save: 4); walk 11 asserts the taps of the shortest way, which is 8 (a folder screen takes two taps, the folder and Choose), so that it cannot grow unnoticed. If the 6 of SC-001 means taps, the criterion is not met and the pair page needs a shorter way (for example a suggested reader folder); that is the owner's call.
- Everything that needs a real server or the reader: LV-S and DT-S items.
