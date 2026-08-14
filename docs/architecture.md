# 架构设计书 — CubeGM 开源替代系统（RK3036G）

> 角色：架构师 高见远 ｜ 项目：cubegm-replacement ｜ 版本 v0.1（SOP Stage0→Stage3 设计基线）
> 配套：PRD（许清楚）、构建脚手架（寇豆码）、验证方案（严过关）

---

## 一、设计目标与边界

| 项 | 结论 |
|---|---|
| 目标硬件 | Rockchip **RK3036G**，双核 Cortex-A7（ARMv7-A），NEON+VFPv3，Mali-400MP，DDR3 |
| 用途 | **个人非商用自用**（唯一被 NC 许可覆盖的用途） |
| 野心 | 功能对等原厂，并**在可维护性与可扩展性上超越原厂**（原厂已停更） |
| 合规 | 复用 TreeFrogUI / FrogUI / picoarch 全部开源代码（CC BY-NC-SA 4.0 / GPL/LGPL/MAME+BSD），**不得商业化** |
| 不重写项 | `driver.so` 硬件抽象层（已确认为标准 Linux DRM/ALSA/evdev，picoarch `plat_linux.c` 直接可用） |
| 必须自研/适配项 | 启动劫持（`autorun`）、输入映射（joystick.zip→evdev）、UI 资源管线（RGB565 1280×720）、核心自定义 ABI 适配层 |

---

## 二、设备 ABI 实测结论（构建硬约束）

由 `tools/probe_device_abi.py` 对 20 个 `libemu_*.so` 实测：

| 维度 | 实测值 | 含义 |
|---|---|---|
| EI_CLASS | 1（32-bit） | 32 位 ELF |
| E_MACHINE | 40（EM_ARM） | ARM 架构 |
| E_FLAGS | **0x5000400** | HARD-float（armhf）+ EABIv5 |
| GLIBC 上限 | **GLIBC_2.17** | 设备 glibc 地板，链接不得超出 |
| NEEDED 高频库 | libc.so.6(20) / libgcc_s.so.1(17) / libm.so.6(16) / libpthread(6) / libdl / libz / libbz2 | 标准 C 运行库集 |
| 特殊依赖 | 1 个 core 链接 **libGL.so** | Mali-400 3D（GLES），需 sysroot 含 Mali 用户态库 |

**核心推论**：不能直接用「现代工具链（glibc 2.38）动态链接」编译出的二进制在设备上运行——会出现 `version GLIBC_2.xx not found`。
**正确做法**：链接目标必须是 **glibc ≤ 2.17** 的 sysroot。`--sysroot=` 可指向①设备 rootfs（金标准），或②**自举的 glibc-2.17 sysroot**（crosstool-NG / buildroot，无需设备，见 `docs/sysroot_strategy.md` + `build/toolchain/build_sysroot_ctng.sh`）。gcc 前端版本可新（ARM GNU 13.2）。**全静态链接已被否决**：picoarch 通过 `dlopen` 把 core 加载进同一进程，与 `libemu_*.so` 共用 glibc，静态 2.38 前端 + 设备 2.17 core 会双 glibc 冲突崩溃。

---

## 三、开源基础选型（已联网核实可用）

| 仓库（GitHub `tzubertowski`） | 角色 | 许可 | 复用方式 |
|---|---|---|---|
| `TreeFrogUI_picoarch` | libretro 前端（菜单/内核加载/存档） | GPL/LGPL/MAME+BSD | 主程序，裁剪适配 RK3036G |
| `FrogUI` | 启动器（Python+C） | CC BY-NC-SA 4.0 | 桌面/分类入口 |
| `treefrog-ui` | UI 资源 + `clone_cores.sh`/`build_all.sh`（57 core） | CC BY-NC-SA 4.0 | UI 皮肤 + 核心拉取脚本 |
| `TreeFrogUI_pcsx4all` | PCSX 派生（备用） | — | 可选 |

