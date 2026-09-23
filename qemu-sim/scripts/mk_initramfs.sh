#!/bin/sh
# mk_initramfs.sh —— 组 initramfs 并保证关键条件成立。
#
# ★★ 必须用**系统 cpio**，不要手写 cpio 打包器 ★★
#   newc header 是 110 字节（110 % 4 == 2），name 字段的 padding 必须**相对整个 cpio 流**对齐。
#   按"相对字段"对齐会差 2 字节 ⇒ 内核报
#       rootfs image is not initramfs (broken padding); looks like an initrd
#   于是退回旧式 initrd 路径 ⇒ 往 4MB 的 /dev/ram0 写 19MB ⇒
#       RAMDISK: incomplete write (5397 != 18559) ⇒ VFS: Unable to mount root fs
#   （这是本项目实际踩过的坑，代价是一整轮排查。）
#
# 用法: sh scripts/mk_initramfs.sh <rootfs_dir> <out.cpio.gz> [sdcard_dir]
set -u
R="${1:?usage: mk_initramfs.sh <rootfs_dir> <out.cpio.gz> [sdcard_dir]}"
OUT="${2:?}"
SD="${3:-}"
[ -d "$R" ] || { echo "  ★ rootfs 目录不存在: $R"; exit 11; }

# 原厂 rootfs 里 /sdcard -> mnt/sdcard（真机挂载点），所以 SD 内容要落到 mnt/sdcard/
mkdir -p "$R/proc" "$R/sys" "$R/tmp" "$R/run" "$R/dev/pts" "$R/dev/shm" "$R/mnt/sdcard/cubegm"
if [ -n "$SD" ] && [ -d "$SD" ]; then
  cp -a "$SD/." "$R/mnt/sdcard/cubegm/" 2>/dev/null
fi
chmod +x "$R/mnt/sdcard/cubegm/"* 2>/dev/null

( cd "$R" && find . -print0 | cpio --null -o -H newc --quiet 2>/dev/null | gzip -6 > "$OUT" )
echo "  initramfs: $OUT $(stat -c%s "$OUT") B"
echo "  提示：/dev/console 等设备节点由内核 devtmpfs 自动补，不必手工创建（容器里通常无 CAP_MKNOD）。"
