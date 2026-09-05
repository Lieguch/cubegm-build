# CubeGM 独立 Agent 执行任务书（SOLO HANDOFF · v1.0 全程）

> **本文件版本**：v2.0 · 2026-08-22 · 用户批准方向 A+B+C
> **读者**：接手本项目的**唯一执行 Agent**。你不是"协助者"，你是**从此刻到 v1.0 的独立负责人**。本任务书 + 随包文件 = 你所需的全部上下文；读完即可自动开始工作，不需要联系前一个 Agent。
> **你的交付**：在 RK3036G 真机上跑起来的、功能对等甚至超越原厂 CubeGM 的开源系统（模仿 TreeFrogUI/picoarch/FrogUI），移植 57 核全表 + LDFLAGS_S + 子模块，通过全部 21 条锁定需求 + 用户真机验证 → 标记 v1.0。
> **铁律**：21 条锁定需求（`PROJECT_CHARTER.md` §零）凌驾一切；违反 = 设备可能变砖（见 §3 禁忌清单）。

---

## 0. 开工第一步（解压后 30 分钟内的动作序列）

1. 解压本包 → 得到 `CubeGM_SOLO/` 目录（下文路径均相对此目录）。
2. **按顺序读**（约 40 分钟）：
   1. `00_START_HERE.md`（本入口，看完即开工）
   2. `PROJECT_CHARTER.md` §零（21 条铁律，最高依据）→ §0.4（A+B+C 方向）
   3. `DELIVERY_REQUIREMENTS.md`（1.0 交付定义 + 验收清单）
   4. `docs/00x_dat_platform_mapping.md`（item 14 的 9 分类映射）
   5. `memory/MEMORY.md`（项目铁律 + **禁忌清单**——必须背下来）
   6. `HANDOFF.md`（历史 + 交接约定）
3. **核对环境**：确认 `source/TreeFrogUI_picoarch`（补丁已应用状态）与 `git/cubegm-build.bundle`（完整历史）可用。
4. **开工顺序**：先做方向 B（diag 实机事实）→ 方向 A（plat_rk3036g）→ 方向 C 贯穿。详见 §6。

---

## 1. 项目现状（你接手时的真实状态）

### 1.1 硬件（已逆向实证，勿再怀疑）

- RK3036G：双核 Cortex-A7 @~1.0-1.2GHz，ARMv7-A，NEON+VFPv4，Mali-400MP，**无屏仅 HDMI**。
- 系统：标准 buildroot Linux —— **DRM/KMS 显示、ALSA 音频、evdev 输入**（driver.so 字符串实证）。
- 二进制 ABI：全部 armhf（e_flags=0x5000400）；**glibc 天花板 2.17**（20 个设备核心实测）。
- HDMI：实测 1280x720@60 modeset 成功；datasheet 支持最大 1920×1080（分辨率决策见 §6.2-W3）。

### 1.2 已完成的（不要重做）

| 项 | 状态 |
|---|---|
| 启动劫持链（rkgame→autorun→zhijack.sh→picoarch+FrogUI） | ✅ 多轮实机成功，root.dat 未动 |
| 显示：hwdisp.c DRM（RGB565 dumb buffer + aspect-fit + 竖屏旋转预计算 + O_CLOEXEC） | ✅ 已验证 |
| 音频：plat_sound_init_alsa（dlopen+set_params+全局重置）+ `-Wl,--export-dynamic` | ✅ 已验证 |
| 输入：cubevol_bridge（evdev→shm 供 FrogUI）+ evdev 绑定表对齐原厂 profile + 游戏输入 shm 双通道 | ✅ 已验证（过渡方案） |
| diag 设备诊断工具（deploy/diag.c，五模块，diag.flag 触发） | ✅ 已实现，待实机 |
| 9 个 dat 映射（docs/00x_dat_platform_mapping.md） | ✅ 已锁定（005 待字节核验） |
| 全量补丁（patch/picoarch_rk3036g_full.patch + frogui_rk3036g_full.patch） | ✅ 基于 pin commit，单补丁应用 |

