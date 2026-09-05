# CubeGM 系统分析报告

> 分析方法：对 `R:\aa` 目录下的二进制与配置做静态逆向分析（ELF 头解析、`.dynsym` 动态符号表提取、二进制 ASCII 字符串扫描），**全部基于文件实证，非凭印象推测**。
> 分析时间：2026-08-13

---

## 一、一句话结论（TL;DR）

`R:\aa\cubegm` 是一个运行在 **32-bit ARM Linux（ARMhf 硬浮点）** 上的**街机 / 复古游戏模拟器前端系统（CubeGM）**，本质是一个 **定制版 libretro 前端**（架构等价于 RetroArch / Lakka / 国产 ARM 游戏机固件）。它由主程序 `rkgame` + 20 个 `libemu_*.so` 模拟器核心 + 配置驱动，**没有可读源码，是编译好的成品系统**。

---

## 二、系统身份与定位

| 项目 | 结论 | 证据 |
|------|------|------|
| 系统名 | **CubeGM**（Cube Game Manager，国产 ARM 游戏机/街机主板固件） | 目录名、配置体系、`icube` 启动器 |
| 运行平台 | **32-bit ARM，小端，硬浮点（ARMhf）** | ELF 头 `e_machine=0x28(EM_ARM)`，`ld-linux-armhf.so.3` |
| 系统类型 | 定制版 **libretro 前端**（非标准 RetroArch，而是改版 ABI） | `rkgame` 引用 21 个 `retro_*` 接口 |
| 目标市场 | 中文街机（三国战纪、西游释厄传等 IGS 街机） | `recent.lst` 含 `KNIGHTSOFVALOUR-SANGOKUSENKI(V117)/三国战纪`；中文 UI 默认启用 |
| 形态 | 成品二进制（无源码、无构建脚本） | 全部为 .so / ELF / 配置 / 资源，无 .c/.py 源码 |

---

## 三、技术架构

```
rkgame (主前端 ELF, 3.9MB)
  │  读取 setting.xml / cores/config.xml / cores/filelist.xml
  │  dlopen + dlsym 动态加载核心
  ▼
libemu_*.so ×20 (模拟器核心, libretro 兼容, 改版 ABI)
  ├─ 街机类:  fbalpha / fbalpha2012 / fba / cps2 / pgm / mame2000 / extend
  ├─ 主机类:  pcsx(PS1) / md(MegaDrive)
  ├─ 掌机类:  mgba / vbam / gpsp / tgbdual(GB/GBC)
  └─ 家用机:  nes / nestopia / snes9x / sfc / snes9x2010 / stella / prosystem
  ▼
saves/ (1740 个 .sav 存档) · states/ (即时存档) · lib/ (BIOS+系统库) · ui_*.zip (9 语言包)
```

- **前端-核心解耦**：主程序不内联模拟逻辑，运行时 `dlopen` 核心、调用 `retro_*` 接口驱动。
- **配置驱动**：核心注册、ROM 映射、用户设置全部由 XML / lst 文件描述。
- **多语言 UI**：`ui_cn/en/sp/ru/ar/po/ko/ge/fr.zip` 共 9 套，UI 国际化已就绪。

---

## 四、已实现功能

1. **多模拟器核心**：街机（FBA/MAME/PGM/CPS2）、FC、SFC、GBA、PS1、MD、NES、Atari 2600/7800 等。
2. **游戏库管理**：`cores/filelist.xml` 已登记 **121 个 ROM→核心映射**；`saves/000` 下有 1740 个存档文件。
3. **即时存档 / 恢复**：`savestatehotkey=3072`、`autorestore=03`，`states/` 目录支持快照。
4. **收藏夹 / 最近游玩**：`favorites.lst`、`recent.lst`（界面与数据层已就位）。
5. **多语言界面**：9 套 UI 包，中文默认（`gamelist=1`）。
6. **音频系统**：BGM（`Back_In_The_City.mp3`）+ 音效（`chord.wav`/`Button1.wav`），独立音频线程 `mui_SoundplayThread`。
7. **输入系统**：Linux 手柄 `/dev/input/js%d` + 键盘。
8. **金手指 / 作弊**：核心导出 `CheatInit/CheatSearchStart/CheatApply/CheatUpdate` 等。
9. **固件 OTA 更新**：代码引用 `update/firmware.upk`（升级通道已编码）。
10. **开机自动运行**：`setting.xml` 含 `autorun` 字段（预留）。

---

## 五、接口清单（三类）

### A. 核心 ↔ 前端契约（`retro_*` ABI，取自 `rkgame` 实际引用，共 21 个）

**标准 libretro 接口：**
- `retro_init` / `retro_deinit` / `retro_run`
- `retro_load_game` / `retro_unload_game`
- `retro_serialize` / `retro_serialize_size` / `retro_unserialize`
- `retro_set_environment` / `retro_set_video_refresh` / `retro_set_audio_sample_batch`
- `retro_set_input_poll` / `retro_set_input_state` / `retro_set_controller_port_device`
- `retro_get_region`

**⚠️ CubeGM 定制扩展（非官方 libretro，是改版 ABI 的标志）：**
- `retro_is_support` —— 询问核心是否支持某特性
- `retro_load_state` / `retro_save_state` —— 自有存档接口（替代标准 serialize 流程）
- `retro_set_unzip` —— 解压钩子（核心直接读 zip 内 ROM）
- `retro_set_progress_callback` —— 加载进度回调

