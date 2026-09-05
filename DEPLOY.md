# CubeGM 部署与烧录指南（RK3036G，全核心移植版）

> 对应构建产物：`payload-<run#>`（本次提交 = 全核心移植：boot 劫持修复 + 66 核构建委托）。
> 仓库：`Lieguch/cubegm-build`。本固件是**开源替代前端**（picoarch + FrogUI），
> **不修改原厂系统分区**（root.dat / rkgame / icube / driver.so 原样保留）。

## 0. 启动机制（务必先读 —— 与旧版不同）

旧版文档把 `cubegm/autorun` 当成启动入口，这是**错的**：原厂 `rkgame` **不会**
执行 `autorun` 脚本，它只会读取 `cubegm/setting.xml` 的 `<autorun>` 字段，按 ROM
扩展名解析核心并 `dlopen`。这就是为什么早前实机测试仍进入原厂界面 —— 启动项根本
没被劫持。

**修正后的启动链（已在 treefrog-ui/hijack/tfhijack.c 核实）：**

```
设备上电
  └─ 原厂 rkgame 读 cubegm/setting.xml 的 <autorun file="/mnt/sdcard/MD/dummy.md" driver=""/>
       driver="" → 按扩展名 .md 解析核心 → dlopen cubegm/cores/libemu_md.so
            └─ libemu_md.so 实为我们的 libemu_tfhijack.so（改名）
                 retro_load_game() → fork → zhijack.sh
                      └─ picoarch frogui_libretro.so   (FrogUI 桌面前端)
                           └─ 用户在 FrogUI 选游戏 → picoarch <core> <rom>
```

因此烧录包必须包含三件劫持件（build.sh STAGE 9 自动打入）：

| 文件 | 作用 |
|------|------|
| `cubegm/setting.xml` | 覆盖原厂设置，写入 `<autorun file="/mnt/sdcard/MD/dummy.md" driver=""/>` |
| `cubegm/MD/dummy.md` | autorun 指向的“假 MD 卡带”（含最小合法 MD 头，内容被 tfhijack 忽略） |
| `cubegm/cores/libemu_md.so` | **改名自** `libemu_tfhijack.so` —— 顶替原厂 MD 核心，被 rkgame dlopen 后拉起前端 |

> `cubegm/autorun` 仍随包附带，但**仅是文档化兜底**，原厂不会调用它。

## 1. 产物里有什么

`payload-<run#>` Release 附带的 `cubegm-payload.zip` 解压后得到一个 `cubegm/` 目录：

```
cubegm/                         <- 这一层必须保留，整体拷到 SD 卡根
├── setting.xml                 # 启动劫持（覆盖原厂，写入 <autorun>）
├── MD/
│   └── dummy.md               # autorun 指向的假 MD 卡带
├── autorun                    # 文档化兜底入口（原厂不调用）
├── zhijack.sh                 # RK3036G 专用：起 picoarch + FrogUI
├── picoarch                   # libretro 前端（交叉编译 armhf / glibc<=2.17）
├── frogui_libretro.so         # FrogUI 启动器核心（首个加载的 core）
├── cores/
│   ├── config.xml             # 原厂 <emucore> 格式注册表（66 核，文档/兜底用）
│   ├── libemu_md.so           # 启动劫持核心（改名自 tfhijack）
│   ├── fceumm_libretro.so     # NES ……（其余 60+ 核见下表）
│   └── ……（实际构建出的 *_libretro.so）
├── lib/                       # 运行时库（SDL1.2 / libpng12 / libz，设备 rootfs 不自带）
└── bin/                       # 辅助二进制（如有）
```

> - 仓库 git 树只提交脚本与配置（`setting.xml`、`zhijack.sh`、`config.xml`、各 `*.sh`、
>   `hijack/`、`build_cores.sh` 等）；`picoarch`、各 `*.so`、`lib/`、`MD/dummy.md` 由 CI
>   构建期生成并打进 zip。STAGE 8 / 8.5 / 9.5 门禁已校验它们存在于产物中。
> - **构建数量**：`build_cores.sh` 尝试全部 66 核（WARN-only，逐个），实际在 armhf 上
>   约 35–50 个能编译通过；**基线 5 核（mgba / snes9x / fceumm / picodrive / nestopia）
>   为硬门禁，必定构建**。具体成功清单见 CI 的 `_BUILD_SUMMARY.txt`。

