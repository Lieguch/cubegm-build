#!/usr/bin/env bash
# =============================================================================
#  CubeGM open-source replacement -- one-command builder (Linux x86_64 host)
# =============================================================================
#  Produces deploy/cubegm/ with compiled binaries that you copy straight to the
#  device SD card. The device boots the open-source menu (picoarch + FrogUI)
#  via the autorun hijack; stock rkgame/icube/driver.so and root.dat are left
#  untouched, so the device will NOT report "sdcard is damaged".
#
#  All produced binaries are linked against glibc <= 2.17 (the device ceiling,
#  measured on the 20 device cores) and verified by verify_target_abi.sh.
#
#  USAGE
#    ./build.sh                 # full build (auto-builds glibc-2.17 sysroot)
#    SYSROOT=/path/to/sysroot ./build.sh
#    ARM_GNU=/opt/arm-gnu-13.2 ./build.sh
#    CORES="mgba snes9x fceumm" ./build.sh   # build only these libretro cores
#    SKIP_SYSROOT=1 SYSROOT=/existing ./build.sh
#
#  REQUIREMENTS: x86_64 Linux, git, curl, xz, build-essential, autoconf,
#  automake, pkg-config, gperf, texinfo, flex, bison, python3, make, gcc.
# =============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WORKDIR="${WORKDIR:-$HERE/buildroot}"
SYSROOT="${SYSROOT:-}"                 # if empty -> built by crosstool-NG below
ARM_GNU="${ARM_GNU:-}"                 # optional: extracted ARM GNU 13.2 dir
SKIP_SYSROOT="${SKIP_SYSROOT:-0}"

# --- toolchain / target ------------------------------------------------------
TARGET="arm-linux-gnueabihf"
ARCH_FLAGS="-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -O2"

# --- repos ------------------------------------------------------------------
PICOARCH_REPO="https://github.com/tzubertowski/TreeFrogUI_picoarch.git"
FROGUI_REPO="https://github.com/tzubertowski/FrogUI.git"
ALSA_LIB_REPO="https://github.com/alsa-project/alsa-lib.git"

# Stage-1 default cores (proves the loop across GBA/SNES/NES/MD/Atari).
# Each is built from github.com/libretro/<NAME> with the libretro common Makefile.
DEFAULT_CORES="mgba snes9x fceumm picodrive nestopia"
CORES="${CORES:-$DEFAULT_CORES}"

log(){ printf '\033[1;32m[build]\033[0m %s\n' "$*"; }
err(){ printf '\033[1;31m[ERROR]\033[0m %s\n' "$*" >&2; }
die(){ err "$*"; exit 1; }

# --- resilient network ops (timeout + retry) ---
# A flaky runner network otherwise hangs git clone/submodule forever, turning
# the CI run into a zombie. Wrap every network op in timeout(600s) + 8 retries
# (partial clone --filter=blob:none defeats the huge-repo/timeout failure on fceumm).
git_clone(){
    local repo="$1" dest="$2" branch="${3:-}"
    for n in 1 2 3 4 5 6 7 8; do
        log "git clone attempt $n/8: $repo -> $dest"
        rm -rf "$dest"
        if timeout 600 git clone --depth 1 --filter=blob:none ${branch:+-b "$branch"} "$repo" "$dest"; then
            return 0
        fi
        log "  clone attempt $n failed; retrying in 5s..."
        sleep 5
    done
    die "git clone failed after 8 attempts: $repo"
}
git_submodule(){
    local dir="$1"
    for n in 1 2 3 4 5; do
        log "submodule update attempt $n/5 in $dir"
        if ( cd "$dir" && timeout 300 git submodule update --init --recursive ); then
            return 0
        fi
        log "  submodule attempt $n failed; retrying in 5s..."
        sleep 5
    done
    die "submodule init failed after 5 attempts in $dir"
}

[ "$(uname -s)" = "Linux" ] || die "This script must run on a Linux x86_64 build host."
command -v git >/dev/null || die "git not found."
command -v make >/dev/null || die "make not found."

mkdir -p "$WORKDIR"
cd "$WORKDIR"

# -----------------------------------------------------------------------------
# STAGE 1 -- locate / install the ARM cross compiler
# -----------------------------------------------------------------------------
if command -v ${TARGET}-gcc >/dev/null 2>&1; then
    CC="${TARGET}-gcc"
    log "Using system cross compiler: $($CC --version | head -1)"
elif [ -n "$ARM_GNU" ]; then
    CC="$ARM_GNU/bin/${TARGET}-gcc"
    [ -x "$CC" ] || die "ARM_GNU set but $CC missing. Extract arm-gnu-toolchain-13.2 first."
    log "Using ARM GNU toolchain: $CC"
