#!/usr/bin/env bash
# scripts/package-device.sh on the host, with a fake ARM "binary" (a text file holding the version) and test doubles
# for the store's tool `pbstore-catalog` (spec 007):
#   - the package gets a manifest.json at its root and nothing but applications/ besides it,
#   - the real tool (when installed) accepts the package,
#   - the tool is found through PBSTORE_CATALOG or PATH, a missing tool is reported before any work,
#   - two packs must be identical, the check must pass, and a failed build leaves no ZIP behind,
#   - the files under applications/ are the ones of release 0.6.0.
#
#   tests/packaging/test_package_device.sh [repo-root]
# Exit 77 (CTest: skipped) only when nothing could run.
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
SCRIPT="$ROOT/scripts/package-device.sh"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fail=0 ran=0 skipped=0
VERSION="$(head -n 1 "$ROOT/VERSION" | tr -d '[:space:]')"
HANDOFF_FAKE=""

# The staged tree must not depend on the machine: no SDK, the image plugins come from a fake folder.
unset POCKETBOOK_SDK PBC_MAX_ZIP_BYTES
mkdir -p "$tmp/plugins"; : > "$tmp/plugins/libqjpeg.so"; : > "$tmp/plugins/libqgif.so"
export PBC_QT_IMAGE_PLUGINS="$tmp/plugins"

check() { if ! "$@"; then echo "FAIL: $*" >&2; fail=1; fi; }
check_not() { if "$@"; then echo "FAIL (expected to fail): $*" >&2; fail=1; fi; }
case_start() { echo "== $1"; ran=$((ran + 1)); }
skip() { echo "SKIP: $1"; skipped=$((skipped + 1)); ran=$((ran - 1)); }

# The real tool: PBSTORE_CATALOG if it names an executable, else the first pbstore-catalog on PATH; empty when absent.
real_tool() {
    if [ -n "${PBSTORE_CATALOG:-}" ] && [ -x "$PBSTORE_CATALOG" ]; then echo "$PBSTORE_CATALOG"
    else command -v pbstore-catalog || true; fi
}

# Remember the real tool, then hide the variable: the cases below choose the tool through PATH or set the variable themselves.
REAL_TOOL="$(real_tool)"
unset PBSTORE_CATALOG

# A test double for the tool: $1 = directory, $2 = file name, $3 = shell body (sees "$@").
stub() { mkdir -p "$1"; printf '#!/bin/sh\n%s\n' "$3" > "$1/$2"; chmod +x "$1/$2"; }

# Body of a well-behaved double: records its calls in $STUB_LOG, keeps the tree it was asked to pack in $STUB_SEEN,
# writes a fixed placeholder as the ZIP and accepts every package.
GOOD_STUB='echo "$*" >> "$STUB_LOG"
case "$1" in
    pack) rm -rf "$STUB_SEEN"; cp -R "$2" "$STUB_SEEN"; echo "stub zip" > "$4" ;;
    check-package) exit 0 ;;
    *) exit 2 ;;
esac'

# build <out-dir> [version]: run the script on a fake binary holding that version (default: VERSION), with a fake device
# probe (and a fake hand-off probe when HANDOFF_FAKE is set); sets rc, and stdout/stderr in $tmp/stdout and $tmp/stderr.
# The caller sets PATH and PBSTORE_CATALOG as needed.
build() {
    local out="$1" ver="${2:-}"
    printf 'fake pbcloud %s\n' "${ver:-$VERSION}" > "$tmp/pbcloud.fake"
    printf 'fake probe\n' > "$tmp/pbc-probe.fake"
    local extra=()
    [ -z "$HANDOFF_FAKE" ] || extra=("$HANDOFF_FAKE")
    rc=0
    "$SCRIPT" "$tmp/pbcloud.fake" "$ver" "$out" "$tmp/pbc-probe.fake" "${extra[@]}" > "$tmp/stdout" 2> "$tmp/stderr" || rc=$?
}

# json_get <file> <dotted.key>: print one value of a JSON file
json_get() { python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1]))
for k in sys.argv[2].split("."): d=d[k]
print(d)' "$1" "$2"; }

# json_has <file> <key>: true when the top-level key exists
json_has() { python3 -I -c 'import json,sys; sys.exit(0 if sys.argv[2] in json.load(open(sys.argv[1])) else 1)' "$1" "$2"; }

