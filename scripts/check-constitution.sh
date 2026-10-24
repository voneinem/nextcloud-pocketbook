#!/usr/bin/env bash
# Mechanical checks of the project constitution (run by CTest and scripts/pre-merge.sh).
#
#   scripts/check-constitution.sh [repo-root]    scan (default: this repository)
#   scripts/check-constitution.sh --self-test    prove that every rule below detects a violation
#
# Rules (a finding makes the script exit non-zero):
#   R1 TLS verification is never disabled, in code AND shell scripts:
#        curl -k, curl --insecure, wget --no-check-certificate,
#        CURLOPT_SSL_VERIFYPEER / CURLOPT_SSL_VERIFYHOST set to 0 (VERIFYHOST must be 2),
#        CURLOPT_SSL_VERIFYSTATUS, CURLOPT_PROXY_SSL_VERIFYPEER, CURLOPT_PROXY_SSL_VERIFYHOST,
#        CURLOPT_DOH_SSL_VERIFYPEER, CURLOPT_DOH_SSL_VERIFYHOST, CURLOPT_DOH_SSL_VERIFYSTATUS
#        set to 0 / false.
#   R2 No hard-coded SDK locations ("pocketbook-sdk", "/opt/pocketbook") in build files and scripts.
#   R3 core/ is Qt-free and InkView-free.
#   R4 No logging call is given a password, user name, note content or title.
#   R5 No absolute local paths (user home directories, drive letters) in any tracked text file.
#   R6 No log::text / log::count key that is, starts with or ends with the word url, path, file, name
#        or address (e.g. "url", "file_name", "link_url") in core/, bridge/ and app/ (spec 002 FR-033: no link addresses, attachment names or file paths in logs).
#   R7 TLS is required for every mail/calendar connection: CURLOPT_USE_SSL is only ever set to
#        CURLUSESSL_ALL, and the literal schemes imap:// and smtp:// (no TLS) appear in core/, bridge/ and
#        app/ only in core/src/curl_session.cpp (tests/ may use them for the fake servers).
#   R8 No log::text / log::count key that is, starts with or ends with the word host, folder, subject, from,
#        to, recipient, title, summary, attendee, location, contact, phone, email, book, org, note, path, name, file, filename, url, address,
#        server, remote, local, rel, etag, password, secret or account in core/, bridge/ and app/ (no mail or calendar content, no server names
#        and no names or paths of the user's files in logs; counts such as files, folders or entries are fine).
#   R9 The identifier pbnotes (the application this one was started from) appears in no tracked file except
#        under docs/reference/, in NOTICE and under licenses/ (the planning documents in specs/ and .specify/
#        describe the rename, tests/templates/ holds the old perf tests until they are rewritten).
#   R11 The identifier pboffice (PB Office, the application this one was bootstrapped from) appears in no tracked file
#        except under docs/reference/, in NOTICE and under licenses/ (specs/ and .specify/ describe the origin), and in two
#        files that address its launcher for the one permitted hand-off call (constitution 1.1.0): app/probe/pbc_handoff.cpp
#        and docs/device-findings.md. The two apps must never share a binary name, a launcher entry or a data directory.
#   N1 No plaintext http:// URL literal in core/, bridge/, app/ or qml/*.qml (comments excepted). XML namespace URIs of the DAV
#        vocabularies (owncloud.org, nextcloud.org, calendarserver.org, sabredav.org, www.w3.org) are protocol identifiers, not endpoints,
#        and are on an explicit exempt list.
#   N2 HTTP write methods (PUT, MKCOL, MOVE, COPY, DELETE) as a string literal only in core/src/dav_write.cpp, queue_sender.cpp and
#        upload_session.cpp (feature 002: every write carries its condition there, tests/integration/test_write_conditions.cpp). The
#        generic transport (curl_transport.cpp, http_transport.cpp, dav_common.cpp) carries any method and is not scanned. PROPPATCH, LOCK
#        and UNLOCK are never sent by this app: they appear nowhere else and not even in the audited modules. The bridge, the app and
#        the QML name no method at all - except the device probe (app/probe/pbc_probe.cpp), a developer's console tool that sends one PUT of its
#        own temporary file, only when PBC_PROBE_PUT_URL names a test address by hand (DT-W1); it names no other write method.
#   N3 No QTimer, QBasicTimer or sleep in core/ and bridge/ except core/src/downloader.cpp and its header (no background activity; the
#        download wait), bridge/src/queue_bridge.cpp with its header, which may hold the one single-shot QTimer of Auto Sync (never sleep), and
#        bridge/src/sync_bridge.cpp with its header, which may hold exactly one QTimer (the single-shot timer of the per-pair Auto Sync of feature
#        003; never a QBasicTimer, never sleep, never a second timer). core/src/sync_*.cpp stay forbidden.
#   N4 No log field named href, term, user, login, token, query, search, link, etag, checksum or upload_id in core/, bridge/ and app/ (server data and user
#        input stay out of the log).
#   N6 The files an upload reads are only ever read: no O_WRONLY, O_RDWR, O_CREAT, O_TRUNC, O_APPEND, write-mode fopen, ofstream, std::ios::out,
#        QIODevice write modes or QFile::remove/rename/copy in core/src/queue_sender.cpp, core/src/upload_source.cpp and
#        bridge/src/picker_bridge.cpp.
#   N5 No unlink/remove/rename/rmdir/truncate in core/src outside local_files.cpp, downloader.cpp, credentials_file.cpp, ca_selector.cpp,
#        curl_transport.cpp and store*.cpp (all file mutation goes through the audited modules; the folder synchronization of feature 003
#        - sync_*.cpp - removes and renames nothing itself: it asks local_files.cpp, the Downloader or the queue).
#   N7 Paths are never joined by hand in core/src/sync_runner.cpp, sync_scan.cpp and sync_planner.cpp (feature 003): no `x + "/" + y`
#        outside a function named join_path_checked, which refuses ".." and separators in a component. Every reader path of a sync is built
#        by that function or by choose_local_path, every server path by remote_path.h.
#   N2 (feature 004) core/src/share_api.cpp is a fourth audited module: it may name PUT and DELETE (the share requests); it names no other
#        write verb. Every other share_* module and the share bridge name none.
#   N8 The share modules (core/src/share_*.cpp, core/include/pbc/share_*.h, secret_text.*, bridge/src/share_bridge.cpp and its header) cannot log
#        secrets (feature 004): no log::text, log::path or log::Field, no qDebug/qWarning/qInfo/qCritical, std::cerr/cout/clog or printf family,
#        every log::event code starts with "share.", and SecretText::reveal() is called only in share_api.cpp (the form encoder).
#   N9 Nothing opens a link (feature 004): no QProcess, system()/popen(), xdg-open, openUrlExternally/Qt.openUrl, QUrl::fromUserInput or
#        QDesktopServices in core/, bridge/, app/ and qml/*.qml - except the one hand-over of a downloaded local file to the viewer in
#        app/device_bridge.cpp (QDesktopServices::openUrl(QUrl::fromLocalFile(...))).
#   R10 libcurl tracing is never switched on (it would write LOGIN / AUTH PLAIN passwords to the log):
#        CURLOPT_VERBOSE, CURLOPT_DEBUGFUNCTION, CURLOPT_DEBUGDATA, CURLOPT_STDERR, curl -v / --verbose /
#        --trace* in core/, bridge/, app/ and scripts/.
set -u

