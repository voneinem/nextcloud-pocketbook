# Contract: Bridge and QML (`bridge/`, `qml/`)

**Feature**: [../spec.md](../spec.md) | **Core contract**: [core-interfaces.md](core-interfaces.md)

The bridge is the only Qt layer between the QML pages and `pbc_core`. All network and file work runs on the one
`Worker` thread; a job reports back with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`. Properties are read
by the UI thread only. The bridges never block the UI thread and never start a request on their own: every
network-using method is an `Q_INVOKABLE` that a tap calls (the single exception, the account check at start, is
called once by `main.cpp`, see `ConnectionBridge.checkAtStart`).

Names that the UI shows are passed through `displaySafe()` (control characters removed, 200 characters, bidi
override characters removed) before they reach QML.

## ConnectionBridge (context property `connection`)

| Member | Type | Meaning |
|--------|------|---------|
| `state` | QString | `none`, `untested`, `ok`, `needs_signin`, `problem`, `offline` |
| `serverName`, `serverVersion` | QString | from the last successful check |
| `quotaText` | QString | "12.3 GB of 50 GB", "12.3 GB (unlimited)", or "" when unknown (formatted by the bridge, locale-aware) |
| `checkedAt` | qint64 | epoch seconds, 0 = never |
| `problemKey` | QString | error key of the last failure, "" = none |
| `clockWarningMinutes` | int | 0 = no warning |
| `busy` | bool | a check or login step is running |
| `loginActive`, `loginUrl`, `loginExpiresInMinutes` | | the browser login; `loginUrl` is "" when none is active |
| `credentialsProtection` | QString | `enforced`, `not_enforced`, `unknown` (existing `credentials_protection`) |
| `customCaStatus` | QString | shared with `settings` (existing) |
| `testConnection(address, loginName, appPassword)` | Q_INVOKABLE | runs R-3 steps; on success saves the account and the password; the typed password is cleared from memory afterwards |
| `startLogin(address)` / `checkLogin()` / `cancelLogin()` | Q_INVOKABLE | Login Flow v2 (R-4); `checkLogin` sends exactly one request |
| `signOut()` | Q_INVOKABLE | forgets the app password file first, then the account row; local copies, marks and the cache stay |
| `checkAtStart()` | called once by `main.cpp` | only when `device.wifiConnected()`; one account check (R-12) |
| signals | `changed`, `testFinished(ok, key)`, `loginChecked(status)` | status: `approved`, `pending`, `expired`, `failed` |

`qrImageUrl` is `image://qr/<counter>`; an `QQuickImageProvider` renders the matrix of `loginUrl` black and white at an
integer scale with a quiet zone. The page shows the text link and the image.

## BrowseBridge (context property `browse`)

| Member | Type | Meaning |
|--------|------|---------|
| `mode` | QString | `folder`, `favorites`, `search` |
| `path` | QString | current remote path (display form), `/` = root |
| `pathSegments` | list | `[{label, path}]` for the path line; long paths are shortened in the middle by the QML |
| `page`, `pageCount`, `pageSize` | int | the QML sets `pageSize` from the visible height; the bridge answers `rows` for one page only |
| `rows` | list of maps | `{name, kind, sizeText, dateText, availability, reasonText, state, kept, favorite, remotePath}` |
| `sortKey`, `sortAscending` | QString/bool | persisted in `settings` |
| `listingAgeText` | QString | "Updated 3 hours ago", "Not synced" (never confirmed), "Removed on the server" |
| `listingState` | QString | `fresh`, `cached`, `not_synced`, `gone`, `offline` |
| `offline` | bool | the reader has no Wi-Fi now |
| `busy`, `errorKey` | | a refresh is running; key of the last failure |
| `open(remotePath)` | Q_INVOKABLE | folder: show the cached listing; if never fetched and online, fetch it once; if never fetched and offline, show "Not available offline". File: emit `showDetails(remotePath)` |
| `up()` | Q_INVOKABLE | parent folder (cached listing only, no request unless never fetched) |
| `refresh()` | Q_INVOKABLE | explicit refresh of the shown folder or of the favorites |
| `nextPage()`, `previousPage()`, `goToPage(n)` | Q_INVOKABLE | local only |
| `setSort(key, ascending)` | Q_INVOKABLE | local only |
| `showFavorites()` | Q_INVOKABLE | shows the cached favorites; fetches only on `refresh()` or when never fetched and online |
| `searchOffline(term)` | Q_INVOKABLE | local only; results in `rows` with `source = reader` |
| `searchServer(term)` | Q_INVOKABLE | the explicit "also search the server" tap; results with `source = server`; `moreResults` flag |
| signals | `changed`, `showDetails(remotePath)`, `refreshFinished(ok, key)` | |

