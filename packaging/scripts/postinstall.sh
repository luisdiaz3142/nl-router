#!/bin/sh
# Postinstall scriptlet — runs after files are unpacked.
#
# Three jobs:
#   1. Create stateful directories (chowned to nl-router:nl-router).
#   2. Build the bundled Python venv from the host's python3 and install
#      the shipped wheel into it. We do this at install time (rather than
#      shipping a venv inside the package) because venv layouts depend on
#      the target Python version and are not relocatable in general.
#   3. systemd daemon-reload so the new units are visible. We do NOT
#      auto-enable services — operators run `nl-router init` first.

set -e

NL_USER=nl-router
NL_GROUP=nl-router
NL_VAR=/var/lib/nl-router
NL_LOG=/var/log/nl-router
NL_ETC=/etc/nl-router
NL_VENV=/usr/libexec/nl-router/python
NL_WHEEL_DIR=/usr/libexec/nl-router/wheels

# ---- 1. Stateful directories --------------------------------------------
for d in \
    "${NL_VAR}" \
    "${NL_VAR}/incoming" \
    "${NL_VAR}/processing" \
    "${NL_LOG}"; do
    mkdir -p "${d}"
    chown "${NL_USER}:${NL_GROUP}" "${d}"
    chmod 0750 "${d}"
done

# /etc/nl-router is readable by the service account but root-owned (so the
# operator owns the config). Permissions of individual files (kek.key in
# particular) are tighter — see `nl-router init`.
mkdir -p "${NL_ETC}"
chgrp "${NL_GROUP}" "${NL_ETC}"
chmod 0750 "${NL_ETC}"

# Drop the example config in place if /etc/nl-router/config.toml does
# not yet exist. Never overwrite an existing operator config.
if [ ! -e "${NL_ETC}/config.toml" ] && [ -f "${NL_ETC}/config.toml.example" ]; then
    cp "${NL_ETC}/config.toml.example" "${NL_ETC}/config.toml"
    chgrp "${NL_GROUP}" "${NL_ETC}/config.toml"
    chmod 0640 "${NL_ETC}/config.toml"
fi

# ---- 2. Bundled Python venv --------------------------------------------
# We require Python 3.11+. The package metadata declares the dependency;
# we double-check here for a clearer error message.
if ! command -v python3 >/dev/null 2>&1; then
    echo "nl-router postinstall: python3 not found on PATH" 1>&2
    exit 1
fi

PYTHON_VERSION=$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')
PY_OK=$(python3 -c 'import sys; print(1 if sys.version_info >= (3,11) else 0)')
if [ "${PY_OK}" != "1" ]; then
    echo "nl-router postinstall: python3 ${PYTHON_VERSION} is too old (need >= 3.11)" 1>&2
    exit 1
fi

# Recreate the venv from scratch on every install/upgrade. The wheel may
# have changed; the host python3 may have moved; rebuilding is cheap
# (< 30s on modern hardware) and avoids subtle stale-state bugs.
rm -rf "${NL_VENV}"
python3 -m venv --upgrade-deps "${NL_VENV}"

# Install the shipped wheel and its declared dependencies. --no-index +
# --find-links would pin us to bundled wheels only; in slice 1 we accept
# pulling deps from PyPI because the package is too large otherwise.
# Slice 2 of M11 (or a follow-up) will ship a wheelhouse for air-gapped
# installs.
# The build pipeline stages the wheel as a stable filename so nfpm
# manifests don't need globs. Either the stable name or a versioned
# wheel (in operator-rebuilt packages) is acceptable.
WHEEL="${NL_WHEEL_DIR}/nl_router.whl"
if [ ! -f "${WHEEL}" ]; then
    WHEEL=$(find "${NL_WHEEL_DIR}" -maxdepth 1 -name 'nl_router*.whl' | head -n1)
fi
if [ -z "${WHEEL}" ] || [ ! -f "${WHEEL}" ]; then
    echo "nl-router postinstall: no nl_router wheel found in ${NL_WHEEL_DIR}" 1>&2
    exit 1
fi
"${NL_VENV}/bin/pip" install --quiet "${WHEEL}[api]"

# The /usr/bin/nl-router shim execs this venv's python3.
chmod 0755 /usr/bin/nl-router

# ---- 3. systemd ---------------------------------------------------------
# daemon-reload is safe on both fresh install and upgrade. We do NOT
# enable units — operator workflow is:
#     sudo nl-router migrate
#     sudo nl-router init
#     sudo systemctl enable --now nl-router-receiver nl-router-route ...
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
fi

cat <<'EOF'

  nl-router installed.

  Next steps:
    sudo $EDITOR /etc/nl-router/config.toml        # set DSN, server_id
    sudo nl-router migrate                          # apply schema
    sudo nl-router init                             # mint admin token
    sudo systemctl enable --now nl-router-api \
                                nl-router-receiver \
                                nl-router-route \
                                nl-router-dispatcher \
                                nl-router-cleaner

  Documentation: /usr/share/doc/nl-router/

EOF

exit 0
