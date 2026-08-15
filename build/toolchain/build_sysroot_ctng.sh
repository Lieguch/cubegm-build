#!/usr/bin/env bash
# build_sysroot_ctng.sh -- 用 crosstool-NG 自举 RK3036G 的 glibc-2.17 ARMv7 hard-float sysroot
#
# 为什么需要它：
#   picoarch 通过 dlopen(core.c:720) 把 libemu_*.so 加载进【同一进程】，必须与设备 core
#   共用 glibc。设备全二进制天花板 = GLIBC_2.17（libemu_fbalpha.so 拉高；其余仅 2.4-2.7）。
#   现代 ARM GNU 13.2 工具链自带 glibc 2.38，直接链接会在设备运行期报 GLIBC_2.xx not found。
#   静态链接也被否决（同进程双 glibc 冲突崩溃）。
#   -> 链接目标必须是 glibc <= 2.17 的 sysroot。本脚本从公开上游源码构建一份等价的 2.17 sysroot。
#
# 版本组合（armv7-rpi2-linux-gnueabihf 样本默认，已在 crosstool-NG 1.26.0 核实；与本机参考工具链
#           ARM GNU 13.2 同代，目标 sysroot 锁 glibc 2.17 以满足设备 GLIBC_2.17 天花板）：
#   glibc   2.17     (CT_GLIBC_V_2_17；packages/glibc/2.17)
#   gcc     13.2.0   (样本默认；与参考工具链 ARM GNU 13.2 同代)
#   binutils 2.40    (样本默认)
#   linux   6.4      (内核头不影响产物 glibc 天花板)
#   gmp/mpfr/mpc/isl 样本默认 (6.2.1 / 4.2.1 / 1.2.1 / 0.26)
# 注：早期曾尝试把 gcc 钉到 9.5.0 / binutils 2.28.1，但与样本 choice 冲突被 kconfig 丢弃、回退默认；
#     经验证默认组合（gcc 13.2 + glibc 2.17）可正常构建且匹配设备参考工具链，故直接采用样本默认。
#
# 运行环境：x86_64 Linux 构建机（需 git/make/autotools/gcc/g++/libncurses-dev/libtool 等 ct-ng 依赖）。
# 耗时：约 30-90 分钟（首次下载并编译 gcc/glibc/binutils）。
# 用法： ./build_sysroot_ctng.sh [PREFIX]     # PREFIX 默认 /opt/cubegm-toolchain
set -euo pipefail

PREFIX="${1:-/opt/cubegm-toolchain}"
CTNG_VER="1.26.0"            # 已知可构建 glibc 2.17 的 crosstool-NG 版本
JOBS="$(nproc)"

# ---- 日志：全程 tee 到文件，供 Agent 通过 build-output 分支取回诊断 ----
LOGDIR="$(cd "$(dirname "$0")" && pwd)/../../cubegm-build-logs"
mkdir -p "$LOGDIR"
LOG="$LOGDIR/crosstool_build.log"
: > "$LOG"                   # 清空旧日志
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
#  (a) sample 目标名是「裸 sample 名」armv7-rpi2-linux-gnueabihf（不是内核风格 _defconfig 后缀，
#      samples.mk 里只有 $(CT_SAMPLES): 规则，没有 *_defconfig 目标）。
#  (b) --enable-local 且未 make install 时，ct-ng.in:28 的 CT_LIB_DIR=@pkgdatadir@ 在独立 make
#      语境下为空，导致 samples.mk 的 wildcard 找不到样本、连裸目标都不生成。必须在命令行显式
#      传入 CT_LIB_DIR 指向 crosstool-NG 源码目录（命令行赋值会覆盖 makefile 里的非 override 赋值）。
CTNG_DIR="$(cd crosstool-NG && pwd)"
echo "== 2) 生成 armv7-rpi2-linux-gnueabihf 基础配置（ARMv7 hard-float；ct-ng 1.26.0 样本）=="
./crosstool-NG/ct-ng CT_LIB_DIR="$CTNG_DIR" armv7-rpi2-linux-gnueabihf

echo "== 3) 固定版本组合（glibc 2.17；gcc/binutils 等采用样本默认）=="
# 思路：kconfig 的版本选择是 choice。glibc 2.17 是设备天花板，显式钉死；
# gcc 13.2.0 / binutils 2.40 等直接采用 armv7-rpi2 样本默认（与参考工具链 ARM GNU 13.2 同代），
# 不再手动钉旧版本——早期钉 9.5.0/2.28.1 因与样本 choice 冲突被 kconfig 丢弃、回退默认，徒劳且误导。
# 钉死 glibc：先把已选中的 =y 行注释掉，再写目标 =y，删残留 "not set" 注释，最后 olddefconfig 校验。

# ---- glibc 2.17（设备 GLIBC_2.17 天花板，必须钉死）----
sed -i -E 's/^CT_GLIBC_V_[0-9_]+\=y/# & (disabled)/' .config
sed -i -E '/^# CT_GLIBC_V_2_17 is not set/d' .config
grep -q '^CT_GLIBC_V_2_17=y' .config || echo 'CT_GLIBC_V_2_17=y' >> .config

