#!/bin/sh
# ============================================================================
# run.sh —— 一条命令把「原厂启动链」在 qemu 里跑起来（真实设备，非桩）。
#
#   U-Boot 的搬运职责 → qemu -kernel/-initrd/-append（等价替换，不是绕过）
#   kernel 以下全部真跑：kernel → busybox init → rcS → S80icube → icube → driver.so → 应用
#
# 用法:
#   sh run.sh [秒数]                       # 默认 60 秒
#   sh run.sh 120 --rootfs <other.sqsh>    # 换根文件系统（另一个设备/固件版本）
#   sh run.sh 120 --sdcard <dir>           # 换 SD 内容（要跑的应用放这里）
#   sh run.sh 120 --app <elf>              # 额外把应用塞进 SD 树的 cubegm/ 下
#
# 环境变量:
#   CGM_SIM_DIR   工作目录（默认 /tmp/cgmsim）
#   CGM_SIM_MEM   guest 内存 MB（默认 2048；initramfs 含全量模块时别低于 1024）
#   CGM_SIM_SECS  运行秒数
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
V="${CGM_SIM_DIR:-/tmp/cgmsim}"
# 可恢复小缓存（vmlinuz-lts 8MB + modloop-lts 49.8MB ≈ 58MB < CNB file-keeper 100MB 上限）：
#   CNB 云开发环境放 /workspace/.cgmsim-cache ⇒ file-keeper 自动备份/漫游, 环境回收重建后
#   自动恢复 ⇒ fetch_kernel/fetch_kmods 命中缓存秒过免重下；本机/无 /workspace 放 /tmp（重建重下）。
if [ -d /workspace ] && [ -w /workspace ]; then CACHE="/workspace/.cgmsim-cache"; else CACHE="/tmp/cgmsim-cache"; fi
mkdir -p "$CACHE"
SECS="60"; ROOTFS=""; SD=""; APP=""
while [ $# -gt 0 ]; do
  case "$1" in
    --rootfs) ROOTFS="$2"; shift 2 ;;
    --sdcard) SD="$2"; shift 2 ;;
    --app)    APP="$2"; shift 2 ;;
    *) SECS="$1"; shift ;;
  esac
done
[ -n "${CGM_SIM_SECS:-}" ] && SECS="$CGM_SIM_SECS"
MEM="${CGM_SIM_MEM:-2048}"
[ -n "$ROOTFS" ] || ROOTFS="$HERE/assets/rootfs.sqsh"
[ -n "$SD" ]     || SD="$HERE/assets/sdcard"
mkdir -p "$V"

echo "########## 1) 主机依赖 ##########"
if ! command -v qemu-system-arm >/dev/null 2>&1 || ! command -v unsquashfs >/dev/null 2>&1; then
  apt-get update -qq > "$V/apt.log" 2>&1
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
    qemu-system-arm squashfs-tools cpio >> "$V/apt.log" 2>&1
  echo "  apt rc=$?（详情 $V/apt.log）"
fi
for c in qemu-system-arm unsquashfs cpio gzip; do printf "  %-18s %s\n" "$c" "$(command -v $c 2>/dev/null || echo 缺)"; done

echo "########## 2) 内核（通用 ARMv7）##########"
sh "$HERE/scripts/fetch_kernel.sh" "$V/vmlinuz" "$CACHE" 2>&1 | sed 's/^/  /'

echo "########## 3) 解 rootfs ##########"
if [ ! -d "$V/rootfs/bin" ]; then
  rm -rf "$V/rootfs"
  unsquashfs -q -no-xattrs -d "$V/rootfs" "$ROOTFS" > "$V/unsq.log" 2>&1
  echo "  rc=$? 条目=$(find "$V/rootfs" 2>/dev/null | wc -l)（dev/console 建不出是正常的）"
fi

echo "########## 4) 真实内核模块 ##########"
# ★ 必须放在「解 rootfs」之后（PITFALLS #11）：
#   旧顺序 fresh 环境首次运行时，解 rootfs 的 rm -rf "$V/rootfs" 会把
#   上一步刚放进 $V/rootfs/lib/modules 的 2449 个 .ko 全部删掉 ⇒
#   S00cgmmod 的 modprobe virtio_gpu/snd_dummy 无模块可载 ⇒
#   /dev/dri 为空 ⇒ 原厂 rkgame 报 "cannot find/open a drm device"。
sh "$HERE/scripts/fetch_kmods.sh" "$V/rootfs/lib/modules" "$CACHE" 2>&1 | sed 's/^/  /'

