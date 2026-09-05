# CubeGM 闭源系统「开源替代」可行性评估

> 日期：2026-08-14
> 方法：联网调研开源生态（WebSearch + WebFetch）+ 设备二进制实测（ELF 标志位 + driver.so 字符串提取）。
> 目标：判断能否用开源代码 + RK3036G 硬件信息，开发一整套替代闭源 rkgame/icube/driver.so 的系统用于迭代升级。

---

## 一、现有系统拆解（闭源/开源边界）

| 组件 | 现状 | 角色 | 开源替代 |
|---|---|---|---|
| `rkgame` | 闭源（392KB ELF）| 菜单引擎 | **TreeFrogUI**（`frogui_libretro.so`，开源） |
| `icube` | 闭源（12KB ELF）| 启动器 | **autorun 劫持** → 直接拉起 picoarch |
| `driver.so` | 闭源（39KB ELF）| 硬件驱动（显示+音频） | ★ **不需要替代**（见 §三，标准 DRM+ALSA，picoarch 自带 Linux 平台层） |
| `cores/libemu_*.so` (20个) | 开源上游编译产物 | 模拟器核心 | 交叉编译到 ARM（上游全部开源） |
| `ui_*.zip`/`joystick.zip` 的 `.raw` | 资源 | UI 位图 | FrogUI 重做或适配 1280×720 RGB565 |
| `cores/config.xml` | 配置 | 核心注册 | 直接沿用/扩展 |
| BIOS（neogeo/pgm） | 数据 | 街机 BIOS | 用户合法备份，沿用 |

---

## 二、开源生态调研（联网核实）

### 2.1 核心项目（GitHub `tzubertowski` 组织）

| 项目 | 角色 | 语言 | 许可证 | 关键能力 |
|---|---|---|---|---|
| `treefrog-ui` | 总构建/补丁/文档 | Shell | CC BY-NC-SA 4.0 | `clone_cores.sh` + `build_all.sh` 交叉编译全量核心 |
| `TreeFrogUI_picoarch` | libretro 前端 | C | GPL/LGPL/MAME + BSD | 多平台抽象层（`plat_linux/trimui/miyoomini/sdl/sf3000.c`） |
| `FrogUI` | 启动器（菜单 UI） | Python 89.7% + C | CC BY-NC-SA 4.0 | 缩略图/主题/截图/存档/收藏 |
| `TreeFrogUI_pcsx4all` | PS1 模拟器 | C | 随上游 | 独立 PS1 |

**treefrog-ui 自述**："frontend replacement for DataFrog SF3000, GB350, SF3500, SF3000HD… replaces the stock menu and runs hundreds of retro systems"，已支持 **57 个模拟器核心**（vs 原厂 14）。

### 2.2 架构关键（来自 vbauer/treefrog-ui）

```
[boot] icube(/mnt/sdcard/cubegm/icube)
       └─► picoarch frogui_libretro.so   ← TreeFrogUI 自身也是一个 libretro core
             │ 用户选 ROM
             └─► fork()
                [parent] waitpid() 阻塞    [child] 跑游戏核心
                       ◄─ child 退出 ──────► 返回菜单
```
- picoarch 负责：显示、音频、libretro 核心生命周期
- TreeFrogUI（frogui_libretro.so）= 一个渲染文件浏览器的 libretro core，用 fork+waitpid 启动游戏

### 2.3 ⚠️ 关键差异：标准设备 vs 你的设备

| 维度 | 标准 SF3000/R36S/GB350 | **你的设备** |
|---|---|---|
| SoC | HCSEMI C3100/E3100 | **Rockchip RK3036G** |
| CPU 架构 | MIPS 74Kf | **ARM Cortex-A7（ARMv7-A, armhf）** |
| UI 分辨率 | 640×480 BGRA | **1280×720 RGB565** |
| TreeFrogUI 原生支持 | ✅（MIPS） | ❌（需 ARM 适配，但源码可移植） |

→ 你这是一台**换芯的 ARM 移植版 CubeGM 兼容机**。TreeFrogUI 上游为 MIPS 写，但 picoarch 是可移植 C，已有 ARM 设备（TrimUI/Miyoo）的 plat 文件可参照。

---

## 三、硬件层实测（决定性，driver.so 字符串证据）

从 `driver.so` 提取的字符串直接揭示硬件抽象：

### 3.1 显示 = 标准 Linux DRM/KMS（非 SF3000 私有接口）
```
drmIoctl
DRM_IOCTL_GEM_CLOSE / DRM_IOCTL_MODE_CREATE_DUMB / DRM_IOCTL_MODE_MAP_DUMB
/dev/dri                  ← 标准 DRM 设备节点
drm_disable_crtc / disable_non_main_crtcs
mmap()                    ← dumb buffer 映射
video_drivers_init / video_driver_disp_frame / video_driver_setmode
ScaleDisplayThread / DisplayThreadflag   ← 双缓冲+缩放显示线程
video_driver_set_aspect_ratio 16:9 / 4:3
overscan_percent / overscan_offset_x / overscan_offset_y
```

### 3.2 音频 = 标准 ALSA
```
snd_pcm_open / snd_pcm_hw_params_* / snd_pcm_writei / snd_pcm_prepare / snd_pcm_start / snd_pcm_drop / snd_pcm_close
sound_driver_init / sound_driver_playframe / sound_driver_deinit
sndout_alsa_stop / sndout_alsa_wait
```