**前端内核加载模型**（picoarch，源码实测）：`core.c:720` 用 `dlopen(corefile, …)` 把 core 加载进 picoarch **同一进程**（标准 libretro 模型）；启动时 `main.c` 通过 `execl(picoarch_for_core(...))` 调对应 picoarch 变体，`plat_sdl.c` 则 `fork()` 劫持 core 以保留原厂 rkgame。→ **picoarch 与 `libemu_*.so` 必须共用同一份 glibc（2.17）**，这是 sysroot 必须 ≤2.17 的根本原因（静态链接被否）。

---

## 四、系统分层架构

```
┌──────────────────────────────────────────────────────────────┐
│  UI 层（FrogUI + picoarch 前端）  资源: ui_*.raw = RGB565 1280×720 │
├──────────────────────────────────────────────────────────────┤
│  输入层  joystick.zip 解码 → evdev 映射                        │
│   · 26 动作词表 · P1/P2 扫描码矩阵(2×27) · 4 USB 手柄 profile  │
├──────────────────────────────────────────────────────────────┤
│  前端/核心层  picoarch（plat_linux.c）  fork() libemu_*.so      │
│   · 自定义 core ABI 适配: retro_is_support/save_state/...      │
├──────────────────────────────────────────────────────────────┤
│  硬件抽象层  driver.so（标准 Linux，无需重写）                 │
│   · DRM/KMS 显示（/dev/dri, dumb buffer mmap）                 │
│   · ALSA 音频（snd_pcm_*）                                     │
│   · evdev 输入（/dev/input/event*）                            │
├──────────────────────────────────────────────────────────────┤
│  Linux 用户态（设备 rootfs = sysroot, glibc 2.17）             │
│  RK3036G: Cortex-A7 / Mali-400 / DRM / ALSA                    │
└──────────────────────────────────────────────────────────────┘
```

**启动链路（规避「sdcard is damaged」）**：
```
原厂 boot → rkgame → 读 autorun → cubegm/zhijack.sh
  → picoarch frogui_libretro.so → FrogUI 桌面
  → 选择游戏 → fork() libemu_<core>.so
```
不替换 `root.dat`/不破坏原厂校验分区即可挂载我们的系统，规避启动校验失败。

---

## 五、关键适配点

### 5.1 硬件抽象层（driver.so）— 不重写
实测 `driver.so` 字符串含标准 Linux 接口：`DRM_IOCTL_MODE_CREATE_DUMB`、`snd_pcm_*`、`/dev/input/event*`、路径 `/home/vmuser/work/Rk3036/host/arm-buildroot-linux-gnueabihf/sysroot/`（buildroot + armhf）。
→ **结论**：picoarch `plat_linux.c` 直接可用，设备驱动层零改写。
> 补充：picoarch 源码另含 `plat_sf3000.c`（SF3000 平台层）—— 本机即 SF3000 类设备，该文件是最直接的适配起点，优先于从 `plat_linux.c` 全新编写。

### 5.2 核心自定义 ABI 适配层（libemu_*）
本机 core 暴露非标准符号：`retro_is_support` / `save_state` / `load_state` / `set_unzip` / `set_progress_callback`。
→ 在 picoarch 侧加一个 **shim 层**将这些定制入口桥接到标准 libretro `retro_*` 生命周期；对不支持的标准接口返回「未实现」降级。

### 5.3 输入映射（joystick.zip → evdev）
- 26 动作词表（`0000_0000`=107B 配置）—— 映射为 evdev `KEY_*` / `BTN_*`。
- P1/P2 扫描码矩阵 `ui.cfg`（2×27）—— 行=P1/P2，列=动作，值=扫描码 0–52。
- 4 个 USB 手柄 profile（VID_PID_REV）：`0810_0001_0100`、`0810_0001_0110`、`20bc_5500`、`2563_0555`，每 profile 20 token=17 按钮 + [hat, axisX, axisY]。
→ 产出 `input/evdev_keymap.h`（见构建脚手架），新增手柄=加一个 `VID_PID_REV` 文件。

### 5.4 UI 资源管线（ui_*.raw）
- 全部 `ui_*.raw` = **RGB565 1280×720**，字节 0 起为完整帧（1,843,200 B），尾部为精灵/分层。
- 设备存在两套分辨率：UI 1280×720 vs 原厂 `root.dat` 720×480（仅启动 logo 区）。
→ UI 渲染目标固定 **1280×720 RGB565**；提供 PIL 解码/编码工具（`tools/render_raw.py`）做资源校验与生成。

