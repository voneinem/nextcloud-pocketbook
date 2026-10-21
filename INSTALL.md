# Installing PB Cloud on the PocketBook InkPad One

These steps install the app from the release zip. They are written for the InkPad One (RK3566); other models are untested. PB Cloud is
a separate app from PB Notes and PB Office: it uses its own files and its own data folder and never touches theirs.

## 1. Copy the files

1. Connect the reader over USB so its storage appears as a drive.
2. Unzip the package and copy the `applications/` folder into the **root** of the reader's storage, merging it with the existing
   `applications/` folder. You should end up with:

   ```text
   applications/pbcloud.app             launcher (shell script)
   applications/pbcloud.bin             the program
   applications/pbcloud-cacert.pem      bundled CA certificates (used when the reader has none)
   applications/pbcloud-emoji.ttf       monochrome emoji font (Noto Emoji)
   applications/pbcloud-emoji-OFL.txt   license of the emoji font (SIL OFL 1.1)
   applications/pbcloud-zoneinfo/       time zone data for the dates in lists (copy the whole folder)
   applications/pbcloud-plugins/        Qt image plugins (copy the whole folder)
   applications/pbcloud-licenses/       license texts shown by Settings > About (copy the whole folder)
   applications/pbcloud-probe.sh        the device probe script (see "Device probe")
   applications/pbc-probe.bin           the device probe (see "Device probe")
   applications/icons/pbcloud.bmp
   applications/icons/pbcloud_f.bmp
   ```

## 2. Register the app in the Applications menu

The reader only lists apps that are registered in its desktop configuration.

1. On the reader's storage open `system/config/desktop/view.json`. **Back it up first.**
2. Under the top-level `"applications"` object add:

   ```json
   "U_PbCloud": {
     "path": "/mnt/ext1/applications/pbcloud.app",
     "title": "PB Cloud",
     "icon": { "path": "applications/icons/pbcloud.bmp" },
     "focused_icon": { "path": "applications/icons/pbcloud_f.bmp" }
   }
   ```

3. Add `"U_PbCloud"` to one of the `"groups"` application lists, for example the one named `@General`.
4. Rules to keep in mind: the id must start with `U_`, `path` must be absolute, and the icons must be opaque BMP files (122x122, 8 bit).
5. Safely eject the reader and **reboot** it.

### Upgrading

Copy the same files over the old ones. The data folder `system/pbcloud/` (database, the saved app password, log) is kept; the app upgrades
its database by itself, after making a backup copy, and refuses to start with a clear message if the data were written by a newer
version. Your downloads in `PBCloud/` are never touched by an upgrade.

## 3. Start the app and connect

Open Applications, then the group you added the entry to, and start **PB Cloud**. Open **Settings** (the gear), type your server
address (for example `cloud.example.org`), your user name and an **app password**, and tap **Test**. The result shows the server's name
and version and your quota.

Create the app password in Nextcloud under Settings > Security > Devices & sessions. Do **not** type your account password: the app never
stores it, and with two-factor authentication it would not work anyway.

Instead of typing a password you can use **Browser login** in Settings: the reader shows a link and a QR code; open it on your phone or
laptop, approve the login there, then tap **Check** on the reader.

The language of the interface (English or German) follows the reader and can be changed in Settings. The launcher writes a log to
`system/pbcloud/pbcloud.log` (the launch before is `pbcloud.log.prev`). The app writes no addresses, names, paths or passwords to it. The reader's own libraries print the path of a book when it is opened; the launcher removes such lines before they reach the log (it replaces each by a notice), and a log written by an older package may still hold them.

## Where files go

Downloads are saved in the folder `PBCloud/` in the root of the reader's storage, in the same folders as on the server, so that the
reader's own library finds them. A download is first written under a temporary name that the library does not show, and renamed only
when it is complete. Open a downloaded file with one tap on **Open**; the app hands it to the reader's viewer.

## What the upload browser shows

*Add > Upload files* lists the folders of the reader's storage that exist: `Audio Books`, `Downloads`, `Music`, `My pictures`, `Notes`,
`Photo`, `screens` and `PBCloud` (the folder of the downloaded copies, so that a copy you edited can be sent back). Nothing else of the
storage is shown, and the app's own data folder (the database, the credentials, the log) can never be picked. Files are only read; the app
never changes or removes a file you pick. There is nothing to install for this.

## Passwords

PB Cloud stores the **app password** as plain text in a file in `system/pbcloud/`. This reader's storage cannot make the file private, and
Settings says what it measured. Anyone who gets the reader's storage can use the app password until you remove it in Nextcloud
(Settings > Security). The account password is never stored.

## Servers with a private certificate authority

PB Cloud always verifies the server's certificate and has no switch to skip this. If your server uses a certificate from a private
authority, give the app that authority's certificate:

1. Get the certificate file in PEM format (a text file that starts with `-----BEGIN CERTIFICATE-----`).
2. Connect the reader over USB and copy the file to `system/pbcloud/custom-ca.pem` on the reader's storage (create the folder `pbcloud`
   inside `system` if it does not exist yet).
3. Safely eject the reader and start PB Cloud again. **Settings** shows whether a custom CA file is loaded. If it says the file could not
   be used, it is ignored (it must contain only well-formed certificates and be smaller than 1 MB).

## Device probe

The package can contain `pbc-probe.bin` and `pbcloud-probe.sh` in `applications/`. They print what this reader's system can do (the libcurl
protocols, certificate files, whether file permissions are kept, free space, whether a file above 4 GiB can be made, memory) and no
personal data. To run them:

1. Create an empty file named `run-probe` in `system/pbcloud/` on the reader's storage (create the folder if it does not exist yet).
2. Eject the reader safely and start **PB Cloud** once. The screen stays blank for a moment; the app itself is not started on this run and
   the file is removed.
3. Connect the reader again and read `system/pbcloud/probe.txt`.

## After installing: what has been checked

The tests of this project use a fake Nextcloud and the host. Work through `docs/device-test-checklist.md` on the reader and
`docs/live-server-checklist.md` against your own server, and write the dates and results next to the items; until then every item is
**unverified**.

## Troubleshooting

- Nothing happens when starting: read `system/pbcloud/pbcloud.log` on the reader's storage (the launch before is in `pbcloud.log.prev`).
  If it says `failed to open device file /dev/mali0 ... Permission denied` (exit code 134), the reader's system has lost access to the
  graphics chip for new apps. PB Cloud retries once by itself; if that fails, restart the reader. Please keep both log files and note what
  you did before.
- The entry does not appear: check that the JSON of `view.json` is still valid, then reboot.
- "The server's certificate is not trusted": see the private certificate authority section above.
- "The reader's date and time are wrong": set the date and time in the reader's settings; certificates cannot be checked with a wrong clock.
- A download stops with "not enough space": the message names how much is needed and how much is free. Remove copies under **Storage**.
