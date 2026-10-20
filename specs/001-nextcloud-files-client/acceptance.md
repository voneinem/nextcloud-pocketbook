# Acceptance traceability: PB Cloud 0.1.0

Every acceptance scenario of [spec.md](spec.md) and the test that proves it **on the host against the fake Nextcloud** (`tests/support/fake_nextcloud_server.h`).
"Walk" is the scripted walk through the real pages (`tests/walks/*.walk`, CTest `ui_walk`, pictures with `scripts/ui-walk.sh`). A passing test proves what
this project assumes the server and the reader do; **nothing here is verified against a real Nextcloud server or on the InkPad One** (the items LV-N* in
`docs/live-server-checklist.md` and DT-NC-* in `docs/device-test-checklist.md` stay unverified until someone runs them and writes the date).

## US1 Connect

| # | Scenario | Evidence |
|---|----------|----------|
| 1 | Test shows name, version, quota | `test_account_check` (`a_good_account_shows_...`, `the_quota_of_an_unlimited_or_unknown_account`), `test_connection_bridge` (`a_good_test_saves_...`), walk 01 (`settings-tested`) |
| 2 | Browser login shows a link and a QR code; Check before approval says "not yet" and keeps the login | `test_login_flow` (`check_sends_one_request_and_says_not_yet_...`), `test_connection_bridge` (`the_browser_login_goes_through_pending_to_approved_...`, `checking_sends_exactly_one_request_...`), `test_qr_code` |
| 3 | Approval: the account is stored, the token is gone | `test_login_flow` (`after_the_approval_check_returns_the_credentials_once_and_the_token_is_gone`), `test_connection_bridge` |
| 4 | `http://` or no scheme is refused, never retried over http | `test_canonical_url`, `test_connection_bridge` (`a_bare_host_is_completed_and_an_http_address_is_refused_without_any_request`), `test_tls_verification` (`plain_http_urls_are_refused_before_any_connection`) |
| 5 | Wrong credentials | `test_account_check` (`a_wrong_password_is_auth_failed_and_the_check_stops_there`) |
| 6 | Two-factor, account password typed: an app password is needed | `test_account_check` (`a_marker_for_an_app_password_gives_the_distinct_key`); the marker in a real answer is **unverified** (LV-N2) |
| 7 | Maintenance, too old, no file access, not a Nextcloud: own messages | `test_account_check` (`maintenance_mode_is_reported_before_any_login`, `a_server_too_old_and_an_address_that_is_not_a_nextcloud`, `a_server_without_file_access_is_no_webdav`), `test_server_status` |
| 8 | Untrusted certificate: refused; `custom-ca.pem`; Settings shows whether one is loaded; no way to skip | `test_tls_verification`, `test_ca_selector`, `test_effective_ca`; walk 01 (`settings-tested`: "Your own certificate authority is loaded") |
| 9 | Wrong clock is named as such | `test_account_check` (`a_clock_before_the_floor_is_reported_before_anything_is_sent`, `a_difference_to_the_server_time_above_ten_minutes_raises_the_warning`), `test_clock_skew` |
| 10 | Statement on how the app password is stored | `test_connection_bridge` (`the_measured_protection_of_the_credentials_file_is_reported`), `test_credentials_file`; the text in Settings (walk 01); what the reader's storage really does is DT-NC-2 |

## US2 Browse

| # | Scenario | Evidence |
|---|----------|----------|
| 1 | Root listed, folders first, with name, size, date | `test_browse_bridge` (`opening_the_root_fetches_it_once_and_shows_folders_first_...`), walk 01 (`root`) |
| 2 | Pages of a long folder, page n of m | `test_browse_bridge` (`rows_hold_one_page_and_turning_pages_is_local`), `test_budget_cache` |
| 3 | Sort by name, date, size; folders first; kept for the next folder | `test_store_listing`, `test_browse_bridge` (`the_sort_is_applied_kept_for_the_next_folder_and_needs_no_request`) |
| 4 | Up; the path line | `test_browse_bridge` (`the_path_line_and_up_work_from_the_cache`), walk 01 (`books`) |
| 5 | A folder never visited is fetched once and stored | `test_browse_cache` (`an_unvisited_folder_is_fetched_once_with_depth_1_and_the_second_open_sends_nothing`) |
| 6 | Offline: the stored list with its age | `test_browse_cache` (`a_visited_folder_is_shown_from_the_cache_without_any_connection`), `test_browse_bridge` (`a_visited_folder_opens_offline_with_its_age_...`), `test_age_text` |
| 7 | Refresh is the only fetch of a shown list | `test_browse_bridge` (`refresh_is_the_only_call_that_fetches_a_folder_that_is_shown`), `test_browse_cache` (`a_refresh_replaces_the_rows_...`, `a_failed_refresh_keeps_the_old_rows_and_the_old_time`) |
| 8 | Unavailable entries are marked with a reason and cannot be opened | `test_browse_cache` (`entries_the_user_cannot_use_come_back_marked_and_the_rest_works`), `test_listing_parse`, `test_browse_bridge` (`a_file_opens_its_details_and_an_unavailable_entry_only_says_why`); the markers of a real server are **unverified** (LV-N5) |
| 9 | A folder gone on the server | `test_browse_cache` (`a_folder_that_is_gone_on_the_server_stays_readable_and_is_marked`) |

