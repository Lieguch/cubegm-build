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
# the CI run into a zombie. Wrap every network op in timeout(300s) + 5 retries.
git_clone(){
    local repo="$1" dest="$2" branch="${3:-}"
    for n in 1 2 3 4 5; do
        log "git clone attempt $n/5: $repo -> $dest"
        rm -rf "$dest"
        if timeout 300 git clone --depth 1 ${branch:+-b "$branch"} "$repo" "$dest"; then
            return 0
        fi
        log "  clone attempt $n failed; retrying in 5s..."
        sleep 5
    done
    die "git clone failed after 5 attempts: $repo"
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

# Like git_clone but recurses into submodules. Required for picodrive, which
# vendors libretro-common as a git submodule; without --recursive the
# libretro-common headers (streams/trans_stream.h, compat/strcasestr.h) are
# missing and the core fails to build.
git_clone_recursive(){
    local repo="$1" dest="$2" branch="${3:-}"
    for n in 1 2 3 4 5; do
        log "git clone --recursive attempt $n/5: $repo -> $dest"
        rm -rf "$dest"
        if timeout 300 git clone --depth 1 --recursive ${branch:+-b "$branch"} "$repo" "$dest"; then
            return 0
        fi
        log "  clone attempt $n failed; retrying in 5s..."
        sleep 5
    done
    die "git clone --recursive failed after 5 attempts: $repo"
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
# Upstream FrogUI declares ptr_gs_run_game_* / direct_loader only under #ifdef
# SF2000 (a hardcoded ROM loader, meaningless on RK3036G). Our build is NOT
# SF2000, so we apply a self-contained bridge patch that backs the symbols with
# real buffers and a functional direct_loader() writing /tmp/frogui_launch.txt
# + requesting RETRO_ENVIRONMENT_SHUTDOWN (picoarch native launch protocol).
if [ -f "$HERE/../patch/frogui_gs_bridge.patch" ]; then
    if git -C FrogUI apply --check "$HERE/../patch/frogui_gs_bridge.patch" 2>/dev/null; then
        log "Applying CubeGM gs->picoarch launch bridge to FrogUI..."
        git -C FrogUI apply "$HERE/../patch/frogui_gs_bridge.patch"
    else
        log "WARN: gs bridge patch already applied or not applicable -- skipping."
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
# -----------------------------------------------------------------------------
# STAGE 7 -- build standard libretro cores
#
# NOTE on cross-compilation: several libretro core Makefiles hardcode `gcc` /
# `g++` when building their bundled libretro-common sub-tree (e.g. fceumm's
# src/drivers/libretro/libretro-common). A `CC=...` passed on the make command
# line does NOT reach those objects, so they get compiled for the host and the
# final .so is either unlinkable or broken on the device. The robust,
# buildroot/crosstool-style fix is to expose the cross compiler as `gcc`/`g++`
# on PATH for the duration of the core build loop.
# -----------------------------------------------------------------------------
CORE_OUT="$WORKDIR/cores"
mkdir -p "$CORE_OUT"

CROSS_BIN="$WORKDIR/cross-bin"
rm -rf "$CROSS_BIN"; mkdir -p "$CROSS_BIN"

# v3 -- a COMPILER WRAPPER (not a plain symlink). Every call to the cross
# compiler -- including each core's bundled libretro-common sub-tree, which
# hardcodes `gcc`/`$(CC)` and ignores make command-line CFLAGS -- must be
# compiled with -fPIC and see the ALSA headers + (for nestopia) the bundled
# libretro-common include dir. We expose the wrapper under BOTH the bare names
# (`gcc`/`g++`/`cc`) and the full triplet (`arm-linux-gnueabihf-gcc` etc.) so it
# catches every rule. The real compiler is invoked by absolute path (no PATH
# lookup) to avoid infinite recursion.
REAL_CC="$(command -v "$CC" 2>/dev/null || echo "$CC")"
REAL_CXX="$(command -v "$CXX" 2>/dev/null || echo "$CXX")"
TRIPLET="${CC##*/}"        # e.g. arm-linux-gnueabihf-gcc
TRIPLET_GXX="${CXX##*/}"   # e.g. arm-linux-gnueabihf-g++
make_wrapper () {
    local name="$1"; local real="$2"
    cat > "$CROSS_BIN/$name" <<WRAP
#!/bin/bash
exec "$real" -fPIC -marm -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
    -I"$ALSA_INC" -Ilibretro-common/include -DFCEU_VERSION_NUMERIC=9900 -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS "\$@"
WRAP
    chmod +x "$CROSS_BIN/$name"
}
make_wrapper "$TRIPLET"      "$REAL_CC"
make_wrapper "$TRIPLET_GXX"  "$REAL_CXX"
ln -sf "$CROSS_BIN/$TRIPLET"     "$CROSS_BIN/gcc"
ln -sf "$CROSS_BIN/$TRIPLET_GXX" "$CROSS_BIN/g++"
ln -sf "$CROSS_BIN/$TRIPLET"     "$CROSS_BIN/cc"
OLDPATH="$PATH"
export PATH="$CROSS_BIN:$PATH"
CORE_FAIL=""
# ===== FBA-family wrapper (official treefrog fba-gcc/g++ translated to armhf) =====
# FBA/FBNeo assume signed char and type-pun heavily -> need -fsigned-char
# -fno-strict-aliasing (else segfault at game load). Only FBA-family picks this up.
cat > "$CROSS_BIN/fba-gcc" <<FBAWRAP
#!/bin/bash
exec "$REAL_CC" -fPIC -marm -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
    -fsigned-char -fno-strict-aliasing \
    -I"$ALSA_INC" -Ilibretro-common/include "\$@"
FBAWRAP
cat > "$CROSS_BIN/fba-g++" <<FBAWRAP
#!/bin/bash
exec "$REAL_CXX" -fPIC -marm -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
    -fsigned-char -fno-strict-aliasing \
    -I"$ALSA_INC" -Ilibretro-common/include "\$@"
FBAWRAP
chmod +x "$CROSS_BIN/fba-gcc" "$CROSS_BIN/fba-g++"
AR="${CC%%gcc}ar"
RANLIB="${CC%%gcc}ranlib"

# ===== data-driven core build (mirrors treefrog-ui build_all.sh, ARM/RK3036G port) =====
# Specs: name|url|subdir|makefile|MAKEFLAGS|MARKERS
#   name     = output base (name_libretro.so)
#   url      = official git repo (clone_cores.sh)
#   subdir   = dir inside clone holding the Makefile ("" = root)
#   makefile = "" or Makefile.libretro
#   MAKEFLAGS= appended make vars (e.g. -lstdc++ -lpthread LTO= use_libchdr=0)
#   MARKERS  = comma list: fba (use fba wrapper), submod (init submodules),
#              cmake (mgba), gpsp (two-stage relink)
# Arcade aliases: fbalpha/pgm/fba all resolve to FBNeo (one build, copied).
CORE_SPECS='fbneo|https://github.com/libretro/FBNeo|src/burner/libretro||-lstdc++ -lpthread|fba
mame2000|https://github.com/libretro/mame2000-libretro||||-lstdc++
fbalpha2012|https://github.com/libretro/fbalpha2012_cps1||||-lstdc++|fba
snes9x|https://github.com/libretro/snes9x|libretro|||
snes9x2005|https://github.com/tzubertowski/snes9x2005|||LTO=
snes9x2010|https://github.com/libretro/snes9x2010||Makefile.libretro|LTO=
mgba|https://github.com/libretro/mgba||||cmake
vbam|https://github.com/libretro/vbam-libretro|libretro|Makefile.libretro||-lstdc++
gpsp|https://github.com/libretro/gpsp||||gpsp
picodrive|https://github.com/libretro/picodrive||Makefile.libretro|use_libchdr=0|submod
genesis_plus_gx|https://github.com/libretro/Genesis-Plus-GX||Makefile.libretro||-lstdc++
fceumm|https://github.com/tzubertowski/libretro-fceumm||Makefile.libretro||-lstdc++
nestopia|https://github.com/libretro/nestopia|libretro||||
stella|https://github.com/libretro/stella2014-libretro||||-lstdc++
prosystem|https://github.com/libretro/prosystem-libretro||||-lstdc++
gambatte|https://github.com/tzubertowski/libretro-gambatte||Makefile.libretro||-lstdc++
tgbdual|https://github.com/libretro/tgbdual-libretro||||-lstdc++
pcsx_rearmed|https://github.com/libretro/pcsx_rearmed||Makefile.libretro||-lstdc++|submod
mednafen_pce|https://github.com/libretro/beetle-pce-fast-libretro||||-lstdc++
prboom|https://github.com/libretro/libretro-prboom||||-lstdc++
scummvm|https://github.com/libretro/scummvm||||-lstdc++|submod'
CORE_ALIASES='fbalpha:fbneo pgm:fbneo fba:fbneo'

build_one_core () {
    local name="$1" url="$2" subdir="$3" mk="$4" mflags="$5" markers="$6"
    log "Building libretro core: $name"
    local d="$WORKDIR/libretro-$name"
    [ -d "$d" ] || git_clone "$url" "$d" || { log "ERROR: clone $name failed"; CORE_FAIL="${CORE_FAIL} $name"; return; }
    if echo "$markers" | grep -q submod; then
        ( cd "$d" && git submodule update --init --recursive ) || true
    fi
    local CCW="$CROSS_BIN/gcc" CXXW="$CROSS_BIN/g++"
    if echo "$markers" | grep -q fba; then CCW="$CROSS_BIN/fba-gcc"; CXXW="$CROSS_BIN/fba-g++"; fi
    local so builddir="$d"
    [ -n "$subdir" ] && builddir="$d/$subdir"
    case "$name" in
        mgba)
            rm -rf build-cubegm
            cmake -B build-cubegm -DCMAKE_BUILD_TYPE=Release -DLIBMGBA_ONLY=ON -DBUILD_LIBRETRO=ON \
                  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
                  -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
                  -DCMAKE_C_FLAGS="$CFLAGS" -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
                  -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS" -DCMAKE_SHARED_LINKER_FLAGS="$LDFLAGS" . >/dev/null 2>&1 \
                  || log "WARN: mgba configure had issues"
            cmake --build build-cubegm --target mgba_libretro -- -j"$(nproc)" 2>&1 | tail -3
            so=$(find build-cubegm -name "mgba_libretro.so" 2>/dev/null | head -1)
            ;;
        gpsp)
            make clean >/dev/null 2>&1 || true
            make platform=armv CC="$CCW" CXX="$CXXW" AR="$AR" RANLIB="$RANLIB" -j"$(nproc)" 2>&1 | tail -3 || true
            rm -f gpsp_libretro.so
            make platform=armv CC="$CROSS_BIN/g++" CXX="$CROSS_BIN/g++" AR="$AR" RANLIB="$RANLIB" \
                LDFLAGS="$LDFLAGS -lstdc++" gpsp_libretro.so 2>&1 | tail -3
            so=$(find . -name "gpsp_libretro.so" 2>/dev/null | head -1)
            ;;
        *)
            pushd "$builddir" >/dev/null
            if [ -n "$mk" ]; then
                make -f "$mk" clean >/dev/null 2>&1 || true
                make -f "$mk" platform=armv-neon-hardfloat $mflags -j"$(nproc)" 2>&1 | tail -3
            else
                make clean >/dev/null 2>&1 || true
                make platform=armv-neon-hardfloat $mflags -j"$(nproc)" 2>&1 | tail -3
            fi
            popd >/dev/null
            so=$(find "$d" -name "${name}_libretro.so" 2>/dev/null | head -1)
            ;;
    esac
    if [ -n "$so" ]; then
        cp "$so" "$CORE_OUT/${name}_libretro.so" && log "  -> $CORE_OUT/${name}_libretro.so"
    else
        log "ERROR: core $name did not produce a .so"
        CORE_FAIL="${CORE_FAIL} $name"
    fi
}

SPEC_FILE="$WORKDIR/core_specs.txt"
printf '%s\n' "$CORE_SPECS" > "$SPEC_FILE"
while IFS='|' read -r name url subdir mk mflags markers; do
    [ -z "$name" ] && continue
    build_one_core "$name" "$url" "$subdir" "$mk" "$mflags" "$markers"
done < "$SPEC_FILE"

# Resolve arcade aliases (copy FBNeo output to fbalpha/pgm/fba)
for alias_spec in $CORE_ALIASES; do
    aname=${alias_spec%%:*}; base=${alias_spec##*:}
    if [ -f "$CORE_OUT/${base}_libretro.so" ] && [ ! -f "$CORE_OUT/${aname}_libretro.so" ]; then
        cp "$CORE_OUT/${base}_libretro.so" "$CORE_OUT/${aname}_libretro.so" \
            && log "  alias -> $CORE_OUT/${aname}_libretro.so"
    fi
done

if [ -n "$CORE_FAIL" ]; then
    log "STAGE7 FAILED -- missing cores:${CORE_FAIL}"
    exit 1
fi
export PATH="$OLDPATH"

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
