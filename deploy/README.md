# CubeGM 开源替代系统 — 设备部署包

一套用于 **RK3036G** 掌机（R36SX / DataFrog SF3000 / SF3500 / GB350 同类）的
开源固件替代系统，基于 `TreeFrogUI/picoarch` + `FrogUI` 构建，目标是**功能等价并超越原厂**。

---

## 两种用法

### 用法 A — 立即可用（无需编译，改一个文件）
原厂固件里 `libemu_gpsp.so`(GBA) 和 `libemu_prosystem.so`(Atari 7800) **已经在机器上，
只是没被注册**。把本包的

```
deploy/cubegm/cores/config.xml
```

直接覆盖设备 `cubegm/cores/config.xml`，重启即可**多玩 GBA 和 Atari 7800**，
其它系统照常。零编译、零风险（不动 root.dat，不会 "sdcard is damaged"）。

> 注意：这个 config.xml 同时注册了标准 libretro 核心名（给用法 B 用）。
> 在纯原厂固件下，只有 gpsp / prosystem 两项会真正生效（对应机器上的 libemu_*.so）。

### 用法 B — 完整开源替代（一键构建，拷贝即用）
在 **x86_64 Linux 构建机**上执行：

```bash
cd cubegm-work/deploy
./build.sh
```

脚本会自动：
1. 定位/安装 ARM 交叉编译器（`arm-linux-gnueabihf-gcc`）
2. 用 crosstool-NG 自举 **glibc-2.17** sysroot（设备天花板，实测）
3. 取 ALSA 头文件（工具链 sysroot 不含）
4. 克隆并交叉编译 `picoarch`（基于 `plat_sf3000.c`）+ `FrogUI`
5. 交叉编译一组标准 libretro 核心（默认 mgba/snes9x/fceumm/picodrive/nestopia）
6. 跑 **ABI 门禁**（`verify_target_abi.sh`：EM_ARM / 0x5000400 / glibc ≤ 2.17）
7. 把所有产物打包进 `deploy/cubegm/`

构建完成后，把整个 `deploy/cubegm/` 目录**覆盖拷贝到 SD 卡根目录的 `cubegm/`**：

```
SD:/cubegm/
   autorun            <- 启动劫持（指向 zhijack.sh）
   zhijack.sh
   picoarch           <- 编译产物
   frogui_libretro.so <- 编译产物
   cores/
      config.xml
      *_libretro.so   <- 编译的核心
   ui_boot.raw        <- 开机画面（RGB565 1280x720）
   rkgame/icube/driver.so/root.dat  <- 原厂文件保持不动
```

下次开机，设备直接进入开源菜单（picoarch + FrogUI），
原厂 `rkgame/icube/driver.so` 与 `root.dat` **完全不动** → 不会 "sdcard is damaged"。

---

## 为什么本 Windows 沙箱不能直接给你编译好的二进制
本环境是 Windows/Git-Bash，**没有 Linux 构建环境、没有 WSL/Docker、也没有 ARM 交叉编译器**。
已实测：ARM 官方 mingw(Windows-native) 工具链 CDN 返回 404、Sourcery 不可达、
Bootlin 仅提供 Linux-hosted 工具链。而下载到的 ARM GNU 13.2 是 **Linux ELF**，
无法在 Windows 下运行。

因此"编译"这一步必须在你的 **Linux PC** 上跑 `build.sh` 完成；
**一切数据、配置、脚本、UI 资源、ABI 门禁、构建逻辑都已在本包就绪**，
你只需执行一条命令即可得到可直接拷贝到设备的成品。

---

## ABI 铁规则（已实测，写入 verify_target_abi.sh）
- 设备 20 个 `libemu_*.so` 实测 glibc 天花板 = **GLIBC_2.17**（由 `libemu_fbalpha.so` 拉高）。
- picoarch 用 `dlopen` 把核心加载进**同一进程**，所以前端与核心必须共用同一份 glibc。
- 因此：**静态链接 glibc 2.38 前端 + dlopen 2.17 核心是错的**（同进程双 glibc 崩溃）。
- 正确做法：gcc 版本可新，但**链接目标必须是 ≤2.17 的 sysroot**（crosstool-NG 自举）。
- 每个产物都过 `verify_target_abi.sh` 门禁（EM_ARM / 0x5000400 / ≤2.17）后才允许部署。

## 运行时闭环
```
开机 → autorun → zhijack.sh → picoarch(glibc2.17) ─dlopen→ frogui_libretro.so
                                                  └─dlopen→ *_libretro.so (≤2.17)
设备提供: libdrm / libkms / libasound / libGL (Mali-400) + 自身 glibc 2.17
→ DRM 出画(1280x720) + ALSA 出声 + evdev 响应输入(26 动作 + 4 USB 手柄)
```

## 目录
```
deploy/
  build.sh                  一键构建（Linux）
  README.md                 本文件
  cubegm/
    autorun                 SD 卡启动入口
    zhijack.sh              启动劫持逻辑
    cores/config.xml        核心注册表（标准 libretro 名 + gpsp/prosystem + 超越项）
    ui_boot.raw             示例开机画面（RGB565 1280x720，真实二进制）
  toolchain/
    build_sysroot_ctng.sh   crosstool-NG 自举 glibc-2.17 sysroot
    setup_toolchain.sh      工具链安装/校验
    verify_target_abi.sh    ABI 门禁
```

## 下一步（Stage2 超越原厂）
在 build.sh 基础上扩展 `CORES` 列表即可加入更多核心（如 genesis_plus_gx、fbneo、
mednafen_pce、scummvm、prboom）；再叠加 Quick Resume、主题切换、游戏缩略图、
多语言菜单等，均在 `docs/prd.md` 的 Stage3 路线中。