echo "########## 5) 注入加载脚本 + 应用 ##########"
cat > "$V/rootfs/etc/init.d/S00cgmmod" <<'EOS'
#!/bin/sh
# S00cgmmod —— ★ 本项目新增的初始化脚本（不是原厂文件；红线要求绝不改原厂文件）。
#
# 为什么用「新增 S?? 脚本」这条路：
#   busybox 的 /etc/init.d/rcS 是 `for i in /etc/init.d/S??*; do $i start; done`
#   ⇒ 只要文件名排序在前面（S00 < S01），就会**先于**原厂脚本执行，且**零改动**原厂文件。
#
# ★ 为什么不用 export LD_PRELOAD 注入全系统：
#   rcS 里每个脚本都是**子进程**（`$i start`）或**子 shell**（`. $i`），
#   环境变量**传不到**后面的脚本。要给全系统注入只能用 /etc/ld.so.preload（glibc 系统级）。
#
# 本脚本的目的：给 guest 提供**真实设备**（真实内核驱动），而不是伪造成功。
log() { echo "cmdmod: $*"; }
ld() { modprobe "$1" 2>/dev/null; }

case "$1" in
  start)
    mount -t devtmpfs devtmpfs /dev 2>/dev/null
    mkdir -p /dev/dri /dev/snd /dev/input

    # --- 图形：真实 DRM（virtio-gpu，需 qemu 侧 -device virtio-gpu-device）---
    ld virtio_mmio          # qemu 的 virtio-mmio 总线（-device virtio-*-device 挂它上面）
    ld drm
    ld drm_kms_helper
    ld drm_shmem_helper
    ld virtio_gpu

    # --- 音频：真实 ALSA（snd-dummy 是内核自带的虚拟声卡驱动）---
    ld snd
    ld snd_pcm
    ld snd_dummy

    # --- 输入：uinput + joydev（/dev/input/jsN 需要再触发才有节点）---
    ld uinput
    ld joydev

    log "--- /dev/dri ---";        ls -l /dev/dri 2>/dev/null
    log "--- /dev/snd ---";        ls   /dev/snd 2>/dev/null
    log "--- /sys/class/drm ---";  ls   /sys/class/drm 2>/dev/null
    ;;
esac
exit 0

EOS
chmod +x "$V/rootfs/etc/init.d/S00cgmmod"
if [ -n "$APP" ] && [ -f "$APP" ]; then
  cp -a "$APP" "$V/rootfs/mnt/sdcard/cubegm/" 2>/dev/null || { mkdir -p "$V/rootfs/mnt/sdcard/cubegm"; cp -a "$APP" "$V/rootfs/mnt/sdcard/cubegm/"; }
  chmod +x "$V/rootfs/mnt/sdcard/cubegm/$(basename "$APP")"
fi
sh "$HERE/scripts/mk_initramfs.sh" "$V/rootfs" "$V/initramfs.cpio.gz" "$SD" 2>&1 | sed 's/^/  /'

echo "########## 6) 启动（${SECS}s）##########"
echo "  设备：-global virtio-mmio.force-legacy=false -device virtio-gpu-device（真实 DRM）+ snd-dummy（真实声卡）"
set +e
timeout "$SECS" qemu-system-arm -M virt -cpu cortex-a7 -m "$MEM" \
  -display none -serial stdio -monitor none -no-reboot -net none \
  -global virtio-mmio.force-legacy=false \
  -device virtio-gpu-device \
  -kernel "$V/vmlinuz" -initrd "$V/initramfs.cpio.gz" \
  -append "console=ttyAMA0 rdinit=/sbin/init loglevel=7" > "$V/boot.log" 2>&1
echo "  qemu rc=$?（124=超时未退，正常）  行数=$(wc -l < "$V/boot.log")"

echo "########## 7) 启动链判定 ##########"
hit() { c=$(grep -acE "$2" "$V/boot.log" 2>/dev/null); [ "$c" -gt 0 ] && printf "  ✓ [%2d] %s\n" "$c" "$1" || printf "  · [ 0] %s\n" "$1"; }
hit "内核起来"            "Linux version [0-9]"
hit "initramfs 解包成功"  "Run /sbin/init as init process"
hit "rcS 跑起来"          "Starting logging|Starting network"
hit "★ 真实 DRM 设备"     "Initialized virtio_gpu"
hit "★ 真实声卡"          "controlC|pcmC0D0"
hit "原厂启动脚本"        "Starting icube|S80"
hit "应用被拉起"          "open driver.so sucess|rkgame v|RetroArch"
hit "应用主循环"          "MemFree|MemTotal"
hit "失败/异常"           "failed ret=|cannot find|Segmentation|corruption|abort"
echo
echo "  --- 去重后的关键段 ---"
grep -aE "cmdmod: ---|---|Starting |open driver.so|rkgame v|DRM_IOCTL|failed ret|Initialized virtio_gpu|MemTotal" \
  "$V/boot.log" 2>/dev/null | awk '!seen[$0]++' | head -30 | sed 's/^/    /'
echo
echo "  --- 末 15 行 ---"
tail -15 "$V/boot.log" | sed 's/^/    /'
echo "  完整日志：$V/boot.log"
echo "########## DONE ##########"
