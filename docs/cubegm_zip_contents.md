# CubeGM 压缩文件内容扫描报告

> 补遗：上一版 `cubegm_analysis.md` 只看了文件名，本次**逐个打开全部 14 个 `.zip`**，读取条目清单、扩展名分布与关键文本配置。
> 方法：Python `zipfile` 静态读取（不写出到磁盘），全部基于文件实证。

---

## ⚠️ 更正（2026-08-13，cubegm_input_and_ui.md §六）

本报告 §三 "joystick.zip" 与 §二 ".raw 格式" 中存在以下事实错误，已在 **`docs/cubegm_input_and_ui.md`** 全文重写并更正。此处仅做修订标注，方便对照：

| 位置 | 原文 | 错误 | 实测 |
|---|---|---|---|
| §三 joystick.zip 条目清单 | `0000_0000 (0B) 手柄标识占位` 等 5 项都标 (0B) 占位 | ❌ 0B、占位 | 实为 **56–107 B 的文本配置文件**：1 个 26 动作词表（0000_0000, 107 B）+ 4 个 USB 手柄 profile（60/60/56/56 B）；还遗漏了 `2563_0555` |
| §二 .raw 格式 | "≈ 1280×720 的 RGB565/**带 alpha** 贴图" | ⚠️ "带 alpha" 不实 | 纯 **RGB565 无 alpha**，PIL 实测字节 0 起就是完整全屏一帧 |
| §三 .raw 尺寸 | "≈ 3.2MB / 2.8MB / 2.1MB" | ⚠️ 不精确 | 实测：3,282,288 / 2,911,744 / 2,857,048 / 2,234,144 / 2,274,136 |

§四 BIOS 包 neogeo/pgm 的描述、§二 ui.cfg 的 RecoverRect 内容仍然正确，请放心引用。

---

## 一、扫描范围

`R:\aa` 下共 **14 个 `.zip`**，分 3 类：

| 类别 | 数量 | 文件 |
|------|------|------|
| UI 多语言界面资源包 | 9 | `ui_cn/en/fr/ge/ko/po/ru/sp/ar.zip` |
| 手柄/摇杆配置包 | 1 | `joystick.zip` |
| 街机 BIOS 包 | 4 | `cores/bios/neogeo.zip`、`cores/bios/pgm.zip`、`lib/neogeo.zip`、`lib/pgm.zip` |

> 注意：`cores/filelist.xml` 里引用的 `000/*.zip` 是**游戏 ROM**，它们并不在 cubegm 目录内（`Roms/` 当前为空），与上面这 14 个 zip 无关。

---

## 二、UI 多语言界面资源包（9 个，结构完全一致）

每个包 6 个条目，结构完全相同：

```
ui_xx.zip
├─ menu.raw      (~3.2MB)   主菜单界面位图
├─ game.raw      (~2.8MB)   游戏列表界面位图
├─ setting.raw   (~2.8MB)   设置界面位图
├─ type.raw      (2.2MB)    分类/筛选界面位图
├─ search.raw    (2.1MB)    搜索界面位图
└─ ui.cfg        (242B)     该语言 UI 配置 (INI)
```

**`.raw` 是什么**：预渲染的**界面帧缓冲位图**（不是 XML/矢量布局）。`ui.cfg` 中 `RecoverRect=1278,718` 暗示目标分辨率 **1280×720（720p）**，每个 `.raw` ≈ 1280×720 的 RGB565/带 alpha 贴图。系统把对应 `.raw` 直接当图层渲染到这 5 个界面。

**`ui.cfg` 是什么**（GBK 编码，原文中文注释因编码显示乱码）：
```ini
[Setting]
RecoverRect=1278,718,1,1        # 出错/恢复提示框位置与尺寸
RecoverFontSize=1               # 默认字体大小
RecoverInfo1=.                  # 第一行提示信息
RecoverInfo2=.                  # 第二行提示信息
```

**怎么用**：
- 由 `setting.xml` 的 `<ui name=" " filename="ui_en.zip" gamelist="0"/>` 引用。
- 系统按 `config language` 设置**自动加载**对应语言包，**无需手动解压**。
- 想新增语言：放一个 `ui_xx.zip`（含 5 个 `.raw` + `ui.cfg`），在 `setting.xml` 加一行 `<ui filename="ui_xx.zip" gamelist="1"/>` 即可。
- 9 个包当前除中文 `gamelist=1` 外，其余 `gamelist=0`（上一报告"待开发模块 #6"在此坐实）。

---

## 三、joystick.zip（手柄/摇杆配置，1 个）

```
joystick.zip
├─ joystick.raw          (2.9MB)   手柄设置界面背景图
├─ ui.cfg                (215B)    ★ 按键映射矩阵（核心配置）
├─ 0000_0000            (0B)      手柄标识占位
├─ 0810_0001_0100       (0B)      手柄标识占位
├─ 20bc_5500            (0B)      手柄标识占位
├─ 0810_0001_0110       (0B)      手柄标识占位
└─ （另含 1 个无扩展名 0B 文件）
```

