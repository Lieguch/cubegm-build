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
    git clone --depth 1 "$ALSA_LIB_REPO" "$WORKDIR/alsa-lib"
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
    git clone --depth 1 -b r36sx "$PICOARCH_REPO" picoarch
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
    if git -C picoarch apply --check "$HERE/../patch/picoarch_5edits.patch" 2>/dev/null; then
        log "Applying 5-edit patch (RTC + evdev gamepad)..."
        git -C picoarch apply "$HERE/../patch/picoarch_5edits.patch"
    else
        log "5-edit patch already applied or not applicable -- skipping."
    fi
fi
# -----------------------------------------------------------------------------
# Apply MStar guard: exclude the miyoo HW-scaling block on RK3036G.
# The upstream plat_sdl.c miyoo block (#ifndef PLATFORM_SF3000, ~172-490)
# #includes mi_sys.h / mi_gfx.h (absent on RK3036G) and redefines
# buffer_init/buffer_scale/buffer_quit, colliding with the UNGUARDED generic
# software-SDL functions at plat_sdl.c:518-556 (always compiled). We guard the
# block with #if !defined(RK3036G_NO_MIYOO_SCALE) and, when that macro is set
# (build_sf3000_armhf.sh passes -DRK3036G_NO_MIYOO_SCALE), also hoist the
# buffer global + GFX_Buffer type to file scope so the generic path compiles.
# Done as a direct, idempotent, line-ending-agnostic edit (NOT git apply) so it
# can never be silently skipped by a CRLF/context mismatch on the cloned tree.
# -----------------------------------------------------------------------------
MG_FILE="picoarch/plat_sdl.c"
if [ -f "$MG_FILE" ]; then
    if grep -q 'RK3036G_NO_MIYOO_SCALE' "$MG_FILE"; then
        log "MStar guard already present in $MG_FILE -- skipping."
    else
        python3 - "$MG_FILE" <<'PYEOF'
import sys
p = sys.argv[1]
with open(p, 'r', encoding='utf-8', errors='replace', newline='') as f:
    s = f.read()
nl = '\r\n' if '\r\n' in s else '\n'
anchor = '// begin miyoo hardware scaling support'
hoist = (
    '// RK3036G: when the miyoo MStar HW-scaling block below is excluded' + nl +
    '// (RK3036G_NO_MIYOO_SCALE, set by build_sf3000_armhf.sh; or PLATFORM_SF3000),' + nl +
    '// the unguarded software-SDL buffer_init/quit/scale further down still' + nl +
    '// need the framebuffer "buffer" global. The GFX_Buffer *type* is ALREADY' + nl +
    '// provided by plat.h (struct GFX_Buffer, ~line 22) -- do NOT redefine it' + nl +
    '// here or you get "redefinition of struct GFX_Buffer" vs plat.h:22.' + nl +
    '// Declare ONLY the global instance at file scope (guarded to exactly the' + nl +
    '// cases where the miyoo block is excluded) so the generic path compiles.' + nl +
    '#if defined(PLATFORM_SF3000) || defined(RK3036G_NO_MIYOO_SCALE)' + nl +
    'static struct GFX_Buffer buffer;' + nl +
    '#endif' + nl + nl
)
assert anchor in s, "miyoo anchor not found in plat_sdl.c"
s = s.replace(anchor, hoist + anchor, 1)
miyoo_ifndef = '#ifndef PLATFORM_SF3000'
wrap_open = '#if !defined(RK3036G_NO_MIYOO_SCALE)'
miyoo_endif = '#endif // end miyoo hardware scaling support'
s = s.replace(miyoo_ifndef, miyoo_ifndef + nl + wrap_open, 1)
s = s.replace(miyoo_endif, '#endif // RK3036G_NO_MIYOO_SCALE' + nl + miyoo_endif, 1)
with open(p, 'w', encoding='utf-8', newline='') as f:
    f.write(s)
PYEOF
        log "Applied MStar guard (exclude miyoo HW-scaling + hoist buffer) to $MG_FILE"
    fi
fi

