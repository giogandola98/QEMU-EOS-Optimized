#!/bin/bash
# Run a command with one camera's ROMs fetched into a private tmpfs dir (CI host side).
# Usage: ci/with-roms.sh CAM-FW COMMAND...    COMMAND sees ROM_DIR (contains CAM/ROM*.BIN).
# ROMs live only in RAM (/dev/shm, mode 700), are removed whatever happens, and must only be mounted
# read-only into sandboxed containers. Exits 0 with "SKIPPED" when the ROMs are not available.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ver=${1:?usage: with-roms.sh CAM-FW COMMAND...}; shift
ROM_DIR=$(mktemp -d /dev/shm/qemu-eos-roms.XXXXXX)
chmod 700 "$ROM_DIR"
cleanup() { chmod -R u+w "$ROM_DIR" 2>/dev/null || true; rm -rf "$ROM_DIR"; }
trap cleanup EXIT INT TERM
st=0
"$HERE/fetch_roms.sh" "$ver" "$ROM_DIR" || st=$?
if [ "$st" -eq 3 ]; then echo "SKIPPED: no ROM for $ver"; exit 0; fi
[ "$st" -eq 0 ] || exit "$st"
unset ROM_TOKEN
export ROM_DIR
"$@"
