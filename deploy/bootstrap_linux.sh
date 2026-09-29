#!/usr/bin/env bash
# =============================================================================
#  CubeGM / RK3036G -- ONE-COMMAND bootstrap (ANY x86_64 Linux)
# -----------------------------------------------------------------------------
#  Runs on ANY fresh x86_64 Linux host: WSL2, cloud VM, VirtualBox VM,
#  GitHub Actions runner, or bare-metal Ubuntu/Debian. It does the ENTIRE
#  build end-to-end:
#
#    STAGE 0  apt install build dependencies (Ubuntu/Debian only)
#    STAGE 1  crosstool-NG: self-build a glibc-2.29 ARM sysroot + gcc
#             (the device ceililing -- picoarch dlopen's cores in-process, so
#              it MUST share glibc 2.29; this is NOT optional)
#    STAGE 2  cross-build libpng12 + alsa-lib + SDL1.2 into that sysroot
#             (picoarch links -lSDL -lpng12; RISKIEST stage, see build_sdl_libpng.sh)
#    STAGE 3  deploy/build.sh: clone r36sx + libretro-common submodule + apply
#             5-edit patch -> build picoarch(ARM) + FrogUI + cores -> ABI gate
#             -> stage into deploy/cubegm/
#
#  Everything is checkpointed: re-running skips already-done stages (the
#  crosstool build alone takes ~30-90 min, so you do NOT want to redo it).
#
#  USAGE
#    cd deploy
#    sudo ./bootstrap_linux.sh            # full build (default PREFIX=/opt/cubegm-toolchain)
#    PREFIX=~/cubegm-tc ./bootstrap_linux.sh
#    SYSROOT=/existing/sysroot CORES="mgba fceumm" ./bootstrap_linux.sh   # skip toolchain
#
#  REQUIREMENTS
#    - x86_64 Linux of any kind (WSL2, cloud VM, VirtualBox, GitHub Actions, bare
#      metal). Ubuntu/Debian 22.04+ recommended for STAGE 0's apt step. ~15 GB free disk.
#    - Network access to github.com, sourceforge.net, libsdl.org, developer.arm.com.
#    - sudo (for apt, only needed on Debian-family hosts).
#
#  !!! NOT EXECUTED BY THE AGENT -- authored in a Windows sandbox !!!
#  Stages 1-2 are unverified on real hardware; if crosstool or SDL fails, the
#  on-screen error + build_sdl_libpng.sh header tell you what to adjust.
# =============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

# ---- 自身日志：tee 到文件供 Agent 取回诊断 ----
LOGDIR="$(cd "$HERE/.." && pwd)/cubegm-build-logs"
mkdir -p "$LOGDIR"
BOOTSTRAP_LOG="$LOGDIR/bootstrap.log"
: > "$BOOTSTRAP_LOG"
exec > >(tee -a "$BOOTSTRAP_LOG") 2>&1
echo "[bootstrap] logging to $BOOTSTRAP_LOG"

log(){ printf '\033[1;32m[bootstrap]\033[0m %s\n' "$*"; }
warn(){ printf '\033[1;33m[bootstrap]\033[0m %s\n' "$*"; }
die(){ printf '\033[1;31m[ERROR]\033[0m %s\n' "$*" >&2; exit 1; }

# ---- pre-build static gate: forbid undefined helper calls -------------------
# Catches the bug class that `bash -n` CANNOT: a call to a helper function that
# is referenced but never defined in the same file (e.g. calling warn() that was
# never defined). This is deterministic -- no guessing. Runs BEFORE any expensive
# stage (crosstool takes ~30-90 min) so CI fails fast instead of mid-STAGE-2.
SCRIPT_VERIFY="$HERE/../tools/verify_build_scripts.py"
if command -v python3 >/dev/null 2>&1 && [ -f "$SCRIPT_VERIFY" ]; then
  log "Pre-build gate: static check for undefined helper calls ..."
  python3 "$SCRIPT_VERIFY" "$HERE" "$HERE/../build" \
    || die "PRE-BUILD GATE FAILED: fix the undefined helper call(s) above before pushing. Do NOT bypass."
  log "Pre-build gate: passed."
else
  warn "Pre-build gate skipped (python3 or tools/verify_build_scripts.py missing)."
fi

