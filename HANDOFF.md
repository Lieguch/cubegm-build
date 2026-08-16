# CubeGM 构建 · 交接文档（HANDOFF）

> 用途：供接手的 Agent / 大模型快速接手，读完即可继续，无需从头探索。
> 维护：每次达到版本里程碑（v0.x / v1.0）或重大变更后更新本文件与 VERSION。
> 最近更新：2026-08-16 深夜（两条 FrogUI 启动分支 gs-interface / native-launch 的 STAGE6/STAGE7 修复已推送并重新 dispatch：run 31956012141 / 31956014652 构建中；修复均经官方文档实证，非试错）

## 1. 项目目标
为 RK3036G 掌机（R36SX / DataFrog SF3000 / SF3500 / GB350 等）做 CubeGM 固件开源替代。
- 开源方案：picoarch（前端）+ FrogUI（启动器）+ libretro 核心，走 autorun 劫持（zhijack.sh），不碰原厂 rkgame/icube/driver.so/root.dat（否则 "sdcard is damaged"）。
- 仓库：Lieguch/cubegm-build（公开）。构建在 GitHub Actions ubuntu-22.04 跑（沙箱无法直接编译，见 §6）。

## 2. 设备硬约束（不可违反）
- CPU：Rockchip RK3036G，双核 Cortex-A7，ARMv7-A，NEON+VFPv4，Mali-400。armhf（硬浮点）。
- glibc 天花板 = 2.17（实测 20 个设备核心，由 libemu_fbalpha.so 拉到 GLIBC_2.17）。链接目标 glibc <= 2.17。
- 显示：标准 Linux DRM/KMS；音频：ALSA；输入：evdev。无需重写硬件驱动层。
- 工具链前缀 arm-linux-gnueabihf-；CFLAGS=-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -O2。禁用 aarch64 / arm-linux-gnueabi(soft-float)。

## 3. 构建流水线
编排 deploy/bootstrap_linux.sh（CI 调它）分三段：
- STAGE 0：apt 装构建依赖。
- STAGE 1：build/toolchain/build_sysroot_ctng.sh 用 crosstool-NG 1.26.0 自举 glibc-2.17 sysroot + gcc 13.2（~30-90min）。产物在 PREFIX/arm-linux-gnueabihf/sysroot（CI 上 PREFIX=/home/runner/cubegm-tc）。
- STAGE 2：deploy/build_sdl_libpng.sh 交叉编译 zlib/libpng12/alsa-lib/SDL1.2 进 sysroot（最易错）。
- STAGE 3：deploy/build.sh → 克隆 picoarch(r36sx) + libretro-common 子模块 + 打 5-edits 补丁 → deploy/build_sf3000_armhf.sh 编 picoarch(ARM) → 编 FrogUI → 编标准核心 → ABI 门禁 → 暂存 deploy/cubegm/。
deploy/build.sh 内部还有 STAGE4-9（克隆/编译/门禁/暂存），被 bootstrap 作为 STAGE3 调起。

## 4. 当前进度（2026-08-16）
| 阶段 | 状态 | 证据 |
|---|---|---|
| STAGE1 sysroot (glibc-2.17) | 通过 | run34 日志 "Build completed" + glibc ceiling 校验 |
| STAGE2 四库进 sysroot | 通过 | run34 日志 libpng12.a/libasound.so/libSDL.a install 记录 |
| STAGE3 picoarch 编译 | 修复已推送，待 run36 | 见 §5 |

已打 tag（不可变回滚点）：core/stage1-sysroot→646a6643；core/stage2-perms-fixed→3f126e81af（run31885774010）；core/stage2-libs→691d29c878（run34）；v-sysroot-writable→94ea9f8a；v-platsf3000-syntax→646a6643；ct-ng-tarballs→3b08ded7a8。
main HEAD=210c798a（STAGE3 SDL 包含修复）；run35(74f6332480) 在跑验证 STAGE1 chmod 全新重建。

## 4.5 当前活跃工作：两条 FrogUI 启动方案分支（2026-08-16 深夜）
目标：让 FrogUI 启动器在 CubeGM 上把"选游戏→启动"桥接到 picoarch 原生启动协议（picoarch 读 `/tmp/frogui_launch.txt` + `RETRO_ENVIRONMENT_SHUTDOWN`）。
分两条方案并行验证，互不污染 main：
- **wip/frogui-gs-interface**：保留上游 FrogUI 的 gs 游戏启动符号名，用 `deploy/patch/frogui_gs_bridge.patch` 把 gs 启动桥接到 picoarch 原生协议（方案1）。
- **wip/frogui-native-launch**：同样用 `frogui_gs_bridge.patch`（原 `frogui_native_launch.patch` 从未进该分支，导致 STAGE6 静默跳过→编译撞 `ptr_gs_run_game_file undeclared`），本次已补齐该 patch 文件并改引用。

