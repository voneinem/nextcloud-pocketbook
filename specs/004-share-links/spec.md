# Feature Specification: Share Links

**Feature Branch**: `feature/004-share-links`

**Created**: 2026-10-22

**Status**: Draft

**Input**: User description: "Share links from the PocketBook. Builds on feature 001. For a file or folder the user can create a public share link, optionally with a password, an expiry date and read-only or (for folders) upload permission; see the existing links and shares of that item (including shares with users and groups, read-only list); copy the link text so that it can be shown or typed elsewhere; change a link's password, expiry or permission; and remove a link after a confirmation. Links and passwords are shown as plain text only on the screen the user asked for, are never opened by the app, never written to logs, and the password is never stored on the reader after it was sent. Show clear messages when the server forbids public links, forces a password or an expiry date, or limits the permissions. Out of scope: creating shares with users or groups, share-by-mail, Talk, notes-to-recipient, QR codes."

## Clarifications

### Session 2026-10-22

- Q: Should the app offer a Copy button that uses a clipboard other apps can paste from, or only show the link on screen for typing? → A: Both. The link is always shown legibly; a Copy button is added only when the reader has a system clipboard (hidden otherwise); its behavior on the device is unverified until tested there.
- Q: When the user creates a link for a folder, which permissions can they choose? → A: Two: read-only, and read-and-upload (recipients can see and add files). Upload-only (file drop) is not offered for new links; existing upload-only links are listed with their permission.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Create a public link for a file or folder (Priority: P1)

The user has a file or folder in their Nextcloud and wants to give someone access to it. They select the item in the folder listing of feature 001 and choose Share. The app asks the server for the existing sharing state of the item (Story 2) and offers Create link. The user may set a password, an expiry date, and the permission: read-only (the default), or, for a folder only, read and upload. Before anything is sent, the screen states in plain words what the link will allow ("anyone who has this link can read this file", "anyone with this link can read this folder and add files to it") and whether a password and an expiry date are set. After the user confirms, the app asks the server to create the link and then shows the new link (Story 3). Creating a link is a deliberate act: nothing is created on opening the screen, and nothing is created without a connection.

**Why this priority**: Creating a link is the reason for the feature; every other story works on links that this story produces.

**Independent Test**: Against a simulated server, select a file, create a link with the defaults, and verify that exactly one create request is made after the confirmation and none before, and that the new link is shown; repeat for a folder with a password, an expiry date and upload permission and verify that the request carries those three choices and that the link shown reports them.

**Acceptance Scenarios**:

1. **Given** a connected reader and a file in a listing, **When** the user chooses Share and then Create link, **Then** the screen offers password, expiry date and permission, the permission defaults to read-only, and nothing is created until the user confirms.
2. **Given** a folder, **When** the user opens the permission choice, **Then** read-only and read-and-upload are offered; **Given** a file, **Then** only read-only is offered.
3. **Given** the choices are made, **When** the user confirms, **Then** the app states what the link allows, creates the link, and shows it with its password state, expiry date and permission as reported back by the server.
4. **Given** the user sets a password, **When** the link is created, **Then** the password is sent once with the request and is not kept anywhere on the reader afterwards (Story 7).
5. **Given** the user enters an expiry date in the past or a malformed date, **When** they try to confirm, **Then** the app refuses with the reason and sends nothing.
6. **Given** the reader has no connection, **When** the user chooses Share, **Then** the app says that sharing needs a connection, offers nothing that looks like it will work later, and queues nothing.
7. **Given** the server refuses or changes the choices (Story 6), **When** the link is created, **Then** the app shows what was refused or changed, and never shows a link that does not exist on the server.
8. **Given** an item that is unavailable to the user (no permission to share it, an encrypted folder, an entry in an unsupported state), **When** the user opens its actions, **Then** Share is shown as unavailable with the reason.

---

### User Story 2 - See the existing links and shares of an item (Priority: P1)

