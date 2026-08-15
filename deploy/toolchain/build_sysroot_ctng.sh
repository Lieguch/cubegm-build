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

echo "== 2) 生成 armv7a-hardfloat 基础配置 =="
./crosstool-NG/ct-ng armv7a-hardfloat-linux-gnueabihf_defconfig

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

echo "== 6) 放开 sysroot 写权限（供 STAGE 2 交叉编 zlib/libpng/alsa/SDL 的 make install 落盘）=="
# 实测 run 31882525698 根因：crosstool-NG 把 sysroot 内文件/目录装成受限权限
# （如 /usr/include/stdio.h = r--r--r-- 0444、/usr/lib、/usr/share 对运行 bootstrap 的
#  非-root 用户 runner 不可写），导致 STAGE 2 make install 写 /usr/lib、/usr/share/man
#  时 Permission denied -> make: *** [Makefile:309: install-libs] Error 1 -> bootstrap exit 2。
# 本脚本以 runner 身份构建 sysroot，整个 sysroot 归 runner 所有，问题纯是【权限位】被设成
#  不可写（0555/0444），不是属主。因此以当前用户直接 chmod 即可（无需 sudo）。
#  先 best-effort chown（应对缓存恢复带来的 stale-uid / root 归属），再【无条件】chmod -R u+rwX
#  （真正修复权限位；旧 build_sdl_libpng.sh 的 sudo chown ... || sudo chmod ... 因 || 短路，
#   chown 成功时 chmod 永不执行，正是本次 Permission denied 的元凶）。
if [ -d "$SYSROOT" ]; then
  chown -R "$(id -u):$(id -g)" "$SYSROOT" 2>/dev/null || sudo chown -R "$(id -u):$(id -g)" "$SYSROOT" 2>/dev/null || true
  chmod -R u+rwX "$SYSROOT" 2>/dev/null || sudo chmod -R u+rwX "$SYSROOT" 2>/dev/null || true
  for d in "$SYSROOT/usr" "$SYSROOT/usr/lib" "$SYSROOT/usr/include" "$SYSROOT/usr/bin" "$SYSROOT/usr/share" "$SYSROOT/usr/lib/pkgconfig"; do [ -d "$d" ] || mkdir -p "$d" 2>/dev/null || sudo mkdir -p "$d" 2>/dev/null || true; done
  echo "sysroot 权限已放开；/usr/lib 可写检查: $([ -w "$SYSROOT/usr/lib" ] && echo YES || echo NO)"
else
  echo "[WARN] $SYSROOT 不存在，跳过权限放开"
fi
