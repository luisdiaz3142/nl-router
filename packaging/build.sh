#!/usr/bin/env bash
# packaging/build.sh — build .deb + .rpm artifacts for nl-router.
#
# Designed to run inside the Dockerfile.build container, where every
# build dependency (DCMTK, libpq, OpenSSL, Python 3.11+, nfpm, the
# bundled golang-migrate binary, and hatchling) is already installed.
#
# Output: packaging/dist/nl-router_<version>_<arch>.{deb,rpm}
#
# The script is intentionally linear — top to bottom is the dependency
# order. No parallelism inside the script (CI matrix handles arch
# parallelism by running the script per-arch).

set -euo pipefail

# ---- Locate repo root ---------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

# ---- Version ------------------------------------------------------------
# Pull from pyproject.toml as the single source of truth. Sed picks the
# first `version = "X.Y.Z"` line, which is the project version (build
# system version uses `requires = [...]` syntax).
VERSION="${NL_ROUTER_VERSION:-$(awk -F'"' '/^version = /{print $2; exit}' pyproject.toml)}"
ARCH="${NL_ROUTER_ARCH:-$(dpkg --print-architecture 2>/dev/null || echo amd64)}"
echo ">>> building nl-router ${VERSION} for ${ARCH}"

# ---- Paths --------------------------------------------------------------
DIST_DIR="${REPO_ROOT}/packaging/dist"
STAGING="${DIST_DIR}/staging"
TARBALL_DIR="${DIST_DIR}/tarball-${ARCH}"
# Wipe only this arch's outputs + the staging dirs, so a multi-arch
# orchestrator (build-all.sh) can accumulate all targets in dist/ across
# back-to-back invocations.
rm -rf "${STAGING}" "${TARBALL_DIR}"
rm -f "${DIST_DIR}/nl-router_${VERSION}_${ARCH}".{deb,rpm}
rm -f "${DIST_DIR}/nl-router-${VERSION}-${ARCH}.tar.gz"
rm -f "${DIST_DIR}/nfpm.yaml"
mkdir -p "${STAGING}" "${DIST_DIR}"

# ---- 1. Build C++ binaries ---------------------------------------------
echo ">>> [1/5] building C++ daemons + modules"
# Wipe the build dir so arch-specific cached library paths from a prior
# build-all.sh run don't bleed into this arch (CMake caches absolute
# library paths that include the multiarch suffix, e.g.
# /usr/lib/x86_64-linux-gnu vs /usr/lib/aarch64-linux-gnu).
rm -rf cpp/build-pkg
# NL_ROUTER_STATIC_DCMTK=ON statically links DCMTK into every C++ binary
# so the resulting .deb / .rpm don't depend on the host's libdcmtk.so
# SOVERSION. Lets one set of artifacts target Debian 12+, Ubuntu 22.04+,
# RHEL 9+, etc. — see cpp/cmake/FindDCMTK.cmake for the implementation.
cmake -S cpp -B cpp/build-pkg -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DNL_ROUTER_BUILD_TESTS=OFF \
      -DNL_ROUTER_STATIC_DCMTK=ON
cmake --build cpp/build-pkg --parallel

# ---- 2. Build Python wheel ---------------------------------------------
echo ">>> [2/5] building Python wheel"
WHEEL_BUILD_DIR="${REPO_ROOT}/dist"
rm -rf "${WHEEL_BUILD_DIR}"
python3 -m build --wheel --outdir "${WHEEL_BUILD_DIR}" .
WHEEL_PATH="$(find "${WHEEL_BUILD_DIR}" -maxdepth 1 -name 'nl_router-*.whl' | head -n1)"
if [[ -z "${WHEEL_PATH}" ]]; then
    echo "FATAL: wheel build produced no output" 1>&2
    exit 1
fi
echo "    wheel: $(basename "${WHEEL_PATH}")"

# ---- 3. Stage filesystem -----------------------------------------------
echo ">>> [3/5] staging filesystem at ${STAGING}"
install -d \
    "${STAGING}/usr/bin" \
    "${STAGING}/usr/libexec/nl-router/modules" \
    "${STAGING}/usr/libexec/nl-router/wheels" \
    "${STAGING}/usr/share/nl-router/migrations" \
    "${STAGING}/lib/systemd/system" \
    "${STAGING}/etc/nl-router"

# C++ binaries
install -m0755 cpp/build-pkg/receiver/nl-receiver   "${STAGING}/usr/bin/"
install -m0755 cpp/build-pkg/router/nl-route        "${STAGING}/usr/bin/"
install -m0755 cpp/build-pkg/dispatcher/nl-dispatch "${STAGING}/usr/bin/"
install -m0755 cpp/build-pkg/cleaner/nl-clean       "${STAGING}/usr/bin/"

# DSL predicate-validation helper (M22). Tiny CLI wrapper around the
# router's parser; the management API shells out to it at rule
# create/update time so syntactically invalid predicates fail at
# HTTP-POST time instead of silently in the router's 15-second
# rule_cache_refresh loop.
install -m0755 cpp/build-pkg/common/dsl/nl-dsl-validate \
    "${STAGING}/usr/libexec/nl-router/nl-dsl-validate"

# DICOM C-ECHO probe helper (M31). Statically links DCMTK; the
# management API's destination test-connection feature shells out
# to it for `dicom`-kind destinations so the green check on the UI
# actually means "DIMSE handshake succeeded" instead of just "TCP
# connect succeeded."
install -m0755 cpp/build-pkg/common/dcm-probe/nl-dcm-probe \
    "${STAGING}/usr/libexec/nl-router/nl-dcm-probe"