# ---- gcc / binutils / mpfr / isl / gmp / mpc：样本默认（gcc 13.2.0 / binutils 2.40 / mpfr 4.2.1
#      / isl 0.26 / gmp 6.2.1 / mpc 1.2.1），不手动覆盖，避免与 choice 冲突被丢弃。----

# ---- 目标三元组：必须与下游 build.sh / build_sf3000_armhf.sh 一致（arm-linux-gnueabihf）----
# 失效做法（已废弃，run 31863236025 失败根因）：直接 sed CT_TARGET="arm-linux-gnueabihf"
#   不生效 —— crosstool-NG 在 olddefconfig/build 时按 scripts/functions:1192 的
#   arch[-vendor]-kernel-sys 公式重算，覆盖回样本默认 armv7-rpi2-linux-gnueabihf，
#   于是 sysroot 装在 ${PREFIX}/armv7-rpi2-linux-gnueabihf/sysroot，而 bootstrap/下游
#   去 ${PREFIX}/arm-linux-gnueabihf/sysroot 找，文件不存在 → exit 2。
# 正确做法（已对照 scripts/functions:1129,1198-1229 + config/arch.in + config/toolchain.in:124 真源核实）：
#   tuple 拼接：CT_TARGET_ARCH = CT_ARCH + CT_ARCH_SUFFIX (functions:1129)
#             = arm + v7 = armv7 ；再加 vendor 段 = armv7-rpi2-linux-gnueabihf。
#   要让最终 tuple = arm-linux-gnueabihf，必须同时处理两处：
#   1) CT_OMIT_TARGET_VENDOR=y —— 省略 vendor 段（config/toolchain.in:124 depends on !OMIT_TARGET_VENDOR）。
#   2) 清空 CT_ARCH_SUFFIX（""）—— 它是 string 类型输入字段（非计算字段），不会被
#      olddefconfig 重算覆盖；清空后 CT_TARGET_ARCH = arm，arch 段即纯 arm。
#   （run 31865727927 只做了 1) 没做 2)，结果 target = armv7-linux-gnueabihf，仍不匹配。）
#   经 CT_DoConfigSub 规范化 + functions:1229 重排 → 最终 tuple = arm-linux-gnueabihf
#   （与设备参考工具链 ARM GNU 13.2 完全一致）。
sed -i -E 's/^CT_TARGET_VENDOR=.*/# & (disabled by CT_OMIT_TARGET_VENDOR)/' .config
sed -i -E '/^CT_OMIT_TARGET_VENDOR=/d' .config
echo 'CT_OMIT_TARGET_VENDOR=y' >> .config
# 清空 arch 后缀（armv7 -> arm）
sed -i -E 's/^CT_ARCH_SUFFIX=.*/CT_ARCH_SUFFIX=""/' .config
grep -q '^CT_ARCH_SUFFIX=' .config || echo 'CT_ARCH_SUFFIX=""' >> .config

# ---- 输出前缀 + 日志 ----
grep -q '^CT_PREFIX_DIR=' .config \
  && sed -i "s|^CT_PREFIX_DIR=.*|CT_PREFIX_DIR=\"$PREFIX\"|" .config \
  || echo "CT_PREFIX_DIR=\"$PREFIX\"" >> .config
echo 'CT_LOG_PROCESS_BARE=true' >> .config

echo "== 3b) 解析后的关键 .config 选择（供诊断）=="
grep -E '^CT_(GLIBC|CC_GCC|BINUTILS|LINUX)_V_[0-9]' .config || true
grep -E '^CT_(GLIBC|GCC|BINUTILS|LINUX|KERNEL)_VERSION=' .config || true
grep -E '^CT_OMIT_TARGET_VENDOR=' .config || true
grep -E '^CT_ARCH_SUFFIX=' .config || true
grep -E '^CT_TARGET_VENDOR=' .config || true
grep -E '^CT_TARGET=' .config || true

echo "== 3c) 校验配置（olddefconfig，提前暴露非法组合）=="
./crosstool-NG/ct-ng CT_LIB_DIR="$CTNG_DIR" olddefconfig
echo "olddefconfig OK"

echo "== 3d) 预置 crosstool-NG 源码 tarball（从官方 CDN 下载并校验 sha256）=="
# 已联网核实的关键事实：
#   * CI 用 GitHub 官方托管 runner (ubuntu-22.04)，具备完整公网，可直连 cdn.kernel.org / ftp.gnu.org。
#   * crosstool-NG 1.26.0 的 CT_DoFetch（scripts/functions:955）：CT_TARBALLS_DIR 里存在同名可读
#     文件就直接 return 0 复用，不下载、不校验本地文件。故把官方 tarball 放进去即可绕开下载。
#   * isl-0.26 的 crosstool-NG 自带镜像(gcc.gnu.org/.../isl-0.26.tar.xz)本构建可用，但为稳妥仍从
#     libisl.sourceforge.io 预置，规避偶发 404。
#   * 下方版本/sha256 与 armv7-rpi2 样本默认完全一致（已在 crosstool-NG 1.26.0 各
#     packages/<pkg>/<ver>/chksum 真源核实），下载后逐一校验 sha256，命中即由 CT_DoFetch 复用。
TB_DIR="$(pwd)/.build/tarballs"
mkdir -p "$TB_DIR" "${HOME}/.build/tarballs"
files=( linux-6.4.tar.xz glibc-2.17.tar.bz2 gcc-13.2.0.tar.xz binutils-2.40.tar.xz
        gmp-6.2.1.tar.xz mpfr-4.2.1.tar.xz mpc-1.2.1.tar.gz isl-0.26.tar.xz )
