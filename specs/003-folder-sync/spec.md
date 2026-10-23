# Feature Specification: Folder Synchronization

**Feature Branch**: `feature/003-folder-sync`

**Created**: 2026-10-21

**Status**: Draft

**Input**: User description: "Folder synchronization between chosen Nextcloud folders and folders on the reader. Builds on features 001 and 002. The user pairs a server folder with a folder on the reader (for example a \"Books\" folder to read from, and an \"Outbox\" folder on the reader whose new files go to the server). Direction per pair: download only (default), upload only, or two-way. A sync runs only at app start (when Wi-Fi is already on), on the Sync button, or shortly after the user's own change; there is never a background sync. Before anything is changed the user can review what a sync will do (what is downloaded, uploaded, skipped), and the result is shown afterwards with counts. Deletions are never propagated automatically: a file removed on one side is shown as a proposal that needs confirmation. Changes on both sides keep both versions. The sync is resumable and idempotent: repeating an interrupted sync gives the same result. Bounded by a configurable file count and total size per pair so that the reader's storage cannot be filled unnoticed. Out of scope: photo auto-upload, live file watching, more than the paired folders."

## Clarifications

### Session 2026-10-21

- Q: When a sync starts by itself (at app start or through Auto Sync), may it change files without the user looking at the plan first? → A: No. Always review: such a sync only compares and prepares a plan, marks the pair "plan ready", and changes nothing until the user approves the plan.
- Q: When a plan would take a pair over its file-count or size limit, may the user approve a partial sync that takes only the entries that fit? → A: No. The whole plan is refused until the user raises the limit or removes entries so that it fits; the app never picks entries on its own.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Pair a server folder with a reader folder (Priority: P1)

The user wants a Nextcloud folder, for example "Books", to be available on the reader without picking files one by one, and another folder on the reader, for example "Outbox", to hand its new files to the cloud. They open Sync in the app and add a pair: they choose a folder on the server (with the same folder chooser that works from stored listings, also offline), a folder on the reader's storage (with the reader-side file browser of feature 002, or a new folder created for the purpose), and a direction: download only (the default), upload only, or two-way. Before the pair is saved the app states in plain words what the direction means ("files from the server are copied to the reader; nothing on the reader is sent") and shows the limits that will apply (file count and total size). Saving a pair changes nothing on either side: the first sync is a separate, reviewed step. The user can see all pairs in a list with direction, last sync time and state, change a pair's direction and limits, and remove a pair; removing a pair never deletes any file on either side.

**Why this priority**: Nothing else in this feature exists without a pair, and the safe default (download only) makes the first use harmless.

**Independent Test**: Against a simulated server, add a pair "Books" to a reader folder with the default direction, see it listed with "never synced" and no network request or file change caused by saving, change its direction to two-way after a confirmation, then remove it and verify that every file on both sides is still present.

**Acceptance Scenarios**:

1. **Given** the Sync screen, **When** the user taps Add pair, **Then** they choose a server folder, a reader folder and a direction, the direction defaults to download only, and a summary of what the direction means is shown before saving.
2. **Given** the reader has no connection, **When** the user adds a pair, **Then** the server folder is chosen among folders the reader already knows and the pair is saved; the first sync waits for a connection.
3. **Given** a pair is saved, **When** the user looks at the pair list, **Then** each pair shows its server folder, reader folder, direction, limits, the time and outcome of its last sync (or "never synced"), and how many proposals or conflicts are open.
4. **Given** the user picks a reader folder that is the storage root, the app's own data folder, the folder where the app keeps its downloads for feature 001, or a folder inside or containing another pair's reader folder, **When** they confirm, **Then** the pair is refused with the reason.
5. **Given** the user picks a server folder that is inside or contains the server folder of another pair, **When** they confirm, **Then** the pair is refused with the reason.
6. **Given** a pair with a direction other than download only is being saved, **When** the user confirms, **Then** the app states which side's files can be sent to the other side and requires an explicit confirmation naming both folders.
7. **Given** a pair, **When** the user changes its direction, **Then** the change takes effect from the next sync, files already transferred stay where they are, and nothing is deleted on either side because of the change.
8. **Given** a pair, **When** the user removes it and confirms, **Then** the pair and its sync records disappear, all files on the reader and on the server stay untouched, and queued items of feature 002 that came from the pair stay in the queue until the user removes them.

---

### User Story 2 - Review what a sync will do, then see the result (Priority: P1)

The user starts a sync of a pair with the Sync button. The app first compares the two sides and shows a plan, before changing anything: the files that would be downloaded, the files that would be uploaded, the files that are skipped with the reason for each (already in sync, over the limit, name not allowed, unreadable, locked, and so on), the changes that exist on both sides, and the proposals for deletions. The plan shows counts and total sizes per kind and is a paged list of entries, with free space on the reader and the quota on the server where known. The user approves the plan, approves part of it by removing entries, or cancels, in which case nothing has happened. Only an approved plan is carried out. When the sync has finished, a result is shown with counts: downloaded, uploaded, skipped, kept both, proposals open, failed (with reasons), and the time it took; the last result of each pair stays available on the Sync screen.

**Why this priority**: Review before change is the safety promise of the feature; without it a pairing could fill the reader or touch the cloud unnoticed.

