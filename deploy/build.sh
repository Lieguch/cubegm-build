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
# Game-able core set: FBA2012 (000 arcade group), FBNeo, MD, PS1 (lightrec),
# GBA/NES/SNES/Atari/GBC etc. fbalpha2012_* build fast; pcsx/mgba slower.
DEFAULT_CORES="fceumm snes9x2005_plus picodrive stella2014 mgba nestopia vba_next tgbdual gpsp prosystem mame2000 mame2003_plus fbalpha2012 fbalpha2012_cps1 fbalpha2012_cps2 fbalpha2012_cps3 fbalpha2012_neogeo fbneo pcsx_rearmed"
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
if [ -n "${GITHUB_TOKEN:-}" ]; then
    git config --global url."https://x-access-token:${GITHUB_TOKEN}@github.com/".insteadOf "https://github.com/" || true
fi
export GIT_TERMINAL_PROMPT=0

# clone_repo <repo> <dest> [extra-args...] -- retry shallow clone 3x; remove partial dest
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
# DRM UAPI headers for the RK3036G HDMI modeset path (hwdisp_drm in the display
# patch #includes <drm/drm.h>). The crosstool glibc-2.17 sysroot ships no DRM
# headers, so bundle them (Linux v4.4 UAPI, MIT) and drop them into the source.
mkdir -p picoarch/include/drm
cp -f "$HERE/drm_headers/drm/"*.h picoarch/include/drm/ 2>/dev/null || log "WARN: drm headers not copied (hwdisp_drm won't compile!)"
# Crash backtrace logging (SIGSEGV/SIGABRT -> /mnt/sdcard/crash.log).
# Applies to core.c AFTER 5-edit + display (its core.c hunks are based on that
# state). Build with -rdynamic (added above) so backtrace shows symbol names.
if [ -f "$HERE/../patch/core_rk3036g_crashlog.patch" ]; then
    if git -C picoarch apply --ignore-whitespace --check "$HERE/../patch/core_rk3036g_crashlog.patch" 2>/dev/null; then
        log "Applying RK3036G crashlog patch (backtrace on SIGSEGV)..."
        git -C picoarch apply --ignore-whitespace "$HERE/../patch/core_rk3036g_crashlog.patch"
    elif git -C picoarch apply --ignore-whitespace -R --check "$HERE/../patch/core_rk3036g_crashlog.patch" 2>/dev/null; then
        log "RK3036G crashlog patch already applied -- skipping."
    else
        log "WARN: RK3036G crashlog patch NOT applicable."
    fi
fi
# RK3036G input patch (gamepad): PSX-style keycodes in evdev defbinds + d-pad
# hat handling. MUST run after the display patch (both touch plat_sf3000.c?
# no -- input touches plat_sf3000.c + libpicofe/linux/in_evdev.c; display
# touches hwdisp.c/plat_sdl.c/plat.h, so no overlap).
if [ -f "$HERE/../patch/picoarch_rk3036g_input.patch" ]; then
    if git -C picoarch apply --ignore-whitespace --check "$HERE/../patch/picoarch_rk3036g_input.patch" 2>/dev/null; then
        log "Applying RK3036G input patch (gamepad keycodes + hat)..."
        git -C picoarch apply --ignore-whitespace "$HERE/../patch/picoarch_rk3036g_input.patch"
    elif git -C picoarch apply --ignore-whitespace -R --check "$HERE/../patch/picoarch_rk3036g_input.patch" 2>/dev/null; then
        log "RK3036G input patch already applied -- skipping."
    else
        log "WARN: RK3036G input patch NOT applicable and NOT applied -- gamepad may not work."
    fi
