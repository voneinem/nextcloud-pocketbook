# Feature Specification: Upload and Manage Files on the Server

**Feature Branch**: `feature/002-manage-server-files`

**Created**: 2026-10-20

**Status**: Draft

**Input**: User description: "Upload and manage files on the Nextcloud server from the PocketBook. Builds on feature 001 (spec in specs/001-*). The user can upload files from the reader's storage using a file browser like the attach browser of PB Notes (list and tile layout, remembered), into the folder currently shown or a chosen one; create folders; rename; move and copy within the server; and delete files and folders on the server. Everything is queued on the reader first and sent only on explicit request or shortly after the user's own action (Auto Sync, on by default and switchable), with each queued item marked \"not synced\". Uploads are resumable, large files go in chunks, and an upload whose outcome is unknown is kept and flagged, never repeated silently. Never overwrite silently: uploading a name that exists asks (keep both with a new name, replace after confirmation, or skip), replacing and deleting use the stored ETag, and a conflict (changed on the server meanwhile) keeps both versions until the user decides. Deleting asks for confirmation and says it goes to the server's trash; permanent deletion is not part of this feature. Handle full quota, forbidden names, files the server rejects (size or type), and a locked file with clear messages. Out of scope: sharing, trash and versions screens, folder synchronization, automatic upload of any kind, background activity."

## Clarifications

### Session 2026-10-20

- Q: Should the user be able to pick a whole folder (with its subfolders) from the reader's storage to upload, or only individual files? → A: Files and whole folders; a picked folder is recreated on the server with its subfolders and each file in it is queued.
- Q: When the server has no trash, should Delete be refused, or allowed after a stronger warning that it cannot be undone? → A: Refuse; Delete is not offered for that server, with a plain explanation that the server has no trash.
- Q: When an uploaded file is the app's own downloaded copy of the entry it replaces, should the app link the two? → A: Yes, but only when the upload's source is the entry's own local copy and the replace succeeded; the local copy then counts as downloaded at the new server version.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Upload files from the reader (Priority: P1)

The user wants to put a file that is on the reader (a scanned page, an exported note, a book they converted) into their Nextcloud. While looking at a server folder they choose Upload. A file browser of the reader's own storage opens, the same kind of browser PB Notes uses to attach files: it shows folders and files either as a list or as tiles, and it remembers the chosen layout. The user picks one or several files, or whole folders. The files go into the folder currently shown on the server, or into another server folder the user chooses first. A picked folder is recreated on the server together with its subfolders and the files in them. This is a one-time, user-started transfer: the app does not remember the folder or watch it afterwards. Nothing is sent at that moment: each picked file and each folder to be created is stored in a queue on the reader and appears in the folder listing marked "not synced".

**Why this priority**: Getting a file from the reader to the cloud is the main reason to extend a read-only client, and everything else in this feature builds on the queue it introduces.

**Independent Test**: Against a simulated server, open a folder, pick two files and one folder that has a subfolder in the reader's file browser, see all of it listed as "not synced" with no network request made, then send the queue and see the files and the folder tree on the server with identical content.

**Acceptance Scenarios**:

1. **Given** a server folder is shown, **When** the user taps Upload, **Then** a file browser of the reader's storage opens with the layout (list or tiles) the user chose last time, and a way to switch layouts that is remembered for the next time.
2. **Given** the file browser, **When** the user picks one or several files and confirms, **Then** each file is added to the queue for the folder that was shown, the listing shows each as "not synced", and no network request is made because of the pick alone.
3. **Given** the user wants a different target, **When** they choose a server folder before picking files, **Then** the picked files are queued for that folder, and the queue shows the target of every item.
4. **Given** the reader has no connection, **When** the user picks files, **Then** they are queued exactly as when online, the target can be chosen among folders the reader already knows, and the queue stays until the user sends it.
5. **Given** a picked file that is 4 GiB or larger, empty, unreadable, or in a state the reader cannot read, **When** the user confirms the pick, **Then** that file is refused with a reason before it is queued and the other picks are unaffected.
6. **Given** the user picks a folder of the reader's storage, **When** they confirm, **Then** the app shows how many folders and files it contains and how large they are together, and after confirmation queues one new folder per folder and one upload per file, in an order in which every folder is created before what it contains.
7. **Given** a picked folder whose name already exists in the target on the server, **When** it is queued, **Then** the existing server folder is used (no new folder is created and nothing in it is replaced), and each file inside that already exists there goes through the name question of Story 3, with a choice to apply one answer to all remaining files of the pick.
8. **Given** a picked folder that contains unreadable files, links to other places, or more items than the stated queue limit, **When** the user confirms, **Then** unreadable files are skipped and listed in a report, links are not followed, a pick over the limit is refused as a whole with the reason, and nothing is queued partially without the user being told.
9. **Given** a queued upload, **When** the user removes it from the queue before it is sent, **Then** it disappears from the queue and the listing and the original file on the reader is untouched.
10. **Given** the app is closed, killed, or the reader loses power after files were queued, **When** the app starts again, **Then** the queue is complete and still shows every item as "not synced".

