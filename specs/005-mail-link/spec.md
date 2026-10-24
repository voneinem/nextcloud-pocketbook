# Feature Specification: Mail Link

**Feature Branch**: `feature/005-mail-link`

**Created**: 2026-10-24

**Status**: Draft

**Input**: User description: "Feature 005 'Mail link': on the link screen of a share (the screen that shows the share link after Share), add a 'Mail link' tool that hands the link to PB Office, which opens a new mail message containing it. The user still reviews and sends the mail in PB Office. A confirmation that defaults to Cancel says where the link goes and that the password, if any, is not included. One hand-off call with a subject such as 'Link to <item name>' and no recipient; at most 2 taps from the link screen to the editor; if PB Office is missing or the call is not accepted, a short calm message and no retry. Links and passwords are shown as plain text only on the screen the user asked for, are never opened by the app, never written to logs, and the password is never stored on the reader after it was sent; the password is never handed over. Out of scope: other mail apps, recipients, attachments, mailing the password, a general share menu, a clipboard Copy, handing over to a running PB Office, any change to PB Office."

## Clarifications

### Session 2026-10-24

- Q: What text should the new mail contain when the link is handed to PB Office? (FR-008) → A: The link only; the user adds their own words in PB Office.
- Q: Should the Mail link tool be shown when PB Office is not installed on the reader? (FR-020) → A: Always shown; if PB Office is missing, the failure message of FR-011 appears.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Send a link by mail from the link screen (Priority: P1)

The reader has no system clipboard, so a link shown on the link screen can only be carried to its recipient by typing it. The user has just created or opened a public link and wants to mail it. On the link screen they tap "Mail link". A confirmation says that the link will be passed to PB Office as a new mail draft, that a password (if the link has one) is not included and must be told to the recipient separately, and that the draft will later be saved in the Drafts folder on the mail server. The confirmation defaults to Cancel. When the user confirms, PB Cloud passes the link once to PB Office, which opens a new message that already contains the link and a subject such as "Link to <item name>". The user chooses the recipient, reviews the message and sends it in PB Office; PB Cloud sends nothing itself.

**Why this priority**: This is the whole feature: it replaces typing a long link by hand with a reviewed mail.

**Independent Test**: Open the link screen of a link with a known text in the host build with a stand-in for PB Office; tap Mail link and confirm; verify that exactly one hand-off was made, that its text equals the link and its subject names the item, that it holds no password and no recipient, and that Cancel makes no hand-off. On the reader (PB Office installed, not running): the new message opens with the link and subject.

**Acceptance Scenarios**:

1. **Given** the link screen of a public link, **When** the user looks at the bottom panel, **Then** a "Mail link" tool is available next to the existing tools.
2. **Given** the link screen, **When** the user taps Mail link, **Then** a confirmation appears that names PB Office and a new draft as the destination, states that a password is not included when the link has one, says that the draft is saved to the Drafts folder on the mail server later, and has Cancel as its default.
3. **Given** the confirmation, **When** the user confirms, **Then** PB Cloud makes exactly one hand-off to PB Office whose text is the complete link and whose subject is one line of at most 300 characters that names the item, with no recipient.
4. **Given** the confirmation, **When** the user cancels (or leaves the screen), **Then** no hand-off is made and the link screen stays as it was.
5. **Given** PB Office is installed and not running, **When** the user confirms, **Then** PB Office opens a new message containing the link and the subject; the number of taps from the link screen to that message is two (the tool, the confirmation).
6. **Given** the hand-off was accepted, **When** the user returns to PB Cloud, **Then** the link screen is gone (the existing rule of feature 004: the link is dropped when the app is switched away) and the link is shown again only after the user opens that link again.

---

### User Story 2 - A hand-off that fails is explained, not repeated (Priority: P1)

PB Office may not be installed, or may be running already; observed on the reader, a running PB Office does not take the text and shows its own screen. In these cases the user must not be left guessing and nothing must be sent twice. PB Cloud checks the answer to its single hand-off. If PB Office did not accept it, PB Cloud shows a short, calm message the next time its screen is shown ("PB Office could not take the link. Close PB Office and try again."). It does not retry by itself; the user opens the link again and repeats the action. The case with PB Office not running is the supported one.

**Why this priority**: A silent failure would look like a lost link, and a retry that the user did not ask for could create a duplicate draft.

**Independent Test**: With the stand-in for PB Office answering "not accepted" (and, separately, "not installed"), confirm Mail link; verify one call only, the message the next time the screen is shown, no second call, and that the message contains no link or item name.

**Acceptance Scenarios**:

1. **Given** the hand-off is not accepted (any answer other than success), **When** PB Cloud's screen is shown again, **Then** the short message says that PB Office could not take the link and what to try, and it contains no link, subject or item name.
2. **Given** the hand-off was not accepted, **When** the message is shown, **Then** no second hand-off has been made, and none is made until the user confirms again.
3. **Given** PB Office is not installed, **When** the user confirms, **Then** the same kind of message appears and nothing else happens.
4. **Given** the text would exceed the size PB Office takes (8192 bytes), **When** the user taps Mail link, **Then** nothing is handed over and the user is told that the link is too long to mail.

---

### User Story 3 - The link and password stay private (Priority: P1)

A share link can be a key to private files. The hand-off must not weaken the rules of feature 004: the link appears as plain text only on the screen the user opened, is never opened by the app, never written to a log, never stored, never put on a clipboard, and a password is never stored on the reader after it was sent, and is never handed over. The one new thing that leaves PB Cloud is the link text in the single hand-off the user confirmed; after it, buffers that held the text are wiped. The mechanical constitution checks cover the new source file.

**Why this priority**: The rules are constitutional and the main risk of the feature; a leak cannot be undone.

**Independent Test**: Run the hand-off in the host build with a known link text and a known password while capturing everything the app writes; search for both; verify the password appears nowhere (including the hand-off arguments), the link appears only in the hand-off arguments, and the constitution checks (including the extended one) pass.

**Acceptance Scenarios**:

1. **Given** a link with a password, **When** the user mails the link, **Then** the hand-off arguments contain the link and subject only; the password is not in them and is not stored anywhere on the reader.
2. **Given** a hand-off, accepted or not, **When** the app's logs, database, settings, cache and exported files are inspected, **Then** they contain no link text, token, password or item name.
3. **Given** the hand-off call returned, **When** the memory PB Cloud controls is inspected, **Then** the buffers that held the link text are overwritten and no copy of it is kept.
4. **Given** the source files that name PB Office's launcher, **When** the constitution checks run, **Then** only the files allowed by the constitution contain it, and one call site makes the hand-off.
5. **Given** the hand-off, **When** the network is observed, **Then** PB Cloud makes no request because of it; the link is not opened or fetched by PB Cloud.

---

### Edge Cases

- The link is flagged as unreadable or "different server" (feature 004 FR-029): the Mail link tool is not offered for it.
- The share has a password: the confirmation says it is not included; the mail contains the link only.
- The item name contains control characters, line breaks or is very long: the subject is cleaned to one line, shortened to at most 300 characters, and never ends in a half character.
- The reader is offline: nothing is needed from the network, so Mail link works offline; PB Office syncs its draft later by itself.
- The user taps Mail link twice quickly, or confirms twice: only one hand-off is made.
- PB Office is running (supported only when it is not): see Story 2; the user closes PB Office and repeats.
- The user returns from PB Office with Home or Back: PB Cloud's behaviour is the feature 004 behaviour (the link screen is gone after being switched away).
- A hand-off is confirmed while a sharing request is still in flight: the tool is unavailable while a request runs (the same one-request rule as feature 004).

## Requirements *(mandatory)*

### Functional Requirements

**The tool and the confirmation**

- **FR-001**: The link screen of a public link MUST offer a Mail link tool when the link is complete and readable (FR-020), and MUST NOT offer it for a link flagged as unreadable or as belonging to a different server. A link that is merely flagged as not secure (not https) is still offered; the screen already warns about it.
- **FR-002**: Tapping the tool MUST open a confirmation that names PB Office and a new draft as the destination, states that a password, if the link has one, is not included and must be given to the recipient separately, states that the draft is later saved to the Drafts folder on the mail server, and has Cancel as its default.
- **FR-003**: The system MUST make no hand-off before the user confirms, and MUST make none when the user cancels or leaves the screen.
- **FR-004**: The path from the link screen to the PB Office editor MUST take at most two taps (the tool, the confirmation).
- **FR-005**: The tool MUST be unavailable while a sharing request is running, and a repeated tap or confirmation MUST NOT cause a second hand-off.

**The hand-off**