**Independent Test**: With a simulated server holding five new files and a reader folder holding two new files, start a two-way sync, see a plan listing five downloads and two uploads with counts and sizes while nothing has changed on either side, remove one download from the plan, approve, and verify that the result shows 4 downloaded, 2 uploaded, 1 skipped and that the file removed from the plan is still not on the reader.

**Acceptance Scenarios**:

1. **Given** a pair, **When** the user taps Sync, **Then** the app compares both sides, shows the plan, and makes no change to any file on either side until the user approves.
2. **Given** a plan, **When** it is displayed, **Then** it lists every entry with its action (download, upload, skip with reason, keep both, deletion proposal), its size, and counts and total sizes per action, in pages with page buttons and without kinetic scrolling.
3. **Given** a plan, **When** the user removes some entries from it and approves, **Then** only the remaining entries are carried out and the removed ones are reported as skipped by the user; they appear again in the next plan.
4. **Given** a plan, **When** the user cancels, **Then** no file on either side has changed and no queued item was created.
5. **Given** a plan whose downloads do not fit in the reader's free space, **When** it is displayed, **Then** the plan says so before approval, and the entries that would not fit are marked and cannot be approved.
6. **Given** a sync has finished, **When** the result is shown, **Then** it states the counts of downloaded, uploaded, skipped, kept both, open proposals and failed entries, the reasons for failures, and the time of the sync, and it stays available as the last result of the pair.
7. **Given** a pair with nothing to do, **When** the user taps Sync, **Then** the app says the pair is in sync, with the time of the comparison, and no transfer happens.
8. **Given** the reader has no connection, **When** the user taps Sync, **Then** the app says it is offline and compares nothing, and it offers to show what is known from stored listings without claiming it is current.

---

### User Story 3 - Keep a reader folder up to date from the server (Priority: P1)

The user has a server folder "Books" paired, download only. On the first sync the books on the server are copied into the paired reader folder, in the same folder structure, so the reader's own library can find them. On later syncs only what is new or changed on the server is downloaded; files that are already identical are not downloaded again. Files in the reader folder that the user put there, edited, or kept from earlier are never overwritten by a download; if a server file changes while the reader's copy was changed too, both versions are kept. With a download-only pair nothing on the reader is ever sent to the server.

**Why this priority**: Reading from the cloud is the most common use and is the safest direction; it is the minimum viable product of this feature.

**Independent Test**: Pair a simulated server folder with two files and a subfolder to an empty reader folder, sync, and verify identical files in the same structure; change one file and add one on the server, sync again and verify that exactly those two are downloaded and nothing else; put an extra file into the reader folder and verify that it is never sent and never removed.

**Acceptance Scenarios**:

1. **Given** a download-only pair with an empty reader folder, **When** the plan is approved, **Then** every file of the server folder and its subfolders is downloaded into the same structure and is complete, and shows no partial file at any time.
2. **Given** an earlier sync, **When** a file changed on the server, **Then** the next plan lists it as a download, and after approval the reader's copy equals the server's version.
3. **Given** a file that is already on both sides with the same content, **When** a plan is made, **Then** the entry is skipped as "already in sync" and is not transferred again.
4. **Given** the reader folder contains a file that does not exist on the server, **When** a download-only sync runs, **Then** the file is left alone, is not sent and is not offered for deletion.
5. **Given** a file the user changed on the reader and the server version changed too, **When** a plan is made, **Then** both versions are kept (Story 6), and the reader's version is not overwritten.
6. **Given** a name from the server that cannot be used on the reader's storage (separators, reserved characters, too long, case-only difference from another name), **When** a plan is made, **Then** it is shown with the safe name it would get, or, when no safe name can be formed, as skipped with the reason.
7. **Given** a file on the server that is not available to the user (no permission, encrypted folder), **When** a plan is made, **Then** it is listed as skipped with the reason and does not stop the rest.
8. **Given** a pair, **When** files are downloaded by a sync, **Then** they are shown as downloaded in the folder listings of feature 001 as well, and removing a pair keeps them.

---

### User Story 4 - Send an Outbox folder to the server (Priority: P2)

The user pairs a reader folder "Outbox" with a server folder "Inbox", upload only. They scan, convert or export files into the Outbox on the reader. On a sync, the new files in the Outbox are found, shown in the plan as uploads, and, once approved, sent into the server folder. The files stay in the Outbox: nothing on the reader is moved or deleted after the upload, and the app records which files have been sent so that they are not sent again. If the user edits a file that was already sent, it appears in the next plan as changed and is sent as a new version of that file after the user approved it, never replacing the server's version silently. With an upload-only pair nothing from the server is ever copied to the reader.

**Why this priority**: It completes the cloud round trip for files created on the reader, but reading from the cloud is needed first.

**Independent Test**: Put three files and one subfolder with a file into an Outbox paired upload only to an empty simulated server folder, sync, and verify that four files and the folder exist on the server with identical content, that the Outbox is unchanged, and that a second sync sends nothing.

**Acceptance Scenarios**:

1. **Given** an upload-only pair, **When** a plan is made, **Then** new files and new subfolders of the reader folder are listed as uploads, and nothing from the server is listed as a download.
2. **Given** an approved plan, **When** the sync runs, **Then** each file is sent through the queue of feature 002, so that chunking, conditional writes, name collisions, the "outcome unknown" flag and clear refusal messages behave as specified there.
3. **Given** a file already sent, **When** the next plan is made, **Then** it is skipped as "already in sync" and no upload is made again.
4. **Given** a file already sent that the user changed on the reader, **When** the next plan is made, **Then** it is listed as a changed upload, and when the server's version also changed since the last sync, both versions are kept (Story 6) instead of replacing the server file.
5. **Given** an upload whose outcome is unknown, **When** later syncs run, **Then** that file is not sent again by the sync; it is handled with the choices of feature 002 until the user decides.
6. **Given** a file in the reader folder that is empty, unreadable, 4 GiB or larger, or whose name the server forbids, **When** a plan is made, **Then** it is listed as skipped with the reason and does not stop the others.
7. **Given** an upload finished, **When** the user looks at the Outbox, **Then** all files are still there unchanged.

---

### User Story 5 - Choose when a sync runs, never in the background (Priority: P1)

The user decides when synchronization happens. A sync of a pair runs in these cases only: when the app starts and Wi-Fi is already on; when the user taps Sync (for one pair or for all pairs); and, when Auto Sync is on, a short stated time after the user's own change related to a pair. The user's own changes are those made in the app that touch a paired folder, such as queuing an upload into the paired server folder, deleting, renaming or moving there, or changing the pair's settings. There is no timer, no waiting for Wi-Fi to appear, no watching of folders for new files, and no activity while the app is closed. A file the user copies into a paired reader folder from outside the app (for example by USB) is found at the next start or Sync, not by watching. Each pair can have its start-up sync and its Auto Sync switched on or off; every started sync follows the rules of review in Story 2; a sync started at app start or by Auto Sync only compares and prepares the plan, and nothing is changed until the user has opened the plan and approved it.

**Why this priority**: The reader's battery and the user's trust depend on sync being predictable; this is also a constitutional rule.

**Independent Test**: With two pairs, start the app with Wi-Fi on and see the start-up comparison happen once; leave the app idle for an hour and verify no request is made; switch Wi-Fi off and on and verify no request is made; tap Sync and verify one run; with Auto Sync on, queue an upload into a paired folder and verify a sync of that pair after the stated delay and none for other pairs.

**Acceptance Scenarios**:

1. **Given** the app starts and Wi-Fi is already on, **When** a pair has start-up sync switched on, **Then** the pair is compared once, and the app only compares and prepares a plan, changes nothing on either side, and marks the pair "plan ready" until the user opens the plan and approves it (Story 2).
2. **Given** the app starts and Wi-Fi is off, **When** the app is used, **Then** no request is made and the pairs show "not synced" with the age of their last sync; the app never turns Wi-Fi on.
3. **Given** any time after start, **When** time passes or Wi-Fi comes on, **Then** no sync starts by itself.
4. **Given** the user taps Sync on the Sync screen, **When** several pairs exist, **Then** they can sync one pair or all pairs, and with all pairs one plan per pair is shown in turn.
5. **Given** Auto Sync is on for a pair, **When** the user makes a change in the app that touches the pair's folders, **Then** a comparison of that pair is started after a short stated delay, further changes within the delay are covered by the same comparison, the resulting plan waits for approval (scenario 1), and comparisons of other pairs are not started.
6. **Given** Auto Sync is on and the reader is offline when the delay ends, **When** the sync cannot start, **Then** nothing is retried by itself and the pair shows that it is waiting.
7. **Given** a sync is running, **When** the user taps Cancel, **Then** it stops after the item in flight, completed items stay completed, and the rest stay planned for the next sync.
8. **Given** a sync is running, **When** the screen turns off or Wi-Fi drops, **Then** nothing is lost, the sync stops cleanly, and the pair shows that it was interrupted.

---

### User Story 6 - Changes on both sides keep both versions (Priority: P2)

For a two-way pair, or any pair where a file was changed on the reader and on the server since the last sync, the app never decides which version wins. The plan shows the entry as a conflict. When the sync is carried out, both versions are kept: the server's version stays as it is, the reader's version stays as it is, and the other side receives the version it does not have under a new, clearly marked name (for example the file name with a marker and a date). The user is told which two files exist, and decides later what to keep, using ordinary file management on either side. The same holds when the same name was created independently on both sides before the first sync and the contents differ, and when a file was deleted on one side and changed on the other.

**Why this priority**: Last-writer-wins would destroy data; keeping both is the constitutional answer, but it only matters once two-way sync or edits exist.

**Independent Test**: Sync a two-way pair, change the same file on both sides with different content, sync again, and verify that both contents exist on both sides under distinct names, that neither original name lost its own version, and that a repeated sync does not create further copies.

**Acceptance Scenarios**:

1. **Given** a file changed on both sides since the last sync, **When** a plan is made, **Then** it is listed as a conflict with both sizes and dates, and nothing is overwritten on either side.
2. **Given** an approved plan with a conflict, **When** the sync runs, **Then** each side ends up with both versions, the original name keeps the version that side had, and the other version is added under a new name that did not exist.
3. **Given** a conflict has been resolved by keeping both, **When** the next sync runs, **Then** the conflict is not detected again and no further copies are created.
4. **Given** the same file name exists on both sides with different content and the pair has never synced that file, **When** a plan is made, **Then** it is treated as a conflict, and a file with the same name, the same size and, where the server offers a checksum, the same checksum is treated as already in sync.
5. **Given** a file deleted on one side and changed on the other, **When** a plan is made, **Then** the changed version is kept and the deletion is not carried out; the entry is shown to the user.
6. **Given** a conflict in a download-only or upload-only pair, **When** a plan is made, **Then** the version of the side that is not transferred stays untouched and the other version is kept under a new name only on the receiving side.

---

### User Story 7 - Deletions are proposals, never automatic (Priority: P2)

When a file that was synced earlier is gone from one side, the app does not delete its counterpart. The next plan lists the file in a separate section "Deleted on the server" or "Deleted on the reader" as a proposal, with the name, size and which side still has it. For each proposal the user can choose: keep it (and have it transferred again to the side that lacks it, or leave both sides as they are and stop proposing), or confirm the deletion on the other side. A confirmed deletion of a file on the server goes to the server's trash, uses the stored version marker, and is queued as in feature 002; a confirmed deletion on the reader removes the local file only after the user confirmed it by name and only if the local file has no changes since the last sync. No proposal is ever carried out by an approval of the rest of the plan: it requires its own confirmation, and approving a plan never confirms deletions.

**Why this priority**: It is the riskiest part of any sync; making it explicit and opt-in keeps the feature safe, but the feature is useful without carrying any deletion out.

**Independent Test**: Sync a two-way pair with three files, delete one on the server and one on the reader, sync again, and verify that both files are listed only as proposals, that approving the plan alone deletes nothing, and that confirming one proposal deletes exactly that file on the other side (server trash, or reader), leaving the other unchanged.

**Acceptance Scenarios**:

1. **Given** a file synced earlier that is now missing on the server, **When** a plan is made, **Then** it is listed under "Deleted on the server" as a proposal and its local copy stays untouched.
2. **Given** a file synced earlier that is now missing on the reader, **When** a plan is made, **Then** it is listed under "Deleted on the reader" as a proposal and its server file stays untouched.
3. **Given** a plan with proposals, **When** the user approves the plan without acting on the proposals, **Then** no deletion is carried out on either side, and the proposals stay for the next plan.
4. **Given** a proposal, **When** the user confirms the deletion, **Then** the confirmation names the file and the side it will be deleted on, and states that a server file goes to the server's trash and a reader file is removed from the reader's storage and cannot be recovered through this app.
5. **Given** a confirmed server deletion, **When** it is queued, **Then** it behaves as a deletion of feature 002 (conditional on the stored version marker, refused when the server has no trash, conflict when the server file changed meanwhile).
6. **Given** a proposal for a file on the reader that has been changed since the last sync, **When** the user looks at the proposal, **Then** deletion is not offered; the file is kept and transferred again to the side that lacks it, if the user chooses so.
7. **Given** a proposal, **When** the user chooses to keep it, **Then** the file is either transferred again to the side that lacks it, or both sides are left as they are and the entry is no longer proposed unless it changes again.
8. **Given** a whole folder missing on one side, **When** a plan is made, **Then** it is listed as one proposal naming the folder and the number of files it contains, and confirming it covers only the files the pair had synced, never files added later.

---

### User Story 8 - Interrupted syncs finish with the same result (Priority: P1)

A sync can be interrupted at any point: Wi-Fi drops, the screen turns off, the battery dies, the app is killed or the user cancels. Repeating the sync gives exactly the result an uninterrupted sync would have given. Files already transferred completely are not transferred again; a download in progress resumes or restarts cleanly with no partial file visible in the library; an upload in progress continues through the queue of feature 002; an upload whose outcome is unknown is kept and flagged and not repeated by the sync. The record of what has been synced is only advanced for a file after that file is complete on both sides, so an interruption never makes the app believe something was synced that was not. Running the same sync twice in a row, with no changes between, makes no second transfer and creates no further files.

**Why this priority**: Data safety and trust; a sync that cannot be repeated safely cannot be used on a flaky Wi-Fi connection.

**Independent Test**: Run a sync of 20 downloads and 5 uploads and kill the app after each transferred item in turn; after each kill restart and sync again, and verify that the final state of both sides and of the sync records is identical to an uninterrupted run, that no file is transferred twice, and that no partial file is ever visible.

**Acceptance Scenarios**:

1. **Given** a sync was interrupted, **When** the user syncs the pair again, **Then** a new plan lists only what is still to do, and the final state equals that of an uninterrupted sync.
2. **Given** a completed sync, **When** the user syncs again without any change on either side, **Then** the plan is empty, nothing is transferred, and no new file or copy is created.
3. **Given** a download interrupted by a connection drop, **When** the sync is repeated and the server's file is unchanged, **Then** the download resumes or restarts cleanly, and no partial file was ever visible under its final name.
4. **Given** the app is killed while a plan is being carried out, **When** it starts again, **Then** the pair shows that its last sync was interrupted with the counts done so far, and no file was lost.
5. **Given** a file changed on either side while a sync was running, **When** the sync reaches it, **Then** it is not transferred with a mixture of old and new content; it is left for the next plan and reported as changed during the sync.
6. **Given** two syncs started at the same time for the same pair (for example a tap and an Auto Sync), **When** the second starts, **Then** it does not run in parallel; the user is told one is already running.
7. **Given** a power loss at any moment, **When** the app restarts, **Then** the sync records are consistent, and no file on either side is lost, duplicated, or half-written.