SELF="$(basename "${BASH_SOURCE[0]}")"

# Lists the text files to scan, relative to $1. Uses git when available, else find.
list_files() {
    local root="$1"
    if [ -d "$root/.git" ] && command -v git >/dev/null 2>&1; then
        git -C "$root" ls-files -z --cached --others --exclude-standard
    else
        (cd "$root" && find . -type f -not -path './.git/*' -not -path './build*' -print0 | sed -z 's#^\./##')
    fi
}

# report <rule> <file-label> <path> <regex>: prints the matching lines and returns 0 when there are any.
report() {
    local out
    out="$(grep -nE -- "$4" "$3" 2>/dev/null)" || return 1
    printf '%s\n' "$out" | sed "s#^#$1 $2:#"
    return 0
}

scan() {
    local root="$1" bad=0 f out
    while IFS= read -r -d '' f; do
        [ -f "$root/$f" ] || continue
        case "$f" in
            # .specify/extensions/ holds vendored Spec Kit extensions (their own docs, not this project's text)
            third_party/*|"scripts/$SELF"|.claude/*|.specify/extensions/*|*.bmp|*.zip|*.pem) continue ;;
        esac
        grep -Iq . "$root/$f" 2>/dev/null || continue   # skip binary/empty files
        local p="$root/$f"

        # R5: absolute local paths, everywhere
        if grep -nE '/home/[A-Za-z0-9._-]+/|/Users/[A-Za-z0-9._-]+/|(^|[^A-Za-z0-9])[A-Za-z]:\\[A-Za-z0-9_.-]' "$p" >/dev/null 2>&1; then
            grep -nE '/home/[A-Za-z0-9._-]+/|/Users/[A-Za-z0-9._-]+/|(^|[^A-Za-z0-9])[A-Za-z]:\\[A-Za-z0-9_.-]' "$p" | sed "s#^#R5 $f:#"
            bad=1
        fi

        case "$f" in
            core/*|bridge/*|app/*|tests/*|scripts/*|platform/*|cmake/*|CMakeLists.txt|*/CMakeLists.txt)
                # R1: TLS bypasses
                if grep -nE '(curl|wget)[^#|;&]*[[:space:]](-k|--insecure|--no-check-certificate)([[:space:]]|$)' "$p" >/dev/null 2>&1 ||
                   grep -nE 'CURLOPT_(PROXY_|DOH_)?SSL_VERIFY(PEER|HOST|STATUS)[[:space:]]*,[[:space:]]*(0|false|FALSE)' "$p" >/dev/null 2>&1 ||
                   grep -nE 'CURLOPT_SSL_VERIFYHOST[[:space:]]*,[[:space:]]*1[^0-9]' "$p" >/dev/null 2>&1 ||
                   grep -nE 'CURLOPT_SSL_VERIFYSTATUS' "$p" >/dev/null 2>&1; then
                    grep -nE '(curl|wget)[^#|;&]*[[:space:]](-k|--insecure|--no-check-certificate)([[:space:]]|$)|CURLOPT_(PROXY_|DOH_)?SSL_VERIFY(PEER|HOST|STATUS)[[:space:]]*,[[:space:]]*(0|false|FALSE|1[^0-9])|CURLOPT_SSL_VERIFYSTATUS' "$p" | sed "s#^#R1 $f:#"
                    bad=1
                fi
                ;;
        esac
        case "$f" in
            cmake/*|CMakeLists.txt|*/CMakeLists.txt|scripts/*|platform/*)
                # R2: hard-coded SDK locations
                if grep -nE 'pocketbook-sdk|/opt/pocketbook' "$p" >/dev/null 2>&1; then
                    grep -nE 'pocketbook-sdk|/opt/pocketbook' "$p" | sed "s#^#R2 $f:#"
                    bad=1
                fi
                ;;
        esac
        case "$f" in
            core/*)
                # R3: no Qt / InkView in the core
                if grep -nE '#[[:space:]]*include[[:space:]]*[<"](Q[A-Za-z]|inkview)|\bQObject\b|\bQString\b' "$p" >/dev/null 2>&1; then
                    grep -nE '#[[:space:]]*include[[:space:]]*[<"](Q[A-Za-z]|inkview)|\bQObject\b|\bQString\b' "$p" | sed "s#^#R3 $f:#"
                    bad=1
                fi
                ;;
        esac
        case "$f" in
            core/*|bridge/*|app/*)
                # R4: personal data into the log
                if grep -nE '(log::|qDebug|qWarning|qInfo|std::cerr|fprintf\(stderr)[^;]*(password|passwd|username|\.content|->content|\.title|->title)' "$p" >/dev/null 2>&1; then
                    grep -nE '(log::|qDebug|qWarning|qInfo|std::cerr|fprintf\(stderr)[^;]*(password|passwd|username|\.content|->content|\.title|->title)' "$p" | sed "s#^#R4 $f:#"
                    bad=1
                fi
                # R6: log keys that would carry addresses, names or paths
                if grep -nE 'log::(text|count)[[:space:]]*\([[:space:]]*"([A-Za-z0-9.]+_)?(url|path|file|name|address)(_[A-Za-z0-9_.]+)?"' "$p" >/dev/null 2>&1; then
                    grep -nE 'log::(text|count)[[:space:]]*\([[:space:]]*"([A-Za-z0-9.]+_)?(url|path|file|name|address)(_[A-Za-z0-9_.]+)?"' "$p" | sed "s#^#R6 $f:#"
                    bad=1
                fi
                ;;
        esac
        # R9: the old application's identifier (any file type, minus the allowed places)
        case "$f" in
            docs/reference/*|NOTICE|licenses/*|specs/*|.specify/*|tests/templates/*) ;;
            *) report R9 "$f" "$p" 'pbnotes' && bad=1 ;;
        esac
        # R11: PB Office's identifier (any file type, minus the allowed places)
        case "$f" in
            docs/reference/*|NOTICE|licenses/*|specs/*|.specify/*|app/probe/pbc_handoff.cpp|docs/device-findings.md) ;;
            *) report R11 "$f" "$p" 'pboffice' && bad=1 ;;
        esac
        case "$f" in
            core/*|bridge/*|app/*)
                # R7: TLS always on, no clear-text URL schemes outside the one place that builds URLs
                out="$(grep -nE 'CURLOPT_USE_SSL' "$p" 2>/dev/null | grep -v 'CURLUSESSL_ALL')" && {
                    printf '%s\n' "$out" | sed "s#^#R7 $f:#"; bad=1; }
                if [ "$f" != core/src/curl_session.cpp ]; then
                    report R7 "$f" "$p" '(imap|smtp)://' && bad=1
                fi
                # R8: log keys that would carry mail or calendar content or server names
                report R8 "$f" "$p" 'log::(text|count)[[:space:]]*\([[:space:]]*"([A-Za-z0-9.]+_)?(host|folder|subject|from|to|recipient|title|summary|attendee|location|contact|phone|email|book|org|note|path|name|file|filename|url|address|server|remote|local|rel|etag|password|secret|account)(_[A-Za-z0-9_.]+)?"' && bad=1
                ;;
        esac
        case "$f" in
            core/*|bridge/*|app/*|scripts/*)
                # R10: no libcurl tracing
                report R10 "$f" "$p" 'CURLOPT_(VERBOSE|DEBUGFUNCTION|DEBUGDATA|STDERR)|(^|[^A-Za-z0-9_-])curl[^#|;&]*[[:space:]](-[A-Za-z]*v[A-Za-z]*|--verbose|--trace[a-z-]*)([[:space:]]|$)' && bad=1
                ;;
        esac
        # N1: no plaintext endpoint (XML namespace URIs are exempt)
        case "$f" in
            core/*|bridge/*|app/*|qml/*.qml)
                out="$(grep -nE 'http://' "$p" 2>/dev/null |
                       grep -vE '^[0-9]+:[[:space:]]*(//|\*|#|/\*)' |
                       grep -vE '^[0-9]+:[^"'"'"']*[[:space:]]//[[:space:]].*http://' |
                       grep -vE 'http://(www\.w3\.org|owncloud\.org|nextcloud\.org|calendarserver\.org|sabredav\.org)(/|"|\\|$)')" && {
                    printf '%s\n' "$out" | sed "s#^#N1 $f:#"; bad=1; }
                ;;
        esac
        # N2: write methods only in the audited modules; PROPPATCH, LOCK and UNLOCK nowhere
        case "$f" in
            core/src/dav_write.cpp|core/src/queue_sender.cpp|core/src/upload_session.cpp)
                report N2 "$f" "$p" '"(PROPPATCH|LOCK|UNLOCK)"' && bad=1
                ;;
            core/src/share_api.cpp)
                # feature 004: the share requests (POST, PUT, DELETE on the sharing resources) live here and nowhere else; no WebDAV write verb
                report N2 "$f" "$p" '"(MKCOL|MOVE|COPY|PROPPATCH|LOCK|UNLOCK)"' && bad=1
                ;;
            core/src/curl_transport.cpp|core/src/http_transport.cpp|core/src/dav_common.cpp) ;;
            app/probe/pbc_probe.cpp)
                report N2 "$f" "$p" '"(MKCOL|MOVE|COPY|DELETE|PROPPATCH|LOCK|UNLOCK)"' && bad=1
                ;;
            core/*|bridge/*|app/*|qml/*.qml)
                report N2 "$f" "$p" '"(PUT|MKCOL|MOVE|COPY|DELETE|PROPPATCH|LOCK|UNLOCK)"' && bad=1
                ;;
        esac
        # N3: no background activity: no timers and no sleeping outside the download wait; the queue bridge may hold its one QTimer
        case "$f" in
            core/src/downloader.cpp|core/include/pbc/downloader.h) ;;
            bridge/src/queue_bridge.cpp|bridge/include/queue_bridge.h)
                report N3 "$f" "$p" '\bQBasicTimer\b|sleep_for|\busleep[[:space:]]*\(|\bnanosleep[[:space:]]*\(|(^|[^A-Za-z0-9_])sleep[[:space:]]*\(' && bad=1
                ;;
            bridge/src/sync_bridge.cpp|bridge/include/sync_bridge.h)
                report N3 "$f" "$p" '\bQBasicTimer\b|sleep_for|\busleep[[:space:]]*\(|\bnanosleep[[:space:]]*\(|(^|[^A-Za-z0-9_])sleep[[:space:]]*\(' && bad=1
                # exactly one timer: more than one instance (new QTimer, or a QTimer value) is a second timer
                if [ "$(grep -oE 'new[[:space:]]+QTimer|QTimer[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*(;|\{|\(|=)' "$p" 2>/dev/null | wc -l)" -gt 1 ]; then
                    echo "N3 $f: more than one QTimer"; bad=1
                fi
                ;;
            core/*|bridge/*)
                report N3 "$f" "$p" '\bQ(Basic)?Timer\b|sleep_for|\busleep[[:space:]]*\(|\bnanosleep[[:space:]]*\(|(^|[^A-Za-z0-9_])sleep[[:space:]]*\(' && bad=1
                ;;
        esac
        # N4: server data and user input stay out of the log
        case "$f" in
            core/*|bridge/*|app/*)
                report N4 "$f" "$p" 'log::(text|count)[[:space:]]*\([[:space:]]*"([A-Za-z0-9.]+_)?(href|term|user|login|token|query|search|link|etag|checksum|upload_id)(_[A-Za-z0-9_.]+)?"' && bad=1
                ;;
        esac
        # N5: file mutation only in the audited modules
        case "$f" in
            core/src/local_files.cpp|core/src/downloader.cpp|core/src/credentials_file.cpp|core/src/ca_selector.cpp|core/src/curl_transport.cpp|core/src/store*.cpp) ;;
            core/src/*.cpp)
                report N5 "$f" "$p" '(^|[^A-Za-z0-9_.>])(::|std::)?(unlink|remove|rename|rmdir|truncate|ftruncate)[[:space:]]*\(|filesystem::(remove|remove_all|rename|resize_file)' && bad=1
                ;;
        esac
        # N6: the files an upload reads are only ever read
        case "$f" in
            core/src/queue_sender.cpp|core/src/upload_source.cpp|bridge/src/picker_bridge.cpp)
                report N6 "$f" "$p" 'O_WRONLY|O_RDWR|O_CREAT|O_TRUNC|O_APPEND|fopen[[:space:]]*\([^)]*"[wa]b?\+?"|"r\+b?"|std::ios::out|ofstream|QIODevice::(WriteOnly|ReadWrite|Append|Truncate|NewOnly)|QFile::(remove|rename|copy)' && bad=1
                ;;
        esac
        # N7: no hand-made path joins in the sync modules (feature 003): only inside join_path_checked
        case "$f" in
            core/src/sync_runner.cpp|core/src/sync_scan.cpp|core/src/sync_planner.cpp)
                out="$(awk '
                    # a line that defines join_path_checked starts the exempt region; it ends with the closing brace of its body
                    { line = $0
                      if (!infn && line ~ /join_path_checked[[:space:]]*\(/ && line !~ /;[[:space:]]*$/) { infn = 1; depth = 0; seen = 0 }
                      if (infn) {
                          n = gsub(/\{/, "{", line); m = gsub(/\}/, "}", line)
                          depth += n - m; if (n > 0) seen = 1
                          if (seen && depth <= 0) infn = 0
                          next
                      }
                      if ($0 ~ /\+[[:space:]]*"\/"|"\/"[[:space:]]*\+|\+[[:space:]]*\x27\/\x27/) print NR ":" $0
                    }' "$p")" && [ -n "$out" ] && {
                    printf '%s\n' "$out" | sed "s#^#N7 $f:#"; bad=1; }
                ;;
        esac
        # N8: the share modules cannot log (feature 004: links and passwords are secrets, constitution IV): no log::text/log::path/log::Field, no Qt or C
        # logging, only share.* event codes, and SecretText::reveal() only in share_api.cpp (the form encoder)
        case "$f" in
            core/src/share_*.cpp|core/include/pbc/share_*.h|core/src/secret_text.cpp|core/include/pbc/secret_text.h|bridge/src/share_bridge.cpp|bridge/include/share_bridge.h)
                report N8 "$f" "$p" 'log::(text|path|Field)|(^|[^A-Za-z0-9_])(qDebug|qWarning|qInfo|qCritical)[[:space:]]*\(|std::(cerr|cout|clog)|(^|[^A-Za-z0-9_])(printf|fprintf|puts|fputs|perror)[[:space:]]*\(' && bad=1
                out="$(grep -nE 'log::event[[:space:]]*\([[:space:]]*"' "$p" 2>/dev/null | grep -vE 'log::event[[:space:]]*\([[:space:]]*"share\.')" && {
                    printf '%s\n' "$out" | sed "s#^#N8 $f:#"; bad=1; }
                case "$f" in
                    core/src/share_api.cpp|core/src/secret_text.cpp|core/include/pbc/secret_text.h) ;;
                    *) report N8 "$f" "$p" '\.reveal[[:space:]]*\(' && bad=1 ;;
                esac
                ;;
        esac
        # N9: nothing opens a link (feature 004): no browser, no process, no shell, no URL opener in the code or the QML; the one hand-over of a
        # downloaded local file to the reader's viewer (app/device_bridge.cpp) may use QDesktopServices::openUrl(QUrl::fromLocalFile(...)) only
        case "$f" in
            core/*|bridge/*|app/*|qml/*.qml)
                report N9 "$f" "$p" 'QProcess|(^|[^A-Za-z0-9_:.>])(system|popen)[[:space:]]*\(|std::(system|popen)[[:space:]]*\(|xdg-open|openUrlExternally|Qt\.openUrl|QUrl::fromUserInput' && bad=1
                case "$f" in
                    app/device_bridge.cpp)
                        out="$(grep -nE 'QDesktopServices' "$p" 2>/dev/null | grep -vE '#include <QDesktopServices>|QDesktopServices::openUrl\(QUrl::fromLocalFile\(')" && {
                            printf '%s\n' "$out" | sed "s#^#N9 $f:#"; bad=1; }
                        ;;
                    *) report N9 "$f" "$p" 'QDesktopServices' && bad=1 ;;
                esac
                ;;
        esac
    done < <(list_files "$root")
    # T1: no test that CMake does not build or run (a stale test must not be claimed as passing): C++ tests only in the directories CMake globs or lists,
    # every bridge test named in tests/CMakeLists.txt, every walk named in app/CMakeLists.txt
    while IFS= read -r -d '' f; do
        f="${f#"$root"/}"
        case "$f" in
            tests/unit/*|tests/integration/*|tests/templates/*) ;;
            tests/bridge/*) grep -q "$(basename "$f" .cpp)" "$root/tests/CMakeLists.txt" 2>/dev/null || { echo "T1 $f: a bridge test that tests/CMakeLists.txt does not build"; bad=1; } ;;
            *) echo "T1 $f: a test file outside the directories that CMake builds"; bad=1 ;;
        esac
    done < <(find "$root/tests" -name 'test_*.cpp' -print0 2>/dev/null)
    while IFS= read -r -d '' f; do
        grep -q "$(basename "$f")" "$root/app/CMakeLists.txt" 2>/dev/null || { echo "T1 ${f#"$root"/}: a walk that app/CMakeLists.txt does not run"; bad=1; }
    done < <(find "$root/tests/walks" -name '*.walk' -print0 2>/dev/null)
    return $bad
}

if [ "${1:-}" = "--self-test" ]; then
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    fail=0
    mkdir -p "$tmp/core/src" "$tmp/scripts" "$tmp/cmake" "$tmp/docs" "$tmp/bridge" "$tmp/app" "$tmp/platform"
    expect() {  # expect <pass|fail> <description>
        scan "$tmp" >/dev/null 2>&1
        local rc=$?
        if { [ "$1" = pass ] && [ $rc -eq 0 ]; } || { [ "$1" = fail ] && [ $rc -ne 0 ]; }; then
            echo "ok   $2"
        else
            echo "FAIL $2 (rc=$rc)"; fail=1
        fi
    }
    violate() {  # violate <file> <content> <description>
        printf '%s\n' "$2" > "$tmp/$1"
        expect fail "$3"
        rm -f "$tmp/$1"
    }
    printf 'int main() { return 0; }\n' > "$tmp/core/src/ok.cpp"
    printf '#!/bin/sh\necho hello\n' > "$tmp/scripts/ok.sh"
    expect pass "a clean tree passes"
    printf 'curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);\ncurl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);\n' > "$tmp/core/src/tls_ok.cpp"
    expect pass "VERIFYPEER 1 / VERIFYHOST 2 are allowed"; rm "$tmp/core/src/tls_ok.cpp"

    violate scripts/a.sh 'curl -k https://example.com' "R1: curl -k"
    violate scripts/a.sh 'curl -sS --insecure https://example.com' "R1: curl --insecure"
    violate scripts/a.sh 'wget --no-check-certificate https://example.com' "R1: wget --no-check-certificate"
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);' "R1: VERIFYPEER 0"
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);' "R1: VERIFYHOST 0"
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 1L);' "R1: VERIFYHOST 1"
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_SSL_VERIFYSTATUS, 0L);' "R1: VERIFYSTATUS"
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_PROXY_SSL_VERIFYPEER, 0L);' "R1: PROXY_SSL_VERIFYPEER 0"
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_DOH_SSL_VERIFYHOST, false);' "R1: DOH_SSL_VERIFYHOST false"
    violate cmake/a.cmake 'set(SDK_PATH /opt/pocketbook-sdk/x)' "R2: /opt/pocketbook path"
    violate scripts/a.sh 'SDK=../pocketbook-sdk-6.11/sdk' "R2: pocketbook-sdk path"
    violate core/src/a.cpp '#include <QString>' "R3: Qt include in core"
    violate core/src/a.cpp '#include <inkview.h>' "R3: inkview include in core"
    violate core/src/a.cpp 'log::text("x", account.password);' "R4: password into the log"
    violate core/src/a.cpp 'log::text("x", note.content);' "R4: note content into the log"
    violate core/src/a.cpp 'log::text("url", link);' "R6: url key in the log"
    violate core/src/a.cpp 'log::text("file_name", n);' "R6: file_name key in the log"
    violate core/src/a.cpp 'log::count("path", 1);' "R6: path key in the log"
    violate core/src/a.cpp 'log::text("address", a);' "R6: address key in the log"
    printf 'log::text("result", r); log::count("pulled", 3); log::path(p);\n' > "$tmp/core/src/log_ok.cpp"
    expect pass "result/count keys and log::path are allowed"; rm "$tmp/core/src/log_ok.cpp"
    # R7: TLS is always required (USE_SSL == ALL) and URL schemes are built in one place
    printf 'curl_easy_setopt(c, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);\n' > "$tmp/core/src/ssl_ok.cpp"
    expect pass "R7: CURLOPT_USE_SSL with CURLUSESSL_ALL is allowed"; rm "$tmp/core/src/ssl_ok.cpp"
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_USE_SSL, CURLUSESSL_TRY);' "R7: USE_SSL TRY"
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_USE_SSL, CURLUSESSL_CONTROL);' "R7: USE_SSL CONTROL"
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_USE_SSL, 0L);' "R7: USE_SSL 0"
    violate core/src/a.cpp 'const char* u = "imap://mail.example.org/INBOX";' "R7: imap:// literal in core"
    violate core/src/a.cpp 'std::string u = std::string("smtp://") + host;' "R7: smtp:// literal in core"
    violate bridge/a.cpp 'auto u = "imap://x";' "R7: imap:// literal in bridge"
    mkdir -p "$tmp/tests"
    printf 'const char* u = "imap://localhost:1/";\n' > "$tmp/tests/t.cpp"
    printf 'const char* u = "smtp://localhost:1/";\n' > "$tmp/core/src/curl_session.cpp"
    expect pass "R7: imap:// / smtp:// literals are allowed in tests/ and core/src/curl_session.cpp"
    rm -f "$tmp/tests/t.cpp" "$tmp/core/src/curl_session.cpp"
    printf 'const char* u = "imaps://localhost/";\n' > "$tmp/core/src/imaps.cpp"
    expect pass "R7: imaps:// / smtps:// are not matched"; rm "$tmp/core/src/imaps.cpp"
    # R8: no mail/calendar content or server names as log fields
    for w in host folder subject from to recipient title summary attendee location; do
        violate core/src/a.cpp "log::text(\"$w\", v);" "R8: log key $w"
    done
    for w in contact phone email book org note; do   # spec 002 FR-027: no contact data in logs
        violate core/src/a.cpp "log::text(\"$w\", v);" "R8: log key $w"
    done
    for w in path name file filename url address server remote local rel etag password secret account; do   # spec 001: no file names, paths or server data in logs
        violate core/src/a.cpp "log::text(\"$w\", v);" "R8: log key $w"
    done
    violate core/src/a.cpp 'log::text("remote_path", v);' "R8: log key remote_path"
    violate bridge/a.cpp 'log::text("file_name", v);' "R8: log key file_name"
    violate core/src/a.cpp 'log::count("contact_name_len", 1);' "R8: log key contact_name_len"
    violate bridge/a.cpp 'log::text("primary_email", v);' "R8: log key primary_email"
    violate bridge/a.cpp 'log::text("sender_from", v);' "R8: log key sender_from"
    violate app/a.cpp 'log::count("folder_name_len", 1);' "R8: log key folder_name_len"
    printf 'log::text("result", r); log::count("pulled", 3); log::text("tofu", x); log::count("books", 2); log::count("contacts", 5); log::count("notes_total", 1); log::count("files", 4); log::count("folders", 2); log::count("entries", 9); log::text("source", s);\n' > "$tmp/core/src/log_ok.cpp"
    expect pass "R8: other keys (also tofu, books, contacts as counts) are allowed"; rm "$tmp/core/src/log_ok.cpp"
    # R9: the old application's identifier stays out of this one
    violate docs/a.md 'see pbnotes for the idea' "R9: pbnotes in docs"
    violate core/src/a.cpp 'namespace pbnotes {}' "R9: pbnotes in code"
    mkdir -p "$tmp/docs/reference" "$tmp/licenses"
    mkdir -p "$tmp/specs/x"; printf 'pbnotes renamed\n' > "$tmp/specs/x/plan.md"
    printf 'pbnotes analysis\n' > "$tmp/docs/reference/a.md"
    printf 'pbnotes was the origin\n' > "$tmp/NOTICE"
    printf 'pbnotes\n' > "$tmp/licenses/a.txt"
    expect pass "R9: pbnotes is allowed in docs/reference/, NOTICE, licenses/ and specs/"
    rm -rf "$tmp/docs/reference" "$tmp/NOTICE" "$tmp/licenses/a.txt" "$tmp/specs"
    printf 'PB Notes and notes-pocketbook are fine\n' > "$tmp/docs/b.md"
    expect pass "R9: only the identifier pbnotes is matched"; rm "$tmp/docs/b.md"
    # R11: PB Office's identifier stays out of this one
    violate platform/a.app 'exec /mnt/ext1/applications/pboffice.bin' "R11: pboffice in a launcher"
    violate CMakeLists.txt 'set(PBC_APP_NAME "pboffice")' "R11: pboffice in a build file"
    mkdir -p "$tmp/docs/reference" "$tmp/licenses"
    printf 'pboffice analysis\n' > "$tmp/docs/reference/a.md"
    printf 'pboffice was the origin\n' > "$tmp/NOTICE"
    printf 'id: pboffice\n' > "$tmp/licenses/a.txt"
    expect pass "R11: pboffice is allowed in docs/reference/, NOTICE and licenses/"
    rm -rf "$tmp/docs/reference" "$tmp/NOTICE" "$tmp/licenses/a.txt"
    printf 'PB Office and PBC are fine\n' > "$tmp/docs/b.md"
    expect pass "R11: only the identifier pboffice is matched"; rm "$tmp/docs/b.md"
    mkdir -p "$tmp/app/probe"
    printf 'const char* k = "/mnt/ext1/applications/pboffice.app";\n' > "$tmp/app/probe/pbc_handoff.cpp"
    printf 'the launcher pboffice.app returned 0\n' > "$tmp/docs/device-findings.md"
    expect pass "R11: pboffice is allowed in the hand-off probe and docs/device-findings.md"
    rm -f "$tmp/app/probe/pbc_handoff.cpp" "$tmp/docs/device-findings.md"
    violate app/probe/other.cpp 'const char* k = "/mnt/ext1/applications/pboffice.app";' "R11: pboffice in another probe file"
    violate docs/other.md 'the launcher pboffice.app' "R11: pboffice in another document"
    # R10: libcurl tracing would write LOGIN / AUTH PLAIN passwords to the log
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_VERBOSE, 1L);' "R10: CURLOPT_VERBOSE"
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_DEBUGFUNCTION, cb);' "R10: CURLOPT_DEBUGFUNCTION"
    violate bridge/a.cpp 'curl_easy_setopt(c, CURLOPT_DEBUGDATA, p);' "R10: CURLOPT_DEBUGDATA"
    violate app/a.cpp 'curl_easy_setopt(c, CURLOPT_STDERR, f);' "R10: CURLOPT_STDERR"
    violate scripts/a.sh 'curl -v imaps://mail.example.org' "R10: curl -v"
    violate scripts/a.sh 'curl -sSv https://example.org' "R10: curl -sSv"
    violate scripts/a.sh 'curl --verbose https://example.org' "R10: curl --verbose"
    violate scripts/a.sh 'curl --trace-ascii - https://example.org' "R10: curl --trace-ascii"
    printf 'curl -sS --max-time 5 https://example.org\n' > "$tmp/scripts/curl_ok.sh"
    expect pass "R10: curl without tracing is allowed"; rm "$tmp/scripts/curl_ok.sh"
    # N1: no plaintext http:// endpoint; XML namespace URIs and comments are fine
    mkdir -p "$tmp/qml" "$tmp/tests" "$tmp/bridge/src" "$tmp/core/include"
    violate core/src/a.cpp 'const char* u = "http://cloud.example.org/x";' "N1: http:// literal in core"
    violate bridge/a.cpp 'auto u = QStringLiteral("http://cloud.example.org");' "N1: http:// literal in bridge"
    violate app/a.cpp 'const char* u = "http://localhost/x";' "N1: http:// literal in app"
    violate qml/A.qml 'Item { property string u: "http://cloud.example.org" }' "N1: http:// literal in QML"
    printf 'inline constexpr const char* kNsOc = "http://owncloud.org/ns";\nconst char* b = "xmlns:nc=\\"http://nextcloud.org/ns\\"";\n// see http://example.org/doc\nint x; // docs: http://example.org/y\n' > "$tmp/core/src/ns_ok.cpp"
    expect pass "N1: namespace URIs and comments are allowed"; rm "$tmp/core/src/ns_ok.cpp"
    printf 'const char* u = "http://localhost:1/";\n' > "$tmp/tests/t.cpp"
    expect pass "N1: http:// literals are allowed in tests/"; rm "$tmp/tests/t.cpp"
    # N2: write methods only in the audited modules (feature 002); PROPPATCH, LOCK and UNLOCK nowhere
    for m in PUT MKCOL MOVE COPY DELETE PROPPATCH LOCK UNLOCK; do
        violate core/src/nc_files.cpp "req.method = \"$m\";" "N2: method $m in a request builder"
    done
    violate core/src/downloader.cpp 'r.method = "DELETE";' "N2: DELETE in the downloader"
    violate core/src/update.cpp 'r.method = "COPY";' "N2: COPY in the updater"
    violate core/src/queue_helpers.cpp 'r.method = "PUT";' "N2: PUT in another core module"
    violate core/src/store_queue.cpp 'r.method = "MOVE";' "N2: MOVE in the store"
    violate bridge/a.cpp 'r.method = "MKCOL";' "N2: MKCOL in the bridge"
    violate bridge/src/queue_bridge.cpp 'r.method = "DELETE";' "N2: DELETE in the queue bridge"
    violate app/a.cpp 'r.method = "PUT";' "N2: PUT in the app"
    violate qml/Page.qml 'Item { property string m: "MOVE" }' "N2: MOVE in QML"
    mkdir -p "$tmp/app/probe"
    violate app/probe/pbc_probe.cpp 'r.method = "DELETE";' "N2: DELETE in the device probe (only its one PUT is allowed)"
    for f in dav_write queue_sender upload_session; do
        for m in PROPPATCH LOCK UNLOCK; do
            violate core/src/$f.cpp "req.method = \"$m\";" "N2: $m in $f.cpp (never sent by this app)"
        done
    done
    for f in dav_write queue_sender upload_session curl_transport; do
        printf 'r.method = "PUT"; r.method = "MKCOL"; r.method = "MOVE"; r.method = "COPY"; r.method = "DELETE";\n' > "$tmp/core/src/$f.cpp"
    done
    printf 'r.method = "PROPFIND"; r.method = "GET"; r.method = "REPORT"; r.method = "SEARCH"; r.method = "POST";\nstd::string sql = "DELETE FROM folders";\n' > "$tmp/core/src/nc_listing.cpp"
    expect pass "N2: the audited modules may name the write verbs, read methods and SQL text are allowed"
    rm -f "$tmp/core/src/dav_write.cpp" "$tmp/core/src/queue_sender.cpp" "$tmp/core/src/upload_session.cpp" "$tmp/core/src/curl_transport.cpp" "$tmp/core/src/nc_listing.cpp"
    printf 'r.method = "PROPPATCH"; r.method = "LOCK"; r.method = "UNLOCK";\n' > "$tmp/core/src/curl_transport.cpp"
    expect pass "N2: the generic transport may carry any method"
    rm -f "$tmp/core/src/curl_transport.cpp"
    printf 'r.method = "PUT";\n' > "$tmp/app/probe/pbc_probe.cpp"
    expect pass "N2: the device probe may name PUT (DT-W1), nothing else"
    rm -f "$tmp/app/probe/pbc_probe.cpp"
    # N3: no timers, no sleeping, except the download wait and the one Auto Sync timer of the queue bridge
    violate bridge/src/a.cpp 'QTimer t;' "N3: QTimer in the bridge"
    violate bridge/src/browse_bridge.cpp 'QTimer t;' "N3: QTimer in the browse bridge"
    violate bridge/src/a.cpp 'QBasicTimer t;' "N3: QBasicTimer in the bridge"
    violate core/src/a.cpp 'std::this_thread::sleep_for(std::chrono::seconds(1));' "N3: sleep_for in the core"
    violate core/src/queue_sender.cpp 'std::this_thread::sleep_for(std::chrono::seconds(1));' "N3: sleep_for in the sender"
    violate core/src/a.cpp 'usleep(1000);' "N3: usleep in the core"
    violate core/src/a.cpp 'sleep(1);' "N3: sleep in the core"
    violate bridge/src/queue_bridge.cpp 'std::this_thread::sleep_for(std::chrono::seconds(1));' "N3: sleep_for in the queue bridge"
    violate bridge/src/queue_bridge.cpp 'QBasicTimer t;' "N3: QBasicTimer in the queue bridge (only QTimer is allowed there)"
    violate core/src/auto_sync.cpp 'QTimer t;' "N3: QTimer in the core"
    mkdir -p "$tmp/bridge/include"
    printf 'std::this_thread::sleep_for(std::chrono::seconds(5));\n' > "$tmp/core/src/downloader.cpp"
    printf 'int timers = 0; int sleeping = 1; bool asleep() { return false; }\n' > "$tmp/bridge/src/ok.cpp"
    printf 'QTimer* timer_ = nullptr; timer_->setSingleShot(true);\n' > "$tmp/bridge/src/queue_bridge.cpp"
    printf 'QTimer* timer_;\n' > "$tmp/bridge/include/queue_bridge.h"
    expect pass "N3: the downloader may wait, the queue bridge may hold its one QTimer; words that merely contain sleep or timer are fine"
    rm -f "$tmp/core/src/downloader.cpp" "$tmp/bridge/src/ok.cpp" "$tmp/bridge/src/queue_bridge.cpp" "$tmp/bridge/include/queue_bridge.h"
    # N4: server data and user input stay out of the log
    for w in href term user login token query search link; do
        violate core/src/a.cpp "log::text(\"$w\", v);" "N4: log key $w"
    done
    for w in etag checksum upload_id; do
        violate core/src/a.cpp "log::text(\"$w\", v);" "N4: log key $w"
    done
    violate bridge/a.cpp 'log::text("search_term", v);' "N4: log key search_term"
    violate app/a.cpp 'log::count("login_name_len", 1);' "N4: log key login_name_len"
    printf 'log::text("result", r); log::count("entries", 3); log::count("skipped_hostile", 1); log::text("key", k);\n' > "$tmp/core/src/log_ok.cpp"
    expect pass "N4: counts and result keys are allowed"; rm "$tmp/core/src/log_ok.cpp"
    # N5: file mutation only in the audited modules
    violate core/src/a.cpp '::unlink(p.c_str());' "N5: unlink"
    violate core/src/a.cpp 'std::remove(p.c_str());' "N5: std::remove"
    violate core/src/a.cpp 'std::rename(a.c_str(), b.c_str());' "N5: std::rename"
    violate core/src/a.cpp 'std::filesystem::remove(p);' "N5: filesystem::remove"
    for m in local_files downloader credentials_file ca_selector curl_transport store_files; do
        printf 'std::remove(p.c_str()); ::rename(a, b); ::unlink(c);\n' > "$tmp/core/src/$m.cpp"
    done
    expect pass "N5: the audited modules may remove and rename"
    for m in local_files downloader credentials_file ca_selector curl_transport store_files; do rm -f "$tmp/core/src/$m.cpp"; done
    printf 'list.remove(x); items.erase(it); remove_if(a, b, c); int removed = 0;\n' > "$tmp/core/src/ok2.cpp"
    expect pass "N5: member calls and other names are not matched"; rm "$tmp/core/src/ok2.cpp"
    # N6: the files an upload reads are only ever read (feature 002)
    mkdir -p "$tmp/bridge/src"
    for lit in 'open(p, O_WRONLY)' 'open(p, O_RDWR)' 'open(p, O_RDONLY | O_CREAT)' 'open(p, O_RDONLY | O_TRUNC)' 'open(p, O_RDONLY | O_APPEND)' \
               'fopen(p, "w")' 'fopen(p, "wb")' 'fopen(p, "a")' 'fopen(p, "r+")' 'std::ofstream f(p)' 'std::fstream f(p, std::ios::out)'; do
        violate core/src/queue_sender.cpp "$lit;" "N6: $lit in the sender"
        violate core/src/upload_source.cpp "$lit;" "N6: $lit in the source reader"
    done
    violate bridge/src/picker_bridge.cpp 'QFile f(p); f.open(QIODevice::WriteOnly);' "N6: QIODevice::WriteOnly in the picker"
    violate bridge/src/picker_bridge.cpp 'QFile::remove(p);' "N6: QFile::remove in the picker"
    printf 'int fd = open(p, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); FILE* f = fopen(p, "rb"); QFile q(p); q.open(QIODevice::ReadOnly);\n' > "$tmp/core/src/upload_source.cpp"
    printf 'int fd = open(p, O_RDONLY | O_NOFOLLOW);\n' > "$tmp/core/src/queue_sender.cpp"
    printf 'QFile q(p); q.open(QIODevice::ReadOnly);\n' > "$tmp/bridge/src/picker_bridge.cpp"
    expect pass "N6: read-only opens are fine"
    rm -f "$tmp/core/src/upload_source.cpp" "$tmp/core/src/queue_sender.cpp" "$tmp/bridge/src/picker_bridge.cpp"
    printf 'FILE* f = fopen(p, "w");\n' > "$tmp/core/src/downloader.cpp"
    expect pass "N6: it concerns only the upload source modules"; rm "$tmp/core/src/downloader.cpp"
    # N4/R8 in the sync modules (feature 003): no names, paths, server addresses, versions or checksums in the log
    for m in sync_runner sync_scan sync_planner; do
        for key in etag checksum href link; do
            violate core/src/$m.cpp "log::text(\"$key\", v);" "N4: log key $key in $m.cpp"
        done
        for key in rel local_rel name path server remote_folder; do
            violate core/src/$m.cpp "log::text(\"$key\", v);" "R8: log key $key in $m.cpp"
        done
        printf 'log::event("sync.compare.end", {log::count("pair", 1), log::count("plan", 2), log::count("seq", 3), log::text("reason", r), log::count("files", 4)});\n' > "$tmp/core/src/$m.cpp"
        expect pass "N4: pair, plan, seq, reason and counts are allowed in $m.cpp"
        rm -f "$tmp/core/src/$m.cpp"
    done
    # feature 003 (folder synchronization): N3 allow-list, N5 for the sync modules and the new calls, N2 and the new N7
    violate bridge/src/plan_helper.cpp 'QTimer t;' "N3: QTimer in a bridge file other than the sync and queue bridges"
    violate bridge/src/sync_bridge.cpp 'QBasicTimer t;' "N3: QBasicTimer in the sync bridge (only one QTimer is allowed there)"
    violate bridge/src/sync_bridge.cpp 'std::this_thread::sleep_for(std::chrono::seconds(1));' "N3: sleep_for in the sync bridge"
    violate bridge/src/sync_bridge.cpp 'timer_ = new QTimer(this); other_ = new QTimer(this);' "N3: a second timer in the sync bridge"
    violate bridge/src/sync_bridge.cpp 'QTimer a; QTimer b;' "N3: two QTimer values in the sync bridge"
    violate core/src/sync_runner.cpp 'std::this_thread::sleep_for(std::chrono::seconds(1));' "N3: sleep_for in the sync runner"
    violate core/src/sync_scan.cpp 'sleep(1);' "N3: sleep in the sync scan"
    violate core/src/sync_scheduler.cpp 'QTimer t;' "N3: QTimer in the sync scheduler"
    printf 'timer_ = new QTimer(this); timer_->setSingleShot(true); int sleeping = 1;\n' > "$tmp/bridge/src/sync_bridge.cpp"
    printf 'QTimer* timer_ = nullptr;\n' > "$tmp/bridge/include/sync_bridge.h"
    expect pass "N3: the sync bridge may hold its one single-shot QTimer"
    rm -f "$tmp/bridge/src/sync_bridge.cpp" "$tmp/bridge/include/sync_bridge.h"
    for m in sync_runner sync_scan sync_planner; do
        for call in '::unlink(p.c_str());' 'std::remove(p.c_str());' 'std::rename(a, b);' '::rmdir(d);' '::truncate(p, 0);'; do
            violate core/src/$m.cpp "$call" "N5: $call in $m.cpp"
        done
    done
    violate core/src/a.cpp '::rmdir(d.c_str());' "N5: rmdir outside the audited modules"
    violate core/src/a.cpp '::truncate(p, 0);' "N5: truncate outside the audited modules"
    violate core/src/a.cpp 'std::filesystem::resize_file(p, 0);' "N5: filesystem::resize_file outside the audited modules"
    for m in local_files downloader credentials_file ca_selector curl_transport store_files store_sync; do
        printf 'std::remove(p.c_str()); ::rename(a, b); ::unlink(c); ::rmdir(d); ::truncate(p, 0);\n' > "$tmp/core/src/$m.cpp"
    done
    expect pass "N5: the audited modules (also local_files.cpp for the sync) may remove, rename, rmdir and truncate"
    for m in local_files downloader credentials_file ca_selector curl_transport store_files store_sync; do rm -f "$tmp/core/src/$m.cpp"; done
    for m in sync_runner sync_scan sync_planner; do
        for m2 in PUT MKCOL MOVE COPY DELETE; do
            violate core/src/$m.cpp "req.method = \"$m2\";" "N2: write verb $m2 in $m.cpp"
        done
    done
    for m in sync_runner sync_scan sync_planner; do
        violate core/src/$m.cpp 'std::string p = root + "/" + name;' "N7: root + / + name in $m.cpp"
        violate core/src/$m.cpp 'auto q = folder + "/" + rel;' "N7: folder + / + rel in $m.cpp"
        violate core/src/$m.cpp 'auto q = dir + "/" + name;' "N7: dir + / + name in $m.cpp"
        violate core/src/$m.cpp 'auto q = "/" + name;' "N7: / + name in $m.cpp"
    done
    printf 'std::string join_path_checked(const std::string& root, const std::string& rel) {\n    if (rel.empty()) return root;\n    return root + "/" + rel;\n}\nauto p = join_path_checked(dir, name);\nauto q = remote_join(folder, name);\nstd::string s = "a/b";\n' > "$tmp/core/src/sync_scan.cpp"
    expect pass "N7: the join inside join_path_checked and calls to it are the allowed form"
    printf 'std::string join_path_checked(const std::string& root, const std::string& rel);\nauto p = join_path_checked(dir, name);\n' > "$tmp/core/src/sync_runner.cpp"
    printf 'std::string join_path_checked(const std::string& root,\n                              const std::string& rel) {\n    return root + "/" + rel;\n}\nauto x = root + "/" + name;\n' > "$tmp/core/src/sync_planner.cpp"
    expect fail "N7: a raw join after the end of join_path_checked is found"
    rm -f "$tmp/core/src/sync_scan.cpp" "$tmp/core/src/sync_runner.cpp" "$tmp/core/src/sync_planner.cpp"
    printf 'std::string path = root + "/" + name;\n' > "$tmp/core/src/store_sync.cpp"
    printf 'std::string path = root + "/" + name;\n' > "$tmp/core/src/local_files.cpp"
    expect pass "N7: it concerns only the planner, the scan and the runner of the sync"
    rm -f "$tmp/core/src/store_sync.cpp" "$tmp/core/src/local_files.cpp"
    # feature 004 (share links): N2 allow-list entry for share_api.cpp, N8 (the share modules cannot log secrets), N9 (nothing opens a link)
    mkdir -p "$tmp/core/include/pbc" "$tmp/bridge/src" "$tmp/bridge/include" "$tmp/qml" "$tmp/app"
    for m in share_parse share_diagnose share_rules share_date secret_text; do
        for verb in PUT DELETE; do
            violate core/src/$m.cpp "r.method = \"$verb\";" "N2: $verb in $m.cpp (only share_api.cpp may name it for shares)"
        done
    done
    violate bridge/src/share_bridge.cpp 'r.method = "DELETE";' "N2: DELETE in the share bridge"
    for verb in PROPPATCH LOCK UNLOCK MKCOL MOVE COPY; do
        violate core/src/share_api.cpp "r.method = \"$verb\";" "N2: $verb in share_api.cpp (it may name PUT and DELETE only)"
    done
    printf 'r.method = "POST"; r.method = "PUT"; r.method = "DELETE"; r.method = "GET";\n' > "$tmp/core/src/share_api.cpp"
    expect pass "N2: share_api.cpp may name POST, PUT, DELETE and GET"; rm "$tmp/core/src/share_api.cpp"
    for f in core/src/share_parse.cpp core/src/share_diagnose.cpp core/src/share_api.cpp core/src/share_rules.cpp core/src/share_date.cpp core/src/secret_text.cpp \
             core/include/pbc/share_types.h core/include/pbc/secret_text.h bridge/src/share_bridge.cpp bridge/include/share_bridge.h; do
        for lit in 'log::text("result", r);' 'log::path(p);' 'log::Field f{"a", "b"};' 'qDebug() << x;' 'qWarning() << x;' 'qInfo() << x;' 'std::cerr << x;' \
                   'printf("%d", n);' 'fprintf(stderr, "x");' 'puts("x");' 'log::event("upload.done", {log::count("n", 1)});'; do
            violate $f "$lit" "N8: $lit in $f"
        done
    done
    printf 'log::event("share.open", {log::status(200), log::count("rows", 3)});\nint prints = 0; int logs = 1; // no log::text here would be wrong\n' > "$tmp/core/src/share_parse.cpp"
    expect fail "N8: a comment that names log::text is still found (the rule is a plain scan; reword the comment)"
    printf 'log::event("share.open", {log::status(200), log::count("rows", 3)});\nint prints = 0; int logs = 1;\n' > "$tmp/core/src/share_parse.cpp"
    expect pass "N8: a share.* event with status and count fields is allowed"; rm "$tmp/core/src/share_parse.cpp"
    printf 'log::text("result", r); log::path(p); log::event("upload.done", {log::count("n", 1)});\n' > "$tmp/core/src/queue_sender.cpp"
    expect pass "N8: it concerns only the share modules"; rm "$tmp/core/src/queue_sender.cpp"
    violate core/src/share_parse.cpp 'auto s = pw.reveal();' "N8: reveal() outside share_api.cpp"
    violate bridge/src/share_bridge.cpp 'auto s = request.password.reveal();' "N8: reveal() in the share bridge"
    printf 'const std::string& reveal() const { return value_; }\n' > "$tmp/core/include/pbc/secret_text.h"
    printf 'auto body = form_encode({{"password", request.password.reveal()}});\n' > "$tmp/core/src/share_api.cpp"
    expect pass "N8: reveal() is declared in secret_text.h and used by share_api.cpp"; rm "$tmp/core/include/pbc/secret_text.h" "$tmp/core/src/share_api.cpp"
    for lit in 'QDesktopServices::openUrl(QUrl(link));' 'QProcess p;' 'system("xdg-open x");' 'popen("x", "r");' 'std::system(c);' 'QUrl u = QUrl::fromUserInput(s);' 'run("xdg-open", l);'; do
        violate core/src/a.cpp "$lit" "N9: $lit in core"
        violate bridge/src/a.cpp "$lit" "N9: $lit in the bridge"
        violate app/a.cpp "$lit" "N9: $lit in the app"
    done
    violate qml/A.qml 'onClicked: Qt.openUrlExternally(link)' "N9: Qt.openUrlExternally in QML"
    violate qml/A.qml 'onClicked: Qt.openUrl(link)' "N9: Qt.openUrl in QML"
    violate app/device_bridge.cpp 'QDesktopServices::openUrl(QUrl(x));' "N9: the viewer hand-over may open a local file only"
    printf '#include <QDesktopServices>\nreturn QDesktopServices::openUrl(QUrl::fromLocalFile(path));\nauto n = QLocale::system().name();\n' > "$tmp/app/device_bridge.cpp"
    expect pass "N9: app/device_bridge.cpp may hand a local file to the viewer; QLocale::system() is not a shell call"; rm "$tmp/app/device_bridge.cpp"
    printf 'std::filesystem::path p; int systems = 0; bool ecosystem() { return true; } // opens nothing\n' > "$tmp/core/src/ok9.cpp"
    expect pass "N9: words that merely contain system are fine"; rm "$tmp/core/src/ok9.cpp"
    mkdir -p "$tmp/tests/qml" "$tmp/tests/bridge" "$tmp/tests/walks" "$tmp/tests/unit"
    printf 'add_executable(test_ok bridge/test_ok.cpp)\n' > "$tmp/tests/CMakeLists.txt"; printf 'tests/walks/01-a.walk\n' > "$tmp/app/CMakeLists.txt"
    printf 'int main() {}\n' > "$tmp/tests/bridge/test_ok.cpp"; printf 'size 1 1\n' > "$tmp/tests/walks/01-a.walk"; printf 'int main() {}\n' > "$tmp/tests/unit/test_u.cpp"
    expect pass "T1: tests that CMake builds and runs pass"
    violate tests/qml/test_x.cpp 'int main() {}' "T1: a test file in a directory CMake does not build"
    violate tests/bridge/test_stale.cpp 'int main() {}' "T1: a bridge test missing from tests/CMakeLists.txt"
    violate tests/walks/02-b.walk 'size 1 1' "T1: a walk missing from app/CMakeLists.txt"
    rm -rf "$tmp/tests" "$tmp/app/CMakeLists.txt"
    violate docs/a.md 'see /home/someone/project/file' "R5: Linux home path"
    violate docs/a.md 'see /Users/someone/project/file' "R5: macOS home path"
    violate docs/a.md 'see C:\Users\x\file' "R5: drive-letter path"
    exit $fail
fi

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
if scan "$ROOT"; then
    echo "constitution checks passed"
else
    echo "constitution checks FAILED (see findings above)" >&2
    exit 1
fi