### 1.3 未解决（你的工作）

- **实机事实缺失**：真实键码表、HDMI mode 全集、ALSA 子进程结果、57 核逐个健康（→ diag 回答）。
- **架构错位**：当前仍跑在 SF2000 平台代码（plat_sf3000.c/plat_sdl.c）的过渡补丁上 → 方向 A 重构为独立 plat_rk3036g.c。
- **验收未达**：21 条基线逐条核对未完成；57 核未全移植；性能/全手柄/策略存档未验收。

---

## 2. 为什么必须重构（前车之鉴，避免重蹈）

- 前 30+ 次补丁均未达验收，用户判定"穷举试错"。根因：
  1. **架构错位**：在 SF2000 专用代码（cubevol shm 输入 / /dev/dis 显示 / 专用 driver.so 音频）上用补丁强适配标准 Linux 的 RK3036G（DRM/ALSA/evdev）→ 每层残留 SF2000 假设，修 A 坏 B。
  2. **无设备端验证**：键码/显示/音频全靠日志猜。
- **你的纪律**：①同一点第二次失败 → 停手联网查官网/官方论坛（铁律 item 20）；②一切修复基于 diag 实测事实；③单子系统迭代（方向 C），一次只动一个子系统。

---

## 3. 铁律与禁忌清单（不遵守 = 变砖）

### 3.1 21 条铁律（charter §零，原文为准，此处为执行要点）

- item 0/14 **LOCKED**：57 核全表 + 11.png 目录结构，1.0 必备，不可压缩。
- item 2/3：禁止猜测/穷举；修复必须联网查官方文档后"解决式"输出（改哪个文件哪行、为什么、依据哪个源）。
- item 4-7：极致压缩聊天、保留关键上下文、省 token、减少 AI 调用（防限流）。
- item 8：支持官方全部核心，榨干硬件。
- item 9：CI 每月 2000 分钟，每次 push 一次对；失败禁止"取消重试碰运气"。
- item 10/11：立项/交付/交接文件齐全；软件工程全流程（版本/里程碑/验收/需求/方案/提交）。
- item 12：除铁律外自动静默完成检查/复盘/定位/修正/测试/上传/构建；自动删失效监控对话。
- item 13：HDMI 分辨率（1080P vs 720P）须 datasheet+diag 实证落定。
- item 15-18：root.dat 绝不改写；每次构建 ABI 门禁；策略存档全实现；全手柄+自定义映射。
- item 19-21：目标不可压缩；二次失败必联网；删除动作先自检。

### 3.2 禁忌清单（血泪教训，最高优先级）

| 禁忌 | 后果（已发生） | 正确做法 |
|---|---|---|
| 改名/删除原厂 `icube`/`rkgame`/`driver.so`/`cores/libemu_*.so`/`root.dat` | "sdcard is damaged" 或卡启动画面变砖（260 曾因改名 icube→icube.bak 卡死） | 一律保留原厂文件；只覆盖**自己构建的同名文件**；对抗原厂进程用 kill/SIGSTOP |
| 改写 root.dat/校验分区 | 系统损坏校验失败 | 只读沿用；CI 检查哈希不变 |
| 链接 glibc > 2.17 | core 加载失败 | 每次构建 `verify_target_abi.sh` 强制 |
| 删除已认可里程碑文件（HANDOFF/VERSION/tag） | 失去回滚锚点 | 受保护，禁止删 |

---

## 4. 可行性分析（基于逆向实证，结论：可行）

| 项 | 可行性 | 证据 | 风险 |
|---|---|---|---|
| 启动劫持 | ✅ 已验证 | 多轮 payload 实机成功；root.dat 哈希未变 | 中：R1 |
| DRM 显示 | ✅ 已验证 | SETCRTC→HDMI 白色帧可见 | 低 |
| evdev 输入 | ✅ 可达 | bridge 日志事件到达；映射 diag 校准 | 低-中 |
| ALSA 音频 | ✅ 基本可达 | 父进程 open 成功；EBUSY 已修 | 中：R6 |
| 57 核移植 | ✅ 可行 | 20 原厂核 armhf 同 ABI；CI 已编 19 核 | 中：R7 |

