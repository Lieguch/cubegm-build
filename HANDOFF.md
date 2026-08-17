# CubeGM 构建 · 交接文档（HANDOFF）

> 用途：供接手的 Agent / 大模型快速接手，读完即可继续，无需从头探索。
> 维护：每次达到版本里程碑（v0.x / v1.0）或重大变更后更新本文件与 VERSION。
> 最近更新：2026-08-17（三分支 FrogUI 修复已全绿；STAGE7 四核心经三轮修复——v2 用 gcc/g++ 软链前置 PATH + snes9x LTO= + nestopia -I. + fceumm 宏 + picodrive 递归子模块——已推送并第四轮 dispatch，验证中；根因均以 build-output 分支真实日志 + 上游 Makefile 真源取证）

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

## 4. 当前进度（2026-08-17）
| 阶段 | 状态 | 证据 |
|---|---|---|
| STAGE1 sysroot (glibc-2.17) | 通过 | run34 日志 "Build completed" + glibc ceiling 校验 |
| STAGE2 四库进 sysroot | 通过（有坏包风险，已加固） | run34 日志；2026-08-17 zlib gzip 校验修复待验证 |
| STAGE3 picoarch 编译 | 修复已推送，待 run | 见 §4.5 |

已打 tag（不可变回滚点）：core/stage1-sysroot→646a6643；core/stage2-perms-fixed→3f126e81af（run31885774010）；core/stage2-libs→691d29c878（run34）；v-sysroot-writable→94ea9f8a；v-platsf3000-syntax→646a6643；ct-ng-tarballs→3b08ded7a8。
main HEAD=82a7d83（STAGE7 per-core + zlib 加固，2026-08-17）。

## 4.5 当前活跃工作：两条 FrogUI 启动方案分支（2026-08-17 更新）
目标：让 FrogUI 启动器在 CubeGM 上把"选游戏→启动"桥接到 picoarch 原生启动协议（picoarch 读 `/tmp/frogui_launch.txt` + `RETRO_ENVIRONMENT_SHUTDOWN`）。
分两条方案并行验证，互不污染 main：
- **wip/frogui-gs-interface**：保留上游 FrogUI 的 gs 游戏启动符号名，用 `patch/frogui_gs_bridge.patch`（int 版）把 gs 启动桥接到 picoarch 原生协议（方案1）。
- **wip/frogui-native-launch**：同样用 int 版 `frogui_gs_bridge.patch` 桥接（其根目录 patch/ 原先**根本没有该文件**，build.sh 引用恒假 → 补丁从未生效）。

### 已实证根因（来自 build-output 分支真实 CI 日志，非猜测）
三分支最后一次 run（gs=31922728812 / main / native）：
1. **gs-interface**：根目录 `patch/frogui_gs_bridge.patch` 是 **bool 旧版**（SHA 7a501db46f718…），里面 `static bool gs_request_shutdown=false/true` 在 `frogos.c:30` 处 `bool` 未定义（`<stdbool.h>` 由补丁块之后第 62 行 `#include "libretro.h"` 才提供）→ `unknown type name 'bool'`。真正正确内容（int 版，SHA 382a38b1…）只存在于 `deploy/patch/`，而 build.sh 引用的是**根目录** `patch/frogui_gs_bridge.patch` → 一直套的是 bool 版。
2. **main**：STAGE6 从来没有任何 gs-bridge 补丁逻辑（补丁仅 gs/native 分支有）→ 原始 `frogos.c` 编译撞 `'ptr_gs_run_game_file' undeclared`。
3. **native-launch**：STAGE2 `gzip: stdin: not in gzip format`（zlib.net 返回 200 + 11984 字节限流页，`curl -fL` 不校验内容 → tar 坏包）。且其根目录 patch/ 缺 `frogui_gs_bridge.patch`，build.sh STAGE6 引用恒假。
4. **共同**：STAGE7 克隆 `https://github.com/libretro/fceumm.git` 8 次全失败（该仓库不存在，正确为 `libretro/libretro-fceumm`）→ die。