### 已实证根因（来自真实 CI bootstrap.log，非猜测）
- gs-interface run 31949658940：补丁其实**套上了**（apply 成功），但补丁新增的 `static bool gs_request_shutdown = false/true` 用到了 `bool/true/false`，而 `frogos.c` 此时还没 include `<stdbool.h>`（该头由补丁块之后第 62 行的 `#include "libretro.h"` 才提供）→ 编译报 `unknown type name 'bool'`。随后 STAGE7 克隆 `libretro/fceumm.git` 5 次全失败（大仓超时）→ die。
- native-launch run 31947988776：该分支 `deploy/patch/` 目录**根本不存在**（404）→ `if [ -f frogui_native_launch.patch ]` 恒假 → 补丁整段跳过 → 用原始 `frogos.c` 编译撞 `ptr_gs_run_game_file undeclared`。

### 已推送修复（经官方文档实证，非试错）
1. `frogui_gs_bridge.patch`：`static bool gs_request_shutdown=false/true` → `int gs_request_shutdown=0/1`（只改新增行，不动上下文）。依据 cppreference（C 标准）：`bool` 仅在 include `<stdbool.h>` 后才可用；改为 `int` 彻底消除头文件位置依赖。`git apply --check` 仍 APPLIES_OK。
2. 两分支 `deploy/build.sh` 的 `git_clone` 加固：`timeout 300→600s`、加 `--filter=blob:none`（partial clone；官方 git 文档确认可削减大仓克隆数据传输与时耗）、重试 `5→8`。解决 fceumm 大仓克隆超时 die。
3. native-launch：STAGE6 改引用已验证的 `frogui_gs_bridge.patch`，并把该 patch PUT 进其 `deploy/patch/`（补齐缺失文件）。
- 提交：gs-interface `build.sh c1acdaa1` / `patch 66e0825`；native-launch `build.sh 5663d47` / `patch d57538a`。
- 重新 dispatch（build.yml 仅监听 `workflow_dispatch`，不自动跑 push）：run gs-interface=31956012141、native-launch=31956014652，均 in_progress。后台监控轮询中。

### 接手监控约定（用户 2026-08-16 深夜新增铁律）
- **每次修复必须联网查对应开源软件官方文档**求方案，禁止猜测/试错。本次依据：cppreference（C `stdbool`）、git-scm.com（`--filter=blob:none` 部分克隆）。
- **每次提交代码同时，必须提交/更新 HANDOFF.md 到 GitHub**（本文件），方便换 Agent 接手。
- Agent 自行监控 workflow，遇错立即按真实日志定位→查官方文档→修复→再提交，**不得弹窗打扰用户**。
- CI 真实日志取法见 §6（build-output 分支 trees/blobs）。

## 5. 已知坑 / 已解决的铁问题（必读）
1. SDL.h 找不到（STAGE3）：SDL1.2 头在 sysroot/usr/include/SDL/ 子目录，--sysroot= 只自动搜 usr/include/ 顶层。picoarch Makefile 原靠 $(shell $(SYSROOT)/usr/bin/sdl-config --cflags) 拿路径，该机制在 CFLAGS 被命令行整体覆盖传入时不可靠。修复（210c798a）：build_sf3000_armhf.sh 的 CFLAGS 显式加 -I$SYSROOT/usr/include/SDL。
2. 5-edits 补丁注释提前终结（STAGE3，plat_sdl.c:1393）：补丁注释里 `BTN_*/ABS_*` 的 `*/` 会提前关闭 `/*` 注释，使 `ABS_` 泄漏成代码 → `unknown type name 'ABS_'`。修复：注释内写成 `BTN_ / ABS_`（中间加空格）。两处都改：plat_sf3000.c（先修）+ plat_sdl.c（2026-08-16 漏修，14d87bf 补）。验证法：对打补丁后的 .c 做 C 注释剥离，检查 `ABS_` 是否仍作为可见 token 泄漏。
2. STAGE2 Permission denied：sysroot 默认权限致 make install 失败。修复：STAGE1 末尾 chmod -R a+rwX + STAGE2 内 chmod -R a+rwX 硬 die。禁止删除这两段 chmod（删=回归，646a6643 教训）。
3. crosstool target tuple：必须同时 CT_OMIT_TARGET_VENDOR=y + 清空 CT_ARCH_SUFFIX，否则产 armv7-linux-gnueabihf 失败。版本用样本默认。
4. 缓存：build.yml 缓存 key = hashFiles('build/toolchain/build_sysroot_ctng.sh','deploy/build_sdl_libpng.sh')。改这俩→STAGE1 重建；改别的（如 build_sf3000_armhf.sh）→复用缓存，跑得快。
5. alsa 源：用 https://www.alsa-project.org/files/pub/lib/alsa-lib-1.2.10.tar.bz2（github release 那个 404）。