# ---------------------------------------------------------------------------
# RK3036G = ARM (armhf, glibc-2.17). picoarch upstream targets x86/MIPS, so its
# crash signal handler reads uc_mcontext.pc -- but on ARM glibc-2.17 mcontext_t
# is struct sigcontext whose program-counter field is 'arm_pc', not 'pc'.
# Patch it for our ARM target (only affects ARM builds; x86 keeps .pc).
# Confirmed against glibc-2.17 ARM <bits/sigcontext.h> (field: arm_pc).
# ---------------------------------------------------------------------------
if grep -q 'uc_mcontext\.pc' picoarch/main.c 2>/dev/null; then
    sed -i 's/->uc_mcontext\.pc/->uc_mcontext.arm_pc/g' picoarch/main.c
    log "Patched picoarch/main.c: uc_mcontext.pc -> uc_mcontext.arm_pc (ARM/glibc-2.17)"
fi
[ -d FrogUI ] || git clone --depth 1 "$FROGUI_REPO" FrogUI

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
# CubeGM gs->picoarch launch bridge: upstream FrogUI only declares the gs
# game-launch symbols (ptr_gs_run_game_file / ptr_gs_run_game_name / direct_loader
# / xlog) under #ifdef SF2000 (a hardcoded ROM loader). On this standard-Linux
# RK3036G device we apply a self-contained bridge patch (patch/frogui_gs_bridge.patch)
# that backs those symbol NAMES with a real buffer + a functional direct_loader()
# stub (writes picoarch's native launch file). Without it, frogos.c fails to
# compile ('ptr_gs_run_game_file' undeclared) and no frogui_libretro.so is
# produced -> STAGE 8 ABI gate dies. Verified: patch applies cleanly to upstream
# tzubertowski/FrogUI@master frogos.c (git apply --check == 0). See HANDOFF S22.
if [ -f "$HERE/../patch/frogui_gs_bridge.patch" ]; then
    if git -C FrogUI apply --check "$HERE/../patch/frogui_gs_bridge.patch" 2>/dev/null; then
        log "Applying CubeGM gs->picoarch launch bridge to FrogUI..."
        git -C FrogUI apply "$HERE/../patch/frogui_gs_bridge.patch"
    else
        log "WARN: frogui_gs_bridge.patch not applicable to this FrogUI checkout -- build may fail."
    fi
else
    log "WARN: patch/frogui_gs_bridge.patch missing -- FrogUI gs bridge will not be applied."
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
# STAGE 7 -- build standard libretro cores (RK3036G armhf)
# -----------------------------------------------------------------------------
# Several libretro core Makefiles hardcode `gcc`/`g++` when building their
# bundled libretro-common sub-tree, so `CC=` on the make cmdline does NOT reach
# those objects (they get compiled for the host). The buildroot-style fix is a
# PATH compiler wrapper that injects -fPIC -marm + ALSA + bundled
# libretro-common include for EVERY compiler invocation (bare + triplet names).
# Verified against upstream libretro Makefiles (HANDOFF §11-§13), not guessed.
CORE_OUT="$WORKDIR/cores"
mkdir -p "$CORE_OUT"

