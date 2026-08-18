# CubeGM 开源替代系统 — 立项文件（PROJECT CHARTER）

> 本文件是项目**总纲**：记录立项初始需求、最终交付要求、版本/里程碑/验收清单、Agent 交接流程。
> 任何 Agent 接手工作前必须先读本文件 + `HANDOFF.md`。
> 最近更新：2026-08-18

---

## 一、立项初始需求（用户原始要求，铁律级）

1. **目标**：模仿 TreeFrogUI / picoarch / FrogUI 开源代码，构建功能对等甚至**超越原厂 CubeGM 固件**的系统，在**原硬件（RK3036G）上运行模拟游戏**。
2. **系统必须**：
   - 构建出**整套系统**，可在实体机上玩模拟游戏；
   - **支持策略游戏存档功能**（SRAM/电池存档落 `.srm`）；
   - **支持全部通用手柄**（标准 HID evdev，无需逐设备档案）。
3. **工作方式铁律**：
   - 杜绝猜测、碰运气、推理、穷举试错等无把握发散；
   - 所有修复必须**联网搜集对应项目官网文档、官方论坛、知名论坛方法**后分析出有效解决手段，解决式输出；
   - 极致压缩无关聊天内容；压缩上下文时保留关键要求；
   - 在不牺牲效率前提下节省 token（按 Token 收费）；减少 AI 调用次数（防止每分钟请求上限）。
4. **性能要求**：支持官方全部核心，把硬件性能榨干。
5. **GitHub 构建时间预算**：每月仅 2000 分钟，**每次使用谨慎且最大利用率**。
6. **立项与交接**：
   - 开始前创建立项文件和交付要求，方便其他 Agent 随时接手；
   - 创建 Agent 交接流程：每个 Agent 接手知道从哪开始，结束创建交接文件；
   - 所有 Agent 知道立项初始需求和最终交接要求。
7. **软件工程流程**：创建版本号、里程碑（可组合/回滚）、验收清单；进行需求分析 → 实施方案确定 → 实施方案验收 → 提交代码。
8. **自动化**：自动静默完成除铁律外所有检查、复盘、定位、修正、测试、上传、构建行为；自动删除已完成/已取消/已失效的监控对话。

## 二、硬件约束（不可违反）

| 项 | 值 | 来源 |
|---|---|---|
| SoC | Rockchip **RK3036G**（双核 Cortex-A7 / Mali-400 / DDR3） | 用户 + datasheet V1.1 |
| CPU ABI | 32-bit ARM / **armhf**（e_flags=0x5000400，硬浮点 NEON+VFPv4） | 20 个 libemu_*.so 实测 |
| glibc 天花板 | **≤ 2.17**（实测由 libemu_fbalpha.so 拉高到 2.17） | probe_device_abi.py |
| 显示 | 设备无屏幕，仅 **HDMI 输出**；datasheet 声明支持 1080P。渲染目标待 datasheet 确认后定（1280×720 或 1920×1080） | 用户 + datasheet V1.1 |
| 音频/输入 | 标准 Linux ALSA / evdev（driver.so 字符串证据） | 逆向 |
| 启动安全 | **绝不改写 root.dat / 校验分区**，否则报「sdcard is damaged」；只走 autorun 劫持（zhijack.sh） | 用户红线 |
| 构建门禁 | **每次构建强制 verify_target_abi.sh**，杜绝 glibc 错配上线 | 用户要求 |

## 三、交付要求（1.0 版本定义）

1. **v1.0** = 第一个用户能正式使用、全部功能正常的版本（用户规定）。
2. 交付物按原设备工作目录截图（`11.png`）结构交付：
   - 目录 000–008 内 9 个 `*.dat` 文件**沿用原设备**，不额外构建记录；
   - 000.dat–008.dat 解析出的内容须回忆并对应到游戏模拟器核心的指向；
   - 9 个文件夹外的模拟器核心工作目录指向 **`Roms`**。
3. 系统交付后：插 SD 卡进设备即进入自研系统，可玩模拟游戏，有存档、全手柄。
4. 同步目标仓库：`Lieguch/cubegm-build-monkey`（含已定稿代码 + 构建好的 release）。