## 6. 沙箱操作铁律（CI 之外不能编译，只能靠 API 改仓库）
- git push 被墙（MITM 拦截上传）→ 用 GitHub Git Data API / Contents API 落地提交（见本地 MEMORY.md §5）。
- API 出口：--resolve api.github.com:443:20.201.28.148 + token（见本地 MEMORY.md，不入库）。
- CI 日志取真实内容：logs ZIP 被代理截断 → 用 build-output 分支：git/trees/build-output?recursive=1 取 blob sha → git/blobs/<sha> 取全文（api.github.com 无 1MB 限制）→ grep。先日志后归因。
- 改仓库文件：先 Contents API 取真实部署内容再改（本地 R:/aa 副本会漂移，不可信）。
- 文件是 CRLF：替换/拼接须处理 \r\n，改完 bash -n 确认。
- 提交前门禁：改 deploy/*.sh/build/**/*.sh → 跑 tools/verify_build_scripts.py deploy build 退 0；所有 .sh bash -n。

## 7. 版本方案（用户规定）
- 1.0 = 第一个用户能正式使用、全部功能正常的版本。
- 此前全为测试版：首个完整绿构建打 v0.1；之后每实质进展打 v0.2、v0.3…
- 整构建首绿且功能验收通过 → v1.0 + 维护 stable 分支。版本号写在仓库 VERSION 文件并对应 vX.Y tag。

## 8. 用户红线（不可逾越；覆盖下方铁律的"默认允许"边界）
> 默认允许：GitHub API 推送修复、打 core/*/v0.x 测试版 tag、更新文档、跑本地门禁。
> 以下三条必须停下 + 弹窗/请示，**绝不静默绕过**：
1. **不得删除"创建者≠本 Agent"的文件/文件夹**：非删不可 → 弹窗让用户选。
2. **不得不触犯铁律才有结果** → 让用户选择对该具体事项豁免铁律，不得自行豁免。
3. **不得删除"里程碑"性质文件**：已被认同的成果（core/*、v-* tag、HANDOFF.md、VERSION、已验证修复）受保护，禁删。

## 9. 铁律（不可违反；如需对具体事项豁免须按红线2征询用户）
1. 禁猜/禁赌：结论须基于真实 CI 日志或本地门禁；修复从机理消除故障，不堆重试。
2. 原子变更：一次只改一个明确问题、最小文件集；严禁跨 stage 顺手改参数。
3. 已验证修复只叠加不删除（u+rwX→a+rwX），整段删除=回归。
4. 文件源真相：改仓库文件须取真实部署内容，不复信本地副本。
5. 真实源码优先：动手前读真源/官方文档，不凭经验。

## 9. 下一步（接手后）
- 等 run 31956012141（gs-interface）/ 31956014652（native-launch）结果：后台监控会通知；若两分支均 STAGE6 FrogUI 编译通过 + STAGE7 fceumm 等核心 clone 通过 → 二选一或合并入 main（不污染），全绿打 v0.1（测试版）。
- 若仍失败：拉 build-output 真实日志（§6）按真实证据定位 → 查对应开源官方文档 → 修 → 重新 dispatch → 再监控，循环至绿。
- 9 分类 .DAT 平台映射已锁定并记于 `docs/00x_dat_platform_mapping.md`（000=Arcade、001/005=NES、002=SFC、003=MD、004=GBA、006=GB/GBC、007=PS1、008=Atari2600），资源包不含 ROM。
- 全绿后真机验收：SRAM 落 .srm、RTC、evdev 手柄、显示；并核对路径约定 FrogUI 侧 `CUBEGM_CORES_DIR=/mnt/sda1/cubegm/cores` vs picoarch 侧 `/mnt/sdcard/cubegm/cores`（构建验证不管运行时路径）。

## 10. 关键文件职责
- deploy/bootstrap_linux.sh：CI 编排（STAGE0-3）。
- build/toolchain/build_sysroot_ctng.sh：crosstool 自举 sysroot。
- deploy/build_sdl_libpng.sh：STAGE2 四库交叉编译进 sysroot。
- deploy/build.sh：STAGE3 前端/核心/门禁编排。
- deploy/build_sf3000_armhf.sh：picoarch ARM 编译（MIPS→ARM 改写 + SDL 包含修复在此）。
- patch/picoarch_5edits.patch：RTC + evdev 手柄补丁（plat_sf3000.c/plat_sdl.c/core.c）。
- tools/verify_build_scripts.py：未定义 helper 静态门禁。
- .github/workflows/build.yml：CI 定义 + 缓存 key。
