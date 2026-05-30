#!/bin/sh
# Runs on a remote host (typically scp'd here by
# packaging/scripts/redeploy.sh, then ssh'd to). Performs the full
# install + service-cycle sequence in one shot so the caller can fire
# a single `make redeploy` command and know every post-install step
# the operator would normally forget actually ran.
#
# Sequence:
#   1. apt install --reinstall <deb>
#   2. nl-router init           (regenerates env files from config.toml)
#   3. systemctl daemon-reload  (picks up any unit-file changes)
#   4. systemctl reset-failed   (clears auto-restart counters on units
#                                that have been crash-looping)
#   5. restart every *enabled* core daemon
#   6. restart every *enabled* module-worker instance
#   7. final is-active health check; non-zero exit if anything's dead
#
# Idempotent — running twice produces the same end state. Operators
# who don't want services restarted (e.g. mid-pilot) can run
# `apt install` by hand; this script is opinionated for the deploy
# loop.
#
# Caveat: services restart cold. Mid-flight API requests / DICOM
# associations fail. Acceptable for dev / single-node pilot; real
# production deploys want rolling restarts or a load-balancer drain.

set -eu

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 path/to/nl-router_*.deb" 1>&2
    exit 2
fi
DEB="$1"

if [ ! -f "$DEB" ]; then
    echo "FAIL: .deb not found at $DEB" 1>&2
    exit 2
fi

step() { printf '\n>>> %s\n' "$*"; }

# ---- 1. apt install ----------------------------------------------------

step "installing $(basename "$DEB")"
# --reinstall handles the case where the version on disk matches the
# version in the .deb (every redeploy of an in-progress branch); apt
# would otherwise skip with "already at newest version".
sudo apt install -y --reinstall "$DEB"

# ---- 2. nl-router init -------------------------------------------------

step "running nl-router init"
# init is idempotent: KEK preserved if present, env files rewritten
# from config.toml (intentional — keeps systemd env in sync with the
# operator's config edits), schema migrations skipped if already
# applied, admin token only minted on a clean DB.
sudo nl-router init

# ---- 3. daemon-reload --------------------------------------------------

step "reloading systemd"
sudo systemctl daemon-reload

# ---- 4. reset-failed ---------------------------------------------------

step "clearing failed-restart counters"
# `||  true` because some patterns may not match anything on a host
# that hasn't run a particular module yet — that's not a failure for
# our purposes.
sudo systemctl reset-failed 'nl-router-*' 2>/dev/null || true
sudo systemctl reset-failed 'nl-router-module@*' 2>/dev/null || true

# ---- 5. restart core daemons ------------------------------------------

step "restarting core services"
for svc in nl-router-api \
           nl-router-receiver \
           nl-router-route \
           nl-router-dispatcher \
           nl-router-cleaner; do
    if systemctl is-enabled --quiet "${svc}.service" 2>/dev/null; then
        echo "    restart ${svc}"
        sudo systemctl restart "${svc}.service"
    else
        echo "    skip    ${svc} (not enabled)"
    fi
done

# ---- 6. restart module-worker instances -------------------------------
# Module workers are templated units (nl-router-module@.service). To
# find which instances are enabled, walk the multi-user.target.wants
# symlinks — `systemctl list-unit-files` doesn't expose the bound
# instance names directly.

step "restarting module workers"
for symlink in /etc/systemd/system/multi-user.target.wants/nl-router-module@*.service; do
    if [ -L "$symlink" ]; then
        name=$(basename "$symlink")
        echo "    restart ${name}"
        sudo systemctl restart "$name"
    fi
done

# ---- 7. health check --------------------------------------------------

step "service status"
FAILED=0
for svc in nl-router-api \
           nl-router-receiver \
           nl-router-route \
           nl-router-dispatcher \
           nl-router-cleaner; do
    if ! systemctl is-enabled --quiet "${svc}.service" 2>/dev/null; then
        printf '    %-30s  %s\n' "${svc}" "disabled"
        continue
    fi
    state=$(systemctl is-active "${svc}.service" 2>&1 || true)
    printf '    %-30s  %s\n' "${svc}" "${state}"
    [ "$state" = "active" ] || FAILED=$((FAILED + 1))
done

# Module workers, if any.
for symlink in /etc/systemd/system/multi-user.target.wants/nl-router-module@*.service; do
    if [ -L "$symlink" ]; then
        name=$(basename "$symlink" .service)
        state=$(systemctl is-active "${name}.service" 2>&1 || true)
        printf '    %-30s  %s\n' "${name}" "${state}"
        [ "$state" = "active" ] || FAILED=$((FAILED + 1))
    fi
done

echo ""
if [ "$FAILED" -gt 0 ]; then
    echo "FAIL: ${FAILED} service(s) not active after redeploy. Check journalctl." 1>&2
    exit 1
fi

echo ">>> redeploy complete; all services active"
