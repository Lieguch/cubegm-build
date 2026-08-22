# CubeGM 重构立项实施计划（A+B+C · 2026-08-22 用户批准）

> **文档版本**：v1.0 · 日期：2026-08-22 · 批准：用户（"我选择A+B+C"）
> **用途**：供**多个 Agent 并行接手**（用户将用本计划同时启动其他 Agent）。任何 Agent 读完本文即可开工，无需重新探索。
> **铁律**：服从 `PROJECT_CHARTER.md` §零（21 条锁定需求）与 `DELIVERY_REQUIREMENTS.md`（1.0 交付定义）；冲突以 charter 为准。
> **封存点**：commit `39f0c28`（v0.3.1，补丁路线终点）→ 当前 HEAD `bc691ab`（含 diag 工具，重构起点）。

---

## 一、项目背景与目标

### 1.1 背景（为什么重构）

- 原路线（v0.3.1 前 30+ 次补丁）**未达验收**，用户判定为"穷举试错"。
- 根因（实证）：**架构错位**——在 SF2000 专用平台代码（`plat_sf3000.c`/`plat_sdl.c` 的 cubevol 共享内存输入、`/dev/dis` 专用显示、专用 `driver.so` 音频）上用补丁强适配标准 Linux 的 RK3036G（DRM/KMS、ALSA、evdev）。每层残留 SF2000 假设，修 A 坏 B。
- 次因：**无设备端验证手段**——键码/显示/音频全靠日志推断，一轮实机往返周期长信息残 → 只能"猜→补丁→猜错→再补丁"。

### 1.2 目标（item 0 原文）

模仿 TreeFrogUI / picoarch / FrogUI 这套开源代码，构建功能对等甚至超越原厂 CubeGM 固件的系统；**在原硬件上打游戏**。移植 57 核全表 + LDFLAGS_S + 子模块。

### 1.3 三方向总览（用户批准）

| 方向 | 名称 | 一句话 | 优先级 |
|---|---|---|---|
| **B** | 设备端自动诊断闭环 | `diag` 工具开机测量真实键码/显示/音频/core 事实，消灭猜测 | **先做**（事实底座） |
| **A** | 平台层重构 | 新建独立 `plat_rk3036g.c`（DRM+ALSA+evdev），脱离 SF2000 专属路径 | 基于 B 事实 |
| **C** | 单子系统迭代验收 | 输入→显示→音频→性能，一次一个子系统、独立版本、独立实机验证 | 贯穿全程 |

---

## 二、现状与逆向实证基础

> 以下全部为**已完成实测/反编译证据**（`docs/`、`memory/`、实机日志），非推断。

### 2.1 硬件与固件身份（ELF 反编译实证）

| 项 | 证据 | 来源 |
|---|---|---|
| SoC | RK3036G · 双核 Cortex-A7 @~1.0-1.2GHz · ARMv7-A · NEON+VFPv4 · Mali-400MP | 用户确认 + ELF |
| 系统 | 标准 buildroot Linux（非 MIPS） | driver.so 字符串 |
| 7 个设备二进制 | 全部 32-bit ARM，e_machine=0x28，e_flags=0x5000400 = armhf + EABIv5 | cubegm_elf_analyze.py |
| glibc 天花板 | **2.17**（20 个设备核心实测） | tools/probe_glibc.py |
| 显示 | 标准 DRM/KMS（/dev/dri/card0 + dumb buffer），无 /dev/dis | driver.so 字符串 + 日志 |
| 音频 | 标准 ALSA；声卡 rockchiphdmi | driver.so 字符串 + /proc/asound |
| 输入 | 标准 evdev（无原厂 cubevol 守护进程） | driver.so 字符串 + 日志 |
| 输出 | 无屏仅 HDMI；实测 1280x720@60 modeset 成功 | picoarch_init.log |

### 2.2 启动链与劫持安全（多轮实机验证）

