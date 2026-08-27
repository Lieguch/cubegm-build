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

if [ -f "$OUT" ]; then
    log "already in sysroot ($OUT) -- skip"
    exit 0
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
log "installed: $SYSROOT/usr/include/libudev.h + $SYSROOT/usr/lib/libudev.so.1 ($(stat -c%s "$OUT" 2>/dev/null || ls -la "$OUT" | awk '{print $5}') bytes)"
