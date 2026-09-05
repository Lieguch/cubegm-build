# CubeGM 输入子系统 + UI 资源 + 核心注册 深度报告

> 日期：2026-08-13
> 方法：直接读取 `cubegm/cubegm/` 文件 + Python 静态解析 + PIL 实测 RGB565 渲染 + config.xml 审计。
> ⚠️ 本文修正 `cubegm_zip_contents.md`、`root_dat_analysis.md` 两份前报告的若干错误，详见 §六。

---

## 一、一句话结论

| 子系统 | 结论 | 验证 |
|---|---|---|
| **输入** | `joystick.zip` = 26 个逻辑动作词表 + 2 玩家默认扫描码矩阵 + 4 个 USB 手柄 profile（按键→动作 + hat/axis 索引）| 全部条目逐字节解析（§二） |
| **UI .raw** | 全部 .raw = **RGB565 1280×720**，首帧从字节 0 起就是完整全屏画面 | PIL 渲染 `menu/game/setting/search/type/joystick` 6 张全部成功（§三） |
| **设备分辨率** | UI **1280×720**，stock 系统 **720×480**（root.dat）→ LCD 实际是 1280×720，stock 启动画用 720×480 | .raw 大小 + 渲染双确认（§三.4） |
| **核心注册** | `cores/` 20 个 .so，`config.xml` 注册 18 个；**`gpsp`（GBA）和 `prosystem`（Atari 7800）没注册但 .so 在** | config.xml vs .so 一一比对（§四） |

---

## 二、输入子系统（joystick.zip 全解码）

joystick.zip 共 **7 个条目**（标准 ZIP、无密码、deflate 压缩）：

| 文件 | 大小 | 实际内容 |
|---|---|---|
| `0000_0000` | **107 B** | 26 个逻辑动作名（P1 套 13 + P2 套 13） |
| `0810_0001_0100` | 60 B | USB 手柄 profile（VID:PID=0810:0001，rev=0100） |
| `0810_0001_0110` | 56 B | USB 手柄 profile（同 0810:0001，rev=0110） |
| `20bc_5500` | 60 B | USB 手柄 profile（VID:PID=20bc:5500） |
| `2563_0555` | 56 B | USB 手柄 profile（VID:PID=2563:0555） |
| `joystick.raw` | 3,017,008 B | RGB565 1280×720：P1+P2 手柄布局图 + 精灵尾 |
| `ui.cfg` | 215 B | 2×27 默认按键扫描码矩阵（行 0/1 = P1/P2；第 3 行 `[1]` 是启用标志） |

### 2.1 逻辑动作词表（0000_0000）

```
SELECT, START, UP, DOWN, LEFT, RIGHT, A, B, TL1, X, Y, TR1, RESET,
SELECT, START, UP, DOWN, LEFT, RIGHT, A, B, TL1, X, Y, TR1, RESET
```

前 13 = **P1 套**（含 d-pad 4 个 + SELECT/START + A/B + TL1/X/Y/TR1 + RESET），
后 13 = **P2 套**（完全相同）。  
**这 13 个动作正好是 joystick.raw 那张手柄图（P1+P2 各 13 键）的可视化定义**。

> 注：ui.cfg 矩阵有 **27 列**（§2.2），比词表多 1。第 27 列对应的动作未在 `0000_0000` 列出，推测是 MENU/HOTKEY/COIN 之类的"系统键"。

### 2.2 ui.cfg 默认按键矩阵（行 = 玩家，列 = 动作）

| | 列 0 (SELECT-P1) | 列 1 (START-P1) | 列 2 (UP-P1) | 列 3 (DOWN-P1) | 列 4 (LEFT-P1) | 列 5 (RIGHT-P1) | 列 6 (A-P1) | 列 7 (B-P1) | 列 8 (TL1-P1) | 列 9 (X-P1) | 列 10 (Y-P1) | 列 11 (TR1-P1) | 列 12 (RESET-P1) | 列 13 (SELECT-P2) | 列 14 (START-P2) | 列 15 (UP-P2) | 列 16 (DOWN-P2) | 列 17 (LEFT-P2) | 列 18 (RIGHT-P2) | 列 19 (A-P2) | 列 20 (B-P2) | 列 21 (TL1-P2) | 列 22 (X-P2) | 列 23 (Y-P2) | 列 24 (TR1-P2) | 列 25 (RESET-P2) | 列 26 (系统键?) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **行 0（P1 物理扫描码）** | 0 | 3 | 2 | 32 | 4 | 41 | 33 | 36 | 1 | 31 | 40 | 35 | 34 | 38 | 37 | 39 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
| **行 1（P2 物理扫描码）** | 0 | 18 | 17 | 43 | 19 | 52 | 44 | 47 | 16 | 42 | 51 | 46 | 45 | 49 | 48 | 50 | 20 | 21 | 22 | 23 | 24 | 25 | 26 | 27 | 28 | 29 | 30 |