---

### User Story 2 - Send the queue, with Auto Sync (Priority: P1)

The user decides when the queue goes to the server. They can send it on request, or leave Auto Sync on, which is the default: a short time after the user's own action (queuing an upload, creating a folder, renaming, moving, copying, deleting) the queue is sent without another tap. Auto Sync can be switched off in Settings; then nothing is sent until the user asks. The queue screen shows every item with its state: not synced, sending with coarse progress, sent, failed with a reason, in conflict, or outcome unknown. Large files are sent in pieces, so a Wi-Fi drop or an app exit costs only the piece in flight; sending the queue again continues instead of starting over. If the app cannot tell whether an upload reached the server (for example the connection broke after the last byte was sent), the item is kept, flagged "outcome unknown", and never sent again by itself.

**Why this priority**: Without a reliable, honest way to send, queued work is stuck on the reader, and a wrong automatic repeat can duplicate or damage the user's data.

**Independent Test**: Queue three items, send, cut the connection halfway through a large upload, restore it, send again and verify that the file on the server is complete and identical, that nothing was sent twice, and that a simulated lost final answer leaves one flagged item that the app does not resend on its own.

**Acceptance Scenarios**:

1. **Given** queued items and Auto Sync off, **When** time passes, the app starts, or Wi-Fi comes on, **Then** no request is made; **When** the user taps Send, **Then** the queue is sent in order.
2. **Given** Auto Sync on, **When** the user performs a queuing action, **Then** the queue is sent after a short stated delay, and further actions within that delay are sent together with it.
3. **Given** Auto Sync on and the reader offline when the delay ends, **When** the send cannot start, **Then** the items stay queued and marked "not synced", no retry runs by itself, and the user is told the queue is waiting.
4. **Given** the app starts with leftover queued items, **When** Auto Sync is on, **Then** nothing is sent at start (only the single account check of feature 001); items wait for the next user action or an explicit Send.
5. **Given** a large file being sent, **When** the connection drops and later returns and the user sends again, **Then** sending continues with the unsent pieces, and the final file on the server equals the file on the reader.
6. **Given** a large file for which pieces were already sent, **When** the file on the reader was changed in the meantime, **Then** the app does not mix old and new content: it discards the sent pieces on the server, starts the file over from the changed content, and says so.
7. **Given** an upload whose outcome is unknown, **When** the user opens the queue, **Then** the item is shown as "outcome unknown" with a plain explanation, is skipped by every Send and Auto Sync, and offers three choices: check the server for it, send it again as a new item, or remove it from the queue.
8. **Given** the user chooses "check the server", **When** a file with the item's name exists with the item's size, **Then** the app reports that the upload most likely arrived and leaves the decision to the user; when no such file exists, it says so and offers to send again.
9. **Given** a send in progress, **When** the user taps Cancel, **Then** sending stops after the piece in flight, completed items stay completed, and the rest stay queued and "not synced".
10. **Given** a send in progress, **When** the screen turns off or Wi-Fi drops, **Then** nothing is lost, completed items stay completed, and the interrupted item is resumable.
11. **Given** a source file that was removed or became unreadable between queuing and sending, **When** that item is reached, **Then** it is marked failed with that reason, other items continue, and nothing else is affected.
12. **Given** any state, **When** the user looks at a folder listing, **Then** every entry that has a queued or unconfirmed change shows "not synced", and entries whose change the server confirmed no longer do.

---

### User Story 3 - Never overwrite silently (Priority: P1)

Nothing on the server or on the reader may be replaced without the user deciding. When an upload, a new folder, a rename, a move or a copy would use a name that already exists in the target folder, the app asks: keep both (the new item gets a new name that does not exist), replace (only after a confirmation that names the file that will be replaced), or skip. Replacing a file is only allowed if the server's current version is the one the reader last saw; otherwise the file was changed on the server meanwhile, and that is a conflict: both versions are kept (the one on the server untouched, the one from the reader held in the queue) until the user decides.

