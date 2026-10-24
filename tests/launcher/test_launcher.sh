#!/usr/bin/env bash
# The reader's launcher (platform/pocketbook/pbcloud.app) on the host, with a fake pbcloud.bin:
#   - it is valid sh,
#   - a start that aborts with the GPU refusal (exit 134, "mali0") is retried exactly once and both results are logged,
#   - any other failure is not retried,
#   - the log of the launch before is kept as pbcloud.log.prev,
#   - no file name or path of the user's files reaches the log, even when the system's libraries print one (Constitution IV).
#
#   tests/launcher/test_launcher.sh [repo-root]
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
LAUNCHER="$ROOT/platform/pocketbook/pbcloud.app"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fail=0
check() { if ! "$@"; then echo "FAIL: $*" >&2; fail=1; fi; }

sh -n "$LAUNCHER" || { echo "FAIL: launcher is not valid sh" >&2; exit 1; }

# the launcher with the device paths replaced by the throw-away layout
adapt() {
    sed -e "s#/mnt/ext1/applications#$tmp/app#; s#/mnt/ext1/system/pbcloud#$tmp/data#; s#cd /mnt/ext1#cd $tmp/root#; s#^STORAGE=/mnt/ext1#STORAGE=$tmp/root#" \
        -e 's#^sleep 2$#true#; s#^    sleep 2$#    true#' "$LAUNCHER"
}

# run the launcher against a fake device layout; $1 = body of the fake binary
launch() {
    rm -rf "$tmp/app" "$tmp/data" "$tmp/root" "$tmp/count"; mkdir -p "$tmp/app" "$tmp/root"
    printf '#!/bin/sh\necho x >> "%s/count"\n%s\n' "$tmp" "$1" > "$tmp/app/pbcloud.bin"
    chmod +x "$tmp/app/pbcloud.bin"
    adapt > "$tmp/launch.sh"
    sh "$tmp/launch.sh"
}
runs() { wc -l < "$tmp/count" | tr -d ' '; }

# 1. GPU refusal on the first start, fine on the second
launch 'if [ -f "'"$tmp"'/ok" ]; then echo started; exit 0; fi; touch "'"$tmp"'/ok"; echo "failed to open device file /dev/mali0 with errno 13" >&2; kill -ABRT $$'
rm -f "$tmp/ok"
launch 'if [ -f "'"$tmp"'/ok" ]; then echo started; exit 0; fi; touch "'"$tmp"'/ok"; echo "failed to open device file /dev/mali0 with errno 13" >&2; kill -ABRT $$'
check [ "$(runs)" = 2 ]
check grep -q 'exit code: 134' "$tmp/data/pbcloud.log"
check grep -q 'retrying once' "$tmp/data/pbcloud.log"
check grep -q 'exit code (retry): 0' "$tmp/data/pbcloud.log"
check grep -q 'diagnostics: uid/groups' "$tmp/data/pbcloud.log"

# 2. GPU refusal twice: one retry only
launch 'echo "failed to open device file /dev/mali0 with errno 13" >&2; kill -ABRT $$'
check [ "$(runs)" = 2 ]

# 3. an abort that is not the GPU refusal is not retried
launch 'echo "something else" >&2; kill -ABRT $$'
check [ "$(runs)" = 1 ]

# 4. a normal exit is not retried, and the previous log is kept
launch 'echo first >&2; exit 0'
adapt > "$tmp/launch.sh"
printf '#!/bin/sh\necho second >&2; exit 0\n' > "$tmp/app/pbcloud.bin"
sh "$tmp/launch.sh"
check grep -q second "$tmp/data/pbcloud.log"
check grep -q first "$tmp/data/pbcloud.log.prev"

# 5. the device probe: a run-probe flag file makes the launcher run the probe script instead of the app, once
launch 'echo app-started >&2; exit 0'
printf '#!/bin/sh\necho "probe args: $1 $2"\n' > "$tmp/app/pbcloud-probe.sh"
mkdir -p "$tmp/data"; : > "$tmp/data/run-probe"
rm -f "$tmp/count"
sh "$tmp/launch.sh"
check [ ! -e "$tmp/count" ]                                  # the app was not started
check [ ! -e "$tmp/data/run-probe" ]                         # the flag is consumed: no loop
check grep -q "probe args: $tmp/app/pbc-probe.bin $tmp/data" "$tmp/data/probe.txt"
sh "$tmp/launch.sh"                                          # next start: the app again
check [ "$(runs)" = 1 ]

