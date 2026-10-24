# Feature Specification: Trash and File Versions

**Feature Branch**: `feature/006-trash-and-versions`

**Created**: 2026-10-24

**Status**: Draft

**Input**: User description: "Deleted files and file versions. Builds on features 001 and 002. The user can open the server's trash, see the deleted files with their original location and deletion date, restore a file or folder (to its original place, or to a chosen place if that is taken without overwriting anything) and delete items for good or empty the trash only after an explicit second confirmation that names what will be lost. For a file the user can list its older versions (date and size) and restore one, which keeps the current version as a new version rather than discarding it. The app explains when the server has the trash or versions app disabled. Out of scope: retention settings, comments, activity feed."

## Clarifications

### Session 2026-10-24

- Q: Should restoring from the trash, restoring an older version and deleting for good run only right away on a connected reader, or may they be queued while offline? (FR-035) → A: Right away on a connected reader only; nothing is queued, and all three actions are unavailable offline with a stated reason.
- Q: In the trash list, should the user act on one item at a time, or select several items to restore or delete for good in one go? (FR-006) → A: One item at a time for Restore and Delete for good; Empty trash is the only bulk action.
- Q: For the irreversible actions, is one confirmation dialog after the tap enough, or must some ask a second, separate confirmation? (FR-022) → A: One dialog for a single file; Empty trash and a folder deleted for good ask two successive dialogs, each naming what will be lost, with Cancel as the default in both.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - See what was deleted (Priority: P1)

Feature 002 lets the user delete files and folders on the server, and says that they go to the server's trash. Today the user cannot look into that trash from the reader, so a deletion made by mistake (here or on another device) cannot be undone. The user opens the trash from the app and sees one entry per deleted file or folder, newest deletion first: its name, the folder it was deleted from (its original location), the date it was deleted, and for a file its size. The list is paged like every other list in the app, can be refreshed on request, and shows the age of the last refresh. Opening the trash is the only network activity of this story, and only when the user asks for it.

**Why this priority**: Without a way to see the trash nothing else in this feature is possible, and seeing it already answers "where did my file go?".

**Independent Test**: Against the in-process fake server with a trash holding files and folders deleted at known times from known folders, open the trash; verify one entry per item with name, original location, deletion date and (for files) size, newest first, paged; verify that with the connection off the last stored list is shown marked with its age.

**Acceptance Scenarios**:

1. **Given** the user is connected and the server has a trash with items, **When** the user opens the trash, **Then** every item is listed with its name, original location and deletion date, and a file also with its size, with the newest deletion first.
2. **Given** the trash holds more items than fit on a page, **When** the user looks at the list, **Then** it is paged with page buttons and has no kinetic scrolling.
3. **Given** the trash is empty, **When** the user opens it, **Then** the app says in plain words that the trash is empty.
4. **Given** a trash list was shown before and the reader is now offline, **When** the user opens the trash, **Then** the stored list is shown, marked with its age and with the actions that need the server unavailable and saying why.
5. **Given** the user deleted a file with feature 002 and the server confirmed it, **When** the user refreshes the trash, **Then** that file is listed with the folder it was deleted from and the time of the deletion.
6. **Given** the server sends an item with a malformed name, a missing date or an impossible size, **When** the list is shown, **Then** that item is shown as unreadable (still listed, not restorable by name) and the other items are unaffected.

---

### User Story 2 - Restore a deleted file or folder (Priority: P1)

The user selects a deleted file or folder in the trash and taps Restore. The app puts it back where it was deleted from. A folder comes back with its content. If something with the same name is now at the original place, or the original folder no longer exists, nothing is overwritten and nothing is lost: the app says why the original place cannot be used and asks the user to choose another folder with the folder chooser of feature 002, and, if the name is taken there too, to give a new name. The user can also choose another place deliberately instead of the original one. The restored entry then appears in the folder listing like any other file, and the item leaves the trash.

**Why this priority**: Restoring is the reason to look into the trash; it turns the safety net promised by feature 002 into something the user can use.

**Independent Test**: Against the fake server, restore a trashed file to its free original place; restore one whose original name is taken (verify the app offers another place and the existing file is untouched); restore into a chosen folder where the name is also taken (verify a new name is required and nothing is overwritten); restore a folder and verify its content comes back; verify the restore is refused, not forced, when the server answers that the target changed.