- **FR-006**: On confirmation the system MUST make exactly one hand-off to the installed PB Office, using the interface that PB Office publishes for this purpose (the sibling project's contract "handoff-contract v1"), carrying the complete link as the text and a subject, and no recipient.
- **FR-007**: The subject MUST be one line of at most 300 characters that names the shared item (for example "Link to <item name>"), with control characters and line breaks removed and long names shortened at a character boundary.
- **FR-008**: The text handed over MUST be exactly the complete link and nothing else (no introductory line, no item name in the body); the user adds their own words in PB Office.
- **FR-009**: The system MUST NOT hand over a text longer than 8192 bytes, and MUST tell the user that the link is too long to mail.
- **FR-010**: The system MUST check the answer to the hand-off. Only the answer "accepted" counts as success; every other answer, and a missing PB Office, MUST be treated as not accepted.
- **FR-011**: When the hand-off is not accepted, the system MUST show a short, calm message the next time its screen is shown, naming no link, subject or item, MUST NOT retry by itself, and MUST NOT re-show a link that was already dropped.
- **FR-012**: The system MUST NOT start PB Office or any other program for any other purpose, MUST NOT read or write PB Office's data, and MUST NOT hand over anything but the text and the subject (no recipient, no attachment, no password).

**Privacy and safety**

- **FR-013**: The system MUST NOT hand over, store, log or display a password in connection with Mail link; a password stays as feature 004 defines (sent once, then discarded).
- **FR-014**: The system MUST NOT write the link, the subject, the item name or the hand-off arguments to any log, diagnostic export, stored error record or file, MUST NOT place them on a clipboard, and MUST NOT store the link anywhere (feature 004 FR-026 and FR-027).
- **FR-015**: The system MUST overwrite the buffers it controls that held the link text and the arguments immediately after the hand-off call returns, whether it was accepted or not.
- **FR-016**: The system MUST end the display of the link when the app is switched away, as feature 004 FR-028 requires; after an accepted hand-off the link screen therefore no longer shows the link.
- **FR-017**: The system MUST NOT open, follow or fetch the link because of this feature, and MUST NOT perform any network activity because of it (constitution Principle VI).
- **FR-018**: The hand-off MUST be a narrow, tested exception to the rule that the application starts no other program: one call site, to PB Office's launcher only, only after a confirmed tap; the mechanical constitution checks MUST be extended so that the launcher's name and the call occur in no other source file, and the extension MUST be self-tested.

**Interface**

- **FR-019**: The tool and the confirmation MUST use the existing component set and behave like the other tools and dialogs of the app (icon plus label in the bottom panel, dialog with footer buttons, no animation).
- **FR-020**: The tool MUST always be shown for a readable link, whether or not PB Office is installed; when PB Office is missing, the failure message of FR-011 appears and nothing else happens. The system MUST NOT inspect the reader's applications to decide whether to show the tool.
- **FR-021**: All new texts MUST be added to the translation sources in English and German like the existing texts, and the message in FR-011 MUST be one of them.

### Key Entities

- **Mail hand-off**: one request to PB Office made on the user's confirmation. It has a text (the link), a subject (the item name, one line) and an outcome (accepted or not accepted). It exists only while the call runs; nothing about it is stored.
- **Link screen**: the existing detail screen of a public link (feature 004), which owns the link text while it is shown.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: With PB Office installed and not running, a user can get a new mail message containing a share link from the link screen in two taps and under 10 seconds from the second tap on the reader.
- **SC-002**: In 100% of the automated runs, exactly one hand-off follows a confirmation and none follows Cancel, leaving the screen or a repeated tap.
- **SC-003**: In 100% of the automated runs, the password does not occur in the hand-off, the logs, the database, the settings, the cache or any file; the link occurs only in the hand-off.
- **SC-004**: In 100% of the runs where PB Office does not accept the hand-off, the user sees the message on the next screen and no second hand-off occurs.
- **SC-005**: The mechanical constitution checks, including the extended rule with its self-test, and the full pre-merge gates pass with the feature included.
- **SC-006**: On the reader, after a hand-off and after a failure, a search of PB Cloud's logs and files for the link text finds nothing.

## Assumptions

- PB Office 0.4.0 or later is installed on the reader for the feature to succeed, in the same applications folder as PB Cloud; no change to PB Office is needed or planned.
- The interface of PB Office is its "handoff-contract v1" (text, text length, subject, recipient); this feature uses text, text length and subject only.
- Observed on the reader (InkPad One, 2026-10-23/24): a hand-off to PB Office that is not running is accepted and opens a draft; leaving PB Office with Back shows PB Office's main screen and Home returns to PB Cloud; a hand-off to a running PB Office is not accepted and PB Office shows its own screen.
- The reader has no system clipboard, so the feature does not add a Copy action.
- The mail draft is saved by PB Office and synced by it; its handling of the text after the hand-off is PB Office's responsibility.
- The link is far shorter than 8192 bytes in practice; the size guard exists for hostile or unexpected server answers.
- Out of scope: other mail apps, recipients, attachments, mailing the password, a general share menu, a clipboard Copy, handing over to a running PB Office, and any change to PB Office.
