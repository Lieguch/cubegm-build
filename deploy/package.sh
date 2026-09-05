#!/usr/bin/env bash
# Package the CubeGM open-source build outputs into a deployable SD-card tree.
#
# Usage:  deploy/package.sh [OUTPUT_TARBALL]
# Must be run AFTER deploy/build.sh has produced artifacts under deploy/buildroot/.
#
# Produces cubegm-deploy.tar.gz containing the on-device layout:
#   cubegm/
#     picoarch              (front-end binary)
#     zhijack.sh            (autorun entry — launches picoarch + FrogUI)
#     cores/                (frogui_libretro.so + libretro cores *.so)
#     Roms/                 (empty — drop user ROMs here)
#     setting.xml           (TEMPLATE with <autorun>; merge into device setting.xml)
#     DEPLOY.md             (deployment + verification guide)
#
# The tarball is extracted at the SD-card ROOT so the tree lands at /cubegm/.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILDROOT="${HERE}/buildroot"
OUT="${1:-${HERE}/cubegm-deploy.tar.gz}"

PICOARCH_BIN="${BUILDROOT}/picoarch/picoarch"
CORES_DIR="${BUILDROOT}/cores"

[ -x "$PICOARCH_BIN" ] || { echo "ERROR: picoarch not built ($PICOARCH_BIN). Run deploy/build.sh first." >&2; exit 1; }
[ -d "$CORES_DIR" ]    || { echo "ERROR: cores dir missing ($CORES_DIR)." >&2; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

DST="${TMP}/cubegm"
mkdir -p "${DST}/cores" "${DST}/Roms"

cp "$PICOARCH_BIN"                "${DST}/picoarch"
cp "${CORES_DIR}"/*.so            "${DST}/cores/"
cp "${HERE}/zhijack.sh"           "${DST}/zhijack.sh"
cp "${HERE}/DEPLOY.md"            "${DST}/DEPLOY.md"
cp "${HERE}/setting.xml.cubegm"   "${DST}/setting.xml" 2>/dev/null || true
: > "${DST}/Roms/.keep"

chmod +x "${DST}/picoarch" "${DST}/zhijack.sh"

tar -C "$TMP" -czf "$OUT" cubegm

echo "Packaged -> $OUT ($(du -h "$OUT" | cut -f1))"
echo "Deploy : extract to SD root, merge <autorun file=\"cubegm/zhijack.sh\"/> into setting.xml, reboot."
