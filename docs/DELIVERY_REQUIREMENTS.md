# 交付要求与实机验收清单（DELIVERY REQUIREMENTS）— CubeGM 开源替代系统

> 角色：交付负责人 ｜ 目标：把 CI 产出的 payload 变成用户可烧录、可验收的成品
> 配套：立项 `docs/PROJECT_CHARTER.md`、需求 `docs/prd.md`、验证 `docs/qa_verification.md`、烧录 `DEPLOY.md`

---

## 一、交付物形态

| 物品 | 来源 | 说明 |
|---|---|---|
| `payload-<run#>.zip` | GitHub Release（每个绿构建自动发布） | 内含完整 `cubegm/` 烧录包，即插即用 |
| `cubegm/` 目录内容 | `deploy/cubegm/` 暂存后由 `build.yml` 打包 | picoarch + frogui + 5 基线核 + zhijack.sh + autorun + config.xml + 运行时库（SDL/libpng12/zlib） |
| 真机验收清单 | 本文件 §四 | 用户实体机逐项核对 |

- **发布包即烧录包**：CI 已把 `picoarch + frogui + 5 核 + zhijack.sh + autorun + config.xml + 运行时库` 全部暂存进 `cubegm/`，`build.yml` 打成 `payload-<run#>` Release。无需手工拼装。
- 下载位置：仓库 `Releases` 页 → `payload-<run#>` → asset `cubegm-payload.zip`。

## 二、烧录步骤（最短路径）

1. 取一张 FAT32 格式 SD 卡，确认设备原厂 `cubegm/` 工作目录结构已就位（沿用原厂 `000.dat–008.dat`，不额外构建）。
2. 把 `cubegm-payload.zip` 解压出的 `cubegm/` **整体覆盖**到 SD 卡根目录的 `cubegm/`。
3. 确认 `cubegm/setting.xml` 含 `<autorun file="/mnt/sdcard/MD/dummy.md" driver=""/>`（已随 payload 内置 `boot-override`：setting.xml + dummy.md + 改名自 `libemu_tfhijack.so` 的 `libemu_md.so`）。
4. 弹出 SD 卡，插入掌机开机 → 经 `zhijack.sh` 劫持进入 FrogUI（**不触碰 root.dat/校验分区**，绝不会 "sdcard is damaged"）。
5. 进 FrogUI → 选 ROM → picoarch 启动对应核心。

> 完整步骤与排错见 `DEPLOY.md` 第 5 节。

## 三、版本与发布节奏（用户规定）

| 版本 | 定义 | tag |
|---|---|---|
| v0.1 | 首个完整绿构建（前端 + 5 核 ABI 全 PASS） | `v0.1` / `v-build-good`（05ee252dfe） |
| v0.2 | 首个含全量自动化测试门禁的绿构建（34 核 + 8.5/9.5 门禁） | `v0.2`（377ac862208a） |
| **v0.3** | 32/34 核基线 + **MD 策略存档写权限加固**（#162/#166） | 待打（run #166 绿后） |
| **v1.0** | 整构建首绿 **且真机功能验收通过**（出画/出声/输入/存档/全手柄） | 待打 + 维护 `stable` 分支 |

- **测试版**：v0.x（首个绿构建 = v0.1）；**正式版**：第一个用户能正式用且全功能正常 = v1.0。
- 每次实质进展递增版本号；跨里程碑打 `core/*`、`v0.x` tag 作回滚点。
- CI 预算：GitHub Actions 每月仅 2000 分钟，每次推送谨慎（任一 main 推送触发一次完整构建）。

## 四、实机验收清单（用户逐项核对，**唯一 CI 无法替代的环节**）

> CI 已把「能否烧录」全部转成绿/红灯（ABI 门禁 + 符号门禁 + payload 完整性门禁）。
> 以下 6 项只能由真机确认；失败 run 不计费。

### 4.1 启动安全
- [ ] 开机进入 FrogUI，**无「sdcard is damaged」**。
- [ ] `zhijack.sh` 未被原厂 `rkgame/icube` 抢回（持续循环 picoarch）。

### 4.2 显示 / 音频 / 输入
- [ ] HDMI 出画（DRM/KMS 1280×720）。
- [ ] ALSA 出声。
- [ ] evdev 手柄/按键有响应（标准 HID 全通用，无需逐设备档案）。

### 4.3 核心运行
- [ ] FrogUI 前端能枚举已构建核心（picoarch 读 `cubegm/cores/*.so`）。
- [ ] 选 ROM → 对应核心启动、可玩。

### 4.4 策略游戏存档（**本轮重点回归项**）
- [ ] 世嘉 MD 策略游戏：游戏中存档一次 → 退出后重进，存档仍在。
- [ ] 验证落盘位置（二选一，取决于设备可写性，由 `picoarch: save_dir=` 日志决定）：
  - 默认可写：`/mnt/sdcard/picoarch/<TAG>/*.sav` 生成；
  - 回退路径：`/mnt/sdcard/cubegm/saves/<TAG>/*.sav` 生成（#162 加固自动回退）。
- [ ] 其它带电池存档核心（GBA/gpsp、NES/fceumm 等）同理验证。

### 4.5 诊断方法（若 4.4 仍失败）
1. 抓 `picoarch` stderr：在 `zhijack.sh` 给 `picoarch` 加 `2>/tmp/picoarch.log`，重进游戏存档后 `cat /tmp/picoarch.log | grep save_dir`。
2. 看到 `picoarch: save_dir=/mnt/sdcard/cubegm/saves/...` → 回退已生效，去该目录查 `.sav`。
3. 若 `save_dir` 正确但无 `.sav` → 次因：该核心未导出 `RETRO_MEMORY_SAVE_RAM`（sram_size==0 静默返回）。**需联网核对该核心官方 libretro 文档**确认电池 RAM 支持，再决定增量修复（铁律禁猜测式调试）。
4. 若 `save_dir` 指向不可写路径且无回退日志 → 上报，由下一轮加固处理。

### 4.6 回滚
- [ ] 任一版本不稳定：SD 卡换回上一版 `cubegm/` 即回退（payload 各自独立，互不污染）。

## 五、交付门禁（CI 已强制）

| 门禁 | 脚本/阶段 | 不通过 = 红，不发布 |
|---|---|---|
| ABI 门禁 | `build/toolchain/verify_target_abi.sh`（EM_ARM/0x5000400/≤GLIBC_2.17） | 运行期崩溃 |
| libretro 符号门禁 | `STAGE 8.5` 校验每个 .so 导出必需符号 | 前端加载失败 |
| payload 完整性门禁 | `STAGE 9.5` 断言 picoarch/frogui/5核/zhijack/autorun/config.xml 全在 | 烧录包残缺 |

---

*下接：HANDOFF.md（构建流水线与根因记录）、DEPLOY.md（烧录与排错细节）。*