### 5.5 核心注册（config.xml）
原厂注册 18/20，`libemu_gpsp.so`(GBA) 与 `libemu_prosystem.so`(Atari 7800) **存在但未注册**。
→ 在 `cores/config.xml` 补两条 `<core>` 即可启用（详见构建脚手架 `cores/config.xml`）。

---

## 六、工具链与构建策略

| 项 | 方案 |
|---|---|
| 前缀 | `arm-linux-gnueabihf-`（或 `arm-buildroot-linux-gnueabihf-`） |
| CFLAGS | `-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -O2` |
| 禁用 | aarch64（64 位不兼容）、arm-linux-gnueabi（soft-float ABI 不兼容） |
| sysroot | **glibc ≤ 2.17**，三条等价来源（任选其一）：①设备 rootfs（`sysroot_from_device.sh`，金标准）；②crosstool-NG 自举 2.17 sysroot（`build_sysroot_ctng.sh`，**无需设备，推荐**）；③glibc≤2.17 预编译旧工具链。**设备 rootfs 非必需**。2.17 sysroot 多缺 `alsa/asound.h` → 取 alsa-lib 源码仅 `include/` 加到 `-I`（纯 C 头，无 glibc 耦合）；DRM/evdev 头随 sysroot 自带 |
| 编译器获取 | ARM GNU 13.2（Linux-hosted）作 gcc 前端；**链接**指向 2.17 sysroot 规避 glibc 错配 |
| 构建机 | 用户 Linux 构建机（原厂证据为 `/home/vmuser/work/Rk3036` buildroot VM） |

---

## 七、四阶段路线（与 PRD 对齐）

| 阶段 | 目标 | 退出标准 |
|---|---|---|
| **Stage0 SDK** | 工具链+sysroot 就位，ABI 断言脚本通过 | `verify_target_abi.sh` 对样例二进制断言 EM_ARM/0x5000400/≤GLIBC_2.17 |
| **Stage1 MVP** | picoarch+FrogUI+3–5 core 证明 DRM+ALSA+evdev 闭环 | 设备启动进入 FrogUI，能选游戏并出声/出画 |
| **Stage2 功能对等** | 20+ core、config.xml 全注册、1280×720 UI、输入映射全覆盖 | 原厂能跑的在本系统都能跑；gpsp/prosystem 启用 |
| **Stage3 超越原厂** | 57 core、Quick Resume、主题、缩略图、多语言 | 超出原厂可维护性与可扩展性 |

---

## 八、风险与缓解

| 风险 | 等级 | 缓解 |
|---|---|---|
| glibc 错配导致运行崩溃 | 高 | 设备 rootfs 作 sysroot；ABI 断言门禁 |
| 启动校验失败（sdcard is damaged） | 高 | autorun 劫持，不碰 root.dat 校验分区 |
| 核心定制 ABI 不完全兼容标准 libretro | 中 | shim 适配层 + 逐 core 降级测试 |
| 分辨率/UI 适配（双分辨率） | 中 | 固定 1280×720 渲染管线 + PIL 校验 |
| NC 许可商用红线 | 高 | 仅个人非商用；分发须保留署名/非商用条款 |
| 获取/校验 glibc-2.17 sysroot | 中 | 用 `build_sysroot_ctng.sh` 自举或 `sysroot_from_device.sh`；`verify_target_abi.sh` 门禁；**静态链接不可行**（dlopen 同进程双 glibc 冲突） |

---
*下接：PRD（许清楚）→ 构建脚手架（寇豆码）→ 验证方案（严过关）。工具链已下载并校验（sha256=df0f49…），sysroot 实测含 drm.h/input.h、不含 asound.h；picoarch 源码已确认含 `plat_sf3000.c` 适配起点，且 `core.c` 确认 **dlopen 同进程加载 core**（故必须 2.17 sysroot、静态链接否决）。无设备 rootfs 时的 sysroot 自举方案见 `docs/sysroot_strategy.md` + `build/toolchain/build_sysroot_ctng.sh`。*