fi
# RK3036G v8.6 patch (applied LAST, on top of input/display/crashlog):
#   - hwdisp.c: RGB565 dumb buffer (halves per-frame write volume -> FPS),
#     aspect-fit letterbox for game frames, 90° rotation row-pointer bug fix,
#     cvt565 LUT, per-frame present timing log, exported hwdisp_restore()
#   - plat_sdl.c: exported plat_sound_finish(), ALSA open retry, skip SDL
#     joystick on RK3036G (duplicate devices -> menu drift)
#   - core.c/main.c: crash-log handler installed at startup, SIGBUS/ILL/FPE
#     covered, ROM path in crash.log
#   - menu.c/libpicofe/menu.c: pause-menu auto-repeat 70/100ms -> 500ms
if [ -f "$HERE/../patch/picoarch_rk3036g_v86.patch" ]; then
    if git -C picoarch apply --ignore-whitespace --check "$HERE/../patch/picoarch_rk3036g_v86.patch" 2>/dev/null; then
        log "Applying RK3036G v8.6 patch (FPS/menu/audio/crashlog/display-restore)..."
        git -C picoarch apply --ignore-whitespace "$HERE/../patch/picoarch_rk3036g_v86.patch"
    elif git -C picoarch apply --ignore-whitespace -R --check "$HERE/../patch/picoarch_rk3036g_v86.patch" 2>/dev/null; then
        log "RK3036G v8.6 patch already applied -- skipping."
    else
        log "WARN: RK3036G v8.6 patch NOT applicable and NOT applied."
    fi
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
# Apply the RK3036G 000-008 folder-table patch (stock-core aliases + arcade/PS1)
if [ -f "$HERE/../patch/frogui_table_000_008.patch" ]; then
    if git -C FrogUI apply --ignore-whitespace --check "$HERE/../patch/frogui_table_000_008.patch" 2>/dev/null; then
        log "Applying RK3036G FrogUI 000-008 folder table patch..."
        git -C FrogUI apply --ignore-whitespace "$HERE/../patch/frogui_table_000_008.patch"
    elif git -C FrogUI apply --ignore-whitespace -R --check "$HERE/../patch/frogui_table_000_008.patch" 2>/dev/null; then
        log "RK3036G FrogUI 000-008 table already applied -- skipping."
    else
        log "WARN: frogui 000-008 table patch NOT applicable -- arcade/PS1 folders will not resolve."
    fi
fi
# Extension fallback: ROMs dropped straight into /roms root (folder name not in
# console_mappings -> previously nothing happened on select). Maps by file ext.
if [ -f "$HERE/../patch/frogui_ext_fallback.patch" ]; then
    if git -C FrogUI apply --ignore-whitespace --check "$HERE/../patch/frogui_ext_fallback.patch" 2>/dev/null; then
        log "Applying FrogUI extension-fallback patch (root .bin/.md/.nes/... resolvable)..."
        git -C FrogUI apply --ignore-whitespace "$HERE/../patch/frogui_ext_fallback.patch"
    elif git -C FrogUI apply --ignore-whitespace -R --check "$HERE/../patch/frogui_ext_fallback.patch" 2>/dev/null; then
        log "FrogUI ext-fallback already applied -- skipping."
    else
        log "WARN: FrogUI ext-fallback patch NOT applicable -- root ROMs will not resolve."
    fi
fi
# ROMS_PATH runtime probe (roms vs stock Roms): fixes black/empty menu when the
# card is mounted case-sensitively (stock card ships Roms/).
if [ -f "$HERE/../patch/frogui_roms_path.patch" ]; then
    if git -C FrogUI apply --ignore-whitespace --check "$HERE/../patch/frogui_roms_path.patch" 2>/dev/null; then
        log "Applying FrogUI roms-path probe patch (roms/Roms)..."
        git -C FrogUI apply --ignore-whitespace "$HERE/../patch/frogui_roms_path.patch"
    elif git -C FrogUI apply --ignore-whitespace -R --check "$HERE/../patch/frogui_roms_path.patch" 2>/dev/null; then
        log "FrogUI roms-path patch already applied -- skipping."
    else
        log "WARN: FrogUI roms-path patch NOT applicable."
    fi