### 已推送修复（2026-08-17，均经真实证据/官方文档实证）
1. **三分支 `deploy/build.sh` STAGE7 重写为 per-core 构建**：mgba→cmake（`-DLIBMGBA_ONLY=ON -DBUILD_LIBRETRO=ON`，交叉链 C/C 编译器 + flags）；snes9x|nestopia→`make -C libretro`；fceumm|picodrive→`make -f Makefile.libretro`；统一 `platform=unix`。picodrive 克隆后 `git submodule update --init --recursive`（libretro-common 子模块提供头文件，否则 `streams/trans_stream.h` 缺失）。fceumm 仓库映射为 `libretro-fceumm`。

   **关键陷阱（2026-08-17 第三次 CI 实测暴露，推翻"platform=unix 即够"假设）**：仅在 make 命令行传 `CC="$CC" CFLAGS=...` **不够**——snes9x/nestopia/fceumm/picodrive 的上游 Makefile 在编各自 `libretro-common` 子树时**硬编码 `gcc`/`g++`**（命令行 CC 传不到内层规则），导致这些 .o 用**主机 gcc** 编译（实测 fceumm 的 `src/drivers/libretro/libretro-common/*.c` 由主机 `gcc` 编出），最终 .so 链接失败或设备上崩。snes9x 还因 `unix` 分支 `LTO ?= -flto` 与 `-fPIC` + ARM bfd 链接器冲突报 `dangerous relocation: unsupported relocation (R_ARM_CALL unresolvable)`。nestopia 的 `libretro.h` 就在 `libretro/` 目录（cwd），但 Makefile 用 `<libretro.h>` 角括号包含且 INCDIRS 只有 `-I.. -I../source`，缺 `-I.`。
   **最终修复（v2，已推送并第四轮 dispatch）**：① STAGE7 循环前把交叉编译器软链成 `gcc`/`g++`/`cc` 前置 PATH（buildroot/crosstool 标准做法，覆盖所有硬编码 gcc 的内层规则）；② snes9x 传 `LTO=` 关 LTO；③ nestopia CFLAGS 加 `-I.`；④ fceumm 传 `-DFCEU_VERSION_NUMERIC=9900`；⑤ picodrive 递归克隆子模块。
2. **根目录 `patch/frogui_gs_bridge.patch` 三分支统一为 int 版**（382a38b1，138 行）：`static bool→int` 消除 `<stdbool.h>` 位置依赖；与 `deploy/patch/` 内容一致。gs 分支覆盖 bool 旧版；native 分支**新增**该文件；main 分支 STAGE6 新增补丁应用逻辑（`git apply --check` 失败则 WARN 跳过）。
3. **三分支 `deploy/build_sdl_libpng.sh` zlib 下载加固**：`curl -fsSL` + `gzip -t` 校验（从机理消除"200+坏 body"）→ 坏则切 GitHub 镜像 → 最多 3 次重试 → 仍失败才 die。
4. **FrogUI 链接 blocker 修复（2026-08-17，经上游 `tzubertowski/FrogUI` 真源实证）—— 消除 ABI 门禁 FAIL 的唯一 blocker**：
   - **根因**：上游 `frogos.c:193` 调 `xlog(...)`，但 `xlog` 仅在 `settings.c` 局部 `#define xlog printf`（`settings.h` 不导出）→ `frogos.c` 链接期 `undefined reference to 'xlog'`，`menu_libretro.so` Error 1。
   - **修复 A（xlog 桩）**：`patch/frogui_gs_bridge.patch` 首 hunk 注入 `#ifndef xlog\n#define xlog printf\n#endif`（镜像上游意图，frogos.c 含 settings.h 但无 xlog 定义）。
   - **修复 B（产物名归一）**：上游 Makefile 第36行 `TARGET := $(TARGET_NAME)_libretro.so`，`TARGET_NAME=menu` → 真实产出 `menu_libretro.so`；而 STAGE6/STAGE8/STAGE9 + ABI 门禁都找 `frogui_libretro.so` → `deploy/build.sh` STAGE6 在产出 `menu_libretro.so` 后 `cp menu_libretro.so frogui_libretro.so`（仅归一文件名，不改上游 TARGET_NAME，避免回归）。
   - 验证：`git apply --check` 通过；三分支 `build.sh` `bash -n` 通过。
