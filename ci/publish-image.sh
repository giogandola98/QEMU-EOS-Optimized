#!/bin/bash
# Push the CI image to the private Forgejo container registry (org <private-org>).
# Usage: ci/publish-image.sh LOCAL_IMAGE TAG...     e.g. ci/publish-image.sh qemu-eos-ci:opt-abc 1a2b3c4 main
# Needs REGISTRY_TOKEN (repo secret: personal access token of a <private-org> member with write:package).
# Credentials go to a throw-away authfile (the runner is a shared root host: never ~/.config or /run).
# Prints "<registry>/<repo>@<digest>" of the pushed manifest as the last line.
set -euo pipefail
local=${1:?usage: publish-image.sh LOCAL_IMAGE TAG...}; shift
[ $# -gt 0 ] || { echo "no tag given" >&2; exit 1; }
REGISTRY=${REGISTRY:-<forgejo-server>}
REPO=$REGISTRY/${REGISTRY_OWNER:-<private-org>}/qemu-eos
[ -n "${REGISTRY_TOKEN:-}" ] || { echo "REGISTRY_TOKEN not set: image not published" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
chmod 700 "$tmp"
# Plain HTTP registry on the LAN, hence --tls-verify=false.
printf '%s' "$REGISTRY_TOKEN" | podman login --tls-verify=false --authfile "$tmp/auth.json" \
    -u "${REGISTRY_USER:-root}" --password-stdin "$REGISTRY" >/dev/null
unset REGISTRY_TOKEN
for tag in "$@"; do
    podman push -q --tls-verify=false --authfile "$tmp/auth.json" --digestfile "$tmp/digest" "$local" "$REPO:$tag"
    echo "pushed $REPO:$tag ($(cat "$tmp/digest"))"
done
echo "$REPO@$(cat "$tmp/digest")"
