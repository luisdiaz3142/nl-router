# nl-router runtime image.
#
# One image, role-dispatched at startup via the first argument:
#
#   docker run nl-router:latest receiver       # runs nl-receiver
#   docker run nl-router:latest route           # runs nl-route
#   docker run nl-router:latest dispatcher      # runs nl-dispatch
#   docker run nl-router:latest cleaner         # runs nl-clean
#   docker run nl-router:latest api             # runs nl-router serve
#   docker run nl-router:latest module <kind>   # runs a processing module
#   docker run nl-router:latest migrate <args>  # runs nl-router migrate
#   docker run nl-router:latest cli <args>      # generic nl-router passthrough
#
# Single-image keeps the build pipeline trivial — the .deb staging tree
# is the source of truth and we COPY it in wholesale. Per-service images
# would shave 10–20 MB each but compound build complexity; that split
# is a v2 option if operators ask.
#
# This Dockerfile expects to be built from a context that contains:
#   staging/        # the packaged filesystem tree (same layout as the .deb)
#   entrypoint.sh   # role dispatcher
# build-all.sh assembles that context per arch and runs buildx against it.

FROM debian:12-slim

ENV DEBIAN_FRONTEND=noninteractive \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

# ---- Runtime deps ------------------------------------------------------
# These are the .so families the C++ daemons + Python wheel actually need.
# tini supervises pid 1 so signal forwarding works under `docker run`.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libdcmtk17 \
        libpq5 \
        libssl3 \
        libcurl4 \
        python3 \
        python3-venv \
        ca-certificates \
        tini \
 && rm -rf /var/lib/apt/lists/*

# ---- nl-router service account ----------------------------------------
# Matches the .deb's user (uid/gid 999 is the Debian system-user default).
RUN groupadd --system --gid 999 nl-router \
 && useradd  --system --uid 999 --gid 999 --no-create-home \
             --shell /usr/sbin/nologin \
             --comment "nl-router service account" \
             nl-router

# ---- Staged filesystem -------------------------------------------------
# Drop the .deb's staging tree into /. Three discrete COPYs rather than
# a single `staging/ /` because Debian 12 ships /lib as a symlink to
# /usr/lib (the usrmerge transition), and buildkit refuses to merge a
# directory tree onto a symlink target. The .deb dpkg-resolves the
# symlink at install time; we resolve it here by pointing systemd units
# at /usr/lib directly (systemd searches both).
COPY staging/usr/         /usr/
COPY staging/etc/         /etc/
COPY staging/lib/systemd/ /usr/lib/systemd/

# ---- Python venv -------------------------------------------------------
# Build the same venv the postinstall builds on bare-metal installs, so
# /usr/bin/nl-router (the shim) just works inside the container.
RUN python3 -m venv --upgrade-deps /usr/libexec/nl-router/python \
 && WHEEL=$(ls /usr/libexec/nl-router/wheels/nl_router-*.whl | head -n1) \
 && /usr/libexec/nl-router/python/bin/pip install --quiet "${WHEEL}[api]" \
 && rm -rf /usr/libexec/nl-router/wheels \
 && find /usr/libexec/nl-router/python -name '__pycache__' -prune -exec rm -rf {} +

# ---- Stateful dirs -----------------------------------------------------
# Volumes are the operator's responsibility (mount /var/lib/nl-router and
# /etc/nl-router from the host or a PVC); we pre-create them with the
# right ownership so non-root containers can write.
RUN mkdir -p /var/lib/nl-router/incoming /var/lib/nl-router/processing /var/log/nl-router \
 && chown -R nl-router:nl-router /var/lib/nl-router /var/log/nl-router \
 && chown -R root:nl-router /etc/nl-router \
 && chmod 0750 /etc/nl-router

# ---- Entrypoint --------------------------------------------------------
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod 0755 /usr/local/bin/entrypoint.sh

USER nl-router
WORKDIR /var/lib/nl-router

# Standard DICOM SCP port + management API port. Operators publish only
# the ones they need per-role.
EXPOSE 11112 2762 8080 9180 9181 9182 9183 9184

# tini reaps zombies and forwards signals (the C++ daemons need this).
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/entrypoint.sh"]
CMD ["help"]
