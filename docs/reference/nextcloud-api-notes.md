# Nextcloud server contract: API notes for PB Cloud

> **Status of everything in this file: UNVERIFIED.** Nothing here has been exercised against a real Nextcloud
> server. It is assembled from public documentation and RFCs. A statement changes status only when
> `docs/live-server-checklist.md` has an entry with a date and a target. Fake servers in the tests prove nothing
> about a real server.
>
> **Sources.**
> - Nextcloud Developer Manual (the "latest" build, which showed itself as the Nextcloud 36 manual when read on
>   2026-10-19): *WebDAV basics*, *Chunked file upload*, *Trashbin*, *Versions*, *Search*, *Login Flow*, *OCS API
>   overview*, *OCS Share API*; the Nextcloud User Manual page on WebDAV access; the Nextcloud Administration Manual
>   pages on release schedule and brute-force protection.
> - The documentation of the `files_lock` app (README in its repository), for file locking.
> - RFC 4918 (WebDAV), RFC 7232 (conditional requests), RFC 7233 (range requests), RFC 9110 (HTTP semantics, for
>   `Retry-After` and status codes).
> - Where a page does not say something, this file says "the page is silent" and gives the RFC rule or my
>   inference, marked **(inferred)**.
> - For how a mature client uses these APIs see `docs/reference/nextcloud-android-features.md`.
>
> **Reading the tables.** "Req" = request headers that matter; "Props" = properties to ask for; "OK" = success status;
> "Err" = errors to handle. Namespaces: `d` = `DAV:`, `oc` = `http://owncloud.org/ns`,
> `nc` = `http://nextcloud.org/ns`. `<user>` is the **user id**, which can differ from the login name (section 2.3).
> `<id>` is a numeric file id.

---

## 1. Base URLs and general rules

| Purpose | Path pattern | Notes |
|---------|--------------|-------|
| Files of a user | `/remote.php/dav/files/<user>/<path>` | The one path to use. Every path segment is percent-encoded; folders end in `/` in listings. |
| Legacy files root | `/remote.php/webdav/<path>` | The developer-manual and user-manual pages read for this document do not mention it at all. Treat it as *not supported* by PB Cloud; do not build on it. **(unverified; possibly still served by the server for old clients)** |
| Uploads (chunked) | `/remote.php/dav/uploads/<user>/` | Section 6. |
| Trash bin | `/remote.php/dav/trashbin/<user>/trash` and `.../restore` | Section 8. |
| Versions | `/remote.php/dav/versions/<user>/versions/<id>` and `.../restore` | Section 8. |
| Public shares | `/public.php/dav/files/<share-token>` (since 29) | Not needed. |
| OCS | `/ocs/v2.php/...` (also `/ocs/v1.php/...`) | Section 3. |
| Status document | `/status.php` | Section 3. |
| Login Flow v2 | `/index.php/login/v2` | Section 4. |

**Authentication.** HTTP Basic with `<loginName>:<appPassword>`. The manual says Basic auth with an *app password* is the
way for any client when the server uses two-factor authentication or an external auth provider; it also recommends app
passwords for speed. Never put credentials in a URL (the project constitution forbids it too).

**Always send** `User-Agent` with the app name and version (a server names the app password after it **(inferred from
the legacy `getapppassword` description)**). For OCS requests add `OCS-APIRequest: true` and
`Accept: application/json`.

**TLS.** HTTPS only; verification always on (constitution). A private authority goes in `custom-ca.pem`.

**Redirects.** Follow redirects only on `https` and only to the same host; never forward credentials to another host.
A PROPFIND to a `/.well-known/` URL that answers 301/302/307/308 must stay a PROPFIND with the same body (a redirect
that turns it into a GET is a bug). RFC 9110: 307/308 keep method and body; 301/302 historically do not. **(unverified
for Nextcloud; the existing `curl_transport` was written with this in mind)**

**Rate limits.** See section 9.

---

## 2. The things every request depends on

### 2.1 ETags

- `d:getetag` is a *quoted* string in the XML text, for example `"5f3c0a..."` (the quote characters are part of the
  value). Compare ETags as opaque strings. **Do not strip or add quotes** when sending them back in `If-Match` /
  `If-None-Match`: send exactly what the server gave you, quotes included (RFC 7232 requires the quotes).
