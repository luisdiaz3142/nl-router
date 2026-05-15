#!/usr/bin/env bash
# packaging/build-all.sh — orchestrate package builds across arches.
#
# Iterates ARCHES (default: amd64 arm64) and for each one:
#   1. Builds packaging/Dockerfile.build for that platform via buildx.
#   2. Runs packaging/build.sh inside the container to produce .deb + .rpm.
#
# Output: packaging/dist/nl-router_<version>_<arch>.{deb,rpm} for every
# arch in ARCHES.
#
# Local builds on a single-arch host use QEMU emulation for the foreign
# arch (slow but works). CI matrix builds use native runners per arch
# for speed; the GitHub Actions workflow under .github/workflows/build.yml
# (added separately) calls this same script with ARCHES=<runner-arch>.
#
# Required tooling: docker with buildx (Docker Desktop / docker-ce 20+).
# For QEMU emulation: `docker run --privileged --rm tonistiigi/binfmt --install all`
# once per host.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

ARCHES="${ARCHES:-amd64 arm64}"
BUILD_IMAGES="${BUILD_IMAGES:-0}"           # 1 → also build runtime container images
PUSH_MANIFEST="${PUSH_MANIFEST:-}"          # registry/repo to push the manifest to; empty = local only
VERSION="${NL_ROUTER_VERSION:-$(awk -F'"' '/^version = /{print $2; exit}' "${REPO_ROOT}/pyproject.toml")}"

# Detect whether buildx is available; fall back to plain `docker build` on
# the native arch.
if ! docker buildx version >/dev/null 2>&1; then
    echo "FATAL: docker buildx not available. Install Docker Desktop or" 1>&2
    echo "       docker-ce 20.10+ with the buildx plugin." 1>&2
    exit 1
fi

# Ensure binfmt handlers are loaded for cross-arch emulation. Idempotent
# and cheap — installs nothing if already present.
docker run --privileged --rm tonistiigi/binfmt --install all >/dev/null 2>&1 || true

for arch in ${ARCHES}; do
    case "${arch}" in
        amd64) PLATFORM=linux/amd64 ;;
        arm64) PLATFORM=linux/arm64 ;;
        *) echo "unsupported arch: ${arch}"; exit 1 ;;
    esac

    echo ""
    echo "==> building for ${arch} (${PLATFORM})"
    echo ""

    # Build the build container for the target platform. --load brings the
    # image into the local docker daemon so the subsequent `docker run`
    # can pick it up.
    docker buildx build \
        --platform "${PLATFORM}" \
        --load \
        -t "nl-router-build:${arch}" \
        -f packaging/Dockerfile.build \
        packaging/

    # Run the build script inside the per-arch container. The container
    # detects arch via dpkg --print-architecture, so build.sh emits the
    # right ARCH automatically.
    docker run --rm \
        --platform "${PLATFORM}" \
        -v "${REPO_ROOT}":/src \
        -w /src \
        "nl-router-build:${arch}" \
        packaging/build.sh

    if [ "${BUILD_IMAGES}" = "1" ]; then
        echo ""
        echo "==> building runtime image for ${arch}"
        # The runtime Dockerfile needs the per-arch staging tree alongside
        # entrypoint.sh in its build context. We assemble a fresh temp
        # context per arch (staging is rewritten by the next iteration).
        ctx=$(mktemp -d "${TMPDIR:-/tmp}/nlr-runtime-${arch}-XXXXXX")
        cp -a "${REPO_ROOT}/packaging/dist/staging" "${ctx}/staging"
        cp "${REPO_ROOT}/packaging/runtime/Dockerfile"   "${ctx}/Dockerfile"
        cp "${REPO_ROOT}/packaging/runtime/entrypoint.sh" "${ctx}/entrypoint.sh"

        docker buildx build \
            --platform "${PLATFORM}" \
            --load \
            -t "nl-router:${VERSION}-${arch}" \
            -t "nl-router:latest-${arch}" \
            -f "${ctx}/Dockerfile" \
            "${ctx}"
        rm -rf "${ctx}"
    fi
done

if [ "${BUILD_IMAGES}" = "1" ] && [ -n "${PUSH_MANIFEST}" ]; then
    echo ""
    echo "==> assembling multi-arch manifest at ${PUSH_MANIFEST}"
    # imagetools create combines per-arch tags into a single multi-arch
    # manifest. The arches we built must each have been pushed to the
    # registry first (--push instead of --load above) — leave that
    # decision to the release-publishing workflow; this just shows the
    # invocation operators want.
    docker buildx imagetools create \
        -t "${PUSH_MANIFEST}:${VERSION}" \
        -t "${PUSH_MANIFEST}:latest" \
        $(for a in ${ARCHES}; do echo "${PUSH_MANIFEST}:${VERSION}-${a}"; done)
fi

echo ""
echo "==> all done"
ls -lh packaging/dist/*.deb packaging/dist/*.rpm 2>/dev/null