**Acceptance Scenarios**:

1. **Given** a trashed file whose original place is free, **When** the user taps Restore and confirms the proposed place, **Then** the file is back in its original folder under its original name and is gone from the trash.
2. **Given** a trashed folder with content, **When** the user restores it, **Then** the folder and all its content come back and the folder leaves the trash as one item.
3. **Given** the original place holds a file or folder of the same name, **When** the user taps Restore, **Then** the app says that the place is taken, offers to choose another folder, and the existing entry is not overwritten, renamed or changed in any case.
4. **Given** the original folder no longer exists, **When** the user taps Restore, **Then** the app says so and offers to choose another folder; it does not create the missing folders unasked.
5. **Given** the user chose another folder, **When** the name already exists there, **Then** the app asks for a new name (validated like a rename in feature 002) and offers no replace option.
6. **Given** a restore the server refused (name taken in the meantime, folder locked, no permission, no space), **When** the answer arrives, **Then** the item stays in the trash, the app shows the plain reason with a next step, and nothing was changed on the server.
7. **Given** the connection broke after the restore request was sent, **When** the outcome cannot be determined, **Then** the app says the outcome is unknown, does not repeat the restore by itself, and lets the user refresh the trash and the folder to see what happened.
8. **Given** a restored file that has a local copy from feature 001 that was marked "removed on the server", **When** the file is back, **Then** the local copy shows its normal state again and the app has changed no file on the reader.

---

### User Story 3 - List older versions of a file and restore one (Priority: P2)

For a file, the user opens its details and chooses Versions. The app lists the older versions the server kept, newest first, each with its date and size. The user selects one and taps Restore. The version that is current at that moment is not discarded: the server keeps it as one more version, so the user can go back again. The file's current content afterwards is the chosen version's content. Nothing is downloaded to the reader by this action, and no local file is touched.

**Why this priority**: It rescues the "I saved over the good version" case, but it is less often needed than getting a deleted file back.

**Independent Test**: Against the fake server, open the versions of a file with several versions; verify date, size and order; restore one; verify that the file's current content is the chosen version's and that the former current content is listed as a version afterwards; verify a file without older versions shows a plain "no older versions" message and a folder offers no Versions action.

**Acceptance Scenarios**:

1. **Given** a file with older versions on the server, **When** the user chooses Versions in its details, **Then** the older versions are listed newest first with date and size, paged.
2. **Given** a list of versions, **When** the user selects one and confirms Restore, **Then** the file's current content becomes that version's content and the content that was current before is listed as a version afterwards.
3. **Given** the restore was done, **When** the user lists the versions again, **Then** no version has disappeared except that the restored one may be shown as the current content, and nothing was deleted.
4. **Given** the file has no older versions, **When** the user chooses Versions, **Then** the app says in plain words that there are none.
5. **Given** the file was changed on the server after the versions were listed, **When** the user confirms Restore, **Then** the app refuses to restore, tells the user that the file has changed, and offers to reload the list; it does not restore over the newer content.
6. **Given** the file has a local copy from feature 001, **When** its server content was restored to an older version, **Then** the local copy is shown as outdated like any other server change and is not overwritten by the app.
7. **Given** the user is looking at a folder, **When** the user looks for the Versions action, **Then** it is not offered (versions exist for files only).

---

### User Story 4 - Delete for good, only knowingly (Priority: P2)

A deleted item that the user is sure about can be removed from the trash for good, to free server space or to remove something that should not stay on the server. The user can delete one item for good, or empty the trash. Each of these is irreversible, so each asks for an explicit confirmation after the user's tap on the action (one confirmation for a single file; two successive confirmations for a folder and for emptying the trash), each naming exactly what will be lost: for one item its name and, for a folder, that its content is included; for emptying the trash how many items and how many folders (with content) will be lost, and a few names. The confirmation says plainly that this cannot be undone and has Cancel as its default. Nothing is deleted before the confirmation, and leaving the screen counts as Cancel.

**Why this priority**: It is useful but destructive; the safe actions (see, restore) come first, and this one may be added last without losing the rest.

**Independent Test**: Against the fake server, delete one trashed item for good with and without confirming; empty the trash with and without confirming; verify that the confirmation names the lost items and says it is irreversible, that Cancel (or leaving) deletes nothing, that exactly the named items are removed, and that items added to the trash after the list was read are not removed.

