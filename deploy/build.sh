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
DEFAULT_CORES="fceumm nestopia snes9x2005_plus picodrive stella2014"
CORES="${CORES:-$DEFAULT_CORES}"

log(){ printf '\033[1;32m[build]\033[0m %s\n' "$*"; }
err(){ printf '\033[1;31m[ERROR]\033[0m %s\n' "$*" >&2; }
die(){ err "$*"; exit 1; }

[ "$(uname -s)" = "Linux" ] || die "This script must run on a Linux x86_64 build host."
command -v git >/dev/null || die "git not found."
command -v make >/dev/null || die "make not found."

mkdir -p "$WORKDIR"
cd "$WORKDIR"

# -----------------------------------------------------------------------------
# CI clone robustness -- authenticate github.com with the job token
# -----------------------------------------------------------------------------
# GitHub-hosted runners share NAT IPs; unauthenticated github.com clones hit
# the 60-request/hr secondary rate limit -> 403 -> git prompts for a username
# -> with GIT_TERMINAL_PROMPT=0 this fails as
# "could not read Username ... No such device or address" (or leaves an empty
# clone dir -> "make: No targets specified and no makefile found"). That breaks
# FrogUI + the libretro cores, which then fails the STAGE 8 ABI gate. Embed the
# job token in the github.com URL to raise the limit to 5000/hr and avoid the
# prompt; falls back to anonymous if no token is present (local dev). See
# HANDOFF.md self-heal note.
if [ -n "${GITHUB_TOKEN:-}" ]; then
    git config --global url."https://x-access-token:${GITHUB_TOKEN}@github.com/".insteadOf "https://github.com/" || true
fi

# clone_repo <repo> <dest> [extra-args...]
# Retry a shallow clone up to 3 times; on failure remove the (possibly partial)
# destination so a later retry is not skipped by a stale [ -d "$dest" ] guard.
clone_repo(){
    local repo="$1" dest="$2"; shift 2
    local i tries=3
    for ((i=1;i<=tries;i++)); do
        if git clone --depth 1 "$@" "$repo" "$dest" >/dev/null 2>&1; then
            return 0
        fi
        rm -rf "$dest"
        [ "$i" -lt "$tries" ] && sleep 5
    done
    return 1
}

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
    clone_repo "$ALSA_LIB_REPO" "$WORKDIR/alsa-lib"
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
    clone_repo "$PICOARCH_REPO" picoarch -b r36sx
else
    log "Reusing existing picoarch/ (ensure it is on r36sx AND patched)."
fi
# libretro-common submodule: provides libretro.h / RETRO_DEVICE_ID_*.
if [ ! -f picoarch/libretro-common/include/libretro.h ] \
   && [ ! -f picoarch/libretro-common/include/libretro/libretro.h ]; then
    log "Initializing libretro-common submodule (required for RETRO_DEVICE_ID_*)..."
    ( cd picoarch && git submodule update --init --recursive ) \
        || log "WARN: submodule init failed; run manually: cd picoarch && git submodule update --init libretro-common"
fi
# Apply the 5-edit patch (RTC + evdev gamepad) if a clean checkout and patch present.
if [ -f "$HERE/../patch/picoarch_5edits.patch" ]; then
    if git -C picoarch apply --ignore-whitespace --check "$HERE/../patch/picoarch_5edits.patch" 2>/dev/null; then
        log "Applying 5-edit patch (RTC + evdev gamepad)..."
        git -C picoarch apply --ignore-whitespace "$HERE/../patch/picoarch_5edits.patch"
    else
        log "5-edit patch already applied or not applicable -- skipping."
    fi