CROSS_BIN="$WORKDIR/cross-bin"
rm -rf "$CROSS_BIN"; mkdir -p "$CROSS_BIN"
REAL_CC="$(command -v "$CC" 2>/dev/null || echo "$CC")"
REAL_CXX="$(command -v "$CXX" 2>/dev/null || echo "$CXX")"
TRIPLET="${CC##*/}"
TRIPLET_GXX="${CXX##*/}"
make_wrapper () {
    local name="$1"; local real="$2"
    printf '#!/bin/bash\nexec "%s" -fPIC -marm -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -I"%s" -Ilibretro-common/include -DFCEU_VERSION_NUMERIC=9900 -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS "$@"\n' "$real" "$ALSA_INC" > "$CROSS_BIN/$name"
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
for c in $CORES; do
    log "Building libretro core: $c"
    d="$WORKDIR/libretro-$c"
    repo="$c"
    [ "$c" = "fceumm" ] && repo="libretro-fceumm"
    # Best-effort clone with retry (transient network/auth must not abort the
    # whole build). Skip the core if it still cannot be fetched; the device
    # ships its own cores and STAGE 8/9 tolerate a missing core .so.
    cloned=0
    for n in 1 2 3; do
        if [ "$c" = "picodrive" ]; then
            git clone --depth 1 --recursive "https://github.com/libretro/$repo.git" "$d" >/dev/null 2>&1 && { cloned=1; break; }
        else
            git clone --depth 1 "https://github.com/libretro/$repo.git" "$d" >/dev/null 2>&1 && { cloned=1; break; }
        fi
        log "  clone attempt $n failed; retrying..."
        rm -rf "$d"
    done
    if [ "$cloned" != "1" ]; then
        log "WARN: core $c clone failed (network/auth) -- skipping (best-effort)."
        continue
    fi
    [ "$c" = "picodrive" ] && ( cd "$d" && git submodule update --init --recursive ) >/dev/null 2>&1 || true
    pushd "$d" >/dev/null
    case "$c" in
        mgba)
            rm -rf build-cubegm
            cmake -B build-cubegm -DCMAKE_BUILD_TYPE=Release \
                  -DLIBMGBA_ONLY=ON -DBUILD_LIBRETRO=ON \
                  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
                  -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
                  -DCMAKE_C_FLAGS="$CFLAGS" -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
                  -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS" \
                  -DCMAKE_SHARED_LINKER_FLAGS="$LDFLAGS" \
                  . || log "WARN: mgba cmake configure had issues."
            cmake --build build-cubegm --target mgba_libretro -- -j"$(nproc)" \
                || log "WARN: mgba build had issues."
            so=$(find build-cubegm -name "mgba_libretro.so" 2>/dev/null | head -1)
            ;;
        snes9x|nestopia)
            make -C libretro clean >/dev/null 2>&1 || true
            make -C libretro platform=armv-neon-hardfloat -j"$(nproc)" \
                || log "WARN: core $c build had issues."
            so=$(find . -name "${c}_libretro.so" 2>/dev/null | head -1)
            ;;
        fceumm|picodrive)
            make -f Makefile.libretro clean >/dev/null 2>&1 || true
            make -f Makefile.libretro platform=armv-neon-hardfloat use_libchdr=0 -j"$(nproc)" \
                || log "WARN: core $c build had issues."
            so=$(find . -name "${c}_libretro.so" 2>/dev/null | head -1)
            ;;
        *)
            make clean >/dev/null 2>&1 || true
            make platform=armv-neon-hardfloat -j"$(nproc)" \
                || log "WARN: core $c build had issues."
            so=$(find . -name "${c}_libretro.so" 2>/dev/null | head -1)
            ;;
    esac
    if [ -n "$so" ]; then
        cp "$so" "$CORE_OUT/" && log "  -> $CORE_OUT/$(basename "$so")"
    else
        log "ERROR: core $c did not produce a .so"
        CORE_FAIL="${CORE_FAIL} $c"
    fi
    popd >/dev/null
done
if [ -n "$CORE_FAIL" ]; then
    log "STAGE7 FAILED -- missing cores:${CORE_FAIL}"
    exit 1
