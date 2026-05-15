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
rm -rf "${DIST_DIR}"
mkdir -p "${STAGING}" "${DIST_DIR}"

# ---- 1. Build C++ binaries ---------------------------------------------
echo ">>> [1/5] building C++ daemons + modules"
cmake -S cpp -B cpp/build-pkg -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DNL_ROUTER_BUILD_TESTS=OFF
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

# Module binaries — staged under their DB-kind name (no nl-mod- prefix).
install -m0755 cpp/build-pkg/modules/anonymize_basic/nl-mod-anonymize-basic \
    "${STAGING}/usr/libexec/nl-router/modules/anonymize_basic"

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

# ---- 5. Summary --------------------------------------------------------
echo ">>> [5/5] done"
ls -lh "${DIST_DIR}"/*.deb "${DIST_DIR}"/*.rpm 2>/dev/null || true