fi
# Apply the RK3036G display patch (fb0 direct-drive; fixes black screen on
# RK3036G which has no /dev/dis + no stock driver.so). MUST run AFTER the
# 5-edit patch above (both touch plat_sdl.c but in non-overlapping regions).
if [ -f "$HERE/../patch/picoarch_rk3036g_display.patch" ]; then
    if git -C picoarch apply --ignore-whitespace --check "$HERE/../patch/picoarch_rk3036g_display.patch" 2>/dev/null; then
        log "Applying RK3036G display patch (fb0 direct-drive)..."
        git -C picoarch apply --ignore-whitespace "$HERE/../patch/picoarch_rk3036g_display.patch"
    elif git -C picoarch apply --ignore-whitespace -R --check "$HERE/../patch/picoarch_rk3036g_display.patch" 2>/dev/null; then
        log "RK3036G display patch already applied -- skipping."
    else
        die "RK3036G display patch NOT applicable and NOT already applied -- would ship black-screen binary. Abort."
    fi
fi
[ -d FrogUI/.git ] || clone_repo "$FROGUI_REPO" FrogUI

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
# Apply the RK3036G build fix BEFORE compiling. The upstream FrogUI tip
# (commit 2f41ace) was developed for SF2000 (MIPS); the unix/ARM Makefile
# target references three symbols it never declares, so frogui_libretro.so
# fails to compile/link and the payload cannot boot (zhijack.sh requires it).
# This patch declares the game-launch buffers and stubs direct_loader/xlog.
# A failed/already-applied check is fatal: shipping without it = no menu.
if [ -f "$HERE/../patch/frogui_rk3036g_build.patch" ]; then
    if git -C FrogUI apply --ignore-whitespace --check "$HERE/../patch/frogui_rk3036g_build.patch" 2>/dev/null; then
        log "Applying RK3036G FrogUI build fix (declare buffers + stub direct_loader/xlog)..."
        git -C FrogUI apply --ignore-whitespace "$HERE/../patch/frogui_rk3036g_build.patch"
    elif git -C FrogUI apply --ignore-whitespace -R --check "$HERE/../patch/frogui_rk3036g_build.patch" 2>/dev/null; then
        log "RK3036G FrogUI build fix already applied -- skipping."
    else
        die "RK3036G FrogUI build fix NOT applicable and NOT already applied -- FrogUI would fail to build. Abort."
    fi
fi
log "Building FrogUI (frogui_libretro.so)..."
pushd FrogUI >/dev/null
make CC="$CC" CXX="$CXX" || true   # builds menu_libretro.so (the libretro menu core)
# The FrogUI libretro Makefile emits menu_libretro.so, but the rest of the
# pipeline (zhijack.sh, STAGE 8 ABI gate, STAGE 9 staging) expects
# frogui_libretro.so. Normalize the artifact name -- the libretro menu core IS
# FrogUI's payload for the autorun hijack (see HANDOFF.md self-heal note).
if [ ! -f frogui_libretro.so ] && [ -f menu_libretro.so ]; then
    cp menu_libretro.so frogui_libretro.so
    log "Normalized menu_libretro.so -> frogui_libretro.so"
fi
if [ ! -f frogui_libretro.so ] && [ -f Makefile.libretro ]; then
    make -f Makefile.libretro CC="$CC" CXX="$CXX" || true
    [ ! -f frogui_libretro.so ] && [ -f menu_libretro.so ] && cp menu_libretro.so frogui_libretro.so
fi
popd >/dev/null
if [ -f FrogUI/frogui_libretro.so ]; then
    log "FrogUI built."
else
    log "WARN: frogui_libretro.so not produced -- check FrogUI build output on this repo."
fi

