# 产品需求文档（PRD）— CubeGM 开源替代系统

> 角色：PM 许清楚 ｜ 项目：cubegm-replacement ｜ 版本 v0.1
> 范围结论（来自用户确认）：**个人非商用自用**；目标 **功能对等并超越原厂**；**无现成工具链，由交付团队协助获取/搭建**。

---

## 一、背景与问题

原厂 CubeGM 固件（R36SX / DataFrog SF3000 / SF3500 / GB350 类掌机）闭源且**已停止更新**。本机为 **RK3036G**（双核 Cortex-A7 / Mali-400）。经逆向与实测已明确：

- 系统文件：`rkgame`(菜单引擎)、`icube`(启动器)、`driver.so`(硬件驱动)、`cores/libemu_*.so`(20 个定制 ABI libretro 核心)。
- 输入：`joystick.zip`（26 动作 + P1/P2 扫描码矩阵 + 4 USB 手柄 profile）。
- UI：`ui_*.raw` = **RGB565 1280×720**；`root.dat` 为 720×480（仅启动 logo 区）。
- **驱动层为标准 Linux DRM/ALSA/evdev**，picoarch `plat_linux.c` 可直接复用，无需重写。
- 开源生态（GitHub `tzubertowski`）：TreeFrogUI_picoarch / FrogUI / treefrog-ui，可整体复用（NC 合规）。

**核心诉求**：用开源代码 + RK3036G 硬件信息，开发一整套与原厂等价、且可迭代升级的系统。

---

## 二、目标与非目标

| 类别 | 内容 |
|---|---|
| 目标 | 1) 功能对等原厂（20+ 核心、输入、UI、存档）；2) 可维护/可扩展地**超越原厂**；3) 个人非商用自用；4) 复用开源前端，自研最小适配层 |
| 非目标 | 1) 任何商业分发/售卖；2) 重写 driver.so 硬件驱动；3) 替换内核/U-Boot/启动校验分区（避免「sdcard is damaged」）；4) 支持非 RK3036G 硬件 |

---

## 三、用户与场景

| 角色 | 场景 | 关键诉求 |
|---|---|---|
| 设备持有者（本人） | SD 卡插入掌机即进入自研系统 | 不破坏原厂启动校验；即插即用 |
| 维护者（本人） | 迭代升级核心/UI/功能 | 构建可复现、ABI 受控、有回归验证 |
| 社区（潜在） | 同硬件玩家交流补丁 | 仅限非商用；保留 NC 署名 |

---

## 四、功能需求（优先级 P0→P2）

### P0（Stage1 MVP 必须）
- FR-1 启动劫持：原厂 `autorun` → `cubegm/zhijack.sh` → picoarch+FrogUI，**不触碰 root.dat 校验**。
- FR-2 显示闭环：DRM/KMS dumb buffer 渲染 1280×720，出画。
- FR-3 输入闭环：evdev 读取，覆盖 26 动作 + P1/P2 矩阵。
- FR-4 音频闭环：ALSA 出声。
- FR-5 至少 3–5 个 libemu_* 核心可加载运行（fork 模型）。

### P1（Stage2 功能对等）
- FR-6 全 20 核心注册（补 `gpsp`/`prosystem` 两条 `<core>`，见架构 §5.5）。
- FR-7 完整 UI 皮肤（RGB565 1280×720 资源管线 + PIL 校验工具）。
- FR-8 4 个 USB 手柄 profile 全支持（VID_PID_REV 可扩展）。
- FR-9 存档/读档（复用核心 save_state/load_state 定制 ABI）。

### P2（Stage3 超越原厂）
- FR-10 57 核心（treefrog-ui `clone_cores.sh`）。
- FR-11 Quick Resume（快照恢复）。
- FR-12 主题/缩略图/多语言。
- FR-13 可插拔核心 ABI shim（新 core 零改动接入）。

---

## 五、构建与工具链需求（硬前提）

| 需求 | 规格 |
|---|---|
| 编译器 | `arm-linux-gnueabihf-`（armhf，ARMv7-A） |
| CFLAGS | `-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -O2` |
| ABI 门禁 | 产出二进制必须 EM_ARM / e_flags=0x5000400 / 最大 GLIBC_2.17 |
| sysroot | 设备 rootfs（glibc 2.17），规避现代工具链 glibc 错配 |
| 构建机 | 用户 Linux 构建机（原厂证据为 buildroot VM） |

---

## 六、成功标准（退出准则）

| 阶段 | 退出标准 |
|---|---|
| Stage0 SDK | `verify_target_abi.sh` 对样例二进制断言通过 |
| Stage1 MVP | 设备进入 FrogUI，选游戏可出画+出声+被输入响应 |
| Stage2 对等 | 原厂可跑项本系统均可跑；gpsp/prosystem 启用 |
| Stage3 超越 | 57 核心 + Quick Resume + 主题/缩略图/多语言 |

---

## 七、合规与风险

- **NC 红线**：仅个人非商用；任何对外分发须保留 CC BY-NC-SA 4.0 署名与非商用条款，禁止闭源再发布。
- **启动校验**：仅劫持 `autorun`，绝不改写 `root.dat`/校验分区。
- **ABI 回归**：每次构建跑 `verify_target_abi.sh`，杜绝 glibc 错配上线。

---
*下接：架构设计（高见远）、构建脚手架（寇豆码）、验证方案（严过关）。*
