#!/usr/bin/env bash
# build_libudev_zero.sh — 交叉编译 libudev-zero 进 sysroot
# =============================================================================
# 目的：RetroArch udev 输入驱动（input_driver="udev"）需要 libudev 客户端库。
#   libudev-zero = daemonless 的 libudev 替代（Alpine 官方打包），无 udevd 守护
#   进程也能枚举 /sys + 自算 ID_INPUT_* 属性 + netlink 热插拔。
#   ABI：libudev.so.1，标准 LIBUDEV_183~247 版本符号，与 systemd libudev 二进制兼容。
# 选型依据（源码级查证，2026-08-27）：
#   - eudev：Gentoo 2021 废弃 / 2023 停更（v3.2.14 后零发布，31 issue 无人处理）→ 排除。
#   - systemd libudev：老 glibc-2.29 sysroot 交叉编译工程风险高 → 排除。
#   - libudev-zero：纯 C 零依赖，make CC= 即可交叉编译，符号 100% 覆盖
#     RetroArch udev_input.c / udev_joypad.c 全部调用（已逐符号核对）。
# 交叉编译：make CC=$CROSS_COMPILEgcc（CFLAGS/LDFLAGS 已含 --sysroot）
# 幂等：sysroot 已有 libudev.so.1 则跳过。
# =============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
: "${SYSROOT:?SYSROOT required}"
: "${CC:?CC required}"
CFLAGS="${CFLAGS:-}"
LDFLAGS="${LDFLAGS:-}"

log(){ printf '\033[1;32m[build:libudev]\033[0m %s\n' "$*"; }
err(){ printf '\033[1;31m[build:libudev ERROR]\033[0m %s\n' "$*" >&2; }
die(){ err "$*"; exit 1; }

SRC="$HERE/libudev-zero"
OUT="$SYSROOT/usr/lib/libudev.so.1"

# v11.2: 缓存逻辑修正。旧版 [ -f "$OUT" ] 直接跳过 —— 若 sysroot 里已有
# 旧版 libudev.so.1（未含 v11.2 JOYSTICK 判定补丁），CI 会静默跳过编译，
# 手柄补丁永远不生效。改为时间戳比较：任何源文件比输出新则强制重编。
if [ -f "$OUT" ] && [ "$SRC/udev_device.c" -ot "$OUT" ] \
   && [ "$SRC/udev.c" -ot "$OUT" ] && [ "$SRC/Makefile" -ot "$OUT" ]; then
    log "libudev.so.1 up to date (source not newer) -- skip"
    exit 0
fi
if [ -f "$OUT" ]; then
    log "source changed (udev_device.c/udev.c newer) -- force rebuild"
fi

[ -f "$SRC/udev.c" ] || die "libudev-zero source missing at $SRC (should be vendored)"
[ -f "$SRC/Makefile" ] || die "libudev-zero Makefile missing"

log "Cross-building libudev-zero (CC=$CC) -> $SYSROOT/usr/lib ..."
make -C "$SRC" clean >/dev/null 2>&1 || true
make -C "$SRC" CC="$CC" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" libudev.so.1 || \
    die "libudev-zero build FAILED (see errors above)"

mkdir -p "$SYSROOT/usr/include" "$SYSROOT/usr/lib"
cp -f "$SRC/udev.h"        "$SYSROOT/usr/include/libudev.h"
cp -f "$SRC/libudev.so.1"  "$SYSROOT/usr/lib/libudev.so.1"
ln -sf libudev.so.1        "$SYSROOT/usr/lib/libudev.so"
# ★ libudev.pc (run 36591107267 root cause): libudev-zero 上游自带 libudev.pc.in，
#   Makefile 提供 install-pkgconfig target 生成/安装 libudev.pc（@prefix@ 等占位
#   sed 填充）。旧脚本只手动 cp 了 libudev.h + libudev.so.1，导致 pkg-config 查
#   不到 libudev → RetroArch qb check ("Checking presence of package libudev ... no")
#   → "Forced to build with package libudev, but cannot locate" → configure 失败。
#   按上游 Makefile 同一逻辑生成（PREFIX=$SYSROOT/usr → pc 里 prefix 为绝对
#   sysroot 路径；libdir/includedir 转 ${exec_prefix}/${prefix} 相对展开）。
#   PKG_CONFIG_SYSROOT_DIR 已禁用（见 build_mesa_lima.sh 同因注释），不会再双叠。
make -C "$SRC" libudev.pc PREFIX="$SYSROOT/usr" \
    LIBDIR="$SYSROOT/usr/lib" INCLUDEDIR="$SYSROOT/usr/include" >/dev/null 2>&1 || {
        # 上游 make 目标不可用时兜底：按 libudev.pc.in 模板直接生成
        PKGCONFIG_PREFIX="$SYSROOT/usr" \
        exec_prefix="$SYSROOT/usr" libdir='${exec_prefix}/lib' includedir='${prefix}/include' \
        sed -e 's|@prefix@|'"$SYSROOT/usr"'|g' \
            -e 's|@exec_prefix@|'"$SYSROOT/usr"'|g' \
            -e 's|@libdir@|${exec_prefix}/lib|g' \
            -e 's|@includedir@|${prefix}/include|g' \
            -e 's|@VERSION@|251|g' \
            "$SRC/libudev.pc.in" > "$SRC/libudev.pc"
    }
mkdir -p "$SYSROOT/usr/lib/pkgconfig"
cp -f "$SRC/libudev.pc" "$SYSROOT/usr/lib/pkgconfig/libudev.pc"
log "installed: $SYSROOT/usr/include/libudev.h + $SYSROOT/usr/lib/libudev.so.1 + $SYSROOT/usr/lib/pkgconfig/libudev.pc ($(stat -c%s "$OUT" 2>/dev/null || ls -la "$OUT" | awk '{print $5}') bytes)"