# install_entry <install.md> <dotted.key>: one value of the "U_PbCloud" block that INSTALL.md tells the reader to register
install_entry() { python3 -I -c 'import json,re,sys
t=open(sys.argv[1]).read()
m=re.search(r"\"U_PbCloud\":\s*\{.*?\n\s*\}", t, re.S)
d=json.loads("{"+m.group(0)+"}")["U_PbCloud"]
for k in sys.argv[2].split("."): d=d[k]
print(d)' "$1" "$2"; }

# new_stub_env <name>: a fresh directory with a good double named pbstore-catalog; exports STUB_LOG and STUB_SEEN
new_stub_env() {
    local dir="$tmp/$1"
    rm -rf "$dir"; mkdir -p "$dir"
    export STUB_LOG="$dir/calls" STUB_SEEN="$dir/seen"
    : > "$STUB_LOG"
    stub "$dir/bin" pbstore-catalog "$GOOD_STUB"
    STUB_BIN="$dir/bin"; STUB_OUT="$dir/out"
}

# ---- User Story 1 -----------------------------------------------------------------------------------------------

case_start "package passes the store check"
tool="$REAL_TOOL"
if [ -z "$tool" ]; then
    skip "the real pbstore-catalog is not installed (PATH / PBSTORE_CATALOG)"
else
    # the staged tree holds the real time zone data, whose names contain '+' (contract tag v0.1.4 or newer allows it)
    PATH="$(dirname "$tool"):$PATH" build "$tmp/real-out"
    check [ "$rc" = 0 ]
    zip="$tmp/real-out/pbcloud-inkpad-one-$VERSION.zip"
    check [ -f "$zip" ]
    check "$tool" check-package "$zip" > /dev/null
fi

case_start "top level and manifest"
new_stub_env manifest
PATH="$STUB_BIN:$PATH" build "$STUB_OUT"
check [ "$rc" = 0 ]
check [ -d "$STUB_SEEN" ]
check [ "$(ls -A "$STUB_SEEN" 2> /dev/null | tr '\n' ' ')" = "applications manifest.json " ]
if [ -f "$STUB_SEEN/manifest.json" ]; then
    m="$STUB_SEEN/manifest.json"
    check [ "$(json_get "$m" schema_version)" = "1.0" ]
    check [ "$(json_get "$m" id)" = "pbcloud" ]
    check [ "$(json_get "$m" name)" = "PB Cloud" ]
    check [ "$(json_get "$m" version)" = "$VERSION" ]
    check [ "$(json_get "$m" min_firmware)" = "6.11.1437" ]
    check [ "$(json_get "$m" target_device)" = "InkPad One" ]
    check [ "$(json_get "$m" menu_entry.id)" = "U_PbCloud" ]
    check [ "$(json_get "$m" menu_entry.title)" = "PB Cloud" ]
    check [ "$(json_get "$m" menu_entry.path)" = "applications/pbcloud.app" ]
    check [ "$(json_get "$m" menu_entry.icon)" = "applications/icons/pbcloud.bmp" ]
    check [ "$(json_get "$m" menu_entry.focused_icon)" = "applications/icons/pbcloud_f.bmp" ]
    check [ "$(json_get "$m" menu_entry.group)" = "@General" ]
    check [ "$(json_get "$m" data_dir)" = "system/pbcloud" ]
    check_not json_has "$m" files
else
    echo "FAIL: no manifest.json was staged" >&2; fail=1
fi
check [ -z "$(find "$STUB_SEEN" -name 'INSTALL.md' 2> /dev/null)" ]

case_start "version comes from one source"
new_stub_env version
PATH="$STUB_BIN:$PATH" build "$STUB_OUT" 9.8.7
check [ "$rc" = 0 ]
check [ -f "$STUB_OUT/pbcloud-inkpad-one-9.8.7.zip" ]
if [ -f "$STUB_SEEN/manifest.json" ]; then
    check [ "$(json_get "$STUB_SEEN/manifest.json" version)" = "9.8.7" ]
else
    echo "FAIL: no manifest.json was staged" >&2; fail=1
fi
# a binary that does not hold the version string is refused and leaves no ZIP
printf 'fake pbcloud 1.1.1\n' > "$tmp/other.fake"
rc=0; PATH="$STUB_BIN:$PATH" "$SCRIPT" "$tmp/other.fake" 2.2.2 "$tmp/mismatch-out" > "$tmp/stdout" 2> "$tmp/stderr" || rc=$?
check [ "$rc" = 1 ]
check grep -q 'version mismatch' "$tmp/stderr"
check [ -z "$(find "$tmp/mismatch-out" -name '*.zip' 2> /dev/null)" ]

case_start "manifest equals INSTALL.md (guard)"
t="$ROOT/packaging/manifest.template.json"
check [ -f "$t" ]
check [ "$(install_entry "$ROOT/INSTALL.md" title)" = "$(json_get "$t" menu_entry.title)" ]
check [ "$(install_entry "$ROOT/INSTALL.md" icon.path)" = "$(json_get "$t" menu_entry.icon)" ]
check [ "$(install_entry "$ROOT/INSTALL.md" focused_icon.path)" = "$(json_get "$t" menu_entry.focused_icon)" ]
check [ "$(install_entry "$ROOT/INSTALL.md" path)" = "/mnt/ext1/$(json_get "$t" menu_entry.path)" ]
check grep -q "\"$(json_get "$t" menu_entry.id)\": {" "$ROOT/INSTALL.md"

# ---- User Story 2 -----------------------------------------------------------------------------------------------

# A PATH holding only what the script itself needs (no pbstore-catalog)
minpath="$tmp/minpath"; mkdir -p "$minpath"
for c in env bash sed grep head tr sort mktemp install mkdir rm cmp cat dirname basename mv cp find stat chmod; do
    ln -sf "$(command -v "$c")" "$minpath/$c"
done
no_zip() { [ -z "$(find "$1" -name '*.zip' 2> /dev/null)" ]; }

case_start "tool missing"
new_stub_env missing
started=$SECONDS
PBSTORE_CATALOG="" PATH="$minpath" build "$STUB_OUT"
check [ "$rc" = 1 ]
check grep -q 'pbstore-catalog not found' "$tmp/stderr"
check grep -q 'PBSTORE_CATALOG' "$tmp/stderr"
check grep -q 'v0.1.4' "$tmp/stderr"
check_not grep -q 'missing input' "$tmp/stderr"
check no_zip "$STUB_OUT"
check [ $((SECONDS - started)) -lt 5 ]

case_start "environment variable"
new_stub_env envvar
stub "$tmp/envvar/tools" mytool "$GOOD_STUB"
PBSTORE_CATALOG="$tmp/envvar/tools/mytool" PATH="$minpath" build "$STUB_OUT"
check [ "$rc" = 0 ]
check [ -s "$STUB_LOG" ]
check [ -f "$STUB_OUT/pbcloud-inkpad-one-$VERSION.zip" ]
new_stub_env envbad
PBSTORE_CATALOG="$tmp/envbad/nonexistent" PATH="$STUB_BIN:$PATH" build "$STUB_OUT"
check [ "$rc" = 1 ]
check grep -q 'PBSTORE_CATALOG is set but is not an executable file' "$tmp/stderr"
check [ ! -s "$STUB_LOG" ]
check no_zip "$STUB_OUT"

case_start "reproducibility"
new_stub_env differ
stub "$tmp/differ/bin" pbstore-catalog 'case "$1" in
    pack) echo "$$ $(date +%s%N) $RANDOM" > "$4" ;;
    check-package) exit 0 ;;
