# Research: Share Links

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md)

Decisions for the open questions of the plan. Every statement about what a Nextcloud server does is **unverified** (constitution IX)
until `docs/live-server-checklist.md` LV-L1 ... is run; the Nextcloud code and its client apps are reference-only (constitution VII),
the behavior below is reimplemented from the public OCS Share API documentation and from behavior-only descriptions.

Notation: **Decision** / **Rationale** / **Alternatives considered**.

---

## R-1 Which server interface carries the feature

**Decision**: the OCS Share API of the Files Sharing app, version 1 of its resource tree, called through the `/ocs/v2.php/` prefix
(`/ocs/v2.php/apps/files_sharing/api/v1/shares`), JSON answers (`?format=json`), header `OCS-APIRequest: true`, the same Basic
credentials as every other request. Capabilities come from the call the account check already makes
(`/ocs/v1.php/cloud/capabilities?format=json`, key `files_sharing`).

**Rationale**: it is the only documented way to create, list, change and remove shares; WebDAV carries no share operations. The `v2.php`
prefix mirrors the OCS status in the HTTP status, which keeps the status mapping of `DavSession::status_error` usable; the answer is
also read when a server (or a proxy) answers `200` with a failing `meta.statuscode` (the `v1.php` behavior).

**Alternatives considered**: WebDAV `PROPFIND` of `oc:share-types` (shows only that something is shared, no details); the older
`v1.php` prefix (always HTTP 200, a second place where failures hide).

## R-2 The requests, in the smallest set

**Decision**: six requests, all in `core/src/share_api.cpp`
([contracts/share-requests.md](contracts/share-requests.md)):

| # | Request | Used for |
|---|---------|----------|
| L1 | `GET .../cloud/capabilities` | the server's sharing rules (R-3) |
| L2 | `GET .../shares?path=<p>&reshares=true` | the shares of the item |
| L3 | `GET .../shares/inherited?path=<p>` | shares that reach the item from a parent folder (optional; a `404` means none) |
| L4 | `GET .../shares/<id>` | the detail of one share (the only request whose answer is allowed to hold a link) and the pre-check before a change |
| L5 | `POST .../shares` and `PUT .../shares/<id>` | create / change a public link (form-encoded body) |
| L6 | `DELETE .../shares/<id>` | remove a link |

Create sends `shareType=3` (public link), `path`, `permissions` and, only when chosen, `password` and `expireDate` (`YYYY-MM-DD`).
Permissions are the Nextcloud bit values: read `1`; for a folder, read-and-upload is `15` (read, update, create, delete); a file link
is always `1`. The app never sends `publicUpload`, `label`, `note`, `sendPasswordByTalk`, `hideDownload` or any other field.
Change sends only the field(s) the user changed; removing the password sends `password=` (empty), removing the expiry `expireDate=`.

**Rationale**: one verb per user action; nothing is sent that the spec does not ask for (no share-by-mail, notes, Talk).

**Alternatives considered**: `publicUpload=true` (deprecated spelling of the same permission bits); fetching the share list with
`subfiles=true` (lists all shares inside a folder, unbounded, not asked for).

## R-3 Sharing rules: fetched when Share opens, never stored

**Decision**: opening the Share screen runs L1 and L2 (and L3) in that order on the worker; the result is held in memory by the bridge
for the life of the screen and dropped when the screen is left. **No schema change** (the database stays at schema 5, no migration).
`ShareRules` ([data-model.md](data-model.md)) has an `unknown` state for every field; unknown is treated as "allowed" for what the
controls offer (the server's refusal then explains, R-4) but **never** as "required" or as a limit.

Rules read: `files_sharing.api_enabled`, `public.enabled`, `public.password.enforced` and, where present,
`public.password.enforced_for.{read_only,read_write,upload_only}`, `public.expire_date.{enabled,days,enforced}` (the same object also
carries the internal/remote variants, which are ignored), `public.upload`, `public.upload_files_drop`, `resharing`. JSON is parsed with
`json_min` and the bounds of `server_caps.h` (64 entries, 32 bytes each).

**Rationale**: the spec forbids keeping sharing state beyond the screen (FR-011, FR-027, Assumptions); rules can change on the
server at any time and one small request per visit is cheaper than a stale rule that blocks the user. The existing `server_caps` table
(feature 002) is left alone: sharing rules are not part of what 002 needs at start.

**Alternatives considered**: reusing the account check's stored capabilities (they predate the sharing keys and would go stale; stored
state would need a migration); asking the rules only when a refusal happens (the user would type a password to learn it is required).

