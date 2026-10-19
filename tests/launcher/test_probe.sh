#!/usr/bin/env bash
# scripts/probe-device.sh with the host build of pbc-probe: it prints the facts the design decisions wait for
# (research R-02) and no personal data (host name, user name, home directory).
#
#   tests/launcher/test_probe.sh <repo-root> <pbc-probe-binary>
set -euo pipefail

ROOT="${1:?usage: test_probe.sh <repo-root> <pbc-probe>}"
PROBE="${2:?usage: test_probe.sh <repo-root> <pbc-probe>}"
SCRIPT="$ROOT/scripts/probe-device.sh"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fail=0
check() { if ! "$@"; then echo "FAIL: $*" >&2; fail=1; fi; }

sh -n "$SCRIPT" || { echo "FAIL: probe-device.sh is not valid sh" >&2; exit 1; }

mkdir -p "$tmp/data"
out="$(sh "$SCRIPT" "$PROBE" "$tmp/data" 2>&1)" || { echo "FAIL: probe-device.sh exited non-zero" >&2; echo "$out" >&2; exit 1; }

# the facts (each section header and the values the open items O-1, O-6, O-7, O-8 need)
for needle in '== libcurl' 'version:' 'protocols:' 'imaps: ' 'smtps: ' 'imap: ' 'smtp: ' \
              '== CA files' '/etc/ssl/certs' '== data directory' 'filesystem:' 'chmod 0600:' 'free:' \
              '== system' 'TZ:' 'localtime:' 'MemTotal:' 'gconv:' 'screen:' '== charsets'; do
    check grep -qF -- "$needle" <<<"$out"
done

# no personal data
for secret in "$(hostname 2>/dev/null || true)" "$(id -un 2>/dev/null || true)" "${USER:-}" "${LOGNAME:-}" "${HOME:-}"; do
    [ "${#secret}" -ge 3 ] || continue
    if grep -qF -- "$secret" <<<"$out"; then echo "FAIL: the probe output contains '$secret'" >&2; fail=1; fi
done
if grep -qE '/home/|/Users/' <<<"$out"; then echo "FAIL: the probe output contains a home path" >&2; fail=1; fi

# a missing probe binary is reported, the shell part still runs
out2="$(sh "$SCRIPT" "$tmp/does-not-exist" "$tmp/data" 2>&1)" || { echo "FAIL: missing binary must not abort" >&2; fail=1; }
check grep -qF 'pbc-probe not found' <<<"$out2"
check grep -qF '== system' <<<"$out2"

[ "$fail" -eq 0 ] && echo "probe test passed"
exit "$fail"
