# Contract: Requests to the Nextcloud server

**Feature**: [../spec.md](../spec.md) | **Research**: [../research.md](../research.md) | **API notes**: `docs/reference/nextcloud-api-notes.md`

**Status of every row: UNVERIFIED** (constitution IX). The rows are assembled from the public documentation and RFCs.
They become verified only through an entry in `docs/live-server-checklist.md` (LV-N*, added by this plan). Contract
tests in `tests/` run against the in-process fake in `tests/support/fake_nextcloud_server.h`, which implements *this file*, so a
passing test proves the app follows this contract, not that a real server does.

Conventions: `<base>` is the canonical `https` address of the account (no trailing slash). `<uid>` is the user id (may
differ from the login name). `<path>` is a remote path, each segment percent-encoded as UTF-8 (`%20` for space, never
`+`), no Unicode normalization. Every authenticated request carries HTTP Basic `<login name>:<app password>` through
the transport (never in the URL), `User-Agent: PBCloud/<version>`, and, for OCS, `OCS-APIRequest: true` and
`Accept: application/json`. Only the methods `GET`, `POST`, `PROPFIND`, `REPORT` and `SEARCH` are ever sent (rule N2).
Redirects are followed only on https to the same host and port; credentials never leave it (existing transport).

Body limits: in-memory answers are capped at 8 MiB (existing); file bodies stream to disk.

## 1. Server status

| | |
|--|--|
| Request | `GET <base>/status.php`, **no credentials** |
| OK | 200, JSON object |
| Fields used | `installed` (bool), `maintenance` (bool), `needsDbUpgrade` (bool), `version` (string, `major.minor.micro[.build]`), `productname` (string, optional) |
| Mapping | not JSON, or no `installed` field → `not_nextcloud`; `installed: false` → `not_nextcloud`; `maintenance: true` → `maintenance`; `needsDbUpgrade: true` → `maintenance`; major < 30 → `server_too_old` (message names the found version); transport errors → keys of `map_curl_error` |
| Note | the answer body is capped at 64 KiB for this call; the `version` text is sanitized (digits and dots, 32 characters) before display |

## 2. Capabilities and account check

| | |
|--|--|
| Request | `GET <base>/ocs/v1.php/cloud/capabilities?format=json` with Basic credentials |
| OK | 200, `{"ocs":{"meta":{"statuscode":100,…},"data":{"version":{…},"capabilities":{…}}}}` |
| Fields used | `ocs.meta.statuscode` (100 = success), `ocs.data.version.string`, `ocs.data.capabilities.theming.name` (optional), `ocs.data.capabilities.dav` (presence only, informational) |
| Mapping | 401 → `auth_failed` (see the 2FA rule in research R-3), plus the distinct `auth_app_password_required` when the OCS `message` contains `app password` (case-insensitive; **guess**, LV-N2); 403 → `forbidden`; 429 → `rate_limited` with `Retry-After`; 503 → `maintenance`; 5xx → `server_error`; anything else, or no `ocs` object → `protocol_error` |
| Date | the HTTP `Date` header feeds `clock_skew` after every successful call of every kind |

## 3. Current user

| | |
|--|--|
| Request | `GET <base>/ocs/v1.php/cloud/user?format=json`, Basic |
| OK | 200, `ocs.data.id` (user id), `ocs.data.display-name` (optional, not stored) |
| Validation | `id`: 1–256 bytes, no `/`, no control characters, not `.` or `..`; otherwise `protocol_error` |

## 4. Quota and DAV availability

| | |
|--|--|
| Request | `PROPFIND <base>/remote.php/dav/files/<uid>/`, `Depth: 0`, `Content-Type: application/xml`, body asks `d:quota-available-bytes`, `d:quota-used-bytes` |
| OK | 207 |
| Mapping | 404, 405, 501 → `no_webdav`; 401 → `auth_failed`; 403 → `forbidden`; 207 without the properties → quota unknown (not an error) |
| Quota | `used` = `quota-used-bytes`; `available` ≥ 0 → `total = used + available`; a negative `available` is the server's code for unknown/unlimited: **-3 → unlimited, other negatives → unknown** (inferred, LV-N3) |

