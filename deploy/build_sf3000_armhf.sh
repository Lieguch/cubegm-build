#!/usr/bin/env bash
# =============================================================================
#  CubeGM / RK3036G (ARM) picoarch builder -- ARMHF template
# -----------------------------------------------------------------------------
#  This is the CORRECT build entry for our device. The repo's own
#  build_sf3000.sh and Makefile 'sf3000' branch are HARDCODED to MIPS
#  (-mips32r2 -mtune=24kc). RK3036G is ARM Cortex-A7, so we MUST use ARM flags.
#
#  PREREQUISITES (must exist BEFORE running this):
#    1. armhf cross gcc on PATH: arm-linux-gnueabihf-gcc  (or set CROSS_COMPILE)
#    2. a glibc-2.17 sysroot at $SYSROOT  (crosstool-NG, see build/toolchain/
#       build_sysroot_ctng.sh). THIS is the device ceiling -- do NOT link the
#       toolchain's own glibc 2.38 sysroot.
#    3. SDL1.2 (armhf) + libpng12 (armhf) cross-built INTO $SYSROOT, so that
#       $SYSROOT/usr/bin/sdl-config exists and -lSDL / -lpng12 resolve.
#       (The minimal crosstool-NG sysroot does NOT ship SDL/libpng -- build
#        them yourself; see HANDOFF.md "Build dependencies".)
#    4. libretro-common submodule initialised in the picoarch source
#       (provides libretro.h -> RETRO_DEVICE_ID_JOYPAD_* used by plat_sf3000.c).
#    5. the 5-edit patch already applied (core.c RTC + plat_sf3000.c/plat_sdl.c
#       evdev), OR apply ../patch/picoarch_5edits.patch first.
#
#  ENV (inherited from deploy/build.sh, or set manually):
#    SYSROOT  CC  CXX  CROSS_COMPILE  CFLAGS  CXXFLAGS  LDFLAGS
# =============================================================================
set -e
cd "${PICOARCH_DIR:-$(dirname "$0")/picoarch}"
HERE="$PWD"

SYSROOT="${SYSROOT:?SYSROOT (glibc-2.17) must be set}"
CC="${CC:-${CROSS_COMPILE}gcc}"
CXX="${CXX:-${CROSS_COMPILE}g++}"

ARCH_FLAGS="-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -O2"
# Compose CFLAGS: ARM arch + 2.17 sysroot + ALSA headers (passed via $CFLAGS by
# build.sh). Append the flags the repo's sf3000 branch would have added (minus
# MIPS), and the PLATFORM_SF3000 define that our patch guards on.
CFLAGS="${ARCH_FLAGS} --sysroot=$SYSROOT -I./ -I./libretro-common/include/ -I$SYSROOT/usr/include/SDL -DUSE_C_SCALER -DPLATFORM_SF3000 -DCONTENT_DIR='\"/mnt/SDCARD/Roms\"' ${CFLAGS}"
CXXFLAGS="$CFLAGS"
LDFLAGS="${ARCH_FLAGS} --sysroot=$SYSROOT -L$SYSROOT/usr/lib -lc -ldl -lgcc -lm -lSDL -lpng12 -lz -lpthread -Wl,--gc-sections -s ${LDFLAGS}"

log(){ printf '\033[1;32m[armhf-build]\033[0m %s\n' "$*"; }
die(){ printf '\033[1;31m[ERROR]\033[0m %s\n' "$*" >&2; exit 1; }

# --- rewrite the Makefile 'sf3000' branch MIPS flags -> ARM ------------------
# The Makefile hardcodes -mips32r2 -march=mips32r2 -mtune=24kc -mfp32
# -mhard-float in BOTH the CFLAGS and LDFLAGS lines of the sf3000 branch.
# Replace that exact substring with our ARM flags (keep -DPLATFORM_SF3000 etc.)
if grep -q 'mtune=24kc' Makefile; then
    log "Patching Makefile sf3000 branch: MIPS -> ARM"
    cp Makefile Makefile.mips.bak
    sed -i 's/-mips32r2 -march=mips32r2 -mtune=24kc -mfp32 -mhard-float/-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard/g' Makefile
fi

# --- fix 32-bit ARM (armhf) signal-handler PC access in main.c --------------
# glibc on armhf defines mcontext_t as struct sigcontext, which has NO .pc
# member (the program counter is .arm_pc). The bundled main.c reads
# ->uc_mcontext.pc, which only exists on aarch64/x86_64, so it fails to
# compile on our RK3036G (32-bit ARM): "error: 'mcontext_t' has no member
# named 'pc'". Rewrite to the armhf field. Target is strictly 32-bit ARM,
# so a plain rewrite is safe and idempotent.
if grep -q 'uc_mcontext.pc' main.c; then
    log "Patching main.c: armhf mcontext PC field (pc -> arm_pc)"
    sed -i 's/->uc_mcontext\.pc;/->uc_mcontext.arm_pc;/' main.c
fi

# sanity: ensure SDL config is reachable in the sysroot
if [ ! -x "$SYSROOT/usr/bin/sdl-config" ]; then
    log "WARN: $SYSROOT/usr/bin/sdl-config missing -- SDL1.2 must be"
    log "      cross-built into the sysroot, or the build will fail on sdl-config."
fi

log "Cleaning picoarch objects..."
rm -f libpicofe/input.o libpicofe/in_sdl.o libpicofe/linux/in_evdev.o \
      libpicofe/linux/plat.o libpicofe/fonts.o libpicofe/readpng.o \
      libpicofe/config_file.o cheat.o config.o content.o core.o menu.o menu_font.o \
      main.o options.o overrides.o patch.o scale.o scaler_neon.o \
      unzip.o util.o plat_sf3000.o hwdisp.o picoarch

log "Building picoarch (platform=sf3000, ARM)..."
make platform=sf3000 \
     CC="$CC" CXX="$CXX" CROSS_COMPILE="$CROSS_COMPILE" \
     SYSROOT="$SYSROOT" \
     CFLAGS="$CFLAGS" CXXFLAGS="$CXXFLAGS" LDFLAGS="$LDFLAGS" \
     picoarch -j"$(nproc)" 2>&1 | tail -40

if [ -f picoarch ]; then
    ${CROSS_COMPILE}strip picoarch
    echo "=== BUILD SUCCESS ==="
    ls -la picoarch
    file picoarch
else
    die "picoarch build failed"
fi