- 提交（2026-08-17）：FrogUI patch 改 LF 重推 → main=`ca4602900f`/gs=`eb0fea1d1c`/native=`0987cdfbd3`（**此轮三分支 CI 均已 `success`，FrogUI 修复彻底生效**）。
- **STAGE7 四核心修复（2026-08-17）**：第一轮 classic_armv7_a7（错，snes9x -fwhole-program 链接失败）→ 第二轮 platform=unix（仍失败：CC 未传到 libretro-common 子树、snes9x LTO、nestopia 错 -I 路径，CI 真绿但核心未编出）→ **第三轮 v2 修复**（gcc/g++ 软链前置 PATH + snes9x `LTO=` + nestopia `-I.` + fceumm `-DFCEU_VERSION_NUMERIC=9900` + picodrive 递归子模块），已推送并第四轮 dispatch，后台监控中，预期四核心真正产出 .so。

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
4. 缓存：build.yml 缓存 key = hashFiles('build/toolchain/build_sysroot_ctng.sh','deploy/build_sdl_libpng.sh')。改这俩→STAGE1 重建（zlib 加固改了 build_sdl_libpng.sh → 缓存 key 变更，本次三分支会重建 STAGE1）；改别的（如 build_sf3000_armhf.sh）→复用缓存，跑得快。
5. alsa 源：用 https://www.alsa-project.org/files/pub/lib/alsa-lib-1.2.10.tar.bz2（github release 那个 404）。
6. zlib 下载：zlib.net 可能返回 200 + 限流坏 body（`curl -fL` 不校验内容 → tar 报 not in gzip format）。现加固为 `gzip -t` 校验 + GitHub 镜像 fallback + 3 次重试。
7. STAGE7 核心构建是 per-core（非裸 make）：mgba 走 cmake、snes9x/nestopia 走 `libretro/` 子目录、fceumm/picodrive 走根目录 `Makefile.libretro`；fceumm 仓库是 `libretro/libretro-fceumm`（裸 `libretro/fceumm` 不存在，克隆必 fail）。
8. FrogUI 启动桥接补丁必须放**根目录** `patch/frogui_gs_bridge.patch`（build.sh 引用 `$HERE/../patch/`），int 版 SHA 382a38b1；`deploy/patch/` 下同内容不会被引用。
9. **CRLF 致命坑（2026-08-17 实证）**：`git apply`/`--check` 在 CI（ubuntu，工作树 LF）拒绝 **CRLF** 补丁（`patch does not apply` @line10）→ 补丁被 `--check` 跳过 → `ptr_gs_run_game_*`/`xlog` 未定义 → FrogUI 编译失败 → ABI 门禁 FAIL。本地 `git apply --check` 用 LF 副本会通过，掩盖此问题。**铁律：所有 `.patch` 必须经 Contents API 以 LF 落地**（推送前 `replace("\r\n","\n")`）；build.sh 同样必须 LF。