fi
# FrogUI v8.6 patch (applied LAST, on top of roms_path): /tmp file scan cache
# (cross-process, fixes slow menu), L/R paging in the file list, ALSA release
# before the game fork (dlsym plat_sound_finish -> game sound), and display
# re-assert after the game child exits (dlsym hwdisp_restore -> no black screen
# after a crashed ROM).
if [ -f "$HERE/../patch/frogui_v86.patch" ]; then
    if git -C FrogUI apply --ignore-whitespace --check "$HERE/../patch/frogui_v86.patch" 2>/dev/null; then
        log "Applying FrogUI v8.6 patch (scan cache / L-R paging / audio pre-fork / display restore)..."
        git -C FrogUI apply --ignore-whitespace "$HERE/../patch/frogui_v86.patch"
    elif git -C FrogUI apply --ignore-whitespace -R --check "$HERE/../patch/frogui_v86.patch" 2>/dev/null; then
        log "FrogUI v8.6 patch already applied -- skipping."
    else
        log "WARN: FrogUI v8.6 patch NOT applicable and NOT applied."
    fi
fi
pushd FrogUI >/dev/null
# Build the libretro core via Makefile.sf3000 (LIBRETRO_TARGET=frogui_libretro.so,
# LIBRETRO_SOURCES=frogui_libretro.c with get_core_for_folder + execl(picoarch)).
# Command-line CC/CFLAGS/SYSROOT override the MIPS hardcodes (GNU make precedence).
# v8.6.1: NO silent fallback. Run #276's make failed (RTLD_DEFAULT/lt_last errors)
# and the old code silently "normalized" the unix Makefile's menu_libretro.so (a
# DIFFERENT binary with no FrogUI UI code) into frogui_libretro.so -> device
# black-screen crash loop (retro_init pc=(nil)). A missing/empty FrogUI must fail
# the build loudly, never ship a fake.
if [ -f Makefile.sf3000 ]; then
    make -f Makefile.sf3000 frogui_libretro.so \
        CC="$CC" CXX="$CXX" \
        STRIP="$CROSS_COMPILE"strip \
        CFLAGS="-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -mlong-calls --sysroot=$SYSROOT -fPIC -Wall -Ofast -DPLATFORM_SF3000 -DNDEBUG -DSCREEN_WIDTH=1280 -DSCREEN_HEIGHT=720 -DUI_SCALE=100 -I$SYSROOT/usr/include" \
        SYSROOT="$SYSROOT" || { popd >/dev/null; die "FrogUI make failed (Makefile.sf3000) -- fix frogui_libretro.c, do NOT ship."; }
fi
popd >/dev/null
if [ -f FrogUI/frogui_libretro.so ]; then
    # v8.6.1: sanity-check it really is FrogUI (not an empty/stubbed core) --
    # catches build-script regressions that ship a non-FrogUI binary.
    if grep -q "fork picoarch" FrogUI/frogui_libretro.so 2>/dev/null || \
       strings FrogUI/frogui_libretro.so 2>/dev/null | grep -q "retro_init start"; then
        log "FrogUI built (verified FrogUI binary)."
    else
        die "FrogUI frogui_libretro.so lacks FrogUI code (no 'retro_init start' string) -- refusing to ship a fake menu."
    fi
else
    die "frogui_libretro.so not produced -- FrogUI build failed."
fi

