#!/bin/sh
# Postremove scriptlet — runs after files are removed.
#
# On uninstall: daemon-reload + remove the venv (the package didn't ship
# it; we built it in postinstall, so we own teardown).
#
# We deliberately preserve:
#   * /etc/nl-router/ — operator's config + KEK
#   * /var/lib/nl-router/ — landing-zone files
#   * the nl-router system user/group
# These survive uninstall; on `apt purge` Debian removes /etc files itself
# via dpkg's standard conffile handling. We don't ship a purge hook here
# because we'd need to coordinate with both deb + rpm semantics; ops who
# really want a clean slate run:
#     sudo rm -rf /etc/nl-router /var/lib/nl-router /var/log/nl-router
#     sudo userdel nl-router && sudo groupdel nl-router

set -e

UPGRADING=0
case "$1" in
    upgrade|2)         UPGRADING=1 ;;
    remove|purge|0|"") UPGRADING=0 ;;
esac

if [ "${UPGRADING}" = "1" ]; then
    # daemon-reload only; postinstall will re-run after the upgrade.
    if command -v systemctl >/dev/null 2>&1; then
        systemctl daemon-reload || true
    fi
    exit 0
fi

# Real uninstall.
rm -rf /usr/libexec/nl-router/python || true

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
fi

exit 0