**Acceptance Scenarios**:

1. **Given** the trash list, **When** the user taps Delete for good on a file, **Then** a confirmation appears naming that file, stating that this cannot be undone, with Cancel as the default; nothing is deleted yet.
2. **Given** that confirmation, **When** the user confirms, **Then** the file is removed from the server for good and from the list.
   - **Given** a folder, **When** the user taps Delete for good, **Then** a first confirmation names the folder and says its content is included, and only after the user confirms it a second confirmation repeats the name and the loss and has Cancel as the default; the folder is removed only after the second is confirmed.
3. **Given** that confirmation, **When** the user cancels or leaves the screen, **Then** nothing is deleted and the list stays as it was.
4. **Given** a trash with items, **When** the user taps Empty trash, **Then** a first confirmation appears stating the number of items and folders that will be lost, naming several of them, stating that this cannot be undone, with Cancel as the default; after the user confirms it, a second confirmation repeats the count and has Cancel as the default.
5. **Given** both Empty trash confirmations, **When** the user confirms the second, **Then** exactly the items that were named in the confirmation are removed for good; an item that appeared in the trash after the list was read stays.
6. **Given** some items could not be removed (locked, no permission, connection loss part way), **When** the action ends, **Then** the app says how many were removed and how many were not, and keeps the others listed.
7. **Given** the user has just deleted a file with feature 002, **When** the user opens the trash, **Then** the app never deletes anything from the trash by itself, at start, on refresh or in the background.

---

### User Story 5 - Understand why trash or versions are not available (Priority: P2)

Trash and versions are server apps that an administrator can turn off. When they are off, the entries for them must not look broken. The app checks what the server offers when the user asks for the trash or for versions, and, if the trash app is disabled, says in plain words that this server has no trash app enabled, that the user's deleted files therefore cannot be listed or restored from here, and, linking to feature 002, that deleting on this server is permanent. If the versions app is disabled, it says that this server does not keep older versions, so none can be listed or restored. The messages say what the user can do (ask the server's administrator) and offer no action that would fail.

**Why this priority**: A silent failure or an empty list on such a server would look like data loss; an explanation costs little.

**Independent Test**: Against the fake server answering that the trash app is off, then that versions are off, then both on; open the trash and the versions in each case; verify the plain explanation, that no restore or delete action is offered, and that the other features still work.

**Acceptance Scenarios**:

1. **Given** the server has the trash app disabled, **When** the user opens the trash, **Then** the app explains that no trash is available on this server and offers no Restore or Delete for good.
2. **Given** the server has the versions app disabled, **When** the user chooses Versions for a file, **Then** the app explains that the server does not keep older versions and offers no Restore.
3. **Given** the trash app is enabled and the versions app is disabled (or the other way round), **When** the user uses the available one, **Then** it works normally and only the disabled one shows its explanation.
4. **Given** the server answers neither clearly "on" nor "off" (for example an unexpected error), **When** the user opens the trash or versions, **Then** the app shows a generic message with a short code and does not claim that the app is disabled.
5. **Given** the reader is offline, **When** the user opens the trash or versions, **Then** the app says that the connection is needed (or shows the stored list as in Story 1) and does not claim anything about the server's apps.

---

### Edge Cases

- A trashed item is restored or removed on another device between listing and the user's action: the server's answer decides; the app reports the plain reason and refreshes the list on request; it never forces the action.
- Two items with the same name were deleted from the same folder: both are listed with their own deletion date, and restoring the second while the first's name is taken follows the "place is taken" flow.
- A restored name is longer or has characters the server's rules forbid today: the server's refusal is shown with the plain reason; the app never changes the name by itself.
- The user restores a folder whose restore would place it inside itself or inside a folder that is itself in the trash: the app refuses and says why.
- A file and a folder of the same name are in the trash: they are distinct entries and are never merged.
- A very large trash (thousands of items): the list is fetched and stored within a stated bound, paged, and never held entirely on the screen; the bound and what happens beyond it are shown to the user.
- A very large folder or a large file is restored: the server does the work; the app shows a coarse "waiting for the server" state and the user may leave the screen without losing the result.
- A version's date is in the future or missing: it is shown as such (or as unknown) and sorted by the server's order; it is never hidden.
- The file's server content is identical in two versions: both are listed; the app does not merge them.
- The user restores a version while a local upload of the same file is queued (feature 002): the queued upload keeps its conflict rule (feature 002, "never overwrite silently") and is not changed by this action.
- The app password was revoked or expired during an action: reported as in feature 002, with an offer to reconnect, and the action is not repeated by itself.
- A name or location received for the trash is hostile (path traversal, control characters, overlong, Unicode look-alikes): it is shown in a cleaned form, bounded in length, and never used as a local path.
- Trashed items of a server where end-to-end encrypted folders exist: such entries are shown as unavailable with a reason and cannot be restored or deleted from here (as in feature 001).

