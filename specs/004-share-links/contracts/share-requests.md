# Contract: Share requests to the Nextcloud server

**Feature**: [../spec.md](../spec.md) | **Research**: [../research.md](../research.md) | **001**: [server-requests](../../001-nextcloud-files-client/contracts/server-requests.md) | **002**: [server-requests](../../002-manage-server-files/contracts/server-requests.md)

**Status of every row: UNVERIFIED** (constitution IX). Assembled from the public OCS Share API documentation and behavior-only
descriptions; nothing is copied from Nextcloud sources. A row becomes verified only through an entry in
`docs/live-server-checklist.md` (LV-L1 ... LV-L10). The contract tests run against the in-process fake in
`tests/support/fake_nextcloud_server.h`, which implements *this file*; a passing test proves that the app follows the contract, not
that a real server does.

## Conventions

* `<base>` is the connected server's canonical address (feature 001). `<ocs>` is `<base>/ocs/v2.php/apps/files_sharing/api/v1`.
* Every request carries `OCS-APIRequest: true`, `Accept: application/json`, `User-Agent: PBCloud/<version>`, the Basic credentials
  through the transport (never in a URL), and goes through `DavSession::send` (same-host rule; a redirect to another host is
  refused, as for writes in feature 002).
* `?format=json` is part of every URL. `<p>` is the item's server path (leading `/`, no trailing `/`) encoded with
  `remote_path::encode_path` and used as the value of the `path` parameter.
* Bodies of `POST` and `PUT` are `application/x-www-form-urlencoded`, built by `share_api.cpp` (`form_encode`: UTF-8, every byte
  except `A-Z a-z 0-9 - . _ ~` percent-encoded, space as `%20`, never `+`). **A password appears only in such a body.**
* Answers are read with a cap of **2 MiB** and parsed with `JsonLimits{2 MiB, depth 16, nodes 40000}`. The envelope is
  `{"ocs":{"meta":{"status","statuscode","message"},"data":...}}`. A request succeeded when the HTTP status is 2xx **and** `statuscode`
  is `100` or `200`; the `message` member is never read.
* Timeouts: connect 10 s, total 30 s. No retry of any request by the app (a user tap is the only way to send again).
* Methods added to those of 001/002 for this feature: `POST` (not a rule-N2 verb), `PUT` and `DELETE` on `<ocs>/shares[/<id>]`
  **only in `core/src/share_api.cpp`**, which is added to the audited modules of rule N2. `PROPPATCH`, `LOCK`, `UNLOCK` remain
  forbidden everywhere.
* No request is made except as the consequence of a tap on a sharing screen (Share, Refresh, a link row, Create, Save, Remove).
  Create, Change and Remove are sent only after their confirmation.

## L1. Sharing rules

`GET <base>/ocs/v1.php/cloud/capabilities?format=json` (the request of the account check, repeated when Share opens and once more
while diagnosing a refusal). Read under `ocs.data.capabilities.files_sharing`:

| JSON path | Use |
|-----------|-----|
| `api_enabled` (bool) | false: sharing not offered |
| `public.enabled` (bool) | false: public links not allowed |
| `public.password.enforced` (bool); `public.password.enforced_for.read_only` / `read_write` / `upload_only` (bool) | password required (per permission where present) |
| `public.expire_date.enabled` / `.enforced` (bool); `.days` (integer) | expiry offered / required / maximum distance in days |
| `public.upload` (bool); `public.upload_files_drop` (bool) | read-and-upload / upload-only allowed |
| `resharing` (bool) | read, unused |

A missing, mistyped or out-of-range member is "unknown". At most 64 members are read per object; strings are not read at all.
**The exact keys differ by version and are unverified (LV-L1).**

## L2. Shares of the item

`GET <ocs>/shares?path=<p>&reshares=true&format=json` -> `data` is an array of share objects. Members read (others ignored):

| Member | Meaning |
|--------|---------|
| `id` (string or integer) | share id, > 0 |
| `share_type` (integer) | 0 user, 1 group, 3 public link, other values -> `Other` |
| `permissions` (integer) | bit set; see data-model "ShareKind and LinkPermission" |
| `expiration` (string or null) | `YYYY-MM-DD hh:mm:ss`; only the date part is read |
| `uid_owner` (string) | compared with the signed-in user id for `owned_by_me` (never stored) |
| `share_with_displayname` (string) | users and groups: shown name (sanitized) |
| `label` (string) | links: optional name (sanitized) |
| `password` / `share_with` | **presence only**, to compute `has_password` (R-5); the value is not copied out |
| `url`, `token` | **not read in the list** (R-5); read only by L4 |