The user opens Share on an item and sees what is already shared. The screen shows a paged list with one row per share: public links (with label if any, permission, whether it has a password, and the expiry date), shares with individual users and with groups (the name of the person or group, the permission, and the expiry date if any), and shares of any other kind marked as "other kind of share" without further detail. The list for users and groups is read-only: the app does not create, change or remove them. Each public link opens its detail screen (Stories 3, 4 and 5). The list is fetched from the server because the user asked for it and is not kept after the user leaves the screen, so an old state is never shown as current.

**Why this priority**: The user cannot manage links without seeing them, and seeing who else has access is the safe way to know what a new link adds.

**Independent Test**: Against a simulated server holding two public links, one user share, one group share and one share of another kind on a folder, open Share on the folder and verify that all five rows appear with the right kind and permission, that only the two links open a detail screen, and that leaving and reopening the screen makes a new request.

**Acceptance Scenarios**:

1. **Given** an item with shares, **When** the user opens Share, **Then** the list shows each public link, user share and group share with kind, permission and expiry date, in pages with page buttons and without kinetic scrolling.
2. **Given** a user or group share, **When** the user taps it, **Then** its details are shown read-only, with no control that changes or removes it.
3. **Given** an item without any share, **When** the user opens Share, **Then** the screen says that nothing is shared and offers Create link.
4. **Given** the server answers with a share kind or a field the app does not know, **When** the list is shown, **Then** that row appears as "other kind of share", the rest of the list works, and nothing crashes.
5. **Given** the reader loses its connection while the list is open, **When** the user acts on it, **Then** the app says it is offline and does not claim that the list is current.
6. **Given** the list shows a share that the signed-in user did not create (for example a share on a parent folder, or one created by another person), **When** the row is shown, **Then** it is marked as not owned by the user and offers no change or remove action.
7. **Given** the user leaves the screen, **When** they come back, **Then** the list is fetched again and nothing from the earlier visit is shown as current.

---

### User Story 3 - Show the link so it can be typed or copied elsewhere (Priority: P1)

The user needs the text of a link somewhere else: to type it into another device, read it out, or paste it into a message on another app. The detail screen of a public link shows the full link text as plain text in large letters, wrapped at any character so that no part is cut off, with a hint of its length. Where the reader has a system clipboard that other apps can paste from, a Copy action also places the text there; where it has none, there is no Copy action and the shown text is the way to carry the link. The link text is shown only on this screen, opened by the user's tap on that link, and not in lists, in messages, or on any other screen. The app never opens the link, never loads the page behind it, and never offers to open it in a browser.

**Why this priority**: A link that cannot be carried to its recipient is worth nothing; the reader has no way to send it by itself, so showing it legibly is the delivery.

**Independent Test**: Create a link with a 120-character address in the simulated server, open its detail screen, and verify that the whole text is shown in order without truncation, that no request to the link's address is made, that the same text does not appear on the list screen or in any message, and that leaving the screen removes it.

**Acceptance Scenarios**:

1. **Given** a created link, **When** the user opens its detail screen, **Then** the complete link text is shown as plain text, readable at the reader's normal viewing distance, with no part cut off.
2. **Given** the detail screen, **When** the user taps Copy, **Then** the exact link text is placed on the system clipboard and the screen confirms it; **Given** a reader without a system clipboard, **Then** no Copy action is shown and the link text on the screen is complete and legible.
3. **Given** the list screen, **When** the user looks at a link's row, **Then** the full link text is not shown there, only its kind, label, permission, password state and expiry.
4. **Given** the detail screen, **When** the user taps the link text, **Then** nothing is opened: no browser, no viewer, no request to the link's address.
5. **Given** the user leaves the detail screen, **When** they come back to the app later, **Then** the link text is shown again only after they open the link's detail screen again.
6. **Given** a link with a password, **When** the detail screen is shown, **Then** it says that a password is set and does not show the password, because the server does not give it back and the reader does not keep it.
7. **Given** a link shown on the detail screen, **When** the user checks the app's logs, exported files and messages, **Then** the link text is not found in any of them (Story 7).

---

### User Story 4 - Change a link's password, expiry date or permission (Priority: P2)