含义：第 *r* 行第 *c* 列的数字 = 玩家 *r* 的物理扫描码（手持机内置按键 GPIO/键矩阵编号），当该扫描码被触发时，就执行第 *c* 列对应的逻辑动作。  
例如：行 0 列 6 = **P1 按下扫描码 33 → 触发动作 A**；行 1 列 0 = **P2 按下扫描码 0 → 触发 SELECT**。

### 2.3 USB 手柄 profile（按 VID:PID[:REV] 命名）

格式：**20 个 token = 17 个按钮槽 (btn[0..16]) + 3 个尾部 [hat 索引, axisX 索引, axisY 索引]**。  
"0" 表示该按钮槽未映射；非零值表示映射到的动作名。

| Profile（设备） | 已映射的按钮（btn[i] → 动作） | 尾部 [hat, axisX, axisY] |
|---|---|---|
| `0810_0001_0100` | btn[0]=A, [1]=B, [2]=TL1, [3]=X, [4]=Y, [5]=TR1, [6]=TL1, [7]=TR1, [8]=TL2, [9]=TR2, [10]=SELECT, [11]=START | **1, 0, 1**（hat=1，无模拟轴）|
| `0810_0001_0110` | btn[0]=Y, [1]=B, [2]=A, [3]=X, [4]=TL1, [5]=TR1, [6]=TL2, [7]=TR2, [8]=SELECT, [9]=START | **1, 4, 5**（hat=1，模拟轴 4/5）|
| `20bc_5500` | btn[0]=A, [1]=B, [3]=X, [4]=Y, [6]=TL1, [7]=TR1, [8]=TL2, [9]=TR2, [10]=SELECT, [11]=START, [12]=**RESET** | **1, 6, 7**（hat=1，模拟轴 6/7）|
| `2563_0555` | btn[0]=Y, [1]=B, [2]=A, [3]=X, [4]=TL1, [5]=TR1, [6]=TL2, [7]=TR2, [8]=SELECT, [9]=START | **1, 4, 5**（hat=1，模拟轴 4/5）|

观察：
- `0810:0001` 同款硬件出两个 rev（0100 / 0110），但按键映射不同 — 说明 rev 切换会重新识别。
- `0810_0001_0110` 与 `2563_0555` 映射完全相同，可能是同一 OEM 的不同型号贴牌。
- 只有 `20bc_5500` 暴露了 RESET 按钮。
- 所有 profile 的 hat 索引都是 1；模拟轴索引因设备而异（0/1 无模拟、4/5、6/7）。

### 2.4 输入子系统的整体结构

```
  ┌──────────────┐
  │ 物理输入源    │
  ├──────────────┤
  │ ① 手持机内置   │── ui.cfg（扫描码→动作，2 玩家）
  │ ② USB 手柄    │── 按 VID:PID[:REV] 匹配 joystick.zip 里的 profile
  └──────┬───────┘
         ▼
  ┌──────────────┐
  │ 26 动作词表    │  0000_0000（P1+P2 各 13 键）
  └──────┬───────┘
         ▼
  ┌──────────────┐
  │ libretro 核心 │  各 libemu_*.so 接收 retro_key / retro_pad 调用
  └──────────────┘
```

### 2.5 怎么用 / 怎么改