## R-4 Messages come from rules and status, never from the server's text

**Decision**: a refusal is explained by `diagnose_refusal(rules, request, answer)` ([contracts/core-interfaces.md](contracts/core-interfaces.md)):

1. the app validates **before** sending, from the rules it knows (password missing while enforced, expiry missing while enforced, a
   date beyond `days`, upload permission while `public.upload` is false, public links disabled);
2. when the server still refuses (HTTP 400/403/404 or an OCS `statuscode` of that class), the diagnosis **re-reads L1** once and checks
   the same list against the fresh rules; the first rule that the request violates gives the key
   (`share_public_disabled`, `share_password_required`, `share_expiry_required`, `share_expiry_too_far`, `share_upload_not_allowed`);
3. an item that cannot be shared (the server answers 403/404 for the path while the rules allow links) first gets **one** `PROPFIND` with `Depth: 0` on the item (the read request of feature 001; no new request kind): a `404` gives `share_item_gone` (the item was renamed, moved or deleted since the listing: "Refresh the folder"), anything else gives `share_item_not_shareable`;
   a change of a share the user does not own gives `share_not_changeable`; a `404` on the share id gives `share_gone`;
4. anything else gives `share_refused` with a short code (`E` + status, for example `E403`), no server text.

`meta.message` and any body text are **never** shown, logged or matched: they are localized and free-form, so matching them would be
brittle and displaying them would break FR-023.

**Rationale**: testable on the fake server by switching rules; independent of the server's language; no server text can leak a name or
a link into a message.

**Alternatives considered**: matching the English message text (breaks with another server language and on every wording change).

## R-5 The list never carries a link

**Decision**: two types ([data-model.md](data-model.md)): `ShareRow` (kind, id, permission, expiry, has_password, label, display name,
owned_by_me, inherited) produced by `parse_share_list`, and `ShareDetail` (a `ShareRow` plus the link text) produced **only** by
`parse_share_detail` for L4/L5 answers. The list parser has no code path that reads the answer's `url` or `token` members; a test
(`test_share_parse`) feeds lists in which every row has a recognizable link and asserts that the string occurs nowhere in the result
(FR-014, FR-027).

`has_password` is computed from the answer (a non-empty `password` member, or for a link a non-empty `share_with`, which some versions
fill with a hash) and the value itself is not copied out of the parse function. Kinds: `0` user, `1` group, `3` public link; everything
else (mail `4`, circles, rooms, federated, deck ...) is `Other` and carries no further field.

**Rationale**: a type that cannot hold a link cannot leak one; the guarantee is checked by the compiler and by one test.

**Alternatives considered**: one type with a link member that the bridge clears (a forgotten clear is a leak).

## R-6 The link detail is fetched when the user taps the link

**Decision**: tapping a link row runs L4 and shows the answer's link on `ShareLinkPage`. The text is held in a `QString` of the bridge
until `clearSecrets()` (leaving the page, account change, application inactive), then overwritten (`QString::fill`) and released.
Reopening always asks again (FR-027). The link is checked before it is shown: `https://` required; host equal to the connected
server's host else flagged **"different address"** (a reverse proxy or a configured public address is legitimate, so the text is shown
with a warning line, not hidden); at most 512 bytes; no control characters, blanks or non-ASCII (else `Unreadable`, text not shown).
The app never opens, resolves or requests it (R-9 rule N9).

**Rationale**: spec Story 3 and FR-012/013/029. A mismatch is information for the user, not a reason to hide what the server reported.

**Alternatives considered**: hiding links with a foreign host (the user could not copy their own legitimate link).

