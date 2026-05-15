#!/bin/sh
# install.sh — generic installer for distros without .deb/.rpm support.
#
# Designed for Alpine, Arch, Amazon Linux 2, NixOS minimal, and anywhere
# else the package-format ecosystem isn't a fit. The artifact bundle this
# script lives in has the same filesystem layout the .deb produces, so
# we just copy it into /, then run the same pre/postinstall scriptlets
# the .deb runs.
#
# Usage:
#   tar -xzf nl-router-<version>-<arch>.tar.gz
#   cd nl-router-<version>-<arch>
#   sudo ./install.sh
#
# Or one-shot:
#   curl -L https://nl-router.example.com/install.sh | sudo sh
# (with the install.sh prebuilt to fetch the right tarball — that
# wrapper is part of the release-publishing pipeline, not this script.)

set -eu

# ---- Locate ourselves --------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${SCRIPT_DIR}"

# ---- Sanity checks ------------------------------------------------------
if [ "$(id -u)" != "0" ]; then
    echo "install.sh: must run as root (sudo ./install.sh)" 1>&2
    exit 1
fi

ARCH=$(uname -m)
case "${ARCH}" in
    x86_64)  TARBALL_ARCH=amd64 ;;
    aarch64) TARBALL_ARCH=arm64 ;;
    *)
        echo "install.sh: unsupported architecture: ${ARCH}" 1>&2
        echo "Supported: x86_64 (amd64), aarch64 (arm64)" 1>&2
        exit 1
        ;;
esac

# Cross-check: does the tarball's bundled receiver binary match this host?
# The .tar.gz is arch-tagged in its filename, so a mismatch usually means
# someone unpacked the wrong file.
if [ -x usr/bin/nl-receiver ]; then
    # Use the ELF header byte at offset 18 to identify e_machine:
    #   0x3e = x86_64, 0xb7 = aarch64. We check both via od for portability.
    bin_machine=$(od -An -tx1 -j18 -N2 usr/bin/nl-receiver 2>/dev/null | tr -d ' \n' || true)
    case "${bin_machine}" in
        3e00) BIN_ARCH=amd64 ;;
        b700) BIN_ARCH=arm64 ;;
        *)    BIN_ARCH=unknown ;;
    esac
    if [ "${BIN_ARCH}" != "unknown" ] && [ "${BIN_ARCH}" != "${TARBALL_ARCH}" ]; then
        echo "install.sh: this tarball contains ${BIN_ARCH} binaries, but the host is ${TARBALL_ARCH}." 1>&2
        echo "            Download the matching tarball." 1>&2
        exit 1
    fi
fi

# Python 3.11+ required for the venv. Postinstall double-checks.
if ! command -v python3 >/dev/null 2>&1; then
    echo "install.sh: python3 not found on PATH." 1>&2
    echo "            nl-router requires Python 3.11+ for its management CLI/API." 1>&2
    exit 1
fi

# systemd: we ship units but don't strictly require systemd to install.
# The postinstall handles its absence (skips daemon-reload).
HAS_SYSTEMD=0
if command -v systemctl >/dev/null 2>&1; then
    HAS_SYSTEMD=1
fi

# ---- Helpful preflight banner -------------------------------------------
echo "==> nl-router installer"
echo "    arch          : ${TARBALL_ARCH}"
echo "    python3       : $(python3 --version 2>&1)"
echo "    systemd       : $([ "${HAS_SYSTEMD}" = "1" ] && echo present || echo "absent (skipping unit reload)")"
echo ""

# ---- 1. preinstall (create user/group) ---------------------------------
echo "==> [1/3] running preinstall (creates nl-router user)"
sh scripts/preinstall.sh

# ---- 2. Copy files into / ----------------------------------------------
# We unpack each top-level dir (usr/, etc/, lib/) into /. cp -r preserves
# modes; nfpm's .deb sets specific permissions via file_info, but for
# the tarball path the staging dir's modes (set by build.sh's `install
# -m...`) are already correct.
echo "==> [2/3] copying files into /"
for top in usr etc lib; do
    if [ -d "${top}" ]; then
        # Preserve modes/ownership where set; merge into existing /usr,
        # /etc, /lib without clobbering unrelated content.
        cp -a "${top}/." "/${top}/"
    fi
done

# ---- 3. postinstall (venv + dirs + daemon-reload) ----------------------
echo "==> [3/3] running postinstall (builds Python venv)"
sh scripts/postinstall.sh

echo ""
echo "==> nl-router installed."
echo "    Verify with:  nl-router --version"