- A `W/` prefix marks a weak ETag. Weak ETags are legal in `If-None-Match` (weak comparison) and **never** match in
  `If-Match` (strong comparison required). Nextcloud normally returns strong ETags; a reverse proxy that compresses
  responses can change a strong ETag into a weak one or append a suffix such as `-gzip`. **(unverified)** Rule: store
  the ETag as received; if it starts with `W/`, never rely on it for conditional writes, and log a warning.
- Folder ETags change whenever anything below the folder changes (documented behavior of the server's ETag
  propagation; **unverified** against a real tree). `nc:metadata_etag` changes when only metadata changed (the page
  lists it; exact use not described).
- `PUT`, `MKCOL`, `MOVE` and `COPY` answers carry `OC-Etag` and `OC-FileId` (page). The `ETag` header of a `PUT` answer
  may or may not be present **(unverified)**; use `OC-Etag` first, and fall back to a `PROPFIND` depth 0.

### 2.2 File ids and permissions

- `oc:fileid` is the numeric file id (stable across rename and move within the same storage). `oc:id` is also
  listed on the page; its form is not described. Based on other clients it is the file id padded to eight digits plus
  an instance id string **(inferred; do not parse it)**. Use `oc:fileid` for the versions URL.
- `oc:permissions` is a string of letters. The page lists `S R M G D N V W C K`. Meaning by long-standing convention:
  `S` shared, `R` may reshare, `M` mounted (external storage or share mount), `G` readable, `D` deletable, `N`
  renameable, `V` movable, `W` updateable (files), `C` may create files in a folder, `K` may create folders.
  **(inferred; the page gives the letters, not the meanings.)** Public shares remove `S` and `M` (page). Use the letters
  to hide buttons; never use them as a security check, the server decides.
- `oc:size` works for folders too (page); `d:getcontentlength` is files only.
- `oc:favorite` is `1` or `0`.

### 2.3 User id and login name

Login Flow v2 returns `loginName`. The Login Flow page says the login name can differ from the real user name and that
the real user id must be fetched with `GET /ocs/v1.php/cloud/user` (v1 form named on the page). WebDAV paths use the
**user id**. Store both: the login name for Basic auth, the user id for paths. **(unverified)**

### 2.4 URL encoding

Percent-encode each path segment individually (UTF-8 bytes, `%XX`); keep `/` between segments; encode space as `%20`
(never `+`). Folder names get a trailing `/` in listings; compare paths after decoding and after removing the trailing
slash. The `Destination` header of `MOVE`/`COPY` is a **full URL** (scheme, host, path), each segment encoded the same
way (page). Unicode normalization (NFC vs NFD) is not mentioned anywhere; do not normalize names, keep the bytes the
server sent. **(unverified)**

### 2.5 Time

`d:getlastmodified` is an HTTP date (RFC 9110 IMF-fixdate, always GMT), for example `Wed, 14 Oct 2026 12:00:00 GMT`; the
existing `parse_http_date` in `core/` reads this form. `X-OC-MTime` is an integer number of **seconds** since the epoch.

---

## 3. Server information, capabilities, minimum version

| Call | Path | Req | OK | Notes |
|------|------|-----|----|-------|
| Status | `GET /status.php` | none (no auth) | 200 JSON | Version and maintenance flags. The pages read for this document do not describe the fields. Known field names: `installed`, `maintenance`, `needsDbUpgrade`, `version`, `versionstring`, `edition`, `productname`, `extendedSupport`. **(unverified: field list from memory of the public document, not from a fetched page)** While in maintenance mode the server answers 503 on DAV/OCS paths **(inferred)**; `status.php` stays reachable and says `maintenance: true`. |
| Capabilities | `GET /ocs/v1.php/cloud/capabilities` (also v2) | auth, `OCS-APIRequest: true`, `Accept: application/json` | 200 | The OCS page documents the v1 path; the `version` block (`major`, `minor`, `micro`, `string`, `edition`, `extendedSupport`) and a `capabilities` block. |
| Current user | `GET /ocs/v1.php/cloud/user` (page names the v1 form) | same | 200 | `id` is the user id, plus display name. **(unverified: field names)** |

OCS envelope: `ocs.meta.status` (`ok`), `ocs.meta.statuscode` (`100` for success on v1 paths), `ocs.meta.message`,
and `ocs.data`. The v1 versus v2 difference in HTTP status mapping is not explained on the pages; prefer `v2` paths
where a page names them (sharing does) and treat `statuscode` as authoritative.

Capability keys PB Cloud wants (names inferred from what the Android client stores, from the chunking pages and the
`files_lock` documentation; **(unverified)** until read from a server):

