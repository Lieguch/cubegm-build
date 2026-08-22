# CubeGM · 独立 Agent 开工入口

> **你是接手此项目的唯一执行 Agent。** 本包是自包含的完整交接——不需要联系前一个 Agent，读完本文件即可开始工作。

## 这是什么

在 **RK3036G 掌机**（无屏幕、仅 HDMI 输出的标准 buildroot Linux 设备）上，把开源代码（TreeFrogUI / picoarch / FrogUI + libretro 核心）构建成**功能对等甚至超越原厂 CubeGM 固件**、能真正打游戏的系统。原系统封闭（rkgame/icube 闭源+加密），**稍有出错即无法启动**——所以本包内含完整的逆向实证与禁忌清单。

## 你必须在动手前读完（按序，约 40 分钟）

| 顺序 | 文件 | 为什么 |
|---|---|---|
| 1 | `PLAN_A_B_C_REFACTOR.md` | **你的完整任务书**：可行性、路线图、里程碑、验收、应急 |
| 2 | `PROJECT_CHARTER.md` §零 | 21 条锁定铁律（最高依据）+ §0.4 重构方向 |
| 3 | `DELIVERY_REQUIREMENTS.md` | 1.0 交付定义 + 验收清单 |
| 4 | `memory/MEMORY.md` | **禁忌清单（违反=变砖）** + 项目铁律 + 踩坑历史 |
| 5 | `docs/00x_dat_platform_mapping.md` | 9 个分类 dat 的模拟器核心映射（item 14） |
| 6 | `HANDOFF.md` | 历史交接 + 你的交接义务 |

## 你的第一步动作（读完文件后）

1. 核对环境：`source/TreeFrogUI_picoarch`（补丁应用后状态）+ `git/cubegm-build.bundle`（完整历史，可回滚）。
2. **先做方向 B**：确认 `deploy/diag.c` 已编译进 payload（`cubegm/diag`），交付给用户跑一次 `diag all`（SD 根放空文件 `diag.flag` 触发），回传 `diag_report.txt` → 落盘 `docs/diag_findings_<date>.md`。
3. **在拿到 diag 实机事实之前，禁止做任何平台层修改**（铁律：不猜测）。

## 三条保命红线（随时记住）

1. **绝不**改名/删除原厂 `icube`/`rkgame`/`driver.so`/`cores/libemu_*.so`/`root.dat` → 会变砖（260 设备血泪教训）。对抗原厂进程只用 kill/SIGSTOP。
2. **绝不**改写 root.dat/校验分区 → "sdcard is damaged"。CI 必须检查哈希不变。
3. **每次构建强制** `verify_target_abi.sh`（glibc ≤ 2.17）→ 否则 core 起不来。

## 快速命令参考

```bash
# 恢复 git 仓库（完整历史）
git clone git/cubegm-build.bundle cubegm-build
# 当前源码树已是补丁应用后状态
ls source/TreeFrogUI_picoarch   # plat_sf3000.c/hwdisp.c/plat_sdl.c 等
# 构建（Linux 主机，CI 用 bootstrap）
cd deploy && ./bootstrap_linux.sh
# 打补丁校验（如需重新生成）
git -C source/TreeFrogUI_picoarch diff f8ff5ba > patch/picoarch_rk3036g_full.patch
```

## 完成后

按 `PLAN_A_B_C_REFACTOR.md` §11 完成定义逐条核对；全部通过 + 用户真机全验后打 `v1.0` tag，并更新 HANDOFF.md 交接给下一位。

—— 祝顺利。你的所有上下文都在这个包里。
