#!/bin/bash
# Compile plugins/wakeups.c (next to this script) against the plugin header of the image.
# Used by Containerfile.tools (-> /opt/qemu-eos/plugins) and at job time for quick plugin iterations.
# Usage: build-plugin.sh OUT_DIR [SOURCE.c]   -> OUT_DIR/lib<name>.so (default source: plugins/wakeups.c)
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
QEMU_EOS=${QEMU_EOS:-/opt/qemu-eos}
out=${1:?usage: build-plugin.sh OUT_DIR [SOURCE.c]}
src=${2:-$HERE/plugins/wakeups.c}
name=$(basename "$src" .c)
mkdir -p "$out"
gcc -shared -fPIC -O2 -std=gnu11 -pthread -Wall -Wextra -Wno-unused-parameter -Werror \
    -I"$QEMU_EOS/include" -o "$out/lib$name.so" "$src"
echo "$out/lib$name.so"