**Why this priority**: A cloud file that is overwritten cannot be recovered through this app; protecting it is the first rule of managing files.

**Independent Test**: Against a simulated server, upload a name that exists and walk through keep both, replace and skip; then change the server file between listing and sending and verify the replace is held as a conflict with both versions intact.

**Acceptance Scenarios**:

1. **Given** the target folder already holds a file of the same name, **When** the user queues an upload of that name, **Then** the app asks keep both, replace or skip before the item is queued, and skipping queues nothing.
2. **Given** the user chose keep both, **When** the item is queued, **Then** it gets a new name that does not exist in the target folder (and not among other queued items for it), the new name is shown, and the user may change it.
3. **Given** the user chose replace, **When** they are asked to confirm, **Then** the confirmation names the file and says it will be replaced on the server, and the replace is queued only after they confirm.
4. **Given** a queued replace, **When** the item is sent and the server's version is still the one the reader last saw, **Then** the file is replaced and the reader records the new server version.
5. **Given** a queued replace, **When** the server's version differs from the one the reader last saw, **Then** nothing is overwritten, the item is flagged "changed on the server", and the user chooses: keep both (the reader's file is uploaded under a new name), replace anyway after a second confirmation that says the server version is newer, or skip.
6. **Given** a name collision that the reader cannot see because the folder was never refreshed, **When** the item is sent and the name exists on the server, **Then** the item is held as a collision and the same choices are offered; the new item never replaces an unseen file.
7. **Given** a replace whose source is the entry's own local copy from feature 001, **When** the server confirms it, **Then** that local copy counts as downloaded at the new server version and no longer shows "changed on the reader" or "outdated"; an upload from any other source never changes the state of a local copy.
8. **Given** a file the user changed on the reader after queuing a replace, **When** it is sent, **Then** the file as it is at the time of sending is what is replaced into the server, and the user can see it was changed since queuing.
9. **Given** a name collision for a rename, move, copy or new folder, **When** the user confirms the action, **Then** the same three choices apply (a new folder whose name exists is not created; the existing folder is shown instead).
10. **Given** any combination of choices and interruptions, **When** the queue is sent, **Then** no file on the server and no file on the reader is replaced, removed or truncated that the user did not explicitly decide to replace.

---

### User Story 4 - Create folders and rename (Priority: P2)

The user creates a new folder in the folder shown, and renames a file or folder. Both are queued like uploads: the new folder appears at once in the listing marked "not synced" (and can be used as a target for uploads before it exists on the server, which are then sent after it), and a renamed entry shows its new name marked "not synced". Names the server cannot accept are refused clearly.

**Why this priority**: Without folders and names the user cannot keep the cloud tidy, but uploading alone is already useful.

**Independent Test**: Create a folder, queue an upload into it, rename another file, send, and verify the server shows the folder with the file and the new name; then try a forbidden name and see the refusal.

**Acceptance Scenarios**:

1. **Given** a shown folder, **When** the user taps New folder and enters a name, **Then** the folder appears in the listing marked "not synced" and is queued.
2. **Given** a queued new folder, **When** the user queues an upload into it, **Then** the app sends the folder first and then the file, and if the folder cannot be created the upload waits as failed with that reason.
3. **Given** a file or folder, **When** the user renames it to a free name, **Then** the listing shows the new name marked "not synced", and the entry is queued for the rename.
4. **Given** a rename of an entry that has a local copy or an offline mark from feature 001, **When** the rename is confirmed by the server, **Then** the local copy and the offline mark remain attached to the entry under its new name.
5. **Given** a name that is empty, only dots or spaces, contains a path separator, control characters, characters the server forbids, or is longer than the server allows, **When** the user confirms it, **Then** it is refused immediately with the reason and nothing is queued.
6. **Given** a rename to a name that only differs by letter case, **When** confirmed, **Then** it is accepted as a different name on the server and not treated as a collision.
7. **Given** a queued rename of an entry that changed or disappeared on the server meanwhile, **When** it is sent, **Then** it is held as a conflict and not applied.
8. **Given** a rename queued offline, **When** the user later sends, **Then** the result is the same as if it had been done online.

---

### User Story 5 - Move and copy within the server (Priority: P2)

The user moves or copies one or several files and folders to another server folder, chosen with a folder chooser (using stored listings offline and fetched ones when connected). Both are queued, the entries show "not synced", and nothing leaves the server: the file content is not downloaded or uploaded again for a move or copy.

**Why this priority**: Reorganizing files is a natural part of managing them but less urgent than putting files on the server.

