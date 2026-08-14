#!/usr/bin/env bash
# =============================================================================
#  CubeGM / RK3036G -- cross-build libpng12 + alsa-lib + SDL1.2 into the sysroot
# -----------------------------------------------------------------------------
#  WHY THIS EXISTS
#    picoarch links -lSDL -lpng12 -lz (see deploy/build_sf3000_armhf.sh LDFLAGS).
#    The minimal crosstool-NG glibc-2.17 sysroot does NOT ship these. They MUST
#    be present in $SYSROOT/usr/{lib,include,bin} so the linker resolves them
#    and so sdl-config lives at $SYSROOT/usr/bin/sdl-config.
#
#    CRITICAL: these libs are linked into picoarch, which runs on the device's
#    glibc 2.17. Therefore they are cross-built AGAINST THE 2.17 SYSROOT, so the
#    libs themselves only require glibc <= 2.17. (Do NOT pull x86_64 or a newer
#    glibc's SDL into the sysroot -- that would break on-device.)
#
#  !!! THIS IS THE RISKIEST, LEAST VERIFIED STAGE !!!
#    It was authored WITHOUT a live Linux build (the agent's sandbox is Windows).
#    The cross-build commands follow standard autotools practice, but SDL's
#    video/audio backend selection is device-specific. If it fails, the most
#    likely fixes are:
#      - SDL configure: flip --enable-alsa / --disable-alsa, or --enable-video-fbcon
#        vs --enable-video-dummy, to match what the device actually exposes.
#      - Replace source cross-build with prebuilt armhf .debs unpacked into the
#        sysroot (must be glibc <= 2.17 era, e.g. Debian 7/8 or Ubuntu 12.04/14.04
#        armhf pool) -- see LINUX_BUILD.md "Prebuilt fallback".
#
#  USAGE:  SYSROOT=/path/to/glibc217-sysroot bash build_sdl_libpng.sh
#  Idempotent: re-running skips already-installed components.
# =============================================================================
set -euo pipefail

SYSROOT="${SYSROOT:?SYSROOT (glibc-2.17) must be set}"
TARGET="arm-linux-gnueabihf"
ARCH_FLAGS="-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -O2"

[ -d "$SYSROOT/usr" ] || { echo "ERROR: $SYSROOT/usr missing -- build the sysroot first."; exit 1; }
command -v ${TARGET}-gcc >/dev/null 2>&1 || { echo "ERROR: ${TARGET}-gcc not on PATH -- run build_sysroot_ctng.sh first."; exit 1; }

log(){ printf '\033[1;33m[sdl/libpng]\033[0m %s\n' "$*"; }
die(){ printf '\033[1;31m[ERROR]\033[0m %s\n' "$*" >&2; exit 1; }

# Cross toolchain env for all three builds
export CC="${TARGET}-gcc" CXX="${TARGET}-g++"
export AR="${TARGET}-ar" RANLIB="${TARGET}-ranlib" STRIP="${TARGET}-strip"
export CPPFLAGS="--sysroot=$SYSROOT"
export CFLAGS="$ARCH_FLAGS --sysroot=$SYSROOT"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="--sysroot=$SYSROOT -Wl,--sysroot=$SYSROOT"
export PKG_CONFIG_DIR=
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"

WORK="$(pwd)/.sdl_build"
mkdir -p "$WORK"
cd "$WORK"

NPROC="$(nproc 2>/dev/null || echo 4)"

# -----------------------------------------------------------------------------
# 1) libpng 1.2.x  (-> libpng12, matches picoarch's -lpng12)
# -----------------------------------------------------------------------------
if [ -f "$SYSROOT/usr/lib/libpng12.so" ] || [ -f "$SYSROOT/usr/lib/libpng12.a" ]; then
    log "libpng12 already in sysroot -- skip"
else
    log "Building libpng 1.2.59 ..."
    rm -rf libpng-1.2.59 && curl -fL -o lp.tar.xz \
        "https://downloads.sourceforge.net/project/libpng/libpng12/1.2.59/libpng-1.2.59.tar.xz" \
        || curl -fL -o lp.tar.xz "https://sourceforge.net/projects/libpng/files/libpng12/1.2.59/libpng-1.2.59.tar.xz/download"
    tar -xf lp.tar.xz && cd libpng-1.2.59
    ./configure --host=$TARGET --prefix=$SYSROOT/usr --enable-shared --enable-static
    make -j"$NPROC"
    make install
    cd "$WORK"
    [ -f "$SYSROOT/usr/lib/libpng12.so" ] || die "libpng12 install failed"
fi

# -----------------------------------------------------------------------------
# 2) alsa-lib  (SDL's ALSA backend needs it; device provides libasound.so.2 at
#    runtime, but the LINKER needs libasound present in the sysroot)
# -----------------------------------------------------------------------------
if [ -f "$SYSROOT/usr/lib/libasound.so" ]; then
    log "libasound already in sysroot -- skip"
else
    log "Building alsa-lib 1.2.10 ..."
    rm -rf alsa-lib-1.2.10 && curl -fL -o al.tar.bz2 \
        "https://github.com/alsa-project/alsa-lib/releases/download/v1.2.10/alsa-lib-1.2.10.tar.bz2"
    tar -xf al.tar.bz2 && cd alsa-lib-1.2.10
    ./configure --host=$TARGET --prefix=$SYSROOT/usr --disable-python --with-pcm-plugins=all
    make -j"$NPROC"
    make install
    cd "$WORK"
    [ -f "$SYSROOT/usr/lib/libasound.so" ] || log "WARN: libasound install failed -- SDL will build without ALSA."
fi

# -----------------------------------------------------------------------------
# 3) SDL 1.2.x  (-> libSDL, sdl-config)
#    Device exposes a Linux framebuffer (DRM/KMS). We enable fbcon video and
#    ALSA audio; disable X11/OpenGL (no X server on the handheld). If ALSA was
#    not built above, SDL configures without it (audio via SDL dummy) and the
#    device's driver.so still handles ALSA at runtime.
# -----------------------------------------------------------------------------
if [ -x "$SYSROOT/usr/bin/sdl-config" ] && [ -f "$SYSROOT/usr/lib/libSDL.so" ]; then
    log "SDL already in sysroot -- skip"
else
    log "Building SDL 1.2.15 ..."
    rm -rf SDL-1.2.15 && curl -fL -o sdl.tar.gz "https://www.libsdl.org/release/SDL-1.2.15.tar.gz"
    tar -xf sdl.tar.gz && cd SDL-1.2.15
    ./configure --host=$TARGET --prefix=$SYSROOT/usr \
        --with-sysroot=$SYSROOT \
        --disable-video-x11 --disable-video-opengl \
        --enable-video-fbcon --disable-video-directfb \
        --enable-alsa --disable-arts --disable-esd --disable-pulseaudio \
        --enable-timers --enable-events --enable-joystick
    make -j"$NPROC"
    make install
    cd "$WORK"
    if [ -x "$SYSROOT/usr/bin/sdl-config" ]; then
        log "sdl-config: $($SYSROOT/usr/bin/sdl-config --version)"
    else
        die "SDL install failed (no sdl-config). See LINUX_BUILD.md 'Prebuilt fallback'."
    fi
fi

log "=== SDL/libpng/alsa staged into $SYSROOT/usr ==="
ls -l "$SYSROOT/usr/lib/libSDL.so" "$SYSROOT/usr/lib/libpng12.so" 2>/dev/null
log "DONE. picoarch can now link -lSDL -lpng12 against this sysroot."