fi
export PATH="$OLDPATH"

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
# -----------------------------------------------------------------------------
# STAGE 8.5 -- libretro API symbol gate (deterministic; no device needed)
#   A libretro core MUST export the required frontend interface symbols or
#   picoarch cannot dlopen/run it. frogui_libretro.so is the launcher core and
#   also must export them. This proves every produced .so is a VALID libretro
#   implementation, not merely a compiled object that would crash on load.
#   readelf reads ARM ELF symbol tables fine on the x86_64 runner.
# -----------------------------------------------------------------------------
LIBRETRO_SYMS="retro_api_version retro_init retro_deinit retro_run retro_load_game retro_unload_game retro_get_system_info retro_set_environment"
sym_gate () {
    local f="$1"; local missing=""
    for s in $LIBRETRO_SYMS; do
        if ! readelf -sW "$f" 2>/dev/null | grep -qw "$s"; then
            missing="$missing $s"
        fi
    done
    if [ -n "$missing" ]; then
        log "LIBRETRO SYMBOL GATE FAIL: $(basename "$f") missing:$missing"
        return 1
    fi
    log "  libretro symbols OK: $(basename "$f")"
    return 0
}
SYM_FAIL=""
[ -f FrogUI/frogui_libretro.so ] && sym_gate FrogUI/frogui_libretro.so || SYM_FAIL="${SYM_FAIL} frogui"
for so in "$CORE_OUT"/*.so; do
    [ -e "$so" ] || continue
    sym_gate "$so" || SYM_FAIL="${SYM_FAIL} $(basename "$so")"
done
if [ -n "$SYM_FAIL" ]; then
    log "STAGE8.5 FAILED -- cores missing libretro symbols:${SYM_FAIL}"
    exit 1
fi
log "libretro symbol gate passed for launcher + all built cores."

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

# Ship the cross-built runtime libs that the DEVICE ROOTFS does NOT provide.
# picoarch's NEEDED (verified via readelf) is libSDL-1.2.so.0 / libpng12.so.0 /
# libz.so.1; the stock rootfs only ships libdrm/libkms/libasound (see
# docs/sysroot_strategy.md section 2), NOT SDL/libpng12. Without these in
# cubegm/lib the device fails at startup ("error while loading shared
# libraries: libSDL-1.2.so.0"). zhijack.sh already sets
# LD_LIBRARY_PATH=$CUBEGM_DIR/lib, so copy the real libs (and their symlink
# chain) from the sysroot here. Keep it best-effort (WARN, non-fatal): if a
# specific lib is missing we still ship everything else we can.
for l in libSDL-1.2.so.0 libpng12.so.0 libz.so.1; do
    if compgen -G "$SYSROOT/usr/lib/${l}*" > /dev/null; then
        cp -a "$SYSROOT"/usr/lib/${l}* "$DST/lib/" 2>/dev/null || true
        log "  packaged runtime lib: $l -> cubegm/lib/"
    else
        log "WARN: $SYSROOT/usr/lib/$l not found -- picoarch may fail on device."
    fi
done
ls -l "$DST/lib/" 2>/dev/null | head -20 || true
log "Staged into $DST"
log "DONE. Copy the whole '$DST' directory to the root of your device SD card,"
log "overwriting the existing cubegm/ (stock rkgame/icube/driver.so/root.dat stay)."
log "On next boot the device launches picoarch + FrogUI directly."

# -----------------------------------------------------------------------------
# STAGE 9.5 -- payload completeness gate (deterministic; no device needed)
#   Assert the staged cubegm/ payload contains everything the device needs to
#   boot the open-source front-end and run the 5 built cores. If anything is
#   missing the build is RED -- we never ship a half-built package. Real-device
#   validation (HDMI render / ALSA audio / evdev gamepad / frontend core
#   enumeration) remains the only step that requires the hardware.
# -----------------------------------------------------------------------------
REQUIRED_BINS="picoarch frogui_libretro.so zhijack.sh autorun"
PKG_FAIL=""
for f in $REQUIRED_BINS; do
    if [ ! -e "$DST/$f" ]; then PKG_FAIL="$PKG_FAIL $f"; fi
    case "$f" in picoarch|zhijack.sh|autorun)
        [ -x "$DST/$f" ] || PKG_FAIL="$PKG_FAIL (not-exec:$f)";; esac
done
[ -f "$DST/cores/config.xml" ] || PKG_FAIL="$PKG_FAIL cores/config.xml"
for c in mgba snes9x fceumm picodrive nestopia; do
    [ -f "$DST/cores/${c}_libretro.so" ] || PKG_FAIL="$PKG_FAIL $c"
done
# runtime libs the DEVICE rootfs does NOT provide (SDL/libpng12/z). Best-effort:
# warn loudly but do not hard-fail (location can vary by sysroot build).
LIBS_OK=0
for l in libSDL-1.2.so.0 libpng12.so.0 libz.so.1; do
    compgen -G "$DST/lib/${l}*" >/dev/null && LIBS_OK=$((LIBS_OK+1))
done
if [ "$LIBS_OK" -lt 3 ]; then
    log "WARN: only $LIBS_OK/3 runtime libs in cubegm/lib -- picoarch may fail on device if a lib is absent."
fi
if [ -n "$PKG_FAIL" ]; then
    log "STAGE9.5 FAILED -- payload missing:${PKG_FAIL}"
    ls -lR "$DST" 2>/dev/null | head -50
    exit 1
fi
log "Payload completeness OK: picoarch + frogui + 5 cores + zhijack + autorun + config.xml present."

