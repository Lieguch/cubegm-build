# 任意 x86_64 Linux 构建指南（CubeGM / RK3036G 开源替代系统）

> **目标**：在**任意**一台 x86_64 Linux 上，一条命令编译出可烧录到 SD 卡的设备固件。
>
> `bootstrap_linux.sh` **不依赖 WSL2**——它只在 `uname -s = Linux` 时运行，
> WSL2 / 云 VM / VirtualBox / GitHub Actions runner / 物理机**全都行**。脚本里没有任何
> `/mnt/c` 之类的 WSL2 专属路径。WSL2 只是"在你 Windows 本机上最容易搞到 Linux"的一种，
> 不是唯一方式。
>
> **为什么必须 Linux**：glibc-2.17 的 sysroot 只能在本机 Linux 上自举
> （crosstool-NG 不支持 Windows/MinGW 宿主）；且 ARM 官方交叉工具链是 Linux-hosted ELF，
> Windows 下根本跑不起来。本项目的 Agent 沙箱是纯 Windows，无法替你执行编译。

---

## 0. 选一种 Linux 来源（四选一）

### 方案 A — 本机 WSL2 Ubuntu（Windows 本机，最省事，但需系统支持 WSL2）

若你的 Windows 支持 WSL2（Win10 2004+ / Win11，BIOS 开了虚拟化，未被组策略禁用）：

```powershell
# 管理员 PowerShell 一次
wsl --install
```

重启后设用户名/密码。验证：`uname -a` 应见 `Linux ... x86_64`。
> 若 `wsl --install` 报"可选组件"错误，说明你的环境不支持 WSL2——直接看下面方案 B / C / D。

### 方案 B — 云 Linux VPS（**推荐**，契合"外部托管、本机零负担"偏好）

任意云厂商开一台 x86_64 Ubuntu/Debian 22.04+：**最低 2 vCPU / 4GB**（建议 4 vCPU / 8GB 加速
crosstool 自举），磁盘 ≥ 15GB。把 zip 传上去、ssh 进去跑，跑完取产物、销毁 VM，本机零常驻。

```bash
# —— 在你本地（Windows PowerShell 或任意终端）——
scp cubegm-handoff.zip user@<云主机IP>:~/
ssh user@<云主机IP>

# —— 进去后（云主机内）——
mkdir -p ~/cubegm && cd ~/cubegm
unzip ~/cubegm-handoff.zip
cd cubegm-handoff/deploy
sudo ./bootstrap_linux.sh

# —— 跑完取产物（回到本地终端）——
exit
scp -r user@<云主机IP>:~/cubegm/cubegm-handoff/deploy/cubegm ./device-payload
```

### 方案 C — 本机 VirtualBox / VMware 装 Linux VM（免费，需本机有虚拟化）

下载 VirtualBox + Ubuntu 22.04 ISO，新建 VM（2 vCPU / 4GB / 20GB 磁盘），安装后在 VM 内：

```bash
# 把 zip 从宿主机拖入 VM（或设共享文件夹 / ssh 传），解压后：
cd deploy
sudo ./bootstrap_linux.sh
```

> 注意：VirtualBox 走自己的虚拟化（VT-x/AMD-V），即使你的"不支持 WSL2"是 Hyper-V 被关或
> 组策略导致，VirtualBox 通常仍可运行——只要 BIOS 开了 CPU 虚拟化。

### 方案 D — GitHub Actions 免费 Linux runner（零本机安装、完全外部）

若你不想碰任何 Linux 环境，我可以另提供 `.github/workflows/build.yml`：推到一个 GitHub
仓库后，GitHub 免费的 x86_64 Linux runner 会自动跑 crosstool + 构建，产物作为 **artifact**
在浏览器里下载。本机只需浏览器。

> 注意点：crosstool 自举 30–90 分钟（单 job 6 小时上限够用）；sysroot + 产物体积可能需
> 分卷或走缓存处理。**若选此方案，告诉我，我把 workflow 文件加进交接包。**

---

## 1. 把交接包弄进 Linux 环境（通用）

```bash
# 进工程目录（路径按你的来源定：WSL2 家目录 / 云 VM / VM 内皆可）
mkdir -p ~/cubegm && cd ~/cubegm
# 来源：scp 上传 / 宿主机拖入 / 共享文件夹 / unzip 本地拷贝，随你
unzip cubegm-handoff.zip
cd cubegm-handoff/deploy
```

（WSL2 用户可用 `cp /mnt/c/Users/<用户名>/cubegm-handoff.zip .` 从 Windows 盘拷入；
云 VM / VM 用户用上面的 `scp` 或拖拽。）