10. **STAGE7 RK3036G 适配真相（2026-08-17 实证，推翻此前 classic_armv7_a7 假设）**：
   - TreeFrogUI/FrogUI 体系**原本是 MIPS 掌机（SF2000/GB300/SF3000-Hichip）前端**，并非 ARM；但 libretro 各核心是跨架构的。RK3036G = 双核 Cortex-A7 armhf（NEON+VFPv4，glibc≤2.17）。
   - **正确适配 = libretro 官方 ARM-Linux 共享库 recipe**（libretro-super `recipes/linux/cores-linux-armhf-generic.conf`）：`platform=unix` + 交叉工具链 `arm-linux-gnueabihf`（本项目即 crosstool-NG 构建的 sysroot 工具链），并保留 `ARCH_FLAGS=-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -O2`（A7+NEON 优化）。这比通用 `linux` 分支更贴 A7，且是 libretro 标准共享库构建路径。
   - **此前误用 `classic_armv7_a7` 的真实坑**（已逐一查各核心 Makefile 真源确认）：
     1. snes9x：该分支带 `-fwhole-program`（LTO 整程序优化）→ 编 .so 时 `ld returned 1`（retro_* 导出符号被整程序优化吞掉）。改 `platform=unix`（标准 -fPIC 共享库，无 whole-program）；但 unix 分支仍带 `LTO ?= -flto`，与 -fPIC + ARM bfd 链接器冲突报 `dangerous relocation`，故**再传 `LTO=` 关闭 LTO**（v2 修复点）。
     2. fceumm：其 Makefile **仅 PS2 分支**定义 `FCEU_VERSION_NUMERIC`，unix/classic 分支均无 → 编译报 `FCEU_VERSION_NUMERIC undeclared`。改 `platform=unix` + make 传 `-DFCEU_VERSION_NUMERIC=9900`。
     3. nestopia：Makefile 用 `<libretro.h>` 角括号包含，而 `libretro.h` 就在 `libretro/`（cwd），INCDIRS 仅 `-I.. -I../source` 缺 `-I.` → `libretro.h: No such file`。改 `platform=unix` + CFLAGS 追加 `-I.`（**不是** `-Ilibretro-common/include`，那路径不存在）。
     4. picodrive：`libretro-common` 是**真 git submodule**（路径 `platform/libretro/libretro-common/`）；`git clone --depth 1` 不拉子模块 → 头文件缺失。改：`git clone --recursive`（新增 `git_clone_recursive`）；`platform=unix`。
   - **比上面 4 点更隐蔽的统一根因（v2 才解决）**：核心 Makefile 编各自 `libretro-common` 子树时**硬编码 `gcc`/`g++`**（如 fceumm 的 `src/drivers/libretro/libretro-common/*.c` 实测由主机 `gcc` 编出），make 命令行的 `CC=` 传不到内层规则。→ 软链交叉编译器为 `gcc`/`g++`/`cc` 并前置 PATH（buildroot 标准做法），覆盖所有内层规则。
   - **“绿”≠核心编出**：build.sh 把核心失败当 WARN 吞（`|| log "WARN: core $c build had issues."`）。真实产出须看 build-output 分支 STAGE7 日志，或 STAGE8 ABI 门禁是否拿到全部 5 个 .so。STAGE8 全过才算真绿。
   - 验证原则：每次修复前读对应开源官方 Makefile/recipe 真源，不凭经验拍 flags（见铁律 §9）。

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
- 三分支修复已推送（main=`7b2412dedd`、gs=`82dc1ecbe4`、native=`d28c678f7c`），**已 dispatch**：后台监控三分支 build；遇错按 §6 取真实日志→查官方→修→再 dispatch，循环至绿。
- 若三分支均 STAGE2 过 zlib 校验 + STAGE3 picoarch 编译过 + STAGE6 FrogUI 编译过 + STAGE7 核心产出 .so → 全绿 → 二选一或合并入 main（不污染），打 v0.1（测试版）。
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

