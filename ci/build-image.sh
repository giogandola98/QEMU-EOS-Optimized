#!/bin/bash
# Build (or reuse) the qemu-eos image of this checkout on the CI host. Prints the image name on stdout.
#
# Usage: ci/build-image.sh [PROFILE]     PROFILE = QEMU_PROFILE of ci/qemu/Containerfile (default: opt)
#
# Two layers, both content-addressed so podman skips work that is already done:
#   qemu-eos-base:<profile>-<hash>   QEMU itself (~10 min); hash over the QEMU sources (git index of
#                                    everything .containerignore lets in) + Containerfile + profile
#   qemu-eos-ci:<profile>-<hash>     + plugins (Containerfile.tools, seconds); hash adds ci/qemu/plugins
# --network=host only for these builds: they need apt, and pasta networking fails on this runner.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PROFILE=${1:-opt}
cd "$ROOT"

src_hash() {
    { git ls-files -s -- . ':!ci' ':!bench' ':!.forgejo' ':!docs/PLAN.md' ':!docs/PERFORMANCE.md' \
          ':!hw/eos/cams/*/test/*'
      sha256sum ci/qemu/Containerfile .containerignore
      echo "$PROFILE"; } | sha256sum | cut -c1-12
}
base=qemu-eos-base:$PROFILE-$(src_hash)
tools_hash=$( { echo "$base"; git rev-parse HEAD; sha256sum ci/qemu/Containerfile.tools ci/qemu/build-plugin.sh ci/qemu/plugins/*; } \
              | sha256sum | cut -c1-12)
image=qemu-eos-ci:$PROFILE-$tools_hash

# The runner keeps every image it built and its disk fills up (a build then fails with "no space left on
# device" while podman commits a layer). Before building a new QEMU layer, drop this repository's older
# images, keeping the newest KEEP_IMAGES of each name; podman refuses to remove an image a container uses.
prune_old_images() {
    local name
    for name in qemu-eos-base qemu-eos-ci; do
        podman images --filter "reference=$name" --format '{{.CreatedAt}}	{{.Repository}}:{{.Tag}}' \
            | sort -r | tail -n +$(( ${KEEP_IMAGES:-3} + 1 )) | cut -f2 \
            | while read -r old; do podman rmi "$old" >/dev/null 2>&1 && echo "removed $old" >&2 || true; done
    done
}

if ! podman image exists "$base"; then
    prune_old_images
    start=$(date +%s)
    podman build --network=host --build-arg "QEMU_PROFILE=$PROFILE" -t "$base" -f ci/qemu/Containerfile . 2>&1 \
        | tail -n 80 >&2 || { echo "QEMU build ($PROFILE) failed" >&2; exit 1; }
    echo "built $base in $(( $(date +%s) - start ))s" >&2
else
    echo "reusing $base" >&2
fi
if ! podman image exists "$image"; then
    podman build --network=none --build-arg "BASE=$base" --build-arg "GIT_SHA=$(git rev-parse HEAD)" \
        -t "$image" -f ci/qemu/Containerfile.tools ci/qemu >&2
fi
echo "$image"