```
rkgame(原厂,保留) → setting.xml autorun → cubegm/cores/libemu_md.so(=tfhijack)
   → fork zhijack.sh → SIGSTOP icube + kill rkgame
   → cubevol_bridge(evdev→/tmp/joy_key shm, 供 FrogUI) + picoarch(FrogUI)
```
- root.dat/校验分区**从未改写**（sha256 未变），多轮 payload 实机**无** "sdcard is damaged"。
- **禁忌（变砖教训）**：改名/删除原厂 icube/rkgame/driver.so/cores/libemu_*.so/root.dat → 损坏校验或启动链断裂（260 曾因改名 icube 卡死）。**严禁触碰**。
- 对抗原厂进程用 kill/SIGSTOP，绝不用改名/删除。

### 2.3 原厂资源与核心（已解析）

- `root.dat` = WQW\x03 混淆 ZIP，10 条目，只读沿用。
- 9 个 `.dat` 映射已锁定（`docs/00x_dat_platform_mapping.md`）：000=街机(fbalpha/mame2000/cps2/pgm)、001=NES(fceumm/nestopia)、002=SFC(snes9x系列)、003=MD(picodrive)、004=GBA(mgba/vbam/gpsp)、005=NES变体(待字节核验)、006=GB/GBC(tgbdual/mgba)、007=PS1(pcsx_rearmed)、008=Atari2600(stella)。9 分类外核心指向 `Roms/`。
- 原厂 20 个 libemu_*.so 均 armhf、glibc≤2.17；config.xml 注册 18 个；gpsp+prosystem 未注册（补 `<core>` 启用）。
- 手柄 profile：joystick.zip = 26 动作词表 + 2×27 ui.cfg 扫描码矩阵 + 4 个 USB profile。新手柄 = 加 VID_PID_REV 文件。

### 2.4 实机问题根因（payload-278 已定位，v0.3.1 修复有效）

| 问题 | 根因（实证） | v0.3.1 状态 |
|---|---|---|
| UI 上/左失效 | bridge 把轴值 0 误判"未插" | ✅ 按 EVIOCGBIT 位图判轴 |
| 游戏内按键错位 | evdev 绑定表 0x120-0x12b 错位 | ✅ 对齐原厂 profile + shm 双通道 |
| 无声 | 无 -rdynamic → dlsym 失败 → PCM 未释放 → EBUSY | ✅ -Wl,--export-dynamic |
| 竖屏 25FPS | 旋转逐像素 64 位乘除 | ✅ 预计算像素表 |
| 半白屏 | fork+exec 继承 DRM fd，退出 close 父 fd | ✅ O_CLOEXEC + RMFB |
| 002 约 1FPS | 设备上 snes9x2005 未优化 | ✅ 映射 _plus |
| 005 黑屏 | 005 用 fceumm，原厂 nestopia | ✅ 映射 nestopia |

> **过渡定位**：v0.3.1 修复是重构过渡；方向 A 的 plat_rk3036g **吸收**其中 DRM/ALSA/输入逻辑，**取代** SF2000 专属残留。

---

## 三、可行性分析（核心）

> 原系统封闭（rkgame/icube 闭源+加密），"稍有出错即无法启动"。基于 §二 逆向实证逐项评估。

### 3.1 分方向可行性

| 方向 | 可行性 | 关键证据 | 风险 |
|---|---|---|---|
| B（diag） | ✅ 高 | 只读 SD + 独立小程序，不碰原厂文件；已实现 bc691ab | 极低 |
| A 启动劫持 | ✅ 已验证 | 多轮 payload 实机劫持成功，root.dat 未动 | 中（R1） |
| A DRM 显示 | ✅ 已验证 | SETCRTC 成功→HDMI 白色帧可见 | 低 |
| A evdev 输入 | ✅ 已验证可达 | bridge 日志事件到达；映射用 diag 校准 | 低-中 |
| A ALSA 音频 | ✅ 基本可达 | 父进程 dlopen 成功；子进程 EBUSY 已修 | 中（diag 确认） |
| A 核心运行 | ✅ 已验证 | 多 core 实机加载运行；ABI 门禁全绿 | 中（性能/兼容） |
| C 迭代验收 | ✅ | 流程性 | 低 |

### 3.2 风险登记册（Risk Register）

