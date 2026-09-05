#!/usr/bin/env bash
# =============================================================================
#  CubeGM -- CNB 平台 bootstrap 包装脚本
#  在 CNB ubuntu:22.04 容器内执行：
#    1. 网络快速诊断（DNS + curl，失败只警告不中断）
#    2. 切换腾讯云 apt 镜像（国内加速）
#    3. 防御 CRLF（脚本可能在 Windows 编写）
#    4. 调用 deploy/bootstrap_linux.sh 全链路构建
#  .cnb.yml 的 push / api_trigger 均调用本脚本，避免 YAML 内嵌复杂脚本。
#  用法: cnb_bootstrap.sh [--dev]   --dev=仅初始化云开发环境(全工具链+apt)，
#        不跑全构建(用于 CNB 云原生开发 vscode 事件)。
# =============================================================================
set -euo pipefail

DEV_MODE=0
[ "${1:-}" = "--dev" ] && DEV_MODE=1

export DEBIAN_FRONTEND=noninteractive
HERE="$(cd "$(dirname "$0")" && pwd)"

echo "== CNB bootstrap wrapper (cnb_bootstrap.sh) ==${DEV_MODE:+ [DEV MODE]}"
echo "runner: $(uname -a)"

# ---- 1. network diagnostics (never fatal; only informational) --------------
echo "== network diagnostics (pre-cert; https may 000 without ca-cert) =="
if command -v getent >/dev/null 2>&1; then
  for h in archive.ubuntu.com security.ubuntu.com github.com mirrors.cloud.tencent.com; do
    if getent hosts "$h" >/dev/null 2>&1; then echo "  DNS OK: $h"; else echo "  DNS FAIL: $h"; fi
  done
fi
for u in https://archive.ubuntu.com/ https://github.com/ https://mirrors.cloud.tencent.com/; do
  code="$(curl -m 10 -s -o /dev/null -w '%{http_code}' "$u" 2>/dev/null || echo 000)"
  echo "  curl $u -> $code"
done

# ---- 2. CN apt mirror (Tencent Cloud) ---------------------------------------
echo "== apt mirrors -> Tencent Cloud =="
cp /etc/apt/sources.list /etc/apt/sources.list.bak 2>/dev/null || true
cat > /etc/apt/sources.list <<'APTEOF'
deb http://mirrors.cloud.tencent.com/ubuntu/ jammy main restricted universe multiverse
deb http://mirrors.cloud.tencent.com/ubuntu/ jammy-updates main restricted universe multiverse
deb http://mirrors.cloud.tencent.com/ubuntu/ jammy-backports main restricted universe multiverse
deb http://mirrors.cloud.tencent.com/ubuntu/ jammy-security main restricted universe multiverse
APTEOF
for i in 1 2 3; do
  if apt-get update -qq; then break; fi
  echo "WARN: apt-get update attempt $i failed -- retry $((i+1)) in $((i*5))s"
  sleep "$((i*5))"
done
apt-get install -y -qq --no-install-recommends \
    sudo ca-certificates git make curl wget libncurses-dev libncursesw5-dev >/dev/null 2>&1 || true
# crosstool-NG 1.26.0 configure.ac:320 硬性要求 curses (AX_WITH_CURSES + AC_MSG_ERROR),
# 缺则 STAGE 1 ct-ng configure 直接 'curses library not found' 退出。
# 前两版 fix (4795db0 / a91c334) 用 `> /dev/null 2>&1 || true` 静默吞错,导致运行期
# libncurses-dev 实际未装时无人察觉。本版改为显式校验 dpkg 状态 + 最多 2 次重试,
# 仍失败则 die(让构建直接红,而不是走到 ct-ng 才挂 — 节省 90s bootstrap + 1m ct-ng clone)。
for _nc_try in 1 2; do
  if dpkg -s libncurses-dev >/dev/null 2>&1 && pkg-config --exists ncurses 2>/dev/null; then
    echo "  libncurses-dev OK (dpkg + pkg-config ncurses 命中)"
    break
  fi
  echo "  libncurses-dev missing/broken (try $_nc_try) -- 显式重装并打印错误"
  apt-get install -y -qq --no-install-recommends libncurses-dev libncursesw5-dev 2>&1 | tail -5 || true
  if [ "$_nc_try" = "2" ]; then
    dpkg -s libncurses-dev >/dev/null 2>&1 || { echo "FATAL: libncurses-dev 仍未安装"; exit 1; }
  fi
done

# ---- 1b. post-cert network check -------------------------------------------------
echo "== network diagnostics (post-cert) =="
for u in https://archive.ubuntu.com/ https://github.com/ https://mirrors.cloud.tencent.com/; do
  code="$(curl -m 10 -s -o /dev/null -w '%{http_code}' "$u" 2>/dev/null || echo 000)"
  echo "  curl $u -> $code"
done

# ---- 3. defend CRLF (scripts authored on Windows) ---------------------------
echo "== strip CRLF =="
cd "$HERE"
sed -i 's/\r$//' bootstrap_linux.sh build_sdl_libpng.sh build.sh build_sf3000_armhf.sh 2>/dev/null || true
sed -i 's/\r$//' ../build/toolchain/build_sysroot_ctng.sh ../build/toolchain/verify_target_abi.sh 2>/dev/null || true
chmod +x bootstrap_linux.sh build_sdl_libpng.sh build.sh build_sf3000_armhf.sh 2>/dev/null || true
chmod +x ../build/toolchain/*.sh 2>/dev/null || true

# ---- 3.5 dev-mode: full toolchain, then STOP (no full build) ----------------
if [ "$DEV_MODE" = "1" ]; then
  echo "== [DEV] installing full build toolchain =="
  apt-get install -y -qq --no-install-recommends \
      build-essential gcc g++ flex bison texinfo gawk \
      libgmp-dev libmpfr-dev libmpc-dev pkg-config autoconf automake \
      libtool libtool-bin gperf dpkg-dev binutils-dev zlib1g-dev \
      python3 python3-pip python3-dev help2man zip unzip file \
      libdrm-dev libasound2-dev gettext >/dev/null 2>&1 || true
  echo "== [DEV] toolchain ready =="
  git --version
  gcc --version | head -1
  python3 --version
  echo "== [DEV] environment initialized. Connect via VS Code/WebIDE. =="
  exit 0
fi

# ---- 4. full bootstrap build ------------------------------------------------
echo "== running deploy/bootstrap_linux.sh =="
PREFIX="${PREFIX:-/opt/cubegm-toolchain}" ./bootstrap_linux.sh