---

### User Story 9 - Limits protect the reader's storage (Priority: P2)

Every pair has a limit on the number of files and on the total size it may hold on the reader, both configurable by the user, with stated defaults. The limits are checked when a plan is made: a plan that would take the pair over a limit is not carried out as it is. The plan shows how far over the limit it would go and the entries that fit; the user can raise the limit consciously, remove entries from the plan, or leave the pair as it is. The Sync screen shows for each pair how many files and how much storage it uses on the reader and how much of the limits that is, together with the reader's free space. A sync never leaves the storage so full that the reader itself cannot work: a reserve of free space is kept and cannot be used by a sync.

**Why this priority**: It prevents the failure that a forgotten pairing silently fills the reader; it is safety rather than the core function.

**Independent Test**: Pair a simulated server folder with 50 files to a pair limited to 20 files and 100 MB, sync, and see a plan stating that it is over both limits and carrying out nothing without a decision; raise the limit or remove entries, approve, and verify that the pair never holds more than the limit and that the free-space reserve was kept.

**Acceptance Scenarios**:

1. **Given** a new pair, **When** it is saved, **Then** it has stated default limits for file count and total size, which the user can change when adding the pair and later.
2. **Given** a plan that would exceed a limit, **When** it is displayed, **Then** it states which limit is exceeded and by how much, and the plan cannot be approved as it is. The user can raise the limit consciously, or remove entries from the plan until it fits; there is no partial sync that takes entries on its own, and nothing is transferred until the plan fits.
3. **Given** a start-up or Auto Sync, **When** its plan would exceed a limit, **Then** the plan is marked "plan ready, over limit" and nothing is transferred until the user decides.
4. **Given** a pair, **When** the user looks at the Sync screen, **Then** each pair shows the files and storage it uses against its limits, and the free space of the reader.
5. **Given** the reader's free space would fall below the stated reserve because of a plan, **When** the plan is displayed, **Then** the entries that would break the reserve are marked and cannot be approved.
6. **Given** the user lowers a limit below what the pair already holds, **When** they confirm, **Then** nothing is deleted; the pair simply cannot download more until it is below the limit again.
7. **Given** the limit counts files and size, **When** files downloaded earlier were removed by the user, **Then** they no longer count against the limit once removed.

---

### Edge Cases

- A pair's reader folder is on removable storage that is not present at sync time: the pair shows "folder not available", the sync does not start, and nothing is treated as deleted.
- The reader folder was renamed or removed outside the app: the pair shows the problem; no deletion proposals are made for files that cannot be listed.
- The server folder was renamed, moved or deleted: the pair shows "server folder not found" and makes no proposals until the user chooses the folder again or removes the pair.
- A listing of either side is incomplete or fails halfway (timeout, partial answer from the server): no deletion is proposed from an incomplete listing, and the plan says the comparison is incomplete.
- A file changes size or content while it is being read for upload or downloaded: it is not transferred half-and-half; it is reported as changed during the sync and left for the next plan.
- Names that differ only by case, Unicode normalization or trailing dots and spaces on one side: they are treated as collisions under the rules of features 001 and 002 and never overwrite each other.
- Hostile names (separators, `..`, control characters, overlong) from the server never lead to an operation outside the paired reader folder.
- Links or special files inside the reader folder are not followed and are listed as skipped.
- A very large folder (thousands of entries): the comparison is bounded by the limits, shown page by page, and does not hold the whole tree in memory.
- The account changed, or the app password is revoked: syncs stop with the reconnect message; pairs and records stay, and are never applied to a different account.
- Two pairs touching the same reader folder or server folder are refused (Story 1).
- A file is both in an upload-only pair and picked for upload by hand through feature 002: the same name collision rules apply and nothing is sent twice silently.
- The clock of the reader is wrong: no decision depends on clocks alone; a change is detected by the records of the last sync and the server's version marker.
- The device runs out of storage during a download: the item fails with a clear message, the rest of the sync is stopped safely, and no partial file is left behind.

## Requirements *(mandatory)*

### Functional Requirements

**Pairs**

- **FR-001**: Users MUST be able to add a sync pair consisting of a server folder, a folder on the reader's storage and a direction (download only, upload only, two-way), with download only as the default, and to list, change and remove pairs.
- **FR-002**: The server folder MUST be chosen with the folder chooser that works from stored listings (also offline), and the reader folder with the reader-side browser of feature 002 or by creating a folder; the system MUST show the effect of the chosen direction in plain words before saving.
- **FR-003**: Saving, changing the direction of, or removing a pair MUST NOT change, move or delete any file on either side, and MUST NOT start a transfer.
- **FR-004**: The system MUST refuse a pair whose reader folder is the storage root, the app's own data folder, or the download folder of feature 001, or that overlaps another pair's reader folder, or whose server folder overlaps another pair's server folder, and MUST say why.
- **FR-005**: Creating a pair with a direction that sends files from the reader to the server MUST require an explicit confirmation naming both folders, and changing a pair to such a direction MUST require the same.
- **FR-006**: A pair's synchronization covers the chosen folders and their subfolders, in the same structure on both sides; folders other than the paired ones MUST NOT be read, listed for transfer, uploaded or modified.