| 想做的事 | 改哪里 |
|---|---|
| 改手持机默认按键 | 编辑 `joystick.zip/ui.cfg`，把矩阵里的扫描码改成新值 |
| 让某个动作在 P1/P2 互换 | 交换矩阵行 0 / 行 1 |
| 给一种新型号 USB 手柄写映射 | 把 `joystick.zip` 里加一个 `VID_PID_REV` 文件（如 `1234_5678_0000`），写入 17 按钮映射 + 3 尾部 |
| 改/新增逻辑动作 | 同步改 `0000_0000` 词表 + ui.cfg 矩阵列数 + 对应 .raw 手柄图 |
| 改 P1/P2 手柄视觉布局 | 重做 `joystick.raw`（必须 1280×720 RGB565 起头，§三） |

---

## 三、UI .raw 格式（经验证 = RGB565 1280×720）

### 3.1 尺寸表

| 文件 | 字节数 | 渲染出的画面 |
|---|---|---|
| `ui_cn.zip/menu.raw` | 3,282,288 | 主菜单（收藏 / 列表 / 分类 / 历史 / 搜索）|
| `ui_cn.zip/game.raw` | 2,911,744 | 子菜单（返回游戏 / 退出 / 保存 / 载入 / 画面 / 手柄）|
| `ui_cn.zip/setting.raw` | 2,857,048 | 系统设置（语言 / 看图本机文件 / 恢复默认 / 退出）|
| `ui_cn.zip/search.raw` | 2,234,144 | 搜索（屏幕键盘 + 输入框）|
| `ui_cn.zip/type.raw` | 2,274,136 | 类型筛选（ARCADE / FC / SFC / MD / GBA / GB / GBC / PS / ATARI）|
| `joystick.zip/joystick.raw` | 3,017,008 | P1+P2 虚拟手柄布局图（含 dpad + SELECT/START + A/B + X/Y + TL1/TR1/TL2/TR2 + RESET）|

每个 .raw 都符合这个规律：
```
   字节 0 ───────────── 1,843,200 B ────────────── 末尾
   ▼                                            ▼
   1280×720 RGB565 完整一帧画面         精灵/分层/不同状态 尾
```

首帧 1,843,200 = **1280 × 720 × 2（RGB565）**。尾部是同一屏的额外图层 / 高分辨率精灵 / 不同状态。

### 3.2 渲染证据（核心实证）

把每个 .raw 从**字节 0**开始按 **RGB565 1280×720** 解码、用 PIL 渲染成 PNG，**6 张全部得到可读、连贯、与文件名语义一致的界面**（详见 `docs/render/` 附图）：
- `menu_off0.png`：中文主菜单 + 右侧 P1/P2 手柄视觉
- `game_off0.png`：游戏设置子菜单
- `setting_off0.png`：系统设置（含古风背景）
- `search_off0.png`：屏幕键盘
- `type_off0.png`：按主机类型筛选的列表
- `joystick_off0.png`：P1 + P2 手柄布局（dpad + 10 个动作键 ×2）

**结论：这些 .raw 不是带 alpha 的贴图、不是压缩资源，是从字节 0 起原生的 RGB565 1280×720 帧缓冲。**

### 3.3 joystick.raw 的双身份

`joystick.raw` 的首帧直接就是 **P1+P2 虚拟手柄布局** —— 渲染图上能数出每个按钮的相对位置（d-pad 在左下、SELECT/START 居中、A/B 在右下、X/Y 在左上、TL1/TR1 在顶部、TL2/TR2 在更上方、RESET 在最右侧），位置和 §2.1 的 26 动作词表一一对应。

所以 joystick.raw **既是"手柄设置界面的视觉模板"，也是 §2.1 词表的"可视化字典"**——给玩家改键时直接看图选位置。

### 3.4 两套分辨率的发现（设备硬件线索）

| 资源 | 分辨率 | 出处 |
|---|---|---|
| UI / 手柄 .raw | **1280×720 RGB565** | `ui_*.zip` 5 张 + `joystick.zip/joystick.raw`（§三）|
| 系统/启动 .raw | **720×480 RGB565** | `root.dat` 9 个条目（每条 691,200 B = 720×480×2）|

`ui_cn.zip/ui.cfg` 里的 `RecoverRect=1278,718,1,1` 也间接佐证目标分辨率是 **1280×720**（1278+1+1=1280，718+1+1=720，这是"恢复提示框"的右下角坐标和尺寸）。

**推断**：设备 LCD 实际是 **1280×720（720p）**；原厂固件（root.dat 里的开机 / 系统画面）按 720×480 渲染，cubegm UI 升级到全分辨率。`cores/filelist.xml` 与 `setting.xml` 共同指向这一目标。