## 2. 烧录前提

- FAT32 格式 SD 卡（设备原卡即可，保留原厂 `rkgame/ icube/ driver.so/ root.dat`）。
- 读卡器 + 一台电脑（Windows / Mac / Linux 均可；**不需要** Linux 交叉编译环境，固件已预编译）。
- 下载 `cubegm-payload.zip`。

## 3. 烧录步骤（关键：保留 cubegm/ 这一层）

1. 下载并解压 `cubegm-payload.zip`，得到 `cubegm/` 目录。
2. **把整个 `cubegm/` 文件夹复制到 SD 卡根目录**，最终路径是 `SD:/cubegm/setting.xml`、
   `SD:/cubegm/picoarch` …（**不要**把它们摊平到 SD 根）。
   设备启动时从 `/mnt/sdcard/cubegm/` 读取，`setting.xml` 里的 `<autorun>` 即把控制权
   交给我们的前端。原厂 `rkgame/icube/driver.so/root.dat` 完全不动。
3. **安全红线**：不要改动 `root.dat`，不要删除 / 改名 `rkgame/ icube/ driver.so`。
   原厂系统分区保持原样，设备不会报 "sdcard is damaged"。

## 4. ROM 目录 → 核心 映射表（按文件夹放卡）

开源前端（picoarch + FrogUI）**扫描 cubegm/cores/*.so** 自动发现核心，并按 **ROM 所在
文件夹名** 选择核心。**文件夹名大小写敏感，必须精确匹配下表**。下表只列出本次构建实际
产出的核心；带 `†` 的是“已构建但无独立文件夹”（仅在 FrogUI 按游戏选核器中出现，或备用）。

| ROM 文件夹 | 系统 | 核心 .so | 备注 |
|------------|------|----------|------|
| `nes`, `FC`, `fds` | NES / 红白机 | `fceumm_libretro.so` | FDS 需 `disksys.rom` |
| `nesq`, `NES` | NES（快） | `quicknes_libretro.so` | |
| `nest` | NES（精确） | `nestopia_libretro.so` | |
| `snes`, `SFC` | SNES | `snes9x2005_plus_libretro.so` | snes9x 2005 |
| `snes02` | SNES | `snes9x2002_libretro.so` | snes9x 2002 |
| `snes10`† | SNES（更精确，较重） | `snes9x2010_libretro.so` | 按游戏选核器可选 |
| `gb` | Game Boy | `gambatte_libretro.so` | |
| `gbgb` | Game Boy | `gearboy_libretro.so` | |
| `gbb`, `dblcherrygb` | GB（联机） | `tgbdual_libretro.so` | |
| `gba`, `GBA` | GBA | `gpsp_libretro.so` | |
| `gbav` | GBA | `vba_next_libretro.so` | |
| `mgba`, `gbaf` | GBA（精确） | `mgba_libretro.so` | |
| `pokem` | Pokémon Mini | `pokemini_libretro.so` | |
| `gw` | Game & Watch | `gw_libretro.so` | |
| `sega`, `MD`, `SMS`, `32x` |  Mega Drive / Master System / 32X | `picodrive_libretro.so` | **策略/进度存档走此核**（之前原厂启动下失败，劫持后由 picoarch 接管存档） |
| `gpgx`, `segacd` | Mega Drive（精确）/ Sega CD | `genesis_plus_gx_libretro.so` | Sega CD 需 BIOS |
| `gg`, `GG` | Game Gear | `gearsystem_libretro.so` | |
| `a26` | Atari 2600 | `stella2014_libretro.so` | |
| `a5200` | Atari 5200 | `a5200_libretro.so` | |
| `a78` | Atari 7800 | `prosystem_libretro.so` | |
| `a800` | Atari 800/XL/XE | `atari800_libretro.so` | |
| `lnx` | Atari Lynx | `handy_libretro.so` | |
| `lnx`†（备用） | Atari Lynx | `mednafen_lynx_libretro.so` | 备用，未默认挂文件夹 |
| `pce` | PC Engine / TurboGrafx-16 | `mednafen_pce_fast_libretro.so` | |
| `pcesgx` | PC Engine SuperGrafx | `mednafen_supergrafx_libretro.so` | |
| `pcfx` | PC-FX | `mednafen_pcfx_libretro.so` | |
| `pc8800` | NEC PC-88 | `quasi88_libretro.so` | |
| `ngpc` | Neo Geo Pocket / Color | `race_libretro.so` | |
| `geolith` | Neo Geo AES/MVS | `geolith_libretro.so` | |
| `wswan` | WonderSwan / Color | `mednafen_wswan_libretro.so` | |
| `wsv` | Watara Supervision | `potator_libretro.so` | |
| `spec` | ZX Spectrum | `fuse_libretro.so` | |
| `zx81` | ZX81 | `81_libretro.so` | |
| `amstrad` | Amstrad CPC | `crocods_libretro.so` | |
| `amstradb` | Amstrad CPC+ | `cap32_libretro.so` | |
| `col` | ColecoVision | `gearcoleco_libretro.so` | |
| `thom` | Thomson MO/TO | `theodore_libretro.so` | |
| `xmil` | Sharp X68000 | `x68k_libretro.so` | |
| `c64`, `c64sc` | Commodore 64（VICE，需 ROM） | `vice_x64_libretro.so` | ROM 放 `cubegm/bios/vice/` |
| `c64f`, `c64fc` | Commodore 64（Frodo，免 ROM） | `frodo_libretro.so` | |
| `m2k` | MAME 2000 | `mame2000_libretro.so` | MAME 0.37b5 romset |
| `cps1` | Capcom CPS-1 | `fbalpha2012_cps1_libretro.so` | |
| `cps2` | Capcom CPS-2 | `fbalpha2012_cps2_libretro.so` | |
| `cps3` | Capcom CPS-3 | `fbalpha2012_cps3_libretro.so` | 实验性，帧率低 |
| `neogeo` | Neo Geo | `fbalpha2012_neogeo_libretro.so` | |
| `Quake` | Quake | `tyrquake_libretro.so` | |
| `wolf3d` | Wolfenstein 3D | `ecwolf_libretro.so` | |
| `prboom` | Doom / Doom II / Heretic / Hexen | `prboom_libretro.so` | |
| `outrun` | Out Run（街机） | `cannonball_libretro.so` | |
| `cavestory` | Cave Story | `nxengine_libretro.so` | |
| `flashback` | Flashback | `reminiscence_libretro.so` | |
| `xrick` | Rick Dangerous | `xrick_libretro.so` | |
| `jnb` | Jump 'n Bump | `jumpnbump_libretro.so` | |
| `gong` | Pong 克隆 | `gong_libretro.so` | |
| `pico8`, `fake08` | PICO-8 | `fake08_libretro.so` | |
| `ps1r` | PlayStation（lightrec JIT） | `pcsx_rearmed_libretro.so` | |
| `retro8` | PICO-8 兼容 | `retro8_libretro.so` | |
| `lowres-nx` | LowRes NX | `lowresnx_libretro.so` | |
| `arduboy` | Arduboy | `ardens_libretro.so` | |
| `arduous` | Arduboy（周期精确，慢） | `arduous_libretro.so` | |
| `int` | Intellivision | `freeintv_libretro.so` | |
| `fcf` | Fairchild Channel F | `freechaf_libretro.so` | |
| `vec` | Vectrex | `vecx_libretro.so` | ⚠ 需 OpenGL，RK3036G Mali-400 无 GL，大概率无法运行（仍构建，作备用） |
| `cdg` | CD+G Karaoke | `pocketcdg_libretro.so` | |
| `gme` | Game Music Emu（芯片音乐） | `gme_libretro.so` | |

> 上表共覆盖本次构建的 66 个核心仓库中的绝大多数。**具体哪些 *_libretro.so 真正编出，
> 以 CI 产物 `cubegm/cores/` 实际内容 + `_BUILD_SUMMARY.txt` 为准**；FrogUI 只枚举
> 实际存在的 .so，不会出现“有菜单无核心”的情况。

## 5. 开机预期

设备上电（HDMI 接显示器）后：原厂菜单被跳过，进入 FrogUI（libretro 桌面前端）；
可用手柄 / 按键选择核心与游戏；选择游戏后 picoarch 把对应核心 `dlopen` 进同一进程运行。

## 6. 实机验证清单（须用户逐项确认）

通用：

- [ ] **开机即进前端**：HDMI 看到 FrogUI 画面（标准 DRM/KMS，1280×720），**不再**是原厂菜单。
- [ ] **输入**：手柄 / 按键可识别（标准 evdev）；FrogUI 内能移动光标 / 确认。
- [ ] **音频**：进入游戏有声音（标准 ALSA）；无声请检查 `lib/` 下 ALSA/SDL .so 是否齐全。
- [ ] **核心枚举**：FrogUI 能列出所有实际构建出的核心（不少于基线 5 核）。
- [ ] **存档回归**：重点重测 **MD 策略游戏存档**（之前在原厂启动下失败）——劫持后由 picoarch 接管
      策略/进度存档，应可写、可载入。
- [ ] **回滚**：异常时把 `SD:/cubegm/` 整个删掉（或改名）即回到纯原厂系统。

按文件夹抽查（每类挑一个 ROM 跑通即可）：

- [ ] NES：`nes/` 下放一个 .nes，FrogUI 进 fceumm。
- [ ] SNES：`snes/` 下放一个 .sfc，进 snes9x2005。
- [ ] GBA：`gba/` 下放一个 .gba，进 gpsp / mgba。
- [ ] MD/SMS：`MD/` 下放一个 .md/.sms，进 picodrive（**含存档回归**）。
- [ ] Game Gear：`gg/` 下放 .gg，进 gearsystem。
- [ ] Atari 2600：`a26/` 下放 .bin，进 stella2014。
- [ ] PC Engine：`pce/` 下放 .pce，进 beetle-pce-fast。
- [ ] 街机 CPS：`cps1/` ~ `neogeo/` 任选，进对应 FBA2012。
- [ ] DOS/Port：`prboom/`(Doom)、`wolf3d/`(Wolf3D)、`Quake/`(Quake) 任选其一。
- [ ] 家用电脑：`c64f/`(Frodo 免 ROM) 或 `spec/`(ZX Spectrum) 任选其一。
- [ ] 其它感兴趣的系统按上表文件夹放置测试。

## 7. 排错

- **仍进原厂界面**：说明 `cubegm/setting.xml` 未被我们的版本覆盖，或 SD 路径不对。
  确认 SD 根目录是 `cubegm/setting.xml`（不是 `cubegm/.../setting.xml` 嵌套）。
  `setting.xml` 是配置文件（非 root.dat），修改安全。若你不确定格式，请把 SD 卡根的
  `setting.xml` 发我确认后再改。
- **HDMI 无信号 / 黑屏**：确认显示器支持 1280×720；检查 `lib/` 下 SDL/DRM .so 齐全。
- **某核心进不去 / 闪退**：该核可能 armhf 不兼容或缺少依赖 —— 属构建期 WARN 范畴，
  不影响其它核心。可在 FrogUI 改用同系统的备用核（如上表“备用”列）。

## 8. 已知范围与后续

- 本次为**全核心移植**：构建委托 `build_cores.sh` 尝试 `cores.md` 全部 66 核；实际可用数
  取决于 armhf 兼容性（CI `_BUILD_SUMMARY.txt` 给出确切清单）。
- UI 分辨率 1280×720；输入映射覆盖见 `joystick.zip` 分析（`docs/`）。
- 基线 5 核为已验证构建集；其余核心为最佳努力（WARN-only），失败不阻断整体烧录包。
- 版本：本次提交为“boot 劫持修复 + 66 核构建委托”合并版（全测试门禁：ABI + libretro
  符号 + payload 完整性）；绿构建后打测试版 tag。