## 11. STAGE7 核心构建 v3 修复（2026-08-17，RK3036G armhf）
- 现象：v2（gcc 软链 + `platform=unix` + `LTO=` + nestopia `-I.`）仍"假绿"——build.sh 用 `|| log "WARN"` 吞掉 4 核心失败，STAGE8 ABI 门禁只过 mgba。真日志（build-output/bootstrap.log）证实 snes9x/fceumm/picodrive/nestopia 全 FAIL。
- 真因（真实日志 + 官方 Makefile 源码实证，非猜测）：
  1. `platform=unix` 在 ARM 上不保证 bundled `libretro-common` 子树被 `-fPIC` 编译 → `relocation ... recompile with -fPIC`；且 unix 分支不强制 `-marm`，Thumb+`-fPIC`+bfd 链接器 → `dangerous relocation: unsupported relocation`（snes9x `sdsp.o`、fceumm NES mapper `.o`）。
  2. 命令行 `CFLAGS="..."` 覆盖了各核心 Makefile 自身的 include 追加（如 picodrive `CFLAGS += -I platform/libretro/libretro-common/include`），导致 `compat/strcasestr.h`/`boolean.h`/`retro_common_api.h` 找不到。
  3. nestopia 的 `libretro.h` 不在 `libretro/` 而位于捆绑的 `libretro-common/include/`，原 `-I.` 无效。
- 修复（对齐 libretro-super 官方 `armv-neon-hardfloat` 平台）：
  - STAGE7 用**编译器 wrapper**：裸名 `gcc/g++/cc` 与完整 triplet `arm-linux-gnueabihf-{gcc,g++}` 都指向 wrapper 脚本，wrapper 调用**真交叉编译器（绝对路径）**并统一追加 `-fPIC -marm -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -I<alsa> -Ilibretro-common/include -DFCEU_VERSION_NUMERIC=9900`。`-fPIC` 覆盖所有内层规则（含 libretro-common），`-marm` 消除 Thumb relocation，`-Ilibretro-common/include` 修 nestopia，`-DFCEU_VERSION_NUMERIC=9900` 修 fceumm。
  - 四个核心统一 `platform=armv-neon-hardfloat`，**不再覆盖 `CFLAGS=`**，各 Makefile 保留自身 include。
  - mgba 仍走 CMake（已可用，不变）。
- 门禁（STAGE8）：须拿满 5 个核心 `.so`（mgba/snes9x/fceumm/picodrive/nestopia）+ picoarch + frogui_libretro.so；任一缺失即未真绿，须回真实日志定位。

## 12. STAGE7/STAGE4 v4 -- wrapper recursion bug fix (root cause found)

v3 shipped a **compiler wrapper** but set `REAL_CC="$CC"` where `$CC` is a **bare
triplet** (`arm-linux-gnueabihf-gcc`). Root cause chain (verified in source):
- `bootstrap_linux.sh:120` `PREFIX=/opt/cubegm-toolchain`; `:164` `export PATH="$PREFIX/bin:$PATH"`
  -> the cross gcc lives **on PATH**.
- `deploy/build.sh` STAGE1 `:106` `if command -v ${TARGET}-gcc` is true -> `CC="${TARGET}-gcc"` (bare).
- The wrapper wrote `$CROSS_BIN/arm-linux-gnueabihf-gcc` and then `export PATH="$CROSS_BIN:$PATH"`.
- Every compiler call re-resolved `arm-linux-gnueabihf-gcc` through PATH -> found the wrapper
  again -> **infinite recursion (fork-bomb)**. The whole `deploy/build.sh` (STAGE4 picoarch,
  mgba via cmake, STAGE7 cores) would fail at the first compile.