else
    die "No ${TARGET}-gcc on PATH and ARM_GNU not set.
  Install one of:
    - apt:  sudo apt-get install gcc-arm-linux-gnueabihf
    - or download arm-gnu-toolchain-13.2-rel1-x86_64-arm-none-linux-gnueabihf.tar.xz
      and set ARM_GNU=/path/to/extracted"
fi
CXX="${CC%gcc}g++"
export CROSS_COMPILE="${TARGET}-"
export CC CXX

# -----------------------------------------------------------------------------
# STAGE 2 -- glibc-2.17 sysroot (the device ceiling)
# -----------------------------------------------------------------------------
if [ -z "$SYSROOT" ]; then
    if [ "$SKIP_SYSROOT" = "1" ]; then
        die "SKIP_SYSROOT=1 but no SYSROOT provided."
    fi
    SYSROOT="$WORKDIR/sysroot-glibc217"
    if [ -d "$SYSROOT/usr/lib" ]; then
        log "Reusing existing sysroot: $SYSROOT"
    else
        log "Building glibc-2.17 sysroot with crosstool-NG (one-time, ~20-60 min)..."
        bash "$HERE/toolchain/build_sysroot_ctng.sh" "$SYSROOT"
    fi
fi
[ -d "$SYSROOT" ] || die "SYSROOT not found: $SYSROOT"
log "Sysroot: $SYSROOT"

# -----------------------------------------------------------------------------
# STAGE 3 -- ALSA userspace headers (not in the ARM toolchain sysroot)
# -----------------------------------------------------------------------------
ALSA_INC="$WORKDIR/alsa-lib/include"
if [ ! -d "$ALSA_INC/alsa" ]; then
    log "Fetching alsa-lib headers (include/ only)..."
    rm -rf "$WORKDIR/alsa-lib"
    git_clone "$ALSA_LIB_REPO" "$WORKDIR/alsa-lib"
fi
ALSA_CFLAGS="-I$ALSA_INC"

# Common compile flags for every target binary
export CFLAGS="$ARCH_FLAGS --sysroot=$SYSROOT $ALSA_CFLAGS"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="--sysroot=$SYSROOT -Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3"

# -----------------------------------------------------------------------------
# STAGE 4 -- clone front-end sources
#   IMPORTANT: the 5-edit patch (../patch/picoarch_5edits.patch) only applies on
#   the *r36sx* branch and needs the libretro-common submodule (defines
#   RETRO_DEVICE_ID_JOYPAD_* used by plat_sf3000.c). Clone r36sx explicitly and
#   init the submodule. To reuse an already-patched local checkout, set
#   PICOARCH_LOCAL=/path/to/TreeFrogUI_picoarch.
# -----------------------------------------------------------------------------
if [ -n "${PICOARCH_LOCAL:-}" ] && [ -d "$PICOARCH_LOCAL" ]; then
    log "Using local picoarch source: $PICOARCH_LOCAL"
    rm -rf picoarch && cp -a "$PICOARCH_LOCAL" picoarch
elif [ ! -d picoarch ]; then
    log "Cloning picoarch (r36sx branch)..."
    git_clone "$PICOARCH_REPO" picoarch r36sx
else
    log "Reusing existing picoarch/ (ensure it is on r36sx AND patched)."
fi
# libretro-common submodule: provides libretro.h / RETRO_DEVICE_ID_*.
if [ ! -f picoarch/libretro-common/include/libretro.h ] \
   && [ ! -f picoarch/libretro-common/include/libretro/libretro.h ]; then
    log "Initializing libretro-common submodule (required for RETRO_DEVICE_ID_*)..."
    git_submodule picoarch
fi
# Apply the 5-edit patch (RTC + evdev gamepad) if a clean checkout and patch present.
if [ -f "$HERE/../patch/picoarch_5edits.patch" ]; then
    if git -C picoarch apply --check "$HERE/../patch/picoarch_5edits.patch" 2>/dev/null; then
        log "Applying 5-edit patch (RTC + evdev gamepad)..."
        git -C picoarch apply "$HERE/../patch/picoarch_5edits.patch"
    else
        log "5-edit patch already applied or not applicable -- skipping."
    fi
fi
[ -d FrogUI ] || git_clone "$FROGUI_REPO" FrogUI

