#!/bin/sh
# PocketBook launcher for pbcloud on the InkPad One.
#
# Installed at /mnt/ext1/applications/pbcloud.app, with the ELF binary beside it as
# pbcloud.bin. Runtime data and the log live under /mnt/ext1/system/pbcloud.
# Adapted from the InkFeed launcher (MIT, see NOTICE).
#
# Device notes (InkPad One: RK3566, aarch64 kernel, 32-bit armhf userland):
#   - Qt 6.x ships under /usr/lib with its QPA plugins in /usr/plugins/platforms and the
#     QML modules under /usr/qml. The binary was linked for a /usr/local/qt6 prefix that
#     does not exist here, so point Qt at the real locations below.
#   - Qt Quick needs a GL context: use the EGL QPA (pocketbook2egl).
#   - Content rotation (g-sensor) and devicePixelRatio scaling are handled in the app.

STORAGE=/mnt/ext1
APPDIR=/mnt/ext1/applications
DATADIR=/mnt/ext1/system/pbcloud
mkdir -p "$DATADIR"
# Keep the log of the launch before this one: a start that fails must not erase the evidence of the one before it.
[ -f "$DATADIR/pbcloud.log" ] && mv -f "$DATADIR/pbcloud.log" "$DATADIR/pbcloud.log.prev" 2>/dev/null
exec >"$DATADIR/pbcloud.log" 2>&1

echo "=== pbcloud launch: $(date 2>/dev/null) ==="
# What the system looks like at this start (no personal data). Each command may be missing on the reader.
echo "--- diagnostics: uid/groups: $(id 2>/dev/null || echo unknown)"
echo "--- diagnostics: gpu node: $(ls -l /dev/mali0 2>/dev/null || echo 'not found')"
echo "--- diagnostics: uptime: $(uptime 2>/dev/null || echo unknown)"
echo "--- diagnostics: memory: $(grep -E '^(MemFree|MemAvailable)' /proc/meminfo 2>/dev/null | tr -s ' ' | tr '\n' ' ')"
echo "--- diagnostics: other app processes: $(ps 2>/dev/null | grep -E 'pbcloud\.bin' | grep -v grep || echo none)"

export LD_LIBRARY_PATH="/usr/lib:/usr/local/lib:$LD_LIBRARY_PATH"
export QT_QPA_PLATFORM_PLUGIN_PATH="/usr/plugins/platforms"
export QT_PLUGIN_PATH="/usr/plugins"
export QT_QPA_PLATFORM="pocketbook2egl"
export QML_IMPORT_PATH="/usr/qml"
export QML2_IMPORT_PATH="/usr/qml"

cd /mnt/ext1

# Device probe (INSTALL.md, "Device probe"): when the file run-probe exists in the data folder, this start runs the
# probe script instead of the app, writes probe.txt there and removes the flag, so the next start is a normal one.
if [ -e "$DATADIR/run-probe" ]; then
    rm -f "$DATADIR/run-probe"
    echo "=== pbcloud: run-probe flag found, running the probe instead of the app ==="
    sh "$APPDIR/pbcloud-probe.sh" "$APPDIR/pbc-probe.bin" "$DATADIR" > "$DATADIR/probe.txt" 2>&1
    echo "=== pbcloud: probe finished, output in probe.txt ==="
    exit 0
fi

# Hand-off probe (INSTALL.md, "Hand-off probe"): when the file run-handoff-probe exists in the data folder, this start runs the small InkView program that
# tries to start pbcloud-handoff-target.app with parameters, instead of the app, once.
if [ -e "$DATADIR/run-handoff-probe" ]; then
    rm -f "$DATADIR/run-handoff-probe"
    echo "=== pbcloud: run-handoff-probe flag found, running the hand-off probe instead of the app ==="
    exec "$APPDIR/pbc-handoff.bin" "$DATADIR"
fi

# The log must not hold the name or the path of any file of the user (Constitution IV), and the system's libraries print them: when a book
# is opened, InkView writes "iv_OpenBook filename = <path>" and "task_findbybook('<path>')". Everything the app prints goes through
# this filter before it reaches the log: a line that names anything on the storage, or one of those two calls, is replaced by a notice.
# The path of the app itself (the program_name line) is the one thing that may stay. Nothing else is written anywhere before the filter.
filter_log() {
    # a read loop, not sed: sed holds its output back until it ends, and an app that is killed (the reader's USB mode does that) would
    # leave no log at all. The shell writes each line as it comes.
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            *iv_OpenBook*|*task_findbybook*)
                line="[a line with a file path was removed from the log]" ;;
            *"$STORAGE/"*)
                # the path of the app itself may stay; anything else on the storage may be a file of the user
                other=$(printf '%s\n' "$line" | sed "s#$APPDIR/pbcloud\\.bin##g")
                case "$other" in
                    *"$STORAGE/"*) line="[a line with a file path was removed from the log]" ;;
                esac ;;
        esac
        printf '%s\n' "$line"
    done
}

# The log is a file, so the app's own stdout is block-buffered and lost when the process aborts; line buffering
# keeps the start-up lines of a failed start (stdbuf is not on every reader). The exit code of the app is kept in a small file because
# a pipe through the filter would hide it (no pipefail in this shell).
run_app() {
    rm -f "$DATADIR/exit-code"
    {
        if command -v stdbuf >/dev/null 2>&1; then stdbuf -oL -eL "$APPDIR/pbcloud.bin"; else "$APPDIR/pbcloud.bin"; fi
        echo $? > "$DATADIR/exit-code"
    } 2>&1 | filter_log
    code=$(cat "$DATADIR/exit-code" 2>/dev/null || echo 1)
    rm -f "$DATADIR/exit-code"
    return "$code"
}

# Everything this attempt wrote to the log (stdout and stderr share the file).
mark=$(wc -c < "$DATADIR/pbcloud.log" 2>/dev/null || echo 0)
run_app
code=$?
echo "=== pbcloud.bin exit code: $code ==="

# Seen once: the system refused the GPU node (/dev/mali0, errno 13) and Qt aborted at once (exit 134). A reboot
# cleared it; it is not known whether it passes by itself, so try exactly one more time and record both results.
if [ "$code" -eq 134 ] && tail -c +"$((mark + 1))" "$DATADIR/pbcloud.log" 2>/dev/null | grep -q 'mali0'; then
    echo "=== pbcloud: GPU device refused (mali0), retrying once in 2 s ==="
    sleep 2
    run_app
    echo "=== pbcloud.bin exit code (retry): $? ==="
fi