esac'
PATH="$tmp/differ/bin:$PATH" build "$STUB_OUT"
check [ "$rc" = 1 ]
check grep -q 'package is not reproducible' "$tmp/stderr"
check no_zip "$STUB_OUT"
new_stub_env same
PATH="$STUB_BIN:$PATH" build "$STUB_OUT"
check [ "$rc" = 0 ]
check [ "$(grep -c '^pack ' "$STUB_LOG")" = 2 ]
check [ "$(grep -c '^check-package ' "$STUB_LOG")" = 1 ]

case_start "check failure leaves nothing"
new_stub_env badcheck
stub "$tmp/badcheck/bin" pbstore-catalog 'case "$1" in
    pack) echo "stub zip" > "$4" ;;
    check-package) echo "error[E-PKG-04] x: y" >&2; exit 1 ;;
esac'
mkdir -p "$STUB_OUT"; echo old > "$STUB_OUT/pbcloud-inkpad-one-$VERSION.zip"
PATH="$tmp/badcheck/bin:$PATH" build "$STUB_OUT"
check [ "$rc" = 1 ]
check grep -q 'error\[E-PKG-04\] x: y' "$tmp/stderr"
check grep -q 'package check failed' "$tmp/stderr"
check no_zip "$STUB_OUT"

case_start "bad inputs"
new_stub_env badin
PATH="$STUB_BIN:$PATH" build "$STUB_OUT" 1.2
check [ "$rc" = 1 ]
check grep -q 'version is not MAJOR.MINOR.PATCH' "$tmp/stderr"
PATH="$STUB_BIN:$PATH" build "$STUB_OUT" 1.2.x
check [ "$rc" = 1 ]
check grep -q 'version is not MAJOR.MINOR.PATCH' "$tmp/stderr"
# a different ROOT: scripts/ and packaging/ copied, the other inputs linked from the repository
troot="$tmp/troot"; rm -rf "$troot"; mkdir -p "$troot"
cp -R "$ROOT/scripts" "$ROOT/packaging" "$troot/"
for d in platform third_party res licenses VERSION LICENSE; do ln -s "$ROOT/$d" "$troot/$d"; done
sed -i 's/"InkPad One"/"InkPad One @X@"/' "$troot/packaging/manifest.template.json"
SCRIPT="$troot/scripts/package-device.sh" PATH="$STUB_BIN:$PATH" build "$STUB_OUT"
check [ "$rc" = 1 ]
check grep -q 'unfilled placeholder: @X@' "$tmp/stderr"
rm "$troot/packaging/manifest.template.json"
SCRIPT="$troot/scripts/package-device.sh" PATH="$STUB_BIN:$PATH" build "$STUB_OUT"
check [ "$rc" = 1 ]
check grep -q 'missing input: .*manifest.template.json' "$tmp/stderr"
# an empty VERSION file and no version argument
rm "$troot/VERSION"; : > "$troot/VERSION"
SCRIPT="$troot/scripts/package-device.sh" PATH="$STUB_BIN:$PATH" build "$STUB_OUT"
check [ "$rc" = 1 ]
check grep -q 'no version' "$tmp/stderr"
check no_zip "$STUB_OUT"