## R-7 Changing a link: check, then change, with the race stated

**Decision**: a change (password, expiry, permission) is `L4` (re-read the share), compare with the **snapshot** the screen holds
(kind, permission bits, expiry, has_password), and only if equal send `PUT`. If different: nothing is sent, the screen shows the
current state and says "This link was changed meanwhile" (FR-017); the user confirms again. The `PUT` answer (the share as stored) is
the state shown afterwards. OCS shares have no ETag, so no `If-Match` exists; the remaining race is the time between L4 and `PUT`
(seconds) and costs at most a lost concurrent edit of a share setting, never user data. Recorded as a deviation row in the plan.

**Rationale**: the strictest check the interface offers; shares are settings, not files (constitution III speaks of stored file data).

**Alternatives considered**: a blind `PUT` (loses concurrent edits silently); refusing to change links (removes Story 4).

## R-8 Lost answers: never repeat, say unknown

**Decision**: create, change and remove are each **one request**; the app never repeats one by itself (FR-020). When the transport
reports `delivery` of `FullySent` or `PartlySent` and no answer (timeout, dropped connection, 5xx other than 503), the outcome is
**unknown**: the screen says so (`share_unknown_outcome`), the user's entries are kept (password excepted, R-10), and a **Refresh**
action re-reads the list so that the user can see whether the link exists. A repeated Create is a new deliberate action and may create
a second link; the unknown message says so. `NotSent` is a plain failure (nothing happened). Nothing is persisted: a kill during the
request leaves no record and the next visit simply lists what the server has.

**Rationale**: the same stance as feature 002's unknown outcome, without a queue because sharing needs a live connection (FR-006).

**Alternatives considered**: persisting the intent for a later check (stores a request that may contain a password; spec FR-006 forbids
queuing).

## R-9 Privacy: where a secret may exist, and the mechanical rules

**Decision** ([data-model.md](data-model.md) "Secret lifecycle"): a **password** exists in the on-screen field, one `std::string` in
the worker's request (wiped with `secure_wipe` right after `perform`, also on failure and on exceptions via a guard object), and in
the transport's request body for the duration of the call. It is never written to the database, settings, a file, the log, an error
record or a message. A **link** exists in the answer buffer, in `ShareDetail`, and in the bridge's `linkText` until `clearSecrets()`.
Best effort only for copies the runtime makes (a `std::string` may have reallocated, a QML/JavaScript string cannot be wiped): the
page sets its password field to `""` as soon as the request is handed over; the guarantee the tests check is **persistence and
logging**, not RAM contents (stated honestly in plan and README).

New mechanical rules in `scripts/check-constitution.sh`, each with a self-test:

* **N8** - in `core/src/share_*.cpp`, `core/include/pbc/share_*.h` and `bridge/*/share_bridge.*`: no `log::text`, `log::path`, `qDebug`,
  `qWarning`, `qInfo`, `std::cerr`, `printf` family, and every `log::event` code starts with `share.` and takes only `log::status` and
  `log::count` fields (extends N4/R6/R8, which already forbid field names such as `link`, `token`, `url`, `password`).
* **N9** - the link is never opened: no `QDesktopServices`, `QProcess`, `system(`, `popen(`, `xdg-open`, `Qt.openUrlExternally`,
  `Qt.openUrl` and no `Qt.labs.platform`/`QUrl::fromUserInput` in `core/`, `bridge/`, `app/`, `qml/`. A QML `Text` that would turn a
  link into a clickable one is forbidden by using `textFormat: Text.PlainText` on the link display (`check-ui-style.sh` rule).
* The test `test_share_privacy` runs create/show/change/remove with a known password and link against the fake server while
  capturing the log sink, listing the data directory, dumping the database, and searching all of it for the password, the token and
  the link; it also asserts that the fake saw **no** request to the link's address.
* The log codes added to `core/src/log_codes.h` (the list is policed by `test_log`): `share.open`, `share.create`, `share.change`,
  `share.remove`, `share.fail`.

**Rationale**: Principle IV says logs must contain no share links or tokens, and a rule that can be checked by script must be.

