#!/bin/sh
# Preremove scriptlet — runs before files are removed.
#
# Stops any nl-router services so the binaries can be removed safely.
# Does NOT disable units — operator may want to keep their service
# configuration intact across upgrades. Also does NOT delete /var/lib
# data or /etc config.

set -e

# Distinguish "upgrade" from "uninstall":
#   * Debian:  arg "remove" or "purge" on uninstall; "upgrade" on upgrade.
#   * RPM:     arg "0" on uninstall; "1" on upgrade.
# In both cases we want to stop services on real uninstall only;
# upgrades restart them at the end of postinstall via daemon-reload.

UPGRADING=0
case "$1" in
    upgrade|2)         UPGRADING=1 ;;
    remove|purge|0|"") UPGRADING=0 ;;
esac

if [ "${UPGRADING}" = "1" ]; then
    exit 0
fi

if command -v systemctl >/dev/null 2>&1; then
    for unit in \
        nl-router-api.service \
        nl-router-receiver.service \
        nl-router-route.service \
        nl-router-dispatcher.service \
        nl-router-cleaner.service; do
        if systemctl is-active --quiet "${unit}"; then
            systemctl stop "${unit}" || true
        fi
    done
    # Templated module units: try to find anything enabled by listing.
    for unit in $(systemctl list-units --no-legend --plain 'nl-router-module@*.service' 2>/dev/null | awk '{print $1}'); do
        systemctl stop "${unit}" || true
    done
fi

exit 0