# -----------------------------------------------------------------------------
# STAGE 7 -- build libretro cores (table-driven; builddir+mk from treefrog-ui build_all.sh)
CORE_TABLE="
fceumm|https://github.com/tzubertowski/libretro-fceumm|.|-f Makefile.libretro
nestopia|https://github.com/libretro/nestopia|libretro|
snes9x2005_plus|https://github.com/tzubertowski/snes9x2005|.|-
picodrive|https://github.com/libretro/picodrive|.|-f Makefile.libretro
stella2014|https://github.com/libretro/stella2014-libretro|.|-
"
CORE_OUT="$WORKDIR/cores"
mkdir -p "$CORE_OUT" "$WORKDIR/.toolchain"
ARM_FLAGS="-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -mlong-calls --sysroot=$SYSROOT -Ofast -DNDEBUG"
printf '#!/bin/bash\nexec %sgcc %s "$@"\n' "$CROSS_COMPILE" "$ARM_FLAGS" > "$WORKDIR/.toolchain/arm-gcc"
printf '#!/bin/bash\nexec %sg++ %s "$@"\n' "$CROSS_COMPILE" "$ARM_FLAGS" > "$WORKDIR/.toolchain/arm-g++"
chmod +x "$WORKDIR/.toolchain/arm-gcc" "$WORKDIR/.toolchain/arm-g++"
LDFLAGS_S="-shared -Wl,--no-undefined -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard --sysroot=$SYSROOT -L$SYSROOT/usr/lib -lm -lc -lstdc++"
build_core() {
    local name="$1" repo="$2" bdir="$3" mk="$4"
    local d="$WORKDIR/libretro-$name"
    if [ ! -d "$d/.git" ]; then
        clone_repo "$repo" "$d" || { log "WARN: core $name clone failed (rate-limited or offline)."; return; }
    fi
    [ -d "$d/.git" ] || { log "WARN: core $name missing clone dir."; return; }
    git -C "$d" submodule update --init --depth 1 >/dev/null 2>&1 || true
    pushd "$d/$bdir" >/dev/null
    make clean >/dev/null 2>&1 || true
    make $mk platform=unix \
        CC="$WORKDIR/.toolchain/arm-gcc" CXX="$WORKDIR/.toolchain/arm-g++" \
        AR="$CROSS_COMPILE"ar RANLIB="$CROSS_COMPILE"ranlib LD="$WORKDIR/.toolchain/arm-g++" \
        LDFLAGS="$LDFLAGS_S" -j"$(nproc)" 2>&1 | tail -5 \
        || { log "WARN: core $name build had issues (may need per-core tweaks)."; popd >/dev/null; return; }
    local so
    for so in "$d/$bdir/${name}_libretro.so" "$d/$bdir/$(basename "$bdir")_libretro.so"; do
        [ -f "$so" ] && cp "$so" "$CORE_OUT/" && log "  -> $CORE_OUT/$(basename "$so")" && popd >/dev/null && return
    done
    so=$(find "$d/$bdir" -maxdepth 1 -name "*_libretro.so" 2>/dev/null | head -1)
    [ -n "$so" ] && cp "$so" "$CORE_OUT/" && log "  -> $CORE_OUT/$(basename "$so")"
    popd >/dev/null
}
while IFS='|' read -r name repo bdir mk; do
    [ -z "$name" ] && continue
    case " $CORES " in *" $name "*) log "Building libretro core: $name"; build_core "$name" "$repo" "$bdir" "$mk";; esac
done <<< "$CORE_TABLE"

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

# -----------------------------------------------------------------------------
# STAGE 9b -- bundle runtime libs picoarch + frogui need into cubegm/lib
#   picoarch links SDL/libpng12/z/asound against the CACHED toolchain's bundled
#   sysroot ($ARM_GNU/arm-linux-gnueabihf/sysroot), NOT build.sh's glibc-2.17
#   $SYSROOT. The device loader sets LD_LIBRARY_PATH=cubegm/lib, so we copy every
#   NEEDED .so (and transitive deps) there to be self-contained. Base libs
#   (libc/libm/pthread/dl/gcc/ld) are provided by the device rootfs, so we
#   exclude them to avoid shipping a second glibc that mismatches the device's
#   dynamic linker.
# -----------------------------------------------------------------------------
log "Bundling runtime libs into $DST/lib ..."
mkdir -p "$DST/lib"
READELF="${CROSS_COMPILE}readelf"
# base libs the device rootfs always provides -- do NOT bundle these
BASE_LIBS="libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 libgcc_s.so.1 \
           librt.so.1 libutil.so.1 ld-linux-armhf.so.3 ld-2.17.so \
           libstdc++.so.6 libatomic.so.1"
