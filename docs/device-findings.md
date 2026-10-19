# Device findings

Template. Facts observed on the PocketBook InkPad One. Everything not listed here is **unverified** (constitution,
Principle IX). Each entry has a date and the way it was observed.

PB Cloud has not been run on the reader yet: it has no findings of its own. The facts below are device facts that
were observed with the probe of the sibling app office-pocketbook (PB Office 0.1.0, the probe started through the launcher's
`run-probe` flag, the reader not on Wi-Fi). They describe the reader, not this app, and are carried over as the
starting point. Run the probe of this app (`scripts/probe-device.sh`) and record its own output below.

## Inherited device facts (observed 2026-10-14)

| Item | Result | Status |
|---|---|---|
| libcurl on the reader | 8.12.0 (OpenSSL 3.5.4); the protocol list includes `https` | Protocol list **verified** on the device. The app carries its own static libcurl 8.18.0 (`scripts/build-curl-arm.sh`); that was decided because the reader's libcurl drops the contents of IMAP literals, which PB Cloud does not use. Whether the reader's libcurl is good enough for HTTPS/WebDAV is **unverified**; the minimum version in `core/include/pbc/curl_session.h` stays until the first plan decides |
| `chmod` / protected location | the data folder is **vfat**; `chmod 0600` is **not observable** (the mode reads back 0777) | **Verified.** The credentials file cannot be made private on this storage; the in-app text must say so. No protected location was found |
| `/etc/localtime`, `TZ` | `TZ` is unset; `/etc/localtime` is a link to a relative target named `timezone` | **Partly open.** Must not be relied on: the app uses bundled tzdata |
| gconv modules | `/usr/lib/gconv` present; iconv to UTF-8 works for the 16 charsets probed (ISO-8859-1/2/15, Windows-1250/1251/1252, KOI8-R, ISO-2022-JP, Shift_JIS, EUC-JP, GB2312, GBK, Big5, EUC-KR, UTF-7, UTF-16LE) | **Verified** for this firmware |
| System CA store | `/etc/ssl/certs/ca-certificates.crt` with 150 certificates; the bundled CA file in the package stays as the fallback | No HTTPS handshake was tested (no network) |

### Other device facts

- Kernel `5.10.226pocketbook aarch64` with a 32-bit ARM user space (the app and the probe are armhf binaries).
- Memory: `MemTotal` 1,987,872 kB.
- Frame buffer: 1872 x 1404 pixels as reported by sysfs (the app's Qt window is 1404x1872 portrait).
- **Qt on the device is 6.10.3**; the SDK sysroot is Qt 6.8.2. An app built against the SDK runs on it, but anything Qt-version-sensitive is device-unverified.
- QML modules present on the device include `QtWebView`/`QtWebEngine`; the PB apps never use them (rule U5 of `scripts/check-ui-style.sh`).
- QPA platform plugins present: `linuxfb`, `minimal`, `offscreen`, `pocketbook2`, `pocketbook2egl`, `vnc`.

## PB Cloud findings

None yet.
