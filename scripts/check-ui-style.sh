#!/usr/bin/env bash
# Mechanical check of the user interface style (spec 010 FR-006, SC-003): no bordered text button remains in the QML.
# Run by CTest and scripts/pre-merge.sh.
#
#   scripts/check-ui-style.sh [repo-root]    scan (default: this repository)
#   scripts/check-ui-style.sh --self-test    prove that every rule below detects a violation
#
# Rules (a finding makes the script exit non-zero), over every file under qml/ (QML, the qmldir and the resource list):
#   U1 The bordered text button `InkButton` is not used and not declared (it was deleted: every action is an icon control,
#      a tool of a bottom panel, a footer button of a dialog or a tappable row).
#   U2 No `bordered:` property (the switch that drew a border round a text button).
#   U3 No Qt Quick Controls `Button { ... }` (its own bevel and border read as a grey bar on the panel).
#   U4 No `source:` set to a literal http(s)/ftp address (no remote content is ever loaded, FR-009).
#   U5 No `import QtWebView` / `import QtWebEngine` (texts are shown as plain text).
#   U6 No `Qt.openUrlExternally` (links are shown as text and never opened).
#   U7 (feature 004) qml/pages/ShareLinkPage.qml: the link is shown as Text.PlainText wrapped with Text.WrapAnywhere and has no MouseArea,
#      onLinkActivated, TapHandler, PointerHandler, HoverHandler or Qt.openUrl (nothing in the display can be tapped or opened).
#   U8 (feature 004) a qml/pages/Share*.qml that has a password entry (objectName containing "sharePassword") sets `secret: true` on it.
#   U9 (feature 006) a ConfirmDialog that sets `destructive: true` (an irreversible step) also sets `defaultCancel: true` in the same dialog.
set -u

SELF="$(basename "${BASH_SOURCE[0]}")"

scan() {
    local root="$1" bad=0 f hits
    [ -d "$root/qml" ] || { echo "$SELF: no qml/ directory under $root" >&2; return 2; }
    while IFS= read -r -d '' f; do
        hits="$(grep -nE 'InkButton' "$f" || true)"
        if [ -n "$hits" ]; then echo "U1 $f: InkButton"; echo "$hits" | sed 's/^/    /'; bad=1; fi
        case "$f" in *.qml)
            hits="$(grep -nE '(^|[^A-Za-z_.])bordered[[:space:]]*:' "$f" || true)"
            if [ -n "$hits" ]; then echo "U2 $f: bordered property"; echo "$hits" | sed 's/^/    /'; bad=1; fi
            hits="$(grep -nE '(^|[^A-Za-z_.])Button[[:space:]]*\{' "$f" || true)"
            if [ -n "$hits" ]; then echo "U3 $f: Qt Quick Controls Button"; echo "$hits" | sed 's/^/    /'; bad=1; fi
            hits="$(grep -nE '(^|[^A-Za-z_.])source[[:space:]]*:[[:space:]]*["'"'"'](https?|ftp)://' "$f" || true)"
            if [ -n "$hits" ]; then echo "U4 $f: remote image source"; echo "$hits" | sed 's/^/    /'; bad=1; fi
            hits="$(grep -nE '^[[:space:]]*import[[:space:]]+(QtWebView|QtWebEngine)' "$f" || true)"
            if [ -n "$hits" ]; then echo "U5 $f: web view import"; echo "$hits" | sed 's/^/    /'; bad=1; fi
            hits="$(grep -nE 'Qt\.openUrlExternally' "$f" || true)"
            if [ -n "$hits" ]; then echo "U6 $f: Qt.openUrlExternally"; echo "$hits" | sed 's/^/    /'; bad=1; fi
            case "$f" in
                */pages/ShareLinkPage.qml)
                    hits="$(grep -nE 'MouseArea|onLinkActivated|TapHandler|PointerHandler|HoverHandler|Qt\.openUrl' "$f" || true)"
                    if [ -n "$hits" ]; then echo "U7 $f: the link display must not be tappable"; echo "$hits" | sed 's/^/    /'; bad=1; fi
                    for need in 'Text\.PlainText' 'Text\.WrapAnywhere'; do
                        grep -qE "$need" "$f" || { echo "U7 $f: the link display needs ${need//\\/}"; bad=1; }
                    done
                    ;;
            esac
            hits="$(awk '
                # a ConfirmDialog block: from its opening line to the closing brace of its body
                /ConfirmDialog[[:space:]]*\{/ && !inblock { inblock = 1; depth = 0; start = NR; destructive = 0; cancel = 0 }
                inblock {
                    line = $0
                    if (line ~ /destructive:[[:space:]]*true/) destructive = 1
                    if (line ~ /defaultCancel:[[:space:]]*true/) cancel = 1
                    n = gsub(/\{/, "{", line); m = gsub(/\}/, "}", line)
                    depth += n - m
                    if (depth <= 0) { if (destructive && !cancel) print start ": ConfirmDialog with destructive: true and no defaultCancel: true"; inblock = 0 }
                }' "$f" || true)"
            if [ -n "$hits" ]; then echo "U9 $f: an irreversible dialog must default to Cancel"; echo "$hits" | sed 's/^/    /'; bad=1; fi
            case "$f" in
                */pages/Share*.qml)
                    if grep -q 'sharePassword' "$f" && ! grep -qE 'secret:[[:space:]]*true' "$f"; then
                        echo "U8 $f: a password entry must set secret: true (no Copy, no suggestions)"; bad=1
                    fi
                    ;;
            esac
            ;;
        esac
    done < <(find "$root/qml" -type f \( -name '*.qml' -o -name qmldir -o -name qml.qrc \) -print0)
    return $bad
}