**`ui.cfg` 按键映射矩阵**（这是手柄按键如何对应模拟器功能键的定义）：
```
0,  3,  2, 32,  4, 41, 33, 36,  1, 31, 40, 35, 34, 38, 37, 39,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15
0, 18, 17, 43, 19, 52, 44, 47, 16, 42, 51, 46, 45, 49, 48, 50, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30
1
```
- 结构：**2 行 × 27 列**的映射表，把"手柄物理按键/轴序号"映射到"模拟器功能键码（0–52）"。
- 5 个 **0 字节空文件**（`0000_0000`、`0810_0001_0100`、`20bc_5500`、`0810_0001_0110`）是**手柄 USB 厂商/产品 ID（VID_PID）占位标识**——cubegm 顶层那个 `20bc_5500_0111`（60B）就是同一系列手柄的设备标识。系统检测到手柄后按 VID_PID 匹配这组配置。

**怎么用**：
- `joystick.raw` = 手柄设置界面的背景图；`ui.cfg` = 改键位映射。
- 要支持新手柄：按 `VID_PID` 命名一个空文件放入包内；要改键位：编辑 `ui.cfg` 的映射矩阵（具体键码含义需结合 `rkgame` 的读取逻辑，结构上是一个 2×27 映射表）。

---

## 四、街机 BIOS 包（4 个 = neogeo×2 + pgm×2）

| 文件 | 条目 | 内容 |
|------|------|------|
| `cores/bios/neogeo.zip` | 48 | NeoGeo 街机 BIOS：`271-bios.bin`、`ng-cd.bin`、`sp-45.sp1`(×10)、`uni-bios.31` 等 .bin/.sp1/.rom |
| `cores/bios/pgm.zip` | 6 | IGS PGM 街机 BIOS：`pgm_m01s.rom`(2MB)、`pgm_t01s.rom`(2MB)、`ddp3_bios.u37` 等 |
| `lib/neogeo.zip` | 26 | 另一份（精简版）NeoGeo BIOS：`000-lo.lo`、`asia-s3.rom`、`sfix.sfix` 等 |
| `lib/pgm.zip` | 6 | 与 `cores/bios/pgm.zip` **完全相同**的内容 |

**是什么**：
- `neogeo.zip`：NeoGeo（拳皇、合金弹头等）模拟必需的基板 BIOS。
- `pgm.zip`：IGS PGM 基板 BIOS——**三国战纪、西游释厄传、傲龙传说**等中文街机都依赖它（呼应 `recent.lst` 里的三国战纪）。

**怎么用**：
- 这些是街机核心（`libemu_fbalpha/fba/pgm/cps2/mame2000`）运行对应游戏时**自动从 zip 内读取的基础 BIOS**，libretro 核心支持直接从 zip 内加载，**无需手动解压**。
- **千万不要删除或改名**，否则对应街机游戏无法启动。
- ⚠️ **冗余风险**：`lib/` 和 `cores/bios/` 各存了一份 `neogeo.zip` / `pgm.zip`。`lib/pgm.zip` 与 `cores/bios/pgm.zip` 内容完全一致；`lib/neogeo.zip`(26 条目) 与 `cores/bios/neogeo.zip`(48 条目) 是不同版本。**若后续做更新，注意两处版本可能不一致导致行为差异。**

---

## 五、使用要点总结

| Zip 类别 | 用户需要做的 | 系统怎么消费 | 风险 |
|----------|--------------|--------------|------|
| `ui_xx.zip` | 一般不用管；加语言才需放新包+改 setting.xml | 按 `language` 自动加载 `.raw` 渲染界面 | 改 UI 需重做 `.raw`（无矢量/源码工具） |
| `joystick.zip` | 极少改；改键位编辑 `ui.cfg`，新手柄加 VID_PID 空文件 | 按手柄 VID_PID 匹配映射 | 映射矩阵填错会导致按键错乱 |
| `neogeo/pgm.zip` | **不要动** | 街机核心自动读取 | 删除→对应街机游戏无法启动；两处副本版本不一致 |

**一句话**：这 14 个 zip 都是**"系统自动消费"的资源/配置/依赖包，正常使用时无需手动解压或移动**。你之前看到的目录里那些 `.save`、`.state`、ROM 都不在这 14 个 zip 内——它们是 UI 界面图、手柄按键映射、以及街机 BIOS。

---

## 六、与上一版报告的衔接

- 上一版 `cubegm_analysis.md` 已覆盖：系统身份、架构、功能、接口、待开发模块。
- 本报告的**新增事实**：
  1. UI 是**预渲染 `.raw` 位图**（非 XML 布局），每个语言包 5 张界面图 + 1 个 INI。
  2. `joystick.zip` 内含**2×27 按键映射矩阵** + 按 VID_PID 命名的新手柄标识文件。
  3. BIOS 在 `lib/` 与 `cores/bios/` **双重存放**，存在版本不一致隐患。
  4. `ui.cfg` 的 `gamelist` 标记印证了"多语言游戏列表未全启用"的待开发项。
