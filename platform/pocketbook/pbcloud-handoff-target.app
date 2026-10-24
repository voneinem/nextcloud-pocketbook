#!/bin/sh
# Hand-off probe target (INSTALL.md, "Hand-off probe"): records what an app that was started with OpenBook/OpenBook2 receives. It stands in for PB Office.
# It writes only the byte count and the first and last characters of each parameter - the probe hands over fixed test strings, never real data - and
# never the environment. Not part of the normal app.
OUT=/mnt/ext1/system/pbcloud/handoff-probe.txt
{
    echo "target: t=$(date +%s 2>/dev/null) started, argc=$#, \$0=$0, cwd=$(pwd)"
    n=0
    for a in "$@"; do
        n=$((n + 1))
        len=$(printf %s "$a" | wc -c | tr -d ' ')
        head=$(printf %s "$a" | head -c 40 | tr '\n' '~')
        tail=$(printf %s "$a" | tail -c 12 | tr '\n' '~')
        echo "target: arg$n bytes=$len head=[$head] tail=[$tail]"
    done
    echo "target: caller process still running: $(ps 2>/dev/null | grep -c '[p]bc-handoff.bin')"
    echo "target: my parent is [$(tr '\0' ' ' < /proc/$PPID/cmdline 2>/dev/null | head -c 80)]"
} >> "$OUT" 2>&1
exit 0