**风险登记册（执行中持续对照）**：

| # | 风险 | 缓解 | 回滚 |
|---|---|---|---|
| R1 劫持脚本错→开机卡死 | 双 SD 卡（工作+原厂备份）；zhijack 看门狗回退；拔卡查日志 | 换备份卡 |
| R2 误写 root.dat | 铁律禁止 + CI 哈希检查 | 备份卡 |
| R3 原厂二进制被改 | 铁律禁止 + git 快照 | 备份卡 |
| R4 glibc 错配 | 每次构建 ABI 门禁 | 回退 tag |
| R5 键码错 | diag 实测写表 + 菜单可重绑 | 删 config |
| R6 ALSA 无声 | diag audio 确认；必要时直写 hw 设备 | 回退音频 |
| R7 个别 core 慢 | _plus 核心 + frameskip + 逐核验收 | 换 core |
| R8 CI 超支 | 预算纪律（§9） | 停 push 分析 |

---

## 5. 目标架构（方向 A：plat_rk3036g）

```
RK3036G (标准 buildroot Linux)
┌─────────────────────────────────────────────────────────┐
│ 启动链: rkgame(原厂) → autorun → zhijack.sh(劫持)        │
│   ├─ SIGSTOP icube / kill rkgame（不删不改原厂）         │
│   ├─ cubevol_bridge (evdev→/tmp/joy_key shm) → FrogUI   │
│   └─ picoarch (platform=rk3036g)                        │
│        ├─ plat_rk3036g.c ← 新平台层（方向A核心）          │
│        │    ├─ 显示: DRM/KMS dumb buffer (hwdisp_drm)   │
│        │    ├─ 音频: ALSA dlopen (snd_pcm_*)            │
│        │    └─ 输入: libpicofe in_evdev (原生,可重绑)    │
│        ├─ libretro cores (cubegm/cores/*.so, armhf)     │
│        └─ FrogUI 启动器 (读 shm, 渲染菜单)              │
│ 诊断: cubegm/diag (一次性自检 → diag_report.txt)          │
└─────────────────────────────────────────────────────────┘
```

**与现有代码的关系**：
- `hwdisp.c` DRM/旋转/CLOEXEC、`plat_sdl.c` ALSA → **吸收**进 plat_rk3036g。
- `core.c` 游戏输入的 shm OR 注入 → **移除**（改走原生 evdev）。
- `cubevol_bridge.c` → 保留（FrogUI 菜单输入依赖）。
- 全量补丁 → 重构完成后重新生成（基于 pin commit）。

---

## 6. 执行路线图（按序执行，每步 DoD 通过才进下一步）

### 阶段 B：diag 事实底座（先做）

| 步骤 | 动作 | 完成定义（DoD） |
|---|---|---|
| B0 | 确认 `deploy/diag.c` 编译进 payload（CI 产物 cubegm/diag 存在 + ABI 通过） | CI 绿 |
| B1 | 交付 payload 给用户，附操作说明：SD 根放空文件 `diag.flag` → 开机自动跑 `diag all` → 回传 `/mnt/sdcard/diag_report.txt` | 用户回传 |
| B2 | 将 diag_report.txt 落盘为 `docs/diag_findings_<date>.md`，提取：真实键码表、HDMI mode 集、ALSA 结果、core 健康清单 | 文档完成 |
| B3 | 分辨率决策：diag mode 集 + datasheet → 更新 charter §二（item 13 落定） | 决策记录 |
| B4 | 9 个 dat 字节核验（如用户提供原卡）：修正 `docs/00x_dat_platform_mapping.md`（item 14） | 映射表更新 |

### 阶段 A：平台层重构（基于 B 事实）

