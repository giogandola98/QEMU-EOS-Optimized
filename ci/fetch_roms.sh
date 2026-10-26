#!/bin/bash
# Fetch one camera's ROM dumps from the private Forgejo generic package into DEST/CAM/ and verify them.
# Runs on the CI host (outside containers); same scheme as ML-Optimized ci/qemu/fetch_roms.sh.
#
# Usage: ci/fetch_roms.sh CAM-FW DEST          e.g. ci/fetch_roms.sh 1100D-105 /dev/shm/roms.X
# Package: $GITHUB_SERVER_URL/api/packages/$ROM_PKG_OWNER/generic/canon-rom/<CAM>-<FW>/{ROM0.BIN,ROM1.BIN,SFDATA.BIN}
#          plus optional LiveView frames {LV.422,HD.422} (hw/eos/lv_inject.h)
#          (ROM_PKG_OWNER: CI variable naming a PRIVATE org; unset => exit 3, not available)
# Auth:    $ROM_TOKEN (repo secret: a personal access token with only read:package).
# Exit:    0 fetched and verified (sha256 from the server, MD5 from ci/rom-md5.txt), 3 not available, 1 error.
# The token reaches curl through a mode-600 header file, never a command line or the log.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
VERSION=${1:?usage: fetch_roms.sh CAM-FW DEST}
DEST=${2:?usage: fetch_roms.sh CAM-FW DEST}
FORGEJO_URL=${FORGEJO_URL:-${GITHUB_SERVER_URL:-}}
OWNER=${ROM_PKG_OWNER:-}
if [ -z "$FORGEJO_URL" ] || [ -z "$OWNER" ]; then
    echo "ROM package server/owner not configured (GITHUB_SERVER_URL, ROM_PKG_OWNER): ROMs not available" >&2
    exit 3
fi
PKG=canon-rom

case "$VERSION" in *[!A-Za-z0-9-]*|""|-*) echo "bad CAM-FW '$VERSION'" >&2; exit 1;; esac
CAM=${VERSION%%-*}

if [ -z "${ROM_TOKEN:-}" ]; then
    echo "ROMs for $VERSION: unavailable (no ROM_TOKEN secret)"
    exit 3
fi

umask 077
mkdir -p "$DEST/$CAM"
hdr=$(mktemp "$DEST/.hdr.XXXXXX")
list=$(mktemp "$DEST/.list.XXXXXX")
trap 'rm -f "$hdr" "$list"' EXIT
printf 'Authorization: token %s\n' "$ROM_TOKEN" > "$hdr"
unset ROM_TOKEN

code=$(curl -s -o "$list" -w '%{http_code}' -H "@$hdr" \
    "$FORGEJO_URL/api/v1/packages/$OWNER/generic/$PKG/$VERSION/files" || echo 000)
if [ "$code" != 200 ]; then
    echo "ROMs for $VERSION: unavailable (package $OWNER/$PKG/$VERSION: HTTP $code)"
    exit 3
fi

# name<TAB>sha256 for the files we know; anything else in the package is ignored.
mapfile -t files < <(python3 -I -c '
import json, sys
for f in json.load(open(sys.argv[1])):
    if f["name"] in ("ROM0.BIN", "ROM1.BIN", "SFDATA.BIN", "LV.422", "HD.422"):
        print(f["name"] + "\t" + f["sha256"])
' "$list")
[ "${#files[@]}" -gt 0 ] || { echo "ROMs for $VERSION: unavailable (package has no ROM files)"; exit 3; }

for entry in "${files[@]}"; do
    name=${entry%%$'\t'*}; sha=${entry#*$'\t'}
    curl -sf -H "@$hdr" -o "$DEST/$CAM/$name" "$FORGEJO_URL/api/packages/$OWNER/generic/$PKG/$VERSION/$name" \
        || { echo "download of $name failed" >&2; exit 1; }
    echo "$sha  $DEST/$CAM/$name" | sha256sum -c --quiet - \
        || { echo "sha256 mismatch for $name" >&2; rm -f "$DEST/$CAM/$name"; exit 1; }
    echo "ROMs for $VERSION: $name ok ($(stat -c %s "$DEST/$CAM/$name") bytes, sha256 verified)"
done

# The dumps differ per camera unit, so an MD5 mismatch means a wrong or corrupted file.
n=0
while read -r v f m _; do
    [ "$v" = "$VERSION" ] || continue
    [ -f "$DEST/$CAM/$f" ] || { echo "ROM check: $f listed for $VERSION but missing" >&2; exit 1; }
    [ "$(md5sum "$DEST/$CAM/$f" | cut -c1-32)" = "$m" ] \
        || { echo "ROM check: MD5 mismatch for $VERSION $f" >&2; rm -rf "${DEST:?}/$CAM"; exit 1; }
    n=$((n + 1))
done < <(grep -v '^#' "$HERE/rom-md5.txt")
echo "ROM check: $n file(s) of $VERSION match ci/rom-md5.txt"
chmod -R a-w "$DEST/$CAM"
