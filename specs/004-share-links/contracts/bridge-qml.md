# Contract: Bridge and QML (`bridge/`, `qml/`)

**Feature**: [../spec.md](../spec.md) | **Core contract**: [core-interfaces.md](core-interfaces.md) | **003**: [bridge-qml](../../003-folder-sync/contracts/bridge-qml.md) | **002**: [bridge-qml](../../002-manage-server-files/contracts/bridge-qml.md) | **001**: [bridge-qml](../../001-nextcloud-files-client/contracts/bridge-qml.md)

Rules of 001-003 stay: the bridge is the only Qt layer; all network work runs on the one `Worker` and reports back with
`QMetaObject::invokeMethod(..., Qt::QueuedConnection)`; properties are read by the UI thread only; names shown pass `display_safe()`
first; no bridge starts a request on its own. New in this feature: **a secret (a link, a password) crosses the bridge**; the rules for
it are at the end of this file. **No timer is added** (rule N3 unchanged).

## ShareBridge (context property `share`; `bridge/include/share_bridge.h`)

| Member | Type | Meaning |
|--------|------|---------|
| `open(remotePath)` | Q_INVOKABLE | the Share tap in select mode of the folder page or on the details page of a file: `ShareTarget` from the stored 001 entry of that path, then L1+L2+L3 on the worker; `offline` when the reader has no Wi-Fi (nothing is sent); `busy` if a request is running. Replaces the held screen state |
| `refresh()` | Q_INVOKABLE | re-reads L1+L2+L3 (the Refresh tool and after every change) |
| `state` | string | `closed`, `loading`, `listed`, `failed`, `working` |
| `itemName`, `isFolder`, `itemShareable`, `unavailableReason` | | the item for the heading; `itemName` through `display_safe()` |
| `offline` | bool | no Wi-Fi now (the screen says sharing needs a connection and offers Retry on a tap only) |
| `busy` | bool | a sharing request is running; a second action answers `{errorKey: "busy"}` and is not queued |
| `rules` | map | `{sharingOffered, linkCreationOffered, passwordRequired, passwordRequiredUpload, expiryRequired, expiryMaxDays, uploadOffered, known}`; `known` is false for any value the server did not say; the controls use it (FR-022) |
| `rows` | list of maps | the shown page: `{id, kind, kindText, permissionText, expiryText, passwordText, labelText, withName, ownedByMe, inherited, limited, canOpen}`; **never a link or token** |
| `page`, `pageCount`, `pageSize`, `total`, `truncated` | | page buttons as the other lists; `truncated` shows the "more shares exist" line |
| `hasExistingLink` | bool | true when the listed rows contain a public link; the create form then shows the notice that this adds another link |
| `staleNote` | bool | the list may be out of date (unknown outcome); cleared by `refresh()` |
| `inheritedNote` | string | `""` or `inherited_failed` |
| `message` | map | `{key, text, hint, field, limitDays, code}` of the last refusal or validation failure; empty when none; contains no secret |
| `showLink(id)` | Q_INVOKABLE | the tap on a link row: L4; fills `detail` |
| `detail` | map | `{id, permissionText, expiryText, passwordText, canChange, linkText, linkState, linkLength}` while a link page is open, else `{}`; **`linkText` is the only property that carries a link** |
| `create(map)` | Q_INVOKABLE -> map | `{permission, password, expiry, confirmed}`: without `confirmed` returns `{needsConfirm: true, summary}` (what the link allows, FR-003) and sends nothing; with it validates (`validate_create`) and sends L5; returns `{ok}` or `{errorKey, field}`; on success `detail` is set and the list re-read |
| `change(id, map)` | Q_INVOKABLE -> map | `{action, password|expiry|permission, confirmed}`: without `confirmed` returns `{needsConfirm, effectText}`; with it L4 + compare + L5 `PUT`; `{errorKey: "share_changed_meanwhile", current}` when the link changed; the stated state is the server's |
| `remove(id, confirmed)` | Q_INVOKABLE -> map | without `confirmed`: `{needsConfirm, effectText}`; with it L6; `share_gone` is reported as "already removed" and the list re-read |
| `clipboardAvailable` | bool | constant from `app/main.cpp` (R-11): true on the host, device default false until DT-L2 |
| `copyLink()` | Q_INVOKABLE -> bool | puts exactly `detail.linkText` through `Config::set_clipboard`; false if not available or no link is open |
| `clearSecrets()` | Q_INVOKABLE | drops `detail`, the held `ShareDetail`, the form's values, rules and rows; overwrites `linkText` and every password `QString` with zeros before release; clears the clipboard if it still holds exactly the link; sets `state` to `closed` |
| signals | `changed`, `opened`, `linkOpened(id)`, `refused(message)`, `closed` | |