urls=(  https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.4.tar.xz
        https://ftp.gnu.org/gnu/glibc/glibc-2.17.tar.bz2
        https://ftp.gnu.org/gnu/gcc/gcc-13.2.0/gcc-13.2.0.tar.xz
        https://ftp.gnu.org/gnu/binutils/binutils-2.40.tar.xz
        https://ftp.gnu.org/gnu/gmp/gmp-6.2.1.tar.xz
        https://ftp.gnu.org/gnu/mpfr/mpfr-4.2.1.tar.xz
        https://ftp.gnu.org/gnu/mpc/mpc-1.2.1.tar.gz
        https://libisl.sourceforge.io/isl-0.26.tar.xz )
shas=( 8fa0588f0c2ceca44cac77a0e39ba48c9f00a6b9dc69761c02a5d3efac8da7f3
        80f5acd0bbc573ad80579ae98c789143c75f13fb39e4dbd2449c086774b8315c
        e275e76442a6067341a27f04c5c6b83d8613144004c0413528863dc6b5c743da
        0f8a4c272d7f17f369ded10a4aca28b8e304828e95526da482b0ccc4dfc9d8e1
        fd4829912cddd12f84181c3451cc752be224643e87fac497b69edddadc49b4f2
        277807353a6726978996945af13e52829e3abd7a9a5b7fb2793894e18f1fcbb2
        17503d2c395dfcf106b622dc142683c1199431d095367c6aacba6eec30340459
        a0b5cb06d24f9fa9e77b55fabbe9a3c94a336190345c2555f9915bb38e976504 )
for i in "${!files[@]}"; do
  f="${files[$i]}"; url="${urls[$i]}"; sha="${shas[$i]}"
  if [ -s "$TB_DIR/$f" ]; then echo "  $f 已存在，跳过"; continue; fi
  echo "  下载 $f ..."
  if curl -fsSL --retry 8 --retry-delay 5 --retry-all-errors -o "$TB_DIR/$f" "$url"; then
    got=$(sha256sum "$TB_DIR/$f" | cut -d' ' -f1)
    if [ "$got" = "$sha" ]; then
      cp -f "$TB_DIR/$f" "${HOME}/.build/tarballs/"
      echo "  OK $f (sha256 校验通过)"
    else
      echo "  WARN: $f sha256 不匹配 (got $got)，删除并交由 crosstool-NG 处理"
      rm -f "$TB_DIR/$f"
    fi
  else
    # 关键修复（run 31874292548 失败根因）：下载失败（如 cdn.kernel.org 瞬时
    # HTTP/2 PROTOCOL_ERROR）必须删除残缺文件，否则 crosstool-NG 的 CT_DoFetch
    # 误判已存在而跳过下载、直接解包残缺 tarball -> do_kernel_extract/CT_ZCat 失败。
    echo "  WARN: $f 下载失败，删除残缺文件并交由 crosstool-NG 回退上游下载"
    rm -f "$TB_DIR/$f"
  fi
done
# 把 tarball 目录钉死到我们刚填充的目录（覆盖任何默认差异，确保 CT_DoFetch 命中）
echo "CT_TARBALLS_DIR=\"$TB_DIR\"" >> .config

echo "== 4) 构建（耗时较长，可喝杯茶）=="
./crosstool-NG/ct-ng CT_LIB_DIR="$CTNG_DIR" build CT_JOBS="$JOBS"

echo "== 5) 产出与校验 =="
SYSROOT="$PREFIX/arm-linux-gnueabihf/sysroot"
ls -l "$SYSROOT/lib/libc.so.6" "$SYSROOT/usr/include/stdio.h"
echo
echo "# 校验 glibc 版本：必须含 2.17 且没有 >2.17"
strings "$SYSROOT/lib/libc.so.6" | grep -o 'GLIBC_2\.[0-9]*' | sort -V | uniq | tail -3
echo
echo "# 在 Makefile / 构建命令中使用："
echo "export PATH=\"$PREFIX/bin:\$PATH\""
echo "export CC=arm-linux-gnueabihf-gcc"
echo "export CFLAGS=\"--sysroot=$SYSROOT -march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -O2\""
echo
echo "DONE. 之后对每个产物跑 verify_target_abi.sh 做 ABI 门禁（EM_ARM / 0x5000400 / <=GLIBC_2.17）。"
