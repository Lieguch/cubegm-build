# CubeGM 开源重建 · 交付要求（Delivery Requirements）

> 版本：v1.2 · 与 `PROJECT_CHARTER.md` v1.2 对齐 · 最近更新：2026-08-22
> 本文件定义 1.0 交付物（＝21 条锁定基线全部完成）的"长什么样、怎么验、怎么回滚"。其中 §三.6 的 57 核全表（item 0）属 1.0 必备基线（**非独立 vFinal**）；v1.0＝21 条全部完成＋用户真机验证通过。
> **铁律级需求见 `PROJECT_CHARTER.md` §零（21 条 · 2026-08-22 用户重述锁定基线）；item 0/14 锁定。**
> **架构方向（2026-08-22 用户批准）**：A+B+C 重构取代补丁路线——A 独立 plat_rk3036g 平台层（DRM/ALSA/evdev）、B 设备端 diag 自动诊断闭环、C 单子系统迭代验收。详见 charter §0.4。

---

## 一、1.0 交付物的 SD 卡目录结构

**严格按 `R:\aa\11.png` 截图**。`README.md` 顶部已附图。

```
SD 根
├─ 000/           ← 沿用原设备的分类目录 0（.dat 沿用，禁止重构建）
│  └─ ???.dat     ← 接手 Agent 必须能立刻解释其内容与核心映射（见 §三.1）
├─ 001/
├─ 002/
├─ 003/
├─ 004/
├─ 005/
├─ 006/
├─ 007/
├─ 008/
├─ cubegm/        ← 开源替代系统（替换原厂 rkgame/icube/driver.so + cores/）
│  ├─ picoarch                  ← 主前端可执行
│  ├─ frogui_libretro.so        ← FrogUI 启动器（libretro core 形态）
│  ├─ cores/
│  │  ├─ libemu_mgba.so / fceumm / snes9x / picodrive / nestopia / ...
│  │  ├─ config.xml             ← 核心注册表（必须含 gpsp / prosystem，见 §三.2）
│  │  ├─ filelist.xml           ← ROM→核心映射
│  │  └─ bios/                  ← neogeo.zip / pgm.zip（沿用原设备）
│  ├─ lib/                      ← 第三方库
│  ├─ setting.xml               ← 用户设置（autoscan / autorun）
│  ├─ favorites.lst / recent.lst
│  └─ ui_cn.zip + 其他语言包（按需）
├─ Roms/          ← 9 个目录**之外**的模拟器核心 ROM 工作目录
│  └─ <子目录按核心名>/<rom 文件>
└─ root.dat       ← ⚠️ 原厂启动 UI 资源（WQW\x03 混淆 ZIP，10 条目）
                    启动劫持不得改写此文件或校验分区（红线）
```

### 1.1 9 个 `.dat` 与 `Roms/` 的分工

| 来源 | 路径规则 | 由谁消费 |
|---|---|---|
| `000.dat`–`008.dat` | 在 000–008 各自目录内，列出该分类的 ROM | 启动器按分类号查找 → 列出 → 选定 → 启动核心（核心从 `000/` 读 ROM） |
| `Roms/` | 按核心名分子目录（如 `Roms/gba/`、`Roms/fc/`） | 9 个目录**未覆盖**的核心（如新加的核心、新类型）从 `Roms/` 取 ROM |

> **接手 Agent 必读**：1.0 启动后必须实测 9 个分类的"分类 → 核心 → ROM 路径"全链路通；缺一即阻塞发布。

### 1.2 9 个 `.dat` 内容解析（item 14 必答 · 已锁定映射）

> 来源：用户 2026-08-18 锁定。`000.dat`–`008.dat` 沿用原设备，**不重构建**。本表为已核对的对照关系（详见 `docs/00x_dat_platform_mapping.md` 与 `docs/008_dat_analysis.md` / `007_dat_analysis.md`）。

