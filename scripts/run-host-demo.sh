#!/usr/bin/env bash
# Try the app on the host with a throw-away data folder, without any real account.
#
#   scripts/run-host-demo.sh [build-dir]        build-dir defaults to build-host
#
# Starts the desktop app with PBC_DATA_DIR pointing to a temporary folder. Everything is removed when the app closes.
# The app has no server connection in this baseline; a fake server for trying the sync arrives with the first feature.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$ROOT/build-host}"
APP="$BUILD/app/pbcloud"
[ -x "$APP" ] || { echo "missing $APP - build the host target first: cmake -S . -B build-host -G Ninja && cmake --build build-host" >&2; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/data"
PBC_DATA_DIR="$tmp/data" "$APP"