---

## 四、config.xml 核心注册审计

### 4.1 全面对账表

| .so 文件（存在） | 在 config.xml 的注册名 | 扩展名 | 状态 |
|---|---|---|---|
| libemu_fbalpha.so | `fbalpha` | ZIP, 7Z | ✅ |
| libemu_fbalpha2012.so | `fbalpha2012` | ZIP | ✅ |
| libemu_mame2000.so | `mame2000` | ZIP | ✅ |
| libemu_pgm.so | `libemu_pgm.so` | ZIP | ✅ |
| libemu_fba.so | `libemu_fba.so` | ZIP | ✅ |
| libemu_cps2.so | `libemu_cps2.so` | ZIP | ✅ |
| libemu_extend.so | `libemu_extend.so` | ZIP | ✅ |
| libemu_snes9x.so | `Snes9x` | SMC/SFC/SWC/FIG | ✅ |
| libemu_sfc.so | `Snes9x 2005` | SMC/FIG/SFC/GD3/GD7/DX2/BSX/SWC | ✅ |
| libemu_snes9x2010.so | `Snes9x 2010` | SMC/FIG/SFC/GD3/GD7/DX2/BSX/SWC | ✅ |
| libemu_mgba.so | `mgba` | GBA/GB/GBC | ✅ |
| libemu_vbam.so | `VBA-M` | GBA | ✅ |
| libemu_md.so | `picodrive` | BIN/GEN/SMD/MD | ✅ |
| libemu_nes.so | `FCEUmm` | NES/FDS/UNIF/UNF | ✅ |
| libemu_nestopia.so | `MEStopia` | NES | ✅ |
| libemu_stella.so | `Stella` | A26/BIN | ✅ |
| libemu_tgbdual.so | `TGB Dual` | GB/GBC/SGB | ✅ |
| libemu_pcsx.so | `PCSX ReARMed` | BIN/IMG/MDF/PBP/ISO/TOC | ✅ |
| **libemu_gpsp.so** | — | — | ⚠️ **.so 在但未注册** |
| **libemu_prosystem.so** | — | — | ⚠️ **.so 在但未注册** |

### 4.2 启用未注册的两个核心

直接在 `cores/config.xml` 末尾追加：

```xml
<core>
<emucore name="gpsp" file="libemu_gpsp.so" />
<supported_extensions>GBA</supported_extensions>
<suported_extensions>ZIP</supported_extensions>
</core>

<core>
<emucore name="prosystem" file="libemu_prosystem.so" />
<suported_extensions>A78</supported_extensions>
</core>
```

之后需要：
1. 在 `cores/filelist.xml`（或游戏列表扫描器）的"按扩展名选核心"逻辑里确认 GBA→`gpsp` 或 `mgba`/`VBA-M` 的优先级（当前 GBA 只绑了 `mgba`/`VBA-M`，加 `gpsp` 后会变成可选）。
2. BIOS：`cores/bios/` 里 Atari 7800 没有 BIOS；若有 `.a78` ROM 跑出"缺 BIOS"，可从合法渠道备份。
3. 重启 `rkgame` 生效（注意直接替换原厂 `rkgame`/`icube` 会被开机校验拦截，所以这次只是改 `cores/config.xml`，不动厂商二进制，**安全**）。

---

## 五、ui.cfg（UI 语言包）真实内容

ui_cn.zip/ui.cfg（GBK 编码，注释部分乱码但**键值都正确**）：

```ini
#[Setting]
# 恢复默认的字体大小
# 恢复默认的提示框起始坐标 X,Y 和宽度, 高度
RecoverRect=1278,718,1,1
RecoverFontSize=1
# 恢复默认的第一行提示信息
RecoverInfo1=.
# 恢复默认的第二行提示信息
RecoverInfo2=.
```

`1278,718,1,1` = 提示框左上角 (1278,718)，宽 1，高 1 → 这是一个 1×1 像素的占位提示框（默认隐藏），需要在运行时被替换为真实的"恢复默认设置"提示框坐标。**这是个间接证据：UI 的目标分辨率是 1280×720**。

---

## 六、对前两份报告的错误更正

### 6.1 更正 `cubegm_zip_contents.md` §三（joystick.zip）