| 文件 | 平台 | 对应 libretro 核心 | ROM 扩展名 | 备注 |
|---|---|---|---|---|
| `000.dat` | **Arcade（街机）** | fbalpha / mame2000 / pgm / fba / cps2 / extend | ZIP, 7Z | 需 neogeo.zip / pgm.zip BIOS |
| `001.dat` | **NES（红白机）** | FCEUmm / nestopia | NES, FDS, UNIF, UNF | FCEUmm 主；nestopia 备用 |
| `002.dat` | **SFC（超任）** | snes9x 系列 | SMC, SFC, SWC, FIG | snes9x 主力 |
| `003.dat` | **MD（世嘉五代）** | picodrive | BIN, GEN, SMD, MD | |
| `004.dat` | **GBA（掌机）** | mgba / vbam / gpsp | GBA, GB, GBC | gpsp 需补 `<core>` 注册 |
| `005.dat` | **NES 变体 / FC（待核）** | FCEUmm 或 nestopia | NES, FDS | 与 001.dat 区分待 dat 文件核验 |
| `006.dat` | **GB / GBC（掌机）** | tgbdual / mgba | GB, GBC, SGB | TGB Dual 双屏 |
| `007.dat` | **PS1（PSX）**（已确认 28 款游戏） | pcsx_rearmed | BIN, IMG, MDF, PBP, ISO, TOC | 需 BIOS（pcsx_bios） |
| `008.dat` | **Atari 2600**（已确认 9 款游戏） | a2600 / stella | A26, BIN | |

> **`.dat` 文件本身格式**：`WQW\x03` 混淆 ZIP 容器（与 `root.dat` 同款），内含 N 张 **480×320 RGB565** 缩略图 + 1 个 **GBK 编码游戏清单**。FrogUI/picoarch 需新增 WQW\x03 解析器（详见 `008_dat_analysis.md` §八 的最小解析代码）。`007.dat` 缩略图顺序 ≠ 清单顺序（错位情况已确认），需用 ROM 文件名/csize 做匹配（详见 `007_dat_analysis.md` §四）。

---

## 二、版本号方案

- **测试版 `v0.x`**：交付给用户真机测试用的中间版本。已发：`v0.1`（首个全绿构建 `05ee252dfe`）/ `v0.2`（首个含完整门禁的绿构建 `377ac86220`）/ `v0.3`、`v0.3.1`（payload-278 10 项根治 + A+B+C 重构起点，HEAD `39f0c28`，CI run 279）。
- **正式版 `v1.0`（＝21 条基线完成 + 用户真机验证通过）**：第一个**21 条锁定基线全部完成且用户真机全部验证通过**的版本。范围涵盖 item 0（57 核全表 + LDFLAGS_S + 子模块）+ item 14（11.png 目录）+ item 15–18（启动安全 / ABI 门禁 / 策略存档 / 全手柄）+ item 19–21。**这是唯一正式版，不存在独立的「vFinal」。** 判定：SD 卡插入 → 进入自研系统 → 9 分类（000–008）各至少 1 游戏可启动 → 跑 30 秒 → 退出 → 存档 → 读档 → 切下一个 → 全手柄可用 → 57 核各取 1 ROM 实测通过。
- **里程碑 tag**（commit 锚点，可回滚、可组合）：`core/<stage>-<feature>`（如 `core/stage2-perms-fixed` / `core/stage3-sdl-include`）
- **发布 tag**（用户可见）：`v-<feature>`（如 `v-sysroot-writable` / `v-build-good` / `v1.0`）

> **关键澄清（2026-08-18 用户纠正 · 2026-08-22 重述确认）**：**不存在独立于 1.0 之外的「vFinal」。** 21 条需求（含 item 0 的 57 核全表）是完整交付基线，全部必须完成；完成全部 + 用户真机全部验证通过后，版本号方可标记为 `v1.0`。1.0 之前一律为 `v0.x`。

---

## 三、验收清单（Acceptance Checklist · 1.0 发布前必须 100% 通过）

### 3.1 系统层

- [ ] 真机 SD 卡插上 → 自动进 FrogUI 菜单（不卡 logo、不报 `sdcard is damaged`）
- [ ] `root.dat` 内容在启动后**未被修改**（sha256 与出厂一致）；`sdcard is damaged` 校验通过
- [ ] 启动劫持脚本（`cubegm/zhijack.sh` 等）**仅**劫持 `autorun`，不触碰 `root.dat` / 校验分区
- [ ] 每次提交构建产物前 `tools/verify_target_abi.sh` 通过（EM_ARM / 0x5000400 / ≤ GLIBC_2.17）

### 3.2 平台后端（picoarch · v1.2 起按 A 方向重构）