| Key (JSON path under `ocs.data.capabilities`) | Use |
|------|-----|
| `files.versioning` | show a versions list |
| `files.undelete` | trash bin available |
| `files.locking` (a version string such as `1.0`) | show lock state; page for the `files_lock` app says to check that the key exists |
| `files.bigfilechunking` | legacy chunking flag; ignore, use `dav.chunking` |
| `dav.chunking` | chunked upload v2 available **(inferred)** |
| `files.chunked_upload.max_size`, `.max_parallel_count` | per a third-party client write-up, present from Nextcloud 31; earlier servers state nothing, and that client falls back to 10 MiB chunks, five in parallel **(unverified, third-party)** |
| `files_sharing.api_enabled`, `files_sharing.public.enabled` | show sharing |
| `core.pollinterval` | not needed |
| forbidden file names, characters, extensions, base names | validate names before sending (the Android client reads four lists and a "Windows-compatible names" flag; JSON paths **unverified**) |

### 3.1 Recommended minimum server version

**Support Nextcloud 30 and newer; develop and test against 33, 34 and 35.**

Reasons:
- The Nextcloud release table (administration wiki, read 2026-10-19) shows 33 (end of life 2027-02), 34 (2027-06) and
  35 (2027-09) as the maintained majors; 32 reached end of life in September 2026. A client that only works on
  maintained versions would shut out self-hosters who upgrade late, so allow two majors beyond that.
- 30 is the first version for which the Android client turns on the forbidden-names rules unconditionally (its own
  code comments say "Nextcloud 30 and 31: always on, 32 and later: controlled by a flag"). A floor of 30 lets PB Cloud
  rely on name-validation capabilities instead of finding out by failing uploads.
- Chunked upload v2, Login Flow v2, WebDAV `SEARCH`, trash bin and versions endpoints and `files_lock` all predate 30
  by several releases **(inferred; the pages do not give "since" versions except `public.php/dav` since 29 and
  `X-NC-WebDAV-Auto-Mkcol` since 32)**.
- The Android client's own constant is 20; that is a compatibility floor of a very large client, not a recommendation
  for a new one.

Rule: **gate features by capability, not by version.** Use the version only to produce a clear "server too old"
message below 30 and a soft warning on versions past end of life.

---

## 4. Login Flow v2 and app passwords

| Step | Method and path | Req | Result |
|------|-----------------|-----|--------|
| 1 Start | `POST /index.php/login/v2` | anonymous, empty body, `User-Agent` | 200 JSON: `login` (URL for a browser), `poll.token`, `poll.endpoint` (page example: `/login/v2/poll` on the server) |
| 2 Browser | open `login` in a browser | | The user signs in (any 2FA or SSO the server wants happens here) and grants access. |
| 3 Poll | `POST <poll.endpoint>` | form field `token=<poll.token>` | **404** until the user has granted access; **200** JSON `{server, loginName, appPassword}` — the page says it is returned **only once** |
| 4 Use | Basic auth with `loginName:appPassword` against `server` | | |

- The token is valid 20 minutes (page); the diagram shows an ephemeral session of 5 minutes after the grant. Poll about
  once per second per the diagram; the Android client polls every 30 s, which is gentler. For a battery- and
  refresh-limited reader poll every 5 seconds for at most 5 minutes while the login page is shown, then stop.
- Persist `server` (the canonical base URL, possibly different from what the user typed), `loginName` and
  `appPassword` immediately, in the credentials file; the answer cannot be fetched again.
- **The reader has no good browser.** Options (**inferred**, to be decided in the plan): show the `login` URL as text
  and as a QR code for a phone, then poll; and keep a manual entry (server address, login name, app password created
  in the web UI under personal security settings).
- **Revoke on logout:** `DELETE /ocs/v2.php/core/apppassword` with `OCS-APIRequest: true`, authenticated with the app
  password; 200 on success; on any other status still delete the account locally (page).
- **Login Flow v1** (older): open `/index.php/login/flow` in a web view with `OCS-APIRequest: true`; the result arrives
  as a redirect to `nc://login/server:<server>&user:<login>&password:<password>`; `+` means space in the encoded
  values. Not needed.
- **Legacy conversion:** `POST /ocs/v2.php/core/getapppassword` with real credentials returns an app password named
  after the user agent; it answers 403 when called with an app password. Not needed.
- The page says nothing about SSO or password confirmation; both happen inside the browser step.

---

## 5. Files over WebDAV

All paths are below `/remote.php/dav/files/<user>/`. All statuses in the "OK/Err" columns are from RFC 4918 and the
Nextcloud pages; **unverified** against a real server.

