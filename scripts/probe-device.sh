#!/bin/sh
# Device probe for pbcloud (research R-02, R-06, R-11). Run on the InkPad One (see INSTALL.md, "Device probe")
# or on the host; paste the output into docs/device-findings.md.
#
#   sh probe-device.sh [pbc-probe-binary] [data-dir]
#
# Defaults: /mnt/ext1/applications/pbc-probe.bin and /mnt/ext1/system/pbcloud. The output contains no personal
# data (no host name, user name, home directory or address); tests/launcher/test_probe.sh checks that.
# TLS handshakes are always done with certificate verification ON.

PROBE="${1:-/mnt/ext1/applications/pbc-probe.bin}"
DATADIR="${2:-/mnt/ext1/system/pbcloud}"

echo "== system (shell)"
uname -s -r -m 2>&1     # not -a: that prints the host name
echo

echo "== pbc-probe"
if [ -x "$PROBE" ]; then
    "$PROBE" "$DATADIR" 2>&1
else
    echo "pbc-probe not found at the given path; the libcurl, charset, data directory and system facts are missing."
fi
echo

echo "== Qt runtime (R-2)"
ls /usr/lib/libQt6Core.so* /usr/lib/libQt6Quick.so* /usr/lib/libQt6Qml.so* 2>&1 | head -12
echo "-- QML modules in /usr/qml:"
ls /usr/qml 2>&1 | head -30
echo "-- QPA platform plugins:"
ls /usr/plugins/platforms 2>&1
echo "-- image format plugins (PNG is built in; JPEG and GIF need these):"
ls /usr/plugins/imageformats 2>&1
echo

echo "== HTTPS handshake with each existing CA candidate (verification ON)"
if command -v curl >/dev/null 2>&1; then
    for f in /etc/ssl/certs/ca-certificates.crt /etc/ssl/cert.pem /etc/ssl/certs/ca-bundle.crt; do
        [ -f "$f" ] || continue
        code=$(curl --cacert "$f" -sS -m 20 -o /dev/null -w '%{http_code}' https://curl.se/ 2>&1)
        echo "$f -> $code"
    done
else
    echo "curl command not available; the handshake is tested by the app itself."
fi
echo

echo "== note"
echo "QueryNetwork() (R-11) cannot be called from a shell: it is read by the app and logged as network.connect."
