# Contract: Requests to the server (trash and versions)

**Feature**: [../spec.md](../spec.md) | **Research**: [../research.md](../research.md) R-1 to R-10 | **Core**: [core-interfaces.md](core-interfaces.md) | **002**: [server-requests](../../002-manage-server-files/contracts/server-requests.md)

Every request of the feature, its headers, its answer mapping and its bounds. Everything here is **UNVERIFIED** until a live run
(LV-T1..LV-T10): the property names, the shape of the original location, the restore verbs and the behavior with an app disabled are
written from the public documentation and general knowledge of the endpoints. The fake Nextcloud implements exactly this contract; a
hand-written fixture carries `.unverified`.

Common rules (from `DavSession`, unchanged): https only, certificate and host verified, a redirect to another host is refused, the credentials
go to the transport and never into a URL, every XML body goes through `xml_min`, answers are capped, one request at a time, connect 10 s and
total 30 s, **no retry**. `<base>` is the account's base address, `<uid>` the user id the account check holds, `<segment>` a path segment
validated as in research R-3 and percent-encoded by the app. **No URL is ever taken from an answer**; the app builds every address.

## T1 Capabilities (is the app on)

`GET <base>/ocs/v1.php/cloud/capabilities?format=json` with `OCS-APIRequest: true` (the call of 001 and 004 L1).

| Read | Maps to |
|------|---------|
| `ocs.data.capabilities.files.undelete == true` | trash `On` |
| `ocs.data.capabilities.files.versioning == true` | versions `On` |
| the `files` object present, the key missing or `false` | `Off` |
| any failure, a non-success OCS status, or no `files` object | `Unknown` |

Answer cap 2 MiB; depth 16; 40,000 nodes (`json_min`). Made when the user opens the trash or the versions list; not stored; not repeated within
a screen visit unless the user taps Refresh.

## T2 Trash list

```
PROPFIND <base>/remote.php/dav/trashbin/<uid>/trash/
Depth: 1
Content-Type: application/xml

<d:propfind xmlns:d="DAV:" xmlns:oc="http://owncloud.org/ns" xmlns:nc="http://nextcloud.org/ns">
  <d:prop>
    <nc:trashbin-filename/> <nc:trashbin-original-location/> <nc:trashbin-deletion-time/>
    <d:getcontentlength/> <d:resourcetype/> <oc:fileid/> <oc:permissions/> <nc:is-encrypted/>
  </d:prop>
</d:propfind>
```

| Answer | Maps to |
|--------|---------|
| 207 | `parse_trash` (R-3): rows sorted newest first, <= 1,000 kept, `truncated` if more |
| body larger than 4 MiB | `listing_too_large` |
| 404 | `Unknown` with code `T2-404` (the capability said On; R-2) |
| 401 / 403 | `auth_failed` / `forbidden` (existing mapping) |
| other | the existing `status_error` keys; never the server's text |

The first `d:response` (the `trash/` collection itself) is not an item. A `d:response` whose `href` does not lie under
`/remote.php/dav/trashbin/<uid>/trash/` is counted in `skipped_hostile` and skipped.

## T3 Entry probe (existing `fetch_entry`)

`PROPFIND` Depth 0 on `<base>/remote.php/dav/files/<uid><path>` (a trailing `/` for a folder), the property list of the file listings.
404 = "not there" (not an error). Used for: the original name, the original folder, a chosen target, and the file's ETag before and after a
version restore. No new code in `dav_write`; `trash_api` calls `fetch_entry`.

## T4 Restore to the original place

```
MOVE <base>/remote.php/dav/trashbin/<uid>/trash/<trash-name>
Destination: <base>/remote.php/dav/trashbin/<uid>/restore/<trash-name>
Overwrite: F
```

No body. The name in the `restore/` collection only identifies the request; the server puts the item back at its recorded original place.

| Answer | Maps to |
|--------|---------|
| 201 / 204 | confirmed at the transport level; the app then **verifies** with a T3 probe of the original place (research R-5) |
| 404 | `trash_item_gone` (the item is not in the trash any more): it leaves the stored list |
| 409 / 412 | `restore_name_taken` (the place became taken, or the original folder is gone): nothing changed; the item stays |
| 403 | `forbidden` (no permission) |
| 423 | `locked` |
| 507 | `server_storage_full` |
| no answer after the request was sent | `outcome_unknown`, never repeated |
| other | `status_error` keys + code `T4-<status>` |

