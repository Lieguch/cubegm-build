#!/bin/sh
# install_replace_icube.sh — 把启动方式切到「替换 icube 事前接管」（最终实施方案 §一.1）
#
# 背景：原厂启动链 S80icube 会执行 /sdcard/cubegm/icube（fork rkgame + driver.so）。
# 替换 icube 后，S80icube 直接执行我们的 icube_replacement → fork cubevol_bridge +
# exec picoarch（DRM/ALSA 自初始化），原厂 rkgame/driver.so 永远不会启动，根治半白屏/无声。
#
# 用法（在设备上，或 PC 上挂载 TF 卡到 cubegm/ 目录执行）：
#   cd /mnt/sdcard/cubegm && sh install_replace_icube.sh
#
# 安全性：
#   - 自动备份原 icube → icube.orig（可回滚）
#   - 原 zhijack（autorun → zhijack.sh）作为回退路径保留，未被删除
#   - 回滚：mv icube.orig icube

set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

if [ ! -f ./icube_replacement ]; then
    echo "错误：找不到 icube_replacement（应在本目录）。" >&2
    exit 1
fi

# 1. 备份原厂 icube（仅首次，避免覆盖已有备份）
if [ -f ./icube ] && [ ! -f ./icube.orig ]; then
    cp -f ./icube ./icube.orig
    echo "已备份原厂 icube → icube.orig"
fi

# 2. 用替换版覆盖 icube
cp -f ./icube_replacement ./icube
chmod +x ./icube
echo "已安装替换版 icube（事前接管）。重启设备生效。"

# 3. 提示回滚
echo "回滚方法：cd /mnt/sdcard/cubegm && cp -f icube.orig icube"