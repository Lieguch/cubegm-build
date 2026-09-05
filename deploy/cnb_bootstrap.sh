#!/usr/bin/env bash
# =============================================================================
#  CubeGM -- CNB 平台 bootstrap 包装脚本
#  在 CNB ubuntu:22.04 容器内执行：
#    1. 网络快速诊断（DNS + curl，失败只警告不中断）
#    2. 切换腾讯云 apt 镜像（国内加速）
#    3. 防御 CRLF（脚本可能在 Windows 编写）
#    4. 调用 deploy/bootstrap_linux.sh 全链路构建
#  .cnb.yml 的 push / api_trigger 均调用本脚本，避免 YAML 内嵌复杂脚本。
# =============================================================================
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
HERE="$(cd "$(dirname "$0")" && pwd)"

echo "== CNB bootstrap wrapper (cnb_bootstrap.sh) =="
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
# crosstool-NG configure 需要 curses; 纯净容器缺 libncurses-dev
if ! apt-get install -y -qq libncurses-dev libncursesw5-dev >/dev/null 2>&1; then
  echo "WARN: libncurses-dev install failed (crosstool configure may fail)"
fi

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

# ---- 4. full bootstrap build ------------------------------------------------
echo "== running deploy/bootstrap_linux.sh =="
PREFIX="${PREFIX:-/opt/cubegm-toolchain}" ./bootstrap_linux.sh