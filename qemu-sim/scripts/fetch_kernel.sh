#!/bin/sh
# fetch_kernel.sh —— 取通用 ARMv7 内核（可跑 qemu virt 的 machine）
# 为什么不用原厂 zImage：原厂内核是 RK3036 专用（无 dummy-virt / pl011 / arch_timer 支持），
# 在 qemu virt 上**零输出**。本脚本取 Alpine armv7 lts 内核，与 fetch_kmods.sh 的模块**同版本同构建**。
# 用法: sh scripts/fetch_kernel.sh <outfile>
set -u
OUT="${1:?usage: fetch_kernel.sh <outfile>}"
NB="https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/armv7/netboot"
[ -s "$OUT" ] && { echo "  已存在 $OUT ($(stat -c%s "$OUT") B)"; exit 0; }
if curl -sfL --max-time 180 -o "$OUT.try" "$NB/vmlinuz-lts"; then
  mv "$OUT.try" "$OUT"
  echo "  OK $OUT $(stat -c%s "$OUT") B  [0x24]=$(od -An -tx1 -j36 -N4 "$OUT"|tr -d ' ')（期望 18286f01 = ARM zImage）"
else
  echo "  ★ 下载失败（注意：文件名是 vmlinuz-lts，不是 vmlinuz-virt —— 后者 404）"; exit 11
fi
