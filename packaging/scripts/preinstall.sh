#!/bin/sh
# Preinstall scriptlet — runs before files are unpacked.
#
# Idempotent: handles both fresh install and upgrade. The sole job is to
# create the nl-router system user/group; everything else (venv,
# permissions, daemon-reload) happens in postinstall.
#
# Distro conventions:
#   * `useradd --system` does NOT fail if the user already exists when
#     `--no-create-home` is set on some distros; we still guard via getent.
#   * On RHEL/Fedora the user must be added with `--user-group` to also
#     create the group; on Debian `adduser --system --group --quiet`.

set -e

NL_USER=nl-router
NL_GROUP=nl-router
NL_HOME=/var/lib/nl-router

# Locate the nologin shell across distros:
#   Debian / Ubuntu / RHEL family : /usr/sbin/nologin
#   Alpine / BusyBox              : /sbin/nologin
# Fall back to /bin/false if neither exists (BusyBox without shadow).
if   [ -x /usr/sbin/nologin ]; then NL_SHELL=/usr/sbin/nologin
elif [ -x /sbin/nologin ];     then NL_SHELL=/sbin/nologin
else                                NL_SHELL=/bin/false
fi

if ! getent group "${NL_GROUP}" >/dev/null 2>&1; then
    if command -v groupadd >/dev/null 2>&1; then
        groupadd --system "${NL_GROUP}"
    else
        addgroup --system "${NL_GROUP}"
    fi
fi

if ! getent passwd "${NL_USER}" >/dev/null 2>&1; then
    if command -v useradd >/dev/null 2>&1; then
        useradd \
            --system \
            --gid "${NL_GROUP}" \
            --home-dir "${NL_HOME}" \
            --shell ${NL_SHELL} \
            --comment "nl-router service account" \
            "${NL_USER}"
    else
        # Debian fallback if useradd is missing (some minimal images).
        adduser \
            --system \
            --ingroup "${NL_GROUP}" \
            --home "${NL_HOME}" \
            --shell ${NL_SHELL} \
            --gecos "nl-router service account" \
            --quiet \
            --disabled-password \
            "${NL_USER}"
    fi
fi

exit 0