# 5b. the hand-off probe: a run-handoff-probe flag file makes the launcher run the InkView probe with the data folder instead of the app, once
launch 'echo app-started >&2; exit 0'
printf '#!/bin/sh\necho "handoff args: $1" > "$(dirname "$0")/../data/handoff-args.txt"\n' > "$tmp/app/pbc-handoff.bin"
chmod +x "$tmp/app/pbc-handoff.bin"
mkdir -p "$tmp/data"; : > "$tmp/data/run-handoff-probe"
rm -f "$tmp/count"
sh "$tmp/launch.sh"
check [ ! -e "$tmp/count" ]                                  # the app was not started
check [ ! -e "$tmp/data/run-handoff-probe" ]                 # the flag is consumed: no loop
check grep -q "handoff args: $tmp/data" "$tmp/data/handoff-args.txt"
sh "$tmp/launch.sh"                                          # next start: the app again
check [ "$(runs)" = 1 ]

# 6. no file name or path reaches the log: the lines the system's libraries print when a book is opened are replaced; the app's own
#    lines, the exit code and the path of the app itself stay
body=$(cat <<EOF
echo "2026-10-20T21:16:46 update.run files=5 added=5"
echo "PERF_TESTING START: (iv_OpenBook) filename = [$tmp/root/PBCloud/E-Books/Secret Book.pdf], time = [1]"
echo "task_findbybook('$tmp/root/PBCloud/E-Books/Secret Book.pdf')"
echo "program_name = [$tmp/app/pbcloud.bin], basename = [pbcloud.bin]"
echo "a folder called $tmp/root/PBCloud/$tmp/app/pbcloud.bin/Secret Folder/x.pdf" >&2
echo "no path here" >&2
exit 3
EOF
)
launch "$body"
check [ "$(runs)" = 1 ]
check grep -q 'update.run files=5' "$tmp/data/pbcloud.log"
check grep -q 'no path here' "$tmp/data/pbcloud.log"
check grep -q "program_name = \[$tmp/app/pbcloud.bin\]" "$tmp/data/pbcloud.log"
check grep -q 'exit code: 3' "$tmp/data/pbcloud.log"                  # the exit code survives the filter
check grep -q 'removed' "$tmp/data/pbcloud.log"
check test -z "$(grep -E 'Secret|iv_OpenBook|task_findbybook|E-Books' "$tmp/data/pbcloud.log" || true)"
check test -z "$(find "$tmp" -type f -not -path "$tmp/app/*" -exec grep -l 'Secret' {} + 2>/dev/null || true)"   # and no file of the layout holds it

# 7. the log is written while the app runs, not when it ends: an app that is killed (the reader's USB mode does that) must leave its lines
body=$(cat <<EOF
echo "early line"
while [ ! -f "$tmp/go" ]; do sleep 0.1; done
echo "late line"
EOF
)
rm -f "$tmp/go"
rm -rf "$tmp/app" "$tmp/data" "$tmp/root" "$tmp/count"; mkdir -p "$tmp/app" "$tmp/root"
printf '#!/bin/sh\n%s\n' "$body" > "$tmp/app/pbcloud.bin"; chmod +x "$tmp/app/pbcloud.bin"
adapt > "$tmp/launch.sh"
sh "$tmp/launch.sh" &
lpid=$!
seen=0
for _ in $(seq 1 40); do
    if grep -q 'early line' "$tmp/data/pbcloud.log" 2>/dev/null; then seen=1; break; fi
    sleep 0.1
done
touch "$tmp/go"
wait "$lpid"
check [ "$seen" = 1 ]                                        # seen while the app was still running
check grep -q 'late line' "$tmp/data/pbcloud.log"

[ "$fail" = 0 ] && echo "launcher tests passed"
exit "$fail"