# -----------------------------------------------------------------------------
# STAGE 7 -- build libretro cores (table-driven; builddir+mk from treefrog-ui build_all.sh)
CORE_TABLE="
fceumm|https://github.com/tzubertowski/libretro-fceumm|.|-f Makefile.libretro||arm
snes9x2005_plus|https://github.com/tzubertowski/snes9x2005|.|-||arm
snes9x2002|https://github.com/tzubertowski/snes9x2002|.|-||arm
snes9x2010|https://github.com/libretro/snes9x2010|.|-f Makefile.libretro|LTO=|arm
picodrive|https://github.com/libretro/picodrive|.|-f Makefile.libretro|CFLAGS=-DAT_HWCAP2=26 NO_ARM_ASM=1 LDFLAGS=__LDFLAGS_S__|arm
stella2014|https://github.com/libretro/stella2014-libretro|.|-|LDFLAGS=__LDFLAGS_S__|arm
mgba|https://github.com/libretro/mgba|.|-f Makefile.libretro||arm|rebase
vba_next|https://github.com/libretro/vba-next|.|-||arm
tgbdual|https://github.com/libretro/tgbdual-libretro|.|-||arm
gambatte|https://github.com/libretro/gambatte-libretro|.|-f Makefile.libretro||arm
gpsp|https://github.com/libretro/gpsp|.|-f Makefile|platform=classic_armv7_a7|arm
prosystem|https://github.com/libretro/prosystem-libretro|.|-|LDFLAGS=__LDFLAGS_S__|arm
mame2000|https://github.com/libretro/mame2000-libretro|.|-||arm
mame2003_plus|https://github.com/libretro/mame2003-plus-libretro|.|-|LDFLAGS=__LDFLAGS_S__|arm
fbalpha2012|https://github.com/libretro/fbalpha2012|svn-current/trunk|-f makefile.libretro||fba
fbalpha2012_cps1|https://github.com/libretro/fbalpha2012_cps1|.|-f makefile.libretro||fba
fbalpha2012_cps2|https://github.com/libretro/fbalpha2012_cps2|.|-||fba
fbalpha2012_cps3|https://github.com/libretro/fbalpha2012_cps3|svn-current/trunk|-f makefile.libretro||fba
fbalpha2012_neogeo|https://github.com/libretro/fbalpha2012_neogeo|.|-f makefile.libretro||fba
fbneo|https://github.com/libretro/FBNeo|src/burner/libretro|-|CFLAGS=-DAT_HWCAP2=26 LDFLAGS=__LDFLAGS_S__|fba
pcsx_rearmed|https://github.com/libretro/pcsx_rearmed|.|-f Makefile.libretro|ARCH=arm DYNAREC=ari64 HAVE_NEON=1 BUILTIN_GPU=unai LDFLAGS=__LDFLAGS_S__ HAVE_PHYSICAL_CDROM=0|arm
nestopia|https://github.com/libretro/nestopia|libretro||arm
"
CORE_OUT="$WORKDIR/cores"
mkdir -p "$CORE_OUT" "$WORKDIR/.toolchain"
# -include stdint.h removed from the generic wrapper: gpsp/pcsx compile .S
# assembly with the same wrapper and the forced C header broke the assembler
# ("bad instruction __extension__"). Only the fba wrapper keeps it (FBA's old
# C code uses uint32_t etc. without including stdint.h).
ARM_FLAGS="-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -mlong-calls --sysroot=$SYSROOT -Ofast -DNDEBUG"
printf '#!/bin/bash\nexec %sgcc %s "$@"\n' "$CROSS_COMPILE" "$ARM_FLAGS" > "$WORKDIR/.toolchain/arm-gcc"
printf '#!/bin/bash\nexec %sg++ %s "$@"\n' "$CROSS_COMPILE" "$ARM_FLAGS" > "$WORKDIR/.toolchain/arm-g++"
printf '#!/bin/bash\nexec %sgcc %s "$@" -fno-strict-aliasing -fsigned-char -include stdint.h\n' "$CROSS_COMPILE" "$ARM_FLAGS" > "$WORKDIR/.toolchain/fba-gcc"
printf '#!/bin/bash\nexec %sg++ %s "$@" -fno-strict-aliasing -fsigned-char -include stdint.h\n' "$CROSS_COMPILE" "$ARM_FLAGS" > "$WORKDIR/.toolchain/fba-g++"
chmod +x "$WORKDIR/.toolchain/arm-gcc" "$WORKDIR/.toolchain/arm-g++" "$WORKDIR/.toolchain/fba-gcc" "$WORKDIR/.toolchain/fba-g++"
# Plain LDFLAGS: cores whose Makefile adds -shared/-fPIC itself (most libretro
# cores, and any with host build tools like picodrive's cyclone_gen -- passing
# -shared there links the host tool as a .so and dies). LDFLAGS_S is only for
# cores whose Makefile does NOT add -shared (mame2003_plus/fbneo/stella2014/
# prosystem per upstream build_all.sh) -- passed via extra LDFLAGS=__LDFLAGS_S__.
LDFLAGS="-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard --sysroot=$SYSROOT -L$SYSROOT/usr/lib -lm -lc -Wl,-Bstatic -lstdc++ -Wl,-Bdynamic -lpthread -static-libstdc++ -static-libgcc -rdynamic"
LDFLAGS_S="-shared -Wl,--no-undefined $LDFLAGS"
build_core() {
    local name="$1" repo="$2" bdir="$3" mk="$4" extra="$5" wrap="${6:-arm}" branch="${7:-}"
    local d="$WORKDIR/libretro-$name"
    if [ ! -d "$d/.git" ]; then
        if [ -n "$branch" ]; then
            clone_repo "$repo" "$d" --branch "$branch" || { log "WARN: core $name clone failed (branch $branch)."; return; }
        else
            clone_repo "$repo" "$d" || { log "WARN: core $name clone failed (rate-limited or offline)."; return; }
        fi
    fi
    [ -d "$d/.git" ] || { log "WARN: core $name missing clone dir."; return; }
    # Per-core patches (e.g. FBA family timer.h include fixes) live in
    # patch/core-<name>.patch and are applied inside the core repo.
    if [ -f "$HERE/../patch/core-${name}.patch" ]; then
        if git -C "$d" apply --ignore-whitespace --check "$HERE/../patch/core-${name}.patch" 2>/dev/null; then
            git -C "$d" apply --ignore-whitespace "$HERE/../patch/core-${name}.patch" && log "  $name: core patch applied"
        elif git -C "$d" apply --ignore-whitespace -R --check "$HERE/../patch/core-${name}.patch" 2>/dev/null; then
            log "  $name: core patch already applied"
        else
            log "WARN: core patch core-${name}.patch NOT applicable"
        fi
    fi
    git -C "$d" submodule update --init --recursive 2>&1 | tail -5 || log "WARN: submodule init incomplete for $name"
    # Some cores (nestopia/picodrive) carry libretro-common as an UNCONFIGURED
    # gitlink (no .gitmodules entry) -> submodule update silently skips it and
    # the include/ headers are missing. Detect and backfill from the official
    # repo (verified: headers exist upstream; API 200).
    _lc="$(find "$d" -maxdepth 4 -path "*/.git" -prune -o -type d -name libretro-common -print 2>/dev/null | head -1)"
    if [ -n "$_lc" ] && [ ! -f "$_lc/include/compat/strcasestr.h" ]; then
        log "WARN: $_lc vendored headers incomplete -- backfilling libretro-common (official)"
        rm -rf "$_lc"
        git clone --depth 1 https://github.com/libretro/libretro-common.git "$_lc" 2>&1 | tail -2 || log "WARN: libretro-common backfill failed"
    fi
    pushd "$d/$bdir" >/dev/null
    make clean >/dev/null 2>&1 || true
    # extra may contain CFLAGS=... -- pass it as an ENVIRONMENT variable, not a
    # make command-line var: a command-line CFLAGS overrides the core Makefile's
    # own "CFLAGS += ..." chain (kills libretro-common -I includes; observed on
    # picodrive run #248/#249). Env CFLAGS is honoured by "CFLAGS ?=" and still
    # appended by "CFLAGS +=". Save/restore so one core's CFLAGS can't leak.
    local -a EA=()
    local _old_cf="${CFLAGS-}"
    if [ -n "$extra" ]; then
        local x
        for x in $extra; do
            case "$x" in
                CFLAGS=*) export CFLAGS="${x#CFLAGS=}"; log "  $name: CFLAGS env = ${x#CFLAGS=}" ;;
                # single argv element (quoted) so -shared flags stay intact
                LDFLAGS=__LDFLAGS_S__) EA+=("LDFLAGS=$LDFLAGS_S") ;;
                *) EA+=("$x") ;;
            esac
        done
    fi
    # extra may carry LDFLAGS=__LDFLAGS_S__ (cores whose Makefile lacks -shared);
    # otherwise use the plain LDFLAGS and let the core Makefile add -shared.
    local _ld="$LDFLAGS"
    for x in "${EA[@]}"; do
        case "$x" in LDFLAGS=*) _ld="${x#LDFLAGS=}";; esac
    done
    # Force re-link: cached core clones + incremental make keep the OLD .so
    # when only LDFLAGS changed (e.g. -static-libstdc++ was ignored for
    # stella2014 -> GLIBCXX_3.4.32 still present in payload-258).
    find "$d" -name "*_libretro.so" -delete 2>/dev/null
    timeout 1800 make $mk platform=unix "${EA[@]}" \
        CC="$WORKDIR/.toolchain/$wrap-gcc" CXX="$WORKDIR/.toolchain/$wrap-g++" \
        AR="$CROSS_COMPILE"ar RANLIB="$CROSS_COMPILE"ranlib LD="$WORKDIR/.toolchain/$wrap-g++" \
        LDFLAGS="$_ld" -j"$(nproc)" 2>&1 | tail -25 \
        || { if [ -n "$_old_cf" ]; then export CFLAGS="$_old_cf"; else unset CFLAGS; fi; log "WARN: core $name build had issues (may need per-core tweaks)."; popd >/dev/null; return; }
    if [ -n "$_old_cf" ]; then export CFLAGS="$_old_cf"; else unset CFLAGS; fi
    local so
    for so in "$d/$bdir/${name}_libretro.so" "$d/$bdir/$(basename "$bdir")_libretro.so"; do
        [ -f "$so" ] && cp "$so" "$CORE_OUT/" && log "  -> $CORE_OUT/$(basename "$so")" && popd >/dev/null && return
    done
    so=$(find "$d/$bdir" -maxdepth 1 -name "*_libretro.so" 2>/dev/null | head -1)
    [ -n "$so" ] && cp "$so" "$CORE_OUT/" && log "  -> $CORE_OUT/$(basename "$so")"
    popd >/dev/null
}
while IFS='|' read -r name repo bdir mk extra wrap branch; do
    [ -z "$name" ] && continue
    case " $CORES " in *" $name "*) log "Building libretro core: $name"; build_core "$name" "$repo" "$bdir" "$mk" "$extra" "$wrap" "$branch";; esac
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
cp -f FrogUI/frogui_libretro.so        "$DST/cores/" 2>/dev/null || true
cp -f "$CORE_OUT"/*.so                  "$DST/cores/" 2>/dev/null || true
cp -f "$HERE/cubegm/cores/config.xml"   "$DST/cores/" 2>/dev/null || true
cp -f "$HERE/cubegm/zhijack.sh"         "$DST/" 2>/dev/null || true
cp -f "$HERE/cubegm/autorun"            "$DST/" 2>/dev/null || true
chmod +x "$DST/picoarch" "$DST/zhijack.sh" "$DST/autorun" 2>/dev/null || true

# -----------------------------------------------------------------------------
# cubevol_bridge: emulate the stock cubevol input daemon (FrogUI reads
# /tmp/joy_key shared memory; the stock daemon is inside rkgame, so after the
# hijack there is NO input source). Read USB pads via evdev, write the mask.
if [ -f "$HERE/cubevol_bridge.c" ]; then
    log "Building cubevol_bridge (evdev -> cubevol shm)..."
    if ${CROSS_COMPILE}gcc -O2 -Wall -march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard --sysroot="$SYSROOT" -I"$SYSROOT/usr/include" "$HERE/cubevol_bridge.c" -o "$DST/cubevol_bridge"; then
        ${CROSS_COMPILE}strip "$DST/cubevol_bridge"
        log "cubevol_bridge built: $(ls -la "$DST/cubevol_bridge" 2>/dev/null | awk '{print $5}') bytes"
    else
        log "WARN: cubevol_bridge build failed -- FrogUI will have no input!"
    fi
fi

# STAGE 9b -- bundle runtime libs picoarch + frogui need into cubegm/lib
#   The device rootfs does NOT ship SDL/libpng12/z/asound (see zhijack.sh:
#   LD_LIBRARY_PATH=/mnt/sdcard/cubegm/lib). picoarch is linked against those,
#   so without them it dies at load time ("cannot open shared object file")
#   and the screen never lights. Copy every NEEDED .so (and transitive deps)
#   from the sysroot into $DST/lib. Base libs (libc/libm/pthread/dl/gcc/ld)
#   are provided by the device rootfs, so we exclude them to avoid shipping a
#   second glibc that could mismatch the device's dynamic linker.
# -----------------------------------------------------------------------------
log "Bundling runtime libs into $DST/lib ..."
mkdir -p "$DST/lib"
READELF="${CROSS_COMPILE}readelf"
# base libs the device rootfs always provides -- do NOT bundle these
BASE_LIBS="libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 libgcc_s.so.1 \
           librt.so.1 libutil.so.1 ld-linux-armhf.so.3 ld-2.17.so \
           libstdc++.so.6 libatomic.so.1"
is_base() { for b in $BASE_LIBS; do [ "$1" = "$b" ] && return 0; done; return 1; }
declare -A _seen=()
_queue=()
for _b in picoarch/picoarch FrogUI/frogui_libretro.so; do
    [ -f "$_b" ] || continue
    if command -v "$READELF" >/dev/null 2>&1; then
        while IFS= read -r _l; do [ -n "$_l" ] && _queue+=("$_l"); done \
            < <("$READELF" -d "$_b" 2>/dev/null | awk -F'[][]' '/NEEDED/ {gsub(/[ \t]/,"",$2); print $2}')
    fi
done
# fallback: if readelf was unavailable, seed the known direct deps
if [ ${#_queue[@]} -eq 0 ]; then
    _queue=(libSDL.so.1 libpng12.so.0 libz.so.1 libasound.so.2)
    log "WARN: readelf unavailable -- seeding hardcoded SDL/libpng/z/asound."
fi
while [ ${#_queue[@]} -gt 0 ]; do
    _lib="${_queue[0]}"; _queue=("${_queue[@]:1}")
    [ -n "${_seen[$_lib]:-}" ] && continue
    _seen[$_lib]=1
    is_base "$_lib" && continue
    _found=""
    for _d in "$SYSROOT/lib" "$SYSROOT/usr/lib" "$SYSROOT/usr/lib/arm-linux-gnueabihf" "$SYSROOT/lib/arm-linux-gnueabihf"; do
        if [ -e "$_d/$_lib" ]; then _found="$_d/$_lib"; break; fi
    done
    if [ -z "$_found" ]; then
        log "WARN: runtime lib $_lib not in sysroot -- device must provide it."
        continue
    fi
    cp -L "$_found" "$DST/lib/" 2>/dev/null || log "WARN: copy failed for $_lib"
    if command -v "$READELF" >/dev/null 2>&1; then
        while IFS= read -r _dep; do [ -n "$_dep" ] && _queue+=("$_dep"); done \
            < <("$READELF" -d "$_found" 2>/dev/null | awk -F'[][]' '/NEEDED/ {gsub(/[ \t]/,"",$2); print $2}')
    fi
done
log "Bundled $(ls -1 "$DST/lib" 2>/dev/null | wc -l) runtime libs into $DST/lib."

# -----------------------------------------------------------------------------
# -----------------------------------------------------------------------------
# STAGE 9c -- ship stock-core alias script (device-side)
#   The device ALREADY carries the stock cores at cubegm/cores/libemu_*.so
#   (they are the original firmware files; user requirement: reuse them).
#   FrogUI resolves folder->core via frogui_libretro.c console_mappings which
#   hardcodes *_libretro.so names. This script aliases (hardlinks) each stock
#   libemu_*.so to its FrogUI table name ON DEVICE at first boot, so the
#   menu can launch them without recompiling anything.
# -----------------------------------------------------------------------------
if [ -d "$HERE/../cubegm_stock" ]; then
    # CI/local runs that DO have the stock tree: copy real binaries (e.g. dev builds)
    STK="$HERE/../cubegm_stock"
    log "cubegm_stock present -- copying stock binaries directly."
    for src in "$STK"/cores/libemu_*.so; do
        [ -e "$src" ] || continue
        base=$(basename "$src")
        case "$base" in
            libemu_nes.so)        dst=fceumm_libretro.so ;;
            libemu_nestopia.so)   dst=nestopia_libretro.so ;;
            libemu_sfc.so)        dst=snes9x2005_plus_libretro.so ;;
            libemu_snes9x.so)     dst=snes9x_libretro.so ;;
            libemu_snes9x2010.so) dst=snes9x2010_libretro.so ;;
            libemu_md.so)         dst=picodrive_libretro.so ;;
            libemu_mgba.so)       dst=mgba_libretro.so ;;
            libemu_vbam.so)       dst=vba_next_libretro.so ;;
            libemu_gpsp.so)       dst=gpsp_libretro.so ;;
            libemu_tgbdual.so)    dst=tgbdual_libretro.so ;;
            libemu_stella.so)     dst=stella2014_libretro.so ;;
            libemu_prosystem.so)  dst=prosystem_libretro.so ;;
            libemu_mame2000.so)   dst=mame2000_libretro.so ;;
            libemu_fbalpha2012.so)dst=fbalpha2012_libretro.so ;;
            libemu_fbalpha.so)    dst=fbalpha_libretro.so ;;
            libemu_fba.so)        dst=fba_libretro.so ;;
            libemu_cps2.so)       dst=cps2_libretro.so ;;
            libemu_pgm.so)        dst=pgm_libretro.so ;;
            libemu_extend.so)     dst=extend_libretro.so ;;
            libemu_pcsx.so)       dst=pcsx_rearmed_libretro.so ;;
            *) continue ;;
        esac
        cp -f "$src" "$DST/cores/$dst" && log "  stock $base -> $dst"
    done
    [ -d "$STK/cores/bios" ]     && cp -rf "$STK/cores/bios"     "$DST/cores/" 2>/dev/null || true
    [ -d "$STK/cores/mame2000" ] && cp -rf "$STK/cores/mame2000" "$DST/cores/" 2>/dev/null || true
    [ -d "$STK/lib" ]            && cp -rf "$STK/lib"            "$DST/lib/"    2>/dev/null || true
    log "Stock binaries copied (bios+mame2000+lib included)."
else
    # Device-side: ship an alias script; device stock cores stay untouched.
    log "cubegm_stock absent -- shipping device-side alias script."
    cat > "$DST/alias_stock_cores.sh" << 'ALIAS_EOF'
#!/bin/sh
# CubeGM stock-core aliaser: hardlink libemu_*.so -> FrogUI table names.
# Safe to run repeatedly (idempotent). Does NOT modify/remove any stock file.
CD="$(dirname "$0")/cores"
link() { [ -f "$CD/$1" ] && [ ! -e "$CD/$2" ] && ln "$CD/$1" "$CD/$2" 2>/dev/null || cp "$CD/$1" "$CD/$2" 2>/dev/null; }
link libemu_nes.so fceumm_libretro.so
link libemu_nestopia.so nestopia_libretro.so
link libemu_sfc.so snes9x2005_plus_libretro.so
link libemu_snes9x.so snes9x_libretro.so
link libemu_snes9x2010.so snes9x2010_libretro.so
link libemu_md.so picodrive_libretro.so
link libemu_mgba.so mgba_libretro.so
link libemu_vbam.so vba_next_libretro.so
link libemu_gpsp.so gpsp_libretro.so
link libemu_tgbdual.so tgbdual_libretro.so
link libemu_stella.so stella2014_libretro.so
link libemu_prosystem.so prosystem_libretro.so
link libemu_mame2000.so mame2000_libretro.so
link libemu_fbalpha2012.so fbalpha2012_libretro.so
link libemu_fbalpha.so fbalpha_libretro.so
link libemu_fba.so fba_libretro.so
link libemu_cps2.so cps2_libretro.so
link libemu_pgm.so pgm_libretro.so
link libemu_extend.so extend_libretro.so
link libemu_pcsx.so pcsx_rearmed_libretro.so
ALIAS_EOF
    chmod +x "$DST/alias_stock_cores.sh"
    log "alias_stock_cores.sh staged (run once on device to link stock cores)."
fi

log "Staged into $DST"
log "DONE. Copy the whole '$DST' directory to the root of your device SD card,"
log "overwriting the existing cubegm/ (stock rkgame/icube/driver.so/root.dat stay)."
log "On next boot the device launches picoarch + FrogUI directly."