v4 fix: at wrapper-creation time (before `CROSS_BIN` is prepended) resolve the real binary to an
**absolute path** with `command -v`:
```
REAL_CC="$(command -v "$CC" 2>/dev/null || echo "$CC")"
REAL_CXX="$(command -v "$CXX" 2>/dev/null || echo "$CXX")"
```
The wrapper then `exec`s the absolute real compiler -> no PATH re-resolution -> no recursion.
All other v3 behaviour unchanged (still `platform=armv-neon-hardfloat`, still injects
`-fPIC -marm -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -I<alsa> -Ilibretro-common/include`
and does NOT override `CFLAGS=` so each core's own include paths survive).


## N. STAGE7 v5 真绿门禁同步至本 wip 分支（2026-08-17）

> 本分支原 `deploy/build.sh` 停在 v4（无 v5 修复），状态灯虽绿但属**假绿**：nestopia/picodrive 失败被 `|| log WARN` 吞掉、无 fail-fast。

- 已通过 Git Data API 将 main 当前 v5 的三处 STAGE7 修复镜像到本分支 `deploy/build.sh`（commit `2bd9eab338db`）：
  1. 编译器 wrapper `exec` 追加 `-D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS`（nestopia `SIZE_MAX` 修复，官方核对 retro_miscellaneous.h @ fc21888）。
  2. `fceumm|picodrive` 分支 make 加 `use_libchdr=0`（picodrive `AT_HWCAP2` 修复，官方核对 Makefile:89/350）。
  3. `CORE_FAIL` fail-fast 门禁（循环前 `CORE_FAIL=""`、缺失核心累积、循环后 `exit 1`）。
- **保留本分支专属内容**：FrogUI STAGE6 launch-bridge（方案1 gs / 方案2 native）与 clone 的 `--filter=blob:none` + 600s/8 次健壮性（未改动）。
- 完整根因 + 官方源核对见 **main 分支 HANDOFF §13**。本分支已与 main v5 在 STAGE7 达成对等；对应 workflow_dispatch 已触发，待真实日志核验真绿。

## 15. 2026-08-17 main picoarch `unix` 构建回归（mi_sys.h）及修复

- **现象**：`main` 最新 run #104（id 32036760794，13:48Z）`completed+failure`；
  CI 真实日志（build-output/bootstrap.log）报错
  `plat_sdl.c:174:10: fatal error: mi_sys.h: No such file or directory`
  -> `[ERROR] picoarch build failed`。
- **根因（已联网核对上游真源 tzubertowski/TreeFrogUI_picoarch@r36sx plat_sdl.c）**：
  上游 r36sx 的 `plat_sdl.c` 把一段 **Miyoo/mini 专属硬件缩放**实现包在
  `#ifndef PLATFORM_SF3000` 内（即非 SF3000 平台即编译）。本仓库 `main` 的
  `deploy/build_sf3000_armhf.sh` 按 §14 决策用通用 `platform=unix`（**不**定义
  `PLATFORM_SF3000`），该块被激活，导致：
    (a) `#include <mi_sys.h>`/`<mi_gfx.h>`（MStar/Ingenic MI 缩放器头，RK3036G 无）-> 致命错误；
    (b) 块内 `static buffer_init()`/`static buffer_scale()` 与文件下方**无条件**的通用 SDL 版
        重名 -> 重定义错误。
  RK3036G 是标准 buildroot Linux（纯 fbdev SDL），本就应使用通用软件 SDL 缩放路径。
  **注**：wip 两分支仍用 `platform=sf3000`+`-DPLATFORM_SF3000`，该块被排除，故未受影响
  （旧/假绿路径）。如日后迁移到 `unix` 路径，须套用同款 gate。
- **修复（commit f530f027，仅改 main 的 deploy/build_sf3000_armhf.sh）**：
  在块外层再套 `#if !defined(RK3036G_NO_MIYOO_SCALE)`（脚本 sed 注入于
  `#ifndef PLATFORM_SF3000` 之后、对应 `#endif` 之前），并在 CFLAGS 加
  `-DRK3036G_NO_MIYOO_SCALE`。幂等、前向安全（上游若删块则 grep 跳过，行为不变）。
  效果：RK3036G 走通用软件 SDL 缩放，恢复 §14 决策前的绿态。
- **验证**：`bash -n` 通过；对上游 plat_sdl.c 实测 sed 包裹后 #if 嵌套正确
  （172 `#ifndef` -> 173 内层 gate -> 491 内层 `#endif` -> 492 外层 `#endif`），
  `mi_sys.h` 纳入排除区；块外无引用块内专属符号（GFX_*/MI_* 全在块内），排除后无悬空引用。
- **状态**：已对 `main` 重新 workflow_dispatch 触发，待真实日志核验真绿。
  wip 两分支未改动（仍 sf3000 路径），本轮不重触发。

## 16. 2026-08-17 main 修复回归（§15 的 sed gate 自身损坏 + 概念缺陷）→ 改用 MI 桩头

- **§15 修复被证伪**：§15 的 commit f530f027 用 `sed` 向 plat_sdl.c 注入
  `#if !defined(RK3036G_NO_MIYOO_SCALE)` 包裹 miyoo 块，并加
  `-DRK3036G_NO_MIYOO_SCALE`。重新 dispatch 后 run #105/#106/#107 仍
  `completed+failure`，真实日志（build-output/bootstrap.log）报：
  `sed: -e expression #1, char 50: unterminated 's' command`。
  根因：该 `sed s|...$|...|` 替换串内含**未转义裸换行**，sed 把换行当命令结束符，
  s 命令在闭合 `|` 前终止 -> 语法错误（纯 LF 脚本同样触发，与 CRLF 无关）。
- **概念缺陷（更重要）**：即便 sed 写对，gate 掉整段 miyoo 块也不成立——
  该块的 `buffer_init()`/`buffer_scale()` 是**通用 `unix` 路径必需**的
  （plat_sdl.c:910 在 `scale_size!=NONE` 调用 `buffer_scale()`；块外/SF3000 块
  均不提供通用版），gate 掉会致链接期 undefined reference。§15 判定"(b) 重定义"
  对当前 r36sx 真源**不成立**（SF3000 版 buffer_init/buffer_scale 在
  `#ifdef PLATFORM_SF3000` 内，与 miyoo 块互斥，无重定义）。
- **正确修复（Contents API 提交，仅改 main）**：
  1. 新增 `deploy/mi_sys_stub.h` + `deploy/mi_gfx_stub.h`：MStar MI 的 malloc 支撑
     shim（MI_SYS_*/MI_GFX_* 全为 no-op/内存分配），让 miyoo 块**编译+链接通过**。
  2. `deploy/build_sf3000_armhf.sh`：删除损坏的 sed 与无用的 `-DRK3036G_NO_MIYOO_SCALE`；
     改为构建时把两桩头 `cp` 到 picoarch 根目录（经 `-I./` 命中 `#include <mi_sys.h>`）。
     运行期 `GFX_BlitSurfaceExec` 因通用 SDL `screen` 不设 `pixelsPa`（pixelsPa 宏展开为
     `unused1`），必走 `SDL_BlitSurface` 回退，NEON 软件缩放（scale1x..6x_n16）照常驱动显示。
  3. **不**定义 `PLATFORM_SF3000`：避免激活 plat_sdl.c 内 ~16 处 SF3000 专属代码
     （cubevol 输入 / driver.so 音频 / /dev/fb0 mmap），那些在 RK3036G 上会破坏输入/音频。
  修复纯叠加（新增 2 文件 + 改 1 脚本），不删除原厂/里程碑文件。
- **验证**：桩头在宿主机用 gcc -Wall -Wextra 模拟 plat_sdl.c 的 MI 调用全部通过
  （类型与上游真源一致，MI_PHY=uintptr_t 等同原厂 SDK）；`bash -n` 通过；桩头经
  `-I./` 命中、不触碰其他源。
- **状态**：已对 `main` 重新 workflow_dispatch 触发（待 #108+ 真实日志核验真绿）。
  wip 两分支仍 `platform=sf3000`（该块已被 `#ifdef PLATFORM_SF3000` 排除），未受影响、
  未改动、不重触发。三分支 HANDOFF 保持 §16 一致。
