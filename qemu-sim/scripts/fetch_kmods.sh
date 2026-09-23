#!/bin/sh
# fetch_kmods.sh —— 取「同版本内核」的真实模块集（DRM/声卡/输入），供沙箱使用。
#
# 为什么需要：真机 rootfs 里**没有** /lib/modules（原厂内核把这些驱动 built-in）。
# 本沙箱用 **与内核同版本同构建** 的 Alpine lts 模块，让 rkgame 面对的是
# **真实的内核驱动**（virtio-gpu / snd-dummy / joydev…），而不是被伪造成功。
#
# 用法（在 bootchain.sh 内部调用）: sh tools/fetch_kmods.sh <destdir>
set -u
DEST="${1:?usage: fetch_kmods.sh <destdir>}"
NB="https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/armv7/netboot"
KV="6.12.110-0-lts"
mkdir -p "$DEST"
if [ ! -d "$DEST/$KV" ]; then
  echo "  下载 modloop-lts（Alpine $KV 模块全集）..."
  curl -sfL --max-time 300 -o /tmp/modloop-lts "$NB/modloop-lts" || { echo "  ★ modloop 下载失败"; exit 11; }
  echo "  modloop $(stat -c%s /tmp/modloop-lts) B"
  rm -rf /tmp/ml; mkdir -p /tmp/ml
  unsquashfs -q -no-xattrs -d /tmp/ml /tmp/modloop-lts >/dev/null 2>&1 || { echo "  ★ 解包失败"; exit 12; }
  SRC=$(find /tmp/ml -maxdepth 3 -type d -name "$KV" | head -1)
  [ -n "$SRC" ] || { echo "  ★ 找不到 $KV 模块目录"; exit 13; }
  cp -a "$SRC" "$DEST/$KV"
  # modloop 的模块目录名是 modules/<ver>，Alpine 里 /lib/modules 指向它
  echo "  已放入 $DEST/$KV（$(find "$DEST/$KV" -name '*.ko' | wc -l) 个 .ko）"
fi
ls "$DEST/$KV" | head -6 | sed 's/^/    /'
