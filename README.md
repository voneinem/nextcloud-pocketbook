# PB Cloud

A **Nextcloud files client** for the PocketBook InkPad One. Browse your files page by page, download them on demand, keep
chosen files and folders offline, open them in the reader's own viewer, upload and manage files on your server, and share files and folders with
public links. Built for an
e-ink screen, offline first, with no background network activity: the app writes to the server only on your action.

![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)
![Version](https://img.shields.io/badge/version-0.6.0-informational.svg)
![Platform: PocketBook InkPad One](https://img.shields.io/badge/platform-PocketBook%20InkPad%20One-lightgrey.svg)
![Status: in development](https://img.shields.io/badge/status-in%20development-orange.svg)

PB Cloud is a sibling of [notes-pocketbook](https://github.com/voneinem/notes-pocketbook) and office-pocketbook (MIT, same
author). It is a separate app with its own binary, launcher and data folder.

> **Unofficial.** PB Cloud is not made, endorsed or supported by Nextcloud GmbH or by PocketBook. "Nextcloud" is named only to
> say which server it talks to; the app does not use the Nextcloud logo.

> **Status: in development.** The code is tested on the host against a fake Nextcloud server. Nothing has been verified
> against a real Nextcloud server or on a reader yet; every such item is listed as **unverified** in
> [docs/live-server-checklist.md](docs/live-server-checklist.md) and [docs/device-test-checklist.md](docs/device-test-checklist.md).

## Table of contents

- [Features](#features)
- [Not in this version](#not-in-this-version)
- [Requirements](#requirements)
- [Installation](#installation)
- [Credentials and privacy](#credentials-and-privacy)
- [Building from source](#building-from-source)
- [Testing](#testing)
- [Verification](#verification)
- [Documentation](#documentation)
- [License](#license)

## Features

- **Connect.** Server address, user name and an app password, with a Test button that shows the server's name, version and your
  quota. Or start a browser login: the reader shows a link and a QR code to open on a phone or laptop, you tap Check on the reader
  when you are done, and no password is typed on the reader. Only https is accepted. Every failure has a plain message with what to
  do: wrong credentials, two-factor authentication (use an app password), maintenance mode, a server that is too old or has no
  WebDAV, an address that is not a Nextcloud, an untrusted certificate (see `custom-ca.pem` in [INSTALL.md](INSTALL.md)), a wrong
  reader clock.
- **Browse.** Folders first, with name, size and date, page by page with page buttons, sorted by name, date or size. A folder is
  fetched when you open it for the first time and when you tap Refresh, never by itself. Folders you visited stay readable without
  a connection and say how old the list is.
- **Download and open.** The details of a file show its size, type, date, whether it is on the reader and whether the server has a
  newer version. Downloads show progress, can be cancelled, resume after a lost connection, check free space first, warn about very
  large files and refuse what the storage cannot hold (the 4 GiB limit of FAT). Files land in `PBCloud/` on the reader's storage and are
  opened with one tap in the reader's viewer. A copy you changed is never overwritten without asking.
- **Keep offline and Update.** Mark files or whole folders as kept. **Update** (only when you tap it) checks them against the server
  and downloads what is new or changed; a file changed on both sides is left alone for you to decide. Entries show whether they are
  not downloaded, downloaded, outdated, changed on the reader, in conflict or removed on the server.
- **Favorites and search.** The files you starred in Nextcloud. Search by name works offline in what the reader knows; **Also search the
  server** asks the server once, on your tap.
- **Upload.** *Add > Upload files* opens a file browser of the reader's storage (list or tiles, remembered): pick files and whole folders
  and they are put in the **queue**, shown in the listing marked **Not synced**. Nothing is sent by picking. Large files go in chunks
  and continue where they stopped after a lost connection; an upload whose outcome is unknown is kept and flagged, never repeated
  silently.
- **Manage.** New folder, rename, move, copy and delete (the select mode of a folder). Each is queued first and sent later, always on
  the version of the entry you saw: if it changed on the server meanwhile, the change is held and both versions stay until you decide.
  A name that is already taken asks: keep both under a new name, replace after a confirmation, or skip. Delete asks for confirmation and
  says it goes to the **server's trash**; on a server without a trash it is not offered at all (a permanent delete is not part of this app).
- **Queue and Auto Sync.** The Queue screen shows what waits, what failed and why, what needs your decision. **Send** runs the queue.
  With **Auto Sync** (Settings; on by default, switchable) the queue is also sent a few seconds after your own action, when Wi-Fi is
  already on; never at start, never when Wi-Fi comes back, never again by itself after a failure.
- **Folder sync.** *Sync* pairs a server folder with a folder on the reader, as **download only** (the default), **upload only** (for an
  Outbox) or **two-way**. A comparison runs only at start (when Wi-Fi is already on, for pairs that have it switched on), on the **Sync**
  button, or shortly after your own change (Auto Sync, per pair); never in the background. Every comparison ends in a **plan you review**:
  what is downloaded, uploaded, kept in both versions or skipped. Nothing changes until you approve it, and the result is shown with
  counts afterwards. A file changed on both sides **keeps both versions** under distinct names. **Nothing is ever deleted
  automatically**: a file removed on one side is only a proposal, and deleting needs your confirmation naming the file (on the server it goes to the trash; on the
  reader it cannot be recovered with this app). Every pair has a limit of files and bytes (500 files and 2 GB by default) and a reserve
  of free space; a plan over a limit cannot be approved. An interrupted sync is finished by the next one without doubled or partial files.
- **Share links.** *Select* an entry in a folder listing (or open a file's details) and tap **Share**. You see what is already shared: public links, and
  shares with users and groups (listed only; this app does not create, change or remove those). **Create link** takes a read-only link or, for a
  folder, one that also lets visitors **upload**, with an optional **password** and an optional **expiry date**, and asks for a confirmation that
  says in plain words what the link will allow. A link is shown **in large letters** on its own screen so you can read it off or type it elsewhere;
  **Copy** is offered only where the reader has a clipboard that other apps can use (it is off in this version on the reader, see below). You can change a
  link's password, expiry date or permission, one confirmed action at a time, and **remove** a link after a confirmation that says the file itself is not
  deleted. When the server forbids public links, forces a password or an expiry date, or limits the permissions, you get a plain message and a next
  step. Nothing about sharing is queued, retried, cached or sent without your tap, and the app **never opens a link**.
- **Mail link.** On the screen of a link, **Mail link** hands the link to **PB Office**, which opens a new message with the link and the subject "Link to <name>"
  (a confirmation that defaults to Cancel says where it goes and that a **password is not included**). You choose the recipient and send the mail in PB Office;
  PB Cloud sends nothing. It needs PB Office 0.4.0 or later, installed and **not running**: a running PB Office does not take the link (a short notice says so).
  The text is passed to PB Office and PB Office keeps it as a draft, which later syncs to your Drafts folder on the mail server; PB Cloud does not store it,
  write it to a log or put it on a clipboard, never hands over a password, and overwrites the buffers it controls right after the call.
- **Trash and versions.** *Trash* (on the Home page and in the folder page) lists what was deleted on the server, newest first, with the folder it came from, the day it
  was deleted and its size; the list is read only when you open the screen or tap Refresh, and the last one is shown, marked with its age, when there is no connection.
  Choose **one item** and **Restore** it: it goes back to its original place after a confirmation. If something with that name is there now, or the folder is gone,
  nothing is overwritten, renamed or created: you choose another folder (server folders only) and, if the name is taken there too, another name. Restoring to a chosen
  place needs the server to allow it; a server that does not says so and changes nothing. A restore whose answer was lost is shown as **outcome unknown** and is never
  repeated by the app. **Delete for good** and **Empty trash** are irreversible and exist **only on this screen**: a file asks once, a folder and Empty trash ask twice,
  every confirmation names what will be lost (the item, or the number of items and folders and some of their names), says it cannot be undone and has Cancel first; only
  the items it named are removed, one request at a time, and an item that reached the trash meanwhile stays. In a file's details, **Versions** lists the older versions
  (date and size) and **Restore** makes one the current content after a confirmation that names the file and the version; the content that was current is kept by the
  server as one more version, and a file that changed after the list was read is refused. When the server's trash app or versions app is off, the screen says so in
  plain words and offers no action; an unclear answer gets a generic message with a short code, never the claim that an app is off. Nothing here is queued: every action
  is one request on a live connection, and none is made at start, by a timer, by Auto Sync, by a sync or by an update.
- **Clear refusals.** A full account, a name the server does not accept, a file that is too large or of a type the server refuses, a
  locked file and a revoked password each have their own message and a next step; the item stays in the queue.
- **Storage.** Lists everything the app downloaded with sizes, removes local copies (the server is never touched), shows free space
  and the size of the saved lists, and clears them.

The interface is English with a German translation (Settings > Language; texts in `qml/strings_de.tsv`).

## Not in this version

Creating shares with users or groups, sharing by mail through the server (Mail link above uses PB Office instead), notes to the recipient, QR codes for links, retention settings of the trash and of the versions, comments, the activity feed, deleting a single version, downloading an old version, permanent deletion anywhere but on the trash screen, photo auto-upload, live watching of folders, any
background activity, several accounts, end-to-end encrypted folders (they are shown as such and cannot be opened), Talk, photo upload,
editing documents, notifications. PB Cloud writes to the server only when you ask it to: through the queue, with a condition on
every request, and never by itself.

## Requirements

- A PocketBook InkPad One (other models are untested).
- A Nextcloud server reachable over **https**, and an **app password** (Nextcloud: Settings > Security > Devices & sessions).

## Installation

See [INSTALL.md](INSTALL.md): copy the package to the reader, register the app in the Applications menu, optionally add a private
certificate authority, and tap Test in Settings.

## Credentials and privacy

- The app stores an **app password**, never your account password. It is kept as **plain text** in a file in `system/pbcloud/` on the
  reader. The reader's storage is FAT and **cannot make that file private** (the mode bits are not enforced): anyone who gets the
  reader's storage can read it and use the app password until you remove it in Nextcloud (Settings > Security). Settings shows what
  this reader's storage really does with the file.
- Certificates are always verified; there is no switch to turn that off. Credentials are never sent to another host than the one
  you entered, and a redirect to another host is refused.
- Server addresses, user names, file names, paths, search terms and tokens are not written to the log and not shown in error texts. The reader's libraries print the path of a book when it is opened; the launcher filters those lines out of the log.
- Names that come from the server are never trusted as local paths: they are cleaned and checked before any file is created, and a
  link or an unexpected file in the download folder stops the download.
- **Share links and their passwords are secrets.** A link or a password is shown as plain text only on the screen you asked for. Neither is shown in a
  list, written to the log or an error text, stored in the database or in a file, or exported; the app never opens a link or asks the server for its page.
  The password you type is sent **once** with the request that needs it and is then discarded: the app stores no copy of it (not even after a failed request:
  you type it again). The memory the app controls is overwritten, but the on-screen keyboard and the interface's text fields are libraries the app cannot
  wipe, so the honest statement is "never stored, never logged, never shown again", not "gone from every byte of RAM". The link screen clears the link when you
  leave it, when the account changes and when the app is no longer the active one. **Copy** puts the link on the system clipboard, where other apps can read
  it until you copy something else; the reader has no system clipboard (a test on 2026-10-23: nothing copied here could be pasted into PB Office), so **Copy is not offered on the reader**
  (build option `PBC_DEVICE_CLIPBOARD`, off) and the link is shown in large letters for typing. On a platform with a clipboard Copy is offered.
- **The trash list is stored.** The last trash list (names, original folders, dates) is kept in the database so that it can be shown without a connection; it holds only what
  the trash screen shows, is replaced at every refresh, and is removed when you sign out or connect another account. Restoring and deleting never touch the files on the reader.
  Everything the app knows about the server's trash and versions is **unverified** against a real server until the items LV-T1 to LV-T10 are run.
- Nothing is sent in the background. The app asks the server only at start (one check that the login still works, and only when
  Wi-Fi is already on) and when you tap something that needs it.

## Building from source

Host build (desktop, with Qt 6 for the bridge, the app and the bridge tests; the core builds without Qt):

```sh
cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
ctest --test-dir build-host
```

Try the app on the desktop against a fake Nextcloud (a throw-away data folder, its own certificate authority, no real account):

```sh
scripts/run-host-demo.sh
```

Device build: set `POCKETBOOK_SDK` to the PocketBook SDK (or pass `-DSDK_PATH`); no path to the SDK is written into the repository.

```sh
POCKETBOOK_SDK=<sdk> scripts/build-curl-arm.sh     # once: a static libcurl for the reader
POCKETBOOK_SDK=<sdk> scripts/package-device.sh     # the zip with INSTALL.md
```

## Testing

`ctest --test-dir build-host` runs the unit, integration and bridge tests, the fixture and constitution checks and the UI style check.
`scripts/pre-merge.sh` runs everything a merge needs, including a build with the sanitizers. Tests that use a fake server say so
in their output, and no server behavior is treated as verified because a fake agrees with it.

## Verification

What is checked and what is not:

- **Host tests against a fake server:** passing. They prove the app does what this project assumes the server does.
- **A real Nextcloud server:** not yet. Items LV-N1 ... in [docs/live-server-checklist.md](docs/live-server-checklist.md).
- **The InkPad One:** not yet (speed, memory, the library finding the downloaded files, Wi-Fi drops, the viewer). Items DT-NC-1 ... in
  [docs/device-test-checklist.md](docs/device-test-checklist.md). Figures in that file marked "host" are from a desktop.

## Documentation

- [specs/004-share-links/spec.md](specs/004-share-links/spec.md): share links (spec, plan, research, contracts, acceptance).
- [specs/005-mail-link/spec.md](specs/005-mail-link/spec.md): mailing a share link to PB Office (spec, plan, research, contracts, acceptance).
- [specs/001-nextcloud-files-client/spec.md](specs/001-nextcloud-files-client/spec.md): what the app does and why.
- [specs/001-nextcloud-files-client/plan.md](specs/001-nextcloud-files-client/plan.md), `research.md`, `data-model.md`, `contracts/`.
- [.specify/memory/constitution.md](.specify/memory/constitution.md): the rules this project is held to.
- [docs/baseline-leftovers.md](docs/baseline-leftovers.md), [docs/device-findings.md](docs/device-findings.md).

## License

MIT, Copyright (c) 2026 Volker von Einem. Third-party components and their licenses are listed in [NOTICE](NOTICE) and shown in the
app under Settings > About.