self_test() {
    local tmp rc=0
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' RETURN
    mk() { mkdir -p "$tmp/$1/qml"; }
    # a clean tree passes
    mk good
    printf 'import QtQuick\nItem { IconButton { text: "x" } ToolButton { text: "y" } }\n' > "$tmp/good/qml/A.qml"
    printf 'IconButton 1.0 IconButton.qml\n' > "$tmp/good/qml/qmldir"
    scan "$tmp/good" >/dev/null || { echo "self-test: a clean tree was reported" >&2; rc=1; }
    # each bad pattern is found
    for case_ in "U1:Item { InkButton { text: \"x\" } }" "U2:Item { property bool bordered: true }" "U3:import QtQuick.Controls
Button { text: \"x\" }" \
        "U4:Image { source: \"https://example.org/a.png\" }" "U4:AnimatedImage { source : 'http://example.org/a.gif' }" \
        "U5:import QtWebView" "U5:import QtWebEngine 1.0" "U6:MouseArea { onClicked: Qt.openUrlExternally(link) }" ; do
        mk bad
        printf 'import QtQuick\n%s\n' "${case_#*:}" > "$tmp/bad/qml/B.qml"
        if scan "$tmp/bad" >/dev/null; then echo "self-test: ${case_%%:*} was not detected" >&2; rc=1; fi
        rm -rf "$tmp/bad"
    done
    # local image sources and ordinary text pass
    mk good2
    printf 'import QtQuick\nItem { Image { source: "qrc:/icons/a.png" } Text { text: "see https://example.org" } }\n' > "$tmp/good2/qml/C.qml"
    scan "$tmp/good2" >/dev/null || { echo "self-test: a local image source or URL text was reported" >&2; rc=1; }
    # U7/U8 (feature 004, share links): the link display is plain, wrapped and cannot be tapped; a password entry is a secret field
    good_link='import QtQuick\nItem { Text { objectName: "shareLinkText"; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; text: share.detail.linkText } }'
    mk u7ok; mkdir -p "$tmp/u7ok/qml/pages"; printf "$good_link\n" > "$tmp/u7ok/qml/pages/ShareLinkPage.qml"
    scan "$tmp/u7ok" >/dev/null || { echo "self-test: a plain, wrapped link display was reported" >&2; rc=1; }
    for bad_ in 'MouseArea { anchors.fill: parent }' 'Text { onLinkActivated: function(l) { x(l) } }' 'TapHandler { onTapped: x() }' 'PointerHandler { }' 'Item { Component.onCompleted: Qt.openUrl(a) }'; do
        mk u7bad; mkdir -p "$tmp/u7bad/qml/pages"
        printf "$good_link\nItem { $bad_ }\n" > "$tmp/u7bad/qml/pages/ShareLinkPage.qml"
        if scan "$tmp/u7bad" >/dev/null; then echo "self-test: U7 was not detected for: $bad_" >&2; rc=1; fi
        rm -rf "$tmp/u7bad"
    done
    for missing in 'Text.PlainText' 'Text.WrapAnywhere'; do
        mk u7miss; mkdir -p "$tmp/u7miss/qml/pages"
        printf "$good_link\n" | sed "s/$missing/Text.Wrap/" > "$tmp/u7miss/qml/pages/ShareLinkPage.qml"
        if scan "$tmp/u7miss" >/dev/null; then echo "self-test: U7 was not detected when $missing is missing" >&2; rc=1; fi
        rm -rf "$tmp/u7miss"
    done
    mk u8ok; mkdir -p "$tmp/u8ok/qml/pages"
    printf 'import QtQuick\nItem { TextEntryDialog { objectName: "sharePasswordDialog"; secret: true } }\n' > "$tmp/u8ok/qml/pages/ShareFormPage.qml"
    scan "$tmp/u8ok" >/dev/null || { echo "self-test: a secret password dialog was reported" >&2; rc=1; }
    mk u8bad; mkdir -p "$tmp/u8bad/qml/pages"
    printf 'import QtQuick\nItem { TextEntryDialog { objectName: "sharePasswordDialog" } }\n' > "$tmp/u8bad/qml/pages/ShareFormPage.qml"
    if scan "$tmp/u8bad" >/dev/null; then echo "self-test: U8 was not detected (password dialog without secret: true)" >&2; rc=1; fi
    # U9 (feature 006): an irreversible dialog defaults to Cancel
    mk u9ok
    printf 'import QtQuick\nItem { ConfirmDialog { objectName: "a"; destructive: true; defaultCancel: true; onAccepted: x() } ConfirmDialog { objectName: "b"; title: "y" } }\n' > "$tmp/u9ok/qml/D.qml"
    scan "$tmp/u9ok" >/dev/null || { echo "self-test: a destructive dialog with defaultCancel was reported" >&2; rc=1; }
    mk u9multi
    printf 'import QtQuick\nItem {\n ConfirmDialog {\n  id: a\n  destructive: true\n  defaultCancel: true\n }\n ConfirmDialog {\n  id: b\n  title: "t"\n }\n}\n' > "$tmp/u9multi/qml/D.qml"
    scan "$tmp/u9multi" >/dev/null || { echo "self-test: separate blocks were mixed up" >&2; rc=1; }
    mk u9bad
    printf 'import QtQuick\nItem { ConfirmDialog { objectName: "a"; destructive: true; onAccepted: x() } }\n' > "$tmp/u9bad/qml/D.qml"
    if scan "$tmp/u9bad" >/dev/null; then echo "self-test: U9 was not detected (destructive without defaultCancel)" >&2; rc=1; fi
    mk u9bad2
    printf 'import QtQuick\nItem {\n ConfirmDialog {\n  id: ok\n  destructive: true\n  defaultCancel: true\n }\n ConfirmDialog {\n  id: bad\n  destructive: true\n }\n}\n' > "$tmp/u9bad2/qml/D.qml"
    if scan "$tmp/u9bad2" >/dev/null; then echo "self-test: U9 was not detected for the second of two dialogs" >&2; rc=1; fi
    # the qmldir and the resource list are scanned too
    mk bad2
    printf 'InkButton 1.0 InkButton.qml\n' > "$tmp/bad2/qml/qmldir"
    if scan "$tmp/bad2" >/dev/null; then echo "self-test: an InkButton line in qmldir was not detected" >&2; rc=1; fi
    return $rc
}

case "${1:-}" in
    --self-test) self_test; rc=$?; [ $rc -eq 0 ] && echo "check-ui-style self-test passed"; exit $rc ;;
    *) ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
       scan "$ROOT"; rc=$?
       if [ $rc -ne 0 ]; then echo "check-ui-style: the findings above must be fixed" >&2; fi
       exit $rc ;;
esac