## Requirements *(mandatory)*

### Functional Requirements

**Opening and listing the trash**

- **FR-001**: Users MUST be able to open the server's trash from the app, and the app MUST list every item in it with its name, its original location, and the date of its deletion, and, for a file, its size, newest deletion first, in pages with page buttons and without kinetic scrolling.
- **FR-002**: The app MUST fetch the trash only when the user opens it or taps Refresh, MUST NOT refresh a shown list on its own, and MUST show the age of the list and whether the reader is online or offline.
- **FR-003**: The app MUST store the last trash list it fetched within a stated size bound so that it can be shown, marked with its age, when there is no connection; while offline, every action that needs the server MUST be shown as unavailable with the reason.
- **FR-004**: The app MUST treat everything received about the trash (names, locations, dates, sizes, identifiers) as untrusted, bound its size and count, show a malformed item as unreadable without failing the rest, and never use a server-supplied name or location unsanitized as a local path.
- **FR-005**: A trash entry that is a file and one that is a folder MUST be told apart in the list, and an entry that is unavailable to the user (no permission, encrypted folder, unsupported state) MUST be shown as unavailable with a reason and MUST NOT offer Restore or Delete for good.

**Choosing an item**

- **FR-006**: Restore and Delete for good MUST act on one chosen trash item at a time, and the app MUST show which item is chosen before any action is taken; Empty trash is the only action that affects several items at once.

**Restoring from the trash**

- **FR-007**: Users MUST be able to restore a deleted file or folder; the proposed target MUST be the item's original location under its original name, and a restored folder MUST come back with its entire content.
- **FR-008**: A restore MUST NEVER overwrite, replace, merge into, or rename anything that exists on the server: if the original name is taken, or the original folder no longer exists, the app MUST say so and offer another folder, chosen with the folder chooser of feature 002; if the name is taken in the chosen folder too, the app MUST require a new name validated as in feature 002 and MUST NOT offer a replace option.
- **FR-009**: Users MUST be able to restore to a chosen folder deliberately even when the original place is free.
- **FR-010**: The app MUST NOT create missing parent folders by itself, and MUST refuse, with a plain reason, a target that is inside the item being restored or inside another item of the trash.
- **FR-011**: A restore MUST be conditional on the target name not existing (creation rule of feature 002, FR-022); a name that appeared in the meantime MUST be treated as a conflict that leaves the item in the trash and the existing entry unchanged.
- **FR-012**: An item MUST leave the trash list only after the server has confirmed the restore; a refused restore MUST leave the item in the trash and MUST show the plain reason with a next step, using the same plain messages as feature 002 (name forbidden, no space, locked, no permission, other with a short code).
- **FR-013**: When the outcome of a restore cannot be determined, the app MUST mark it "outcome unknown", MUST NOT repeat it by itself, and MUST let the user check by refreshing the trash and the folder (feature 002, FR-019 and FR-020).
- **FR-014**: A restore MUST NOT download, change, move or delete any file on the reader; a local copy that was shown as "removed on the server" MUST return to its normal state once the file is back, and a local copy with unconfirmed changes MUST NOT be touched.

**Versions**

- **FR-015**: For a file, the user MUST be able to list its older versions that the server keeps, newest first, each with its date and size, in pages; a folder MUST NOT offer this action; a file without older versions MUST show a plain message saying so.
- **FR-016**: Users MUST be able to restore a chosen older version of a file after a confirmation that names the file and the version (date and size), and says that the current content is kept as one more version.
- **FR-017**: Restoring a version MUST keep the content that was current before as an additional version and MUST NOT delete or discard any version; the app MUST NOT offer an action that deletes a version.
- **FR-018**: Restoring a version MUST be conditional on the file still being at the server version that the user last saw (feature 002, FR-022); if the file changed meanwhile, the app MUST refuse, say that the file changed, offer to reload the list, and change nothing.
- **FR-019**: Restoring a version MUST NOT download anything, MUST NOT overwrite or change any file on the reader, and a local copy MUST then be shown as outdated by the ordinary rule of feature 001; a queued change for the same file keeps the conflict rules of feature 002.
- **FR-020**: When the outcome of a version restore cannot be determined, the app MUST apply FR-013 in the same way.

