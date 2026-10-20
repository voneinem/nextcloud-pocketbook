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
#        server, remote, local, etag, password, secret or account in core/, bridge/ and app/ (no mail or calendar content, no server names
#        and no names or paths of the user's files in logs; counts such as files, folders or entries are fine).
#   R9 The identifier pbnotes (the application this one was started from) appears in no tracked file except
#        under docs/reference/, in NOTICE and under licenses/ (the planning documents in specs/ and .specify/
#        describe the rename, tests/templates/ holds the old perf tests until they are rewritten).
#   R11 The identifier pboffice (PB Office, the application this one was bootstrapped from) appears in no tracked file
#        except under docs/reference/, in NOTICE and under licenses/ (specs/ and .specify/ describe the origin). The two
#        apps must never share a binary name, a launcher entry or a data directory.
#   N1 No plaintext http:// URL literal in core/, bridge/, app/ or qml/*.qml (comments excepted). XML namespace URIs of the DAV
#        vocabularies (owncloud.org, nextcloud.org, calendarserver.org, sabredav.org, www.w3.org) are protocol identifiers, not endpoints,
#        and are on an explicit exempt list.
#   N2 No HTTP write method (PUT, MKCOL, MOVE, COPY, DELETE, PROPPATCH, LOCK, UNLOCK) as a string literal in the code that builds
#        requests: core/src/nc_*.cpp, downloader.cpp, update.cpp, local_files.cpp, bridge/ and app/ (the client is read-only toward the
#        server). The generic transport files carry any method and are not scanned.
#   N3 No QTimer, QBasicTimer or sleep in core/ and bridge/ except core/src/downloader.cpp and its header (no background activity; the download wait).
#   N4 No log field named href, term, user, login, token, query, search or link in core/, bridge/ and app/ (server data and user
#        input stay out of the log).
#   N5 No unlink/remove/rename in core/src outside local_files.cpp, downloader.cpp, credentials_file.cpp, ca_selector.cpp,
#        curl_transport.cpp and store*.cpp (all file mutation goes through the audited modules).
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
            docs/reference/*|NOTICE|licenses/*|specs/*|.specify/*) ;;
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
                report R8 "$f" "$p" 'log::(text|count)[[:space:]]*\([[:space:]]*"([A-Za-z0-9.]+_)?(host|folder|subject|from|to|recipient|title|summary|attendee|location|contact|phone|email|book|org|note|path|name|file|filename|url|address|server|remote|local|etag|password|secret|account)(_[A-Za-z0-9_.]+)?"' && bad=1
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
        # N2: the client is read-only toward the server
        case "$f" in
            core/src/nc_*.cpp|core/src/downloader.cpp|core/src/update.cpp|core/src/local_files.cpp|bridge/*|app/*)
                report N2 "$f" "$p" '"(PUT|MKCOL|MOVE|COPY|DELETE|PROPPATCH|LOCK|UNLOCK)"' && bad=1
                ;;
        esac
        # N3: no background activity: no timers and no sleeping outside the download wait
        case "$f" in
            core/*|bridge/*)
                if [ "$f" != core/src/downloader.cpp ] && [ "$f" != core/include/pbc/downloader.h ]; then
                    report N3 "$f" "$p" '\bQ(Basic)?Timer\b|sleep_for|\busleep[[:space:]]*\(|\bnanosleep[[:space:]]*\(|(^|[^A-Za-z0-9_])sleep[[:space:]]*\(' && bad=1
                fi
                ;;
        esac
        # N4: server data and user input stay out of the log
        case "$f" in
            core/*|bridge/*|app/*)
                report N4 "$f" "$p" 'log::(text|count)[[:space:]]*\([[:space:]]*"([A-Za-z0-9.]+_)?(href|term|user|login|token|query|search|link)(_[A-Za-z0-9_.]+)?"' && bad=1
                ;;
        esac
        # N5: file mutation only in the audited modules
        case "$f" in
            core/src/local_files.cpp|core/src/downloader.cpp|core/src/credentials_file.cpp|core/src/ca_selector.cpp|core/src/curl_transport.cpp|core/src/store*.cpp) ;;
            core/src/*.cpp)
                report N5 "$f" "$p" '(^|[^A-Za-z0-9_.>])(::|std::)?(unlink|remove|rename)[[:space:]]*\(|filesystem::(remove|remove_all|rename)' && bad=1
                ;;
        esac
    done < <(list_files "$root")
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
    for w in path name file filename url address server remote local etag password secret account; do   # spec 001: no file names, paths or server data in logs
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
    # N2: no write method toward the server
    for m in PUT MKCOL MOVE COPY DELETE PROPPATCH LOCK UNLOCK; do
        violate core/src/nc_files.cpp "req.method = \"$m\";" "N2: method $m in a request builder"
    done
    violate core/src/downloader.cpp 'r.method = "DELETE";' "N2: DELETE in the downloader"
    violate bridge/a.cpp 'r.method = "MKCOL";' "N2: MKCOL in the bridge"
    violate app/a.cpp 'r.method = "PUT";' "N2: PUT in the app"
    printf 'if (request.method == "PUT") { }\n' > "$tmp/core/src/curl_transport.cpp"
    printf 'r.method = "PROPFIND"; r.method = "GET"; r.method = "REPORT"; r.method = "SEARCH"; r.method = "POST";\n' > "$tmp/core/src/nc_listing.cpp"
    expect pass "N2: the generic transport may name any method, read methods are allowed"
    rm -f "$tmp/core/src/curl_transport.cpp" "$tmp/core/src/nc_listing.cpp"
    # N3: no timers, no sleeping, except the download wait
    violate bridge/src/a.cpp 'QTimer t;' "N3: QTimer in the bridge"
    violate bridge/src/a.cpp 'QBasicTimer t;' "N3: QBasicTimer in the bridge"
    violate core/src/a.cpp 'std::this_thread::sleep_for(std::chrono::seconds(1));' "N3: sleep_for in the core"
    violate core/src/a.cpp 'usleep(1000);' "N3: usleep in the core"
    violate core/src/a.cpp 'sleep(1);' "N3: sleep in the core"
    printf 'std::this_thread::sleep_for(std::chrono::seconds(5));\n' > "$tmp/core/src/downloader.cpp"
    printf 'int timers = 0; int sleeping = 1; bool asleep() { return false; }\n' > "$tmp/bridge/src/ok.cpp"
    expect pass "N3: the downloader may wait; words that merely contain sleep or timer are fine"
    rm -f "$tmp/core/src/downloader.cpp" "$tmp/bridge/src/ok.cpp"
    # N4: server data and user input stay out of the log
    for w in href term user login token query search link; do
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