A link that was created earlier is too open, too old or needs a new password. On the link's detail screen the user can set or replace the password, remove the password, set or remove the expiry date, and change the permission between read-only and read-and-upload (for folders). Each change is a separate, explicit action with a confirmation that states the effect ("the old password stops working", "the link will not work after 31 December", "anyone with the link will be able to add files"). The new state is read back from the server and shown. The link text itself does not change.

**Why this priority**: Tightening or fixing a link is the common follow-up after sharing, but a user can already work around its absence by removing the link and creating a new one.

**Independent Test**: Against a simulated server, change a link's password, set an expiry date, switch a folder link to read-and-upload, then remove the password, and verify after each action that exactly one change request is sent, that the screen shows the state the server reports, and that the link text is unchanged.

**Acceptance Scenarios**:

1. **Given** a link without a password, **When** the user sets one and confirms, **Then** the password is sent once, the link then shows "password set", and the password is not kept on the reader.
2. **Given** a link with a password, **When** the user enters a new one and confirms, **Then** it is replaced, and the screen states that the old password no longer works.
3. **Given** a link with a password, **When** the user removes the password and confirms, **Then** the link shows "no password", unless the server forces a password (Story 6).
4. **Given** a link, **When** the user sets, changes or removes the expiry date and confirms, **Then** the server's resulting date is shown, and a date the server refuses is reported (Story 6).
5. **Given** a folder link, **When** the user changes its permission and confirms, **Then** the new permission is shown as the server reports it.
6. **Given** a link that someone else changed on the server after the screen was opened, **When** the user confirms a change, **Then** the app tells the user that the link changed meanwhile, shows the current state, and applies nothing until the user confirms again.
7. **Given** a share the user does not own, **When** its row or detail is shown, **Then** no change action is offered.
8. **Given** any failed change, **When** the error is shown, **Then** the link keeps the state the server reports and the app does not retry by itself.

---

### User Story 5 - Remove a link after a confirmation (Priority: P2)

The user no longer wants a link to work. On the link's detail screen they choose Remove link. The app asks for a confirmation that names the item and states the effect ("the link stops working at once for everyone who has it; the file itself is not deleted"). Only on confirmation does the app ask the server to remove the link. Afterwards the list no longer shows it. Removing a link never deletes, moves or changes the shared file or folder.

**Why this priority**: Withdrawing access is the safety counterpart of creating a link, but it is needed less often than creating.

**Independent Test**: Against a simulated server, remove a link, verify that no request is made before the confirmation and exactly one after it, that the list no longer shows the link, and that the shared file is still present.

**Acceptance Scenarios**:

1. **Given** a link's detail screen, **When** the user chooses Remove link, **Then** a confirmation names the item, states the effect, and defaults to cancel.
2. **Given** the confirmation, **When** the user cancels, **Then** nothing has been sent and the link is unchanged.
3. **Given** the confirmation, **When** the user confirms, **Then** the app asks the server to remove the link, and on success shows the list without it.
4. **Given** the link was already removed on the server, **When** the user confirms, **Then** the app reports that it was already gone and shows the current list, without an error that implies data loss.
5. **Given** the server refuses the removal (no permission, not reachable), **When** the answer arrives, **Then** the link stays in the list, the reason is shown, and nothing is retried by itself.
6. **Given** a link is removed, **When** the user looks at the shared file or folder, **Then** it is unchanged on the server and on the reader.
7. **Given** a share the user does not own, **When** its detail is shown, **Then** Remove is not offered.

---

### User Story 6 - Clear messages when the server limits sharing (Priority: P2)

The server's administrator can switch off public links, force a password, force an expiry date (with a maximum number of days), or limit the permissions (for example no upload through public links, or no sharing of some folders). The app knows these rules from what the server reports and from its answers, and tells the user in plain words with a next step: public links are not allowed on this server; a password is required (the password field is marked and cannot be left empty); an expiry date is required and may be at most N days away (the date field is marked and limited); upload permission is not allowed; the item cannot be shared; the user may not change this share. When the server's rule is known before the user acts, the controls already reflect it; when it only shows in the server's refusal, the refusal is shown in the same plain way. Nothing is retried silently, and the server's own text is never shown unsanitized.

