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


## 13. STAGE7 v5 -- 真绿门禁 + RK3036G 核心全编出（含 CORE_FAIL 修复）

> 状态：v4 是**假绿**（5 核只 3 核真编出，picodrive/nestopia 被 `|| log WARN` 吞掉不 fail-fast）。
> v5 目标 = 真绿：5 核全编出 `.so` + STAGE8 ABI 门禁 PASS。所有根因均经**真实 CI 日志 + 官方源**核对，非猜测。

### 13.1 v4 假绿复盘（run 88 真实日志）
- 仅 3 核产出 `.so`：mgba / snes9x / fceumm。
- `WARN: core picodrive build had issues.` / `WARN: core nestopia build had issues.` 被 `||` 吞掉，job 仍 green。
- STAGE8 门禁只检查到 5 个对象（picoarch/frogui/fceumm/mgba/snes9x），picodrive/nestopia 无 `.so` → 无对象 → 不报错。

### 13.2 nestopia 真因（官方核对 retro_miscellaneous.h @ commit fc21888）
- glibc-2.17 的 `<stdint.h>` 把 `SIZE_MAX` 置于 `__STDC_LIMIT_MACROS` 保护后；nestopia 用 `-std=gnu99` 未定义该宏 → `retro_miscellaneous.h:523 #error PRI_SIZET: unknown SIZE_MAX`。
- 修复：编译器 wrapper `exec` 行追加 `-D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS`（wrapper 注入，所有核心共享）。

### 13.3 picodrive 真因（官方核对 Makefile:89 / :350）
- 默认 `use_libchdr ?= 1` 启用 `USE_LIBCHDR`，拉入捆绑 `lzma-24.05`，其 `CpuArch.c` 用 `AT_HWCAP2`/`HWCAP2_*`（glibc-2.17 / 旧内核头 sysroot 未定义）→ `error: 'AT_HWCAP2' undeclared`。
- 修复：STAGE7 `fceumm|picodrive` 分支 make 加 `use_libchdr=0`。代价：picodrive 不支持 CHD 压缩光盘；RK3036G 无此需求，可接受（等价官方 armhf 构建）。

### 13.4 fail-fast 门禁
- 循环内：`[ -n "$so" ]` 为假 → `CORE_FAIL="${CORE_FAIL} $c"`。
- 循环后：`[ -n "$CORE_FAIL" ]` → `log STAGE7 FAILED ...; exit 1`。未来"绿"=真绿。

### 13.5 CORE_FAIL unbound 崩溃（run 32019897845 真实日志，commit 49198ec）
- `set -euo pipefail` 下 `CORE_FAIL` 在引用前未初始化 → `deploy/build.sh: line 370: CORE_FAIL: unbound variable`，job 失败。
- **关键证据**：该次运行 5 个核心（mgba/snes9x/fceumm/picodrive/nestopia）**全部产出 `.so`**（nestopia 在崩溃行前已 `[build] -> .../cores/nestopia_libretro.so`），即 v5 修复已生效，仅门禁末段 unbound 变量致 job 失败。
- 修复：循环前 `CORE_FAIL=""`（commit 524c380749）。`bash -n` 通过。
- 预期：修复后 v5 运行 = 5 核全编出 + STAGE8 ABI 门禁（verify_target_abi.sh 对 7 个对象：picoarch/frogui/5 核）PASS = 真绿。

### 13.6 分支现状（2026-08-17）
- `main` = v5（含 §13.5 修复），本次修复后预期真绿。
- `wip/frogui-gs-interface`、`wip/frogui-native-launch` = 旧 `gs`/`native` 改名，**仍 v4（假绿风险）**，待同步 v5 修复。
- 历史 `gs`/`native` 分支已删除。


## 14. 从"CI 绿"转向"构建对等/超越原厂的系统" — STAGE7 核心扩展（feat/core-expansion）

> 方向纠正（用户 2026-08-17）：此前误把"CI 转绿"当终点；真实目标是**模仿 TreeFrogUI/picoarch/FrogUI 开源代码，构建功能对等甚至超越原厂 CubeGM 的系统**。CI 绿只是 Stage0 地基。

### 14.1 系统缺口（已用真实文件核对）
- `deploy/cubegm/cores/config.xml` 注册 **24 个核心**（覆盖原厂 20 系统 + gpsp/prosystem + 超越项 PCE/FBNeo/PrBoom/ScummVM）。
- 旧 `build.sh` STAGE7 仅编出 **5 个**（mgba/snes9x/fceumm/picodrive/nestopia）。**系统实际只有 5 核心，远未功能对等**。
- 启动链路 `zhijack.sh`、菜单桥接 `frogui_gs_bridge.patch`、picoarch evdev/RTC 补丁、STAGE9 打包均已完成（Stage1 MVP 骨架齐备）。

### 14.2 官方依据（已取回 `tzubertowski/treefrog-ui`）
- `clone_cores.sh`：57 核心**官方仓库清单**（含 tzubertowski 分支）。
- `build_all.sh`：每核心**官方构建配方 + 补丁列表**（设备级修正：FBA 族 `-fsigned-char -fno-strict-aliasing`、mame2003/fbneo `-shared -lstdc++ -lpthread`、gpsp 两阶段重链、pcsx_rearmed/scummvm submodule 等）。
- 官方为 **SF3000(MIPS)** 构建；本机 **RK3036G(armhf)**，故只移植其**仓库名+配方+补丁**到我们的工具链（`platform=armv-neon-hardfloat` + sysroot wrapper）。

### 14.3 实现（feat/core-expansion 分支，保持 main 绿）
- STAGE7 改为**数据驱动**：`CORE_SPECS`（name|url|subdir|makefile|MAKEFLAGS|MARKERS）映射 config.xml 每个核心 → 官方仓库 + 配方。
- 新增 `fba-gcc/fba-g++` wrapper（signed-char/no-strict-aliasing，仅 FBA 族用）。
- 每核心：克隆（官方 URL）→ 按需 submodule → `make platform=armv-neon-hardfloat`（FBA 用 fba wrapper；mgba cmake；gpsp 两阶段；picodrive `use_libchdr=0`）→ 拷贝 `name_libretro.so`。
- 街机别名：fbalpha/pgm/fba 由 FBNeo 一次构建拷贝（避免重复）。
- 保留 `CORE_FAIL` fail-fast + STAGE8 ABI 门禁（EM_ARM/0x5000400/glibc≤2.17）。
- 每个核心失败按"真实 CI 日志 → 官方源 → 修 → 重提"迭代（铁律）。
- 推到 `feat/core-expansion`，CI 验证全绿后再合并 main（main=已知可工作状态）。