**Review and result**

- **FR-007**: Before any file is changed on either side, the system MUST compute a plan from the stored sync records and current listings of both sides, and MUST show it to the user; the plan MUST list each entry with its action (download, upload, skip with reason, keep both, deletion proposal), its size, and counts and total sizes per action.
- **FR-008**: The user MUST be able to approve a plan, remove entries from it before approving, or cancel; cancelling MUST leave both sides and the sync records unchanged, and entries removed by the user MUST reappear in the next plan.
- **FR-009**: Only entries of an approved plan MUST be carried out; approving a plan MUST NOT confirm any deletion proposal.
- **FR-010**: After a sync the system MUST show a result with counts of downloaded, uploaded, skipped, kept both, open proposals and failed entries, a reason for each failed entry, and the duration, and MUST keep the last result of each pair available.
- **FR-011**: The plan MUST be paged, MUST show the reader's free space and, when known, the server quota, and MUST mark entries that cannot be carried out (too large for the free space, name not allowed, unreadable, locked) with the reason.
- **FR-012**: The comparison MUST fetch folder listings only for the paired server folder and its subfolders, one level at a time, MUST bound the size of what it holds in memory, and MUST NOT compare when the reader is offline.

**Directions**

- **FR-013**: In a download-only pair the system MUST NOT send anything from the reader to the server and MUST NOT create or change anything on the server; files on the reader that the server does not have MUST be left alone.
- **FR-014**: In an upload-only pair the system MUST NOT copy anything from the server to the reader; files on the server that the reader does not have MUST be left alone.
- **FR-015**: In a two-way pair the system MUST transfer new and changed files in both directions and MUST treat a file changed on both sides as a conflict (FR-021).
- **FR-016**: Downloads MUST follow the rules of feature 001 (temporary file in the same folder, atomic rename after size and checksum match, no partial file visible, resume or clean restart, free-space check) and MUST never overwrite a reader file that the user changed.
- **FR-017**: Uploads MUST be performed through the queue of feature 002, so that persistence, ordering, chunking, conditional writes, name-collision handling, the "outcome unknown" flag and the refusal messages of that feature apply unchanged.
- **FR-018**: A file that was uploaded by a sync MUST remain unchanged in the reader folder; the system MUST NOT move, rename or delete source files on the reader as part of a sync.
- **FR-019**: Names from the server MUST be made safe for the reader's storage as in feature 001, and a name for which no safe form can be created MUST be skipped with the reason, never changed silently into another existing name.
- **FR-020**: A file that is identical on both sides before the first sync (same name, same size, and the same checksum where the server offers one) MUST be recorded as in sync without a transfer; if identity cannot be shown, the entry MUST be treated as a conflict (FR-021).

**Conflicts**

- **FR-021**: When the same file changed on both sides since the last sync, or the same name was created on both sides with different content, the system MUST keep both versions: neither side's original file is replaced, and the version the other side lacks is added there under a new, non-existing, clearly marked name; the user MUST be told which files exist.
- **FR-022**: A resolved conflict MUST NOT be detected again, and repeating the sync MUST NOT create further copies.
- **FR-023**: The system MUST NOT use clocks, file sizes or names alone to decide that a file is unchanged; the stored server version marker and the sync records decide, and a replacement on the server MUST be conditional on the version marker the reader last saw, as in feature 002.
- **FR-024**: A file deleted on one side and changed on the other MUST be kept (never deleted) and shown to the user.

**Deletions**