**Why this priority**: Without understandable reasons the user cannot succeed against a policy on an e-ink device with no other tools; it is lower than the core flows because the core flows work on a server without such rules.

**Independent Test**: Against a simulated server configured in turn to forbid public links, to force a password, to force an expiry of at most 7 days, to forbid upload on public links, and to refuse sharing of the item, open Share and create a link, and verify that each case yields its own message and next step, that forced fields are marked, and that no link is shown that was not created.

**Acceptance Scenarios**:

1. **Given** a server that does not allow public links, **When** the user opens Share, **Then** the screen says that public links are not allowed on this server, Create link is not offered, and the lists of existing shares still show.
2. **Given** a server that forces a password, **When** the user creates or edits a link, **Then** the password field is marked required, an empty password cannot be confirmed, and removing the password is refused with the reason.
3. **Given** a server that forces an expiry date with a maximum, **When** the user creates or edits a link, **Then** the date field is marked required, dates beyond the maximum are refused with the limit stated, and the maximum is the default.
4. **Given** a server that does not allow upload through public links, **When** the user opens the permission choice for a folder, **Then** read-and-upload is shown as unavailable with the reason.
5. **Given** a rule that the app only learns from the server's refusal, **When** the refusal arrives, **Then** the message names the kind of rule (password, expiry, permission, not allowed) as far as the server says, and otherwise says that the server refused without a known reason and shows a short code.
6. **Given** any refusal, **When** the message is shown, **Then** it contains no link, no password, no server address and no names from the server, and the user's entries are kept on the screen so that they can correct them.
7. **Given** a revoked or expired app password, **When** any sharing action is tried, **Then** the app reports that the login is no longer valid and offers to reconnect (as in feature 001).

---

### User Story 7 - Links and passwords stay private (Priority: P1)

