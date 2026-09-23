# HANDOFF —— qemu-sim 交接说明

> 交接格式遵循本仓库既有规范（见根目录 `HANDOFF.md` / `00_START_HERE.md`）：
> **自包含、可独立验证、不猜、不依赖前一个 Agent 的口头上下文。**

## 一、这份东西是什么

一套**可无限重跑的 RK3036G 原厂启动链仿真系统**：不插卡、不开真机，把
`kernel → busybox init → rcS → 原厂脚本 → icube → driver.so → 应用`
在 QEMU 里跑起来（约 1 秒），并自动打 9 条里程碑判定。

## 二、你现在就能做的（5 分钟）

```sh
cd qemu-sim && sh run.sh 60
```
对照 `README.md` §2 的"成功长这样"。**不需要先读其它任何文件。**

想给**你的项目**用：
```sh
sh run.sh 120 --app <你的可执行>      # 或 --sdcard <你的整棵 SD 树>
```

## 三、已验证的事实（可直接引用，均已实测）

| 事实 | 证据 |
|---|---|
| 启动链在 QEMU 里**跑通** | `Run /sbin/init` → `Starting logging/network` → `Starting icube:` → `open driver.so sucess` → `rkgame v1.42` → 主循环 |
| DRM 是**真实**设备 | `/dev/dri/card0` major 226；`[drm] Initialized virtio_gpu 0.1.0`；`fb0: virtio_gpudrmfb` |
| ALSA 是**真实**声卡 | `/dev/snd/controlC0` + `pcmC0D0c/p`（内核 `snd-dummy`） |
| 设备 glibc = **2.29** | `lib/libc-2.29.so` + 版本串 `GNU C Library (Buildroot) stable release version 2.29.` |
| RK3036 外设表 | 原厂 DTB 提取，52 个带 `reg` 节点（见 `ASSETS.md` §3.3） |
| qemu **没有** RK3036 机器模型 | 实测 `-M help` 无 Rockchip；`rockchip-linux` 无 qemu 仓库 |
| 机器模型骨架很便宜 | qemu 官方最简 SoC 机器 52 行（`xlnx-zynq-mp-generic.c`）；贵的是外设语义 |

## 四、**未完成**（明确的续做项）

| # | 事项 | 现状 / 下一步判据 |
|---|---|---|
| 1 | **`DRM_IOCTL_MODE_CREATE_DUMB failed ret=-1`** | 原厂程序创帧缓冲失败（它只打印 `ret`，没打印 `errno`）。已备好探针 `drm_probe.c`（枚举 `DRM_CAP_*` + 7 组 w/h/bpp 参数扫描 + MAP_DUMB/mmap），把 `ret=-1` 变成具体 errno ⇒ 才能判定是"参数不被接受"还是"virtio-gpu 能力不足" |
| 2 | `Unknown format 875713089` | = `0x34325258` = `"XR24"` = `DRM_FORMAT_XRGB8888`。原厂程序在自己的格式表里没认出它 ⇒ 需查它的格式表来源（很可能来自真机 VOP 的 format 集合） |
| 3 | `double free or corruption (fasttop)` | 上述失败路径上的堆损坏。**在 #1 解决前不要单独去追它**（很可能是失败处理分支的次生问题） |
| 4 | `/dev/mem` 的 RK GRF/GPIO 窗口 | 需要 qemu 侧补一块 MMIO（见 `PITFALLS.md` #7：内核侧本来就允许访问非 RAM 区） |
| 5 | `/dev/input/jsN` | `uinput`+`joydev` 已加载，节点需再触发 |
| 6 | 显示/音频的**真机能力集** | 当前用 virtio-gpu/snd-dummy，**不是 RK VOP/I2S**；要 1:1 得写机器模型（见 `README.md` §5） |

## 五、红线（沿用本仓库铁律，本套件完全遵守）

1. **绝不**改名/删除原厂 `icube` / `rkgame` / `driver.so` / `cores/libemu_*.so` / `root.dat`。
   本套件把原厂件放在 `assets/sdcard/`，**只读使用**；所有注入都走**新增文件**（`/etc/init.d/S00cgmmod`）。
2. **绝不**改写 `root.dat` / 校验分区。
3. 不改动 `org.bin` / 设备树 / 原厂 rootfs 里的任何文件；需要差异时**复制到工作目录再改**。

## 六、本套件的自我限制（避免误用）

- 它**不是** RK3036 机器模型：中断/总线/DMA/时序与真机不同。
- 它**不替代**真机验收：显示/音频能力集、`/dev/mem` 寄存器行为都不等价。
- **正确用法**：把它当"快速回归 + 崩溃现场抓取 + 同环境差分"的台子；真机只做最后确认。

## 七、复现与对账

- 全部材料带 sha256（`ASSETS.md` §1），可逐字节对账。
- 材料可用 `scripts/extract_assets.py` 从 `org.bin` 重建（脚本会打印 sha256 供比对）。
- 脚本已过 `sh -n` 语法检查。