**Deleting for good**

- **FR-021**: Users MUST be able to delete an item of the trash for good, and to empty the trash; both MUST be offered only on the trash screen, MUST be clearly marked as irreversible, and MUST NOT be offered anywhere else in the app (feature 002, FR-032 stays true for every other screen: deleting a file or folder in a folder listing still only moves it to the trash).
- **FR-022**: Each of these actions MUST require an explicit confirmation after the user's tap on the action. Deleting a single file for good MUST ask one confirmation; Empty trash and deleting a folder for good MUST ask two successive confirmations, the second shown only after the first was confirmed. Every confirmation MUST name what will be lost (for one item its name and, for a folder, that its content is included; for emptying the trash the number of items and of folders with their content, and the names of several of them), MUST state that this cannot be undone, MUST have Cancel as its default, and MUST be cancelled by leaving the screen; Cancel at either of two confirmations MUST delete nothing.
- **FR-023**: Nothing MUST be deleted for good before that confirmation, and a confirmation MUST apply to exactly the items that it named: items that reached the trash after the list was read MUST NOT be removed by it.
- **FR-024**: The app MUST NEVER delete from the trash by itself: not at start, not on refresh, not on a timer, not in the background, and not as a side effect of any other action.
- **FR-025**: When some of several items cannot be removed, the app MUST report how many were removed and how many were not, with the plain reason for the others, and MUST keep the others listed.
- **FR-026**: Deleting from the trash for good MUST NOT remove any file on the reader, and a local copy of such a file MUST keep the state it has under feature 001 ("removed on the server").

**When the server does not offer it**

- **FR-027**: The app MUST find out, when the user opens the trash or the versions of a file, whether the server has the trash app and the versions app enabled, and MUST NOT guess it from an empty list or from an error.
- **FR-028**: If the trash app is disabled, the app MUST say in plain words that this server has no trash, that deleted files cannot be listed or restored from here, and that deleting on this server is permanent (as in feature 002, FR-033); it MUST NOT offer Restore, Delete for good or Empty trash.
- **FR-029**: If the versions app is disabled, the app MUST say in plain words that this server does not keep older versions and MUST NOT offer a list or a Restore of versions.
- **FR-030**: If the server's answer is unclear or an error, the app MUST show a generic message with a short code and MUST NOT state that an app is disabled; the two apps MUST be judged independently of each other.

**Safety, privacy and interface**

- **FR-031**: Every action of this feature MUST be an explicit user action on a connected reader; the only network activity MUST be: opening or refreshing the trash or a versions list, a confirmed restore, and a confirmed delete for good. There MUST be no polling, no background activity and no automatic repeat of an action whose outcome is unknown.
- **FR-032**: The app MUST NOT write file names, folder names, paths, server addresses or user names to logs, diagnostic exports or error messages shown to the user except where the name is the thing the user asked about on the screen; secrets MUST never appear anywhere.
- **FR-033**: A revoked or expired app password during an action MUST be reported as in feature 002 (FR-039), with an offer to reconnect and without repeating the action by itself.
- **FR-034**: All new interface texts MUST be in English with a German translation, and the screens MUST look and behave like the rest of the app: the existing component set, icon-and-label tools in a bottom panel, dialogs with footer buttons, large touch targets, page-based lists, black-and-white high contrast, no animation.
- **FR-035**: Restoring from the trash, restoring a version and deleting for good MUST run only right away, on a connected reader, as the user's explicit action: nothing MUST be put in the persistent queue of feature 002, and while the reader is offline all three MUST be shown as unavailable with the reason (a connection is needed).
- **FR-036**: This feature lifts the rule of feature 002 that the app does not delete permanently, but only for the actions of FR-021 on the trash screen; retention settings, comments and the activity feed remain out of scope, and everything else feature 001 and feature 002 guarantee stays in force.