| 原报告说法 | 错误 | 实际情况 |
|---|---|---|
| `0000_0000 (0B) 手柄标识占位` | ❌ 0B、占位 | ✅ **107 B 文本**，是 **26 动作词表**（§2.1） |
| `0810_0001_0100 (0B) 手柄标识占位` | ❌ 0B、占位 | ✅ **60 B 文本**，是 **该手柄 12 按钮映射**（§2.3） |
| `20bc_5500 (0B) 手柄标识占位` | ❌ 0B、占位 | ✅ **60 B 文本**，是手柄映射（含 RESET） |
| `0810_0001_0110 (0B) 手柄标识占位` | ❌ 0B、占位 | ✅ **56 B 文本**，是同 0810:0001 的 rev=0110 映射 |
| `（另含 1 个无扩展名 0B 文件）` | ❌ | 实际是 **`2563_0555`（56 B）** —— 第四个 USB profile |
| "5 个 0 字节空文件是 VID_PID 占位标识" | ❌ 整体错误 | 这些是**实配置**：1 个词表 + 4 个手柄 profile |

### 6.2 更正 `cubegm_zip_contents.md` §二（.raw 格式）

| 原报告说法 | 错误 | 实际情况 |
|---|---|---|
| "≈ 1280×720 的 RGB565/**带 alpha 贴图**" | ⚠️ "带 alpha" 不实 | 实测纯 **RGB565 无 alpha**（PIL 渲染证实，§三.2）|
| 未指出首帧从字节 0 起就是完整画面 | ⚠️ 不够明确 | **字节 0 起就是 1280×720 全屏一帧**（1,843,200 B），其余为精灵尾 |
| 尺寸描述为"≈ 3.2MB / 2.8MB / 2.1MB" | ⚠️ 不精确 | 实测精确：3,282,288 / 2,911,744 / 2,857,048 / 2,234,144 / 2,274,136 |

### 6.3 保留仍然正确的部分

- `cubegm_zip_contents.md` §二 `ui.cfg` 的 GBK 乱码 + RecoverRect 内容仍然正确（§五已重新核实）。
- §四 BIOS 包的 neogeo/pgm 描述、`lib/` 与 `cores/bios/` 双副本版本不一致风险仍然成立。
- `root_dat_analysis.md` 的 **9 × 691,200 字节条目 = 720×480 RGB565** 推断**仍然成立**——这正好与本报告 §三.4 的"两套分辨率"形成对照，是新发现而非错误。

---

## 七、待办 / 下一步可选

- [ ] 联网核实 VID:PID `0810:0001` / `20bc:5500` / `2563:0555` 对应的真实手柄型号（用于二次开发时确认兼容性、写新 profile 模板）。
- [ ] 把 `gpsp` + `prosystem` 补进 `cores/config.xml`（§四.2），实测启动 GBA/Atari 7800。
- [ ] `Roms/` 当前为空 → 放 ROM → `rkgame` 扫盘生成 `cores/filelist.xml`（已观察到 `recent.lst` 里有"三国战纪"残迹，但 ROM 文件已被分离）。
- [ ] 如果做"自定义键位/皮肤"：编辑 `joystick.zip/ui.cfg`（改默认扫描码）或重做 `joystick.raw`（改手柄图，必须 1280×720 RGB565）。
- [ ] 如果做"多语言全开"：在 `setting.xml` 给 9 个 `ui_*.zip` 的 `<ui>` 全部加 `gamelist="1"`，并把 `language` 选项扩到对应语种。
- [ ] 根因排查"两套分辨率"：root.dat 的 720×480 是原厂 Hichip 系统残留，cubegm 升级到 1280×720 LCD；如果某些屏幕异常出现拉伸/黑边，就是分辨率切换的痕迹。
- [ ] 完成 `source/` 的 `TreeFrogUI_picoarch` + `FrogUI` 克隆后，对照源码确认 `joystick.zip` 的解析函数（输入守护进程 `cubevol` → `/tmp/joy_key` 共享内存），把 ui.cfg 矩阵和 profile 格式的定义对上号（克隆已在后台执行，本报告未阻塞等待）。

---

*本报告所有实证均可在 `R:\aa\cubegm-work\cubegm\cubegm\` 直接复现，渲染图存于 `docs/render/`。*