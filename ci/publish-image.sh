#!/bin/bash
# Push the CI image to the container registry of this CI server, under the private org named by the CI
# variable ROM_PKG_OWNER (the same org that owns the ROM packages).
# Usage: ci/publish-image.sh LOCAL_IMAGE TAG...     e.g. ci/publish-image.sh qemu-eos-ci:opt-abc 1a2b3c4 main
# Needs REGISTRY_TOKEN (repo secret: personal access token of a member of that org with write:package).
# Credentials go to a throw-away authfile (the runner is a shared root host: never ~/.config or /run).
# Prints "<registry>/<repo>@<digest>" of the pushed manifest as the last line.
set -euo pipefail
local=${1:?usage: publish-image.sh LOCAL_IMAGE TAG...}; shift
[ $# -gt 0 ] || { echo "no tag given" >&2; exit 1; }
: "${GITHUB_SERVER_URL:?}" "${ROM_PKG_OWNER:?CI variable ROM_PKG_OWNER (private org) is required}"
REGISTRY=${GITHUB_SERVER_URL#*://}; REGISTRY=${REGISTRY%%/*}
REPO=$REGISTRY/$ROM_PKG_OWNER/qemu-eos
[ -n "${REGISTRY_TOKEN:-}" ] || { echo "REGISTRY_TOKEN not set: image not published" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
chmod 700 "$tmp"
# The registry may be plain HTTP on a private network, hence --tls-verify=false; digests pin the content.
printf '%s' "$REGISTRY_TOKEN" | podman login --tls-verify=false --authfile "$tmp/auth.json" \
    -u "${REGISTRY_USER:-root}" --password-stdin "$REGISTRY" >/dev/null
unset REGISTRY_TOKEN
for tag in "$@"; do
    podman push -q --tls-verify=false --authfile "$tmp/auth.json" --digestfile "$tmp/digest" "$local" "$REPO:$tag"
    echo "pushed $REPO:$tag ($(cat "$tmp/digest"))"
done
echo "$REPO@$(cat "$tmp/digest")"
