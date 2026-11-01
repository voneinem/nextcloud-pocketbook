# Bug Assessment: Open (eye icon) does nothing for a PDF without ".pdf" in its name

- **Slug**: open-synced-file-silent (the name comes from an earlier, wrong hypothesis; the slug is kept because downstream commands use it)
- **Created**: 2026-11-01 (revised twice; this version is based on the reader's logs and database)
- **Source**: pasted text, three clarifying answers, then the reader's `pbcloud.log` and `pbcloud.db` read over USB
- **Verdict**: valid (cause confirmed on the device: the file opens after it is renamed to end in `.pdf`)
- **Severity**: medium

## Report (verbatim or summarized)

> I tried to open a pdf document with the eye icon in folder "Rezepte" but it did not open in the pdf reader. This used to work before. Reader is mounted.

"Reader is mounted" means: the user connected the reader to a computer afterwards so the logs could be read. It did not happen while the bug occurred.

## Symptom

Tapping the eye ("Open") for the file `Baguette magique` in "Rezepte" does not start the PDF viewer, and the app shows nothing. The other PDFs in the same folder, which have `.pdf` names, are the ones that "used to work".

## Evidence (from the reader, read 2026-11-01)

- `PBCloud/Rezepte/Baguette magique` was downloaded at 07:50 (`pbcloud.log`: `download.start job=13`, `download.done bytes=374049`). `file` reports it as a real PDF (version 1.4, 2 pages), but its name has **no extension**.
- `pbcloud.db`, table `entries`: the server file is named `Baguette magique`, MIME `application/octet-stream`. The four other PDFs are named `….pdf` with MIME `application/pdf`. A second extension-less file, `Saftiger veganer Schoko-Kirschkuchen vom Blech`, is on the server too.
- `local_files` stores the copy as `Rezepte/Baguette magique`; the copy is a plain download, inside `PBCloud`. The only sync pair is `/Test` → `/mnt/ext1/screens`, so "Rezepte" is **not** in a pair (this corrects the earlier answer "yes" and the pair-folder hypothesis).
- The log has no line for the Open attempt: the hand-off is not logged (path lines are removed by the log filter, and there is no `open` event).

## Reproduction

1. On the server, put a PDF named without an extension (e.g. `Baguette magique`) into a folder.
2. Download it in the app, open its details page and tap the eye "Open".
3. Observed: nothing happens. Expected: the viewer opens, or the app says why it cannot.
4. Compare with a file named `….pdf` in the same folder: it opens.

Confirmed by the reporter: after renaming the file to end in `.pdf` it opens.

## Suspected Code Paths

- `app/device_bridge.cpp:135-148` — `DeviceBridge::openFile` hands the path to `OpenBook(path, "", 0)`, whose comment says "the reader application picks the viewer by file type". With no extension, the reader has no type to pick a viewer by. The result of `OpenBook` is not checked and the function returns `true`.
- `core/src/local_name.cpp` (`split_extension`, line 24) — builds the local name from the server name and already knows what counts as an extension; the app keeps the name as it is (decision below).
- `bridge/src/transfer_bridge.cpp:230-244` — `TransferBridge::open` ignores the result of `open_file`, so no failure reaches the page.
- `bridge/src/transfer_bridge.cpp:96-99` — `canOpen` is true for any regular file in the download folder, whatever its name.
- `qml/pages/DetailsPage.qml:158-163` — the Open button has no failure feedback (the Share button does: `shareFail`).

## Root Cause Hypothesis

The reader chooses the viewer from the file name's extension. The app mirrors the server name locally, and the server file is called `Baguette magique` (MIME `application/octet-stream`), so the downloaded PDF has no extension. `OpenBook` therefore finds no viewer, and the app shows no error because it neither checks nor reports the hand-off result. This fits "used to work before": all earlier files had `.pdf` in their names. Confidence: **high** (a PDF without extension was the only difference from the working files, and the reporter confirmed that it opens after the rename).

## Proposed Remediation

**Decision (reporter, 2026-11-01):** the app does not rename or add an extension. A PDF should end with `.pdf`; the fix is to say so. Confirmed on the reader: `Baguette magique` opens after it is renamed to end in `.pdf`.

**Preferred**: When the user taps Open and the file name has no extension, do not hand the file to the reader. Show a short dialog that says no app on the reader can open the file, shows the **full file name** and marks the missing ending, for example: "No app can open this file. Its name has no ending (missing extension): Baguette magique". Use the existing `ConfirmDialog` pattern (`shareFail`, `showCancel: false`) in `qml/pages/DetailsPage.qml`; the text comes from the bridge so the name is shown through `display_safe` like other names. Also make every other failed hand-off visible: `TransferBridge::open` uses the result of `open_file`, logs an `open.handoff` event with the outcome (no path), and the same dialog says the file could not be opened. Add the German strings to `qml/strings_de.tsv`.

**Alternatives** (considered and rejected by the reporter):
- Add an extension to the local name from the server MIME or the file content. Rejected: a PDF should end with `.pdf`; the user fixes the name on the server (Rename exists since 002).

**Files likely to change**:
- `bridge/src/transfer_bridge.cpp`, `bridge/include/transfer_bridge.h` (extension check, result of `open_file`, a signal or return value for the page, the log event)
- `qml/pages/DetailsPage.qml`, `qml/strings_de.tsv`
- `tests/bridge/test_transfer_bridge.cpp`
- `docs/device-test-checklist.md`

**Tests to add or update**:
- Bridge test: `open` on a downloaded file named without an extension does not call `open_file` and reports "missing extension" with the full name.
- Bridge test: a name with an extension (`x.pdf`) still calls `open_file` with the same path as before; a name that only starts with a dot (`.hidden`) or ends with a dot counts as missing (decide the exact rule in the fix).
- Bridge test: when `open_file` returns `false`, `open` reports a failure and logs it.
- Device check (next to the DT-M entries): Open `Baguette magique` shows the message with the full name; after renaming on the server to `….pdf` and downloading again it opens.

## Risks & Considerations

- What counts as "has an extension" is a rule the fix must define (last dot, not the first character, short ending); reuse `split_extension` from `core/src/local_name.cpp` instead of a second rule. A file with an unknown ending (`x.abc`) still goes to `OpenBook`; the new message covers only the case with no ending.
- The app never executes a download and does not choose the viewer (Principle IV); the message must not offer "open with".
- Files already on the reader are unchanged; no migration.
- The file name in the dialog is untrusted server text: show it through `display_safe` and keep it from running past the dialog (wrap or shorten the middle, never cut off the ending).
- `OpenBook` is device-only, so host tests cannot prove the viewer opens; a device check is needed.
- Separate item, not the cause here: `DeviceBridge::openFile` accepts only paths under `/mnt/ext1/PBCloud`, but `TransferBridge::open` also passes sync-pair folder paths (`sc->root`). A pair whose reader folder is outside `PBCloud` would be refused silently. Track as its own bug; the new failure dialog will at least make it visible.

## Open Questions

- [NEEDS CLARIFICATION: Should the sync-pair folder mismatch be fixed with this bug or tracked separately (recommended: separately)?]