# ---- build report (always emitted on exit, success or failure) -------------
emit_report(){
  local rc=${1:-$?}
  {
    echo "=== CubeGM build report (bootstrap_linux.sh) ==="
    echo "date     : $(date -u)"
    echo "host     : $(uname -a)"
    echo "gcc      : $(arm-linux-gnueabihf-gcc --version 2>/dev/null | head -1 || echo 'NOT ON PATH')"
    echo "PREFIX   : ${PREFIX:-<unset>}"
    echo "SYSROOT  : ${SYSROOT:-<unset>}"
    if [ -n "${SYSROOT:-}" ] && [ -f "$SYSROOT/lib/libc.so.6" ]; then
      echo "sysroot glibc ceiling: $(strings "$SYSROOT/lib/libc.so.6" 2>/dev/null | grep -o 'GLIBC_2\.[0-9]*' | sort -V | uniq | tail -1)"
    else
      echo "sysroot libc: NOT BUILT YET (STAGE 1 not completed)"
    fi
    echo "--- deploy/cubegm (device payload) ---"
    if [ -d "$HERE/cubegm" ]; then
      echo "present; top-level:"
      ls -la "$HERE/cubegm" 2>/dev/null | head -20
      echo "picoarch GLIBC req: $(strings "$HERE/cubegm/picoarch" 2>/dev/null | grep -o 'GLIBC_2\.[0-9]*' | sort -V | uniq | tail -1 || echo n/a)"
    else
      echo "NOT PRODUCED YET (build did not reach STAGE 3)"
    fi
    echo "--- crosstool build log ---"
    CTLOG="$LOGDIR/crosstool_build.log"
    if [ -f "$CTLOG" ]; then
      echo "path: $CTLOG ($(wc -l < "$CTLOG") lines)"
      echo "=== tail (last 80 lines) ==="
      tail -n 80 "$CTLOG"
    else
      echo "NOT FOUND ($CTLOG)"
    fi
    echo "--- bootstrap exit code: $rc ---"
  } > "$HERE/BUILD_REPORT.txt" 2>&1
  echo ""
  echo "==================================================================="
  echo " BUILD_REPORT -> $HERE/BUILD_REPORT.txt"
  echo " Send its contents back to the agent for diagnosis (any outcome)."
  echo "==================================================================="
}
trap 'emit_report $?' ERR

# ---- preflight --------------------------------------------------------------
[ "$(uname -s)" = "Linux" ] || die "Must run on a Linux host (x86_64). Current: $(uname -s)"
case "$(uname -m)" in x86_64|amd64) ;; *) die "Need x86_64 build host, got $(uname -m)";; esac
command -v git  >/dev/null || die "git missing"
command -v curl >/dev/null || die "curl missing"
command -v make >/dev/null || die "make missing"

# ---- config -----------------------------------------------------------------
PREFIX="${PREFIX:-/opt/cubegm-toolchain}"
SYSROOT="${SYSROOT:-}"          # if set, skip STAGE 1+2 (assume already built)
CORES="${CORES:-}"   # default resolved inside build.sh (DEFAULT_CORES, 17 cores)

# =============================================================================
# STAGE 0 -- apt build dependencies (Ubuntu / Debian)
# =============================================================================
if command -v apt-get >/dev/null 2>&1; then
    if [ -f /tmp/.cubegm_apt_done ]; then
        log "STAGE 0: apt deps already installed (marker present) -- skip"
    else
        log "STAGE 0: installing build dependencies via apt ..."
        export DEBIAN_FRONTEND=noninteractive
        # apt hangs without an http timeout (GitHub issue actions/runner-images#11382).
        # Official remedy: Acquire::Retries + explicit http timeout + retry loop; a
        # transient mirror blip must NOT kill the whole build (runner preinstalls
        # most of these tools anyway).
        for i in 1 2 3; do
            sudo apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=90 update \
                && break
            log "WARN: apt-get update attempt $i failed -- retrying in $((i*5))s"
            sleep "$((i*5))"
        done
        # ★ 2026-09-29 ROOT CAUSE FIX: this used to be ONE atomic apt transaction.
        #   jammy's `pkgconf` declares `Breaks: pkg-config (>= 0.29-1)`, and the
        #   ubuntu-22.04 runner image PREINSTALLS pkg-config 0.29.2 (official:
        #   actions/runner-images/images/ubuntu/Ubuntu2204-Readme.md). Both cannot
        #   coexist, so apt aborted the WHOLE transaction:
        #       pkgconf : Breaks: pkg-config (>= 0.29-1)
        #       E: Unable to correct problems, you have held broken packages.
        #   -> meson / ninja-build / python3-mako were never installed
        #   -> STAGE 4.8 died with "ERROR: meson missing".
        #
        #   Fix: (1) drop pkgconf -- it is NOT needed, pkg-config is the Debian
        #   default and build_mesa_lima.sh already accepts either binary
        #   (PKGCONF="$(command -v pkgconf || command -v pkg-config)");
        #   (2) split the install into independent groups so one bad package can
        #   never take down the toolchain-critical ones again.
        _apt_group() {
            sudo apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=90 install -y \
                --no-install-recommends "$@" \
                || log "WARN: apt group failed (continuing): $*"
        }
        _apt_group build-essential gcc g++ make git curl wget xz-utils \
            flex bison texinfo gawk libgmp-dev libmpfr-dev libmpc-dev \
            gperf dpkg-dev binutils-dev zlib1g-dev python3 python3-pip python3-dev \
            help2man zip unzip file
        _apt_group autoconf automake libtool libtool-bin
        _apt_group pkg-config libdrm-dev libasound2-dev
        _apt_group python3-mako meson ninja-build

        # Meson official install route when the distro package is unavailable:
        # https://mesonbuild.com/Getting-meson.html  (pip install meson / ninja)
        for _t in meson ninja; do
            if ! command -v "$_t" >/dev/null 2>&1; then
                log "WARN: $_t missing after apt -- installing via pip (Meson official route)"
                sudo pip3 install --no-cache-dir "$_t" >/dev/null 2>&1 \
                    || pip3 install --user --no-cache-dir "$_t" >/dev/null 2>&1 \
                    || warn "pip install $_t FAILED -- STAGE 4.8 will not build"
            fi
        done
        # Mesa requires the Python Mako module (official: docs.mesa3d.org/meson.html
        # "Build requirements": python3-mako).
        if ! python3 -c 'import mako' >/dev/null 2>&1; then
            log "WARN: python3-mako missing -- installing via pip (Mesa requirement)"
            sudo pip3 install --no-cache-dir Mako >/dev/null 2>&1 \
                || pip3 install --user --no-cache-dir Mako >/dev/null 2>&1 \
                || warn "pip install Mako FAILED -- Mesa meson.build will not configure"
        fi
        sudo touch /tmp/.cubegm_apt_done
        log "STAGE 0: apt deps installed (meson=$(command -v meson || echo MISSING), ninja=$(command -v ninja || command -v ninja-build || echo MISSING), pkg-config=$(command -v pkg-config || echo MISSING))"
    fi