| # | 风险 | 影响 | 触发 | 缓解 | 回滚 |
|---|---|---|---|---|---|
| R1 | 劫持脚本出错→开机卡死 | 设备"变砖"观感（可恢复） | zhijack 逻辑错、picoarch 起不来 | 工作卡+原厂备份卡双份；zhijack 30s 看门狗回退；拔卡电脑查日志 | 换原厂备份卡 |
| R2 | 误写 root.dat/校验分区 | "sdcard is damaged" | 任何 Agent 改写 | 铁律禁止；CI 检查 root.dat 哈希不变 | 备份卡恢复 |
| R3 | 原厂二进制被改名/删除 | 启动链断裂卡画面 | 误操作 | 铁律禁止；改动前 git 快照+备份 | 备份恢复 |
| R4 | glibc 错配 >2.17 | core 起不来 | 链接超天花板 | **每次构建强制 verify_target_abi.sh** | 回退上一 tag |
| R5 | 键码/轴映射错 | 输入乱 | 绑定表与设备不符 | diag 实测后写表；设备菜单可重绑存 config | 删 config |
| R6 | ALSA 仍无声 | 体验缺失 | fork 竞争/路由 | 已修；diag audio 确认；必要时直写 hw 设备 | 回退音频改动 |
| R7 | 个别 core 性能不足 | 特定游戏不可玩 | core 未优化/32bpp 慢 | 用 _plus；C 阶段逐核验收；frameskip | 换 core 版本 |
| R8 | 多 Agent 并行冲突 | 提交覆盖 | 无分区协作 | §十 分工表分区；独立分支/文件域；交接文件锁定 | git revert |
| R9 | CI 预算超支（2000 分/月） | 无法构建 | 频繁全量 push | §十一 预算分配；增量构建；失败本地定位再推 | 停 push 分析 |

### 3.3 反编译待补事实（可行性收口）

| 待确认 | 手段 | 回答后 |
|---|---|---|
| 9 个 dat 字节内容 | 读原设备 000.dat–008.dat 解析 | 修正映射表（item 14） |
| HDMI 实际 mode 全集（能否 1080P） | `diag display` dump modes | item 13 分辨率决策落定 |
| 每按键真实键码/轴/HAT | `diag input` 交互捕获 | 写死绑定表（方向 A 输入） |
| ALSA 子进程实际 open | `diag audio` | 音频链路闭环 |
| 57 核逐个 dlopen 健康 | `diag cores` | 核心编译/ABI 问题清单 |
| rkgame/icube 启动流程细节 | strings/反汇编 | 强化 zhijack 看门狗 |

---

## 四、目标架构（方向 A：plat_rk3036g）

### 4.1 目标架构