| 步骤 | 动作 | 完成定义（DoD） |
|---|---|---|
| A1 | 新建 `plat_rk3036g.c`（仿 plat_linux.c 结构，PLATFORM_RK3036G 宏隔离）+ Makefile `platform=rk3036g` 分支 | ✅ CI #308+ 可编译链接 |
| A2 | 输入：in_evdev 默认绑定表 = diag 实测键码；移除 core.c shm OR | ✅ 权威键码表（cubevol_bridge.c 0810） |
| A3 | 显示：hwdisp_drm 挂接 plat_rk3036g；构建切换 `platform=rk3036g` | ✅ CI #316 真编译通过（6 轮错误链见 a3-error-chain） |
| A4 | 音频：ALSA 路径独立化 + fork 前释放 | ✅ CI #318 真绿（63 PASS/0 FAIL，dlopen libasound + SDL fallback） |
| A5 | FrogUI 全链路（菜单+启动+返回） | 🔧 代码就绪（fork+execl 启动 <-> 退出 execl 回菜单），**待实机验证** |
| A6 | 性能调优（frameskip/编译 flags/逐核验收） | ⏳ 待 A5 实机通过后启动 |

### 阶段 C：单子系统迭代验收（贯穿）

```
顺序: 输入 → 显示 → 音频 → 性能
每步: 独立分支 → 实现 → CI 绿 → payload → 用户实机反馈 → 通过才进下一项
失败: 同一点第二次失败 → 停手联网查官方源（item 20）
```

### 阶段 M8-M10：57 核 + 手柄 + v1.0

| 里程碑 | 交付 |
|---|---|
| M8 | 57 核全表构建 + 策略存档 + ABI/符号/payload 门禁 |
| M9 | 全手柄 + 自定义映射（原厂 4 profile + VID_PID_REV 路径） |
| M10 | 21 条基线全过 + 用户真机全验 → 打 `v1.0` tag + stable 分支 |

---

## 7. 版本控制与里程碑

### 7.1 版本号

- 测试版 `v0.x`（交付实机验证）；**v1.0 = 21 条基线完成 + 用户真机全验通过**（唯一正式版，无 vFinal）。
- 子系统版本：`v0.4-input` / `v0.5-display` / `v0.6-audio` / `v0.7-perf` / `v0.8-57cores` / `v0.9-input-full`。

### 7.2 分支与 tag

- `main` = 稳定主线（只合入已验证子系统）；子系统用 `feat/<subsystem>` 分支。
- tag：里程碑 `core/<stage>-<feature>`；发布 `v-<feature>` / `v1.0`。
- commit：原子提交，消息含"为什么"；禁止 `--no-verify`。

### 7.3 回滚

| 场景 | 手段 |
|---|---|
| 代码问题 | git revert / checkout 上一 tag |
| 开机失败 | 换原厂备份 SD 卡 |
| payload 问题 | 上一 payload zip（GitHub Release 留档） |

### 7.4 里程碑总表（M0-M10）

| # | 内容 | DoD |
|---|---|---|
| M0 | 封存+任务书（本包） | 你可独立开工 |
| M1 | diag 实机事实 | diag_findings_*.md 完整 |
| M2 | 9 dat 核验 | 映射表修正 |
| M3 | 分辨率决策 | charter §二 落定 |
| M4 | 输入子系统 | v0.4-input 实机通过 |
| M5 | 显示子系统 | v0.5-display 实机通过 |
| M6 | 音频子系统 | v0.6-audio 实机通过 |
| M7 | 性能达标 | v0.7-perf 实机通过 |
| M8 | 57 核全表 + 存档 | v0.8-57cores 全核可玩+存档可用 |
| M9 | 全手柄+映射 | v0.9-input-full 实机通过 |
| M10 | v1.0 完整验收 | 21 条基线 + 用户全验 → tag v1.0 |

---

## 8. 软件工程流程（每个子系统）

