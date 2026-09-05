#!/usr/bin/env bash
# =============================================================================
#  CubeGM / RK3036G (ARM) picoarch builder -- Direction-A rk3036g platform
# -----------------------------------------------------------------------------
#  Direction A (charter §0.4): build picoarch against the NEW clean
#  plat_rk3036g.c platform layer (DRM display + ALSA audio + native evdev
#  input), replacing the SF2000 transitional plat_sf3000.c path.
#
#  Unlike build_sf3000_armhf.sh (which sed-rewrites the Makefile sf3000
#  branch MIPS->ARM and passes -DPLATFORM_SF3000), this uses the Makefile's
#  own rk3036g branch (already ARMv7 cortex-a7 neon-vfpv4 hard-float in the
#  A1 patch) and -DPLATFORM_RK3036G. No MIPS sed rewrite needed.
#
#  PREREQUISITES (same as build_sf3000_armhf.sh):
#    1. armhf cross gcc on PATH: arm-linux-gnueabihf-gcc (CROSS_COMPILE)
#    2. glibc-2.29 sysroot at $SYSROOT (crosstool-NG)
#    3. SDL1.2 + libpng12 + zlib cross-built into $SYSROOT
#    4. libretro-common submodule initialised
#    5. patches applied: picoarch_rk3036g_full.patch + picoarch_rk3036g_platform.patch
#       (build.sh applies both before calling this)
#
#  ENV (inherited from deploy/build.sh): SYSROOT CC CXX CROSS_COMPILE CFLAGS CXXFLAGS LDFLAGS
# =============================================================================
set -e
cd "${PICOARCH_DIR:-$(dirname "$0")/picoarch}"
HERE="$PWD"

SYSROOT="${SYSROOT:?SYSROOT (glibc-2.29) must be set}"
CC="${CC:-${CROSS_COMPILE}gcc}"
CXX="${CXX:-${CROSS_COMPILE}g++}"

ARCH_FLAGS="-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -O2"
# Direction-A rk3036g platform: -DPLATFORM_RK3036G (NOT PLATFORM_SF3000).
# The Makefile rk3036g branch supplies its own -marm/armv7 flags; these
# CFLAGS are passed through and merged. SCREEN 1280x720 RGB565 (bpp=2).
CFLAGS="${ARCH_FLAGS} --sysroot=$SYSROOT -I./ -I./libretro-common/include/ -I$SYSROOT/usr/include/SDL -DPLATFORM_RK3036G -DUSE_C_SCALER -DSCREEN_WIDTH=1280 -DSCREEN_HEIGHT=720 -DSCREEN_BPP=2 -DCONTENT_DIR='\"/mnt/sdcard/Roms\"' ${CFLAGS}"
CXXFLAGS="$CFLAGS"
# -Wl,--export-dynamic exports main-binary symbols so the FrogUI launcher
# core can dlsym(NULL,"plat_sound_finish")/("hwdisp_restore") (same as sf3000).
LDFLAGS="${ARCH_FLAGS} --sysroot=$SYSROOT -L$SYSROOT/usr/lib -lc -ldl -lgcc -lm -lSDL -lpng12 -lz -lpthread -Wl,--gc-sections -Wl,--export-dynamic ${LDFLAGS}"

log(){ printf '\033[1;32m[rk3036g-build]\033[0m %s\n' "$*"; }
die(){ printf '\033[1;31m[ERROR]\033[0m %s\n' "$*" >&2; exit 1; }

# --- fix 32-bit ARM (armhf) signal-handler PC access in main.c --------------
# (same armhf fix as the sf3000 builder: mcontext has .arm_pc, not .pc)
if grep -q 'uc_mcontext.pc' main.c; then
    log "Patching main.c: armhf mcontext PC field (pc -> arm_pc)"
    sed -i 's/->uc_mcontext\.pc;/->uc_mcontext.arm_pc;/' main.c
fi

# sanity: SDL config reachable in sysroot
if [ ! -x "$SYSROOT/usr/bin/sdl-config" ]; then
    log "WARN: $SYSROOT/usr/bin/sdl-config missing -- SDL1.2 must be cross-built."
fi

log "Cleaning picoarch objects..."
rm -f libpicofe/input.o libpicofe/in_sdl.o libpicofe/linux/in_evdev.o \
      libpicofe/linux/plat.o libpicofe/fonts.o libpicofe/readpng.o \
      libpicofe/config_file.o cheat.o config.o content.o core.o menu.o menu_font.o \
      main.o options.o overrides.o patch.o scale.o scaler_neon.o \
      unzip.o util.o plat_rk3036g.o hwdisp.o picoarch

log "Building picoarch (platform=rk3036g, ARM)..."
make platform=rk3036g \
     CC="$CC" CXX="$CXX" CROSS_COMPILE="$CROSS_COMPILE" \
     SYSROOT="$SYSROOT" \
     CFLAGS="$CFLAGS" CXXFLAGS="$CXXFLAGS" LDFLAGS="$LDFLAGS" \
     picoarch -j"$(nproc)" 2>&1 | tail -40

if [ -f picoarch ]; then
    ${CROSS_COMPILE}strip picoarch
    echo "=== BUILD SUCCESS ==="
    ls -la picoarch
else
    die "picoarch build failed"
fi