---

## 2. 一条命令构建

```bash
sudo ./bootstrap_linux.sh
```

脚本自动完成：装依赖 → 自举 glibc-2.17 工具链（约 30–90 分钟，**断网/失败重跑会自动跳过
已完成步**）→ 交叉编 SDL/libpng/alsa 进 sysroot → 克隆 `r36sx` + 子模块 + 打 5 处补丁
→ 编 picoarch/FrogUI/core → ABI 门禁 → 产出到 `deploy/cubegm/`。

常用变量：

```bash
PREFIX=~/cubegm-tc ./bootstrap_linux.sh        # 改工具链安装位置（默认 /opt/cubegm-toolchain）
CORES="mgba fceumm" ./bootstrap_linux.sh        # 只编这两个核心，省时间
SYSROOT=/已有/sysroot ./bootstrap_linux.sh      # 跳过工具链，直接进前端构建
```

> **构建报告**：无论成功还是中途失败，脚本都会在 `deploy/` 下生成 `BUILD_REPORT.txt`
> （含 sysroot 的 glibc 天花板、设备产物 `picoarch` 的 GLIBC 需求、各 STAGE 状态、退出码）。
> 把它**完整发回给 Agent**，即可远程诊断，你无需手动跑任何检查命令。

---

## 3. 烧录到设备

```bash
# 构建产物在 deploy/cubegm/，整体拷到 SD 卡根目录，覆盖原 cubegm/
# 原厂 rkgame/icube/driver.so/root.dat 不动 -> 设备不会报 "sdcard is damaged"
```

---

## 4. SDL/libpng 交叉编失败时（最常见卡点）

`build_sdl_libpng.sh` 是**风险最高、本沙箱未实跑验证**的环节。若它报错：

**A. 先试微调 SDL 的 configure 开关**（在 `build_sdl_libpng.sh` 第 3 段）：
- 设备若没有 ALSA 音频：把 `--enable-alsa` 改成 `--disable-alsa`（设备 driver.so 仍走 ALSA）。
- 画面不出：把 `--enable-video-fbcon` 改成 `--enable-video-dummy` 先打通链接，再按真机 DRM 调。

**B. 预编译兜底（推荐，最稳）**：放弃源码交叉编，直接把 **glibc ≤ 2.17 时代的 armhf .deb**
解包进 sysroot。**版本铁律**：只能用 **Debian 7 wheezy (glibc 2.13)** 或 **Ubuntu 12.04 precise
(glibc 2.15)** 的 armhf 包；**Debian 8 jessie / Ubuntu 14.04 trusty 都是 glibc 2.19 > 2.17，禁止使用**，
更不能用新发行版——否则引入 >2.17 依赖，设备运行期崩。

```bash
# 从老发行版归档直接下载 armhf .deb（wheezy/precise，glibc ≤ 2.17）：
#   Debian 7 wheezy:  http://archive.debian.org/debian/pool/main/libs/libsdl1.2/
#   Ubuntu 12.04:     http://old-releases.ubuntu.com/ubuntu/pool/universe/libs/libsdl1.2/
# 需要的包：libsdl1.2debian + libsdl1.2-dev + libpng12-0 + libpng12-dev (+ libasound2 + libasound2-dev)
mkdir -p /tmp/sdldeb && cd /tmp/sdldeb
# curl 下载上述 .deb 后：
for d in *.deb; do dpkg-deb -x "$d" .; done
cp -a usr/* "$SYSROOT/usr/"
# 注意：只有当构建主机本身就是 precise/wheezy 时，才可用 apt-get download libsdl1.2debian:armhf；
#       22.04/24.04 主机的 apt 会拉 glibc 2.35 版本，绝不能用。
```

然后 `sdl-config` 应出现在 `$SYSROOT/usr/bin/`，重跑 `bootstrap_linux.sh`（SDL 段会跳过）。

---

## 5. 真机验收（QA 门禁）

- [ ] 设备经 `zhijack.sh` 进 FrogUI，不报 sdcard is damaged
- [ ] 插任意标准 USB 手柄即被 evdev 识别（全手柄支持）
- [ ] RTC 时间正确（策略游戏真实时钟，补丁 core.c `GET_SYSTEM_TIME`）
- [ ] 策略游戏退出后 `.srm` 电池存档不丢（不再只有断点快照）
- [ ] `build/toolchain/verify_target_abi.sh` 对每个产物 = PASS（EM_ARM / 0x5000400 / ≤ GLIBC_2.17）

详细验收矩阵见 `docs/qa_verification.md`；所有坑见 `HANDOFF.md` §5。