### 5.1 Listing and metadata: `PROPFIND`

| | Folder listing | One item |
|--|----------------|----------|
| Request | `PROPFIND <folder>/` | `PROPFIND <path>` |
| Req | `Depth: 1`, `Content-Type: application/xml`, auth | `Depth: 0` |
| Props | see below | same |
| OK | **207 Multi-Status** | 207 |
| Err | 401 bad credentials; 404 folder gone; 403 no access; 400/403 for a depth the server refuses; 405; 503 maintenance; 429 | same |

Request body to ask for what the app needs (my own example; extend as needed):

```xml
<?xml version="1.0" encoding="utf-8"?>
<d:propfind xmlns:d="DAV:" xmlns:oc="http://owncloud.org/ns" xmlns:nc="http://nextcloud.org/ns">
  <d:prop>
    <d:getetag/> <d:getlastmodified/> <d:getcontentlength/> <d:getcontenttype/> <d:resourcetype/>
    <oc:fileid/> <oc:permissions/> <oc:size/> <oc:favorite/> <oc:checksums/> <nc:has-preview/>
  </d:prop>
</d:propfind>
```

- Without a body or without `Depth`, the server returns a default property set (last modified, size, folder flag, ETag,
  MIME type) and, without `Depth: 0`, the children as well (page). **Always send `Depth` explicitly.**
- **`Depth: infinity`: never send it.** The page documents only `Depth: 0` and says nothing about `infinity`.
  RFC 4918 lets a server refuse infinite-depth listings (it should answer 403 with the
  `propfind-finite-depth` precondition); the working assumption of this project is that Nextcloud disables it. **(unverified for Nextcloud:
  no page found; confirm the answer code on a real server)** PB Cloud walks one level per request.