```
需求分析(PRD) → 实施方案(设计) → 实现(代码) → 验证(CI+ABI+实机) → 验收(DoD) → 发布(tag+release) → 交接(HANDOFF更新)
```
- 需求/设计/任务/测试文档落盘：`docs/prd|arch|tasks|test/`。
- 代码注释含依据（文件:行:为什么，引用官方源）。
- 每次版本更新 `HANDOFF.md` + `VERSION`。

---

## 9. CI 预算纪律（2000 分钟/月，红线）

- 全量构建 ≤6 次/月（~120-150 分钟/次，仅里程碑验证用）。
- 增量构建 ≤8 次/月（build.sh 支持 `CORES` 过滤）。
- **失败零重试**：本地能验证的（bash -n / 语法 / 补丁 apply --check）绝不上 CI；失败先读 `BUILD_REPORT.txt`（build-output 分支）本地定位再推。
- 周用量 >500 分钟 = 红区，停手分析。

---

## 10. 应急与回滚剧本

| 场景 | 处置 |
|---|---|
| 开机卡死 | 拔卡电脑查 zhijack.log/diag_report.txt → 修 → 换备份卡恢复测试 |
| "sdcard is damaged" | root.dat 被改 → 备份卡恢复，禁止继续刷机 |
| 半白/黑屏 | 回退上一 tag 重打包；附 diag display 结果 |
| CI 红 | 读 BUILD_REPORT.txt，本地复现修复后单次推送 |
| 用户反馈新问题 | 先跑 diag 拿事实 → 按 §6 流程修复 → 不猜测 |

---

## 11. 你的完成定义（v1.0 判定，全部为是才可打 tag）

- [ ] 21 条铁律基线逐条核对通过（charter §零）
- [ ] 9 分类（000-008）各至少 1 游戏启动→30s→退出→存档→读档
- [ ] 57 核全表构建 + LDFLAGS_S + 子模块 + 三性门禁（ABI/符号/payload）PASS
- [ ] 策略游戏存档覆盖（item 17）
- [ ] 全手柄 + 自定义映射（item 18）
- [ ] root.dat/校验分区哈希不变（item 15）+ 每次构建 ABI 门禁（item 16）
- [ ] 11.png 目录结构交付（item 14）+ HDMI 分辨率落定（item 13）
- [ ] **用户真机全部验证通过** → 打 `v1.0` tag + stable 分支
- [ ] 更新 HANDOFF.md（你的工作交接 + 下一步建议）

---

## 12. 你的交接义务

- 每个里程碑结束：更新 `HANDOFF.md` + `VERSION` + 本任务书状态（勾选完成项）。
- 你离场/交班时：写清"当前进度、未完成项、下一步从哪里开始、踩坑记录"——让下一个 Agent 零成本接手（你接手时受益于同样的机制）。
- 涉及删除动作：先列清单确认（item 21）。

---

## 附录 A：随包文件索引

```
00_START_HERE.md          ← 本入口（即本文件）
PLAN_A_B_C_REFACTOR.md    ← 本任务书
PROJECT_CHARTER.md        ← 21 条铁律（最高依据）
DELIVERY_REQUIREMENTS.md  ← 1.0 交付定义 + 验收
HANDOFF.md                ← 历史交接 + 约定
VERSION / README.md       ← 版本语义 / 项目速览
patch/                    ← 全量补丁（picoarch/frogui，单补丁应用）
deploy/                   ← 构建脚本 + diag.c + cubevol_bridge.c + cubegm/ 配置
docs/                     ← 全部分析（dat 映射 / input_and_ui / root_dat 等）
memory/                   ← 项目铁律 + 禁忌清单 + 工作日志
source/                   ← 上游源码（补丁应用后状态）
git/cubegm-build.bundle   ← 完整 git 历史（可 clone/回滚）
```

## 附录 B：本任务书变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| v1.0 | 2026-08-22 | 初稿（多 Agent 协作版） |
| **v2.0** | **2026-08-22** | **改为"单独 Agent 独立执行"视角：新增 §0 开工第一步、§3 禁忌清单前置、§11 完成定义、§12 交接义务；移除多 Agent 并行章节，全程单人路线图。** |
