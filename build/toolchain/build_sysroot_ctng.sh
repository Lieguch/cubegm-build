#!/usr/bin/env bash
# build_sysroot_ctng.sh -- 用 crosstool-NG 自举 RK3036G 的 glibc-2.17 ARMv7 hard-float sysroot
set -euo pipefail

PREFIX="${1:-/opt/cubegm-toolchain}"
CTNG_VER="1.26.0"
JOBS="$(nproc)"

LOGDIR="$(cd "$(dirname "$0")" && pwd)/../../cubegm-build-logs"
mkdir -p "$LOGDIR"
LOG="$LOGDIR/crosstool_build.log"
: > "$LOG"
exec > >(tee -a "$LOG") 2>&1
echo "[ct-ng] logging to $LOG"
echo "[ct-ng] PREFIX=$PREFIX  CTNG_VER=$CTNG_VER  JOBS=$JOBS  host=$(uname -a)"

echo "== 1) 获取 crosstool-NG $CTNG_VER =="
if [ ! -d crosstool-NG ]; then
  git clone https://github.com/crosstool-ng/crosstool-NG.git
  (cd crosstool-NG && git checkout "crosstool-ng-$CTNG_VER")
fi
( cd crosstool-NG && ./bootstrap && ./configure --enable-local && make -j"$JOBS" )

# 关键修复（对照 ct-ng.in / samples.mk 真源）：
#  (a) sample 目标名是「裸 sample 名」armv7-rpi2-linux-gnueabihf（samples.mk 只有 $(CT_SAMPLES): 规则）。
#  (b) --enable-local 且未 make install 时，ct-ng.in:28 的 CT_LIB_DIR=@pkgdatadir@ 在独立 make 语境下
#      为空，导致 samples.mk 的 wildcard 找不到样本。必须显式传入 CT_LIB_DIR 指向源码目录。
CTNG_DIR="$(cd crosstool-NG && pwd)"
echo "== 2) 生成 armv7-rpi2-linux-gnueabihf 基础配置（ARMv7 hard-float；ct-ng 1.26.0 样本）=="
./crosstool-NG/ct-ng CT_LIB_DIR="$CTNG_DIR" armv7-rpi2-linux-gnueabihf

echo "== 3) 固定版本组合（glibc 2.17 / gcc 9.5.0 / binutils 2.28.1）=="
# ---- glibc 2.17 ----
sed -i -E 's/^CT_GLIBC_V_[0-9_]+\=y/# & (disabled)/' .config
sed -i -E '/^# CT_GLIBC_V_2_17 is not set/d' .config
grep -q '^CT_GLIBC_V_2_17=y' .config || echo 'CT_GLIBC_V_2_17=y' >> .config
# ---- gcc 9.5.0 ----
sed -i -E 's/^CT_CC_GCC_V_[0-9_]+\=y/# & (disabled)/' .config
sed -i -E '/^# CT_CC_GCC_V_9_5_0 is not set/d' .config
grep -q '^CT_CC_GCC_V_9_5_0=y' .config || echo 'CT_CC_GCC_V_9_5_0=y' >> .config
sed -i -E 's/^CT_GCC_VERSION=.*/CT_GCC_VERSION="9.5.0"/' .config
# ---- binutils 2.28.1 ----
sed -i -E 's/^CT_BINUTILS_V_[0-9_]+\=y/# & (disabled)/' .config
sed -i -E '/^# CT_BINUTILS_V_2_28_1 is not set/d' .config
grep -q '^CT_BINUTILS_V_2_28_1=y' .config || echo 'CT_BINUTILS_V_2_28_1=y' >> .config
sed -i -E 's/^CT_BINUTILS_VERSION=.*/CT_BINUTILS_VERSION="2.28.1"/' .config
# ---- 目标三元组：arm-linux-gnueabihf ----
sed -i -E 's/^CT_TARGET=.*/CT_TARGET="arm-linux-gnueabihf"/' .config
grep -q '^CT_TARGET=' .config || echo 'CT_TARGET="arm-linux-gnueabihf"' >> .config
# ---- 输出前缀 + 日志 ----
grep -q '^CT_PREFIX_DIR=' .config \
  && sed -i "s|^CT_PREFIX_DIR=.*|CT_PREFIX_DIR=\"$PREFIX\"|" .config \
  || echo "CT_PREFIX_DIR=\"$PREFIX\"" >> .config
echo 'CT_LOG_PROCESS_BARE=true' >> .config

echo "== 3b) 解析后的关键 .config 选择（供诊断）=="
grep -E '^CT_(GLIBC|CC_GCC|BINUTILS|LINUX)_V_[0-9]' .config || true
grep -E '^CT_(GLIBC|GCC|BINUTILS|LINUX|KERNEL)_VERSION=' .config || true
grep -E '^CT_TARGET=' .config || true

echo "== 3c) 校验配置（olddefconfig，提前暴露非法组合）=="
./crosstool-NG/ct-ng CT_LIB_DIR="$CTNG_DIR" olddefconfig
echo "olddefconfig OK"

echo "== 4) 构建（耗时较长，可喝杯茶）=="
./crosstool-NG/ct-ng CT_LIB_DIR="$CTNG_DIR" build CT_JOBS="$JOBS"

echo "== 5) 产出与校验 =="
SYSROOT="$PREFIX/arm-linux-gnueabihf/sysroot"
ls -l "$SYSROOT/lib/libc.so.6" "$SYSROOT/usr/include/stdio.h"
echo
echo "# 校验 glibc 版本：必须含 2.17 且没有 >2.17"
strings "$SYSROOT/lib/libc.so.6" | grep -o 'GLIBC_2\.[0-9]*' | sort -V | uniq | tail -3
echo
echo "DONE. 之后对每个产物跑 verify_target_abi.sh 做 ABI 门禁（EM_ARM / 0x5000400 / <=GLIBC_2.17）。"
