# PB Cloud

A **read-only Nextcloud files client** for the PocketBook InkPad One. Browse your files page by page, download them on
demand, keep chosen files and folders offline, and open them in the reader's own viewer. Built for an e-ink screen, offline
first, with no background network activity.

![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)
![Version](https://img.shields.io/badge/version-0.1.0-informational.svg)
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
- **Storage.** Lists everything the app downloaded with sizes, removes local copies (the server is never touched), shows free space
  and the size of the saved lists, and clears them.

The interface is English with a German translation (Settings > Language; texts in `qml/strings_de.tsv`).

## Not in this version

Uploading, creating, renaming, moving or deleting on the server, sharing, trash, versions, folder synchronization, several
accounts, end-to-end encrypted folders (they are shown as such and cannot be opened), Talk, photo upload, editing documents,
notifications. PB Cloud is read-only toward the server: it sends no write request of any kind.

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

- [specs/001-nextcloud-files-client/spec.md](specs/001-nextcloud-files-client/spec.md): what the app does and why.
- [specs/001-nextcloud-files-client/plan.md](specs/001-nextcloud-files-client/plan.md), `research.md`, `data-model.md`, `contracts/`.
- [.specify/memory/constitution.md](.specify/memory/constitution.md): the rules this project is held to.
- [docs/baseline-leftovers.md](docs/baseline-leftovers.md), [docs/device-findings.md](docs/device-findings.md).

## License

MIT, Copyright (c) 2026 Volker von Einem. Third-party components and their licenses are listed in [NOTICE](NOTICE) and shown in the
app under Settings > About.