The user shares private files, and a link or password is as sensitive as the file. The app therefore treats both as secrets with strict rules: a link or password appears as plain text only on the screen that the user opened for it (the link's detail screen, the entry field of a password); it is not shown in lists, notifications, error messages or the About box; it is never written to a log, a diagnostic export or a stored error record; it is never opened or fetched by the app; and a password the user typed is sent once to the server and is then discarded: no copy of it is stored (saved) on the reader after the request has finished, including when the request fails (the user types it again), and the app wipes the memory it controls. Leaving the screen ends the display of both, and so does switching to another app or the reader going to sleep where the platform reports it.

**Why this priority**: These rules are a constitutional requirement and the main risk of the feature; a leak here cannot be undone.

**Independent Test**: Create, change and remove links with known link text and password in a simulated server while capturing everything the app writes (logs, database, settings and cache files, exported files), then verify by searching for the link text, the password and the link's token that neither occurs anywhere outside the screens listed above, that no request is made to the link's address, and that the password is not in any file after success or failure.

**Acceptance Scenarios**:

1. **Given** a password typed for a new link, **When** the request finishes (success or failure), **Then** the password is not present in any file or database of the app, and a failed request asks the user to type it again.
2. **Given** a link created, shown, changed or removed, **When** the app's logs are inspected, **Then** they contain no link text, token, password, server address, user name, file or folder name or path.
3. **Given** an error from a sharing request, **When** the message and the stored error record are inspected, **Then** neither contains a link, token or password.
4. **Given** a link's detail screen, **When** the user taps the link text or any control, **Then** the app never starts a browser, never opens the address, and never sends a request to it.
5. **Given** the link's detail screen is open, **When** the user leaves the screen (or, where the platform reports it, the reader goes to sleep or the app is switched away), **Then** the link text is cleared from the app's screens and memory it controls, and is shown again only after a new tap on the link.
6. **Given** the mechanical constitution checks, **When** a log call that could carry a link, token or password is added, **Then** the check fails (the existing rule on share links in logs is extended and self-tested).
7. **Given** the app's backup or export functions, **When** they produce a file, **Then** it contains no link text and no password.

---

### Edge Cases

- The file or folder is renamed, moved or deleted on the server after the listing was fetched: the share request fails with that reason, nothing is created for another item, and the user is told to refresh the listing.
- The same item already has a public link: the app lists it and still allows creating another one, with a notice that this adds a second link; nothing is merged.
- The server answers with a link that has an unexpected form (no address, very long, control characters, a different host than the connected server): the link is shown as unreadable or flagged "different server", never shown as if valid, and the rest of the screen works.
- A response to a create or remove request is lost (timeout after sending): the app does not repeat the request silently; it says the result is unknown and offers a refresh of the list, so that the user can see whether the link now exists. A repeated create by the user is a new, deliberate action.
- The user's password contains characters that are hard to type or that the keyboard layout cannot produce: the entry field shows what was typed, so the user can check it, and the server's own rules for passwords (length, strength) are reported when it refuses.
- The expiry date entered is today, or in a different time zone than the server: the shown date is the date the server reports, and the reader's clock is not used to decide whether a link is expired.
- A folder is shared with upload permission and the server also forces a password: both rules apply at the same time and both are explained.
- Very many shares on one item (hundreds): the list is paged and fetched in bounded pieces; the screen stays usable.
- The signed-in account is changed or disconnected while a sharing screen is open: the screen closes, nothing is sent to the other account, and no link or password remains.
- The user picks a link in the list while another change is in flight: only one sharing request runs at a time; the second action waits for the first and is shown as waiting.
- The server reports a share the user may not see in detail: the row is shown as limited and nothing about it is invented.
- Shares are not available at all (an old or restricted server): the Share action explains that this server does not offer sharing, and nothing else in the app is affected.

## Requirements *(mandatory)*

### Functional Requirements

**Create**

- **FR-001**: Users MUST be able to open Share for a file or folder from the folder listing and from the item's details, and the system MUST show the item's existing shares (Story 2) before offering to create a link.
- **FR-002**: Users MUST be able to create a public link for a file or a folder, with optional password, optional expiry date and a permission: read-only (default) for files and folders, read-and-upload additionally for folders. Upload-only (file drop) MUST NOT be offered for new links; an existing link with that permission MUST be listed with it.
- **FR-003**: The system MUST create a link only after an explicit confirmation that states in plain words what the link allows, whether a password is set and whether it expires, and MUST NOT create anything when the screen is opened.
- **FR-004**: The system MUST validate the expiry date before sending (a valid date, not in the past) and MUST send nothing when it is invalid.
- **FR-005**: The system MUST show the created link only from what the server reports back, and MUST NOT show a link that the server did not confirm.
- **FR-006**: The system MUST require a connection for every sharing action and MUST NOT queue, defer or retry any sharing action by itself. A tap on a sharing action while the reader is offline asks the system to bring Wi-Fi up, as the files client does, and goes on when it is up; if it does not come up, the action fails with the "needs a connection" message and nothing is sent. The app never does this at start, while idle or without a tap. *(Changed 2026-10-23 after the first test on the reader: the first version only said "switch on Wi-Fi".)*

**See**

- **FR-007**: The system MUST list the existing shares of an item in pages with page buttons and no kinetic scrolling: public links, shares with users, shares with groups, and other kinds marked as "other kind of share".
- **FR-008**: The system MUST show, per share, its kind, its permission and its expiry date where there is one, and for links also the label and whether a password is set.
- **FR-009**: Shares with users and groups MUST be read-only in this feature: the system MUST NOT create, change or remove them, and MUST NOT offer any control that suggests it can.
- **FR-010**: The system MUST mark shares the signed-in user does not own and MUST NOT offer change or remove actions on them.
- **FR-011**: The system MUST fetch the share list only on the user's request and MUST NOT keep it after the user leaves the screen.

**Show and copy**

- **FR-012**: The system MUST show the full link text, as plain text, only on the detail screen of that link, opened by the user's tap, in large letters wrapped at any character so that no part is cut off. The link MUST always be shown this way. Where the reader has a system clipboard that other apps can paste from, the system MUST also offer Copy, which places the exact link text there; where it has none, Copy MUST NOT be shown. Whether the clipboard works on the device is unverified until tested there.
- **FR-013**: The system MUST NOT open, follow, preview or fetch a link, MUST NOT offer to open it in a browser, and MUST NOT make any request to a link's address.
- **FR-014**: The system MUST NOT show the full link text in lists, messages, notifications or any screen other than the link's detail screen.
- **FR-015**: The system MUST NOT show a link's password; a link with a password is shown as "password set".

**Change and remove**

- **FR-016**: Users MUST be able to set, replace and remove a link's password, set, change and remove its expiry date, and change a folder link's permission, each as a separate explicit action with a confirmation that states the effect.
- **FR-017**: The system MUST show the state after a change as the server reports it, and MUST detect that a link was changed on the server after the screen was opened, tell the user, and apply nothing until the user confirms again.
- **FR-018**: Users MUST be able to remove a public link only after a confirmation that names the item, states that the link stops working at once for everyone and that the item itself is not deleted, and that defaults to cancel.
- **FR-019**: Removing a link MUST NOT delete, move or change the shared item, and a link that is already gone MUST be reported as already removed, not as a failure.
- **FR-020**: The system MUST NOT repeat a create, change or remove request silently; a request whose outcome is unknown MUST be shown as such with a refresh of the list as the way to check.

**Server rules and messages**

- **FR-021**: The system MUST show distinct plain messages, each with a next step, for: public links not allowed, password required, expiry date required (with the maximum when known), upload permission not allowed, item cannot be shared, share not changeable by the user, sharing not offered by the server, and an unrecognized refusal (with a short code).
- **FR-022**: The system MUST reflect a server rule in the controls (required fields marked, limits stated, unavailable choices shown with the reason) when it is known before the user acts, and MUST present it in the same plain way when it is learned from a refusal; the user's entries MUST be kept on the screen after a refusal.
- **FR-023**: The system MUST NOT show server-supplied text unsanitized, and messages MUST NOT contain links, tokens, passwords, server addresses or names from the server.
- **FR-024**: The system MUST report a revoked or expired login as such and offer to reconnect, as feature 001 does.

**Privacy and safety**

- **FR-025**: The system MUST send a password once with the request that needs it and MUST NOT store (save) it in any form on the reader afterwards (no file, database entry, setting, log or error record), whether the request succeeds or fails, and MUST wipe the memory it controls that held it; after a failure the user types it again.
- **FR-026**: The system MUST NOT write a link, link token, password, server address, user name, or file or folder name to any log, diagnostic export, stored error record or exported file, and the mechanical constitution checks MUST be extended with a rule and a self-test that detect a violation.
- **FR-027**: The system MUST NOT store links in its database, settings or cache; a link exists on the reader only while its detail screen is shown.
- **FR-028**: The system MUST end the display of a link and of an entered password when the user leaves the screen, when the account is changed or disconnected, and, where the platform reports it, when the app is switched away or the reader goes to sleep.
- **FR-029**: The system MUST validate the links and fields it receives from the server (size limits, control characters, a link on a different host than the connected server) and MUST show a malformed entry as unreadable or flagged, never crash.
- **FR-030**: The system MUST NOT perform any network activity for this feature other than on the user's explicit action on a sharing screen (Principle VI).

**Interface**

- **FR-031**: The new screens MUST use the existing component set and look and behave like the screens of feature 001 and 002 (page-based lists, icon plus label in the bottom panel, dialogs with footer buttons, no animation, operation with the on-screen keyboard), and every new string MUST have an English source text and a German translation.
- **FR-032**: The system MUST show the offline state on every sharing screen and MUST NOT present stored data of an earlier visit as current.
- **FR-033**: The read-only rule of feature 001 and the server-write rules of feature 002 MUST be extended only as follows: this feature may create, change and remove public share links on the server on the user's explicit action; it MUST NOT create, change or remove shares with users or groups, MUST NOT send mail, MUST NOT add notes to recipients, and MUST NOT create any other kind of share.

### Key Entities

- **Shareable item**: a file or folder on the server as listed by feature 001, with the information whether the user may share it. A folder additionally allows the upload permission on a link.
- **Public link**: a share that anyone holding its address can use, with a label (optional), a permission (read-only or read-and-upload), a password state (set or not set, never the password itself), an expiry date (optional), an owner, and the link text. It exists on the reader only while its detail screen is shown.
- **User or group share**: a share with a named person or group, with permission and expiry date; shown read-only.
- **Other share**: a share of a kind the app does not handle; shown as such with no detail.
- **Server sharing rules**: what the server reports about sharing for the connected account: whether public links are allowed, whether a password or an expiry date is required and the maximum days, and which permissions are allowed. Learned on request and not kept after the screen is left.
- **Sharing message**: the plain-language reason and next step shown for a server rule or refusal; contains no secret.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A user can create a read-only public link for a file, starting from the folder listing, in under 1 minute and at most 8 taps and keyboard entries, with no password or expiry date.
- **SC-002**: A user can read the complete text of a 120-character link off the screen without scrolling horizontally and without any part cut off, in 100% of tested cases.
- **SC-003**: In 100% of tested runs against a simulated server, no create, change or remove request is sent before the user's explicit confirmation, and exactly one request is sent after it.
- **SC-004**: In 100% of tested runs, neither a link text, a link token nor a password appears in any log, database, settings, cache, export or error record of the app, and no request is ever made to a link's address.
- **SC-005**: In 100% of tested runs, a password typed by the user is absent from every file and database of the app once the request has finished, whether it succeeded or failed.
- **SC-006**: In each of the tested server configurations (public links forbidden, password forced, expiry forced with a maximum, upload forbidden, item not shareable, sharing unavailable), the user sees a distinct message with a next step, and in 100% of those cases the entries they made are still on the screen.
- **SC-007**: Opening Share on an item with 200 shares shows the first page of the list within 5 seconds on the device's Wi-Fi, and every page is reachable with the page buttons.
- **SC-008**: Removing a link takes at most 4 taps once the item is selected in the folder listing (Share, the link, Remove link, confirm), never deletes or changes the shared item, and the list no longer shows the link afterwards in 100% of tested cases.
- **SC-009**: With the app idle, switching Wi-Fi off and on, or leaving a sharing screen, no network request is made by this feature in any tested run.
- **SC-010**: Every new screen string is available in English and German, and every new screen shows the offline state.
- **SC-011**: Behavior against a real Nextcloud server and on the InkPad One is labelled unverified until exercised there; the criteria above are budgets for the simulated server and the host tests until measured on the device.

## Assumptions

- Feature 001 is merged and provides the connection, the folder listings, the entry states (including unavailable entries) and the login handling this feature builds on; feature 002 provides the plain-message style for server refusals that this feature follows. Feature 003 is not required.
- The reader has no way to send a message by itself, so "copy" means showing the link legibly for typing and, where the reader has a system clipboard, also a Copy action (FR-012).
- A folder offers two public-link permissions: read-only and read-and-upload. The "upload only" (file drop) permission of some servers is not offered for new links; an existing link of that kind is listed with that permission, and the app does not change it to or from another permission unless the user picks one of the two offered. A file link is always read-only; links that let recipients edit a file are not offered.
- The expiry date is entered as a calendar date with the on-screen keyboard or the date control of the shared component set; the format shown is the one of the reader's language setting, and the date the server reports is authoritative.
- The default for a new link is read-only, no password and no expiry date, unless the server forces a password or an expiry date, in which case the forced value or maximum is the default.
- Existing shares are fetched when the user opens Share and are not stored on the reader; the app does not remember links between visits, so reopening a link's detail screen always asks the server again.
- Only links owned by the signed-in user can be changed or removed; other shares are shown and marked.
- Several links can exist on one item; the app never merges them.
- The server's own wording is not shown; the messages are the app's own and are translated.
- Behavior that depends on a real Nextcloud server (which sharing rules are reported, how a password or an expiry date is refused, what a link looks like, how many shares are returned) and on the real device is **unverified** until exercised there; fixtures that imitate server answers carry the `.unverified` marker.
- Out of scope: creating, changing or removing shares with users or groups, share-by-mail, Talk, notes to the recipient, QR codes, opening or previewing a link, sending a link by any channel from the app, a link history, sharing from the queue or from the reader's own storage, and background or automatic activity of any kind.