**Alternatives considered**: relying on review (misses a later `log::text("link", ...)`); zeroing QML strings (not possible).

## R-10 Password entry: plain, never copyable, no suggestions

**Decision**: the password field is shown as plain text (spec FR-015 and Story 7: plain only on the screen asked for) in a
`TextEntryDialog` style entry with `inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase`, so the
on-screen keyboard does not learn or suggest it. The shared `TextMenu` hides Cut and Copy only for fields whose `echoMode` is not
Normal; a plain password field would offer Copy, so `TextMenu` gets one small additive property `forceSecret` (default false) that
the share password field sets. The entry is cleared when the dialog closes or the request is handed over. The app generates no
password and offers no "suggest" button (out of scope).

**Rationale**: closes the one path by which a typed secret could reach the clipboard; the change to the shared component is one line
and keeps every other field as it is.

**Alternatives considered**: a masked field with a "show" toggle (the spec says the user must be able to read what they type; the toggle
doubles the taps on an e-ink keyboard).

## R-11 Copy and the clipboard (spec clarification 2026-10-22)

**Decision**: the link is **always** shown legibly (large, `Text.PlainText`, wrapped at any character so no part is cut off, length
hint "N characters"). A **Copy** tool exists only when `clipboardAvailable` is true. `clipboardAvailable` is a constant set in
`app/main.cpp`: **true on the host build** (so the tests and the demo exercise it) and, for the device build, **false until DT-L2 has
shown that text copied in this app can be pasted into another app** (CMake option `PBC_DEVICE_CLIPBOARD`, default OFF). **Result 2026-10-23 (DT-L2): it cannot; the reader has no system clipboard, so the device stays OFF.** Evidence for
the default: `qml/TextMenu.qml` records that on the reader "the clipboard is the app's own, text copied in another app is not
reachable" (inbound); the outbound direction is unmeasured. Copy writes the exact link text through one injected setter
(`ShareBridge::Config::set_clipboard`) and the page says "Copied. Other apps can read it until you copy something else." Leaving the
link page clears the clipboard **only if it still holds exactly the link** (no clipboard read other than that comparison).

**Rationale**: honors "where the reader has a system clipboard" without claiming a device behavior nobody measured (constitution IX);
the shipped device default is the safe one (no Copy), and enabling it is a one-line build change after DT-L2.

**Alternatives considered**: probing at run time (an app cannot observe another app's paste); never clearing the clipboard (leaves the
link readable after the user left the screen).

## R-12 Expiry date: typed as an ISO date, checked against the server's day

