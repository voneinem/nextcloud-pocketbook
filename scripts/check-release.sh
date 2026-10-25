#!/usr/bin/env bash
# Release gate: the tag, the VERSION file, the name of the package and the version inside its manifest.json must be
# the same version (spec 007, FR-013). The store finds the package of a version through the tag, so a mismatch makes
# the package unfindable or wrong.
#
#   scripts/check-release.sh [zip] [tag]    default zip: dist/pbcloud-inkpad-one-<VERSION>.zip, default tag: v<VERSION>
#   scripts/check-release.sh --self-test    prove the rules on throw-away packages
#
# Exit 0: all agree. Exit 1: a mismatch (one line each) or no readable manifest. Exit 2: the package does not exist.
# Needs unzip; the self-test builds its packages with python3.
set -euo pipefail

check() {  # check <repo-root> <zip> <tag>   (empty zip / tag: the defaults)
    local root="$1" zip="$2" tag="$3" ver mver bad=0
    ver="$(head -n 1 "$root/VERSION" 2>/dev/null | tr -d '[:space:]')"
    [ -n "$ver" ] || { echo "release check FAILED: no VERSION file in $root" >&2; return 1; }
    [ -n "$zip" ] || zip="$root/dist/pbcloud-inkpad-one-$ver.zip"
    [ -n "$tag" ] || tag="v$ver"
    [ -f "$zip" ] || { echo "release check: package not found: $zip" >&2; return 2; }

    mver="$(unzip -p "$zip" manifest.json 2>/dev/null \
        | sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1 || true)"
    [ -n "$mver" ] || { echo "release check FAILED: $zip has no readable manifest.json with a version" >&2; return 1; }

    if [ "$tag" != "v$mver" ]; then
        echo "release check FAILED: tag $tag does not match manifest version $mver" >&2; bad=1
    fi
    if [ "$ver" != "$mver" ]; then
        echo "release check FAILED: VERSION $ver does not match manifest version $mver" >&2; bad=1
    fi
    if [ "$(basename "$zip")" != "pbcloud-inkpad-one-$mver.zip" ]; then
        echo "release check FAILED: package name $(basename "$zip") does not match manifest version $mver" >&2; bad=1
    fi
    [ "$bad" = 0 ] || return 1
    echo "release check passed ($mver)"
}

self_test() {
    local tmp root
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' RETURN
    root="$tmp/repo"; mkdir -p "$root"
    echo "1.2.3" > "$root/VERSION"

    # mkzip <name> <manifest-json|->: a package with that file name; "-" means no manifest.json inside
    mkzip() {
        local name="$1" json="$2"
        rm -rf "$tmp/z"; mkdir -p "$tmp/z/applications"; echo x > "$tmp/z/applications/a"
        [ "$json" = "-" ] || printf '%s\n' "$json" > "$tmp/z/manifest.json"
        rm -f "$tmp/$name"
        ( cd "$tmp/z" && python3 -I -m zipfile -c "$tmp/$name" $(ls) )
    }
    expect() {  # expect <want-exit> <label> <zip> <tag> [message-part]
        local want="$1" label="$2" zip="$3" tag="$4" part="${5:-}" rc=0 err
        err="$(check "$root" "$zip" "$tag" 2>&1 >/dev/null)" || rc=$?
        if [ "$rc" != "$want" ]; then echo "self-test failed: $label (exit $rc, wanted $want)" >&2; return 1; fi
        if [ -n "$part" ] && ! printf '%s' "$err" | grep -qF -- "$part"; then
            echo "self-test failed: $label (message lacks '$part': $err)" >&2; return 1
        fi
    }

    mkzip pbcloud-inkpad-one-1.2.3.zip '{"id": "pbcloud", "version": "1.2.3"}'
    expect 0 "all agree" "$tmp/pbcloud-inkpad-one-1.2.3.zip" v1.2.3 || return 1
    expect 1 "tag differs" "$tmp/pbcloud-inkpad-one-1.2.3.zip" v1.2.4 "tag v1.2.4 does not match manifest version 1.2.3" || return 1
    echo "1.2.4" > "$root/VERSION"
    expect 1 "VERSION differs" "$tmp/pbcloud-inkpad-one-1.2.3.zip" v1.2.3 "VERSION 1.2.4 does not match manifest version 1.2.3" || return 1
    echo "1.2.3" > "$root/VERSION"
    mkzip pbcloud-inkpad-one-1.2.4.zip '{"id": "pbcloud", "version": "1.2.3"}'
    expect 1 "ZIP name differs" "$tmp/pbcloud-inkpad-one-1.2.4.zip" v1.2.3 "does not match manifest version 1.2.3" || return 1
    mkzip pbcloud-inkpad-one-1.2.3.zip -
    expect 1 "no manifest" "$tmp/pbcloud-inkpad-one-1.2.3.zip" v1.2.3 "no readable manifest.json" || return 1
    expect 2 "no such package" "$tmp/missing.zip" v1.2.3 "package not found" || return 1
    echo "check-release.sh self-test passed"
}

if [ "${1:-}" = "--self-test" ]; then
    self_test
    exit $?
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
rc=0
check "$ROOT" "${1:-}" "${2:-}" || rc=$?
exit "$rc"
