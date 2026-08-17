#!/usr/bin/env bash
# =============================================================================
#  CubeGM / RK3036G (ARM) picoarch builder -- ARMHF, GENERIC LINUX SDL backend
# -----------------------------------------------------------------------------
#  RK3036G is a STANDARD buildroot Linux handheld (DRM/KMS + ALSA + evdev),
#  NOT the SF3000. The upstream `sf3000` platform wires SF3000-only hardware
#  paths (cubevol /tmp/joy_key shared-memory input, dlopen(driver.so) audio,
#  manual /dev/fb0 mmap) that DO NOT EXIST on RK3036G -- building with
#  `platform=sf3000`/`PLATFORM_SF3000` therefore produces a binary that cannot
#  read input or play audio on the device.
#
#  The CORRECT backend for RK3036G is the upstream GENERIC platform
#  (`platform=unix` => plat_linux.c => plat_sdl.c): SDL video via /dev/fb0
#  (fbdev), SDL audio via ALSA (SDL built with ALSA in build_sdl_libpng.sh),
#  and SDL joystick/keyboard input which reads the kernel evdev devices
#  (/dev/input/event*) -- so USB gamepads AND the panel buttons work with NO
#  per-device profile. This is exactly how the miyoomini/trimui ports build.
#
#  The repo's own build_sf3000.sh and Makefile 'sf3000' branch are HARDCODED
#  to MIPS (-mips32r2 -mtune=24kc). We use this ARM template instead.
#
#  PREREQUISITES (must exist BEFORE running this):
#    1. armhf cross gcc on PATH: arm-linux-gnueabihf-gcc  (or set CROSS_COMPILE)
#    2. a glibc-2.17 sysroot at $SYSROOT  (crosstool-NG, see build/toolchain/
#       build_sysroot_ctng.sh). THIS is the device ceiling -- do NOT link the
#       toolchain's own glibc 2.38 sysroot.
#    3. SDL1.2 (armhf, built WITH ALSA) + libpng12 (armhf) cross-built INTO
#       $SYSROOT, so $SYSROOT/usr/bin/sdl-config exists and -lSDL/-lpng12
#       resolve, and SDL's audio backend is ALSA.
#    4. libretro-common submodule initialised in the picoarch source
#       (provides libretro.h). Apply ../patch/picoarch_5edits.patch first for
#       the RTC GET_SYSTEM_TIME fix (helps time-based strategy/sim saves).
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
# Compose CFLAGS: ARM arch + 2.17 sysroot + SDL headers + NEON C scaler.
# We build the GENERIC 'unix' platform (NO -DPLATFORM_SF3000): standard Linux
# fbdev video / ALSA audio / evdev input. Panel is 1280x720 (RK3036G LCD).
CFLAGS="${ARCH_FLAGS} --sysroot=$SYSROOT -I./ -I./libretro-common/include/ -I$SYSROOT/usr/include/SDL -DUSE_C_SCALER -DSCREEN_WIDTH=1280 -DSCREEN_HEIGHT=720 -DSCREEN_BPP=2 -DCONTENT_DIR='\"/mnt/SDCARD/Roms\"' ${CFLAGS}"
CXXFLAGS="$CFLAGS"
LDFLAGS="${ARCH_FLAGS} --sysroot=$SYSROOT -L$SYSROOT/usr/lib -lc -ldl -lgcc -lm -lSDL -lpng12 -lz -lpthread -lasound -Wl,--gc-sections -s ${LDFLAGS}"

log(){ printf '\033[1;32m[armhf-build]\033[0m %s\n' "$*"; }
die(){ printf '\033[1;31m[ERROR]\033[0m %s\n' "$*" >&2; exit 1; }

# --- rewrite the Makefile 'sf3000' branch MIPS flags -> ARM (harmless; we use
#     the 'unix' target, but keep the sf3000 branch buildable for reference) ---
if grep -q 'mtune=24kc' Makefile; then
    log "Patching Makefile sf3000 branch: MIPS -> ARM (reference only)"
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
      unzip.o util.o plat_linux.o picoarch

log "Building picoarch (platform=unix, ARM, RK3036G generic Linux SDL)..."
make platform=unix \
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
