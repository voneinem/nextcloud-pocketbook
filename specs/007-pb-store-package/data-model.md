# Data Model: PB Store Release Package

No database or runtime data changes. These are the files and values the feature handles.

## Manifest template (`packaging/manifest.template.json`, in the repository)

| Field | Value | Source |
|-------|-------|--------|
| `schema_version` | `"1.0"` | fixed |
| `id` | `"pbcloud"` | fixed |
| `name` | `"PB Cloud"` | fixed |
| `version` | `"@VERSION@"` | placeholder, replaced at build time |
| `min_firmware` | `"6.11.1437"` | fixed (the owner's InkPad One; to be re-read at the device check, FR-015) |
| `target_device` | `"InkPad One"` | fixed |
| `menu_entry.id` | `"U_PbCloud"` | fixed (equals `INSTALL.md`, section 2) |
| `menu_entry.title` | `"PB Cloud"` | fixed (equals `INSTALL.md`) |
| `menu_entry.path` | `"applications/pbcloud.app"` | fixed (`INSTALL.md` registers `/mnt/ext1/applications/pbcloud.app`, the same file) |
| `menu_entry.icon` | `"applications/icons/pbcloud.bmp"` | fixed (equals `INSTALL.md`) |
| `menu_entry.focused_icon` | `"applications/icons/pbcloud_f.bmp"` | fixed (equals `INSTALL.md`) |
| `menu_entry.group` | `"@General"` | fixed |
| `data_dir` | `"system/pbcloud"` | fixed |

`files` is **absent** from the template; `pbstore-catalog pack` adds it (path, size, SHA-256, sorted).

Rules (store contract, schema 1.0): `menu_entry.path/icon/focused_icon` must be listed in `files`; `data_dir` is not inside
`applications/`; the package never contains files in it; `id`, menu id and `data_dir` differ from those of PB Notes
(`pbnotes`, `U_PbNotes`, `system/pbnotes`) and PB Office.

## Package manifest (`manifest.json` inside the ZIP)

The template with `@VERSION@` replaced and `files` added. Invariants:
- `version` = content of `VERSION` = version in the ZIP name = the version compiled into `pbcloud.bin`.
- every file under `applications/` is in `files`, every `files` entry exists, sizes and SHA-256 match (written by `pack`).

## Release package (`dist/pbcloud-inkpad-one-<version>.zip`)

```text
manifest.json
applications/pbcloud.bin, pbcloud.app, pbcloud-probe.sh, pbc-probe.bin (when built with the probe)
applications/pbc-handoff.bin, pbcloud-handoff-target.app           (only when built with the hand-off probe; not in a release)
applications/pbcloud-cacert.pem, pbcloud-emoji.ttf, pbcloud-emoji-OFL.txt
applications/pbcloud-zoneinfo/...      (598 time zone files and VERSION; names with '+' are allowed from contract v0.1.4)
applications/pbcloud-plugins/imageformats/libqjpeg.so, libqgif.so   (when the SDK's Qt plugins were found)
applications/pbcloud-licenses/...      (licenses/manifest.txt and every text it names)
applications/icons/pbcloud.bmp, pbcloud_f.bmp
```

Top level: `manifest.json` and `applications/` only. No `INSTALL.md`. The `applications/` file list equals that of the 0.6.0
package for the same build options (the old ZIP also had directory entries; the packed ZIP has none; the files are the same).
Which optional parts a build contains is decided by how it is invoked, as before; the manifest lists whatever is staged.

State: *staged* (temp dir) -> *packed twice* -> *identical* -> *checked* -> *within budget* -> *published* (moved into the
output folder). A failure at any step ends the build with no new ZIP there.

## Release

| Part | Rule |
|------|------|
| Git tag | `v<version>`, created after the merge, equals `v` + manifest version |
| GitHub release | for that tag; the ZIP attached; notes = changelog entry + link to `INSTALL.md` at that tag |
| Verified by | `scripts/check-release.sh` (tag, `VERSION`, ZIP name, manifest version), then `check-package` on the downloaded asset |
| Handed to the owner | the URL of the GitHub release and of the ZIP asset |