`Config` (set by `main.cpp`): `Store*` (only to read the account), `Worker*`, `HttpTransport*`, `Clock*`, `wifi_up`,
`ensure_network` (a tap while offline brings Wi-Fi up first, FR-006; empty = the tap is refused with `no_network`; the application-state clearing is paused while it waits, because the system's own Wi-Fi dialog may take the focus), `format_date`, `set_clipboard`, `clear_clipboard_if_equal`,
`clipboard_available`, `user_id`, `connected_host`. `clearSecrets()` is also called when `ConnectionBridge` reports that the account
changed or was disconnected, and when `QGuiApplication::applicationState()` leaves `ApplicationActive` (R-15).

Bridge rules for secrets:

* no `qDebug`/`qWarning`/`qInfo` and no `log::text`/`log::path` in `share_bridge.cpp` (rule N8); the bridge logs only
  `share.open|create|change|remove|fail` with `log::status`/`log::count`;
* the password arrives as a `QString` inside the call's map; it is converted to a `SecretText`, the `QString` is overwritten
  (`fill(QChar(0))`) and the map entry removed before the job is posted; the job lambda owns only the `SecretText`;
* `linkText` is never copied into a model, a role, a property other than `detail`, or a signal argument other than `linkOpened(id)`;
* nothing opens the link: no `QDesktopServices`, no `Qt.openUrlExternally` (rule N9).

## Pages (all in `qml/pages/`, built from existing components; Principle XI)

| Page | Contents |
|------|----------|
| `SharePage` (new) | `TopBar` (Back, title "Share", item name below it), one line with the offline state, the paged share list (`InkRow`: kind icon, `Public link` / `User: <name>` / `Group: <name>` / `Other kind of share`, second line "Read only · Password · Expires 31 Dec 2026", a "Not yours" or "From a parent folder" mark), page buttons, a note line (`truncated`, `inheritedNote`, `staleNote`), a `message` banner when set; bottom panel: **Create link** (disabled with the reason when `linkCreationOffered` is false or offline), **Refresh**. Rows of a public link open `ShareLinkPage`; user, group and other rows open a `ChoiceDialog` with a read-only summary and **Close** only (FR-009) |
| `ShareLinkPage` (new) | `TopBar` (Back, title "Link"). The link in large letters (`Text`, `textFormat: Text.PlainText`, `wrapMode: Text.WrapAnywhere`, the bolder style of `Style`), "N characters", a warning line for `DifferentAddress`/`NotSecure`, or "Link not readable" for `Unreadable`; below it the state: permission, "Password set"/"No password", expiry; bottom panel: **Copy** (only when `clipboardAvailable`; confirms with "Copied. Other apps can read it until you copy something else."), **Password**, **Expiry**, **Permission** (folders), **Remove link**; the last four are hidden for a share not owned by the user. No tap on the link text does anything (no `MouseArea`, no `onLinkActivated`) |
| `ShareFormPage` (new) | for Create (and as the dialogs for Change): rows *Permission* (two rows with a mark; "Read only" and, for folders, "Read and upload"; unavailable choices shown disabled with the reason), *Password* (opens the password entry), *Expires* (opens the date entry), each row showing its state and a mark "Required" when `rules` say so; the plain-language line of what the link will allow (changes with the choices); bottom panel: **Create** (opens the confirm dialog with `summary`; default button Cancel) and **Cancel** |
| `FolderPage` (change) | a **Share** tool (`objectName: "folderShare"`, icon `share`) in the select-mode panel, enabled when exactly one entry is selected; it calls `share.open(path)` and pushes `SharePage` |
| `DetailsPage` (change) | a **Share** tool (`objectName: "detailsShare"`) in the bottom panel of a file; an unavailable entry is opened too and `SharePage` shows the reason; offline `SharePage` shows the offline line |
| `TextMenu.qml` (change) | one additive property `forceSecret` (default `false`); the field is treated as secret (no Cut, no Copy) when `forceSecret` is true or `echoMode` is not Normal. Existing fields are unchanged |
| `Main.qml` (change) | registers the context property `share`; on `closed` pops any share page still on the stack |

Dialogs use `ConfirmDialog` and `TextEntryDialog` (footer buttons, no animation):

* **Password entry**: `TextEntryDialog` with the placeholder, `inputMethodHints: ImhSensitiveData | ImhNoPredictiveText | ImhNoAutoUppercase`,
  `TextMenu { forceSecret: true }`; shown as plain text; the field is emptied when the dialog closes; the value is read once.
* **Expiry entry**: `TextEntryDialog`, placeholder `YYYY-MM-DD` (German `JJJJ-MM-TT`), the allowed range under the field when known
  ("up to 31 Dec 2026"), `expiry_*` errors in the dialog's `errorText`; an empty accept clears the expiry only through the **Remove
  date** footer button, never by accident.
* **Confirm create / change / remove**: `ConfirmDialog` with the `summary`/`effectText` of the bridge; **Remove** names the item,
  says "The link stops working at once for everyone who has it. The file itself is not deleted." and defaults to Cancel.

## Messages (English source text; German in `qml/strings_de.tsv`; one text and one "what to do" per key)

| Key | Text | What to do |
|-----|------|-----------|
| `share_not_offered` | This server does not offer sharing. | Nothing else in the app is affected. |
| `share_public_disabled` | Public links are not allowed on this server. | Ask the server's administrator. Shares with users and groups are still listed. |
| `share_password_required` | This server requires a password for links. | Enter a password. |
| `share_expiry_required` | This server requires an expiry date for links. | Enter a date (up to N days from today when known). |
| `share_expiry_too_far` | The date is too far away. This server allows at most N days. | Choose an earlier date. |
| `share_upload_not_allowed` | This server does not allow uploads through public links. | Choose read only. |
| `share_item_not_shareable` | This item cannot be shared. | Choose another item or ask the administrator. |
| `share_item_gone` | This item no longer exists where it was. | Refresh the folder. |
| `share_not_changeable` | You may not change this share. | It belongs to someone else. |
| `share_gone` | This link no longer exists. (Remove: The link was already removed.) | The list is shown again. |
| `share_changed_meanwhile` | This link was changed meanwhile. | Check the current settings and confirm again. |
| `share_unknown_outcome` | The request was sent, but no answer arrived. The list may be out of date. | Tap Refresh. Creating again may make a second link. |
| `share_list_too_big` | More shares exist than can be listed here. | Open the item in the web interface. |
| `share_refused` | The server refused (code E403). | Try again later. |
| `expiry_invalid` / `expiry_in_past` / `password_invalid` / `permission_invalid` | The date is not valid (use YYYY-MM-DD). / The date has passed. / The password is not valid (1 to 256 characters). / This permission is not possible for a file. | Correct the entry. Nothing was sent. |

The offline line reads "Sharing needs a connection." and the retry is a tap (FR-006, FR-030).

## What the host walk and the QML self-test cover

`tests/walks/18-share-links.walk` drives: open Share on a file, create a link (defaults), show it, change the password, set an
expiry, remove the link; and on a folder with upload permission. The QML self-test checks that no `MouseArea` or `Link` handler exists
on the link display, that the password field has `forceSecret`, and that `rows` roles contain no `link`/`url`/`token` role name.

## As built (2026-10-22)

The table above is the design; the code differs in these places (see "Implementation notes" of [plan.md](../plan.md)):

- `open(remotePath)`, `refresh()`, `showLink(id)`, `create(options)`, `change(id, options)` and `remove(id, confirmed)` are `Q_INVOKABLE` and return a map (`{ok}`, `{needsConfirm, summary | effectText}`,
  or `{errorKey, field}`); the answers of the requests arrive as the signals `linkOpened(id)`, `createFinished(ok, key)`, `changeFinished(ok, key)` and `removeFinished(ok, key)`.
- `message` is `{key, field, fieldMask, limitDays, code}`; `text` and `hint` are composed by the pages from `Problems.title(key)` and `Problems.action(key)`.
- `detail` also has `hasPassword`, `expires`, `permission` ("read" | "upload" | "other"), `passwordValue` and `expiryValue`; `rows` have `title` and `detailLine` instead of the
  separate kind, permission, expiry and password roles of the table; `state` is closed | loading | listed | failed | working.
- Extra members: `leaveLink()`, `clearMessage()`, `dateText(iso)`, `itemName`, `isFolder`, `itemShareable`, `unavailableReason`, `inheritedNote`, `staleNote`, `hasExistingLink`,
  `truncated`, `total`, `page`, `pageCount`, `pageSize`, `nextPage()`, `previousPage()`; the slots `onApplicationStateChanged` and `onConnectionChanged`.
- The password, expiry and permission are changed from the rows of `ShareLinkPage`; the bottom panel of that page has Copy and Remove link. `ShareFormPage` is pushed from the **Create link** tool of
  `SharePage`; `TextEntryDialog` has an optional third button (`clearText`) and `secret`; `InkField` has `secretField`.