**Decision**: the date is typed as `YYYY-MM-DD` (hint text in the field, German `JJJJ-MM-TT` in `strings_de.tsv`) into a
`TextEntryDialog`; the shown date elsewhere uses the existing date formatter of the reader's language. `share_date.h` validates the
form, the calendar (`civil::valid_date`), and the range: not before **today** and not after today + the server's `days`. "Today" is
the **server's day** from `DavSession::last_server_time()` (the HTTP `Date` header, as feature 001's clock check) when known,
otherwise the reader's clock; in either case the server's own refusal is authoritative and the date the server reports back is what
is shown (spec edge case on time zones; the reader's clock is never used to say that a link is expired).

**Rationale**: no new date-picker component (constitution XI/V); one validated format has no locale ambiguity.

**Alternatives considered**: a calendar widget (a new component with many taps on e-ink); a day-count field ("7 days") (cannot express
a fixed date and invites off-by-one against the server's day).

## R-13 Where the user starts: select mode of the folder page, and the details page of a file

**Decision**: a **Share** tool is added to the bottom panel of `FolderPage` in **select mode**, enabled when exactly one entry (a file or a
folder) is selected, and to `DetailsPage` for a file (a folder row opens the folder, so a folder has no details page). The spec's wording
is the same: "they select the item in the folder listing and choose Share". Select mode then shows six tools (Rename, Share, Move, Copy,
Delete, Done); `BottomPanel` shares the width evenly among the visible tools (no layout change), and DT-L1 checks the touch size on the
reader. The icon `share` is drawn in the project's style by `scripts/make-icons.sh`. The tool is disabled with the reason for an unavailable
entry (no permission, encrypted folder, unsupported share state, no re-share right) and, when offline, still opens the screen that says
sharing needs a connection (FR-006, Story 1 scenario 6).

**Rationale**: the folder page already carries the per-item actions of feature 002 in select mode; it is the only place from which a folder
can be chosen at all.

**Alternatives considered**: a Share tool in the normal panel of the folder page for the current folder (the panel is full since feature 002
and it would not cover files); a long-press menu (a new interaction the sibling apps do not have).

## R-14 Bounds

**Decision**: every response is read with `max_response_bytes` of **2 MiB** and `JsonLimits{2 MiB, depth 16, nodes 40000}`; at most
**500 rows** are kept per list (beyond that the screen says "More shares exist than can be listed here" and `share_list_too_big`);
labels and display names are sanitized with `utf8.h` (control characters removed, at most 64 bytes, never trusted as a path); one
sharing request runs at a time (the bridge refuses a second action with `busy`); connect timeout 10 s, total timeout 30 s per request;
no retry.

**Rationale**: constitution V (bounded footprint) and the hostile-input duty of principle I. The OCS list has no paging for a path, so
the *request* is one bounded read and the *display* is paged (spec Edge Case "fetched in bounded pieces" is met by the bound and the
message; recorded as an owner point in the plan).

**Alternatives considered**: following `Link` headers (not offered by this endpoint).

## R-15 Account change and sleep end the secrets

**Decision**: `ShareBridge.clearSecrets()` is called by the pages' `Component.onDestruction`/Back, by `ConnectionBridge`'s
account-changed signal, and from `QGuiApplication::applicationStateChanged` when the state leaves `ApplicationActive`. The link text
and the form fields vanish; the page re-fetches on return. No timer is added (rule N3 unchanged). Whether the reader reports an
inactive state when it goes to sleep is **unverified** (DT-L3); if it does not, the link stays on the screen until the user leaves,
which is what the screen shows anyway.

**Rationale**: FR-028 within what Qt reports; a timer for idle-hiding would add a background element for little benefit.

**Alternatives considered**: an idle timer (new timer, rule N3 allow-list entry, more battery).

## R-16 Testing strategy (constitution I)

**Decision**: failing-first tests at every boundary:

* **Parsers**: `test_share_parse` (lists, details, hostile: huge, truncated, wrong types, duplicate keys, control characters, a link on
  another host, 600 rows, unknown kinds, a share with a hash in `share_with`), `test_share_rules` (capability shapes old and new, missing
  keys, contradictory keys), `test_share_date`, `test_share_diagnose` (every rule violation gives its key; unknown gives `E` code).
* **Boundary against the fake Nextcloud** (extended with a shares side, rules switches and failure injection): `test_share_list`,
  `test_share_create`, `test_share_change` (including "changed meanwhile"), `test_share_remove` (including already gone),
  `test_share_refusals` (six server configurations of Story 6), `test_share_unknown_outcome`, `test_share_privacy`,
  `test_share_requests` (exact method, path, headers and form body of every request; password never in a URL; no request before the
  confirmation), `test_no_background_network` (extended: nothing from sharing without a tap), real-libcurl `test_share_curl` (POST/PUT/DELETE
  with form bodies through the TLS test server).
* **Bridge/QML**: `test_share_bridge` (Qt Test, offscreen), the QML self-test for the three pages, host walk `18-share-links.walk`.
* **Budgets**: `test_budget_share` (200 shares: parse time, peak memory; unverified on the device).

Server-imitating fixtures live under `tests/fixtures/nextcloud/shares/` with the `.unverified` suffix.

## R-17 Documents that change

**Decision**: README (what the app can do; credential and secret handling paragraph: links and passwords on screen only, the
clipboard caveat), About box text (sharing sentence, no new component), `docs/live-server-checklist.md` (LV-L1 ... LV-L10),
`docs/device-test-checklist.md` (DT-L1 ... DT-L5), `docs/device-findings.md` when measured, `CHANGELOG.md` and `VERSION` to **0.4.0**
(constitution workflow: MINOR for a feature; the bump is the last phase of tasks), `NOTICE`/`licenses/manifest.txt` unchanged (no new
component), `qml/strings_de.tsv` for every new string. The statement of feature 001/002 that the app "cannot share anything" is
replaced by the scope of FR-033 wherever it appears in user-facing text.

## R-18 No new dependency, no schema change

**Decision**: no library is added; `share_*` modules use `json_min`, `xml_min` (not needed), `DavSession`, `remote_path::encode_path`,
`utf8`, `civil`, `secure_wipe`, `log`. The database stays at **schema 5**; an older build opening a database touched by this build
is unaffected because nothing is written. Form encoding (`application/x-www-form-urlencoded`) is a 20-line function in
`share_api.cpp` with a test, because the existing code builds JSON/XML/query strings only for GET.

**Rationale**: constitution V; a pure additive module.

**Alternatives considered**: libcurl's `CURLOPT_POSTFIELDS` encoding helpers (the transport seam carries a body string, so encoding
belongs above it and stays testable on the host).

## R-19 Performance and footprint budgets

**Decision**: see [plan.md](plan.md) Technical Context; the figures restate the spec's success criteria and are **unverified** until
measured on the device (DT-L4): first page of a 200-share list within 5 s after the tap on Wi-Fi (network time excluded in the host
test: parse + model build <= 100 ms host, <= 500 ms device); peak RAM unchanged <= 150 MB (`VmHWM`), share state <= 1 MB; package
growth < 150 KB; cold start unchanged (no share code runs at start).

## R-20 Open facts that need a real server (unverified)

LV-L1 shape of `files_sharing` capabilities across versions; LV-L2 permission bit values for folder links (15) and file links (1/3);
LV-L3 where the password state appears in a link answer (`password` vs `share_with`); LV-L4 `shares/inherited` availability;
LV-L5 `reshares=true` listing of other users' shares; LV-L6 what each enforcement (password, expiry, upload) answers (status,
`statuscode`) so that R-4's re-read finds the rule; LV-L7 clearing a password and an expiry with empty form values; LV-L8 a `DELETE`
of an unknown id; LV-L9 link host versus connected host behind a proxy; LV-L10 form-encoded bodies accepted on both prefixes.