## 四、版本与里程碑（回滚点）

| 版本 | 定义 | 对应 tag |
|---|---|---|
| v0.1 | 首个完整绿构建（STAGE1–8 全绿） | `core/*`、`v-*`（已打） |
| v0.2 | 四阶段全部推进后首个可测版本 | 待打 |
| **v1.0** | 整构建首绿且**真机功能验收通过** | 待打 + 维护 stable 分支 |

里程碑回滚点（已打 tag，受保护）：`core/stage1-sysroot`、`core/stage2-perms-fixed`、`core/stage2-libs`、`v-sysroot-writable`、`v-platsf3000-syntax`、`ct-ng-tarballs`。

## 五、验收清单（QA 门禁）

见 `docs/qa_verification.md`，核心四项硬门禁：
1. **ABI 门禁**：每个产物 ELF 必须 EM_ARM / e_flags=0x5000400 / 最大 GLIBC_2.17（`build/toolchain/verify_target_abi.sh`）。
2. **启动安全**：zhijack.sh 不触碰 root.dat / 校验分区；真机无「sdcard is damaged」。
3. **功能验收**：进 FrogUI → 选游戏出画+出声+输入响应；策略游戏 SRAM 落盘；全通用手柄可用。
4. **分辨率**：UI 渲染目标与 datasheet 能力一致（1280×720 或 1080P，待确认）。

## 六、Agent 交接流程（所有 Agent 必须遵守）

### 接手（进入）流程
1. 读本文件 `docs/PROJECT_CHARTER.md` → 明确立项初始需求与最终交付要求。
2. 读 `HANDOFF.md` 头部「最近更新」→ 确定当前活跃阶段与下一步（§9 起）。
3. 读 `VERSION` → 确认当前版本状态。
4. 用 `git log --oneline -5` + `git status` 确认工作树与远程一致性。
5. 检查运行中 background terminal（`background_terminal_list`），如有 CI 监控任务先接管。
6. 明确当前卡点：CI 最新 run 状态 → 若失败按 §6「沙箱操作铁律」取 build-output 真实日志定位。

### 结束（交接）流程
1. 更新 `HANDOFF.md`：追加本节工作成果、根因证据、下一步。
2. 更新 `VERSION`（实质进展递增）。
3. 提交并推送（如涉及修复，必须同步推送 HANDOFF）。
4. 清理已完成/失效的监控对话与后台终端。
5. 若跨版本里程碑，打 tag（`core/*`、`v0.x`）作回滚点。

### 协作纪律
- 每次修复必须**联网查官方文档**，禁止猜测/试错（§9 铁律 1、5）。
- 每次提交同时更新 HANDOFF 到 GitHub（用户 2026-08-16 铁律）。
- 只改一个明确问题、最小文件集；已验证修复只叠加不删除（§9 铁律 2、3）。
- CI 日志取真实内容（build-output 分支），先日志后归因（§6）。

## 七、当前活跃任务（2026-08-18 交接点）

- main=cb065b23：§32 MD 策略存档加固（#162 注入 save_dir 可写性回退+诊断；#165 编译回归；#166 转义修复），**run #166 构建中，待 PASS**。
- run #164 已验证 34 核基线全绿（#161 revert 回到 33ad9a04 per-core recipe）；run #153 起含全量测试门禁（STAGE8 ABI + STAGE8.5 符号 + STAGE9.5 完整性）。
- 待办：run #166 绿后打 v0.3 + 推送治理文件（HANDOFF §32 / DELIVERY_REQUIREMENTS.md / VERSION）+ 交付 payload（含 MD 策略存档真机回归清单）；真机验收（HDMI/ALSA/evdev/前端枚举/核心运行/存档/全手柄）→ v1.0。
- datasheet HDMI 分辨率已确认：1280×720 维持（SoC 支持 1080P）。
- wip/frogui-gs-interface、wip/frogui-native-launch：旧 sf3000 路径，stale 绿，未改动。

