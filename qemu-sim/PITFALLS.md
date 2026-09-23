# 已踩的坑（**接手前必读**，每条都给了源码级根因）

## 1. ★★★★★ `/dev/dri` 永远不出现：virtio-mmio 默认 legacy

**症状**：`-device virtio-gpu-device` 后，guest 里 `/sys/bus/virtio/devices/virtio0` **存在**且
`device=0x0010`（GPU ✓），但 `/proc/modules` 里 `virtio_gpu 73728 0`（**引用计数恒为 0**），
`/sys/class/drm` 空、`/dev/dri` 不存在，**内核日志里没有任何 probe 消息**（因为驱动根本没被 probe）。

**根因（源码级）**：
```
qemu  hw/virtio/virtio-mmio.c:712   只有 proxy->legacy == false 时才加 VIRTIO_F_VERSION_1
qemu  hw/virtio/virtio-mmio.c:722   默认 proxy->legacy = true
linux drivers/gpu/drm/virtio/virtgpu_kms.c:110
       if (!virtio_has_feature(dev_to_virtio(dev->dev), VIRTIO_F_VERSION_1)) return -ENODEV;
```
（旧内核还会在 release 路径上空指针崩溃。）

**修法**：`-global virtio-mmio.force-legacy=false`
**一眼判据**：`/sys/bus/virtio/devices/virtio0/status` 从 `0x83` → **`0x0f`**（ACK|DRIVER|FEATURES_OK|DRIVER_OK）。
★ 通用：ARM `virt` 上任何 `-device virtio-*-device`（mmio 版）都要带这个 `-global`；`-pci` 版不受影响。

## 2. ★★★★★ `rootfs image is not initramfs (broken padding)` → `VFS: Unable to mount root`

**症状链**：
```
Trying to unpack rootfs image as initramfs...
rootfs image is not initramfs (broken padding); looks like an initrd
RAMDISK: gzip image found at block 0
RAMDISK: incomplete write (5397 != 18559)
Kernel panic - not syncing: VFS: Unable to mount root fs on "" or unknown-block(0,0)
```
**根因**：cpio **newc** header 是 **110 字节**，而 `110 % 4 == 2` ⇒ name 字段的 padding
必须**相对整个 cpio 流**对齐。（自写打包器若按"相对字段"对齐，会差 2 字节。）
padding 错了，内核就认为不是 initramfs，退回旧式 initrd 路径 ⇒ 往 4 MB 的 `/dev/ram0` 写 19 MB ⇒ 失败。

**修法**：**必须用系统 `cpio`**：
```sh
( cd rootfs && find . -print0 | cpio --null -o -H newc --quiet | gzip -6 > initramfs.cpio.gz )
```

## 3. ★★★★ `failed to find romfile "efi-virtio.rom"` ⇒ qemu 直接退出

**根因**：qemu 的 virtio-net option ROM 缺失；**Ubuntu 的 `qemu-system-data` 包里并没有它**（实测 `find / -name 'efi-virtio.rom'` 为空）。
**修法**：`-net none`（不需要网络就不用给 virtio-net 留位置）。

## 4. ★★★★ 内核完全无输出 ⇒ 你以为"内核没启动"

**根因**：用了 `-nodefaults`。ARM `virt` 的 PL011 串口是**依赖 `-serial` 参数实例化**的；
`-nodefaults` 把默认串口也去掉了 ⇒ 内核照样跑，但没有任何控制台输出。
**修法**：**不要用 `-nodefaults`**；用 `-display none -serial stdio -monitor none`。

## 5. ★★★★ 换成原厂 `zImage` 后零输出

**根因**：原厂内核面向 **RK3036**，不支持 `linux,dummy-virt` / `pl011` / `arch_timer`
（在 `assets/kernel.zImage` 里 grep 这些串全为空，而有 `rk3036`/`rk30`）。
**修法**：用通用 ARMv7 内核（Alpine `vmlinuz-lts`）。
★ 注意文件名：**`vmlinuz-lts`**；`vmlinuz-virt` 会 404。

## 6. ★★★ `vkms` 用不了（别在这上面浪费时间）

发行版 lts 内核常把 `vkms` 关掉。实测 Alpine 6.12.110-0-lts：
```
# CONFIG_DRM_VKMS is not set      ← 不可用
CONFIG_DRM_VIRTIO_GPU=m           ← 用这个
```
**先查 kernel config 再选设备**，别按印象选（config 可从 netboot 目录直接下载）。

## 7. ★★★ `/dev/mem` 到底能不能访问外设寄存器

`arch/arm/mm/mmap.c` 的 `devmem_is_allowed()`：
```c
if (iomem_is_exclusive(pfn << PAGE_SHIFT)) return 0;
if (!page_is_ram(pfn)) return 1;   /* ★ 非 RAM（外设 MMIO）⇒ 允许 */
return 0;                          /* RAM ⇒ 拒绝 */
```
⇒ `CONFIG_STRICT_DEVMEM=y` **只禁止 RAM，非 RAM 的外设区是允许的**。
**所以 `mmap(0x20000000)`（RK CRU/GRF）在内核侧是合法的** —— 前提是 **qemu 在该物理地址有映射**。
qemu 未映射的地址访问会变成 bus error。
**⇒ 要支持 RK 外设寄存器，只需在 qemu 侧挂一块 MMIO 窗口，不必重编内核。**

## 8. ★★★ CNB 云开发环境闲置 3–5 分钟即回收

**症状**：ssh 突然 `Permission denied`；`/tmp` 被重置（我一度误判为"容器重建"）。
**对策**：所有步骤"**一次 ssh 跑完**"；不要 `setsid` 后台 + 反复轮询（轮询间隙就没了）。
`run.sh` 已按此结构写。

## 9. ★★ 其它小坑

- `zig cc -target arm-linux-gnueabihf.X` **不支持 `-static`**（报 `libc of the specified target requires dynamic linking`）；
  要静态就用 `-target arm-linux-musleabihf`。
- 自写 C 程序忘 `#include <sys/mman.h>` ⇒ `mmap/munmap/MAP_FAILED`/`PROT_*` 全部 `undeclared`。
- `rcS` 里每个脚本是**子进程**（`$i start`）或**子 shell**（`. $i`）⇒ **`export` 传不到后续脚本**；
  要全系统注入只能用 `/etc/ld.so.preload`。
- 容器内通常**无 `CAP_MKNOD`** ⇒ 不能在 initramfs 里手工做 `/dev/console` 等设备节点；
  交给内核 `devtmpfs` 自动补（`CONFIG_DEVTMPFS_MOUNT`）。