## 5. Folder listing

| | |
|--|--|
| Request | `PROPFIND <base>/remote.php/dav/files/<uid>/<path>/`, `Depth: 1`, `Content-Type: application/xml`, body below |
| OK | 207 multistatus |
| Never | `Depth: infinity`; a missing `Depth` header |

Body:

```xml
<?xml version="1.0" encoding="utf-8"?>
<d:propfind xmlns:d="DAV:" xmlns:oc="http://owncloud.org/ns" xmlns:nc="http://nextcloud.org/ns">
  <d:prop>
    <d:getetag/> <d:getlastmodified/> <d:getcontentlength/> <d:getcontenttype/> <d:resourcetype/>
    <oc:fileid/> <oc:permissions/> <oc:size/> <oc:favorite/> <oc:checksums/> <nc:is-encrypted/>
  </d:prop>
</d:propfind>
```

Parsing rules (`nc_listing.cpp`):

1. `parse_multistatus` (root must be `d:multistatus`, else `malformed_xml`).
2. For each `d:response`: decode `d:href`; if it does not start with `/remote.php/dav/files/<uid>/` it is counted as
   `skipped_foreign` and dropped. Remove the prefix and the trailing `/`; compare to the requested path to find "the
   folder itself" (dropped, its `getetag`/`oc:fileid` update the `folders` row). Anything whose parent is not the
   requested path is dropped (`skipped_foreign`).
3. Name = last segment, validated per data-model (empty, `.`, `..`, `/`, control characters, over 1,024 bytes →
   `skipped_hostile`).
4. Properties are read from the first `propstat` with a 2xx status. A `d:response` with no 2xx `propstat` becomes an
   entry with `availability = unreadable`, `reason = status_<code>`. A 404 `propstat` for a single property is not an
   error (property absent).
5. `kind`: `d:resourcetype` containing `d:collection` → folder. `size`: `oc:size` if present and ≥ 0, else
   `getcontentlength`, else NULL. `mtime`: `parse_http_date(getlastmodified)`, NULL if unparsable. `etag` stored exactly
   as sent. `oc:favorite` `1` → favorite. `oc:checksums` text stored (≤ 256 bytes).
6. Availability: `nc:is-encrypted` = `1` → `encrypted` (reason `end_to_end_encrypted`); `oc:permissions` present, non-empty
   and without `G` → `no_permission` (reason `no_read_permission`); `unsupported` for responses that mark a mount as
   unavailable (an `oc:permissions` of empty text on a mounted entry, or a 2xx `propstat` that lacks `getetag` and
   `resourcetype` both: reason `unsupported_state`).
7. More than 10,000 `d:response` elements → stop, `listing_too_large` (the count so far is reported).
8. A response in which some entries failed and others succeeded is **not** an error: the good entries are stored and
   the bad ones are marked or skipped, `partial = true` is reported to the UI.

Errors: 404 → the folder is `gone`; 401 → `auth_failed`; 403 → `forbidden`; 429/503 as in section 2; 400 or 403 with a
body naming the depth → `protocol_error`; 5xx → `server_error`.

## 6. Download

| | |
|--|--|
| Request | `GET <base>/remote.php/dav/files/<uid>/<path>` |
| Resume | `Range: bytes=<n>-` and `If-Range: <strong ETag>` (only when the stored ETag has no `W/`) |
| Never | `If-Match` / `If-None-Match` (the transfer is not a conditional write); `Accept-Encoding` is set to identity for file bodies so that `Content-Length`, `Range` and the stored size mean the same bytes (**the existing transport asks libcurl for any encoding; the file transfer overrides it, task in the plan**) |
| OK | 200 full body; 206 partial body |
| Reads | `Content-Length`, `Content-Range` (`bytes <a>-<b>/<total>`, `a` must equal our offset), `ETag` of this response (stored as `expected_etag` for the next resume if strong) |
| Errors | 401 `auth_failed`; 403 `forbidden`; 404 `not_found` (file removed: `server_missing` set on the `local_files` row if any); 416 → restart once; 423 → `locked` (try again later); 429 `rate_limited`; 503 `maintenance`/`server_error`; connection loss → `waiting` (research R-8) |

## 7. Favorites