More than 500 rows: the first 500 are kept, the screen says so (`share_list_too_big`). A non-array `data`, an object without `id`, or a
row of a type the parser cannot read is dropped and counted (a count is logged as `share.open` field `skipped`).

## L3. Shares inherited from a parent folder

`GET <ocs>/shares/inherited?path=<p>&format=json` -> array of share objects read like L2, marked `inherited`. `404`/`405`/`501` mean
"not offered" (no inherited rows, no error shown). Any other failure is shown as a note on the list ("Shares from parent folders could
not be read"); the rest of the list is shown. **Availability is unverified (LV-L4).**

## L4. One share (detail and pre-check)

`GET <ocs>/shares/<id>?format=json` -> `data` is an array with one share object (or an object; both are read). This is the **only**
parser that reads `url` (the link) and it fills `ShareDetail::link` subject to the checks of data-model "ShareDetail". `404` means the
share is gone (`share_gone`).

## L5. Create and change a public link

Create: `POST <ocs>/shares?format=json`

```
path=<p raw>&shareType=3&permissions=<1|15>[&password=<secret>][&expireDate=YYYY-MM-DD]
```

`<p raw>` is the path form-encoded (not the URL-path encoding). `permissions` is `1`, or `15` for a folder with upload. A file never
gets `15`. No other member is sent (R-2).

Change: `PUT <ocs>/shares/<id>?format=json`, body with **one** of:

| Action | Body |
|--------|------|
| set or replace password | `password=<secret>` |
| remove password | `password=` |
| set or change expiry | `expireDate=YYYY-MM-DD` |
| remove expiry | `expireDate=` |
| change permission | `permissions=<1|15>` |

Before a change the app runs L4 and compares kind, permission bits, expiry and `has_password` with the screen's snapshot (R-7); on a
difference no `PUT` is sent. The answer to `POST`/`PUT` is the share as stored (read like L4); the screen shows that, and the app
re-reads L2 afterwards. **That empty values clear a field is unverified (LV-L7).**

## L6. Remove a link

`DELETE <ocs>/shares/<id>?format=json` -> success when 2xx and `statuscode` 100/200. `404` -> `share_gone` (shown as "already
removed", not as a failure). Nothing else is sent.

## Status and answer mapping

| Situation | Key | Notes |
|-----------|-----|-------|
| transport failed, `delivery` = NotSent | the transport's key (`no_network`, `timeout`, `cert_untrusted`, ...) | plain failure; entries kept |
| transport failed, `delivery` = PartlySent/FullySent, no answer | `share_unknown_outcome` | R-8; never repeated by the app |
| 401 / revoked app password | `auth_failed` | "Sign in again" as feature 001 |
| 429 / 503 | `rate_limited` / `maintenance` | as 001; no retry |
| 2xx but `statuscode` not 100/200 | handled as the matching 4xx below by `statuscode` | |
| 400, 403, 404 of L2/L5 | `diagnose_refusal` (R-4): a violated rule, else (after one `PROPFIND` `Depth: 0` of the item) `share_item_gone` for a 404 or `share_item_not_shareable`, else `share_refused` + code | one extra L1, and for 403/404 of the path one `PROPFIND` of feature 001 |
| 403 of L5 (`PUT`) / L6 on a share the user does not own | `share_not_changeable` | |
| 404 of L4/L5(`PUT`)/L6 | `share_gone` | |
| `api_enabled` false or 404/501 of the whole API | `share_not_offered` | Share shows "This server does not offer sharing" |
| any other status | `share_refused` with `E<status>` | no server text |
| answer larger than the cap / malformed JSON / wrong envelope | `protocol_error` | list: nothing shown from it |

## Keys that must never appear in any message, log or record

The path, the item name, the link, the token, the password, the user id, the server address, `meta.message`, any body text.
`tests/integration/test_share_requests.cpp` asserts the exact method, URL, headers and body of every request above; `test_share_privacy`
asserts the absence rules.