else
    warn "STAGE 0: apt-get not found -- assume deps already present (non-Debian host)."
fi

# =============================================================================
# STAGE 1 -- glibc-2.29 sysroot + gcc (crosstool-NG)
# =============================================================================
if [ -z "$SYSROOT" ]; then
    SYSROOT="$PREFIX/arm-linux-gnueabihf/sysroot"
    if [ -f "$SYSROOT/lib/libc.so.6" ]; then
        log "STAGE 1: reusing existing sysroot at $SYSROOT"
    else
        log "STAGE 1: building glibc-2.29 sysroot + gcc via crosstool-NG (~30-90 min) ..."
        bash "$HERE/../build/toolchain/build_sysroot_ctng.sh" "$PREFIX"
        [ -f "$SYSROOT/lib/libc.so.6" ] || die "STAGE 1: sysroot not produced at $SYSROOT"
        log "STAGE 1: sysroot ready. glibc ceiling check:"
        strings "$SYSROOT/lib/libc.so.6" | grep -o 'GLIBC_2\.[0-9]*' | sort -V | uniq | tail -3
    fi
else
    log "STAGE 1: SYSROOT provided ($SYSROOT) -- skip crosstool build."
fi
[ -d "$SYSROOT" ] || die "SYSROOT missing: $SYSROOT"
export PATH="$PREFIX/bin:$PATH"
command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1 || die "arm-linux-gnueabihf-gcc not on PATH after STAGE 1"
log "Toolchain: $(arm-linux-gnueabihf-gcc --version | head -1)"

# =============================================================================
# STAGE 2 -- libpng12 + alsa-lib + SDL1.2 into the sysroot
# =============================================================================
if [ -x "$SYSROOT/usr/bin/sdl-config" ] && [ -f "$SYSROOT/usr/lib/libpng12.so" ]; then
    log "STAGE 2: SDL/libpng already in sysroot -- skip"
else
    log "STAGE 2: cross-building libpng12 + alsa-lib + SDL1.2 into sysroot (riskiest stage) ..."
    SYSROOT="$SYSROOT" bash "$HERE/build_sdl_libpng.sh"
fi

# =============================================================================
# STAGE 3 -- front-end + cores + ABI gate + stage (reuses deploy/build.sh)
# =============================================================================
log "STAGE 3: running deploy/build.sh (clone r36sx + patch + picoarch + cores + gate) ..."
SYSROOT="$SYSROOT" CORES="$CORES" bash "$HERE/build.sh"

log ""
log "==================================================================="
log " BOOTSTRAP COMPLETE (on this Linux host)."
log " Next: copy deploy/cubegm/ to the SD card root, overwriting cubegm/."
log " Verify on device: FrogUI boots, USB gamepad works, RTC time correct,"
log " strategy-game .srm persists across power-off."
log " If STAGE 2 (SDL) failed, see deploy/LINUX_BUILD.md 'Prebuilt fallback'."
log "==================================================================="
emit_report 0
