# Acceptance traceability: PB Cloud 0.2.0 (feature 002, upload and manage files)

Every acceptance scenario of [spec.md](spec.md) and the test that proves it **on the host against the fake Nextcloud** (`tests/support/fake_nextcloud_server.h`).
"Walk" is the scripted walk through the real pages (`tests/walks/03` ... `10`, CTest `ui_walk`, pictures with `scripts/ui-walk.sh`). A passing test proves what this
project assumes the server and the reader do; **nothing here is verified against a real Nextcloud server or on the InkPad One** (LV-W1 ... LV-W12 in
`docs/live-server-checklist.md` and DT-W1 ... DT-W9 in `docs/device-test-checklist.md` stay unverified until someone runs them and writes the date).

## US1 Upload files

| # | Scenario | Evidence |
|---|----------|----------|
| 1 | The browser opens with the remembered layout | `test_picker_bridge`, walk 03 (`picker-root`, `picker-tiles`) |
| 2 | Picks are queued as "not synced", no request | `test_queue_bridge` (`picking_files_queues_them_...`), `test_no_background_network`, walk 03 |
| 3 | Another target folder | the **Target folder** tool of the picker opens the folder chooser (`FolderChooserPage`, `test_folder_chooser_bridge`); only exercised by the app self test, no walk |
| 4 | Offline picks | `test_queue_bridge` (`offline_picks_are_queued_exactly_as_online_...`) |
| 5 | Refused picks with a reason | `test_source_hostile`, `test_queue_bridge` (`files_that_cannot_be_sent_are_refused_one_by_one_...`) |
| 6 | Folder pick: counts, confirmation, one folder per folder and one upload per file | `test_queue_actions`, `test_queue_bridge`, walk 03 (`picker-confirm`) |
| 7 | An existing server folder is used, files inside ask | `test_collisions` (`a_folder_pick_into_an_existing_folder_...`) |
| 8 | Unreadable, links, over the limit | `test_source_walk`, `test_source_hostile`, `test_queue_bridge` (`a_pick_over_the_limit_...`) |
| 9 | Removing a queued item leaves the file | `test_queue_bridge` (`removing_a_queued_item_...`), `test_source_hostile` |
| 10 | The queue survives a kill | `test_queue_recover`, `test_queue_kill`, `test_queue_bridge` (`the_queue_is_written_before_the_bridge_answers_...`), walks 03 to 04 (restart between them) |

## US2 Send, Auto Sync

| # | Scenario | Evidence |
|---|----------|----------|
| 1 | Auto Sync off: nothing by itself; Send sends in order | `test_queue_bridge` (`with_auto_sync_off_...`), `test_no_background_network`, walk 03 |
| 2 | Auto Sync on: one send after the quiet period, actions within it go together | `test_queue_bridge` (`auto_sync_sends_once_...`), `test_auto_sync`, walk 03 |
| 3 | Offline at the end of the delay: waits, told, no retry | `test_queue_bridge` (`when_the_quiet_period_ends_without_wifi_...`), `test_no_background_network` |
| 4 | Nothing sent at start | `test_no_background_network` (`a_queue_left_by_an_earlier_session_...`) |
| 5 | A dropped connection, then the missing chunks only | `test_upload_resume`, walk 03 (`queue-connection-dropped`, `queue-resumed`) |
| 6 | A changed source starts over | `test_upload_resume` (`a_source_that_changed_...`) |
| 7, 8 | Outcome unknown and "Check the server" | `test_unknown_outcome`, `test_queue_kill`, `test_queue_bridge`, walk 04 |
| 9 | Cancel after the piece in flight | `test_queue_bridge` (`..._cancel_stops_after_the_piece_in_flight`), `test_upload_resume` (`cancel_stops_...`) |
| 10 | The screen off or Wi-Fi dropping: nothing lost | `test_upload_resume` (`wifi_going_down_...`, `no_connection_at_all_...`), `test_queue_kill` |
| 11 | A source that vanished fails alone | `test_queue_bridge` (`retry_sends_that_one_failed_item_...`) |
| 12 | "Not synced" in the listing until confirmed | `test_overlay`, `test_upload_small` (`a_confirmed_upload_enters_the_cached_listing_...`), `test_rename_mkdir` |

## US3 Never overwrite silently

| # | Scenario | Evidence |
|---|----------|----------|
| 1, 2, 3 | Keep both / replace after a confirmation / skip | `test_collisions`, `test_queue_bridge`, walk 05 (`collision`, `keep-both-name`, `replace-confirm`) |
| 4 | A replace on the stored version | `test_write_conditions`, `test_conflicts`, `test_upload_small` |
| 5 | A changed server version holds the item; keep both, replace anyway, skip | `test_conflicts`, `test_queue_bridge` (`a_held_replace_is_resolved_...`), walk 05 |
| 6 | A name the reader could not see | `test_collisions` (`a_listing_that_was_never_fetched_...`), `test_conflicts` (`a_name_that_appeared_after_queuing_...`) |
| 7 | The copy of the entry itself is linked | `test_local_follow` |
| 8 | The file as it is at send time is sent | `test_upload_small` (`a_file_that_changed_between_the_pick_and_the_send_...`) |
| 9 | Collisions for rename, move, copy, new folder | `test_rename_mkdir` (a taken name is `name_exists`, no replace of a rename), `test_move_copy`, `test_rename_mkdir` (new folder shows the existing one) |
| 10 | Nothing replaced that the user did not decide | `test_write_conditions` (every write has its condition), `test_conflicts`, `test_upload_chunked` |