**Independent Test**: Move one file and copy a folder with content on a simulated server, send, and verify source and destination in each case without any file content moving through the reader.

**Acceptance Scenarios**:

1. **Given** one or several selected entries, **When** the user chooses Move or Copy and picks a destination folder, **Then** the actions are queued, and the listing shows a moved entry as moving and a copy as pending, marked "not synced".
2. **Given** a destination that already has an entry of the same name, **When** the user confirms, **Then** the three choices of Story 3 apply per entry.
3. **Given** a folder is chosen as source, **When** it is moved or copied, **Then** its whole content goes with it, and the user sees that before confirming.
4. **Given** a destination that is the source folder itself, inside the moved folder, or the same location, **When** the user tries to choose it, **Then** it is refused with a reason before queuing.
5. **Given** a queued move or copy whose source changed or vanished on the server, **When** it is sent, **Then** it is held as a conflict and not applied.
6. **Given** a queued move of a file with a local copy or offline mark from feature 001, **When** the server confirms the move, **Then** the local copy and the mark follow the entry to its new place.
7. **Given** a copy, **When** it is sent, **Then** the reader does not download or upload the file content for it.
8. **Given** a move or copy with a quota problem at the destination, **When** it is sent, **Then** the clear message of Story 7 is shown and source remains unchanged.

---

### User Story 6 - Delete on the server (Priority: P2)

The user deletes a file or folder from the server. Before anything is queued the app asks for explicit confirmation and says plainly that the item goes to the server's trash, not that it disappears forever, and for a folder that everything inside goes with it. The deletion is queued, the entry shows "delete pending / not synced" and stays visible until the server confirms. The user can cancel a queued delete before it is sent. Deleting uses the version the reader last saw; if the entry changed on the server meanwhile, the delete is held as a conflict. Permanent deletion and a screen to restore from the trash are not part of this feature.

**Why this priority**: Deleting is the riskiest action in the feature and must be safe, but it is not needed to get files onto the server.

**Independent Test**: Delete a file and a folder on a simulated server, confirm the dialog wording, send, and check they are in the server's trash and gone from the listing; then change a file on the server after queuing its delete and verify the delete is held.

**Acceptance Scenarios**:

1. **Given** a file or folder, **When** the user chooses Delete, **Then** a confirmation names the entry, says it goes to the server's trash, and for a folder says that its content is included; nothing is queued until the user confirms.
2. **Given** a confirmed delete, **When** it is queued, **Then** the entry stays in the listing marked as delete pending and "not synced" until the server confirms, and the user can cancel the queued delete.
3. **Given** a queued delete, **When** it is sent and the entry is unchanged on the server, **Then** it is deleted on the server (into its trash) and removed from the listing.
4. **Given** a queued delete, **When** it is sent and the entry changed on the server since the reader last saw it, **Then** nothing is deleted, the item is flagged "changed on the server", and the user chooses to keep the entry (cancel the delete), or delete anyway after a second confirmation that says the entry is newer.
5. **Given** a deleted entry that has a local copy, **When** the server confirms the deletion, **Then** the local copy is not removed; the entry is shown as "removed on the server" as in feature 001, and a local copy with unconfirmed changes is never touched.
6. **Given** a server that has no trash, **When** the user chooses Delete, **Then** the app does not offer Delete, explains that the server has no trash, and never claims that an item goes to the trash if it does not (FR-033).
7. **Given** a folder that holds entries with queued changes (uploads into it, renames, moves), **When** the user confirms deleting it, **Then** the app warns about these queued items and, once confirmed, those items are flagged as failed with that reason rather than lost without a trace.

---

### User Story 7 - Clear messages when the server says no (Priority: P2)

The server can refuse an action for reasons the user can understand and often fix. The app shows a distinct, plain message with a next step for each of: the user's storage space is full (or the file does not fit into what is left), a name the server forbids, a file the server rejects because of its size or type, and a file or folder that is locked by someone or something else. The failed item stays in the queue with its reason; nothing is lost and nothing is retried silently.

**Why this priority**: Without understandable reasons the user cannot recover from a failure on an e-ink device with no support tools.

**Independent Test**: Against a simulated server configured to refuse in turn with each of the four reasons (plus an unexpected answer), send items and verify that each failure shows its own message and next step and that items stay queued.

**Acceptance Scenarios**:

1. **Given** a server that has no space left for the user, **When** an upload is sent, **Then** the item fails with a message that the storage is full and how much space is free if known; other items that fit still go through.
2. **Given** a file larger than the free space, **When** the user queues or sends it, **Then** the app tells them before sending when it can know (from the quota), and in any case reports the server's refusal in plain words.
3. **Given** a name the server forbids (reserved names, forbidden characters or endings, or names that its rules do not allow), **When** the server refuses it, **Then** the message says the name is not allowed and offers to rename the item and queue it again.
4. **Given** a file the server rejects because it is too large for the server's limit or of a type the server does not accept, **When** it is refused, **Then** the message says which of the two applies as far as the server says, and the item stays in the queue for the user to remove.
5. **Given** a file or folder that is locked, **When** a change or delete on it is refused, **Then** the message says it is locked, the item stays queued and is not retried by itself, and the user can retry later.
6. **Given** an answer the app does not recognize, **When** an item fails, **Then** the message says that the server refused without a known reason and shows a short code, but never shows the server's text unsanitized, secrets, addresses or names in logs.
7. **Given** a revoked or expired app password, **When** sending, **Then** the app reports that the login is no longer valid and offers to reconnect; the queue is kept untouched.
8. **Given** any failure, **When** the user opens the queue, **Then** failed items show their reason and offer Retry (only on the user's tap), Edit (name or target where relevant) and Remove.

---

### Edge Cases

- The server's folder listing is stale: the user queues into a folder that was deleted on the server, or an item whose target folder is gone. The item fails with that reason, the target must be chosen again, and nothing is created elsewhere.
- The user queues several items that depend on one another (new folder, then an upload into it, then a rename of the folder): the dependent items are sent in a valid order, and if an earlier one fails the dependent ones wait and show why.
- The user queues the same file twice, or two files that end up with the same name in the same target folder: the second goes through the collision question.
- The file browser shows a very large folder on the reader, files with unusual or very long names, or storage that is removed while browsing: the browser stays usable and reports the problem.
- The same entry has several queued changes (rename, then delete): they apply in the order the user made them, and a conflict stops the later ones.
- A file is open in the reader's viewer or being downloaded while a change on its entry is sent.
- A response is lost for a create-folder, rename, move, copy or delete: the item shows as outcome unknown, like an upload, and is not repeated silently; checking the server resolves most cases without data risk.
- Queue and storage full on the reader: queuing refuses with a message; existing queued items are never dropped to make room.
- The reader's clock is wrong, so that "not synced" times or last-seen versions seem off: the app does not use the clock to decide what to overwrite.
- A very long queue (hundreds of items) is shown and sent page by page and does not slow the other screens.
- The server answers with a redirect to another host: credentials are not sent there and the item fails with that reason (as in feature 001).
- The account is changed or disconnected while items are queued: queued items are kept, are not sent to a different account, and the user is told what to do with them.
- Items that the user cancels in the middle of a chunked upload leave no incomplete file visible on the server.

## Requirements *(mandatory)*

### Functional Requirements

**Upload and file browser**

- **FR-001**: Users MUST be able to pick one or several files and folders from the reader's storage in a file browser that matches the one used for attachments in PB Notes, showing a list or tiles, with the chosen layout remembered across restarts.
- **FR-002**: Users MUST be able to choose the target folder on the server: by default the folder currently shown, or another folder they choose before picking; every queued upload and folder MUST show its target.
- **FR-003**: Users MUST be able to pick a whole folder of the reader's storage; the system MUST recreate it on the server with its subfolders, queue one new folder per folder and one upload per file in an order that creates every folder before its content, and show the number of folders, the number of files and the total size for confirmation before queuing. A picked folder is a one-time transfer: the system MUST NOT remember it, watch it, or upload anything from it later without a new pick by the user. Unreadable files MUST be skipped and reported, links MUST NOT be followed, and a pick that exceeds the stated queue limit MUST be refused as a whole with the reason. A picked folder whose name already exists in the target MUST use the existing server folder without replacing anything in it, and the name question of FR-021 applies to each file inside it, with a choice to apply one answer to all remaining files of that pick.
- **FR-004**: The system MUST refuse before queuing a picked file that is empty, unreadable, or 4 GiB or larger, and MUST say why, without affecting other picks.
- **FR-005**: The system MUST NOT modify, move or delete the original file on the reader as part of an upload.

**Queue and persistence**

- **FR-006**: Every upload, new folder, rename, move, copy and delete MUST be written to a persistent queue on the reader before any network activity for it, and MUST survive app exit, kill and power loss.
- **FR-007**: A queued item MUST NOT be discarded, evicted, shortened or overwritten until the server has confirmed it or the user has removed it; the queue MUST NOT be reduced to make room for anything else.
- **FR-008**: Every entry in a listing that has a queued or unconfirmed change MUST be shown marked "not synced"; a queued new folder, rename, move, copy or delete MUST be visible in the listing as pending until the server confirms it.
- **FR-009**: The queue screen MUST show every item with its kind, source and target, and one state of: not synced, sending (with coarse progress), sent, failed (with reason), in conflict, outcome unknown; it MUST be paged and offer Remove for items not yet sent and Retry for failed ones.
- **FR-010**: Items MUST be sent in the order the user created them, except that an item depending on another (for example an upload into a new folder) MUST wait for it, and a failed or conflicting item MUST hold back only the items that depend on it.

**Sending and Auto Sync**

- **FR-011**: The system MUST send the queue only when the user taps Send, or, when Auto Sync is on, a short stated time after the user's own queuing action; it MUST NOT send at app start, on a timer, when Wi-Fi becomes available, or in the background.
- **FR-012**: Auto Sync MUST be on by default, MUST be switchable in Settings with the same wording and place as in PB Notes and PB Office, and MUST react only to the user's own local change; when it is off, nothing is sent until the user taps Send.
- **FR-013**: If a send cannot start or is interrupted, the system MUST keep every unsent item "not synced", MUST NOT retry by itself, and MUST tell the user that the queue is waiting.
- **FR-014**: Large files MUST be sent in pieces so that an interruption costs at most the piece in flight; sending again MUST continue from the pieces already on the server when they are still valid and MUST produce the same result as an uninterrupted send.
- **FR-015**: The system MUST NOT combine pieces of an old and a changed source file; when the source changed after sending began, it MUST start the file over from its current content and tell the user.
- **FR-016**: A file MUST appear under its final name on the server only when it is complete; a cancelled, failed or abandoned chunked upload MUST NOT leave an incomplete file visible in the user's folder.
- **FR-017**: Transfers MUST stream from the reader's storage with a small fixed memory use, MUST survive screen-off and Wi-Fi loss without losing queued data, and MUST show progress with a coarse screen-refresh budget.
- **FR-018**: Users MUST be able to cancel a running send; completed items stay completed, the rest stay queued.

**Unknown outcomes**

- **FR-019**: When the outcome of an operation cannot be determined (for example the connection broke after the request was sent), the system MUST keep the item, mark it "outcome unknown", exclude it from every Send and Auto Sync, and MUST NOT repeat it silently.
- **FR-020**: For an item with unknown outcome the user MUST be offered: check the server for it, send it again as a new item (only after their choice), or remove it from the queue; the check MUST only report what it found and MUST NOT decide for the user.

**Collisions and conflicts**

- **FR-021**: When an upload, new folder, rename, move or copy would use a name that already exists in the target, the system MUST ask the user to keep both (with a new, non-existing name that the user may change), replace (only after a confirmation that names the entry to be replaced), or skip, and MUST NOT proceed without an answer.
- **FR-022**: Every replace and every delete MUST be conditional on the server version of the entry that the reader last saw; every creation MUST be conditional on the name not existing; a different server version or an existing name MUST be treated as a conflict and MUST NOT overwrite or delete anything.
- **FR-023**: A conflict MUST keep both versions until the user decides: the server version stays untouched, the reader's version stays in the queue, and the user chooses keep both (upload under a new name), replace or delete anyway (after a second confirmation that says the server version is newer), or skip. For a rename, move or copy held as a conflict, the server entry stays untouched and the user chooses to apply the item anyway (after a confirmation that says the entry changed) or to discard the item.
- **FR-024**: The system MUST NOT use clocks, file sizes or names alone to decide that an entry is unchanged; only the stored server version marker decides.
- **FR-025**: A move or copy MUST treat a destination name collision under FR-021 for each entry, and MUST refuse a destination that is the source itself or inside the moved folder.

**Folders, rename, move, copy**

- **FR-026**: Users MUST be able to create a folder in the folder shown; it MUST appear immediately, marked "not synced", and MAY be used as a target of queued uploads before it exists on the server.
- **FR-027**: Users MUST be able to rename a file or folder; the system MUST validate the new name before queuing (not empty, not only dots or spaces, no path separator or control characters, no characters or endings the server forbids, within the server's length limit) and MUST treat a name differing only by letter case as a different name.
- **FR-028**: Users MUST be able to move and copy one or several files and folders, including folders with their content, to another server folder chosen with a folder chooser that works from stored listings offline; moving and copying MUST NOT transfer file content through the reader.
- **FR-029**: A local copy and an offline mark from feature 001 MUST follow a renamed or moved entry once the server has confirmed it; a copy MUST NOT create a local copy or an offline mark. When a replace whose source is the entry's own local copy is confirmed by the server, the system MUST record that copy as downloaded at the new server version; an upload from any other source MUST NOT change the state of a local copy.

**Delete**

- **FR-030**: Deleting MUST require explicit confirmation that names the entry, states that it goes to the server's trash, and for a folder states that its content is included; nothing is queued before that confirmation.
- **FR-031**: A queued delete MUST keep the entry visible and marked pending until the server confirms it, and MUST be cancellable before it is sent.
- **FR-032**: The system MUST NOT delete permanently: it MUST NOT empty or bypass the server's trash, and MUST NOT offer an action that does.
- **FR-033**: When the server does not offer a trash, the system MUST NOT offer or perform Delete on it, MUST explain in plain words that the server has no trash and that deleting would be permanent, and MUST still allow the user to remove a delete or any other item from the queue before it is sent.
- **FR-034**: Confirming a deletion on the server MUST NOT remove any file on the reader; a local copy of a deleted entry is shown as "removed on the server" (as in feature 001), and a local copy with unconfirmed changes MUST NOT be touched.
- **FR-035**: Deleting a folder that has queued items inside MUST warn the user about them first, and those items MUST end up failed with a reason, not lost silently.

**Server refusals**

- **FR-036**: The system MUST show a distinct, plain message with a next step for each of: storage full or file does not fit, forbidden name, file refused for size or type, and locked file or folder; and a generic message with a short code for any other refusal.
- **FR-037**: A refused item MUST stay in the queue with its reason, MUST NOT be retried except on the user's tap, and MUST NOT stop independent items from being sent.
- **FR-038**: The system MUST check the user's quota before sending when it is known and warn if a file will not fit, and MUST still report the server's own refusal if the check was wrong.
- **FR-039**: A revoked or expired app password MUST be reported as such with an offer to reconnect, and MUST leave the queue unchanged; queued items MUST NOT be sent to a different account than the one they were queued for.

**Cross-cutting**

- **FR-040**: The system MUST treat everything received from the server (names, sizes, versions, headers, error texts) as untrusted, bound its size, and never use a server-supplied name unsanitized as a local path; error texts from the server MUST NOT be shown unsanitized or written to logs.
- **FR-041**: Logs and messages MUST NOT contain secrets, server addresses, user names, file or folder names or paths.
- **FR-042**: The only network activity the system performs MUST be: the start-up account check from feature 001, explicit user actions, and the Auto Sync send after the user's own action; there MUST be no polling, no scheduled sync, no automatic upload of any kind and no background activity.
- **FR-043**: All additions to the interface MUST be in English with a German translation and MUST look and behave like PB Notes and PB Office, using the same components, without animations, with large touch targets, page-based lists and icon-and-label tools in the bottom panel; Settings MUST show the state of Auto Sync and the queue MUST always show whether the reader is offline.
- **FR-044**: This feature lifts the read-only rule of feature 001 only for the actions listed here; sharing, trash and version screens, folder synchronization, automatic upload and background activity remain out of scope, and everything feature 001 guarantees for downloads, browsing and offline files MUST still hold.

### Key Entities

- **Queued item**: one pending change made by the user (a picked folder becomes a group of items, one per folder and per file, that are shown and sent together): its kind (upload, new folder, rename, move, copy, delete), the account, the source (a file on the reader or a server entry), the target, the server version it depends on, its creation order and any dependency on another item, and its state (not synced, sending, sent, failed, in conflict, outcome unknown) with a reason where relevant.
- **Upload progress**: for a large file, what part of it the server has already received and the source file's identity at that time (size and modification), so that a changed source is detected.
- **Collision / conflict**: a held item together with what it meets: an existing name, or a server version that differs from the one the reader last saw; it carries the user's choices (keep both, replace, skip).
- **Remote entry** (from feature 001): extended with the stored server version marker, a "not synced" mark when it has a queued change, and a pending kind (new, renamed, moving, copy pending, delete pending).
- **Local copy and offline mark** (from feature 001): follow an entry that is renamed or moved; unaffected by a server deletion.
- **Server refusal**: the reason the server gave, reduced to one of: storage full, forbidden name, size or type refused, locked, login no longer valid, other.
- **Auto Sync setting**: on or off, on by default, with the stated delay.
- **Browser layout setting**: list or tiles, remembered.

## Success Criteria *(mandatory)*

All time and size figures below are budgets and are unverified until measured on the InkPad One and, where a real server matters, against a real Nextcloud server (see Assumptions).

### Measurable Outcomes

- **SC-001**: A user can pick a file in the reader's file browser and see it listed as "not synced" in the target folder within 5 steps of opening the Upload action, and no network request is made by the pick, in 100% of tested runs.
- **SC-002**: After app kill or simulated power loss at any point of queuing or sending, 100% of queued items are still present on restart, and 0 items are sent twice by the app without an explicit user choice.
- **SC-003**: With Auto Sync on, queued items are sent within the stated delay (working default 10 seconds) of the user's last action; with Auto Sync off, or at app start, or when Wi-Fi comes on, no queue request is made in 100% of tested sessions.
- **SC-004**: A 500 MB file is uploaded without the app's memory use growing with the file size; peak memory stays within 150 MB during an upload, and an upload interrupted by a Wi-Fi drop completes after sending again with a file on the server identical to the reader's file in 100% of tested runs.
- **SC-005**: After an interruption of a large upload, sending again transfers no more than the unsent pieces plus one in-flight piece, in 100% of tested runs.
- **SC-006**: In 100% of tested cases, a lost answer after the last byte leaves exactly one flagged item "outcome unknown" and zero automatic repeats; the same holds for new folder, rename, move, copy and delete.
- **SC-007**: In 100% of tested name collisions (upload, new folder, rename, move, copy), no existing entry is replaced without the user's confirmation, and "keep both" always produces a name that did not exist.
- **SC-008**: In 100% of tested cases where the server's version changed after the reader last saw it, a queued replace or delete does not change the server entry and ends in a conflict that retains both versions.
- **SC-009**: Every delete is preceded by a confirmation that mentions the server's trash in 100% of tested cases, and no action in the app can delete permanently or empty the trash.
- **SC-010**: Each of the four named refusals (storage full, forbidden name, size or type refused, locked) produces its own message with a next step in 100% of simulated cases, and an unknown refusal produces the generic message with a short code.
- **SC-011**: A queue of 200 items is shown page by page, opens within 2 seconds, and each page turn completes within 1 second.
- **SC-012**: A move or copy of a file of any size causes no transfer of file content through the reader in 100% of tested cases.
- **SC-013**: After a rename or move confirmed by the server, 100% of local copies and offline marks of the affected entries remain attached; after a confirmed server deletion, 0 local files are removed.
- **SC-014**: A cancelled or abandoned chunked upload leaves no incomplete file visible in the user's server folder in 100% of tested cases.
- **SC-015**: Every new screen string is available in English and German, and every new screen that depends on the server shows the offline state and which items are "not synced".
- **SC-016**: Hostile names (path separators, `..`, control characters, overlong, case-only differences, Unicode normalization variants) never lead to an operation outside the intended folder or to the replacement of another entry, in 100% of hostile-input tests.

## Assumptions

- Feature 001 is merged and provides the connection, browsing, stored listings, local copies, offline marks and the entry states this feature builds on; this feature extends its read-only rule only as stated in FR-044.
- The file browser of PB Notes (list and tile layout, remembered) is the visual and behavioral reference for the reader-side browser; its look is reproduced with the shared component set and nothing else is copied.
- The Auto Sync setting is new to this app; its wording and place in Settings follow PB Notes and PB Office. It is on by default, as the owner decided. The working delay between the user's action and the send is 10 seconds; the exact value is a planning decision.
- The server's version marker (its ETag) is what "the version the reader last saw" means; entries never seen by the reader have no marker, and for them only creation conflicts (name exists) can be detected.
- Nextcloud's trash is assumed to be available on a normal server; whether it is available is learned from the server's reported capabilities, and when it is not, Delete is refused (FR-033).
- The InkPad One's storage is FAT, so files of 4 GiB or more do not exist on the reader and are refused; this is carried over from feature 001.
- Piece size, retry limits, the queue's size bound (which also limits the size of a folder pick) are planning decisions with measurable budgets in the plan.
- The reader is used with Wi-Fi switched on by the user; the app never turns Wi-Fi on.
- Behavior that depends on a real Nextcloud server (chunked uploads, trash, locks, quota, size and type limits, version markers, proxies) and on the real device is **unverified** until exercised there; the success criteria are budgets, not measurements.
- Sharing, trash and version screens, folder synchronization, automatic upload of any kind, several accounts, end-to-end encrypted folders, and background activity are out of scope; folders of the reader are not synchronized with the server (a picked folder is a one-time transfer, not a link), and no feature here runs without the user's own action.