- [ ] `picoarch` 用 `platform=rk3036g` 编译（独立 `plat_rk3036g.c`：DRM 显示 + ALSA 音频 + evdev 输入；脱离 SF2000 的 cubevol shm / /dev/dis / 专用 driver.so 路径）
- [ ] 注入 `-DSCREEN_WIDTH=1280 -DSCREEN_HEIGHT=720 -DSCREEN_BPP=2` 生效
- [ ] 游戏输入走 libpicofe 原生 in_evdev；绑定可在设备菜单重绑并存 config（`in_config_parse_dev` 实证）；**自研 shm 输入注入已移除**
- [ ] 视频：DRM/KMS modeset 成功，输出到 HDMI（分辨率按 §四决策）
- [ ] 音频：ALSA `snd_pcm_*` 打开成功，出声
- [ ] 输入：evdev 打开 `/dev/input/event*` 成功，面板按键 + USB 手柄均可触发动作
- [ ] **方向 B 闭环**：`diag` 工具（armhf）输出 `diag_report.txt`（逐键真实键码/轴/HAT、显示测试图、1kHz 测试音、逐 core 加载），作为每次实机验证的**事实底座**（禁止无 diag 事实的修复）

### 3.3 核心与游戏

- [ ] 9 个分类（000–008）各**至少 1 个**游戏能启动 → 跑 30 秒 → 退出
- [ ] `gpsp`（GBA）+ `prosystem`（Atari 7800）**已注册**到 `config.xml`（直接补 `<core>` 块）
- [ ] 策略游戏（占位：以 `cores/sfc/Super_World_War_J.sfc` 或同类 RPG/SLG 为例）能存档、能读档
- [ ] 默认核心集合（`DEFAULT_CORES`）至少含：mgba / snes9x / fceumm / picodrive / nestopia

### 3.4 手柄与输入

- [ ] 面板内置 13 键（d-pad + SELECT/START + A/B + X/Y + TL1/TR1 + RESET）全部生效
- [ ] USB 手柄 VID:PID 已映射（沿用原 `joystick.zip` 的 4 个 profile：0810:0001:0100/0110、20bc:5500、2563:0555）
- [ ] 新手柄支持路径明确（"加一个 `VID_PID_REV` 文件写映射"）

### 3.5 GitHub Actions 预算

- [ ] 每次 push → 跑通一次即结束；失败立即定位修复，**不允许**"取消重试碰运气"
- [ ] 月度用量监控（< 200 分钟/周为绿区；200–500 黄区；> 500 红区即停手分析）
- [ ] 已通过 tag（如 `core/stage2-perms-fixed`）可一键回滚到已知绿构建

### 3.6 57 核全表验收清单（item 0 · 属 1.0 必备基线 · 非独立 vFinal）

> **更正（2026-08-18 用户纠正 · 2026-08-22 重述确认）**：本节不是"1.0 之后"的额外阶段，而是 **1.0 必备基线的一部分**（item 0 锁定的 57 核全表 + LDFLAGS_S + 子模块）。21 条基线全部完成 + 用户真机全部验证通过后，方可标记 `v1.0`（不存在独立 vFinal）。
> 基线目标 = 57 核全表 + LDFLAGS_S + 子模块，详见 item 0 原文（属 1.0 必备交付，非独立阶段）。

- [ ] **全 57 核**按 `tzubertowski/treefrog-ui/cores.md` 列表完成 ARM armhf 交叉编译
- [ ] **C++ 核心**（beetle_psx / beetle_pce / beetle_supergrafx / beetle_wswan / beetle_ngp / beetle_vb / stella2014 / prosystem / cannonball / ecwolf / frodo / uae / o2em / castaway / freechaf / geolith / vitaquake2 / ...）通过 `LDFLAGS_S = $LDFLAGS -shared -lstdc++` 链接
- [ ] **子模块核心**（tic80 / ecwolf / freechaf / fake08 / pcsx_rearmed / mgba / arduous / picodrive / ...）经 `git clone --recursive` 完整拉取
- [ ] 9 个分类 + `Roms/` 全 ROM 路径规则在 57 核下端到端通（含扩展核心）
- [ ] 全部 57 核 + 前端 ABI 门禁 + libretro 符号门禁 + payload 完整性门禁均 PASS
- [ ] 真机 57 核各取 1 ROM 实测：启动 → 跑 30 秒 → 退出 → 存档 → 读档 → 无崩溃
- [ ] 完整 57 核列表与 `cores/config.xml` 注册匹配（无失效条目；扩展注册按 FrogUI/picoarch 源码核实）
- [ ] 策略游戏存档（item 17）覆盖 57 核中所有有存档需求的核心
- [ ] 全部通用手柄 + 自定义映射（item 18）覆盖 4 个原厂 profile 之外的 USB HID
- [ ] 验证通过后标记 `v1.0` tag + 维护 stable 分支

---

## 四、显示分辨率决策（待 datasheet 阅读后落定）