- **FR-025**: The system MUST NOT delete any file or folder on either side as part of a sync unless the user confirms that specific deletion; a file missing on one side MUST be shown as a deletion proposal with the side that still has it.
- **FR-026**: For each proposal the user MUST be offered: transfer the file again to the side that lacks it, leave both sides as they are (and stop proposing it until it changes again), or confirm the deletion on the other side; each confirmation MUST name the file and the side.
- **FR-027**: A confirmed deletion on the server MUST be queued as a deletion of feature 002 (to the server's trash, conditional on the version marker, refused when the server has no trash, never permanent); a confirmed deletion on the reader MUST remove only the file the pair synced, only when it is unchanged since the last sync, and MUST be stated to be unrecoverable through this app.
- **FR-028**: A proposal for a folder MUST name the folder and the number of files it holds, and a confirmation MUST cover only the files the pair had synced, never files added since.
- **FR-029**: No deletion MUST be proposed from an incomplete or failed listing, from a folder that is not available, or from a folder that was renamed or moved outside the pair's control.

**Triggers**

- **FR-030**: The system MUST start a sync of a pair only (a) when the app starts and Wi-Fi is already on, for pairs that have start-up sync switched on, (b) when the user taps Sync for one pair or all pairs, and (c) when Auto Sync is on for the pair, a short stated time after the user's own change in the app that touches the pair's folders.
- **FR-031**: The system MUST NOT sync on a timer, when Wi-Fi becomes available, when files appear in a paired reader folder, or in the background, MUST NOT watch folders, and MUST NOT turn Wi-Fi on; files that appear in a paired reader folder from outside the app are found at the next start-up sync or Sync.
- **FR-032**: Auto Sync and start-up sync MUST each be switchable per pair, and the Auto Sync wording and the place of its global switch in Settings MUST follow features 002, PB Notes and PB Office, and the per-pair switches live in the pair's settings; the default of each MUST be stated in the plan.
- **FR-033**: For a sync that the user did not start by tapping Sync (start-up or Auto Sync), the system MUST only compare the two sides and prepare the plan, MUST mark the pair "plan ready", and MUST NOT change any file on either side or queue any item until the user has opened the plan and approved it; there is no setting that lets an unreviewed plan run.
- **FR-034**: Only one sync per pair MAY run at a time; a second trigger MUST NOT start a parallel run and MUST tell the user one is running.
- **FR-035**: A running sync MUST be cancellable; completed items stay completed and the rest stay planned.
- **FR-036**: If a sync cannot start or is interrupted, the system MUST NOT retry by itself and MUST tell the user the pair is waiting.

**Resumable and idempotent**

- **FR-037**: The system MUST keep a persistent sync record per file of a pair (what each side looked like when the file was last in sync), and MUST advance a file's record only after the file is complete on both sides.
- **FR-038**: Repeating an interrupted sync MUST produce the same final state as an uninterrupted one; a file already transferred completely MUST NOT be transferred again, and running the same sync twice without changes in between MUST make no transfer and create no file.
- **FR-039**: An upload whose outcome is unknown MUST be kept and flagged under feature 002, and the sync MUST NOT repeat it; the file MUST NOT count as synced until the user's decision.
- **FR-040**: A file that changes while it is being transferred MUST NOT be transferred as a mixture of old and new content; it MUST be left for the next plan and reported.
- **FR-041**: After power loss, kill or crash the sync records and the files MUST be consistent: nothing lost, duplicated or half-written.

**Bounds**

- **FR-042**: Every pair MUST have a configurable limit on the number of files and on the total size it may hold on the reader, with stated defaults, and the system MUST check them when the plan is made.
- **FR-043**: A plan that would exceed a limit MUST NOT be carried out without the user's decision, and MUST state which limit is exceeded and by how much; the plan MUST NOT be approvable until the user has raised the limit or removed entries so that it fits, and the system MUST NOT pick entries on its own to make a plan fit.
- **FR-044**: A start-up or Auto Sync whose plan would exceed a limit MUST be marked as over the limit, transfer nothing and show that the pair needs a decision.
- **FR-045**: The system MUST keep a reserve of free space on the reader that no sync can use, MUST check free space before every download, and MUST report full or read-only storage and never ignore it.
- **FR-046**: Lowering a limit below what the pair holds MUST NOT delete anything; it MUST only stop further downloads for that pair until it is below the limit again.
- **FR-047**: The Sync screen MUST show for each pair the files and storage it uses against its limits, the free space of the reader, and the time and outcome of its last sync.

**Cross-cutting**

- **FR-048**: The system MUST treat everything received from the server (names, sizes, versions, headers, error texts) as untrusted, bound its size, never use a server name unsanitized as a local path, and never operate outside the paired folders.
- **FR-049**: Logs and messages MUST NOT contain secrets, server addresses, user names, file or folder names or paths.
- **FR-050**: Network activity for synchronization MUST be limited to the triggers of FR-030; a sync MUST NOT start for a different account than the one the pair was created for.
- **FR-051**: Everything that depends on the server MUST show offline state and the age of what is shown; entries with unconfirmed changes MUST be shown as "not synced".
- **FR-052**: All additions to the interface MUST be in English with a German translation and look and behave like PB Notes and PB Office, using the same components, without animations, with large touch targets, page-based lists and icon-and-label tools in a bottom panel.
- **FR-053**: This feature MUST NOT add automatic upload of photos or of files outside the paired folders, MUST NOT watch folders live, and MUST NOT synchronize more than the paired folders; downloaded files remain visible in the listings of feature 001 and uploaded ones follow the queue of feature 002.

### Key Entities

- **Sync pair**: a server folder, a reader folder, a direction (download only, upload only, two-way), the account it belongs to, its limits (file count and total size), its start-up and Auto Sync switches, and the time and outcome of its last sync.
- **Sync record**: for each file of a pair, what each side looked like when the file was last in sync (the server's version marker, and the reader file's identity), so that changes, conflicts and missing files can be told apart without relying on clocks.
- **Sync plan**: the result of comparing the two sides before anything is changed: a list of plan entries with counts and sizes, the limits and the free space it was checked against, and a flag when the comparison was incomplete.
- **Plan entry**: one file or folder with its action (download, upload, skip with reason, keep both, deletion proposal), its size and the reason where there is one.
- **Deletion proposal**: a file or folder that was synced earlier and is now missing on one side, with the side that still has it and the choices of Story 7.
- **Conflict**: a file changed on both sides, or created on both sides with different content, with both versions retained and the name of the added copy.
- **Sync result**: counts of downloaded, uploaded, skipped, kept both, open proposals and failed entries, the reasons for failures, and the duration, kept as the last result of the pair.
- **Pair limits**: the maximum number of files and total size a pair may hold on the reader, and the reserve of free space kept for the reader.
- **Queued item** (from feature 002): used for every upload and for every confirmed server deletion that a sync creates.
- **Local copy** (from feature 001): the file on the reader that a download creates; a paired folder's downloads count as local copies.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A user can add a download-only pair in at most 6 steps from opening the Sync screen, and saving it causes no network request and no file change in 100% of tested runs.
- **SC-002**: In 100% of tested syncs no file on either side is changed before the plan is approved, and cancelling a plan leaves both sides and the sync records identical to before.
- **SC-003**: After a first download-only sync of a simulated server folder with 100 files and 5 levels of subfolders, 100% of the files on the reader are identical to the server's in the same structure, and an immediate second sync makes 0 transfers.
- **SC-004**: After the app is killed at any point of a sync, repeating the sync yields a final state of both sides and of the sync records identical to an uninterrupted run, with 0 files lost, 0 duplicated and 0 partial files visible, in 100% of tested interruption points.
- **SC-005**: In 100% of tested cases where a file changed on both sides, both versions exist afterwards, and 0 further copies appear when the sync is repeated.
- **SC-006**: In 100% of tested cases a file missing on one side is only proposed, never deleted, by approving the plan; and each confirmed deletion affects exactly the one named entry, with server deletions going to the trash.
- **SC-007**: With start-up sync on, exactly one comparison per pair happens at start when Wi-Fi is on, and no request is made at start with Wi-Fi off, on a timer, on Wi-Fi changes or in the background, in 100% of tested sessions over 1 hour of idle use; and 0 files change on either side from a start-up or Auto Sync before the user approves the plan.
- **SC-008**: With Auto Sync on, a comparison of the affected pair starts within the stated delay (working default 10 seconds) of the user's last change and syncs of other pairs do not start, in 100% of tested cases.
- **SC-009**: A pair never holds more files or more total size than its limits after any sync, and the reader's free-space reserve stays intact, in 100% of tested cases including a plan that exceeds the limits.
- **SC-010**: A plan for a pair of 1,000 files is computed and shown page by page within 10 seconds on a simulated fast connection, each page turn takes at most 1 second, and memory use during the comparison does not grow with the number of files beyond the stated budget of the plan.
- **SC-011**: A 500 MB file synced in either direction does not make the app's memory use grow with the file size, and an interrupted transfer continues or restarts cleanly without a partial file visible, in 100% of tested cases.
- **SC-012**: Hostile names from the server (separators, `..`, control characters, overlong, case-only differences, normalization variants) never lead to a read, write or deletion outside the paired reader folder in 100% of hostile-input tests.
- **SC-013**: Removing a pair, changing its direction or lowering its limits deletes 0 files on either side in 100% of tested cases.
- **SC-014**: Every new screen string is available in English and German, and every new screen that depends on the server shows the offline state and the age of what it shows.

## Assumptions

- "Sync records" (FR-020 to FR-024) include, for the reader file, its size and modification time as they were when the sides last agreed; these are compared together with the stored server version marker, and never alone, so a wrong reader clock cannot silently decide an outcome (a differing time without a differing size is only trusted as "unchanged" when a stored checksum confirms it).
- Features 001 and 002 are merged and provide the connection, stored listings, folder chooser, downloads, local copies, the upload queue, Auto Sync wording and setting, conflicts, the "outcome unknown" handling and the server deletion with trash; this feature builds on them and does not change their guarantees.
- The Auto Sync delay of feature 002 (working default 10 seconds) is reused; the exact value and the defaults for the start-up sync and Auto Sync per pair are planning decisions. A safe working default is: start-up sync off and Auto Sync off for download-only pairs until the owner decides otherwise.
- "The user's own change" means a change made in the app that touches a paired folder; because folders are never watched, files placed in a paired reader folder from outside the app are found at the next start-up sync or Sync tap.
- Default limits per pair, the free-space reserve, the largest supported number of pairs, the size of the sync records and the memory budget of the comparison are planning decisions with measurable budgets in the plan; limits are checked per pair, not across pairs.
- Several pairs are allowed, each with its own folders, as long as their folders do not overlap; "more than the paired folders" means nothing outside those folders is ever synchronized.
- A file is the same on both sides before the first sync when its name, size and, where the server offers one, checksum are equal; when no checksum is available, equal size alone is not enough and the entry is a conflict that keeps both versions.
- Files of 4 GiB or more cannot exist on the reader's FAT storage and are skipped, as in features 001 and 002.
- Subfolders are part of a pair; empty folders are created on the receiving side together with the first file in them, and folders themselves are never proposed for deletion unless they are missing on one side as a whole.
- The reader is used with Wi-Fi switched on by the user; the app never turns Wi-Fi on.
- Behavior that depends on a real Nextcloud server (checksums, version markers, trash, quota, locks, proxies) and on the real device is **unverified** until exercised there; the success criteria are budgets, not measurements.
- Photo auto-upload, live file watching, synchronization of anything beyond the paired folders, several accounts, end-to-end encrypted folders, sharing, trash and version screens, and any background activity are out of scope.