### 3.3 构建环境铁证（印证 RK3036G + ARM + buildroot）
```
/home/vmuser/work/Rk3036/host/arm-buildroot-linux-gnueabihf/sysroot/usr/lib/crti.o
/home/vmuser/work/Rk3036/host/arm-buildroot-linux-gnueabihf/sysroot/usr/lib/crtn.o
alsaplay.c / driver.c
```
→ 设备用 **buildroot** 构建，工具链 **`arm-buildroot-linux-gnueabihf`**（hard-float ARM），项目代号 **Rk3036**。与用户说的 RK3036G + 我实测的 e_flags=0x5000400(armhf) **三路完全吻合**。

### 3.4 输入 = 标准 evdev（非 cubevol）
driver.so 内**没有** `cubevol`/`/tmp/joy_key` 字样（标准 SF3000 的输入守护）。→ 输入走标准 Linux `/dev/input/event*`（evdev）。原厂 `joystick.zip` 的 26 动作词表 + ui.cfg 扫描码矩阵是 rkgame 自己的输入映射层，跑在用户态。

### 3.5 结论：硬件驱动层无需重写

| 接口 | 原厂 driver.so 实现 | picoarch 对应 | 是否需要重写 |
|---|---|---|---|
| 显示 | DRM/KMS（/dev/dri + dumb buffer） | `plat_linux.c` 用 DRM/framebuffer | ❌ **不需要** |
| 音频 | ALSA | `plat_linux.c` 用 ALSA | ❌ **不需要** |
| 输入 | evdev（推测） | `plat_linux.c` 用 evdev | ❌ **不需要** |

**这台 RK3036G 设备比标准 SF3000 更接近通用 Linux**。picoarch 的 `plat_linux.c`（或 `plat_sdl.c`）通用平台层**直接可用**——不需要写专门的 `plat_rk3036.c`，不需要原厂 `driver.so`，不需要逆向私有显示接口。

---

## 四、可行性结论

✅ **技术可行，且硬件层是最大利好（标准 DRM+ALSA+evdev，无需重写）。**

整体方案：
```
[boot] stock rkgame（原厂，不动）
     └─► autorun → cubegm/zhijack.sh   ← 劫持点
              └─► picoarch (plat_linux.c, 交叉编译 armhf)
                    ├─ 显示: DRM/KMS (/dev/dri)
                    ├─ 音频: ALSA
                    ├─ 输入: evdev (/dev/input/event*)
                    └─ frogui_libretro.so (TreeFrogUI 菜单 core)
                          └─► 用户选 ROM → fork() 跑 libemu_*.so 核心
```

| 层 | 方案 | 工作量 |
|---|---|---|
| 前端 UI | TreeFrogUI/FrogUI（开源，Python+C） | 中（适配 1280×720 + RGB565 资源）|
| libretro 前端 | picoarch `plat_linux.c`（开源） | 低（交叉编译 + 配置）|
| 模拟器核心 | libemu_*.so 上游交叉编译 | 中（20-57 个核心）|
| 硬件驱动 | **不需要**（用标准 Linux 接口） | 0 |
| 部署 | autorun 劫持（zhijack.sh） | 低 |

---

## 五、风险与待解决

1. **交叉编译工具链/sysroot**：需要 `arm-buildroot-linux-gnueabihf` + DRM/ALSA 开发头（来自 RK3036G 的 buildroot SDK）。**这是最关键的硬前提**——没有 sysroot 就编不出能链接 DRM/ALSA 的 picoarch。
2. **分辨率适配**：picoarch/FrogUI 默认 640×480/854×480，需配 1280×720 + RGB565（我已实测设备 .raw 格式，可复用）。
3. **输入映射**：evdev 事件码 → libretro pad 按键（原厂 joystick.zip 的 26 动作词表 + ui.cfg 矩阵可作映射参考）。
4. **NC 许可证**：TreeFrogUI/FrogUI = CC BY-NC-SA 4.0（**非商用**）。若商用需重写 UI 或商业授权。
5. **开机校验**：直接替换 rkgame/icube 会触发 "sdcard is damaged" → 必须走 autorun 劫持，不动原厂二进制。
6. **picoarch 二进制**：上游不随包发布，必须自己交叉编译。

---

## 六、建议 SOP 路线（MVP → 全量）

**阶段 0（前置）**：拿到 RK3036G 的 buildroot SDK / sysroot（DRM+ALSA+evdev 开发头）。无此则全链路无法编译。

**阶段 1（MVP 验证）**：picoarch（plat_linux）+ FrogUI + 3-5 个核心（gpsp/prosystem/fbalpha/pcsx）跑通一个 ROM，验证 DRM 显示 + ALSA 音频 + evdev 输入闭环。

**阶段 2（功能对齐）**：扩到 20+ 核心、配置 config.xml、适配 1280×720 UI、移植原厂 joystick 输入映射。

**阶段 3（迭代升级）**：超出原厂能力——加 TreeFrogUI 的 57 核心、Quick Resume、主题、缩略图、多语言等原厂没有的功能。

---

*本评估所有结论均基于联网核实 + 设备二进制实测，可复现。driver.so 字符串提取脚本见 `analyze_elf2.py` 同类方法。*
