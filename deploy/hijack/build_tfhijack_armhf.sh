#!/bin/sh
# =============================================================================
#  build_tfhijack_armhf.sh -- build libemu_tfhijack.so for RK3036G (ARM armhf)
# =============================================================================
#  Produces the boot-override libretro core. Install on the SD card by copying
#  it to cubegm/cores/libemu_tfhijack.so and renaming to libemu_md.so (so the
#  stock rkgame dlopens it via the setting.xml autorun -> dummy.md mechanism).
#
#  tfhijack.c self-declares the libretro ABI (no libretro.h needed). It must be
#  a normal libc-linked shared lib (rkgame's process provides libc).
#
#  Env (all optional; build.sh exports them when invoked from the pipeline):
#    CC CXX SYSROOT CFLAGS CXXFLAGS LDFLAGS TFHIJACK_OUT
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")"

: "${CC:=arm-linux-gnueabihf-gcc}"
: "${CXX:=arm-linux-gnueabihf-g++}"
: "${SYSROOT:=}"
: "${CFLAGS:=-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -O2}"
: "${TFHIJACK_OUT:=./libemu_tfhijack.so}"

# Assemble compile flags.
FLAGS="$CFLAGS"
[ -n "$SYSROOT" ] && FLAGS="$FLAGS --sysroot=$SYSROOT"
FLAGS="$FLAGS -fPIC -Wall -O2 -DNDEBUG"

log(){ printf '\033[1;32m[tfhijack]\033[0m %s\n' "$*"; }

[ -x "$(command -v "$CC" 2>/dev/null || echo "$CC")" ] || {
    echo "toolchain not found: $CC (set CC= or ARM_GNU=)"; exit 1;
}

log "Compiling tfhijack.c -> $TFHIJACK_OUT"
$CC $FLAGS -shared -Wl,--gc-sections -o "$TFHIJACK_OUT" tfhijack.c

# Toolchain prefix for strip/readelf (triplet without the trailing "gcc").
TP="${CC%gcc}"
"${TP}strip" "$TFHIJACK_OUT" 2>/dev/null || true
log "built $(ls -la "$TFHIJACK_OUT" | awk '{print $5}') bytes"

log "=== exported retro_* symbols ==="
"${TP}readelf" --dyn-syms "$TFHIJACK_OUT" 2>/dev/null \
    | awk '$4=="FUNC"{print $8}' | grep '^retro_' | sort | tr '\n' ' '; echo
