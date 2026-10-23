#!/usr/bin/env bash
# Try the app on the host against a fake Nextcloud, with a throw-away data folder and download folder. No real account is involved.
#
#   scripts/run-host-demo.sh [build-dir]        build-dir defaults to build-host
#
# Starts tests/tools/pbc_fake_nextcloud (HTTPS on localhost, its own certificate authority), puts that authority into the demo's
# custom-ca.pem (the way a user trusts a private CA), starts the desktop app with PBC_DATA_DIR and PBC_DOWNLOAD_DIR in a temporary folder
# and prints the address, the user name and the app password to type into Settings. Type commands for the server in this terminal:
#   approve        the browser login that is waiting was approved
#   touch <path>   change a file on the server (then tap Update in the app)
#   remove <path>  remove a file or folder on the server (folder sync proposes a deletion, never does it)
#   drop           the next download loses its connection half way
#   offline|online switch the server off and on
#   lose-next-answer [METHOD [part]]  the next write is done on the server but its answer is lost (an upload with an unknown outcome)
#   drop-after <bytes>  the next upload body longer than that is cut; quota <bytes>; lock <path>; no-trash; ignore-conditions
#   share-rules <what> <value>   the server's sharing rules: sharing|public|password|upload on|off, or expiry <days>|off (specs/004-share-links)
#   share-list [path]   the shares on a path (ids and kinds, no addresses); unshare <id>   remove a share behind the app's back
#   quit           close the demo
# Share links (specs/004-share-links quickstart step 3): Files > Select > an entry > Share. /Books has a link, a user, a group, another kind of share and
# the link of another user. After a few actions search the data folder for the password you typed or a link: nothing is stored there.
# Folder sync (specs/003-folder-sync quickstart step 3): Sync > Add pair; the server has Books/ and Documents/, the reader's Downloads/ and Notes/ are
# folders to pair (Downloads is a good Outbox). tests/walks/11 ... 14 script the same steps with pictures.
# Everything is removed when the demo ends. The server is a fake of the tests: nothing here proves anything about a real Nextcloud.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$ROOT/build-host}"
APP="$BUILD/app/pbcloud"
SERVER="$BUILD/tests/pbc_fake_nextcloud"
[ -x "$APP" ] || { echo "missing $APP - build the host target first: cmake -S . -B build-host -G Ninja && cmake --build build-host" >&2; exit 1; }
[ -x "$SERVER" ] || { echo "missing $SERVER - build it: cmake --build $BUILD --target pbc_fake_nextcloud" >&2; exit 1; }

tmp="$(mktemp -d)"
server_pid=""
app_pid=""
cleanup() {
    exec 3>&- 2>/dev/null || true
    [ -n "$app_pid" ] && kill "$app_pid" 2>/dev/null || true
    [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null || true
    rm -rf "$tmp"
}
trap cleanup EXIT

mkdir -p "$tmp/data" "$tmp/downloads" "$tmp/server"
# the reader's storage for the Upload action (specs/002-manage-server-files): the folders the file browser lists, with some files in them
picker="$tmp/picker"
mkdir -p "$picker/Downloads" "$picker/Notes/trip/photos" "$picker/PBCloud" "$picker/system"
printf 'report' >"$picker/Downloads/report.pdf"; printf 'my notes' >"$picker/Downloads/notes.txt"
printf 'plan of the trip' >"$picker/Notes/trip/plan.md"; printf 'image' >"$picker/Notes/trip/photos/a.jpg"
truncate -s 300M "$picker/Downloads/big.bin"   # a file that is sent in chunks
mkfifo "$tmp/console"
"$SERVER" "$tmp/server" <"$tmp/console" >"$tmp/server.log" 2>&1 &
server_pid=$!
exec 3>"$tmp/console"   # keeps the server's stdin open

for _ in $(seq 1 100); do
    grep -q '^READY ' "$tmp/server.log" 2>/dev/null && break
    kill -0 "$server_pid" 2>/dev/null || { cat "$tmp/server.log" >&2; echo "the fake server did not start" >&2; exit 1; }
    sleep 0.1
done
read -r _ url user password ca < <(grep '^READY ' "$tmp/server.log")
cp "$ca" "$tmp/data/custom-ca.pem"

echo "Fake Nextcloud:  ${url#https://}"
echo "User name:       $user"
echo "App password:    $password"
echo "Downloads go to: $tmp/downloads"
echo "Data folder:     $tmp/data (the app writes no password, link or token here; search it after trying share links)"
echo "Uploads come from: $tmp/picker (Downloads, Notes, PBCloud)"
echo "Commands: approve | touch <path> | drop | offline | online | lose-next-answer [METHOD [part]] | drop-after <bytes> | quota <bytes> | lock <path> | no-trash | ignore-conditions | share-rules <what> <value> | share-list [path] | unshare <id> | quit"

PBC_DATA_DIR="$tmp/data" PBC_DOWNLOAD_DIR="$tmp/downloads" PBC_PICKER_ROOT="$picker" "$APP" &
app_pid=$!

# commands for the server until the app closes or "quit" is typed
shown=0
while kill -0 "$app_pid" 2>/dev/null; do
    if read -r -t 1 line; then
        [ "$line" = "quit" ] && break
        echo "$line" >&3
    fi
    total=$(wc -l <"$tmp/server.log")
    if [ "$total" -gt "$shown" ]; then tail -n +"$((shown + 1))" "$tmp/server.log" | grep -v '^READY ' || true; shown=$total; fi
done
