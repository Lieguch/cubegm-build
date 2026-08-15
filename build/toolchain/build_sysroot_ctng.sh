#!/usr/bin/env bash
# build_sysroot_ctng.sh -- 用 crosstool-NG 自举 RK3036G 的 glibc-2.17 ARMv7 hard-float sysroot
#
# 为什么需要它：
#   picoarch 通过 dlopen(core.c:720) 把 libemu_*.so 加载进【同一进程】，必须与设备 core
#   共用 glibc。设备全二进制天花板 = GLIBC_2.17（libemu_fbalpha.so 拉高；其余仅 2.4-2.7）。
#   现代 ARM GNU 13.2 工具链自带 glibc 2.38，直接链接会在设备运行期报 GLIBC_2.xx not found。
#   静态链接也被否决（同进程双 glibc 冲突崩溃）。
#   -> 链接目标必须是 glibc <= 2.17 的 sysroot。本脚本从公开上游源码构建一份等价的 2.17 sysroot，
#      无需设备 rootfs（若日后能拿到 rootfs，sysroot_from_device.sh 是更贴近真机的金标准）。
#
# 运行环境：x86_64 Linux 构建机（需 git/make/autotools/gcc/g++/libncurses-dev/libtool 等 ct-ng 依赖）。
# 耗时：约 20-60 分钟（首次下载并编译 gcc/glibc/binutils）。
# 用法： ./build_sysroot_ctng.sh [PREFIX]     # PREFIX 默认 /opt/cubegm-toolchain
set -euo pipefail

PREFIX="${1:-/opt/cubegm-toolchain}"
CTNG_VER="1.26.0"            # 已知可构建 glibc 2.17 的 crosstool-NG 版本
JOBS="$(nproc)"

echo "== 1) 获取 crosstool-NG $CTNG_VER =="
if [ ! -d crosstool-NG ]; then
  git clone https://github.com/crosstool-ng/crosstool-NG.git
  (cd crosstool-NG && git checkout "crosstool-ng-$CTNG_VER")
fi
( cd crosstool-NG && ./bootstrap && ./configure --enable-local && make -j"$JOBS" )

echo "== 2) 生成 armv7-rpi2-linux-gnueabihf 基础配置（ARMv7 hard-float；已核实存在于 ct-ng 1.26.0，原 armv7a-hardfloat-linux-gnueabihf 不存在）=="
./crosstool-NG/ct-ng armv7-rpi2-linux-gnueabihf_defconfig

echo "== 3) 固定为 glibc-2.17 + gcc-6.5 + Linux 4.4 headers（必须 <=2.17）=="
# crosstool-NG 用 .config 控制；下面用 sed 关闭其它版本、启用目标版本。
# 若 sed 未命中（不同 ct-ng 版本键名略有差异），请改跑 `./crosstool-NG/ct-ng menuconfig` 手动设：
#   C library (glibc) -> Version 2.17
#   GCC -> 6.5.0
#   Linux kernel headers -> 4.4.x
#   Target options -> ARMv7a, float ABI = hard
# ---- glibc 版本 ----
sed -i -E 's/^CT_GLIBC_V_[0-9_]+=y/# & (disabled)/' .config
sed -i -E 's/^# CT_GLIBC_V_2_17 is not set/CT_GLIBC_V_2_17=y/' .config
# ---- gcc 版本（glibc 2.17 用 gcc 6.x 编译最稳）----
sed -i -E 's/^CT_GCC_VERSION=.*/CT_GCC_VERSION="6.5.0"/' .config
# ---- Linux 内核头（RK3036G SDK 用 4.4）----
sed -i -E 's/^CT_LINUX_V_[0-9_]+=y/# & (disabled)/' .config
sed -i -E 's/^# CT_LINUX_V_4_4 is not set/CT_LINUX_V_4_4=y/' .config
# ---- 目标三元组：必须与 build.sh / build_sf3000_armhf.sh 一致（arm-linux-gnueabihf）
# 默认 defconfig 产生 armv7a-hardfloat-linux-gnueabihf，会导致 build 脚本找不到
# ${TARGET}-gcc。强制改为 arm-linux-gnueabihf。
sed -i -E 's/^CT_TARGET=.*/CT_TARGET="arm-linux-gnueabihf"/' .config
grep -q '^CT_TARGET=' .config || echo 'CT_TARGET="arm-linux-gnueabihf"' >> .config
# ---- 输出前缀 + 日志 ----
grep -q '^CT_PREFIX_DIR=' .config && sed -i "s|^CT_PREFIX_DIR=.*|CT_PREFIX_DIR=\"$PREFIX\"|" .config \
                                  || echo "CT_PREFIX_DIR=\"$PREFIX\"" >> .config
echo "CT_LOG_PROCESS_BARE=true" >> .config

echo "== 4) 构建（耗时较长，可喝杯茶）=="
./crosstool-NG/ct-ng build CT_JOBS="$JOBS"

echo "== 5) 产出与校验 =="
SYSROOT="$PREFIX/arm-linux-gnueabihf/sysroot"
ls -l "$SYSROOT/lib/libc.so.6" "$SYSROOT/usr/include/stdio.h"
echo
echo "# 校验 glibc 版本：必须含 2.17 且没有 >2.17"
strings "$SYSROOT/lib/libc.so.6" | grep -o 'GLIBC_2\.[0-9]*' | sort -V | uniq | tail -3
echo
echo "# 在 Makefile / 构建命令中使用："
echo "export PATH=\"$PREFIX/bin:\$PATH\""
echo "export CC=armv7a-hardfloat-linux-gnueabihf-gcc"
echo "export CFLAGS=\"--sysroot=$SYSROOT -march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -O2\""
echo
echo "DONE. 之后对每个产物跑 verify_target_abi.sh 做 ABI 门禁（EM_ARM / 0x5000400 / <=GLIBC_2.17）。"
