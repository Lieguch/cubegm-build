# qemu-sim —— RK3036G 原厂启动链的 QEMU 模拟仿真系统

> **一句话**：不插卡、不开真机，就用 QEMU 把原厂启动链**跑起来**（可无限重跑，全链路约 1 秒）。
> 面向：接手本设备相关项目的执行 Agent。
> 权威性：本目录的设备资料**全部从原厂 `org.bin` 提取**，是 1:1 复刻方向的权威来源。

---

## 0. 先看这一节：与本仓库既有描述的**一处冲突**

本仓库 `PROJECT_CHARTER.md`/`00_START_HERE.md` 写「目标 ABI glibc **≤ 2.17**」。
本套件从**原厂 `org.bin` 的 rootfs 分区**实测得到的是 **glibc 2.29**，证据如下（可自行复核）：

```
rootfs 解包后：
  lib/ld-2.29.so              158,640 B
  lib/libc-2.29.so          1,242,892 B
libc-2.29.so 内的版本串：
  "GNU C Library (Buildroot) stable release version 2.29."
  "Copyright (C) 2019 Free Software Foundation, Inc."
```

**结论：设备侧 glibc = 2.29。** 请接手方以本套件为准，并据此复核 `verify_target_abi.sh` 里的上限断言。
（若坚持 2.17，需要用**同一条**证据链说明来源；本套件的证据是 org.bin 分区内容 + 版本串，可逐字节复核。）

---

## 1. 它到底怎么"真"（而不是打桩）

| 层 | 真机 | 本模拟 | 保真度说明 |
|---|---|---|---|
| U-Boot | 把 kernel/rootfs/DTB 搬到内存 + 按 cmdline 交接 | `-kernel/-initrd/-append/-dtb` | **职责等价替换**（U-Boot 直接操作 RK3036 寄存器，在 qemu 上必挂；它的唯一职责就是搬运） |
| kernel 以下 | 全部真 | **全部真**：真 kernel、真 init、真 rcS、真原厂脚本、真 dlopen、真应用 | 1:1 |
| DRM | RK VOP + HDMI | **内核 `virtio_gpu` 真实驱动 + `/dev/dri/card0`(major 226)** | 真实内核驱动，非桩；但**不是 RK VOP**，能力集不同（见 §5） |
| ALSA | RK I2S/ACODEC | **内核 `snd-dummy` 真实声卡**（`/dev/snd/controlC0`、`pcmC0D0p`） | 真实内核驱动，非桩 |
| `/dev/mem` + RK GRF/GPIO | 真寄存器 | **暂缺**（qemu 未在该物理地址提供 MMIO 窗口） | 已知缺口，见 §5 |

**明确不做的事**：不用 `LD_PRELOAD` 伪造「硬件初始化成功」。
（用户口径：不要补丁、不要假桩、不要假实现 —— 桩会让「等价判定」变成「同环境比两个都被骗过的实现」。）

---

## 2. 一条命令跑起来

```sh
cd qemu-sim
sh run.sh 60                 # 跑 60 秒，把完整日志与 9 条里程碑判定打到屏幕
```

**成功长这样**（节选，真实输出）：

```
cmdmod: --- /dev/dri ---
  crw-------  1 root root 226, 0  card0
Starting logging: OK
Starting network: OK
Starting icube:
open driver.so sucess
video_driver_setting 0 1 1
open drm!
rkgame v1.42
MemTotal: 2056632 kB
```

**判据（9 条，`run.sh` 自动打）**：内核起来 → initramfs 解包 → rcS 跑起来 → **真实 DRM 设备** →
**真实声卡** → 原厂启动脚本 → 应用被拉起 → **应用主循环** → 失败/异常。

**换目标程序**：
```sh
sh run.sh 120 --app /path/to/your/app         # 塞进 SD 树 cubegm/
sh run.sh 120 --sdcard /path/to/sd_root       # 整棵 SD 树替换
```

---

## 3. 目录

```
qemu-sim/
├── README.md          ← 本文件（先读这个）
├── ENVIRONMENT.md     ← 全量环境参数（qemu 每一参数的"为什么"+ 版本锁定 + 判据）
├── ASSETS.md          ← 所需文件清单、来源、sha256、设备事实（含外设地址表）
├── PITFALLS.md        ← 已踩的 8 个坑 + 源码级根因（**接手前必读**）
├── HANDOFF.md         ← 交接声明与未完成项
├── run.sh             ← 一键跑
├── scripts/
│   ├── fetch_kernel.sh     取通用 ARMv7 内核
│   ├── fetch_kmods.sh      取**同版本同构建**的真实内核模块
│   ├── mk_initramfs.sh     组 initramfs（★必须用系统 cpio）
│   └── extract_assets.py   从 org.bin 重建全部材料
├── templates/S00cgmmod     新增的 rcS 前置脚本模板（含说明）
└── assets/           材料（11 MB）
    kernel.zImage / rootfs.sqsh / rk3036.dtb / sdcard/{icube,rkgame,driver.so}
```

---

## 4. 典型用途