- Unknown properties come back in a second `propstat` with `404 Not Found`; that is not an error (page: "reported as
  not found in the multi-status response").
- **207 with partial errors.** A multistatus holds one `d:response` per item and each has its own `d:status` (or
  per-`propstat` status). A 207 does not mean every part succeeded. For `PROPFIND` check the status of the `propstat`
  that holds the properties you need; for `MOVE`/`COPY`/`DELETE` on a folder (RFC 4918 section 9.6, 9.8, 9.9) a 207
  lists the members that failed — treat any `d:response` with a status outside 2xx as a failure of the operation as a
  whole and refresh the folder afterwards.
- The first `d:response` of a `Depth: 1` listing is the folder itself; the children follow. Match entries by the
  decoded `d:href`, not by position **(inferred)**.
- `d:href` values are server paths (`/remote.php/dav/files/<user>/...`), percent-encoded. Strip the
  `/remote.php/dav/files/<user>` prefix to get the app-relative path.
- `oc:checksums` returns the stored checksums (type-prefixed values) if the server has any; it can be empty.

### 5.2 Download: `GET`

| | |
|--|--|
| Request | `GET <path>` |
| Req | auth; optional `If-None-Match: <etag>` (304 when unchanged); optional `X-Hash: sha256` (page: answers `X-Hash-SHA256`; also `md5`, `sha1`, `all`) |
| OK | 200 with body; **304** for a matching conditional |
| Err | 404; 403; 401; 423 (locked, if the file is mid-edit); 503; 429 |

- Take `ETag` (and `Last-Modified`) from the **response headers** of the `GET`: they describe exactly the bytes received
  and avoid a race with a concurrent change **(inferred; header presence unverified)**.
- **Resume (RFC 7233).** Send `Range: bytes=<n>-` with `If-Range: <strong etag>`; the server answers **206** with
  `Content-Range` when it can continue, **200** with the full body when the ETag no longer matches (start over), or
  **416** when the range is wrong. Whether Nextcloud honors `Range` and `If-Range` on file `GET` is **unverified**
  (the pages are silent); test early. If it fails, fall back to a full download into a fresh temp file.
- Download into a temp file in the same file system, verify the size against `Content-Length` (and the checksum if one
  was sent), then rename over the target.
- Folder download as an archive exists as a Nextcloud extension (`GET` on a folder with
  `Accept: application/zip` or `application/x-tar`); not needed. It fails when the sharer denied downloads (page).

### 5.3 Upload: `PUT` (files up to the chunking threshold)

| | |
|--|--|
| Request | `PUT <path>` with the raw body |
| Req | auth, `Content-Type`, `Content-Length`; **`X-OC-MTime: <seconds>`**; optional `X-OC-CTime: <seconds>`; optional **`OC-Checksum: <TYPE>:<hex>`** (for example `SHA256:...`); **`If-Match: <etag>`** to overwrite only if unchanged, or **`If-None-Match: *`** to create only if absent; optional `X-NC-WebDAV-Auto-Mkcol: 1` (NC 32+: create missing parents) |
| OK | **201** created, **204** (or 200) overwritten; headers `OC-Etag`, `OC-FileId`, `X-OC-MTime: accepted` when the server applied the time |
| Err | **412** the condition failed (someone changed it, or it already exists); 409 parent folder missing (without auto-mkcol); 403 forbidden name or no permission; 404 parent missing on some paths; **413** body too large (often the web server in front); **423** locked; **507** quota; 401; 503; 429 |

Gotchas:
- **A plain `PUT` overwrites silently** (page: "overwrites existing files without warning"). A client that does not send
  `If-Match` or `If-None-Match: *` can destroy a newer server version. PB Cloud sends one of the two on **every** file
  `PUT`.
- `X-OC-MTime` is applied only if the server accepts it; look for the answer header `X-OC-MTime: accepted`. If it is
  missing, the file has the upload time; the next listing then shows a different `getlastmodified` than the local
  file. Do not treat that as a conflict by itself: compare ETags, not mtimes.
- `OC-Checksum` is stored but **not validated on a regular `PUT`** (page; bulk upload does validate). So the
  checksum protects a later download comparison, not the upload itself. Compute the SHA-256 while reading the file
  once; do not read the file twice on a slow device.
- Do not send `OC-Chunked` (deprecated, page).
- A `PUT` to a locked file fails with 423 **(files_lock documentation)**.

### 5.4 Create folder: `MKCOL`

`MKCOL <path>/` → **201**. Errors: **405** already exists; **409** parent missing; 403; 423; 507; 401; 503. A folder that
already exists is often *not* an error for a "create folder, ignore if there" flow (the Android client adopts the
existing folder). Add `X-NC-WebDAV-Auto-Mkcol: 1` only when the server is 32 or newer **(page)**.

### 5.5 Delete: `DELETE`

`DELETE <path>` → **204** (some servers 200). Folder deletes are recursive (page). A 207 lists members that could not be
deleted. With the trash bin app the item moves to the trash instead of vanishing. **404** means already gone: treat as
success for the purpose of the app. Send `If-Match: <etag>` when you hold one, so you do not delete a version you have not
seen **(RFC 7232; whether the Nextcloud DAV server enforces `If-Match` on `DELETE` is unverified)**. Errors: 403,
423, 412, 401, 503, 429.

### 5.6 Rename and move: `MOVE`; copy: `COPY`

| | |
|--|--|
| Request | `MOVE <path>` / `COPY <path>` |
| Req | auth; **`Destination: <full URL>`** (page: a relative path is not enough); **`Overwrite: F`** (page: the default is `T`, which silently replaces the target) |
| OK | **201** created at the target, **204** replaced an existing target (only possible with `Overwrite: T`) |
| Err | **412** the target exists and `Overwrite: F`; **409** the target's parent is missing; **403** forbidden or the target is inside the source; **423**; **507**; 404 source gone; 207 partial |

Always send `Overwrite: F` and let the user decide on a collision. Rename is a `MOVE` within the same folder
**(inferred)**. A `MOVE` keeps `oc:fileid`; a `COPY` creates a new id.

### 5.7 Favorites: `PROPPATCH`, `REPORT`

- Set: `PROPPATCH <path>` with `<oc:favorite>1</oc:favorite>` (or `0`) inside `d:set/d:prop` (page). **207** with a
  `propstat` status 200 on success.
- List: `REPORT` on `/remote.php/dav/files/<user>/` with an `oc:filter-files` body and `<oc:favorite>1</oc:favorite>`
  in the filter rules, asking for the same properties as a listing. The page says the filter is recursive within the
  target folder, so the user's root returns all favorites. **(body shape inferred from the page's description; verify)**

### 5.8 Search: `SEARCH`