# Module binaries — staged under their DB-kind name (no nl-mod- prefix).
# Each binary's filename here MUST match the processing_modules.kind value
# the operator references from rule_processing_chain; that's the path the
# systemd template (nl-router-module@.service) execs.
install -m0755 cpp/build-pkg/modules/anonymize_basic/nl-mod-anonymize-basic \
    "${STAGING}/usr/libexec/nl-router/modules/anonymize_basic"
install -m0755 cpp/build-pkg/modules/standardize_institution_group/nl-mod-standardize-institution-group \
    "${STAGING}/usr/libexec/nl-router/modules/standardize_institution_group"

# golang-migrate (bundled into the build container; falls back to system
# binary outside the container — convenient for local hand-builds).
if [[ -x /usr/local/bin/nl-router-migrate ]]; then
    install -m0755 /usr/local/bin/nl-router-migrate "${STAGING}/usr/bin/nl-router-migrate"
elif command -v migrate >/dev/null 2>&1; then
    install -m0755 "$(command -v migrate)" "${STAGING}/usr/bin/nl-router-migrate"
else
    echo "FATAL: no migrate binary found in build environment" 1>&2
    exit 1
fi

# CLI shim, config template, systemd units, migrations.
install -m0755 packaging/bin/nl-router \
    "${STAGING}/usr/bin/nl-router"
install -m0640 packaging/config/config.toml.example \
    "${STAGING}/etc/nl-router/config.toml.example"
install -m0644 packaging/systemd/nl-router-*.service \
    "${STAGING}/lib/systemd/system/"
cp -r migrations/*.sql "${STAGING}/usr/share/nl-router/migrations/"

# Wheel — keep the original PEP 491 filename (pip refuses to install a
# wheel whose name doesn't decompose into name-version-python-abi-platform).
# Export the basename so envsubst can plug it into nfpm.yaml.
WHEEL_NAME="$(basename "${WHEEL_PATH}")"
install -m0644 "${WHEEL_PATH}" "${STAGING}/usr/libexec/nl-router/wheels/${WHEEL_NAME}"
export WHEEL_NAME

# ---- 4. Build packages with nfpm ---------------------------------------
echo ">>> [4/5] running nfpm (deb + rpm)"
export NL_ROUTER_VERSION="${VERSION}"
export NL_ROUTER_ARCH="${ARCH}"
export STAGING

# nfpm only substitutes env vars in a handful of metadata fields; paths
# under `contents.src` are literal. Run envsubst over the template to
# expand ${STAGING} and friends throughout the file.
RENDERED_YAML="${DIST_DIR}/nfpm.yaml"
export REPO_ROOT
envsubst '${NL_ROUTER_VERSION} ${NL_ROUTER_ARCH} ${STAGING} ${REPO_ROOT} ${WHEEL_NAME}' \
    < packaging/nfpm/nfpm.yaml > "${RENDERED_YAML}"

for fmt in deb rpm; do
    out="${DIST_DIR}/nl-router_${VERSION}_${ARCH}.${fmt}"
    nfpm pkg --packager "${fmt}" --target "${out}" --config "${RENDERED_YAML}"
    echo "    produced: ${out}  ($(du -h "${out}" | cut -f1))"
done

# ---- 5. Tarball + install.sh (for distros without .deb/.rpm) -----------
echo ">>> [5/6] building tarball + install.sh"
TARBALL_ROOT="${TARBALL_DIR}/nl-router-${VERSION}-${ARCH}"
mkdir -p "${TARBALL_ROOT}/scripts"

# Copy the staged filesystem tree (usr/, etc/, lib/) into the tarball
# root. The tarball mirrors the .deb's layout 1:1, so install.sh just
# `cp -a` the contents into /.
cp -a "${STAGING}/." "${TARBALL_ROOT}/"

# Reuse the same pre/postinstall scriptlets the .deb runs, bundled
# inside the tarball so install.sh can call them.
cp "${REPO_ROOT}/packaging/scripts/preinstall.sh"  "${TARBALL_ROOT}/scripts/"
cp "${REPO_ROOT}/packaging/scripts/postinstall.sh" "${TARBALL_ROOT}/scripts/"
cp "${REPO_ROOT}/packaging/scripts/preremove.sh"   "${TARBALL_ROOT}/scripts/"
cp "${REPO_ROOT}/packaging/scripts/postremove.sh"  "${TARBALL_ROOT}/scripts/"
chmod 0755 "${TARBALL_ROOT}/scripts"/*.sh

cp "${REPO_ROOT}/packaging/install.sh" "${TARBALL_ROOT}/install.sh"
chmod 0755 "${TARBALL_ROOT}/install.sh"

cp "${REPO_ROOT}/packaging/README.md" "${TARBALL_ROOT}/README.md"

TARBALL_OUT="${DIST_DIR}/nl-router-${VERSION}-${ARCH}.tar.gz"
tar -czf "${TARBALL_OUT}" -C "${TARBALL_DIR}" "nl-router-${VERSION}-${ARCH}"
rm -rf "${TARBALL_DIR}"
echo "    produced: ${TARBALL_OUT}  ($(du -h "${TARBALL_OUT}" | cut -f1))"

# ---- 6. Summary --------------------------------------------------------
echo ">>> [6/6] done"
ls -lh "${DIST_DIR}"/*.deb "${DIST_DIR}"/*.rpm "${DIST_DIR}"/*.tar.gz 2>/dev/null || true