# -----------------------------------------------------------------------------
# STAGE 5 -- build picoarch for RK3036G (ARM, NOT MIPS!)
#   Gotcha: the repo's build_sf3000.sh and the Makefile 'sf3000' branch are
#   HARDCODED to MIPS (-mips32r2). Our device is ARM (RK3036G), so use the
#   provided ARM template build_sf3000_armhf.sh. It rewrites the Makefile's
#   MIPS flags to ARM and expects SDL1.2 + libpng12 (armhf, linked against the
#   2.17 sysroot) to be present in $SYSROOT. The minimal crosstool-NG sysroot
#   does NOT include SDL/libpng -- cross-build them first (see HANDOFF.md).
#   Also note: the Makefile reads lowercase 'platform'; passing PLATFORM=...
#   is a no-op and silently builds the 'unix' target.
# -----------------------------------------------------------------------------
log "Building picoarch (plat_sf3000, ARM)..."
ARM_BUILD="$HERE/build_sf3000_armhf.sh"
if [ -x "$ARM_BUILD" ]; then
    SYSROOT="$SYSROOT" CC="$CC" CXX="$CXX" CROSS_COMPILE="$CROSS_COMPILE" \
    CFLAGS="$CFLAGS" CXXFLAGS="$CXXFLAGS" LDFLAGS="$LDFLAGS" \
    PICOARCH_DIR="$PWD/picoarch" \
        bash "$ARM_BUILD"
else
    log "WARN: build_sf3000_armhf.sh missing -- fallback needs manual Makefile fix."
    pushd picoarch >/dev/null
    make CC="$CC" CXX="$CXX" CROSS_COMPILE="$CROSS_COMPILE" \
         platform=sf3000 SYSROOT="$SYSROOT" \
         CFLAGS="$CFLAGS" CXXFLAGS="$CXXFLAGS" LDFLAGS="$LDFLAGS" \
         picoarch -j"$(nproc)"
    popd >/dev/null
fi
[ -x picoarch/picoarch ] || die "picoarch build failed."
log "picoarch built."

# -----------------------------------------------------------------------------
# STAGE 6 -- build FrogUI launcher core
# -----------------------------------------------------------------------------
# 方案2 (CubeGM): replace the closed-source stock-firmware game-launch interface
# (ptr_gs_run_game_* / direct_loader, declared only under SF2000) with the
# picoarch-native launch protocol. picoarch's quit() reads /tmp/frogui_launch.txt
# (core .so path + ROM path) and exec's into the chosen core+rom. The patch
# makes FrogUI write that file + request RETRO_ENVIRONMENT_SHUTDOWN instead of
# calling stock firmware. NOTE: this branch now reuses the proven
# deploy/patch/frogui_gs_bridge.patch (identical launch-bridge logic to
# wip/frogui-gs-interface), because the original frogui_native_launch.patch was
# never committed to this branch (deploy/patch/ did not exist here).
if [ -f "$HERE/../patch/frogui_gs_bridge.patch" ]; then
    if git -C FrogUI apply --check "$HERE/../patch/frogui_gs_bridge.patch" 2>/dev/null; then
        log "Applying CubeGM native-launch patch to FrogUI..."
        git -C FrogUI apply "$HERE/../patch/frogui_gs_bridge.patch"
    else
        log "WARN: gs-bridge patch already applied or not applicable -- skipping."
    fi
fi

log "Building FrogUI (frogui_libretro.so)..."
pushd FrogUI >/dev/null
make CC="$CC" CXX="$CXX" || true   # some FrogUI builds use a wrapper; fall back below
if [ ! -f frogui_libretro.so ]; then
    # Fallback: build as a libretro core directly if a Makefile.libretro exists
    [ -f Makefile.libretro ] && make -f Makefile.libretro CC="$CC" CXX="$CXX"
fi
popd >/dev/null
# Upstream FrogUI Makefile sets TARGET_NAME=menu so make produces
# menu_libretro.so. Our ABI gate + deploy step expect frogui_libretro.so.
# Normalize the produced artifact name so the rest of the pipeline is unchanged.
if [ -f FrogUI/menu_libretro.so ] && [ ! -f FrogUI/frogui_libretro.so ]; then
    cp FrogUI/menu_libretro.so FrogUI/frogui_libretro.so
    log "  normalized menu_libretro.so -> frogui_libretro.so"
fi
if [ -f FrogUI/frogui_libretro.so ]; then
    log "FrogUI built."
else
    log "WARN: frogui_libretro.so not produced -- check FrogUI build output on this repo."
fi