## Implementation notes (2026-10-22)

Changes to this research, found while building (details in `tasks.md` and `acceptance.md`):

- **Entry point.** A folder has no details page, so **Share** is a tool in *select mode* of `FolderPage` (six tools) and on `DetailsPage` for a file (R-13, rewritten).
- **API shape.** `ShareApi::remove` is `remove_link` (rule N5 reserves the name `remove`); `create` and `change` take the rules the screen holds (no hidden capabilities
  read before a request); `change` also takes the `Snapshot` (which now holds `owned_by_me`); `detail(id, item_is_folder)`; `ShareOp` has a fifth value, `Detail`;
  `RefusalContext` carries the diagnosis input; `DavSession::send` got two additive parameters (`total_timeout_s`, `secret_body`).
- **Bridge.** `ShareBridge::message` carries `{key, field, fieldMask, limitDays, code}`; the texts come from `qml/Problems.qml` (`title`/`action` per key) and the page adds
  the limit in days and the short code. Results of the actions arrive as signals (`createFinished`, `changeFinished`, `removeFinished`, `linkOpened`); `leaveLink`,
  `clearMessage`, `dateText` and the detail keys `passwordValue`, `expiryValue`, `hasPassword`, `expires`, `permission` were added.
- **Link page.** The password, expiry and permission are changed by tapping their rows (not by bottom-panel tools); Copy and Remove link are the tools.
- **Walks.** `19-share-limits.walk` exists because the server's rules can only be set before a walk starts.
- **Order.** `ShareBridge` is declared before `SyncBridge` in `app/main.cpp` (destruction order, see T042 of `tasks.md`).
- **Fake server.** Control characters in JSON strings are now escaped (`quote_json`), as a real server does; the shares side has `set_share_password/permissions/expiration`.