`SEARCH /remote.php/dav/` with `Content-Type: text/xml` and a `d:searchrequest` / `d:basicsearch` body (select, `from`
scope, `where`, `orderby`, `limit`). The scope must begin with `files/<user>`; only files are searchable (page).
Searchable: `{DAV:}displayname`, `{DAV:}getcontenttype`, `{DAV:}getlastmodified`, `{DAV:}creationdate`,
`{http://nextcloud.org/ns}upload_time`, `{http://owncloud.org/ns}size`, `{http://owncloud.org/ns}favorite`,
`{http://owncloud.org/ns}fileid`. Name search: `d:like` on `displayname` with `%term%` **(inferred; the page has no
example)**. Recent files: `d:gt` on `getlastmodified` with an ISO 8601 time, ordered descending with a `limit` (page).
Page examples use `d:depth` `infinity` **inside the search scope** — that is the search request's own element, not a
`PROPFIND` header, but treat it as **unverified** whether a given server limits it. HTTP status codes are not documented;
expect 207 on success and 4xx/5xx errors as for PROPFIND.

### 5.9 Bulk upload

The manual has a "bulk upload" page; PB Cloud does not need it (many small files at once). Not read.

---

## 6. Chunked upload v2

Use when the file is larger than the chunk size the server allows to be sent in one request or larger than your own
threshold (see section 3 for the capability). Base: `/remote.php/dav/uploads/<user>/`.

| Step | Request | Req | OK |
|------|---------|-----|----|
| 1 | `MKCOL /remote.php/dav/uploads/<user>/<upload-id>` (a random UUID is enough) | `Destination: <full URL of the final file>` | 201 |
| 2 | `PUT .../<upload-id>/<chunk-number>` (numbers `1`..`10000`; the order of the names is the assembly order) | `Destination` (every request), `OC-Total-Length: <total bytes>` | 201 |
| 3 | `MOVE .../<upload-id>/.file` | `Destination: <final URL>`, `OC-Total-Length`, optional `X-OC-Mtime: <seconds>` | 201 (new) / 204 (replaced) |
| Abort | `DELETE .../<upload-id>` | none (the destination was stored at step 1) | 204 |

Rules from the page:
- Chunk size 5 MB to 5 GB, only the last chunk may be smaller. At most 10000 chunks.
- `OC-Total-Length` on the chunk `PUT`s makes the server check the quota early: **507** at the first chunk instead of
  a failure at the final `MOVE`.
- An upload folder expires after 24 hours of inactivity; the upload then fails.
- Chunks cannot be downloaded from the upload folder.
- Version 2 supports S3-style multipart targets; an abort also cancels the storage-level upload.

Not on the page (everything below **unverified**):
- Which of `If-Match`, `If-None-Match`, `Overwrite`, `OC-Checksum` the final `MOVE` honors. Plan: send `If-Match` /
  `If-None-Match: *` on the final `MOVE` exactly as on a plain `PUT`, and check the outcome against a real server;
  if the server ignores them, the safe fallback is a `PROPFIND` of the target's ETag immediately before the `MOVE`.
- Resume: an interrupted upload can continue by listing the upload folder (`PROPFIND` depth 1) to see which chunks the
  server already has, then sending the missing ones. The page does not describe the listing. **(inferred from RFC 4918
  and the folder-based design)**
- The status codes beyond 507.
- Public-share chunking needs its own capability (`dav.public_shares_chunking`, per a third-party note); not relevant.
- The chunk size to choose on Wi-Fi: the Android client chooses by connection type and the server's maximum; a
  PocketBook port should start with the server maximum when given, else 10 MiB, and keep one chunk in flight.

---

## 7. Sharing (OCS Share API)