is_base() { for b in $BASE_LIBS; do [ "$1" = "$b" ] && return 0; done; return 1; }
# Search roots: glibc-2.17 sysroot, gcc's configured sysroot, AND the cached
# toolchain's bundled sysroot (where SDL/libpng/z/asound actually live).
GCC_SYSROOT="$(${CC} -print-sysroot 2>/dev/null)"
SEARCH_ROOTS=""
for _r in "$SYSROOT" "$GCC_SYSROOT" "$ARM_GNU/arm-linux-gnueabihf/sysroot" "$ARM_GNU/arm-linux-gnueabihf/libc"; do
    [ -n "$_r" ] && [ -d "$_r" ] && SEARCH_ROOTS="$SEARCH_ROOTS $_r"
done
# Resolve a SONAME to an absolute real path:
#   1) gcc's own resolver (authoritative -- same search the STAGE 5 link used)
#   2) scan SEARCH_ROOTS for the SONAME or its version-stripped linker name
#   3) scan the whole $ARM_GNU tree (libs may sit outside gcc's configured sysroot)
resolve_lib() {
    local soname="$1" linker="${1%.so.*}.so" p
    p=$(${CC} -print-file-name="$soname" 2>/dev/null)
    [ -n "$p" ] && [ -e "$p" ] && { readlink -f "$p"; return 0; }
    for _r in $SEARCH_ROOTS; do
        p=$(find "$_r" \( -name "$soname" -o -name "$linker" \) -type f 2>/dev/null | head -1)
        [ -n "$p" ] && { echo "$p"; return 0; }
    done
    if [ -n "$ARM_GNU" ] && [ -d "$ARM_GNU" ]; then
        p=$(find "$ARM_GNU" \( -name "$soname" -o -name "$linker" \) -type f 2>/dev/null | head -1)
        [ -n "$p" ] && { echo "$p"; return 0; }
    fi
    return 1
}
# extract NEEDED SONAMEs from a binary -- readelf -d line:
#   0x00000001 (NEEDED) Shared library: [libc.so.6]
# split on [ and ] -> field 2 is the name inside the brackets.
needed_of() {
    if command -v "$READELF" >/dev/null 2>&1; then
        "$READELF" -d "$1" 2>/dev/null | awk -F'[][]' '/NEEDED/ { gsub(/[ \t]/,"",$2); if($2!="") print $2 }'
    fi
}
declare -A _seen=()
_queue=()
for _b in picoarch/picoarch FrogUI/frogui_libretro.so; do
    [ -f "$_b" ] || continue
    while IFS= read -r _l; do [ -n "$_l" ] && _queue+=("$_l"); done < <(needed_of "$_b")
done
# fallback: if readelf was unavailable, seed the known direct deps
if [ ${#_queue[@]} -eq 0 ]; then
    _queue=(libSDL-1.2.so.0 libSDL.so.1 libpng12.so.0 libz.so.1 libasound.so.2)
    log "WARN: readelf unavailable -- seeding hardcoded SDL/libpng/z/asound."
fi
while [ ${#_queue[@]} -gt 0 ]; do
    _lib="${_queue[0]}"; _queue=("${_queue[@]:1}")
    [ -n "${_seen[$_lib]:-}" ] && continue
    _seen[$_lib]=1
    is_base "$_lib" && continue
    if ! _found=$(resolve_lib "$_lib"); then
        log "WARN: runtime lib $_lib not found -- device must provide it."
        continue
    fi
    cp -L "$_found" "$DST/lib/" 2>/dev/null || log "WARN: copy failed for $_lib"
    while IFS= read -r _dep; do [ -n "$_dep" ] && _queue+=("$_dep"); done < <(needed_of "$_found")
done
log "Bundled $(ls -1 "$DST/lib" 2>/dev/null | wc -l) runtime libs into $DST/lib."

log "Staged into $DST"
log "DONE. Copy the whole '$DST' directory to the root of your device SD card,"
log "overwriting the existing cubegm/ (stock rkgame/icube/driver.so/root.dat stay)."
log "On next boot the device launches picoarch + FrogUI directly."
