#!/bin/sh
# Verify a freshly-built nl-router .deb contains every path operators
# depend on.
#
# Catches the M27-hotfix bug class: an artifact that builds correctly
# and lands in the staging directory, but is missing from nfpm.yaml's
# `contents:` list and therefore silently absent from the produced
# .deb. That mistake shipped from M22 through M27 without anyone
# noticing — the API and CLI fell through to "binary not installed"
# soft-pass paths, and there was no test that compared what was built
# against what was packaged.
#
# This script is the test. It's run from packaging/build.sh after
# nfpm finishes and from CI on every push. Run manually:
#
#     packaging/scripts/check-deb-contents.sh packaging/dist/nl-router_0.0.1_amd64.deb
#
# Exit 0 if every required path is present in the .deb; 1 otherwise.
# Stderr lists the missing paths so CI's failure output is actionable.

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

# Extract the list of installed paths from the .deb.
# dpkg-deb -c output looks like:
#   -rwxr-xr-x root/root  12345 2025-... ./usr/bin/nl-receiver
# Field 6 is the path; we strip the leading "./" to get absolute paths.
TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT
dpkg-deb -c "$DEB" | awk '{print $6}' | sed 's|^\./|/|' | sort > "$TMP"

# Required exact paths — anything missing from this list is a packaging
# bug worth blocking the release on.
REQUIRED="
/usr/bin/nl-receiver
/usr/bin/nl-route
/usr/bin/nl-dispatch
/usr/bin/nl-clean
/usr/bin/nl-router-migrate
/usr/bin/nl-router
/usr/libexec/nl-router/nl-dsl-validate
/usr/libexec/nl-router/modules/anonymize_basic
/usr/libexec/nl-router/modules/standardize_institution_group
/lib/systemd/system/nl-router-receiver.service
/lib/systemd/system/nl-router-route.service
/lib/systemd/system/nl-router-dispatcher.service
/lib/systemd/system/nl-router-cleaner.service
/lib/systemd/system/nl-router-api.service
/lib/systemd/system/nl-router-module@.service
/lib/systemd/system/nl-router-migrate.service
/etc/nl-router/config.toml.example
"

MISSING=""
for path in $REQUIRED; do
    if ! grep -qFx "$path" "$TMP"; then
        MISSING="${MISSING}${path}
"
    fi
done

if [ -n "$MISSING" ]; then
    echo "FAIL: .deb at $DEB is missing required paths:" 1>&2
    printf '%s' "$MISSING" | sed 's/^/  /' 1>&2
    exit 1
fi

# Wheel filename varies with version, so we glob-match instead of pinning.
# Exactly one wheel should be present.
WHEEL_COUNT="$(grep -c '/usr/libexec/nl-router/wheels/nl_router-.*\.whl$' "$TMP" || true)"
if [ "$WHEEL_COUNT" != "1" ]; then
    echo "FAIL: expected exactly 1 nl_router wheel in the .deb; found ${WHEEL_COUNT}" 1>&2
    grep '/usr/libexec/nl-router/wheels/' "$TMP" 1>&2 || true
    exit 1
fi

# At least one migration .sql file should be shipped. Specific count
# isn't pinned because the set grows over time as new migrations land.
MIGRATION_COUNT="$(grep -c '/usr/share/nl-router/migrations/.*\.sql$' "$TMP" || true)"
if [ "$MIGRATION_COUNT" -lt 1 ]; then
    echo "FAIL: no migration .sql files in /usr/share/nl-router/migrations/" 1>&2
    exit 1
fi

echo "OK: ${DEB}"
echo "  $(wc -l < "$TMP") total paths"
echo "  ${MIGRATION_COUNT} migration files"
echo "  1 wheel"
