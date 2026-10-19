# Test fixtures

Fixtures are hand-written or anonymized samples of what the app parses. They MUST be anonymized (constitution Principle IV) before they
are committed; `scripts/check-fixtures.sh` (run by CTest) enforces the mechanical rules.

## Layout

| Directory | Content |
|---|---|
| `malformed/xml/` | hostile or broken XML inputs for the "never crash" tests of the minimal XML parser |

Samples of server answers (Nextcloud WebDAV, OCS, login flow) are added with the features that parse them.

## Anonymization procedure

| Real data | Replace with |
|-----------|--------------|
| server and host names | `example.org`, `cloud.example.org` (RFC 2606 names only) |
| user names | `alice`, `bob` |
| e-mail addresses | `alice@example.org` |
| subjects, bodies, event titles | generated placeholder text |
| ETags, tokens | freshly generated hex strings |
| `Authorization` headers or values | remove completely |

## Naming

A fixture recorded from a live server is named `<name>.txt` (or `.eml`, `.ics`, `.xml`). A fixture written by
hand from the protocol documents, before it could be compared with a server, carries the suffix `.unverified`
before the extension (for example `list_basic.unverified.txt`). The suffix is removed only after a recording from
a real server matches it. Fake-server and fixture behaviour is never evidence about a real provider (Principle IX).

## Checks

- `scripts/check-fixtures.sh` fails on hosts other than the reserved example names, `localhost`, `127.0.0.1`
  or `*.invalid`, on e-mail addresses outside the reserved example domains, and on any `Authorization` value.
- `scripts/check-fixtures.sh --self-test` proves that the checker rejects bad fixtures.