| 候选 | 1.0 暂定 | 备注 |
|---|---|---|
| **1280×720** | ✅ 1.0 首选 | UI .raw 资源天然匹配；最小风险 |
| 1920×1080 | 1.x 增量 | datasheet 确认 HDMI 支持 1080P 后开启；UI 层做 720p→1080p 上采样 |
| 其他 | 不考虑 | 设备 HDMI 控制器未验证 |

> 接手 Agent 读完 datasheet 后，若有更新 → 修订本节（v1.1）+ 在 `docs/HANDOFF_CURRENT.md` 留痕，并对应调整 `build_sf3000_armhf.sh` 里的 `-DSCREEN_*` 与 `cubegm/ui_cn.zip` 资源。

---

## 五、里程碑与回滚组合

| M | 名称 | 入口 tag | 验收 | 回滚方式 |
|---|---|---|---|---|
| **M0** | crosstool-NG + glibc 2.17 sysroot | `core/stage1-sysroot` | sysroot 完整、含 drm/alsa/evdev 头 | 回到 `core/stage1-sysroot` 重建 |
| **M1** | SDL + libpng + alsa-lib 进 sysroot | `core/stage2-libs` | libSDL.so / libasound.so / libpng12.so 可链 | 回到 M0 重建 STAGE2 |
| **M2** | 平台后端：unix 通、绿构建 | `core/unix-backend-green` | picoarch 编译通过、ABI 正确 | 回到 M1 + 重写 `build_sf3000_armhf.sh` |
| **M3** | FrogUI 启动器 + 9 分类全链路 | `core/frogui-bridge` | 选游戏 → 跑 30s → 退 | 回到 M2 |
| **M4** | 输入：evdev + USB 手柄全 profile | `core/inputs-full` | 面板 13 键 + 4 个 USB profile 全通 | 回到 M3 |
| **M5** | 存档 / 即时存档 / Quick Resume | `core/save-state-full` | state/save/load 端到端 | 回到 M4 |
| **v1.0** | 全功能正式版 | `v1.0` | §三 验收清单 100% 通过 | 回到任一 M tag |

> **可组合性**：所有 `core/*` tag 都从 main HEAD 快进；任何 `v-*` 标签也是 main 快照。回滚即 `git reset --hard <tag>`（本地）或 Git Data API 创建反 commit（远端）。

---

## 六、构建产物规范

| 文件 | 路径（在 release 中） | 校验 |
|---|---|---|
| `picoarch` ELF | `cubegm/picoarch` | `readelf -h` 出 EM_ARM / `tools/verify_target_abi.sh` |
| `frogui_libretro.so` | `cubegm/frogui_libretro.so` | 同上 |
| `libemu_*.so` 核心集 | `cubegm/cores/` | 同上 + dlsym 列出 `retro_*` 必须包含改版 ABI 五件套 |
| `setting.xml` / `config.xml` | `cubegm/` / `cubegm/cores/` | XML 合法 + 含 gpsp / prosystem 块 |
| 启动包（含 zhijack.sh） | `deploy/cubegm/` | sha256 落 `BUILD_REPORT.txt` |

---

## 七、不在 1.0 范围（明确延后）

- 全 9 语言 UI 全开（仅中文 `gamelist=1`）
- OTA 升级通道
- 金手指/作弊
- 收藏夹/历史列表数据迁移
- 多玩家存档槽位（仅默认 1 槽）
- 自定义键位 GUI（仅保留 `joystick.zip/ui.cfg` 文本编辑路径）

> 以上列项的"不做"必须**写进 release notes**告知用户，避免期望偏差。

---

## 八、修订记录

| 版本 | 日期 | 修订人 | 修订内容 |
|---|---|---|---|
| v1.0 | 2026-08-17 | 主理人 | 初版（与 Charter v1.0 同步） |
| **v1.1** | **2026-08-18** | **主理人** | **与 Charter v1.1 同步：明确 v1.0＝19 条基线完成＋真机验证、取消 vFinal 拆分；新增 §一.2 9 个 .dat 内容解析对照表；新增 §三.6「57 核全表验收（属 1.0 必备基线）」。更正此前"57 核＝vFinal"的错误。** |
| **v1.2** | **2026-08-22** | **主理人** | **与 Charter v1.2 同步：19 条→21 条基线（item 19/20/21 新增）；§3.2 平台后端改为 A 方向（platform=rk3036g 独立平台层）；新增方向 B（diag 自动诊断闭环）验收项；版本方案补充 v0.3/v0.3.1。** |