1. **回归验证**：每次改动后 `sh run.sh 30`，看里程碑有没有退化（比真机插卡快 3 个数量级）。
2. **崩溃现场抓取**：日志里带 PC/LR/寄存器/栈回溯（`guest_shim` 的 `dladdr` 会把地址落成 `so+偏移 符号`）。
3. **差分比对**：同一环境下跑「原厂件」与「自建件」，逐行对拍 stdout（**同环境比实现**，公平）。
4. **给真机排雷**：先在 qemu 里把同类问题清掉，真机只做最后确认。

---

## 5. 明确的限制（不要越过它下结论）

| 限制 | 影响 |
|---|---|
| **不是 RK 的 VOP/HDMI** | 分辨率的 mode 列表、图层(plane)数量、旋转、**像素格式集合**与真机不同。★ 已实测（见下表）。**显示相关的行为差异不能作为"复刻错误"的证据。** |
| **不是 RK 的 I2S/ACODEC** | 采样率/格式集合与真机不同 |
| **`/dev/mem` 无 RK GRF/GPIO 窗口** | 任何 `mmap(0x20000000…)` 读寄存器（如摇杆 GPIO）在此环境**不成立** |
| **`/dev/input/jsN` 需再触发** | `uinput`+`joydev` 已加载，但节点需要应用/服务主动创建 |
| **不是 RK3036 机器模型** | 中断/总线行为、时序、DMA 与真机不同 |

**要消除这些限制的两条路（按代价排序）**：
1. **qemu 侧加 RK 寄存器 MMIO 窗口**（只需小补丁：`memory_region_init_io` + 在机器里挂到 `0x20000000` 一带；
   `/dev/mem` 访问非 RAM 区**本来就被允许**，见 `PITFALLS.md` 第 7 条）。
2. **自己写 RK3036 机器模型**（骨架很便宜：qemu 官方最简 SoC 机器 52 行；贵的是 VOP/I2S/GPIO 的语义）。

---

## 6. 快速自查清单

- [ ] `qemu-system-arm --version` 可用（Debian/Ubuntu 的 `qemu-system-arm` 包）
- [ ] `unsquashfs`/`cpio` 可用（`squashfs-tools`、`cpio`）
- [ ] 主机能访问 `dl-cdn.alpinelinux.org`（取内核与模块）
- [ ] **QEMU 参数里必须带 `-global virtio-mmio.force-legacy=false`**（否则 `/dev/dri` 永不出现，见 `PITFALLS.md` 第 1 条）
- [ ] 内存 ≥ 1024 MB（initramfs 含全量模块，默认用 2048）

---

## 7. 使用注意（脚本可执行位 / 行尾）

- **本目录所有文本文件保持 LF**（见 `qemu-sim/.gitattributes`）。本仓库 `core.autocrlf=true`，
  若不显式覆盖，`.sh` 检出后会带 CR ⇒ `#!/bin/sh\r` 执行报 `\r: command not found`。
- Windows 上 git 不记录可执行位 ⇒ **请用 `sh run.sh` 调用**（不要依赖 `./run.sh`）。
  在 Linux/容器里可先 `chmod +x run.sh scripts/*.sh`。


### 5.1 ★ 实测：virtio-gpu 的 dumb buffer **只接受 bpp=32**（会直接影响 RGB565/RGB555 的程序）

用自带探针（`drm_probe.c` 的逻辑已在 `HANDOFF.md` §四列出）在同一环境实测：

```
open(/dev/dri/card0, O_RDWR) = 3                 errno=0(Success)
CAP DUMB_BUFFER = 1   CAP ATOMIC = 1   CAP ADDFB2_MODIFIERS = 64   CAP PRIME = 1

CREATE_DUMB  640x480  bpp=32  → r=0   handle=1 pitch=2560 size=1228800   MAP_DUMB r=0  mmap OK
CREATE_DUMB  640x480  bpp=24  → r=-1  errno=22(EINVAL)
CREATE_DUMB  640x480  bpp=16  → r=-1  errno=22(EINVAL)
CREATE_DUMB 1280x720  bpp=32  → r=0   handle=1 pitch=5120 size=3686400   MAP_DUMB r=0  mmap OK
CREATE_DUMB 1280x720  bpp=16  → r=-1  errno=22(EINVAL)
CREATE_DUMB 1920x1080 bpp=32  → r=0   handle=1 pitch=7680 size=8294400
CREATE_DUMB  320x240  bpp=32  → r=0   handle=1 pitch=1280 size=307200
```

**结论**：
- **bpp=32 完全可用**（含 `MAP_DUMB` + `mmap` + 写入）。
- **bpp=24 / bpp=16 一律 `EINVAL`**。
- ⇒ 若你的目标程序按**真机 VOP 的格式**（RK3036 常见 **RGB565 = 16bpp**）申请 dumb buffer，
  在**本环境会失败**，而**在真机成功**。这类失败**不是**复刻缺陷。
- ⇒ 反过来，这也是判断"要不要写 RK VOP 机器模型"的硬判据：**只要目标程序用非 32bpp 格式，
  就必须有 VOP 模型才能验证显示路径**。