> 注：标准 libretro 的 `retro_get_system_av_info` / `retro_get_system_info` / `retro_reset` / `retro_cheat_set` **未在 `rkgame` 中出现**，进一步印证这是一套**改版 libretro 头文件**。

### B. 配置 / 数据接口（文件约定）

| 文件 | 结构 | 作用 |
|------|------|------|
| `setting.xml` | `autorun` / `savestatehotkey` / `autorestore` / `config(language,volume)` / `ui(语言包,gamelist)` / `sound(bgm,effect)` | 用户全局设置 |
| `cores/config.xml` | `<core><emucore name file/> <supported_extensions/></core>` | 核心注册表（**已注册 18 个**） |
| `cores/filelist.xml` | `<file name="..." core="..."/>` | ROM→核心映射（**121 条**） |
| `favorites.lst` / `recent.lst` | 文本列表 | 收藏 / 最近 |
| `saves/<平台>/*.sav`、`states/<平台>/*` | 二进制 | 存档 / 即时存档 |
| `lib/` | `gba_bios.bin` / `neogeo.zip` / `pgm.zip` + `libcrypto/libfreetype/libz` | BIOS 与系统库 |

### C. 运行时 / 设备接口

- 动态加载：`dlopen` / `dlsym`（核心按需加载）
- 输入：`/dev/input/js0..N`（Linux evdev 手柄）、键盘
- 音频：`retro_set_audio_sample_batch`（独立线程播放）
- 视频：`video_drivers_init`（视频驱动初始化）
- 升级：`update/firmware.upk`（固件包路径，由升级逻辑消费）

---

## 六、待开发 / 未启用模块（基于文件实证）

| # | 模块 | 证据 | 状态 |
|---|------|------|------|
| 1 | **GPSP 核心（GBA）** | `libemu_gpsp.so` 已存在但**未写入 `cores/config.xml`**（`grep gpsp config.xml` = 0） | 已编译，**待接入**（加一条 `<core>` 即可启用） |
| 2 | **ProSystem 核心（Atari 7800）** | `libemu_prosystem.so` 已存在但**未注册**（`grep prosystem config.xml` = 0） | 已编译，**待接入** |
| 3 | **OTA 固件更新通道** | `rkgame` 引用 `update/firmware.upk`，但 `update/` 目录**为空** | 代码就绪，**更新包/升级服务器待上线** |
| 4 | **开机自动运行** | `setting.xml` 中 `autorun file="" driver=""` 为空 | 功能预留，**未配置** |
| 5 | **收藏夹** | `favorites.lst` 仅 1 字节（空） | UI 就位，**无数据 / 待完善** |
| 6 | **多语言游戏列表** | 9 个 UI 包已打包，但 `setting.xml` 中除中文 `gamelist=1` 外，其余 `gamelist=0` | 其余 8 种语言游戏列表**待逐个启用** |
| 7 | **扩展街机驱动框架** | `libemu_extend.so` 含 **12370 个导出符号**、大量 `BurnDrv*` 街机驱动 | 预留的"扩展街机驱动"接入点，可继续扩充游戏 |
| 8 | **金手指 / 封面素材** | `cores/mame2000/{cheat,artwork,sta}` 子目录已就位 | 功能依赖素材补充，待填内容 |

---

## 七、关键文件清单

```
R:\aa\
├─ cubegm/
│  ├─ rkgame              (主前端 ELF 可执行, 3.9MB)
│  ├─ icube               (启动器 ELF, 12KB)
│  ├─ driver.so           (驱动库)
│  ├─ setting.xml         (用户设置)
│  ├─ favorites.lst / recent.lst / menu.log
│  ├─ ui_cn/en/sp/ru/ar/po/ko/ge/fr.zip   (9 套多语言 UI)
│  ├─ font.ttf, Back_In_The_City.mp3, chord.wav, Button1.wav
│  ├─ cores/
│  │  ├─ config.xml       (核心注册表, 18 个已注册)
│  │  ├─ filelist.xml     (ROM→核心映射, 121 条)
│  │  ├─ bios/            (neogeo.zip, pgm.zip)
│  │  ├─ libemu_*.so ×20  (模拟器核心; 其中 gpsp/prosystem 未注册)
│  │  └─ mame2000/{cheat,artwork,sta}
│  ├─ lib/                (gba_bios.bin, neogeo.zip, pgm.zip, libcrypto/libfreetype/libz)
│  ├─ saves/  (1740 个 .sav, 按平台分目录)
│  ├─ states/ (即时存档, 各平台子目录为空)
│  └─ update/ (空 → OTA 待上线)
└─ Roms/  (ROM 根目录, 当前为空, 仅 save/ 子目录)
```

---

## 八、给开发者的下一步建议

1. **启用被遗漏的核心**：把 `libemu_gpsp.so`（GBA）、`libemu_prosystem.so`（Atari 7800）补进 `cores/config.xml` 的 `<core>` 列表即可，无需重新编译。
2. **若要做二次开发**：由于无源码，可向 `cores/` 投放新的 `libemu_*.so`（遵循上述 `retro_*` 改版 ABI，必须导出 `retro_is_support/retro_load_state/retro_save_state/retro_set_unzip` 等定制符号），并在 `config.xml` + `filelist.xml` 登记。
3. **在线更新**：在 `update/` 放置 `firmware.upk` 并搭建分发端点，即可激活 OTA。
4. **补全收藏/多语言**：填充 `favorites.lst`、把各语言 `gamelist` 改为 1，即可启用对应功能。
5. **确认 ROM 来源合法性**：系统中的 BIOS（neogeo/pgm）与 ROM 涉及版权，二次分发需注意合规。
