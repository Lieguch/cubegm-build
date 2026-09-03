#!/usr/bin/env bash
# =============================================================================
#  CubeGM open-source replacement -- one-command builder (Linux x86_64 host)
# =============================================================================
#  Produces deploy/cubegm/ with compiled binaries that you copy straight to the
#  device SD card. The device boots the open-source menu (picoarch + FrogUI)
#  via the autorun hijack; stock rkgame/icube/driver.so and root.dat are left
#  untouched, so the device will NOT report "sdcard is damaged".
#
#  All produced binaries are linked against glibc <= 2.29 (the device ceiling,
#  measured on the 20 device cores) and verified by verify_target_abi.sh.
#
#  USAGE
#    ./build.sh                 # full build (auto-builds glibc-2.29 sysroot)
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
RETROARCH_DST="$HERE/cubegm"

# Stage-1 default cores (proves the loop across GBA/SNES/NES/MD/Atari).
# Each is built from github.com/libretro/<NAME> with the libretro common Makefile.
# Game-able core set: FBA2012 (000 arcade group), FBNeo, MD, PS1 (lightrec),
# GBA/NES/SNES/Atari/GBC etc. fbalpha2012_* build fast; pcsx/mgba slower.
DEFAULT_CORES="fceumm snes9x2005_plus snes9x2002 snes9x2010 picodrive stella2014 mgba nestopia vba_next tgbdual gpsp prosystem mame2000 mame2003_plus fbalpha2012 fbalpha2012_cps1 fbalpha2012_cps2 fbalpha2012_cps3 fbalpha2012_neogeo fbneo pcsx_rearmed gambatte 81 a5200 ardens arduous atari800 bluemsx cannonball cap32 castaway ecwolf fake08 freechaf freeintv frodo fuse gearboy gearcoleco gearsystem genesis_plus_gx geolith gme gong gpsp_multicore gw handy jumpnbump lowresnx mednafen_lynx mednafen_pce_fast mednafen_pcfx mednafen_supergrafx mednafen_vb mednafen_wswan nxengine o2em pocketcdg pokemini potator prboom quasi88 quicknes race reminiscence retro8 snes9x2005 theodore tic80 tyrquake uae vecx vice_x64 vice_xvic vitaquake2 x68k xrick"
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
# STAGE 2 -- glibc-2.29 sysroot (the device ceiling)
# -----------------------------------------------------------------------------
if [ -z "$SYSROOT" ]; then
    if [ "$SKIP_SYSROOT" = "1" ]; then
        die "SKIP_SYSROOT=1 but no SYSROOT provided."
    fi
    SYSROOT="$WORKDIR/sysroot-glibc229"
    if [ -d "$SYSROOT/usr/lib" ]; then
        log "Reusing existing sysroot: $SYSROOT"
    else
        log "Building glibc-2.29 sysroot with crosstool-NG (one-time, ~20-60 min)..."
        bash "$HERE/toolchain/build_sysroot_ctng.sh" "$SYSROOT"
    fi
fi
[ -d "$SYSROOT" ] || die "SYSROOT not found: $SYSROOT"
log "Sysroot: $SYSROOT"

# -----------------------------------------------------------------------------
# STAGE 3 -- ALSA userspace headers (not in the ARM toolchain sysroot)
# -----------------------------------------------------------------------------
# Use the HOST libasound2-dev package headers (arch-independent, include the
# configure-generated asoundlib.h). This is the same approach as libdrm-dev:
# headers are plain text and identical across architectures, only .so/.a are
# arch-specific (the device ships libasound.so.2 at runtime).
# NOTE: cloning alsa-lib git is NOT sufficient — its include/ is flat
# (include/pcm.h, not include/alsa/pcm.h) and asoundlib.h is generated by
# configure, so a bare clone has no usable <alsa/asoundlib.h>.
ALSA_HEADER_DIR="/usr/include/alsa"
if [ ! -f "$ALSA_HEADER_DIR/asoundlib.h" ]; then
    log "Installing libasound2-dev (host headers, arch-independent)..."
    sudo apt-get install -y libasound2-dev 2>/dev/null || \
        die "libasound2-dev not available -- ALSA headers cannot be installed."
fi
[ -f "$ALSA_HEADER_DIR/asoundlib.h" ] || die "ALSA headers missing at $ALSA_HEADER_DIR"
mkdir -p "$SYSROOT/usr/include"
rm -rf "$SYSROOT/usr/include/alsa"
cp -a "$ALSA_HEADER_DIR" "$SYSROOT/usr/include/"
log "ALSA headers installed -> $SYSROOT/usr/include/alsa ($(ls "$SYSROOT/usr/include/alsa" | wc -l) files)"
ALSA_CFLAGS="-I$SYSROOT/usr/include"

# -----------------------------------------------------------------------------
# STAGE 4 -- libdrm headers (for RetroArch plain_drm driver)
# The device ships libdrm.so.2 (driver.so NEEDED), but the crosstool sysroot
# lacks the development headers. The host-installed libdrm-dev package (from
# STAGE 0 apt) provides architecture-independent headers that work for
# cross-compilation. Install them at the sysroot standard location so the
# cross-compiler's --sysroot lookup finds them.
# -----------------------------------------------------------------------------
DRM_HEADER_DIR="/usr/include/libdrm"
if [ ! -f "$DRM_HEADER_DIR/xf86drm.h" ]; then
    log "Installing libdrm-dev (host headers, arch-independent)..."
    sudo apt-get install -y libdrm-dev 2>/dev/null || \
        die "libdrm-dev not available -- RetroArch plain_drm cannot compile."
