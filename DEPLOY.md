# CubeGM 部署与烧录指南（RK3036G）

> 对应构建产物：`payload-153`（run #153，全测试通过）。仓库：`Lieguch/cubegm-build`。
> 本固件是**开源替代前端**（picoarch + FrogUI），**不修改原厂系统分区**。

## 1. 产物里有什么

`payload-153` Release 附带的 `cubegm-payload.zip`（约 4.5 MB）解压后得到 `cubegm/` 目录：

```
cubegm/
├── autorun              # 开机入口（委托给 zhijack.sh）
├── zhijack.sh           # 启动 picoarch + FrogUI 前端
├── picoarch             # libretro 前端（已交叉编译 armhf / glibc<=2.17）
├── frogui_libretro.so   # FrogUI 启动器核心（首个加载的 core）
├── cores/
│   ├── config.xml            # 核心注册（5 个实际构建核）
│   ├── fceumm_libretro.so    # NES
│   ├── mgba_libretro.so      # GBA
│   ├── nestopia_libretro.so  # NES (Accuracy)
│   ├── picodrive_libretro.so # MD/Genesis + SMS/GG
│   └── snes9x_libretro.so    # SNES
├── lib/                 # 运行时库（ALSA / SDL 等 .so）
└── bin/                 # 辅助二进制（如有）
```

> 仓库 git 树只提交 `autorun / zhijack.sh / cores/config.xml`；`picoarch`、各 `*.so`、`lib/`、`bin/`
> 由 CI 在构建期生成并打进 zip。STAGE 9.5 门禁已校验它们存在于产物中。

## 2. 烧录前提

- FAT32 格式 SD 卡（设备原卡即可，保留原厂 `rkgame/ icube/ driver.so/ root.dat`）。
- 读卡器 + 一台电脑（Windows / Mac / Linux 均可；**不需要** Linux 交叉编译环境，固件已预编译）。
- 下载 `cubegm-payload.zip`。

## 3. 烧录步骤

1. 下载并解压 `cubegm-payload.zip`，得到 `cubegm/` 目录。
2. 将 `cubegm/` 目录下的**全部内容**复制到 SD 卡根目录
   （让 `autorun`、`zhijack.sh`、`picoarch`、`frogui_libretro.so`、`cores/`、`lib/`、`bin/` 直接位于 SD 卡根）。
   这样设备开机读取的 `autorun` 即被替换为我们的启动链。
3. **安全红线**：不要改动 `root.dat`，不要删除 / 改名 `rkgame/ icube/ driver.so`。
   原厂系统分区保持原样，设备不会报 "sdcard is damaged"。

> 备选：若你的设备 bootloader 从 `cubegm/` 子目录读取 `autorun`，则把整个 `cubegm/` 文件夹复制过去即可
> （保留 `cubegm/autorun`）。具体路径以你的设备为准——见第 5 节「实机验证」。

## 4. 开机预期

设备上电（HDMI 接显示器）后：原厂菜单被跳过，进入 FrogUI（libretro 桌面前端）；可用手柄 / 按键选择核心与游戏；
选择游戏后 picoarch 把对应核心 `dlopen` 进同一进程运行。

## 5. 实机验证清单（须用户确认）

CI 无法替代以下硬件验证，请逐项确认：

- [ ] **HDMI 显示**：接 HDMI 显示器，开机看到 FrogUI 画面（标准 DRM/KMS，1280x720）。
- [ ] **输入**：手柄 / 按键可被识别（标准 evdev）；FrogUI 内能移动光标 / 确认。
- [ ] **音频**：进入游戏有声音（标准 ALSA）；无声请检查 `lib/` 下 ALSA .so 是否齐全。
- [ ] **核心枚举**：FrogUI 能列出 5 个核心（NES / GBA / NES-Accuracy / MD / SNES）。
- [ ] **核心运行**：各选一个 NES 与一个 GBA 游戏，确认能进入并运行。
- [ ] **存档**：策略 / 进度存档可写、可载入。
- [ ] **回滚**：若异常，把 SD 卡根的 `autorun` 还原为原厂文件即可回到原厂系统。

## 6. 已知范围与后续

- 当前 5 核为已验证构建集；更多核心需 Stage-2 源码核实后扩充 `config.xml`（见 HANDOFF 待办）。
- UI 分辨率 1280x720；输入映射覆盖见 `joystick.zip` 分析。
- 版本：`v0.2`（payload-153）= 首个含完整自动化测试门禁（ABI + libretro 符号 + payload 完整性）的绿构建。
