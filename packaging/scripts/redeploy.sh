#!/bin/sh
# Orchestrate a full redeploy to a remote host.
#
# Source preference:
#   1. CI artifact via `gh run download` — fastest, pre-verified by
#      check-deb-contents.sh in the same CI run. Requires the `gh`
#      CLI, an authenticated session, and a green workflow run.
#   2. Local build via `packaging/build-all.sh` — slow (~10 min cold)
#      but always works.
#
# scp the .deb + remote-install.sh to the host, ssh in, run the
# remote installer. The remote installer (packaging/scripts/
# remote-install.sh) handles the full apt + init + systemd cycle.
#
# Env overrides for SSH-via-proxy setups (gcloud IAP, bastion hosts):
#
#   NLR_SSH        command used for `ssh` (default: "ssh")
#   NLR_SCP        command used for `scp` (default: "scp")
#
# Example for gcloud IAP:
#
#   NLR_SSH='gcloud compute ssh dicom-diablo --tunnel-through-iap --' \
#   NLR_SCP='gcloud compute scp --tunnel-through-iap' \
#       make redeploy HOST=dicom-diablo
#
# (For gcloud-IAP, the HOST argument becomes redundant since it's
# baked into the SSH command; pass any non-empty value to satisfy
# the Makefile.)

set -eu

HOST="${1:?Usage: $0 <host>}"
SSH_CMD="${NLR_SSH:-ssh}"
SCP_CMD="${NLR_SCP:-scp}"

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
STAGING="${TMPDIR:-/tmp}/nlr-redeploy"

step() { printf '\n>>> %s\n' "$*"; }

cd "$REPO_ROOT"

# ---- 1. Pick a .deb source --------------------------------------------

mkdir -p "$STAGING"
# Wipe any previous run's artifacts so a stale .deb can't be picked up
# by the glob below.
rm -f "$STAGING"/nl-router_*.deb

if command -v gh >/dev/null 2>&1; then
    step "attempting to download latest CI .deb (gh run download)"
    if gh run download --name nl-router-deb-amd64 --dir "$STAGING" 2>/dev/null; then
        echo "    using CI-built .deb"
    else
        echo "    no CI artifact available (gh failed or no green run)"
    fi
else
    step "gh CLI not installed; skipping CI artifact fetch"
fi

# Fall back to local build if no .deb landed.
if ! ls "$STAGING"/nl-router_*.deb >/dev/null 2>&1; then
    step "building .deb locally (this is slow; ~10 min cold)"
    ARCHES=amd64 packaging/build-all.sh
    cp packaging/dist/nl-router_*_amd64.deb "$STAGING/"
fi

DEB=$(ls "$STAGING"/nl-router_*.deb | head -1)
DEB_BASE=$(basename "$DEB")

step "deploying $DEB_BASE → $HOST"

# ---- 2. Copy + run --------------------------------------------------

step "scp → $HOST:/tmp/"
$SCP_CMD "$DEB" "$HOST":/tmp/
$SCP_CMD packaging/scripts/remote-install.sh "$HOST":/tmp/

step "running remote-install.sh on $HOST"
# Path on the remote side: /tmp/<deb-name>. The remote script is also
# at /tmp/ because we just scp'd it there. chmod first so we don't
# require it to be executable on the remote FS.
$SSH_CMD "$HOST" "chmod +x /tmp/remote-install.sh && \
                   sudo /tmp/remote-install.sh /tmp/${DEB_BASE}"

step "redeploy complete"
echo "    host:    $HOST"
echo "    package: $DEB_BASE"