# -----------------------------------------------------------------------------
# STAGE 7 -- build standard libretro cores
# -----------------------------------------------------------------------------
CORE_OUT="$WORKDIR/cores"
mkdir -p "$CORE_OUT"
for c in $CORES; do
    log "Building libretro core: $c"
    d="$WORKDIR/libretro-$c"
    # repo name mapping: the NES core lives in libretro-fceumm, not fceumm
    repo="$c"
    [ "$c" = "fceumm" ] && repo="libretro-fceumm"
    [ -d "$d" ] || git_clone "https://github.com/libretro/$repo.git" "$d"
    pushd "$d" >/dev/null
    case "$c" in
        mgba)
            # mgba is a CMake project; its libretro core builds via cmake
            log "  mgba: cmake build (LIBMGBA_ONLY + BUILD_LIBRETRO)"
            rm -rf build-cubegm
            cmake -B build-cubegm -DCMAKE_BUILD_TYPE=Release \
                  -DLIBMGBA_ONLY=ON -DBUILD_LIBRETRO=ON \
                  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
                  -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
                  -DCMAKE_C_FLAGS="$CFLAGS" -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
                  -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS" \
                  -DCMAKE_SHARED_LINKER_FLAGS="$LDFLAGS" \
                  . >/dev/null || log "WARN: mgba cmake configure had issues."
            cmake --build build-cubegm --target mgba_libretro -- -j"$(nproc)" \
                || log "WARN: mgba build had issues."
            so=$(find build-cubegm -name "mgba_libretro.so" 2>/dev/null | head -1)
            ;;
        snes9x|nestopia)
            # these cores keep their libretro Makefile under libretro/
            make -C libretro clean >/dev/null 2>&1 || true
            make -C libretro CC="$CC" CXX="$CXX" CROSS_COMPILE="$CROSS_COMPILE" \
                 platform=armv7-neon-hardfloat \
                 CFLAGS="$CFLAGS" CXXFLAGS="$CXXFLAGS" LDFLAGS="$LDFLAGS" \
                 -j"$(nproc)" || log "WARN: core $c build had issues."
            so=$(find libretro -maxdepth 1 -name "${c}_libretro.so" 2>/dev/null | head -1)
            ;;
        fceumm|picodrive)
            # root Makefile.libretro is the libretro entry point
            make clean >/dev/null 2>&1 || true
            make -f Makefile.libretro CC="$CC" CXX="$CXX" CROSS_COMPILE="$CROSS_COMPILE" \
                 platform=armv7-neon-hardfloat \
                 CFLAGS="$CFLAGS" CXXFLAGS="$CXXFLAGS" LDFLAGS="$LDFLAGS" \
                 -j"$(nproc)" || log "WARN: core $c build had issues."
            so=$(find . -maxdepth 1 -name "${c}_libretro.so" 2>/dev/null | head -1)
            ;;
        *)
            make clean >/dev/null 2>&1 || true
            make CC="$CC" CXX="$CXX" CROSS_COMPILE="$CROSS_COMPILE" \
                 platform=armv7-neon-hardfloat \
                 CFLAGS="$CFLAGS" CXXFLAGS="$CXXFLAGS" LDFLAGS="$LDFLAGS" \
                 -j"$(nproc)" || log "WARN: core $c build had issues."
            so=$(find . -maxdepth 2 -name "${c}_libretro.so" 2>/dev/null | head -1)
            ;;
    esac
    [ -n "$so" ] && cp "$so" "$CORE_OUT/" && log "  -> $CORE_OUT/$(basename "$so")"
    popd >/dev/null
done

# -----------------------------------------------------------------------------
# STAGE 8 -- ABI gate
# -----------------------------------------------------------------------------
log "Running ABI verification gate (EM_ARM / 0x5000400 / glibc <= 2.17)..."
bash "$HERE/toolchain/verify_target_abi.sh" \
    picoarch/picoarch \
    FrogUI/frogui_libretro.so \
    $(ls "$CORE_OUT"/*.so 2>/dev/null) \
    || die "ABI gate FAILED -- do not deploy. Fix sysroot/toolchain and rebuild."

# -----------------------------------------------------------------------------
# STAGE 9 -- stage into deploy/cubegm/
# -----------------------------------------------------------------------------
DST="$HERE/cubegm"
mkdir -p "$DST" "$DST/cores" "$DST/lib"
cp -f picoarch/picoarch               "$DST/" 2>/dev/null || true
cp -f FrogUI/frogui_libretro.so        "$DST/" 2>/dev/null || true
cp -f "$CORE_OUT"/*.so                  "$DST/cores/" 2>/dev/null || true
cp -f "$HERE/cubegm/cores/config.xml"   "$DST/cores/" 2>/dev/null || true
cp -f "$HERE/cubegm/zhijack.sh"         "$DST/" 2>/dev/null || true
cp -f "$HERE/cubegm/autorun"            "$DST/" 2>/dev/null || true
chmod +x "$DST/picoarch" "$DST/zhijack.sh" "$DST/autorun" 2>/dev/null || true
log "Staged into $DST"
log "DONE. Copy the whole '$DST' directory to the root of your device SD card,"
log "overwriting the existing cubegm/ (stock rkgame/icube/driver.so/root.dat stay)."
log "On next boot the device launches picoarch + FrogUI directly."