case_start "size budget and the old modes (guard)"
new_stub_env budget
PBC_MAX_ZIP_BYTES=5 PATH="$STUB_BIN:$PATH" build "$STUB_OUT"
check [ "$rc" = 1 ]
check grep -q 'larger than the budget' "$tmp/stderr"
check no_zip "$STUB_OUT"
# --self-test and --check-size must not need the store's tool
rc=0; PATH="$minpath" "$SCRIPT" --self-test > "$tmp/stdout" 2> "$tmp/stderr" || rc=$?
check [ "$rc" = 0 ]
printf 'x' > "$tmp/tiny.zip"
rc=0; PATH="$minpath" "$SCRIPT" --check-size "$tmp/tiny.zip" > "$tmp/stdout" 2> "$tmp/stderr" || rc=$?
check [ "$rc" = 0 ]

# ---- User Story 3 -----------------------------------------------------------------------------------------------

# The files that must be under applications/: the listed ones, the time zone data and its VERSION file
expected_apps() {
    {
        grep -v '^#' "$ROOT/tests/packaging/applications.expected"
        echo "applications/pbcloud-zoneinfo/VERSION"
        ( cd "$ROOT/third_party/tzdata/zoneinfo" && find . -type f | sed 's|^\./|applications/pbcloud-zoneinfo/|' )
    } | LC_ALL=C sort
}

case_start "applications set unchanged (guard)"
new_stub_env applist
PATH="$STUB_BIN:$PATH" build "$STUB_OUT"
check [ "$rc" = 0 ]
check diff <(cd "$STUB_SEEN" && find applications -type f | LC_ALL=C sort) <(expected_apps)
check [ -z "$(find "$STUB_SEEN" -name 'INSTALL.md')" ]
check cmp "$STUB_SEEN/applications/pbcloud.app" "$ROOT/platform/pocketbook/pbcloud.app"
check cmp "$STUB_SEEN/applications/pbcloud-probe.sh" "$ROOT/scripts/probe-device.sh"
check cmp "$STUB_SEEN/applications/pbcloud-cacert.pem" "$ROOT/third_party/cacert/cacert.pem"
# with the hand-off probe the two extra files are staged, and the manifest is still made from the template
new_stub_env handoff
printf 'fake handoff\n' > "$tmp/pbc-handoff.fake"
HANDOFF_FAKE="$tmp/pbc-handoff.fake" PATH="$STUB_BIN:$PATH" build "$STUB_OUT"
HANDOFF_FAKE=""
check [ "$rc" = 0 ]
check diff <(cd "$STUB_SEEN" && find applications -type f | LC_ALL=C sort) \
           <({ expected_apps; echo applications/pbc-handoff.bin; echo applications/pbcloud-handoff-target.app; } | LC_ALL=C sort)

case_start "instructions stay reachable"
check grep -q '](INSTALL.md)' "$ROOT/README.md"
check_not grep -q 'the zip with INSTALL.md' "$ROOT/README.md"
check grep -q 'merging it with the existing' "$ROOT/INSTALL.md"
check grep -q 'manifest.json' "$ROOT/INSTALL.md"

# ---- end ---------------------------------------------------------------------------------------------------------
if [ "$fail" != 0 ]; then echo "FAILED"; exit 1; fi
if [ "$ran" = 0 ] && [ "$skipped" != 0 ]; then echo "all cases skipped"; exit 77; fi
echo "ok ($ran cases, $skipped skipped)"