| | |
|--|--|
| Request | `REPORT <base>/remote.php/dav/files/<uid>/`, `Content-Type: application/xml`, `Depth` not sent |
| OK | 207, parsed by the listing parser with the parent check disabled (entries from many folders) |

Body (**shape inferred** from the API notes, LV-N8):

```xml
<?xml version="1.0" encoding="utf-8"?>
<oc:filter-files xmlns:d="DAV:" xmlns:oc="http://owncloud.org/ns" xmlns:nc="http://nextcloud.org/ns">
  <d:prop> …the property list of section 5… </d:prop>
  <oc:filter-rules><oc:favorite>1</oc:favorite></oc:filter-rules>
</oc:filter-files>
```

The result is capped at 2,000 entries (more: the first 2,000 are kept and the page says so).

## 8. Server search

| | |
|--|--|
| Request | `SEARCH <base>/remote.php/dav/`, `Content-Type: text/xml` |
| OK | 207, parsed like section 7 |

Body (**shape inferred**, LV-N9); `<term>` is XML-escaped and has `%`, `_` and `\` removed; empty after that → no request:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<d:searchrequest xmlns:d="DAV:" xmlns:oc="http://owncloud.org/ns">
  <d:basicsearch>
    <d:select><d:prop><d:displayname/><d:getcontenttype/><d:getlastmodified/><d:getetag/>
      <d:resourcetype/><oc:size/><oc:fileid/><d:getcontentlength/></d:prop></d:select>
    <d:from><d:scope><d:href>/files/<uid></d:href><d:depth>infinity</d:depth></d:scope></d:from>
    <d:where><d:like><d:prop><d:displayname/></d:prop><d:literal>%<term>%</d:literal></d:like></d:where>
    <d:orderby><d:order><d:prop><d:getlastmodified/></d:prop><d:descending/></d:order></d:orderby>
    <d:limit><d:nresults>101</d:nresults></d:limit>
  </d:basicsearch>
</d:searchrequest>
```

(`nresults` is one more than the 100 results the app shows: the extra one tells that more exist.) (`d:depth infinity` here is the search request's own element, as in the API notes, not a `PROPFIND` header.)

## 9. Login Flow v2

| Step | Request | Handling |
|------|---------|----------|
| Start | `POST <base>/index.php/login/v2`, no credentials, empty body | 200 JSON `{"poll":{"token","endpoint"},"login"}`. Both `endpoint` and `login` must be `https` and have the **same origin** as `<base>` (else `redirect_other_host`). `token` is 1–512 printable ASCII characters. Held in memory only. |
| Check | `POST <endpoint>`, `Content-Type: application/x-www-form-urlencoded`, body `token=<token>` | 404 → "not approved yet", flow stays open; 200 JSON `{"server","loginName","appPassword"}` → `server` must be https and the same origin as `<base>` (the canonical address is **taken from `server`** only after that check); store and discard the token; any other status → `login_failed` and the flow ends |

## Error keys (stable, shown by `qsTr` in the UI)

`scheme_not_allowed`, `address_invalid`, `clock_implausible`, `no_network`, `server_unreachable`, `timeout`,
`cert_untrusted`, `cert_expired`, `cert_host_mismatch`, `not_nextcloud`, `maintenance`, `server_too_old`,
`auth_failed`, `auth_app_password_required`, `no_webdav`, `forbidden`, `not_found`, `rate_limited`, `server_error`,
`protocol_error`, `malformed_xml`, `listing_too_large`, `redirect_other_host`, `login_failed`, `login_expired`,
`too_large_for_storage`, `not_enough_space`, `storage_readonly`, `storage_full`, `checksum_mismatch`, `size_mismatch`,
`local_changed`, `locked`, `path_too_long`, `cancelled`, plus the parser-level keys `foreign_href` (a `d:href` of another account: counted and dropped) and `qr_too_long`, and the local keys `connection_lost` (a connection that ended early), `path_unsafe` (a link or a file where a folder is needed in the download folder) and `write_failed` (the card could not be written). The first group is `Error::key` of the core; the UI has one
message and one "what to do" line per key, in English with a German translation, and the message never contains the
address, user name or any name from the server.