## T5 Restore to a chosen place

```
MOVE <base>/remote.php/dav/trashbin/<uid>/trash/<trash-name>
Destination: <base>/remote.php/dav/files/<uid><chosen folder>/<chosen name>     (a folder item's Destination ends with "/")
Overwrite: F
```

| Answer | Maps to |
|--------|---------|
| 201 / 204 | verified by a T3 probe of the chosen path (present = done) |
| 412 | `restore_name_taken` (the name exists there); the existing entry is untouched |
| 409 | `restore_parent_missing` (the chosen folder is gone) |
| 403 / 405 / 501 / 502 **and** a T3 re-probe shows nothing was restored | `restore_place_unsupported` (research R-6) |
| 404 | `trash_item_gone` |
| 423 / 507 / no answer / other | as T4 |

`Overwrite: F` is always sent; there is no request in this feature without it (nothing is ever replaced).

## T6 Version list

```
PROPFIND <base>/remote.php/dav/versions/<uid>/versions/<fileid>/
Depth: 1
Content-Type: application/xml

<d:propfind xmlns:d="DAV:" xmlns:nc="http://nextcloud.org/ns">
  <d:prop> <d:getlastmodified/> <d:getcontentlength/> <d:getcontenttype/> <nc:version-label/> </d:prop>
</d:propfind>
```

| Answer | Maps to |
|--------|---------|
| 207 | `parse_versions` (R-3): <= 200 rows newest first; the collection itself is not a row; zero rows = `no_older_versions` |
| body larger than 1 MiB | `listing_too_large` |
| 404 | `no_older_versions` when the capability is On and the file exists (T3 says present); else `file_gone` |
| other | `status_error` keys + code `T6-<status>` |

`<fileid>` is `RemoteEntry::fileid` (digits only; anything else sends nothing and reports `refresh_first`).

## T7 Restore a version

```
MOVE <base>/remote.php/dav/versions/<uid>/versions/<fileid>/<version-id>
Destination: <base>/remote.php/dav/versions/<uid>/restore/target
```

Sent only after the stale check of research R-9 (T3 ETag equal to the one the list was read against).

| Answer | Maps to |
|--------|---------|
| 201 / 204 | confirmed; verified by T6 and T3 (a new version is expected; if none appears the page says so) |
| 404 | `version_gone` (reload) |
| 403 / 423 / 507 / other | `forbidden` / `locked` / `server_storage_full` / `status_error` + code `T7-<status>` |
| no answer after the request was sent | `outcome_unknown`, never repeated |

## T8 Delete one trash item for good

```
DELETE <base>/remote.php/dav/trashbin/<uid>/trash/<trash-name>
```

Sent only by `TrashBridge` after the last confirmation of the chain (research R-8), one request per item; also the unit of Empty trash.

| Answer | Maps to |
|--------|---------|
| 204 / 200 | removed |
| 404 | removed ("already gone") |
| 207 | `partial_failure` for a folder item (some members failed): counted as not removed, plain message |
| 403 / 423 / 429 / 5xx | `forbidden` / `locked` / `rate_limited` / `server_error`: counted as not removed (429 and 401 also stop an Empty-trash loop) |
| no answer after the request was sent | `outcome_unknown`; the loop stops; never repeated |

## Write conditions (constitution III)

| Request | Condition used | Where the guarantee comes from |
|---------|----------------|-------------------------------|
| T4, T5 | `Overwrite: F`; a pre-check probe; a post-check probe | R-5, R-6 |
| T7 | a re-read of the file's ETag immediately before | R-9 |
| T8 | the unique trash name | R-7 |

`tests/integration/test_trash_requests.cpp` records every request the app makes in each flow and asserts: no request without its condition,
no `PUT`, `MKCOL`, `COPY`, `PROPPATCH`, `LOCK`, `UNLOCK` anywhere in the feature, no request to the trash collection itself with
`DELETE`, no request outside the account's `trashbin`, `versions` and `files` paths, and no request at all when the user only opens the
app, the Home page or the folder page.

## Bounds

| Item | Bound |
|------|-------|
| Trash answer | 4 MiB; 1,000 rows kept |
| Version answer | 1 MiB; 200 rows kept |
| Name / trash name | 255 bytes; original location 1,024 bytes |
| Empty trash | the named set only, at most 1,000 requests, one at a time |
| Timeouts | connect 10 s; total 30 s per request |