State words for the rows are keys the QML translates: `not_downloaded`, `downloaded`, `outdated`,
`changed_on_reader`, `conflict`, `removed_on_server`, `missing_locally`. Unavailable entries carry `availability` and
`reasonText` and the QML marks them and ignores taps except a tap that shows the reason.

## TransferBridge (context property `transfer`)

| Member | Type | Meaning |
|--------|------|---------|
| `details(remotePath)` | Q_INVOKABLE → map | `{name, sizeText, type, dateText, state, serverNewer, localPath?, canOpen}` from the cache and the local record |
| `preflight(remotePath)` | Q_INVOKABLE → map | `{verdict: ok|confirm|refused, errorKey, sizeText, freeText, neededText}` |
| `download(remotePath, confirmed)` | Q_INVOKABLE | starts the job (worker); one job at a time, a second tap is `busy` |
| `cancel()` / `resume()` | Q_INVOKABLE | |
| `jobState`, `jobProgressText`, `jobPercent`, `jobError` | | updated at most every 2 seconds or 5 % |
| `resolveConflict(remotePath, choice)` | Q_INVOKABLE | `keep_mine`, `keep_both`, `replace` (the QML asks the second confirmation before it passes `replace`) |
| `open(remotePath)` | Q_INVOKABLE | opens the recorded local file in the reader's viewer through `DeviceBridge.openFile` (path must be inside the download root) |
| `setKept(remotePath, kind, on)` | Q_INVOKABLE | add/remove a "keep offline" mark |
| `update()` / `cancelUpdate()` | Q_INVOKABLE | the user-started Update (R-10); `updateReport` map and `updateRunning` |
| `pausedDownloads` | list | journal rows to resume after a restart |
| signals | `changed`, `jobFinished(outcome)`, `updateFinished` | |

## SpaceBridge (context property `space`)

| Member | Type | Meaning |
|--------|------|---------|
| `items` (paged, 20 per page) | list | `{name, remotePath, sizeText, state, changedLocally}` of `local_files` |
| `usedText`, `freeText`, `cacheText` | QString | bytes used by downloads, free space on the card, size of the metadata cache |
| `remove(remotePath, confirmedLoss)` | Q_INVOKABLE | deletes the local copy and its row; `confirmedLoss` must be true when the copy is `changed_on_reader` or `conflict`; never contacts the server |
| `clearCache()` | Q_INVOKABLE | drops all cached listings (not downloads, not marks) |

## DeviceBridge (existing, extended)

`openFile(path)` gains the check "canonical path is inside the download root and is a regular file" (pattern from
notes-pocketbook); `wifiConnected()` and `connectNetwork()` are used as they are; `downloadRoot` property.

## Pages (all in `qml/pages/`, built from the existing components)