fi
[ -d "$DRM_HEADER_DIR" ] || die "libdrm headers missing at $DRM_HEADER_DIR"
# FORCE refresh: CI may cache sysroot with stale headers. Delete and reinstall.
rm -rf "$SYSROOT/usr/include/libdrm"
mkdir -p "$SYSROOT/usr/include/libdrm"
cp -f "$DRM_HEADER_DIR"/*.h "$SYSROOT/usr/include/libdrm/" 2>/dev/null
# ALSO copy to the sysroot include ROOT: RetroArch's gfx/drivers/drm_gfx.c does
# `#include <xf86drm.h>` (angle brackets -> default include search). With
# --sysroot the default path is $SYSROOT/usr/include/, so xf86drm.h must be
# directly visible there (Debian provides it as libdrm/xf86drm.h via pkg-config,
# but our toolchain has no pkg-config and RetroArch's configure does not relay
# -I$SYSROOT/usr/include/libdrm into drm_gfx.c's include resolution).
rm -f "$SYSROOT/usr/include/xf86drm.h"
cp -f "$DRM_HEADER_DIR"/*.h "$SYSROOT/usr/include/" 2>/dev/null
log "libdrm headers installed -> $SYSROOT/usr/include/(libdrm + root) ($(ls "$SYSROOT/usr/include/libdrm" | wc -l) files)"

# Common compile flags for every target binary
export CFLAGS="$ARCH_FLAGS --sysroot=$SYSROOT $ALSA_CFLAGS -I$SYSROOT/usr/include -I$SYSROOT/usr/include/libdrm"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="--sysroot=$SYSROOT -Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3"

# -----------------------------------------------------------------------------
# STAGE 4.7 -- cross-build libudev-zero into sysroot (RetroArch udev input driver)
# -----------------------------------------------------------------------------
# v10.4: udev 驱动引入。libudev = RetroArch udev_input/udev_joypad 的枚举+hotplug
# 客户端库。libudev-zero 是 daemonless 替代（Alpine 官方打包），无 udevd 也能
# 枚举 /sys + 自算 ID_INPUT_* 属性。选型与 ABI 依据见 build_libudev_zero.sh 头注。
# 幂等：sysroot 已含 libudev.so.1 则跳过（含在 CI sysroot 缓存里）。
bash "$HERE/build_libudev_zero.sh" || die "libudev-zero sysroot install FAILED"

# -----------------------------------------------------------------------------
# STAGE 4 -- clone front-end sources
#   IMPORTANT: the 5-edit patch (../patch/picoarch_5edits.patch) only applies on
#   the *r36sx* branch and needs the libretro-common submodule (defines
#   RETRO_DEVICE_ID_JOYPAD_* used by plat_sf3000.c). Clone r36sx explicitly and
#   init the submodule. To reuse an already-patched local checkout, set
#   PICOARCH_LOCAL=/path/to/TreeFrogUI_picoarch.
#
#   v8.6.2: PIN the upstream commit. The upstream r36sx branch moves (HEAD
#   advanced f8ff5ba -> aa591ed on 2026-08-21) and every patch in patch/ is
#   context-based; a fresh clone then fails "display patch NOT applicable"
#   (run 277). All patches are authored against f8ff5ba, so checkout that
#   commit deterministically.
# -----------------------------------------------------------------------------
PICOARCH_PIN="f8ff5ba"
if [ -n "${PICOARCH_LOCAL:-}" ] && [ -d "$PICOARCH_LOCAL" ]; then
    log "Using local picoarch source: $PICOARCH_LOCAL"
    rm -rf picoarch && cp -a "$PICOARCH_LOCAL" picoarch
elif [ ! -d picoarch ]; then
    log "Cloning picoarch (r36sx @ $PICOARCH_PIN)..."
    git clone -q "$PICOARCH_REPO" picoarch
    git -C picoarch checkout -q "$PICOARCH_PIN" || {
        # pin unknown (upstream history rewritten): fall back to branch tip
        log "WARN: pin $PICOARCH_PIN not found -- using r36sx tip (patches may not apply!)"
        git -C picoarch checkout -q r36sx
    }
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
# v8.7: apply ONE full patch (authored against the pinned commit f8ff5ba).
# It contains the ENTIRE RK3036G port delta: 5-edit (RTC+evdev), DRM display
# path + RGB565 dumb buffer + aspect-fit + portrait rotation + hwdisp_restore,
# crash backtrace logging, gamepad keycode/hat input, sf3000 shm game-input
# injection, ALSA pre-fork release + global-config reset, CLOEXEC on DRM/fb0
# fds (fixes the "half-white screen" fd-inheritance bug), portrait pixel-map
# tables. A single patch kills the old 5-patch stacking risk (run 277:
# "display patch NOT applicable" when upstream moved).
PICO_PATCH="$HERE/../patch/picoarch_rk3036g_full.patch"
if [ -f "$PICO_PATCH" ]; then
    if git -C picoarch apply --ignore-whitespace --check "$PICO_PATCH" 2>/dev/null; then
        log "Applying picoarch full RK3036G patch (v8.7)..."
        git -C picoarch apply --ignore-whitespace "$PICO_PATCH"
    elif git -C picoarch apply --ignore-whitespace -R --check "$PICO_PATCH" 2>/dev/null; then
        log "picoarch full patch already applied -- skipping."
    else
        die "picoarch full RK3036G patch NOT applicable and NOT applied -- refusing to ship a broken build. Abort."
    fi
else
    die "picoarch_rk3036g_full.patch missing -- cannot build."
fi
# Direction-A platform layer (plat_rk3036g.c + Makefile rk3036g branch).
# NOTE (2026-08-24): DISABLED. The platform patch's audio hunks delete the
# sf3000_is_rk3036() runtime detection from plat_sound_init/plat_sound_finish
# and replace it with a #ifdef PLATFORM_RK3036G compile-time branch. Since we
# build with platform=sf3000 (see STAGE 5 below), that compile-time branch is
# DEAD code -> plat_sound_init_alsa is never called -> rk3036 gets routed to
# the SF2000 proprietary driver.so audio path -> silent device (#324 real-device
# log: "Proprietary audio driver initialized" + "snd_pcm_start failed: -32").
# The full patch already carries the correct sf3000_is_rk3036() runtime routing
# (DRM/ALSA/evdev), verified on payload-307. Direction-A (platform=rk3036g) is
# deferred until its display+audio layers are fully extracted from the SF3000
# block; until then the platform patch must NOT be applied to the sf3000 build.
PICO_A1_PATCH="$HERE/../patch/picoarch_rk3036g_platform.patch"
# Direction-A (platform=rk3036g) is deferred: its display+audio layers are not
# yet extracted from the SF3000 block, and its audio hunks are HARMFUL to the
# sf3000 build (delete sf3000_is_rk3036 runtime routing -> silent device).
# So we unconditionally SKIP the platform patch for now. When Direction-A is
# resumed, re-enable the git apply block below (gated on a build-platform flag).
log "Skipping platform patch (A1): sf3000 build uses full-patch rk3036 runtime routing."
: # (platform patch application intentionally disabled — see note above)
# DRM UAPI headers for the RK3036G HDMI modeset path (hwdisp_drm #includes
# <drm/drm.h>). The crosstool glibc-2.29 sysroot ships no DRM headers, so
# bundle them (Linux v4.4 UAPI, MIT) and drop them into the source.
mkdir -p picoarch/include/drm
cp -f "$HERE/drm_headers/drm/"*.h picoarch/include/drm/ 2>/dev/null || log "WARN: drm headers not copied (hwdisp_drm won't compile!)"
# v8.6.2: pin FrogUI too (upstream tip moves and frogui patches are
# context-based; all are authored against 2f41ace).
if [ -d FrogUI ]; then
    log "Reusing existing FrogUI/."
else
    log "Cloning FrogUI @ 2f41ace..."
    git clone -q "$FROGUI_REPO" FrogUI
    git -C FrogUI checkout -q 2f41ace || log "WARN: FrogUI pin 2f41ace not found -- using tip (patches may not apply!)"
fi

# -----------------------------------------------------------------------------
# STAGE 5 -- SKIPPED (v9.0: RetroArch replaces picoarch)
# -----------------------------------------------------------------------------
log "STAGE 5 skipped: picoarch deprecated (RetroArch replaces it)"

# -----------------------------------------------------------------------------
# STAGE 5b -- build RetroArch (替代 picoarch + frogui/stockui 全部自定义 UI)
# -----------------------------------------------------------------------------
# v9.0 根源方案：用 RetroArch 成熟 15 年的 libretro 前端替代所有自定义 UI 代码。
# 显示：kms_drm（DRM/KMS 直出，与 hwdisp 同原理但更稳定）
# 音频：alsa（设备已有 libasound.so.2）
# 输入：udev（libudev-zero 客户端库，EVIOCGID/EVIOCGNAME 直读，四要素 autoconfig 自动匹配）
# 菜单：rgui（轻量文字菜单，244MB 内存足够）
# 核心：复用现有 libretro 57 核（同一份 .so 文件）
RETROARCH_REPO="https://github.com/libretro/RetroArch.git"
log "Building RetroArch (standalone libretro frontend)..."
# 将 RetroArch 放入 WORKDIR 以便缓存复用（避免每次 CI 重新克隆 ~100MB）
mkdir -p "$WORKDIR"
if [ -d "$WORKDIR/RetroArch" ]; then
    log "Reusing cached RetroArch/ from $WORKDIR"
    ln -sf "$WORKDIR/RetroArch" RetroArch || cp -r "$WORKDIR/RetroArch" RetroArch
elif [ -d RetroArch ]; then
    log "Reusing existing RetroArch/ (moved to cache)"
    rm -rf "$WORKDIR/RetroArch"
    mv RetroArch "$WORKDIR/RetroArch"
    ln -sf "$WORKDIR/RetroArch" RetroArch
else
    log "Cloning RetroArch (shallow, to save time; submodules init later)..."
    git clone "$RETROARCH_REPO" "$WORKDIR/RetroArch" || \
        die "RetroArch clone failed."
    # v0.7 (2026-08-31): 锁定 RetroArch 到 282a12d —— 402 实测可用稳定版
    # 根因: 402 用 282a12d 正常, 406 漂移到 285d685 (63 commits 含
    # audio_driver.c 重写 +86/-48) 导致无法启动。必须固定 commit。
    cd "$WORKDIR/RetroArch" && git checkout 282a12d || \
        die "RetroArch checkout 282a12d failed."
    git submodule update --init --recursive 2>&1 || \
        die "RetroArch submodule init failed."
    cd "$HERE"
    ln -sf "$WORKDIR/RetroArch" RetroArch || cp -r "$WORKDIR/RetroArch" RetroArch
fi
if [ -d RetroArch ] && [ -f RetroArch/configure ]; then
    cd RetroArch
    # v0.7 (2026-08-31): 缓存复用路径也强制锁定 282a12d（幂等）
    git checkout 282a12d 2>/dev/null || die "RetroArch checkout 282a12d failed (cached)"
    # v0.10 (2026-09-01): 音频多变体驱动 —— 14 个可切 ident 一次刷机 A/B
    # - alsa 家族: alsa/alsathread(S16 官方) + alsa-s24/alsa-s32/alsathread-s24/alsathread-s32
    # - tinyalsa 家族: tinyalsa(S16 官方基石) + s24_3le/s24/s32/s16-p256/s16-p512
    # - common: alsa_init_pcm_fmt(requested_format) 供 alsa 变体指定位深
    # apply 顺序: 01(common) → 02(alsa新文件) → 03(tinyalsa新文件) → 04(注册+Makefile) → 05(tinyalsa pre-negotiate VWL)
    for _ap in \
        "$HERE/patches/audio-variants/01-common-alsa-fmt.patch" \
        "$HERE/patches/audio-variants/02-alsa-variants.patch" \
        "$HERE/patches/audio-variants/03-tinyalsa-variants.patch" \
        "$HERE/patches/audio-variants/04-register-and-build.patch" \
        "$HERE/patches/audio-variants/05-tinyalsa-prenegotiate.patch"; do
        if [ -f "$_ap" ]; then
            if ! git apply "$_ap"; then
                if ! git apply --reverse --check "$_ap" 2>/dev/null; then
                    die "audio-variant patch apply FAILED: $_ap"
                fi
                log "audio-variant patch already applied (skip): $_ap"
            else
                log "audio-variant patch applied: $_ap"
            fi
        else
            die "audio-variant patch missing: $_ap"
        fi
    done
    # ---- 显示/音频策略（2026-08-25 定案，基于权威源码验证）----
    # 设备 = RK3036G 无 GPU framebuffer。项目已在 build_sdl_libpng.sh 将
    # SDL 1.2.15 (fbcon 视频 + ALSA 音频) 交叉编译进 sysroot，且 picoarch
    # 已在设备上用它跑通。RetroArch 的 HAVE_SDL 检测（qb/qb.libs.sh）在
    # pkg-config 缺失时回退到 check_lib 直接链接 -lSDL 测试，sysroot 里
    # 的 libSDL.so 可满足 -> 用 SDL 驱动，零新增依赖。
    # 相反，plain_drm 需要 libdrm 头文件（xf86drm.h），无 pkg-config 时
    # RetroArch 的 -I 传递不可靠，已在 20+ 次 CI 里反复验证为死路。
    # 输入：udev 驱动（libudev-zero 无 udevd 也能枚举/热插拔，见 STAGE 4.7）。
    # 禁用 GL/EGL/Vulkan/X11/Wayland：设备无 GPU/无 X server。禁用 SDL2/SDL3
    # 强制使用 SDL1.2（与 sysroot 已有的 libSDL.so 一致）。
    # RetroArch configure 用环境变量传交叉编译器，不是命令行 CC=
    export CC="${CROSS_COMPILE}gcc"
    export CXX="${CROSS_COMPILE}g++"
    export AR="${CROSS_COMPILE}ar"
    export RANLIB="${CROSS_COMPILE}ranlib"
    export LD="${CROSS_COMPILE}ld"
    export STRIP="${CROSS_COMPILE}strip"
    # SDL.h 在 $SYSROOT/usr/include/SDL/（SDL1.2 布局），alsa/asoundlib.h 在
    # $SYSROOT/usr/include/alsa/。configure 的 SDL/ALSA 链接测试需要
    # --sysroot 才能解析 -lSDL/-lasound；include 路径通过 CPPFLAGS 传入，
    # 同时作为 Makefile DEF_FLAGS 的底（见下方补丁）。
    # 修补 qb/config.libs.sh：check_lib 的 include 目录扫描用硬编码的
    # INCLUDES='usr/include usr/local/include'，不会查 $SYSROOT。
    # 追加 sysroot 路径使 SDL.h 存在性检查通过。
    # 注意：ALSA 不需要此修补，因为 runner 宿主机装了 libasound2-dev。
    sed -i "s|^INCLUDES='usr/include usr/local/include'|INCLUDES='usr/include usr/local/include $SYSROOT/usr/include $SYSROOT/usr/include/SDL'|" qb/config.libs.sh
    export INCLUDE_DIRS="-I$SYSROOT/usr/include/SDL -I$SYSROOT/usr/include/alsa -I$SYSROOT/usr/include"
    ./configure --host=arm-linux-gnueabihf \
        --enable-sdl --disable-sdl2 --disable-sdl3 \
        --enable-alsa \
        --enable-udev \
        --disable-plain_drm --disable-kms --disable-egl \
        --disable-opengl --disable-opengl1 \
        --disable-opengl_core --disable-opengles --disable-opengles3 \
        --disable-vulkan --disable-x11 --disable-wayland \
        --disable-ffmpeg --disable-networking --disable-cheevos \
        --disable-discord --disable-7zip --disable-freetype \
        --disable-rpng --disable-flac --disable-jack --disable-pulse \
        --disable-ssl \
        --disable-builtinmbedtls \
        --disable-videoprocessor --disable-qt --disable-cg \
        --enable-neon --disable-libretro \
        --disable-mali_fbdev \
        --enable-langextra \
        --prefix="$RETROARCH_DST" 2>&1 || \
        die "RetroArch configure failed."
    # 无 pkg-config 时 configure 的 SDL/ALSA include 扫描会误指向宿主
    # /usr/include。用 DEF_FLAGS 追加 sysroot 真实路径，编译时优先解析。
    echo "" >> Makefile
    echo "# Added by build.sh: sysroot include paths (no pkg-config present)" >> Makefile
    echo "DEF_FLAGS += -I$SYSROOT/usr/include/SDL -I$SYSROOT/usr/include/alsa -I$SYSROOT/usr/include" >> Makefile
    # libretro-common 子模块头文件（boolean.h/compat/strl.h/rthreads 等）不在
    # --sysroot 可见范围，make 阶段通过 CPPFLAGS 显式传入。
    # CFLAGS 由 Makefile 内部管理（?= 默认 + += DEF_FLAGS），外部传参会覆盖
    # -I./ 解析导致 RARCH_PATH_* 未定义——所以只传 CPPFLAGS，不传 CFLAGS。
    LIBRETRO_COMMON_INC="-I${WORKDIR}/RetroArch/libretro-common/include"
    RETROARCH_ROOT_INC="-I${WORKDIR}/RetroArch -I${WORKDIR}/RetroArch/libretro-common -I${WORKDIR}/RetroArch/libretro-common/compat"
    DEPS_INC="-I${WORKDIR}/RetroArch/deps -I${WORKDIR}/RetroArch/deps/zstd/lib"
    make -j"$(nproc)" CPPFLAGS="$CFLAGS $LIBRETRO_COMMON_INC $RETROARCH_ROOT_INC $DEPS_INC" \
        Q= 2>&1 || \
        die "RetroArch make failed."
    # Strip Q= on next run to suppress verbose output
    ${CROSS_COMPILE}strip retroarch
    log "RetroArch built: $(ls -la retroarch 2>/dev/null | awk '{print $5}') bytes"
        # v4.0 构建侧验证 1/3: 主二进制必须动态依赖 libasound.so.2（dlopen 的前提）
        if readelf -d retroarch | grep -q 'libasound.so.2'; then
            log "v4.0 verify 1/3: retroarch NEEDED libasound.so.2 = OK"
        else
            die "v4.0 verify 1/3 FAILED: retroarch does not link libasound.so.2"
        fi
        # v5.0 构建侧验证 2/3: libasound 链路代码已编入（strings 证据）
        if strings retroarch | grep -q 'libasound chain active'; then
            log "v5.0 verify 2/3: libasound chain code present = OK"
        else
            die "v5.0 verify 2/3 FAILED: libasound chain code missing from binary"
        fi
        cp retroarch "$RETROARCH_DST/"
        cd "$HERE"
else
    log "WARN: RetroArch directory missing -- using picoarch+frogui fallback."
fi

# -----------------------------------------------------------------------------
# STAGE 6 -- FrogUI launcher core (DEPRECATED by RetroArch v9.0)
# -----------------------------------------------------------------------------
# v9.0: RetroArch 自带 rgui 菜单 + 内置 libretro 核心加载，替代整个
# picoarch+FrogUI 栈。FrogUI 构建不再需要。
log "STAGE 6 skipped: FrogUI deprecated (RetroArch rgui replaces it)"

# -----------------------------------------------------------------------------
# STAGE 6.5 -- download prebuilt cores from libretro buildbot
#   Saves ~40 min CI time per run. Buildbot provides armv7-neon-hf (ARMv7+NEON+HF)
#   and armhf (fallback, more cores) prebuilt binaries. All tested cores have
#   GLIBC <= 2.15 and GLIBCXX <= 3.4.21, compatible with the device glibc 2.29.
#   Cores not in buildbot fall through to STAGE 7 (source compilation).
# -----------------------------------------------------------------------------
log "Downloading prebuilt cores from libretro buildbot..."
CORE_OUT_STAGE65="$WORKDIR/cores"
mkdir -p "$CORE_OUT_STAGE65"
bash "$HERE/download_prebuilt_cores.sh" "$CORE_OUT_STAGE65" $CORES || true
log "Prebuilt cores in $CORE_OUT_STAGE65: $(ls "$CORE_OUT_STAGE65"/*.so 2>/dev/null | wc -l) .so files"

# -----------------------------------------------------------------------------
# STAGE 7 -- build libretro cores (table-driven; builddir+mk from treefrog-ui build_all.sh)
#   Cores already downloaded from buildbot (STAGE 6.5) are skipped by build_core().
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
nestopia|https://github.com/libretro/nestopia|libretro|||arm
81|https://github.com/libretro/81-libretro|.|-||arm|
a5200|https://github.com/libretro/a5200|.|-||arm|
ardens|https://github.com/tiberiusbrown/Ardens|.|-||arm|
arduous|https://github.com/libretro/arduous|.|-||arm|
atari800|https://github.com/libretro/libretro-atari800|.|-||arm|
bluemsx|https://github.com/tzubertowski/libretro-blueMSX|.|-||arm|
cannonball|https://github.com/libretro/cannonball|.|-||arm|
cap32|https://github.com/libretro/libretro-cap32|.|-||arm|
castaway|https://github.com/angree/sf2000-atarist-emulator|.|-||arm|
ecwolf|https://github.com/libretro/ecwolf|.|-||arm|
fake08|https://github.com/tzubertowski/fake-08|.|-||arm|sf3000
freechaf|https://github.com/libretro/FreeChaF|.|-||arm|
freeintv|https://github.com/libretro/FreeIntv|.|-||arm|
frodo|https://github.com/tzubertowski/libretro-frodo|.|-||arm|
fuse|https://github.com/libretro/fuse-libretro|.|-||arm|
gearboy|https://github.com/drhelius/Gearboy|.|-||arm|
gearcoleco|https://github.com/drhelius/Gearcoleco|.|-||arm|
gearsystem|https://github.com/drhelius/Gearsystem|.|-||arm|
genesis_plus_gx|https://github.com/libretro/Genesis-Plus-GX|.|-||arm|
geolith|https://github.com/libretro/geolith-libretro|.|-||arm|
gme|https://github.com/libretro/libretro-gme|.|-||arm|
gong|https://github.com/libretro/gong|.|-||arm|
gpsp_multicore|https://github.com/tzubertowski/gpsp_multicore|.|-||arm|
gw|https://github.com/libretro/gw-libretro|.|-||arm|
handy|https://github.com/libretro/libretro-handy|.|-||arm|
jaxe|https://github.com/libretro/jaxe|.|-||arm|
jumpnbump|https://github.com/libretro/jumpnbump-libretro|.|-|LDFLAGS=__LDFLAGS_S__|arm|
lowresnx|https://github.com/timoinutilis/lowres-nx|platform/LibRetro|-||arm|
mednafen_lynx|https://github.com/libretro/beetle-lynx-libretro|.|-||arm|
mednafen_pce_fast|https://github.com/libretro/beetle-pce-fast-libretro|.|-||arm|
mednafen_pcfx|https://github.com/libretro/beetle-pcfx-libretro|.|-||arm|
mednafen_supergrafx|https://github.com/libretro/beetle-supergrafx-libretro|.|-||arm|
mednafen_vb|https://github.com/libretro/beetle-vb-libretro|.|-||arm|
mednafen_wswan|https://github.com/libretro/beetle-wswan-libretro|.|-||arm|
nxengine|https://github.com/libretro/nxengine-libretro|.|-||arm|
o2em|https://github.com/libretro/libretro-o2em|.|-||arm|
pocketcdg|https://github.com/libretro/libretro-pocketcdg|.|-||arm|
pokemini|https://github.com/libretro/PokeMini|.|-||arm|
potator|https://github.com/libretro/potator|.|-||arm|
prboom|https://github.com/libretro/libretro-prboom|.|-||arm|
quasi88|https://github.com/libretro/quasi88-libretro|.|-||arm|
quicknes|https://github.com/libretro/QuickNES_Core|.|-||arm|
race|https://github.com/libretro/RACE|.|-||arm|
reminiscence|https://github.com/libretro/REminiscence|.|-||arm|
retro8|https://github.com/libretro/retro8|.|-||arm|
theodore|https://github.com/Zlika/theodore|.|-||arm|
tic80|https://github.com/nesbox/TIC-80|.|-||arm|
tyrquake|https://github.com/libretro/tyrquake|.|-||arm|
uae|https://github.com/angree/sf2000-uae-amiga-emulator|.|-||arm|
vaporspec|https://github.com/libretro/vaporspec|.|-||arm|
vecx|https://github.com/libretro/libretro-vecx|.|-||arm|
vice_x64|https://github.com/libretro/vice-libretro|.|-||arm|
vice_xvic|https://github.com/libretro/vice-libretro|.|-||arm|
vitaquake2|https://github.com/tzubertowski/treefrogui_vitaquake2|.|-||arm|
x68k|https://github.com/libretro/xmil-libretro|.|-||arm|
xrick|https://github.com/libretro/xrick-libretro|.|-||arm|
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
    # Skip if already downloaded from buildbot (STAGE 6.5)
    if [ -f "$CORE_OUT/${1}_libretro.so" ]; then
        log "  SKIP $1 (already downloaded from buildbot)"
        return
    fi
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
log "Running ABI verification gate (EM_ARM / 0x5000400 / glibc <= 2.29)..."
bash "$HERE/toolchain/verify_target_abi.sh" \
    RetroArch/retroarch \
    $(ls "$CORE_OUT"/*.so 2>/dev/null) \
    || die "ABI gate FAILED -- do not deploy. Fix sysroot/toolchain and rebuild."

# -----------------------------------------------------------------------------
# STAGE 9 -- stage into deploy/cubegm/
# -----------------------------------------------------------------------------
DST="$HERE/cubegm"
mkdir -p "$DST" "$DST/cores" "$DST/lib" "$DST/assets" "$DST/saves" "$DST/system" "$DST/autoconfig"
# v10.1: 部署中文字体（RGUI 渲染中文必须）
cp -f "$HERE/cubegm/font.ttf"              "$DST/" 2>/dev/null && log "  font.ttf deployed ($(ls -lh "$DST/font.ttf" 2>/dev/null | awk '{print $5}'))" || true
# v0.2 (中文根治): 部署 RGUI 官方位图字体（RetroArch 官方 retroarch-assets 仓库
#   rgui/font/ 下的 rzip 位图字体）。RGUI 按 user_language 动态加载对应字形：
#   bitmap10x10_chn.bin=简体/繁体中文(0x4E00-0x9FFF, 解压 272896B)、jpn/kor/rus。
#   此前 payload 缺失这些 .bin → 中文翻译(16676组)虽已编译进二进制，但
#   bitmapfont_10x10_load() 读文件失败返回 NULL → 中文无字形显示。配合
#   retroarch.cfg assets_directory=/mnt/sdcard/cubegm/assets 生效。
mkdir -p "$DST/assets/rgui/font"
if ls "$HERE/assets/rgui/font/"*.bin >/dev/null 2>&1; then
    cp -f "$HERE/assets/rgui/font/"*.bin "$DST/assets/rgui/font/" 2>/dev/null \
        && log "  RGUI bitmap fonts deployed ($(ls "$DST/assets/rgui/font/"*.bin 2>/dev/null | wc -l) .bin)"
else
    log "WARN: $HERE/assets/rgui/font/*.bin missing -- downloading from retroarch-assets..."
    mkdir -p "$HERE/assets/rgui/font"
    for _f in bitmap10x10_chn bitmap10x10_eng bitmap10x10_jpn bitmap10x10_kor bitmap10x10_rus bitmap6x10_eng bitmap6x10_lse; do
        curl -sL --max-time 60 "https://raw.githubusercontent.com/libretro/retroarch-assets/master/rgui/font/$_f.bin" \
            -o "$HERE/assets/rgui/font/$_f.bin" || log "WARN: download failed for $_f.bin"
    done
    cp -f "$HERE/assets/rgui/font/"*.bin "$DST/assets/rgui/font/" 2>/dev/null \
        && log "  RGUI bitmap fonts deployed ($(ls "$DST/assets/rgui/font/"*.bin 2>/dev/null | wc -l) .bin)"
fi
if [ -f RetroArch/retroarch ]; then
    cp -f RetroArch/retroarch        "$DST/"
else
    die "RetroArch binary MISSING from build tree -- v10 direction requires it. Check STAGE 5b configure/make output above."
fi
cp -f "$CORE_OUT"/*.so               "$DST/cores/" 2>/dev/null || true
# v9.6: 部署 autoconfig 配置文件（手柄自动识别）
# v10.3: 部署完整官方手柄特征库（按驱动子目录，RetroArch 按 driver+name+VID+PID 自动匹配）
# v10.6: 若 joypad-autoconfig 不存在（CI 干净 clone 场景），自动克隆。
if [ ! -d "$HERE/joypad-autoconfig" ] || [ -z "$(ls "$HERE/joypad-autoconfig/udev"/*.cfg 2>/dev/null)" ]; then
  log "Cloning retroarch-joypad-autoconfig (1099 profiles)..."
  if [ -n "${GITHUB_TOKEN:-}" ]; then
    git clone --depth 1 "https://x-access-token:${GITHUB_TOKEN}@github.com/libretro/retroarch-joypad-autoconfig.git" "$HERE/joypad-autoconfig"
  else
    git clone --depth 1 https://github.com/libretro/retroarch-joypad-autoconfig.git "$HERE/joypad-autoconfig"
  fi || die "Failed to clone joypad-autoconfig"
fi
for _drv in udev linuxraw sdl2 sdl3 android dinput xinput hid mfi parport qnx winraw x; do
  [ -d "$HERE/joypad-autoconfig/$_drv" ] && cp -rf "$HERE/joypad-autoconfig/$_drv" "$DST/autoconfig/" 2>/dev/null
done
# 保留本地 VID:PID 精确兜底 profile（平铺目录，RetroArch 兜底扫描）
cp -f "$HERE/autoconfig/"*.cfg        "$DST/autoconfig/" 2>/dev/null
log "  autoconfig db deployed ($(find "$DST/autoconfig" -name '*.cfg' 2>/dev/null | wc -l) profiles)"
# v8.7: alias snes9x2005_plus -> snes9x2005 (any old mapping / launch.txt that
# references the non-plus name still resolves to the OPTIMISED core; the stock
# snes9x2005 on the card was unoptimised -> ~1 FPS on 002 games).
if [ -f "$CORE_OUT/snes9x2005_plus_libretro.so" ]; then
    cp -f "$CORE_OUT/snes9x2005_plus_libretro.so" "$DST/cores/snes9x2005_libretro.so" \
        && log "  snes9x2005_libretro.so <- snes9x2005_plus (optimised alias)"
fi
cp -f "$HERE/cubegm/cores/config.xml"   "$DST/cores/" 2>/dev/null || true
cp -f "$HERE/cubegm/zhijack.sh"         "$DST/" 2>/dev/null || true
cp -f "$HERE/cubegm/autorun"            "$DST/" 2>/dev/null || true
cp -f "$HERE/retroarch.cfg"              "$DST/" 2>/dev/null || true
    # v11.6 音频根治：部署 ~/.asoundrc（官方 alsa.conf @hooks 自动加载的双输出定义）。
    cp -f "$HERE/cubegm/.asoundrc"           "$DST/" 2>/dev/null && log "  .asoundrc deployed"
chmod +x "$DST/retroarch" "$DST/zhijack.sh" "$DST/autorun" 2>/dev/null || true

# -----------------------------------------------------------------------------
# Direction B (charter §0.4): device-side diagnostics. diag.c answers the
# "what are the REAL keycodes/axes/DRM/ALSA/core facts" questions ON the device
# so fixes stop being guesses. Ships as cubegm/diag (runs: diag all | sysinfo
# | input | display | audio | cores). libc + dl only -- no SDL/libpng.
# DRM headers come from deploy/drm_headers/ (bundled Linux v4.4 UAPI).
# -----------------------------------------------------------------------------
if [ -f "$HERE/diag.c" ]; then
    log "Building diag (device diagnostics)..."
    if ${CROSS_COMPILE}gcc -O2 -Wall -march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 \
            -mfloat-abi=hard --sysroot="$SYSROOT" -I"$HERE/drm_headers" -I"$SYSROOT/usr/include" \
            "$HERE/diag.c" -o "$DST/diag" -ldl -static-libgcc; then
        ${CROSS_COMPILE}strip "$DST/diag"
        log "diag built: $(ls -la "$DST/diag" 2>/dev/null | awk '{print $5}') bytes"
    else
        die "diag build failed -- fix deploy/diag.c (diagnostics are the fact base for all fixes)."
    fi
fi


# -----------------------------------------------------------------------------
# icube_replacement: 替换原厂 icube 的启动器（S80icube 环节事前接管）。
#   supervisor 循环 exec: retroarch -c retroarch.cfg --menu（RetroArch 自带
#   RGUI 菜单 + SDL1.2 fbcon 显示 + ALSA 音频 + linuxraw 输入，自给自足）。
#   相比 zhijack 事后劫持，此方案在原厂 rkgame/driver.so 起来前就接管，
#   显示/音频由 RetroArch 自己初始化，根治半白屏/无声。
#   编译成 icube_replacement（不直接叫 icube，避免误覆盖原厂）。
if [ -f "$HERE/icube_replacement.c" ]; then
    log "Building icube_replacement (boot launcher)..."
    if ${CROSS_COMPILE}gcc -O2 -Wall -march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard --sysroot="$SYSROOT" -I"$SYSROOT/usr/include" "$HERE/icube_replacement.c" -o "$DST/icube_replacement"; then
        ${CROSS_COMPILE}strip "$DST/icube_replacement"
        log "icube_replacement built: $(ls -la "$DST/icube_replacement" 2>/dev/null | awk '{print $5}') bytes"
    else
        log "WARN: icube_replacement build failed -- will fall back to zhijack hijack."
    fi
fi

# STAGE 9b -- bundle runtime libs into cubegm/lib
#   The device rootfs does NOT ship SDL/libpng12/z (see zhijack.sh:
#   LD_LIBRARY_PATH=/mnt/sdcard/cubegm/lib). RetroArch is linked against those,
#   so without them it dies at load time ("cannot open shared object file")
#   and the screen never lights. Copy every NEEDED .so (and transitive deps)
#   from the sysroot into $DST/lib. Base libs (libc/libm/pthread/dl/gcc/ld)
#   are provided by the device rootfs, so we exclude them to avoid shipping a
#   second glibc that could mismatch the device's dynamic linker.
#   例外：libasound.so.2 也由设备 rootfs 提供（原厂 1.1.5）——因其编译期
#   ALSA_CONFIG_DIR 正确指向 rootfs /usr/share/alsa，优于 sysroot 1.2.10 的
#   CI 路径，已归入 BASE_LIBS 排除（音频方案A，见 STAGE 9b 上方 BASE_LIBS 说明）。
# -----------------------------------------------------------------------------
log "Bundling runtime libs into $DST/lib ..."
mkdir -p "$DST/lib"
READELF="${CROSS_COMPILE}readelf"
# base libs the device rootfs always provides -- do NOT bundle these.
# v0.2 (音频方案A): libasound.so.2 也归入 BASE_LIBS。设备 rootfs 自带原厂
#   alsa-lib 1.1.5 (/usr/lib/libasound.so.2 -> 2.0.0)，其编译期 ALSA_CONFIG_DIR
#   = /usr/share/alsa（正确指向 rootfs 内完整配置树），且 ABI 经 readelf 验证
#   覆盖 RetroArch 全部 76 个 snd_* 引用符号（0 缺失）。crosstool sysroot 的
#   1.2.10 版会因 ALSA_CONFIG_DIR 指向 CI 路径 (/home/runner/...) 覆盖 rootfs
#   配置树 → "Unknown PCM" 无声。故不再打包 libasound，让 LD_LIBRARY_PATH
#   找不到时回落到 rootfs 原厂库（原版音质，无重采样）。
BASE_LIBS="libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 libgcc_s.so.1 \
           librt.so.1 libutil.so.1 ld-linux-armhf.so.3 ld-2.29.so libasound.so.2"
# v8.8: libstdc++.so.6/libatomic.so.1 removed from BASE_LIBS and forced into the
# bundle. diag-285 on-device cores scan showed nestopia/snes9x/vice_x64 failing
# with "GLIBCXX_3.4.32 not found": the device's /usr/lib/libstdc++.so.6 is too
# old for cores built with the modern GCC toolchain. The sysroot's newer
# libstdc++.so.6 is bundled into cubegm/lib and zhijack sets LD_LIBRARY_PATH
# there first, so C++ cores resolve against it.
is_base() { for b in $BASE_LIBS; do [ "$1" = "$b" ] && return 0; done; return 1; }
declare -A _seen=()
_queue=()
for _b in RetroArch/retroarch; do
    [ -f "$_b" ] || continue
    if command -v "$READELF" >/dev/null 2>&1; then
        while IFS= read -r _l; do [ -n "$_l" ] && _queue+=("$_l"); done \
            < <("$READELF" -d "$_b" 2>/dev/null | awk -F'[][]' '/NEEDED/ {gsub(/[ \t]/,"",$2); print $2}')
    fi
done
# v8.8: also scan every built C++ core for its NEEDED libs (libstdc++.so.6,
# libatomic.so.1) -- they run in the same process and the device libs are too
# old (GLIBCXX_3.4.32 diag failure).
for _so in "$CORE_OUT"/*_libretro.so; do
    [ -f "$_so" ] || continue
    if command -v "$READELF" >/dev/null 2>&1; then
        while IFS= read -r _l; do [ -n "$_l" ] && _queue+=("$_l"); done \
            < <("$READELF" -d "$_so" 2>/dev/null | awk -F'[][]' '/NEEDED/ {gsub(/[ \t]/,"",$2); print $2}')
    fi
done
# v8.8: force the C++ runtime into the bundle even if no core declares it as a
# direct NEEDED (it may be loaded via DT_NEEDED of another bundled lib).
_queue+=("libstdc++.so.6" "libatomic.so.1")
# fallback: if readelf was unavailable, seed the known direct deps
# (libasound.so.2 不在此列：音频方案A 改为回落 device rootfs 原厂 1.1.5)
if [ ${#_queue[@]} -eq 0 ]; then
    _queue=(libSDL.so.1 libpng12.so.0 libz.so.1)
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
log "On next boot retroarch launches with rgui menu."
