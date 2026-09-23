# 全量环境参数

> 每个参数后面写的是**为什么**，不是"照抄就行"。换环境时按这些理由判断能不能改。

## 1. 主机侧

| 项 | 值 | 说明 |
|---|---|---|
| qemu | `qemu-system-arm`（Debian 13 实测 **10.0.13**） | 只要支持 `virt` + `virtio-gpu-device` 即可 |
| 依赖包 | `qemu-system-arm squashfs-tools cpio` | `--no-install-recommends` 足够；**不要**装 `qemu-system-data`（不含 `efi-virtio.rom`，装了也没用） |
| 网络 | 需可达 `dl-cdn.alpinelinux.org` | 取内核与模块 |
| 磁盘 | ≥ 500 MB | modloop 49.8 MB + 解包后模块目录 |

## 2. guest 侧（内核与模块 —— 必须同版本同构建）

| 项 | 值 |
|---|---|
| 内核 | Alpine armv7 netboot **`vmlinuz-lts`**（实测 6.12.110-0-lts，8,028,672 B） |
| 模块全集 | Alpine **`modloop-lts`**（squashfs，48,832,512 B） |
| 内核关键配置（实测） | `CONFIG_MODULES=y`、`CONFIG_DRM=m`、`CONFIG_DRM_VIRTIO_GPU=m`、`CONFIG_SND_DUMMY=m`、`CONFIG_SND_ALOOP=m`、`CONFIG_INPUT_UINPUT=m`、`CONFIG_INPUT_JOYDEV=m`、`CONFIG_DEVMEM=y`、`CONFIG_STRICT_DEVMEM=y` |
| **注意** | `# CONFIG_DRM_VKMS is not set` ⇒ **`vkms` 不可用**，别走这条路。`CONFIG_DRM_VIRTIO_GPU=y` 才是正解 |
| 内核来源 URL | `https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/armv7/netboot/`（**文件名是 `vmlinuz-lts`**；`vmlinuz-virt` 会 404） |
| 原厂内核 | **不要用**。`assets/kernel.zImage` 是原厂 zImage（ARM zImage ✓、4,308,440 B），但它面向 RK3036，**在 qemu virt 上零输出**（无 `dummy-virt`/`pl011`/`arch_timer` 支持）。保留它只为留证。 |

## 3. QEMU 命令行（逐项）

```sh
qemu-system-arm -M virt -cpu cortex-a7 -m 2048 \
  -display none -serial stdio -monitor none -no-reboot -net none \
  -global virtio-mmio.force-legacy=false \
  -device virtio-gpu-device \
  -kernel <vmlinuz> -initrd <initramfs.cpio.gz> \
  -append "console=ttyAMA0 rdinit=/sbin/init loglevel=7"
```

| 参数 | 为什么 |
|---|---|
| `-M virt` | RK3036 是 Cortex-A7/ARMv7；`virt` 是 qemu 上唯一能跑通用 ARMv7 内核的通用机器。**没有 RK3036 机器模型**（qemu 官方无 Rockchip；`rockchip-linux` 只有 kernel/u-boot/rkbin） |
| `-cpu cortex-a7` | 与真机同核（实测内核回显 `CPU: ARMv7 Processor [410fc075]`）。不用默认的 cortex-a15，减少差异 |
| `-m 2048` | initramfs 含全量模块，解压占用大；**低于 1024 可能 OOM** |
| `-display none -serial stdio` | 无图形输出，串口走 stdio（HDMI 输出的设备本来就无屏幕） |
| `-monitor none` | **必须**。否则 monitor 抢 stdin，在非交互 ssh 会话里会导致输出截断/会话早退 |
| `-no-reboot` | 进程崩溃时不要无限重启掩盖现场 |
| `-net none` | **必须**。否则 qemu 去找 `efi-virtio.rom`（Ubuntu 的 `qemu-system-data` 并不提供）⇒ `failed to find romfile` 直接退出 |
| **`-global virtio-mmio.force-legacy=false`** | **★ 最关键的一条**。qemu `hw/virtio/virtio-mmio.c:712` 只在 `legacy==false` 时加 `VIRTIO_F_VERSION_1`，而 `:722` 默认 `true`；内核 `virtgpu_kms.c:110` 检查该 feature 不通过就 `-ENODEV` 拒绝加载驱动。**判据**：`/sys/bus/virtio/devices/virtio0/status` 从 `0x83` → **`0x0f`** |
| `-device virtio-gpu-device` | mmio 版 virtio-gpu ⇒ 内核 `virtio_gpu` 驱动创建 **真实** `/dev/dri/card0`（major 226）。也可用 `-device virtio-gpu-pci`（需 PCIe 与 `pci` 模块） |
| `-append rdinit=/sbin/init` | **原厂 init**（busybox 的 `/sbin/init`，读原厂 `/etc/inittab`）。不要改成别的 —— 启动链要真 |
| `console=ttyAMA0` | virt 的 PL011 串口（`arm,pl011`，内核自动实例化）。**不要配 `-nodefaults`**，否则串口不被创建、内核完全无输出 |

## 4. guest 内部：设备是怎么出来的

```
/etc/init.d/S00cgmmod     ← 本项目新增（不是原厂文件）
    mount -t devtmpfs devtmpfs /dev
    modprobe virtio_mmio drm drm_kms_helper drm_shmem_helper virtio_gpu
    modprobe snd snd_pcm snd_dummy uinput joydev
```
**为什么是"新增 S?? 脚本"而不是改原厂脚本**：busybox `rcS` 是
`for i in /etc/init.d/S??*; do $i start; done` ⇒ 文件名排序靠前就会先执行，**零改动原厂文件**。

**为什么不用 `export LD_PRELOAD` 注入全系统**：`rcS` 里每个脚本都是**子进程**（`$i start`）
或**子 shell**（`. $i`），环境变量**传不到**后续脚本；要给全系统注入只能用 `/etc/ld.so.preload`。

## 5. 期望的启动时间线（实测）

| 时刻 | 事件 |
|---|---|
| 0.30 s | `Trying to unpack rootfs image as initramfs...` |
| 0.83 s | `Run /sbin/init as init process` |
| ~1 s | rcS 各脚本 → `Starting icube:` → 应用开始跑 |

## 6. 已知的 CNB 云开发环境约束（若在 CNB workspace 里跑）

**环境闲置约 3–5 分钟即被回收**（表现为 ssh `Permission denied`、`/tmp` 被重置）。
⇒ **所有步骤必须"一次 ssh 跑完"**，不要 `setsid` 后台跑再反复 ssh 轮询 —— 轮询间隙环境就没了。
`run.sh` 已是"一次跑完"的结构（装依赖→取件→组装→启动→判定，全在一个进程里）。
