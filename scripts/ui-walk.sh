#!/usr/bin/env bash
# Scripted walk through the real pages on the host against the fake Nextcloud (tests/tools/pbc_fake_nextcloud), with pictures.
#
#   scripts/ui-walk.sh [--out <dir>] [--build <build-dir>] <walk-file>...
#
# Each walk file (the format is in app/ui_script.h) is one run of the app; the runs share one throw-away data folder, download folder and
# server, so a later walk starts where the earlier one ended (the app is closed and started again between them). In a walk file
# @ADDRESS@, @USER@, @PASSWORD@ and @OUT@ are replaced by the fake server's values and the picture folder, and lines of the form
#   # server <command>
# are sent to the server's console before that run starts (approve, touch <path>, drop, offline, online).
# The window is offscreen and English; pictures (shot commands) go to the out folder (default ./walk-out). Exit code 0 = every
# expect of every walk held. The server is a fake of the tests: this proves the pages and their wiring, nothing about a real Nextcloud
# or about the reader.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="walk-out"
BUILD="$ROOT/build-host"
while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        --build) BUILD="$2"; shift 2 ;;
        *) break ;;
    esac
done
[ $# -gt 0 ] || { echo "usage: scripts/ui-walk.sh [--out <dir>] [--build <build-dir>] <walk-file>..." >&2; exit 2; }
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
APP="$BUILD/app/pbcloud"
SERVER="$BUILD/tests/pbc_fake_nextcloud"
[ -x "$APP" ] && [ -x "$SERVER" ] || { echo "build first: cmake --build $BUILD" >&2; exit 1; }

tmp="$(mktemp -d)"
server_pid=""
cleanup() { exec 3>&- 2>/dev/null || true; [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null || true; rm -rf "$tmp"; }
trap cleanup EXIT
mkdir -p "$tmp/data" "$tmp/downloads" "$tmp/server"
# the reader's storage for the Upload walk (specs/002-manage-server-files): the folders the picker lists, with a few files
picker="$tmp/picker"
mkdir -p "$picker/Downloads" "$picker/Notes/trip/photos" "$picker/PBCloud" "$picker/system"
printf 'report' >"$picker/Downloads/report.pdf"; printf 'my diary' >"$picker/Downloads/diary.txt"
printf 'plan of the trip' >"$picker/Notes/trip/plan.md"; printf 'image' >"$picker/Notes/trip/photos/a.jpg"; printf 'x' >"$picker/Downloads/.hidden"
# later in the picker's order than the first walk's picks: a small file, a file that goes in chunks (24 MiB, sparse) and one for the unknown outcome
printf 'for the full account' >"$picker/Downloads/zz-quota.txt"
printf 'second' >"$picker/Downloads/second.txt"; truncate -s 24M "$picker/Downloads/zebra.bin"; printf 'last' >"$picker/Downloads/zlast.txt"
mkfifo "$tmp/console"
"$SERVER" "$tmp/server" <"$tmp/console" >"$tmp/server.log" 2>&1 &
server_pid=$!
exec 3>"$tmp/console"
for _ in $(seq 1 100); do grep -q '^READY ' "$tmp/server.log" 2>/dev/null && break; sleep 0.1; done
read -r _ url user password ca < <(grep '^READY ' "$tmp/server.log")
cp "$ca" "$tmp/data/custom-ca.pem"

status=0
for walk in "$@"; do
    echo "== walk: $walk"
    grep -E '^# server ' "$walk" | sed 's/^# server //' >&3 || true
    sleep 0.3
    sed -e "s#@ADDRESS@#${url#https://}#g" -e "s#@USER@#$user#g" -e "s#@PASSWORD@#$password#g" -e "s#@OUT@#$OUT#g" "$walk" >"$tmp/walk.txt"
    rc=0
    LC_ALL=C.UTF-8 LANG=C.UTF-8 QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
        PBC_DATA_DIR="$tmp/data" PBC_DOWNLOAD_DIR="$tmp/downloads" PBC_PICKER_ROOT="$picker" PBC_UI_SCRIPT="$tmp/walk.txt" "$APP" >"$tmp/app.log" 2>&1 || rc=$?
    grep -E '^\[walk\]|QML|warning' "$tmp/app.log" || true
    [ "$rc" -eq 0 ] || { echo "walk failed with exit code $rc: $walk" >&2; status=1; }
done
exit $status