Base `/ocs/v2.php/apps/files_sharing/api/v1`; every call needs `OCS-APIRequest: true`; ask for JSON with
`Accept: application/json` (the page's examples show XML).

| Operation | Request | Notes |
|-----------|---------|-------|
| List shares of one path | `GET /shares?path=<path>` (+ optional `reshares`, `subfiles`) | `path` relative to the user's files root |
| One share | `GET /shares/<id>` | |
| Create | `POST /shares` | `path`, `shareType`, `shareWith`, `permissions`, `password`, `expireDate` (`YYYY-MM-DD`), `publicUpload`, `note`, `label`, `attributes`, `sendMail`; the page says `POST` returns the new share id |
| Update | `PUT /shares/<id>` | `permissions`, `password`, `publicUpload`, `expireDate`, `note`, `attributes`, `sendMail` (`label` not listed) |
| Delete | `DELETE /shares/<id>` | |
| Send mail | `POST /shares/<id>/send-email` | `password` required for a protected share |

`shareType`: `0` user, `1` group, `3` public link, `4` e-mail, `6` federated cloud, `7` circle, `10` Talk. Permissions are a
bitmask: `1` read, `2` update, `4` create, `8` delete, `16` share, `31` all; the default is `31`, or `1` for public
shares (page). Attribute `permissions/download` set to `false` blocks downloads; since NC 30 an attribute's `value`
replaces the older `enabled`.

Statuses on the page: `100` success; `200` for send-email; `400` unknown share type or bad update parameters; `403` public
upload disabled by the admin, or not allowed to send the mail; `404` not found or the action failed.

Not on the page: the response fields of a share. The Android client stores the id, type, target, permissions, share
date, expiration date, token, link, label, note, password flag and download-limit fields, so the answer evidently
carries those **(inferred; field names unverified)**. For a public link the visible URL comes from the answer (a `url`
field) **(inferred)** or is built as `<server>/index.php/s/<token>` **(inferred)**.

PB Cloud scope: public links only (create with optional password and expiry, show the link, delete). No sharee search.

---

## 8. Trash bin, versions, locking, tags

### 8.1 Trash bin

| Operation | Request | Notes |
|-----------|---------|-------|
| List | `PROPFIND /remote.php/dav/trashbin/<user>/trash` (`Depth: 1`) | page: ordinary PROPFIND plus three properties in the `nc` namespace: `trashbin-filename`, `trashbin-original-location`, `trashbin-deletion-time` |
| Restore | `MOVE` the item's trash URL to `/remote.php/dav/trashbin/<user>/restore/<name>` | the page says "move into the restore folder" without naming the method or `Destination`; `MOVE` with a full-URL `Destination` is the WebDAV way **(inferred)** |
| Delete one permanently | `DELETE <item URL in trash>` | |
| Empty | `DELETE /remote.php/dav/trashbin/<user>/trash` | |

Status codes are not on the page. Expect 207 for the listing, 201/204 for restore, 204 for delete. **(unverified)**
Restoring onto a path where a file now exists: behavior not documented; check on a real server.

### 8.2 Versions

| Operation | Request | Notes |
|-----------|---------|-------|
| List | `PROPFIND /remote.php/dav/versions/<user>/versions/<id>` (`Depth: 1`) | `<id>` is `oc:fileid`; the page says each version is named by a timestamp; the properties returned are not listed |
| Restore | `MOVE` the version's URL to `/remote.php/dav/versions/<user>/restore/target` | the page says "move the version into the restore folder"; `target` as the fixed destination name is my recollection of other clients **(inferred; verify)** |
| Download one | `GET <version URL>` | not on the page **(inferred)** |

Show versions only if `files.versioning` is present. No labels, no deletion.

### 8.3 File locking (files_lock app)

Capability `files.locking` (a version string); `api-feature-lock-type: true` means the server accepts a lock type.
Three lock kinds: `0` user, `1` app (editors), `2` token (native WebDAV lock).

| Operation | How | Result |
|-----------|-----|--------|
| Lock as a user | `LOCK <path>` with header `X-User-Lock: 1` (optional `X-User-Lock-Type`, default 0) | 200 with lock properties; **423** with the existing lock's properties if another owner holds it |
| Unlock | `UNLOCK <path>` with the same header and type | 200; **412** not locked; **423** another owner |
| Native token lock | `LOCK` without the header, keep the returned `nc:lock-token`; `UNLOCK` needs it | token locks are the WebDAV standard way (RFC 4918 section 7) |

Properties (PROPFIND): `nc:lock` (boolean), `nc:lock-owner-type`, `nc:lock-owner`, `nc:lock-owner-displayname`,
`nc:lock-owner-editor`, `nc:lock-time`, `nc:lock-timeout` (seconds from creation; `0` = no expiry), `nc:lock-token`. Locks last
forever unless the admin set a timeout. The `d:supportedlock` and `oc:lockdiscovery` properties of the basic DAV page are
dummies that always return fixed values (page): do not use them.

PB Cloud: show the lock state in the details page and treat 423 on a write as "try later"; do not lock automatically.

### 8.4 System tags

No page for system tags was found in the developer manual. The server exposes tags under `/remote.php/dav/systemtags/`
**(inferred; unverified)**; not planned.

---

## 9. Errors, rate limits and maintenance

| Status | Where | What to do |
|--------|-------|------------|
| 207 | PROPFIND, PROPPATCH, REPORT, SEARCH; multi-member MOVE/COPY/DELETE | Read each member's status (section 5.1). |
| 304 | conditional GET | use the cache |
| 400 | malformed request; a depth the server refuses; invalid file name in some versions | fix the request; never retry blindly |
| 401 | bad or revoked credentials | stop, show "sign in again", run Login Flow v2 for the same account; do not retry in a loop (brute-force protection, below) |
| 403 | no permission; forbidden name; public upload disabled; infinite depth | show a specific message |
| 404 | gone | on a folder refresh: drop the cached folder; on delete: success |
| 405 | `MKCOL` on an existing name; method not allowed (a wrong base path, or a web-server rule) | |
| 409 | missing parent | create the parent, or refresh |
| 412 | `If-Match` / `If-None-Match` / `Overwrite: F` failed | the sync-conflict path; never overwrite automatically |
| 413 | request body too large (web server or PHP limit in front) | switch to chunked upload or tell the user the limit |
| 423 | locked | retry later; show the owner if known |
| 429 | rate limit or brute-force throttling | back off (below) |
| 500, 502, 504 | server or proxy trouble | bounded retry with backoff |
| 503 | maintenance mode or overload | read `status.php`; if `maintenance` is true show a maintenance message; otherwise back off |
| 507 | quota exceeded (also from the first chunk if `OC-Total-Length` was sent) | final; show the quota message |

**Rate limits and `Retry-After`.** The administration manual describes brute-force protection: repeated failed logins from one
address are slowed (delay capped at about 25 seconds), and after 10 recent failed attempts the server answers
**429** until the count drops (the manual's figures, read 2026-10-19). A successful login from the same address clears
the record. A general rate limiter (`ratelimit.protection.enabled`) also exists. **The pages read do not say that
`Retry-After` is sent**; RFC 9110 defines the header for 429 and 503 (seconds or an HTTP date). Rule: when present, honor it
(cap at 15 minutes); when absent, use exponential backoff (30 s, 60 s, 120 s, ... capped at 15 minutes) with jitter. After
**two** consecutive 401 answers stop all requests for the account and ask the user to sign in again: wrong passwords
are what triggers the throttling. **(unverified)**

**Maintenance mode.** The pages read do not describe it. Based on `status.php` and common client behavior: DAV and OCS
answer 503; `status.php` answers 200 with `maintenance: true`. A client should show "the server is in maintenance mode"
and retry only when the user asks. **(inferred)**

**Quota.** `PROPFIND` of the root with `d:quota-available-bytes` and `d:quota-used-bytes` (RFC 4331 properties)
gives the numbers **(inferred; not on the pages)**; a negative or special value can mean unlimited or unknown.

---

## 10. Summary of gotchas to carry into the design

1. Always send `Depth`; never `infinity`.
2. Weak and quoted ETags: store the string as sent; weak ones never satisfy `If-Match`.
3. Every file `PUT` carries `If-Match` or `If-None-Match: *`; every `MOVE`/`COPY` carries `Overwrite: F` and a full-URL
   `Destination`.
4. `oc:fileid` for versions; `oc:permissions` letters only for hiding buttons; do not parse `oc:id`.
5. `X-OC-MTime` in seconds; check for `accepted`; compare ETags, never mtimes, for conflicts.
6. `OC-Checksum` is stored, not validated on a plain `PUT`.
7. 207 can hide member failures.
8. URL-encode each segment; `Destination` is a full encoded URL; do not normalize Unicode.
9. User id (paths) differs from login name (auth).
10. App password is returned once.
11. Honor `Retry-After` when present; stop after two 401s.
12. No resume guarantees: verify `Range`/`If-Range` and chunk-folder listing on a real server before relying on them.
13. `remote.php/dav/files/<user>/` only; the legacy path is out of scope.

## 11. What to verify first on a real server (feeds `docs/live-server-checklist.md`)

1. `status.php`, capabilities (the JSON paths in section 3) and the version on a current major and on 30.
2. Login Flow v2 end to end, including a poll after the grant, and revocation.
3. `PROPFIND` depth 0 and 1 with the property set above; ETag quoting; weak ETag behind a proxy; folder ETag propagation
   after a change three levels down.
4. `PUT` with `If-Match` (match, mismatch → 412), with `If-None-Match: *` on an existing file, `X-OC-MTime` accepted,
   `OC-Checksum` stored.
5. `MOVE`/`COPY` with `Overwrite: F` onto an existing name (412), onto a missing parent (409).
6. `Range` / `If-Range` on `GET`.
7. Chunked upload v2: all four steps, an abort, a resume after cutting the connection, quota failure on the first chunk,
   headers honored on the final `MOVE`.
8. Trash list and restore; versions list and restore; a lock held by another client (423).
9. `Depth: infinity` answer code; 429 and `Retry-After` after wrong passwords on a throw-away account; maintenance
   mode answers.