## US3 Download and open

| # | Scenario | Evidence |
|---|----------|----------|
| 1 | Details: size, type, date, on the reader, server newer | `test_transfer_bridge` (`the_details_tell_what_the_file_is_and_whether_a_copy_is_on_the_reader`), walk 01 (`details-*`) |
| 2 | Free space checked, coarse progress | `test_preflight`, `test_transfer_bridge` (`a_download_runs_to_the_end_shows_coarse_progress_...`), `test_budget_download` |
| 3 | Cancel: partial data gone, nothing in the library | `test_download_basic` (`a_cancel_in_the_middle_removes_the_partial_file_and_the_journal_row`), `test_transfer_bridge` (`a_cancel_ends_the_job_and_leaves_no_file`); the library is DT-NC-3 |
| 4 | A dropped connection: resume or restart, file intact | `test_download_resume`, `test_download_curl`; on the reader DT-NC-8 |
| 5 | Interrupted or killed: no partial file listed, clean-up or resume at next start | `test_download_kill`, `test_download_resume` (`a_restart_marks_running_jobs_as_paused_...`), `test_transfer_bridge` (`a_paused_download_is_offered_after_a_restart_and_resumes`) |
| 6 | Not enough space: refused with the numbers | `test_preflight`, `test_download_basic` (`a_file_that_is_too_big_or_does_not_fit_is_refused_before_any_request`), `test_transfer_bridge` (`without_wifi_or_without_room_...`) |
| 7 | A large file asks for confirmation | `test_preflight` (`a_file_at_or_above_the_warning_size_needs_the_users_confirmation`), `test_transfer_bridge` (`a_large_file_waits_for_the_users_confirmation`) |
| 8 | 4 GiB or more is refused before any byte | `test_preflight` (`a_file_of_4_gib_or_more_is_refused_before_anything_else`); the 5 GiB sparse file on the reader is DT-NC-9 |
| 9 | Open in the reader's viewer | `test_transfer_bridge` (`open_hands_only_a_regular_file_inside_the_download_folder_to_the_reader`); the viewer itself is DT-NC-5 |
| 10 | A changed copy is never overwritten without asking (keep mine, keep both, replace after a second confirmation) | `test_download_conflict`, `test_transfer_bridge` (`a_changed_copy_stops_the_download_and_the_choices_do_what_they_say`) |
| 11 | A hostile server name becomes a safe, unique local name | `test_local_name`, `test_name_hostile`, `test_download_basic` (`the_name_on_the_card_is_a_safe_version_of_the_name_on_the_server`) |

## US4 Keep offline and update