| Page | Contents |
|------|----------|
| `HomePage` (changed) | state line (connected as…, checked at…, or the problem/"Offline, not checked"); tools in the bottom panel: **Files**, **Favorites**, **Search**, **Storage**, **Settings** |
| `FolderPage` | `TopBar` (title = folder name, up button), path line, list of `InkRow`s (name, size, date, state word, kept mark), `PageButtons`, bottom panel: **Refresh**, **Sort**, **Favorites**, **Search**; listing age line under the path |
| `DetailsPage` | name, size, type, modified date, "On the reader: yes/no", "Server version: newer/same/unknown", buttons as bottom-panel tools: **Download** / **Open** / **Update this file**, **Keep offline** toggle, progress text and **Cancel** while a job runs |
| `ConflictPage` | sizes and dates of both versions, three tools: **Keep my copy**, **Keep both**, **Replace my copy** (confirm dialog) |
| `UpdatePage` | the Update report after a run, with the list "needs a decision" and "removed on the server" |
| `FavoritesPage` | like FolderPage, mode `favorites` |
| `SearchPage` | `InkField` with on-screen keyboard, results as `InkRow`, **Also search the server** tool |
| `SpacePage` (the tool and title read **Storage** in the UI; "space" is the internal name) | list of downloads with sizes, used/free text, **Remove**, **Clear cache** |
| `SettingsPage` (changed) | account section (address, user name, app password fields, **Test**, **Log in with a browser**, **Sign out**), the honest storage text, custom CA status, language, large-file warning size, About |
| `LoginPage` | the link text, the QR image, minutes left, **Check**, **Cancel** |
| `AboutPage` (changed) | adds the credential-storage text, the unofficial notice, the third-party list (existing) |

UI rules (constitution VI, XI; checked by `check-ui-style.sh` and `app_self_test`): no bordered text button, no
animation, icon plus label tools in the bottom panel, hairline separators, page buttons instead of scrolling, progress
as text updated coarsely, every dialog with footer buttons, every text through `qsTr` with a German entry in
`qml/strings_de.tsv` (`scripts/extract-strings.py --check`).

## Texts that must exist (English source; German in the table)

Connection problems (one message plus one "what to do" line each, FR-006), the storage text of FR-007, the preflight
refusals and the large-file warning, the conflict page, the state words, the listing-age phrases ("Updated just now",
"Updated 3 hours ago", "Updated 2 days ago", "Not synced"), the offline notice, the Update report lines and the
unavailable-entry reasons.

## As built (differences from the tables above)

- **TransferBridge** also has `setKept(remotePath, kind, on)` (kind `file` | `folder`; nothing is fetched by marking), `update()`, `cancelUpdate()`,
  `confirmUpdate(bool)`, the properties `updateRunning`, `updateStage` (`idle` | `checking` | `confirm` | `downloading` | `done`), `updateDone`,
  `updateTotal`, `updateReport` (`folders, files, added, updated, skipped, needDecision, removed, failed, uncheckedFolders, stoppedBy, decisionPaths,
  removedPaths`) and the signals `updateNeedsConfirm(bytesText, freeText)` and `updateFinished`. `update()` is ignored while a download or another Update
  runs; a plan that is larger than the warning size asks first. `details()` adds `state`, `kept`, `keptDirect`, `serverMissing`, `localChanged`, `localSizeText`
  and `localDateText`.
- **BrowseBridge** has `Config.download_root`; rows carry `state`, `kept`, `keptDirect`, `folderPath` and `source`; properties `mode` (`folder` | `favorites` |
  `search`), `moreResults`, `searchTerm`, `folderKept`, `folderKeptDirect`; invokables `showFavorites()`, `searchOffline(term)`, `searchServer(term)`,
  `showFolder()` (the pages of favorites and search call it when they close) and `refreshRows()` (re-reads the page from the store, no request).
  The favorites page is sorted in memory by the same sort setting.
- **SpaceBridge** has `pageSize` (20 until the page sets it from its height), `total`, `refresh()`, `nextPage()`, `previousPage()` and the signals
  `removeFinished(ok, errorKey)` and `cacheCleared(ok)`; `clearCache()` runs on the worker and sets `busy`.
- **Pages.** `FavoritesPage` and `SearchPage` share `qml/FlatList.qml` (rows with the folder path, the state words and, in a search, where the result came
  from). `qml/EntryWords.qml` is the one place for the state words, the source words and the age of a list. `UpdatePage` starts an Update only when it is
  pushed with `autoStart: true` (the tap that opens it is the request; a page created for any other reason, such as the self-test, starts nothing). A page
  that covers another one (favorites, search) takes the `showDetails` and `showReason` signals; the page below ignores them.
- **Host-only walk.** `app/ui_script.{h,cpp}` and `scripts/ui-walk.sh` run a scripted walk through the real pages against the fake server
  (`tests/tools/pbc_fake_nextcloud`); `tests/walks/*.walk` is the acceptance walk (CTest `ui_walk`). Not compiled for the reader.