## US4 Folders and rename

| # | Scenario | Evidence |
|---|----------|----------|
| 1, 2 | New folder queued; an upload into it waits for it | `test_rename_mkdir`, walk 06 |
| 3 | Rename queued and shown | `test_rename_mkdir`, walk 06 |
| 4 | The copy and the mark follow | `test_rename_mkdir` (`the_downloaded_copy_and_the_offline_mark_...`, `renaming_a_folder_...`) |
| 5 | Invalid names refused at once | `test_rename_mkdir`, `test_new_name`, walk 06 (`new-folder-refused`) |
| 6 | A change of case only | `test_rename_mkdir` (`a_case_only_rename_...`) |
| 7 | A changed or vanished entry is held | `test_rename_mkdir` (`an_entry_that_changed_or_vanished_...`) |
| 8 | Offline rename equals online | the queue is the same code online and offline (`test_queue_bridge` `offline_picks_...`, `test_rename_mkdir`); no separate offline test |

## US5 Move and copy

| # | Scenario | Evidence |
|---|----------|----------|
| 1 | Queued, shown as moving or copying | `test_move_copy`, `test_overlay`, walk 07 |
| 2 | A taken name asks per entry | `test_move_copy` (`a_taken_name_at_the_destination_asks_...`), `test_queue_bridge` |
| 3 | A folder goes with its content, shown before confirming | `test_move_copy`, walk 07 (the confirmation text) |
| 4 | Refused destinations | `test_move_copy` (`a_destination_that_is_the_source_...`), walk 07 (`refused`) |
| 5 | A changed or vanished source is held | `test_move_copy` |
| 6 | The copy and the mark follow a move | `test_move_copy` (`a_confirmed_move_takes_the_copy_...`) |
| 7 | No content through the reader | `test_move_copy` (the fake's byte counters) |
| 8 | A quota problem at the destination | `test_move_copy` (`a_full_destination_...`) |

## US6 Delete

| # | Scenario | Evidence |
|---|----------|----------|
| 1 | Confirmation names the trash, folders say "everything inside" | `test_queue_bridge` (`delete_asks_first_...`), walk 08 |
| 2 | Stays as "Delete pending" until confirmed; can be cancelled in the queue | `test_delete_trash`, walk 08 |
| 3 | Deleted into the trash on the stored version | `test_delete_trash` |
| 4 | A changed entry is held; keep or delete anyway | `test_delete_trash` (`an_entry_that_changed_...`) |
| 5 | The local copy stays | `test_delete_trash` (`the_downloaded_copy_and_its_mark_...`) |
| 6 | No trash: not offered | `test_delete_trash`, walk 10 |
| 7 | Warning about queued items inside a folder | `test_delete_trash` (`the_confirmation_data_names_...`), `test_queue_bridge`; the items fail with `target_gone` at send |

## US7 Clear messages

| # | Scenario | Evidence |
|---|----------|----------|
| 1, 2 | A full account, before any byte when known | `test_refusals` (`a_full_account_is_found_out_...`), walk 09 |
| 3 | A forbidden name; edit | `test_refusals` (`a_refused_name_can_be_edited_...`) |
| 4 | Too large, wrong type | `test_refusals` (`the_size_the_type_and_the_name_...`) |
| 5 | Locked, not retried by itself | `test_refusals`, `test_delete_trash`, walk 09 |
| 6 | An unknown answer shows only a code | `test_refusals`, `test_sabre_error_hostile`, `test_failure_messages` |
| 7 | A refused login: queue untouched, "sign in again" | `test_refusals` (`a_refused_login_...`), `test_queue_bridge`; the notice names Settings, there is no button that jumps there |
| 8 | Reasons and Retry, Edit, Remove | `test_queue_bridge`, `test_refusals`, walk 09 |

## Success criteria and budgets

SC-004 and SC-011 are measured on the host by `test_budget_upload` and `test_budget_queue` (figures printed); the device values stay **unverified** (DT-W3, DT-W4,
DT-W7). SC-003 (no background network): `test_no_background_network`. SC-007 and SC-008 (every write is conditional): `test_write_conditions`.

## Wording of Auto Sync (FR-012, FR-043)

PB Notes (`qml/pages/SettingsPage.qml`) has a section **Synchronization** with a row **Auto Sync** and a pill switch, and no German text for it; PB Office calls its
switch "Sync after my changes". PB Cloud follows PB Notes: section **Synchronization**, label **Auto Sync** (the same in German), the same pill switch. The explanatory line
differs on purpose: PB Notes says it switches Wi-Fi on if needed, PB Cloud never does (research R-14, FR-042).

## Deviations from the plan, recorded

- A replace of a file is a conditional delete of the old version followed by a request that cannot overwrite (see `docs/live-server-checklist.md`); a chunked replace
  is refused where the server has no trash.
- Queue logic (collisions, folder picks, move, delete) lives in the core (`QueueActions`), not in the bridge.
- The kill tests simulate the death with an exception at the sender's test seam and reopen the same data folder (a forked child would lose the fake server's state).
- After a change the reader made, the cached versions of the folders above it are forgotten, so a later change of such a folder asks for a refresh instead of
  holding a false conflict.
- QML is covered by the app self test (every page is instantiated) and the walks, not by `tests/qml`.