### Key Entities

- **Trash item**: a file or folder that was deleted on the server and is still held there. It has a name, an original location (the folder it was deleted from), a deletion date, a kind (file or folder), a size (files only), and an identity that the server gives it. Two items may share a name and location.
- **Trash list**: the last fetched list of trash items, stored with the time it was fetched and bounded in size; it is a copy that may be out of date.
- **File version**: an older content of one file that the server keeps, with a date and a size and an identity; the current content is not counted among the older versions, and a restore turns a chosen version into the current content while the previous current content becomes a version.
- **Restore target**: the folder and name under which a trash item will come back; it starts as the original location and name and may be changed by the user, but it never names something that exists.
- **Server capability state**: whether the server has the trash app and the versions app enabled (on, off, or unknown), found out when the user opens the trash or the versions, and never guessed.
- **Irreversible-action confirmation**: the dialog shown before a delete for good, naming exactly what will be lost; it applies only to the items it named.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A user can find a file they deleted by mistake and restore it to its original place in at most 4 taps from the folder listing and under 30 seconds of the user's own time, on a reader whose trash holds 50 items.
- **SC-002**: In 100% of the automated runs, no restore overwrites, replaces or renames an existing server entry, whatever the state of the target name or folder.
- **SC-003**: In 100% of the automated runs, no item is deleted for good without the required confirmation(s) that name it (one for a file, two for a folder and for Empty trash), none is deleted after Cancel or after leaving the screen, and no item that was not named in the confirmation is deleted.
- **SC-004**: In 100% of the automated runs, restoring an older version leaves the previous current content available as a version, and no version is deleted by any action of the app.
- **SC-005**: In 100% of the automated runs, a server with the trash app or the versions app disabled produces the plain explanation and no action that cannot work; and an unclear server answer never produces the claim that an app is disabled.
- **SC-006**: In 100% of the automated runs with a refused restore, an interrupted connection or a revoked password, no item is lost from the trash, no action is repeated by the app on its own, and the user sees one plain message with a next step.
- **SC-007**: A trash of 1,000 items is browsable in pages without the app holding more than the stated bound in memory and storage, and a version list of 100 entries is browsable the same way; the bounds are stated in the plan and checked by tests (marked unverified until measured on the reader).
- **SC-008**: Behavior that needs a real Nextcloud server or the reader (how the server names trash locations, versions of very large files, a server with the apps disabled) is recorded as unverified until it has been exercised there.

## Assumptions

- The trash and the versions are the ones of the user's own Nextcloud account, offered by the server's trash and versions apps; the app talks to them through the same https connection and app-password login as features 001 and 002.
- The server returns for each trash item its name, its original location and its deletion date; where the server returns an original location only as a name without a path, the original place is treated as unknown and the user is asked to choose a folder.
- The server keeps the current content as a version when a version is restored (that is the server's own rule); the app relies on it and checks the result by listing again, and it reports an unverified server that does not.
- A deleted file is in the trash only if the server has the trash app enabled; when it is not, deletions on that server are permanent, as feature 002 already says.
- Folders have no versions; only files do.
- Restoring to a chosen place needs the server to allow a move out of the trash; this is the least certain server behavior of the feature and is unverified. A server that refuses it gets a plain message that it can restore only to the original place (what to do: rename or move the file that is there, then restore again); nothing is changed, and the "chosen place" scenarios hold on servers that allow it.
- Items of the trash are not part of the file tree, so a restore target can lie inside the item itself only for a folder restored into its own former path; a target "inside another item of the trash" cannot arise and needs no separate check.
- The trash and version actions are never put in the persistent queue of feature 002 (FR-035) and are never repeated by the app after an unknown outcome.
- Each user keeps their own trash; shared or group folders may show items from other users in the same trash and are handled as the server returns them.
- A restore into a folder that is shared, or into one that is read-only for the user, is refused by the server, and the app shows the plain reason.
- Downloading or previewing an older version, restoring several versions at once, comparing versions, and naming or protecting a version are not part of this feature.
- The reader's storage, the local copies of feature 001 and the offline marks are never changed by any action of this feature; a state shown for a local copy follows only from what the server now holds.
- Out of scope: retention settings (how long the server keeps trash and versions, and automatic expiry), comments, the activity feed, deleting a single version, restoring from another user's trash, and any change to the server.