| # | Scenario | Evidence |
|---|----------|----------|
| 1 | Mark shown on the entry, survives a restart | `test_store_marks` (`marks_survive_a_restart`), `test_browse_bridge` (`each_row_carries_the_state_of_its_copy_and_whether_it_is_kept_offline`), walk 01/02 |
| 2 | Update downloads only what differs and reports counts | `test_update` (`only_what_changed_or_is_new_is_downloaded`, `twenty_kept_files_three_changed_downloads_exactly_those_three` = SC-011), `test_transfer_bridge`, walk 01 (`update-report`), walk 02 (`update-second`: exactly one file updated) |
| 3 | No tap, no check | `test_no_background_network` (`kept_items_are_never_checked_or_downloaded_without_the_tap_on_update`), `test_transfer_bridge` (`marks_are_set_and_removed_and_nothing_is_fetched_by_marking`) |
| 4 | A changed server version shows as outdated | `test_entry_state`, `test_browse_bridge` (`each_row_carries_the_state_...`) |
| 5 | A copy changed on the reader is not replaced; shown as changed | `test_update` (`a_copy_the_user_changed_while_the_server_stayed_the_same_is_just_skipped`), `test_entry_state` |
| 6 | Changed on both sides: both kept until the user decides | `test_update` (`a_copy_changed_on_the_reader_and_on_the_server_is_a_conflict_that_is_left_alone`), `test_download_conflict` (`keep_both_...`) |
| 7 | Gone on the server: the copy stays, flagged | `test_update` (`what_vanished_on_the_server_is_flagged_and_its_copy_stays`, `a_kept_folder_that_is_gone_flags_the_copies_below_it`) |
| 8 | An interrupted Update, repeated | `test_update` (`a_repeated_update_after_an_interruption_ends_as_an_uninterrupted_one_and_downloads_nothing_twice`) |

## US5 Favorites and search

| # | Scenario | Evidence |
|---|----------|----------|
| 1 | Favorites fetched and shown; offline the stored list with its age | `test_favorites_search`, `test_browse_bridge` (`favorites_are_fetched_once_...`, `favorites_stay_readable_offline_...`), `test_store_favorites`, walk 02 (`favorites`); the request shape is **unverified** (LV-N8) |
| 2 | Offline search: cached entries, marked as from the reader, no network | `test_store_favorites`, `test_browse_bridge` (`the_offline_search_is_local_...`), `test_no_background_network` (`a_search_typed_offline_...`), walk 02 (`search-offline`) |
| 3 | Search the server too: results marked as server results | `test_favorites_search`, `test_browse_bridge` (`the_server_search_runs_on_the_explicit_tap_only_...`), walk 02 (`search-server`); the request shape is **unverified** (LV-N9) |
| 4 | A result behaves like the entry in a listing | `test_browse_bridge` (`the_server_search_runs_..._` opens details, `opening_a_folder_result_goes_back_to_the_folder_mode`), `test_store_favorites` (`server_search_results_are_held_in_memory_only_...`) |

## US6 Manage space

| # | Scenario | Evidence |
|---|----------|----------|
| 1 | The list with sizes, used and free space | `test_space_bridge` (`the_downloads_are_listed_with_size_and_state_and_the_totals_are_shown`), walk 02 (`storage`) |
| 2 | Remove: the file is gone, the server is not contacted | `test_space` (`removing_a_copy_deletes_the_file_and_its_row_and_sends_no_request` = SC-013), `test_space_bridge` (`removing_frees_space_...`), walk 02 (`storage-remove-dialog`, `storage-after-remove`) |
| 3 | A changed copy needs an explicit confirmation | `test_space` (`a_copy_with_changes_is_refused_unless_the_loss_is_confirmed`, `a_conflict_copy_needs_the_confirmation_as_well`), `test_space_bridge` (`a_changed_copy_is_removed_only_with_the_confirmation`) |
| 4 | The cache limit drops the oldest lists, never downloads, marks or changes | `test_store_listing` (`the_entry_limit_evicts_whole_folders_and_keeps_the_folder_just_written`, `eviction_never_touches_downloads_marks_favorites_or_the_journal`), `test_budget_cache` (the database stays within 50 MB at the limits), `test_space` (`clearing_the_cache_drops_the_listings_and_nothing_the_user_owns`) |

## Scenarios without a test of their own, and what is open

- No scenario is without a host test. The scripted walk covers US1 (typed credentials), US2, US3 (download), US4, US5 and US6 on the real pages; the browser login
  and a dropped connection are covered by the bridge and integration tests, not by the walk.
- Everything that depends on the real server or the reader is open and listed in `docs/live-server-checklist.md` (LV-N1 ... LV-N9) and
  `docs/device-test-checklist.md` (DT-NC-1 ... DT-NC-10): among them the format of `oc:checksums`, the markers of encrypted and shared entries, the favorites and
  search request bodies, `Range`/`If-Range`, the reader's library finding `PBCloud/`, and every budget (the host figures in DT-NC-7 are labelled as host).
- The pages were looked at only offscreen on a desktop at 1404 x 1872; e-ink refresh behavior, the on-screen keyboard, Back-key handling and rotation are device items.