```
RK3036G (标准 buildroot Linux)
┌─────────────────────────────────────────────────────────┐
│ 启动链: rkgame(原厂) → autorun → zhijack.sh(劫持)        │
│   ├─ SIGSTOP icube / kill rkgame（不删不改原厂文件）      │
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

### 4.2 模块设计

| 模块 | 设计 | 依据 |
|---|---|---|
| `plat_rk3036g.c` | 仿 `plat_linux.c`（薄包装+#include plat_sdl.c），用 `PLATFORM_RK3036G` 宏隔离干净 DRM/ALSA/evdev 路径，不经过 PLATFORM_SF3000 逻辑 | 上游多平台惯例 |
| Makefile | 新增 `platform=rk3036g`：`OBJS += plat_rk3036g.o hwdisp.o`；armv7-a/neon/hard-float + `-DPLATFORM_RK3036G` | 现状 sf3000 分支 MIPS 硬编码 |
| 游戏输入 | libpicofe 原生 in_evdev：默认绑定（diag 实测键码）+ 菜单重绑存 config（in_config_parse_dev 实证） | 消除 shm 注入与错位 |
| FrogUI 菜单 | 保持 cubevol_bridge（evdev→shm）+ FrogUI cv_read | 上游设计 |
| 显示 | 吸收 hwdisp_drm（RGB565+aspect-fit+旋转表+CLOEXEC） | v0.3.1 已验证 |
| 音频 | 吸收 plat_sound_init_alsa + `-Wl,--export-dynamic` | v0.3.1 已验证 |
| 版本 | 每个子系统独立版本；全局 platform=rk3036g 一次切换可整版回滚 | §六 |

### 4.3 与现有补丁的关系

| 现有内容 | 处置 |
|---|---|
| `patch/picoarch_rk3036g_full.patch` / `frogui_rk3036g_full.patch` | 保留为过渡基线；重构完成后重新生成（基于 f8ff5ba/2f41ace 新全量） |
| hwdisp.c DRM/旋转/CLOEXEC | **吸收**进 plat_rk3036g 平台层 |
| plat_sdl.c ALSA/音频 | **吸收**进平台层；PLATFORM_SF3000 专属块随重构移除 |
| cubevol_bridge.c | 保留（FrogUI 菜单输入依赖） |
| sf3000 shm 游戏输入注入（core.c OR 语句） | **移除**（游戏改走原生 evdev） |
| frogui L/R/映射/缓存修复 | 保留（独立于平台层） |

---

## 五、实施路线（A+B+C 细化）

### 5.1 方向 B：diag 事实底座（✅ 已实现，待实机）

- 已完成：`deploy/diag.c`（sysinfo/input/display/audio/cores 五模块，崩溃防护），build.sh 编译进 cubegm/，zhijack.sh 支持 SD 根 `diag.flag` 一次性触发（commit bc691ab）。
- **待办（其他 Agent 可并行）**：
  - B1：核对 diag.c 编译产物 ABI（verify_target_abi.sh 已含 diag 目标）
  - B2：CI run 结果确认 diag 编译通过、payload 含 diag
  - B3：用户实机跑 `diag all` 并回传 `diag_report.txt`（这轮实机同时验证 v0.3.1 的 10 项修复）
- **完成定义**：拿到 diag_report.txt，键码表/显示 mode 集/ALSA 结果/core 清单全部落盘到 `docs/diag_findings_<date>.md`。

### 5.2 方向 A：plat_rk3036g 平台层（等 B 事实）

| 步骤 | 内容 | 依赖 | 输出 |
|---|---|---|---|
| A1 | 新建 `plat_rk3036g.c` 骨架（平台 pdata + Makefile 分支） | B 完成 | 可编译空平台 |
| A2 | 输入：in_evdev 默认绑定表写入 diag 实测键码；移除 core.c shm OR | A1 | 游戏输入正确 |
| A3 | 显示：hwdisp_drm 独立化挂接 plat_rk3036g | A2 | HDMI 正确出图 |
| A4 | 音频：ALSA 路径独立化 + fork 前释放验证 | A3 | 有声 |
| A5 | FrogUI 对接验证（菜单+启动+返回） | A4 | 全链路可用 |
| A6 | 性能调优（frameskip/编译 flags/逐核验收） | A5 | 达标帧率 |

**每个 A 步骤 = 独立版本 + 实机验证（C 方向），不通过不进入下一步。**

### 5.3 方向 C：单子系统迭代验收

```
子系统顺序: 输入 → 显示 → 音频 → 性能
每子系统: 独立分支 → 实现 → CI 绿 → payload → 用户实机反馈 → 通过才进下一项
失败: 同一点第二次失败 → 停手，联网查官网/官方论坛（item 20 铁律）
```

---

## 六、版本控制规范

### 6.1 版本号语义

| 版本 | 含义 |
|---|---|
| `v0.x` | 测试版（交付实机验证的中间版） |
| `v1.0` | **21 条基线全部完成 + 用户真机全部验证通过**（唯一正式版） |
| 子系统版本 | 如 `v0.4-input`、`v0.5-display`、`v0.6-audio`、`v0.7-perf`（C 方向每子系统递增） |

### 6.2 分支策略

- `main`：稳定主线，只合入已验证子系统（每子系统合并 = 一个版本 tag）。
- `feat/diag` / `feat/plat-rk3036g` / `feat/input` / `feat/display` / `feat/audio`：子系统工作分支。
- 多 Agent：**文件域分区**（§10.3），同一文件域只允许一个 Agent 拥有写权。

### 6.3 tag 规范

- 里程碑：`core/<stage>-<feature>`（可回滚锚点）
- 发布：`v-<feature>` / `v1.0`
- **每个里程碑必须能整版回滚**（git revert 或 bundle 恢复）

### 6.4 commit 规范

- 原子提交：一个逻辑变更一个 commit；消息含"为什么"。
- 禁止 `--no-verify` 跳过门禁。
- 每次推送前：`bash -n` 全部脚本 + ABI 门禁 + root.dat 哈希不变检查。

### 6.5 回滚策略

| 场景 | 手段 |
|---|---|
| 代码问题 | git revert / checkout 上一 tag |
| 固件启动失败 | 换原厂备份 SD 卡（用户持有双卡） |
| payload 问题 | 上一 payload zip（release 留档） |

---

## 七、里程碑规划

| 里程碑 | 目标 | 交付物 | 验收标准（DoD） | 依赖 |
|---|---|---|---|---|
| **M0** | 封存+立项（已完） | archive zip + charter/delivery/handoff v1.2 + 本计划 | 其他 Agent 可独立接手 | — |
| **M1** | diag 实机事实 | `docs/diag_findings_<date>.md` | 键码表/mode 集/ALSA/core 清单完整；10 项修复复验 | B 完成（bc691ab 已推） |
| **M2** | 9 个 dat 字节核验 | 映射表修正（docs） | 9 文件逐字节解析完成，映射确认 | 用户提供 dat 或原卡读取 |
| **M3** | 分辨率决策 | charter §二 更新 | 1080P vs 720P 落定（diag mode 集 + datasheet） | M1 |
| **M4** | 输入子系统 | `v0.4-input` payload | diag input 键码全对；游戏/菜单输入正确；重绑可存 | M1 |
| **M5** | 显示子系统 | `v0.5-display` payload | 全游戏正确出图/黑边/旋转；无半白屏 | M4 |
| **M6** | 音频子系统 | `v0.6-audio` payload | 全 core 有声；fork 后稳定 | M5 |
| **M7** | 性能达标 | `v0.7-perf` payload | 9 分类代表游戏全速或可玩帧率 | M6 |
| **M8** | 57 核全表 + 策略存档 | `v0.8-57cores` payload | 57 核全部构建+加载+代表 ROM 可玩+存档可用 | M7 |
| **M9** | 全手柄 + 自定义映射 | `v0.9-input-full` | 原厂 4 profile + 新手柄映射路径验证 | M8 |
| **M10** | **v1.0 完整验收** | `v1.0` tag + stable 分支 | **21 条基线全过 + 用户真机全验通过** | M2-M9 |

---

## 八、验收清单（全局）

- [ ] 21 条铁律基线逐条核对（charter §零），无遗漏
- [ ] 9 分类（000-008）各至少 1 游戏启动→30s→退出→存档→读档
- [ ] 57 核全表构建 + ABI 门禁 + 符号门禁 + payload 完整性门禁 PASS
- [ ] 策略游戏存档功能覆盖（item 17）
- [ ] 全手柄 + 自定义映射（item 18）
- [ ] root.dat/校验分区哈希不变（item 15）
- [ ] 每次构建 verify_target_abi.sh 强制（item 16）
- [ ] 11.png 目录结构交付（item 14）
- [ ] HDMI 分辨率决策落定（item 13）
- [ ] 用户真机全验通过 → 标记 v1.0

---

## 九、软件工程流程规范

每个子系统走完整流程（产出落盘可追溯）：

```
需求分析(PRD) → 实施方案(设计) → 实现(代码) → 验证(CI+ABI+实机) → 验收(DoD) → 发布(tag+release) → 交接(HANDOFF更新)
```

- **需求分析**：写 `docs/prd/<subsystem>.md`（依据 diag 事实，禁止猜测）
- **设计**：写 `docs/arch/<subsystem>.md`（模块/接口/数据流）
- **实现**：原子提交；代码注释含依据（文件:行:为什么）
- **验证**：本地 bash -n + 括号平衡 + CI 编译 + ABI 门禁 + 实机
- **验收**：DoD 清单逐项勾选；用户实机反馈是硬闸门
- **文档**：每次版本更新 HANDOFF.md + VERSION

---

## 十、多 Agent 协作规范

### 10.1 总则

- 任何 Agent 开工前：读 `PROJECT_CHARTER.md` §零 + `HANDOFF.md` + 本计划。
- 开工：认领任务 → 写 `docs/tasks/<task>.md` 进度追踪 → 开始。
- 结束/交接：更新 HANDOFF.md + 本计划状态 → 明确"下一步从哪里开始"。

### 10.2 可并行工作包（建议分配）

| 工作包 | 文件域 | 输出 |
|---|---|---|
| W1 diag 事实整理 | docs/ | diag_findings_*.md |
| W2 9 个 dat 解析 | docs/ | dat 映射修正 |
| W3 分辨率决策 | docs/ + charter | 分辨率落定 |
| W4 输入子系统 | plat_rk3036g.c + Makefile + in_evdev | v0.4-input |
| W5 显示子系统 | hwdisp + plat_rk3036g | v0.5-display |
| W6 音频子系统 | plat_sdl 音频段 | v0.6-audio |
| W7 57 核编译 | build.sh CORE_TABLE + patch/ | 57 核产物 |
| W8 手柄 profile | joystick/ 映射文件 | 全手柄支持 |

### 10.3 冲突管理

- **文件域锁定**：上表每个工作包的文件域互斥；W4-W6 共享 plat_rk3036g.c 时，按 A1-A6 顺序串行，禁止并发写同一文件。
- 同时只允许 1 个 Agent 拥有 `plat_rk3036g.c` 写权（先在 HANDOFF 声明）。
- 提交冲突：git revert 规则优先，禁止强推覆盖他人。

---

## 十一、CI 预算与资源（2000 分钟/月，极致节约）

| 用途 | 预算 | 说明 |
|---|---|---|
| 全量构建 | ≤6 次/月 | 每次 ~120-150 分钟；仅里程碑验证用 |
| 增量构建（core 追加） | ≤8 次/月 | 只编新增 core（build.sh 支持 CORES 过滤） |
| 失败重试 | 0 | **失败先本地定位，禁止"取消重试碰运气"** |
| 预算红线 | 500 分钟/周 | 超红区停手分析，不盲推 |

节约守则：本地能验证的（bash -n/语法/括号/补丁 apply 检查）绝不上 CI；一个 commit 一个 run。

---

## 十二、应急与回滚

| 场景 | 处置 |
|---|---|
| 设备开机卡死 | ①拔卡电脑查 zhijack.log/diag_report.txt ②确认 zhijack 未死循环 ③换原厂备份卡恢复 |
| "sdcard is damaged" | 校验失败 = root.dat 被改；用原厂备份卡恢复，禁止继续刷机 |
| payload 半白/黑屏 | 回退上一 tag 重新打包；附 diag display 结果 |
| CI 红 | 读 BUILD_REPORT.txt（build-output 分支），本地复现修复后单次推送 |
| 多 Agent 冲突 | 文件域锁定回滚；以 HANDOFF 声明为准 |

---

## 附录 A：接手 Agent 必读索引

1. `PROJECT_CHARTER.md` §零（21 条铁律）+ §0.4（重构方向）
2. `DELIVERY_REQUIREMENTS.md`（1.0 定义 + 验收）
3. `HANDOFF.md` §0（v0.3.1 修复表）+ §0.5（重构交接）
4. 本计划 `PLAN_A_B_C_REFACTOR.md`（路线图）
5. `VERSION`（版本语义）+ `docs/00x_dat_platform_mapping.md`（item 14）
6. 封存档 `CubeGM_v0.3.1_archive.zip`（含 bundle 可回滚任意历史）

## 附录 B：变更记录

| 版本 | 日期 | 修订 |
|---|---|---|
| v1.0 | 2026-08-22 | 初稿（用户批准 A+B+C；含可行性分析/风险/版本/里程碑/协作） |
